"""Strict memory-log and identity gate fixtures; all ADB calls are mocked."""
import argparse
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import shlex
import subprocess
from types import SimpleNamespace
from unittest.mock import patch

import device_gates as gates
from crystal import verify_kunit

BOOT = "f55cf97e-666a-4779-bdf6-fd6d2424ccbc"
OTHER_BOOT = "fe3db5c0-92b0-4b80-91c1-942e87943193"
NOTES = "7" * 64
PREFIX = "[    0.123456] "


def mismatch(path):
    return ("crystal_hybridswap: sddc_integrity mismatch path=" + path + " dev=unknown index=0 kind=1"
            " ref=41:23 ref_size=4096 target_size=4096 wire_size=0 mutation_seq=0"
            " expected_hash=0x12345679 actual_hash=0x12345678 ref_refs=3")


def capture():
    expected, _ = verify_kunit.expected_cases()
    lines = ["KTAP version 1", "1..2"]
    for result, (name, cases) in enumerate(expected.items(), 1):
        lines += ["    KTAP version 1", "    # Subtest: " + name, "    1.." + str(len(cases))]
        for number, case in enumerate(cases, 1):
            if case == verify_kunit.NEGATIVE_CASE:
                lines += [mismatch("resident_read"), mismatch("resident_read_locked")]
            lines.append(f"    ok {number} {case}")
        lines.append(f"ok {result} {name}")
    return (BOOT + "\n" + NOTES + "  /sys/kernel/notes\nLinux version 6.6.89-fixture\n" +
            "\n".join(PREFIX + line for line in lines) + "\n").encode()


def run(args):
    args.output.mkdir(parents=True, exist_ok=False)
    production = Path(gates.__file__)
    source_hash = hashlib.sha256(production.read_bytes()).hexdigest()
    checks, results = {}, {}
    valid = capture()
    live = valid.split(b"Linux version 6.6.89-fixture\n", 1)[1]

    def ok(name, value):
        assert value, name
        checks[name] = True

    def review(name, live_data=live, startup=valid, notes=NOTES, boot=BOOT, error=False, exemptions=0):
        result = gates.review_memory_logs(live_data, startup, notes, boot)
        ok(name, bool(result["errors"]) == error and len(result["controlled_selftest_exemptions"]) == exemptions)
        results[name] = result
        return result

    with patch.object(verify_kunit, "verify_log", wraps=verify_kunit.verify_log) as verifier:
        result = review("full32_exact_raw_fixture_deduplicated", exemptions=2)
        ok("startup parsed once not concatenated twice", verifier.call_count == 1 and verifier.call_args.args[0] == valid)
        ok("duplicate origins retained", all({x["source"] for x in row["origins"]} == {"live_dmesg", "startup"}
                                            for row in result["controlled_selftest_exemptions"]))
    review("no startup proof no exemption", startup=None, error=True)
    review("wrong notes no exemption", notes="8" * 64, error=True)
    review("wrong boot no exemption", boot=OTHER_BOOT, error=True)
    review("incomplete32 no exemption", startup=valid.replace(b"    ok 6 crystal_sddc_codec_lz4kd_decode_delta_bounds_test", b"    not ok 6 crystal_sddc_codec_lz4kd_decode_delta_bounds_test"), error=True)
    # Remove the last suite result without depending on any particular case name.
    review("truncated suite no exemption", startup=b"\n".join(valid.splitlines()[:-1]) + b"\n", error=True)
    bad = valid.replace(("    ok 2 " + verify_kunit.NEGATIVE_CASE).encode(),
                        ("    not ok 2 " + verify_kunit.NEGATIVE_CASE).encode(), 1)
    review("failed negative case no exemption", startup=bad, error=True)
    review("unprefixed fixture cannot be exempted", live_data=live.replace(PREFIX.encode(), b""),
           startup=valid.replace(PREFIX.encode(), b""), error=True)
    review("later same text new timestamp is real error", live_data=live + ("[  900.000001] " + mismatch("resident_read") + "\n").encode(), error=True, exemptions=2)
    review("later real zram0 hash mismatch remains error", live_data=live + (PREFIX + mismatch("resident_read").replace("dev=unknown", "dev=zram0") + "\n").encode(), error=True, exemptions=2)
    review("no broad KUnit context exemption", live_data=live + b"[ 0.2] kunit: crystal_sddc decode failed ret=-5\n", error=True, exemptions=2)
    review("no broad SDDC exemption", live_data=live + b"[ 0.2] SDDC delta decode corrupt\n", error=True, exemptions=2)
    review("startup later fixture-looking error invalidates proof", startup=valid + ("[ 2.0] " + mismatch("resident_read") + "\n").encode(), error=True)
    review("CRCRLF uses verifier line numbering", live_data=live.replace(b"\n", b"\r\r\n"), startup=valid.replace(b"\n", b"\r\r\n"), exemptions=2)

    for index, line in enumerate((
        "zram failed_reads=0 failed_writes=0", "ZMS physical_read_failed_pages: 0 physical_write_failed_pages: 0",
        "SDDC integrity_failures=0 decode_failures=0 flatten_failures=0 delta_proof_failures=0",
        "Crystal errors=0 corruptions=0 hash_mismatches=0 ret=0", "zram io_errors=0x0 ret=+0", "I/O errors=0",
    )):
        ok("zero statistics " + str(index), not gates.memory_error_reasons(line))
    for index, line in enumerate((
        "zram compression failed ret=-12", "lz4kd decompression error", "Crystal backing read failed ret=-5",
        "ZMS physical_read_failed_pages=1", "ZMS writeback ret=-5", "SDDC integrity_failures=2",
        "SDDC expected_hash=0x123 actual_hash=0x456", "Crystal hash mismatch", "SDDC delta corrupt",
        "BUG: bad page", "Oops: crash", "I/O error, dev loop0", "zram failures=0 but decoding failed ret=-5",
    )):
        ok("real errors " + str(index), bool(gates.memory_error_reasons(line)))

    critical = ("sched_walt", "qti_thermal_vendor_hooks", "qcom_cpufreq_thermal", "thermal_config", "oplus_charger_present", "oplus_network_tuning")

    def inspect(name, log=b"", startup=None, expected=NOTES, end_boot=BOOT, end_notes=NOTES, codec="lz4kd"):
        boot_reads, note_reads, calls = 0, 0, []
        output = args.output / (name + ".json")
        startup_path = None
        if startup is not None:
            startup_path = args.output / (name + ".startup.raw")
            startup_path.write_bytes(startup)

        def fake_run(argv, **kwargs):
            nonlocal boot_reads, note_reads
            assert argv[:4] == ["adb", "-s", "HOST-FIXTURE", "exec-out"]
            remote = shlex.split(argv[4])
            assert remote[:2] == ["su", "-c"] and len(remote) == 3
            code = remote[2].strip()
            calls.append(code)
            if code == "getprop ro.product.model": data = "PLQ110"
            elif code == "cat /proc/sys/kernel/random/boot_id":
                boot_reads += 1
                data = BOOT if boot_reads == 1 else end_boot
            elif code == "sha256sum /sys/kernel/notes":
                note_reads += 1
                data = (NOTES if note_reads == 1 else end_notes) + "  /sys/kernel/notes"
            elif code == "cat /proc/version": data = "Linux version 6.6.89-fixture"
            elif code == "cat /proc/modules": data = "\n".join(x + " 1 0 - Live 0x0" for x in critical)
            elif code == "dmesg": return subprocess.CompletedProcess(argv, 0, log + b"\n[ 0.1] Re-Kernel hooked!\n", b"")
            elif "enable_rcu_lazy" in code: data = "Y"
            elif code.startswith("ps -A"): data = "\n".join(f"1 1 rcuop/{n}" for n in range(8))
            elif code == "cat /sys/devices/virtual/workqueue/cpumask": data = "3f"
            elif "workqueue/parameters/power_efficient" in code: data = "Y"
            elif code.startswith("for p in /proc/"): data = "101 kworker/u16:0 3f"
            elif code == "cat /sys/block/zram0/comp_algorithm": data = "lz4 [lz4kd] zstd"
            elif "cat /sys/block/zram0/backing_dev" in code: data = "/dev/block/loop49"
            elif "cat /sys/block/zram0/bd_stat" in code: data = "1 64 64"
            elif "ace6-rekernel-probe" in code: data = "family=rekernel id=28\nversion_response=11.7"
            elif code.startswith("for pid in"): data = "1 AppOpt\nCpus_allowed_list: 0-5"
            elif code.startswith("cat /proc/sys/net/"): data = "bbr\n2\nfq_codel"
            elif code.startswith("for n in status"): data = "status=Discharging"
            else: raise AssertionError("Unexpected read-only command: " + code)
            return subprocess.CompletedProcess(argv, 0, data.encode() + b"\n", b"")

        arguments = SimpleNamespace(serial="HOST-FIXTURE", output=output, expected_notes_sha256=expected,
                                    expected_compressor=codec, startup_snapshot=startup_path)
        with patch.object(gates.subprocess, "run", side_effect=fake_run), contextlib.redirect_stdout(io.StringIO()):
            passed = gates.inspect(arguments)
        result = json.loads(output.read_text(encoding="utf-8"))
        ok(name + " end identity read twice", boot_reads == 2 and note_reads == 2)
        ok(name + " raw capture preserved", output.with_suffix(".dmesg.raw").read_bytes().endswith(b"Re-Kernel hooked!\n"))
        return passed, result

    passed, _ = inspect("old native gate positive")
    ok("old native still passes", passed)
    passed, _ = inspect("valid test fixture gate positive", live, valid)
    ok("full32 exact fixture gate passes", passed)
    passed, _ = inspect("wrong expected notes", expected="8" * 64)
    ok("wrong expected notes cannot pass", not passed)
    passed, result = inspect("boot drift", end_boot=OTHER_BOOT)
    ok("boot drift cannot pass", not passed and not result["identity_guard_passed"])
    passed, result = inspect("notes drift", end_notes="8" * 64)
    ok("notes drift cannot pass", not passed and not result["identity_guard_passed"])
    passed, result = inspect("late real error", live + ("[ 1.0] " + mismatch("resident_read") + "\n").encode(), valid)
    ok("late real error makes memory gate fail", not passed and not result["gates"]["zram_writeback"]["passed"])
    passed, result = inspect("regulatory db exemption", b"cfg80211: failed to load regulatory.db\n")
    ok("original narrow regulatory db exemption retained", passed and len(result["gates"]["factory_modules"]["firmware_warnings"]) == 1)
    passed, result = inspect("other firmware error", b"cfg80211: failed to load other.db\n")
    ok("other loader errors not exempted", not passed and result["gates"]["factory_modules"]["load_errors"])
    passed, _ = inspect("wrong expected codec", codec="lz4kds")
    ok("expected codec check not relaxed", not passed)
    ok("production source frozen", hashlib.sha256(production.read_bytes()).hexdigest() == source_hash)
    report = {"passed": True, "checks_count": len(checks), "checks": checks, "memory_reviews": results,
              "production_source_sha256": source_hash, "kunit_verifier_sha256": hashlib.sha256(Path(verify_kunit.__file__).read_bytes()).hexdigest(),
              "adb_mocked": True, "device_used": False, "kernel_built": False}
    (args.output / "device-gates-host-checks.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "checks_count": len(checks), "device_used": False}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    run(parser.parse_args())
