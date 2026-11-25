"""Focused test-only overlay integration; no kernel build, boot or device I/O."""
import argparse
import contextlib
import copy
import ctypes
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
EFFICIENCY = HERE.parents[1]
sys.path.insert(0, str(EFFICIENCY))
import integrate_crystal as crystal
import apply_profile
import prepare_boot


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def cp(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)


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

    original_package = crystal.HERE
    originals = {path: sha(original_package / path) for path in (
        "sddc-integrity.patch", "sddc-integrity-stats.patch", "sddc-kunit-config.patch",
        "review/sddc-integrity.json", "review/sddc-integrity-stats.json", "review/sddc-kunit-config.json")}
    apply_profile_hash = sha(EFFICIENCY / "apply_profile.py")
    target = crystal.SDDC_INTEGRITY["source_file"]
    raw = (original_package / "source" / target).read_bytes()
    real_run = subprocess.run
    with patch.object(crystal.subprocess, "run", wraps=real_run) as calls:
        intermediate, integrity = crystal.apply_integrity_patch(raw, args.output)
        final, test_policy = crystal.apply_kunit_config_patch(intermediate, args.output)
    commands = [call.args[0] for call in calls.call_args_list]
    ok("real zero-fuzz integrity then test overlay", len(commands) == 4 and
       all("--fuzz=0" in command for command in commands) and
       [Path(command[command.index("-i") + 1]).name for command in commands] ==
       ["sddc-integrity.patch"] * 2 + ["sddc-kunit-config.patch"] * 2)
    ok("integrity intermediate to test overlay installed", hashlib.sha256(intermediate).hexdigest() ==
       test_policy["base_after_integrity_sha256"] and hashlib.sha256(final).hexdigest() == test_policy["patched_sha256"])
    state_names = re.findall(r"\bKUNIT_CASE\((\w+)\)", intermediate.decode())
    ok("state 26 declarations and order unchanged", len(state_names) == 26 and
       re.findall(r"\bKUNIT_CASE\((\w+)\)", final.decode()) == state_names)
    reject("test overlay cannot precede integrity", lambda: crystal.apply_kunit_config_patch(raw, args.output))
    ok("patch scratch cleaned", not list(args.output.glob(".crystal-sddc-integrity-*")))

    # Independently compile the reviewer's exact source extraction in n/y
    # modes. Only XArray/lock/allocation boundaries are mocked, not the test or
    # the two production snapshot function bodies. Do not invoke its metadata writer.
    spec = importlib.util.spec_from_file_location("kunit_config_source_helpers", HERE / "verify_kunit_config_host.py")
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)
    modes = []
    for enabled in (0, 1):
        body = "\n".join(helpers.extract(final.decode(), name) for name in (
            "crystal_sddc_snapshot_locked", "crystal_sddc_snapshot_matches_locked",
            "crystal_sddc_wb_sparse_state_test"))
        source = args.output / f"exact-case-native-{enabled}.c"
        library = args.output / f"exact-case-native-{enabled}.dll"
        write(source, helpers.PREFIX + body + helpers.SUFFIX)
        command = [str(args.compiler), "-shared", "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                   "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fuse-ld=lld", "-nostdlib",
                   f"-DCONFIG_CRYSTAL_HYBRIDSWAP_SDDC_ZMS_NATIVE={enabled}", str(source),
                   "-Wl,/NOENTRY", "-Wl,/NODEFAULTLIB", "-o", str(library)]
        compiled = real_run(command, capture_output=True)
        (args.output / f"native-{enabled}.stderr.txt").write_bytes(compiled.stderr)
        ok(f"native={enabled} exact extracted C compiles", compiled.returncode == 0)
        dll = ctypes.CDLL(str(library.resolve()))
        dll.run_case.restype = ctypes.c_uint
        failures = dll.run_case()
        ok(f"native={enabled} exact test assertions pass", failures == 0)
        modes.append({"native_enabled": enabled, "assertion_failures": failures,
                      "source_sha256": sha(source), "dll_sha256": sha(library)})

    old = args.audit / "lz4kds-lab/profile-smoke-20261124-v2"
    old_integrity = args.audit / "lz4kds-lab/profile-integrity-smoke-20261124-v2"
    combined = args.audit / "lz4kds-lab/combined-stage2-review/profile-checks-v2"

    def fixture(name):
        kernel = args.output / name
        kernel.mkdir(exist_ok=False)
        for rel in ("drivers/Kconfig", "drivers/Makefile", "mm/vmscan.c", "include/trace/hooks/vmscan.h"):
            source = args.audit / "6.6.89-kcompressd-review" / rel
            if not source.is_file():
                source = args.audit / "crystal-base-source" / rel
            cp(source, kernel / rel)
        return kernel

    baseline = load(EFFICIENCY / "patch-series.json")
    stage2 = load(EFFICIENCY / "stage2-series.json")

    def patches(profile):
        return {"version": "6.6.89", "source": baseline["base_commit"], "profile": profile,
                "applied": baseline["patches"] + [stage2[parent] for parent in crystal.stage2_profiles(profile)]}

    profile = "crystal-lz4kds-test"
    kernel = fixture(profile)
    report = crystal.integrate(kernel, kernel / "integration.json", profile)
    ok("test report has exact dedicated overlay record", report["sddc_kunit_config_patch"] == test_policy)
    ok("test preserves immutable import inventory", report["source_sha256"] == crystal.source_manifest(profile)["files"])
    ok("test installs reviewed source", report["installed_sha256"][target] == test_policy["patched_sha256"] == sha(kernel / target))
    config = (old / profile / "evidence/kernel.config").read_text(encoding="utf-8")
    config = re.sub(r'^(CONFIG_CMDLINE=".*)"$', r'\1 kunit.enable=1 kunit.filter_glob=crystal-sddc-*"', config, flags=re.M)
    write(kernel / "out/.config", config)
    evidence = kernel / "evidence"
    write(evidence / "kernel.config", config)
    write(evidence / "efficiency/crystal-integration.json", json.dumps(report))
    for rel in ("mm/page_io.c", "Makefile", "kernel/module/main.c", "lib/lz4kd/Makefile", "fs/erofs/zdata.c", "net/ipv4/tcp_bbr.c"):
        cp(old / profile / rel, kernel / rel)

    def preflight(label):
        with contextlib.redirect_stdout(io.StringIO()):
            apply_profile.verify(kernel, args.output / (label + ".json"), profile)

    preflight("preflight-new-test")
    ok("unchanged apply_profile verifies actual new source", True)
    (kernel / target).write_bytes(intermediate)
    reject("preflight rejects omitted test overlay", lambda: preflight("preflight-reject-old-e511"))
    (kernel / target).write_bytes(final + b"\n")
    reject("preflight rejects altered test source", lambda: preflight("preflight-reject-altered"))
    (kernel / target).write_bytes(final)
    ok("prepare accepts new test evidence", prepare_boot.validate_profile_evidence(evidence, patches(profile), profile) == profile)
    for name, mutate in (
        ("missing overlay record", lambda r: r.pop("sddc_kunit_config_patch")),
        ("wrong overlay digest", lambda r: r["sddc_kunit_config_patch"].__setitem__("patch_sha256", "0" * 64)),
        ("wrong overlay input stage", lambda r: r["sddc_kunit_config_patch"].__setitem__("base_after_integrity_sha256", "0" * 64)),
        ("wrong overlay order", lambda r: r["sddc_kunit_config_patch"]["ordered_patches"].reverse()),
        ("old installed e511", lambda r: r["installed_sha256"].__setitem__(target, integrity["patched_sha256"])),
    ):
        changed = copy.deepcopy(report)
        mutate(changed)
        write(evidence / "efficiency/crystal-integration.json", json.dumps(changed))
        reject("prepare rejects " + name, lambda: prepare_boot.validate_profile_evidence(evidence, patches(profile), profile))
    write(evidence / "efficiency/crystal-integration.json", json.dumps(report))
    for name, changed in (("missing KUnit enable", config.replace(" kunit.enable=1", "")),
                          ("duplicate KUnit enable", config.replace(" kunit.enable=1", " kunit.enable=1 kunit.enable=1")),
                          ("wrong KUnit filter", config.replace("kunit.filter_glob=crystal-sddc-*", "kunit.filter_glob=wrong-*"))):
        write(evidence / "kernel.config", changed)
        reject("prepare still rejects " + name, lambda: prepare_boot.validate_profile_evidence(evidence, patches(profile), profile))
    write(evidence / "kernel.config", config)

    # A package without either test-only file must remain sufficient for all
    # earlier non-test profiles; compare whole imported/installed inventories.
    package = args.output / "package-without-test-overlay"
    shutil.copytree(original_package / "source", package / "source")
    for rel in ("source-manifest.json", "sddc-source-manifest.json", "sddc-integrity.patch", "sddc-integrity-stats.patch",
                "review/sddc-integrity.json", "review/sddc-integrity-stats.json",
                "zms-exact-stats.patch", "review/zms-exact-stats.json", "zms-flush-worker.patch"):
        cp(original_package / rel, package / rel)
    with patch.object(crystal, "HERE", package):
        reject("test rejects absent overlay package", lambda: crystal.integrity_installed_hashes(profile))
        for index, (variant, previous) in enumerate((("crystal", old / "crystal"),
                ("crystal-lz4kds", old_integrity / "crystal-lz4kds"),
                (crystal.COMBINED_PROFILE, combined / crystal.COMBINED_PROFILE))):
            # GNU patch's scratch path also includes the full kernel driver
            # directory; keep fixture names short for Windows MAX_PATH.
            other = fixture("non-test-" + str(index))
            actual = crystal.integrate(other, other / "integration.json", variant)
            historic = load(previous / "integration.json")
            ok(variant + " source inventory unchanged", actual["source_sha256"] == historic["source_sha256"])
            bio = crystal.zms_bio_patch_policy(variant)
            expected = {**historic["installed_sha256"],
                        **{name: item["patched_sha256"] for name, item in bio["files"].items()}}
            ok(variant + " changes only reviewed ZMS sources", actual["installed_sha256"] == expected)
            ok(variant + " does not introduce test stage", "sddc_kunit_config_patch" not in actual)
            old_evidence = previous / "evidence"
            reject(variant + " historical build lacks the ZMS stats fix",
                   lambda: prepare_boot.validate_profile_evidence(old_evidence, patches(variant), variant))
        cp(original_package / "sddc-kunit-config.patch", package / "sddc-kunit-config.patch")
        cp(original_package / "review/sddc-kunit-config.json", package / "review/sddc-kunit-config.json")
        (package / "sddc-kunit-config.patch").write_bytes(b"altered patch\n")
        reject("test rejects altered patch", crystal.kunit_config_patch_policy)
        cp(original_package / "sddc-kunit-config.patch", package / "sddc-kunit-config.patch")
        bad_meta = load(package / "review/sddc-kunit-config.json")
        bad_meta["production_code_changed"] = True
        write(package / "review/sddc-kunit-config.json", json.dumps(bad_meta))
        reject("test rejects changed review metadata", crystal.kunit_config_patch_policy)

    historical_runs = {}
    for run_id in ("45218661660", "45234533599", "45236244009", "45255407860", "45261048280"):
        previous = args.audit / ("run-" + run_id) / "preflash-verification_6.6.89"
        old_patches = load(previous / "efficiency/efficiency-patches.json")
        variant = old_patches.get("profile", "baseline")
        if variant in crystal.CRYSTAL_PROFILES:
            reject("historical Crystal build lacks the ZMS stats fix " + run_id,
                   lambda: prepare_boot.validate_profile_evidence(previous, old_patches, variant))
        else:
            ok("historical non-Crystal " + run_id, prepare_boot.validate_profile_evidence(previous, old_patches, variant) == variant)
        historical_runs[run_id] = variant

    spec = importlib.util.spec_from_file_location("verify_kunit_overlay_declarations", original_package / "verify_kunit.py")
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    declared, metadata = verifier.expected_cases()
    ok("verifier retains 26 state order", declared["crystal-sddc-state"] == state_names)
    ok("verifier retains 6 codec and total32", len(declared["crystal-sddc-codec"]) == 6 and metadata["expected_total_cases"] == 32)
    ok("verifier binds exact test overlay", metadata["sddc_kunit_config_patch"] == test_policy and metadata["installed_sddc_sha256"] == test_policy["patched_sha256"])
    ok("apply_profile source untouched", sha(EFFICIENCY / "apply_profile.py") == apply_profile_hash)
    ok("original patches and review metadata untouched", originals == {path: sha(original_package / path) for path in originals})
    result = {"passed": True, "checks_count": len(checks), "checks": checks, "test_overlay": test_policy,
              "exact_patch_commands": commands, "host_exact_case_modes": modes, "historical_runs": historical_runs,
              "kernel_locks_and_xarray_mocked": True, "kernel_compiled": False, "kernel_kunit_executed": False,
              "device_used": False, "source_sha256": {name: sha(EFFICIENCY / name) for name in (
                  "integrate_crystal.py", "prepare_boot.py", "crystal/verify_kunit.py", "apply_profile.py")}}
    write(args.output / "overlay-integration-checks.json", json.dumps(result, indent=2) + "\n")
    print(json.dumps({"passed": True, "checks_count": len(checks), "kernel_kunit_executed": False}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audit", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, default=Path(r"C:\Program Files\LLVM\bin\clang.exe"))
    run(parser.parse_args())
