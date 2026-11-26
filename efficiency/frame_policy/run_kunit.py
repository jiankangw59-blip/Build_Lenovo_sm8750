"""Run real ARM64 kernel tests and check that the old unsafe path is detected."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent


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
    statuses = []

    def visit(item):
        if isinstance(item, dict):
            if "status" in item:
                statuses.append(item["status"])
            for value in item.values():
                visit(value)
        elif isinstance(item, list):
            for value in item:
                visit(value)

    visit(result)
    if len(statuses) != 22 or set(statuses) != {"PASS"}:
        raise SystemExit("The kernel suite did not report exactly twenty-two passing cases")
    shutil.copyfile(output / ".config", report / "debug.config")
    print("Fixed kernel passed scheduler context and fence lifecycle tests", flush=True)

    source = kernel / "kernel/sched/frame_policy/frame_policy.c"
    text = source.read_text(encoding="utf-8")
    old = "\t\tirq_work_queue(&scene_irq_work);"
    if text.count(old) != 1:
        raise SystemExit("Cannot isolate the one-line legacy comparison")
    source.write_text(text.replace(old, "\t\tqueue_work(policy_wq, &scene_work);"), encoding="utf-8")
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
