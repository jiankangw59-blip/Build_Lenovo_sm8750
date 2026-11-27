"""Run real ARM64 kernel tests and check that the old unsafe path is detected."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent


def expected_cases():
    names = []
    for name in ("frame_policy_kunit.h", "scene_policy_kunit.h"):
        names.extend(re.findall(r"\bKUNIT_CASE\((frame_test_[A-Za-z0-9_]+)\)",
                                (HERE / name).read_text(encoding="utf-8")))
    if not names or len(set(names)) != len(names):
        raise ValueError("The reviewed KUnit declarations are empty or duplicated")
    return sorted(names)


def validate_cases(result, expected):
    cases = []

    def visit(item):
        if isinstance(item, dict):
            declared = item.get("test_cases", [])
            if not isinstance(declared, list):
                raise ValueError("Malformed KUnit case list")
            cases.extend(declared)
            for group in item.get("sub_groups", []):
                visit(group)

    visit(result)
    if any(not isinstance(item, dict) or not isinstance(item.get("name"), str)
           or item.get("status") != "PASS" for item in cases):
        raise ValueError("A kernel case is malformed or did not pass")
    names = [item["name"] for item in cases]
    if len(set(names)) != len(names) or sorted(names) != sorted(expected):
        raise ValueError("Kernel case coverage differs: missing="
                         + repr(sorted(set(expected) - set(names))) + " unexpected="
                         + repr(sorted(set(names) - set(expected))))
    return {"expected": sorted(expected), "executed": sorted(names),
            "all_passed": True, "case_count": len(names)}


def legacy_sched_switch_source(text):
    # Exclude the forward declaration and other legitimate IRQ bridges. The
    # following function is a second boundary check for the reviewed fixture.
    matches = list(re.finditer(
        r"^static void scene_switch\([^;{}]*\)\n\{(?P<body>.*?)^\}\n\nstatic void scene_irq\(",
        text, re.MULTILINE | re.DOTALL))
    if len(matches) != 1:
        raise ValueError("Cannot isolate the scene_switch legacy comparison")
    start, end = matches[0].span("body")
    body = text[start:end]
    old = "\t\tirq_work_queue(&scene_irq_work);"
    if body.count(old) != 1:
        raise ValueError("Cannot isolate the one-line legacy comparison in scene_switch")
    return text[:start] + body.replace(old, "\t\tqueue_work(policy_wq, &scene_work);", 1) + text[end:]


def main(kernel, output, report):
    report.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    program = ["python3", str(kernel / "tools/testing/kunit/kunit.py")]
    common = ["--arch=arm64", "--make_options=LLVM=1",
              "--kunitconfig=" + str(HERE / "kunitconfig"),
              "--build_dir=" + str(output), "--qemu_args=-smp 4 -m 1024M"]

    def run(arguments, name, timeout=1800):
        log = report / (name + ".driver.log")
        print("Running " + name, flush=True)
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(program + arguments, cwd=kernel,
                                    stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        if (output / "test.log").exists():
            shutil.copyfile(output / "test.log", report / (name + ".kernel.log"))
        if result.returncode:
            print("\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[-35:]))
        return result.returncode

    fixed = run(["run", *common, "--jobs=2", "--timeout=90", "--kernel_args=panic_on_warn=1",
                 "--json=" + str(report / "fixed.json"), "frame_policy"], "fixed")
    if fixed:
        raise SystemExit("Fixed kernel did not pass its KUnit tests")
    markers = ("possible circular locking", "possible recursive locking", "BUG: spinlock",
               "WARNING: possible", "BUG: scheduling while atomic", "BUG: KASAN:")
    fixed_log = (report / "fixed.kernel.log").read_text(encoding="utf-8", errors="replace")
    if any(marker in fixed_log for marker in markers):
        raise SystemExit("The fixed kernel emitted a locking or memory-safety warning")
    result = json.loads((report / "fixed.json").read_text(encoding="utf-8"))
    try:
        coverage = validate_cases(result, expected_cases())
    except ValueError as error:
        raise SystemExit(str(error)) from error
    coverage["test_header_sha256"] = {
        name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
        for name in ("frame_policy_kunit.h", "scene_policy_kunit.h")}
    (report / "case-coverage.json").write_text(json.dumps(coverage, indent=2) + "\n", encoding="utf-8")
    shutil.copyfile(output / ".config", report / "debug.config")
    print(f"Fixed kernel passed all {coverage['case_count']} declared cases", flush=True)

    source = kernel / "kernel/sched/frame_policy/frame_policy.c"
    text = source.read_text(encoding="utf-8")
    try:
        legacy_source = legacy_sched_switch_source(text)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    source.write_text(legacy_source, encoding="utf-8")
    if run(["build", *common, "--jobs=2"], "legacy-build"):
        raise SystemExit("Legacy comparison did not compile")
    legacy = run(["exec", *common, "--timeout=40", "--kernel_args=panic_on_warn=1",
                  "--json=" + str(report / "legacy.json"),
                  "frame_policy.frame_test_sched_switch_handoff"], "legacy", timeout=90)
    log = (report / "legacy.kernel.log").read_text(encoding="utf-8", errors="replace")
    lock_failure = any(marker in log for marker in markers)
    hung_after_start = "Exercising the actual sched_switch" in log and legacy != 0
    # lockdep reports an invalid dependency even when scheduling happens not
    # to deadlock in this run and KUnit's individual assertions still pass.
    detected = lock_failure or hung_after_start
    comparison = {"fixed_all_cases_passed": True, "legacy_exit": legacy,
                  "legacy_lock_warning": lock_failure, "legacy_detected": detected,
                  "production_image": False, "device_testing": False}
    (report / "comparison.json").write_text(json.dumps(comparison, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(comparison, indent=2), flush=True)
    if not detected:
        raise SystemExit("The old unsafe path was not reproduced; regression coverage needs review")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    main(args.kernel.resolve(), args.output.resolve(), args.report.resolve())
