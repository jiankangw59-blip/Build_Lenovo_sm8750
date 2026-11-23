"""Additive idle profile host checks; no kernel build, CI, ADB or governor writes."""
import argparse
import contextlib
import copy
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
from unittest.mock import patch
import yaml

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import apply_profile
import idle_refinement as idle
import integrate_crystal as crystal
import prepare_boot


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def cp(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args):
    args.output.mkdir(parents=True, exist_ok=False)
    checks = {}

    def ok(name, value):
        assert value, name
        checks[name] = True

    def reject(name, function):
        try:
            function()
        except (RuntimeError, ValueError, OSError, SystemExit):
            checks[name] = True
        else:
            raise AssertionError("Negative accepted: " + name)

    policy = idle.refinement_policy()
    baseline = read(HERE / "patch-series.json")
    stage2 = read(HERE / "stage2-series.json")
    immutable = [HERE / "patch-series.json", *(HERE / x["file"] for x in baseline["patches"]),
                 HERE / "patches/kcompressd-6.6.89.patch", HERE / "patches/le9uo-6.6.89.patch",
                 HERE / "memory_layout.py", HERE / "kmi_baseline.py", HERE / "test_memory_layout.py",
                 HERE / "kmi/plq110-C16-baseline.json.gz", HERE / "kmi/plq110-memory-layouts.json",
                 *(HERE / "crystal" / name for name in ("sddc-integrity.patch", "sddc-integrity-stats.patch", "sddc-kunit-config.patch")),
                 *(HERE / "runtime" / name for name in ("service.sh", "uninstall.sh"))]
    before_hashes = {str(p): sha(p) for p in immutable}
    before = args.output / "patch"
    for name in (*policy["files"], *policy["required_unchanged_files"]):
        cp(args.before_source / name, before / name)
    idle.source_report(before, "before")
    command = [str(args.patch), "--batch", "--forward", "--fuzz=0", "-p1", "-i", str(HERE / policy["patch_file"])]
    for extra in (["--dry-run"], []):
        completed = subprocess.run([*command, *extra], cwd=before, capture_output=True)
        (args.output / ("patch-dry-run.log" if extra else "patch-apply.log")).write_bytes(completed.stdout + completed.stderr)
        ok("real zero-fuzz " + ("dry-run" if extra else "application"), completed.returncode == 0)
    source_report = idle.source_report(before)
    ok("all source after digests and unchanged gov.h", source_report["passed"])
    expected_before = {name: values["before_sha256"] for name, values in policy["files"].items()}
    expected_after = {name: values["after_sha256"] for name, values in policy["files"].items()}
    reject("old CI after cannot pass refined source check", lambda: idle.source_report(args.before_source))

    old_fixture = args.audit / "lz4kds-lab/combined-stage2-review/profile-checks-v2" / crystal.COMBINED_PROFILE
    kernel = args.output / "k"
    for name in ("drivers/Kconfig", "drivers/Makefile", "include/trace/hooks/vmscan.h"):
        source = args.audit / "6.6.89-kcompressd-review" / name
        if not source.is_file():
            source = args.audit / "crystal-base-source" / name
        cp(source, kernel / name)
    cp(old_fixture / "mm/vmscan.c", kernel / "mm/vmscan.c")
    integrated = crystal.integrate(kernel, kernel / "integration.json", crystal.IDLE_PROFILE)
    historical = read(old_fixture / "integration.json")
    ok("all 30 immutable Crystal import digests equal combined", integrated["source_sha256"] == historical["source_sha256"] and len(integrated["source_sha256"]) == 30)
    ok("all installed Crystal and linkage bytes equal combined", integrated["installed_sha256"] == historical["installed_sha256"])
    ok("same integrity and read-only stats stages", integrated["sddc_integrity_patch"] == historical["sddc_integrity_patch"] and integrated["sddc_stats_patch"] == historical["sddc_stats_patch"])
    ok("same default lock native and KUnit configuration", crystal.crystal_configuration(crystal.IDLE_PROFILE) == crystal.crystal_configuration(crystal.COMBINED_PROFILE))
    for name in ("mm/page_io.c", "Makefile", "kernel/module/main.c", "lib/lz4kd/Makefile", "fs/erofs/zdata.c", "net/ipv4/tcp_bbr.c"):
        cp(old_fixture / name, kernel / name)
    for name in (*policy["files"], *policy["required_unchanged_files"]):
        cp(before / name, kernel / name)
    old_evidence = args.audit / "run-45268976269/preflash-verification_6.6.89"
    actual_old_integration = read(old_evidence / "efficiency/crystal-integration.json")
    ok("all 30 installed source hashes equal actual db79 artifact", all(
        integrated["installed_sha256"][name] == actual_old_integration["installed_sha256"][name]
        for name in integrated["source_sha256"]))
    config = (old_evidence / "kernel.config").read_text(encoding="utf-8")
    write(kernel / "out/.config", config)
    evidence = kernel / "evidence"
    write(evidence / "kernel.config", config)
    write(evidence / "efficiency/crystal-integration.json", json.dumps(integrated))

    def preflight(label):
        with contextlib.redirect_stdout(io.StringIO()):
            apply_profile.verify(kernel, evidence / "efficiency/config-verification.json", crystal.IDLE_PROFILE)
        cp(evidence / "efficiency/config-verification.json", args.output / (label + ".json"))

    preflight("preflight-positive")
    for name in (*policy["files"], *policy["required_unchanged_files"]):
        correct = (kernel / name).read_bytes()
        (kernel / name).write_bytes(correct + b"\n")
        reject("preflight rejects altered " + name, lambda: preflight("negative"))
        (kernel / name).write_bytes(correct)
    preflight("preflight-restored")
    patches = read(old_evidence / "efficiency/efficiency-patches.json")
    patches["profile"] = crystal.IDLE_PROFILE
    patches["applied"].append({**stage2["cpuidle-refinement"], "before": expected_before, "after": expected_after})
    patches["cpuidle_refinement"] = policy

    def accept(records=patches):
        return prepare_boot.validate_profile_evidence(evidence, records, crystal.IDLE_PROFILE)

    ok("exact 22 ordered patches accepted", len(patches["applied"]) == 22 and accept() == crystal.IDLE_PROFILE)
    for name, mutate in (
        ("missing22", lambda d: d["applied"].pop()),
        ("duplicate22", lambda d: d["applied"].append(copy.deepcopy(d["applied"][-1]))),
        ("wrong final order", lambda d: d["applied"].__setitem__(slice(-2, None), list(reversed(d["applied"][-2:])))),
        ("wrong patch digest", lambda d: d["applied"][-1].__setitem__("sha256", "0" * 64)),
        ("missing refinement policy", lambda d: d.pop("cpuidle_refinement")),
        ("wrong refinement policy", lambda d: d["cpuidle_refinement"].__setitem__("patch_sha256", "0" * 64)),
        ("wrong before source", lambda d: d["applied"][-1]["before"].__setitem__(next(iter(expected_before)), "0" * 64)),
        ("wrong after source", lambda d: d["applied"][-1]["after"].__setitem__(next(iter(expected_after)), "0" * 64)),
    ):
        changed = copy.deepcopy(patches)
        mutate(changed)
        reject("prepare rejects " + name, lambda: accept(changed))
    config_report = read(evidence / "efficiency/config-verification.json")
    for name, mutate in (
        ("missing preflight source", lambda d: d.pop("cpuidle_refinement")),
        ("wrong preflight source", lambda d: d["cpuidle_refinement"]["source_sha256"].__setitem__(next(iter(expected_after)), "0" * 64)),
        ("wrong prerequisite gov.h", lambda d: d["cpuidle_refinement"]["source_sha256"].__setitem__(next(iter(policy["required_unchanged_files"])), "0" * 64)),
    ):
        changed = copy.deepcopy(config_report)
        mutate(changed)
        write(evidence / "efficiency/config-verification.json", json.dumps(changed))
        reject("prepare rejects " + name, accept)
    write(evidence / "efficiency/config-verification.json", json.dumps(config_report))
    write(evidence / "efficiency/efficiency-patches.json", json.dumps(patches))

    # Exercise the actual apply entry's early order guard without applying or
    # pretending to apply the unchanged original 21 patches a second time.
    bad_package = args.output / "bad-package"
    cp(HERE / "patch-series.json", bad_package / "patch-series.json")
    changed_stage2 = copy.deepcopy(stage2)
    changed_stage2["cpuidle-refinement"]["file"] = "patches/le9uo-6.6.89.patch"
    write(bad_package / "stage2-series.json", json.dumps(changed_stage2))
    with patch.object(apply_profile, "HERE", bad_package):
        reject("apply rejects wrong final order before writes", lambda: apply_profile.apply(kernel, args.output / "rejected-apply", crystal.IDLE_PROFILE))

    commands = {}
    for profile in (crystal.COMBINED_PROFILE, crystal.IDLE_PROFILE):
        captured = []
        with patch.object(apply_profile, "run", lambda argv, cwd: captured.append(argv)):
            apply_profile.configure(args.audit / "6.6.89-kcompressd-review", profile)
        commands[profile] = captured
    ok("all configure commands equal old combined", commands[crystal.IDLE_PROFILE] == commands[crystal.COMBINED_PROFILE])
    ok("no governor selection or cpuidle configuration added", not any("CPU_IDLE_GOV" in str(x) or "cpuidle.governor" in str(x) for x in commands[crystal.IDLE_PROFILE]))
    ok("new crystal config preserves native/KUnit disabled", not crystal.crystal_configuration_errors(config.splitlines(), crystal.IDLE_PROFILE))
    historical_runs = {}
    for run_id in ("45218661660", "45234533599", "45236244009", "45255407860", "45261048280", "45268976269", "45272103174"):
        previous = args.audit / ("run-" + run_id) / "preflash-verification_6.6.89"
        records = read(previous / "efficiency/efficiency-patches.json")
        profile = records.get("profile", "baseline")
        with patch.object(prepare_boot, "idle_policy", side_effect=AssertionError("old profile read idle policy")):
            ok("historical metadata " + run_id, prepare_boot.validate_profile_evidence(previous, records, profile) == profile)
        historical_runs[run_id] = {"profile": profile, "commit": (previous / "workflow-commit.txt").read_text().strip()}

    workflow = (HERE.parent / ".github/workflows/Build_oneplus_sm8750.yml").read_text(encoding="utf-8")
    ok("workflow input and shell whitelist include new profile", "- '" + crystal.IDLE_PROFILE + "'" in workflow and "|" + crystal.IDLE_PROFILE + ")" in workflow)
    document = yaml.safe_load(workflow)
    start = workflow.index("            ZRAM_PREFLIGHT='drivers/block/zram/zram_drv.o'")
    end = workflow.index('            make -k -j', start)
    preflight_objects = {}
    for profile in (crystal.COMBINED_PROFILE, crystal.IDLE_PROFILE):
        script = args.output / ("objects-" + str(len(preflight_objects)) + ".sh")
        write(script, "EFFICIENCY_PROFILE=" + profile + "\n" + workflow[start:end] + '\nprintf "%s\\n" $ZRAM_PREFLIGHT\n')
        selected = subprocess.run([str(args.bash), str(script)], cwd=kernel, capture_output=True, text=True)
        ok(profile + " workflow object selection succeeds", selected.returncode == 0)
        preflight_objects[profile] = selected.stdout.splitlines()
    ok("same 15 Crystal preflight objects without test/native ZRAM", preflight_objects[crystal.IDLE_PROFILE] == preflight_objects[crystal.COMBINED_PROFILE] and
       len(preflight_objects[crystal.IDLE_PROFILE]) == 15 and not any("codec_test.o" in name or "drivers/block/zram" in name for name in preflight_objects[crystal.IDLE_PROFILE]))
    old_workflow = subprocess.run(["git", "show", "HEAD:.github/workflows/Build_oneplus_sm8750.yml"], cwd=HERE.parent,
                                  capture_output=True, text=True, encoding="utf-8", check=True).stdout
    tokens = ("kmi_baseline.py", "--baseline", "--btf", "--symvers")
    ok("workflow CRC and BTF check invocations unchanged", [line for line in workflow.splitlines() if any(x in line for x in tokens)] ==
       [line for line in old_workflow.splitlines() if any(x in line for x in tokens)])
    core = json.loads(gzip.decompress((HERE / "kmi/plq110-C16-baseline.json.gz").read_bytes()))
    memory = read(HERE / "kmi/plq110-memory-layouts.json")
    ok("all six ABI structure baselines retained", set(core["structures"]) | set(memory) ==
       {"task_struct", "rq", "tcp_sock", "inet_connection_sock", "pglist_data", "scan_control"})
    syntax_checks = 0
    for job in document["jobs"].values():
        for step in job.get("steps", []):
            if "run" in step and any(token in step["run"] for token in ("case \"$EFFICIENCY_PROFILE\"", "CRYSTAL_COMPRESSOR=")):
                script = args.output / ("workflow-" + str(syntax_checks) + ".sh")
                write(script, re.sub(r"\$\{\{.*?\}\}", "github_value", step["run"]))
                result = subprocess.run([str(args.bash), "-n", str(script)], capture_output=True)
                ok("modified workflow syntax " + str(syntax_checks), result.returncode == 0)
                syntax_checks += 1
    ok("all original patches ABI guards and runtime policy untouched", before_hashes == {str(p): sha(p) for p in immutable})
    result = {"passed": True, "checks_count": len(checks), "checks": checks, "profile": crystal.IDLE_PROFILE,
              "patch_count": 22, "source_policy": policy, "source_after_report": source_report,
              "historical_runs": historical_runs, "configure_commands_identical": True,
              "workflow_modified_steps_syntax_checked": syntax_checks,
              "preflight_objects": preflight_objects[crystal.IDLE_PROFILE],
              "kernel_built": False, "full_original21_patch_reapplication_performed": False,
              "new_refinement_patch_really_applied_zero_fuzz": True, "runtime_governor_changed": False,
              "device_used": False, "ci_used": False,
              "immutable_source_sha256": before_hashes}
    write(args.output / "idle-profile-checks.json", json.dumps(result, indent=2) + "\n")
    print(json.dumps({"passed": True, "checks_count": len(checks), "patch_count": 22, "kernel_built": False}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audit", type=Path, required=True)
    parser.add_argument("--before-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--patch", type=Path, default=Path(r"C:\Program Files\Git\usr\bin\patch.exe"))
    parser.add_argument("--bash", type=Path, default=Path(r"C:\Program Files\Git\bin\bash.exe"))
    run(parser.parse_args())
