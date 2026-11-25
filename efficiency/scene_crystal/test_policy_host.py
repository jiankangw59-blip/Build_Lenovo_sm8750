"""Run shipped Crystal policy against isolated files and explicit syscall mocks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent
NOTES = "a" * 64


class LFPath(type(Path())):
    def write_text(self, data, encoding=None, errors=None, newline=None):
        return super().write_text(data, encoding=encoding, errors=errors,
                                  newline="\n" if newline is None else newline)


class Fixture:
    def __init__(self, root, bash, codec="lz4kd", auto=True):
        root = LFPath(root)
        self.root, self.bash = root, bash
        self.state, self.node = root / "state", root / "sys/block/zram0"
        for directory in (self.state, self.node, root / "sys/block/loop49/loop",
                          root / "proc/sys/kernel/random", root / "sys/class/backlight/panel0-backlight",
                          root / "sys/class/power_supply/battery", root / "sys/kernel", root / "dev/block"):
            directory.mkdir(parents=True, exist_ok=True)
        files = {
            "state/crystal-writeback.enabled": "", "state/crystal-writeback-auto.enabled": "",
            "state/known-crystal-kernels": f"{NOTES} {codec}\n", "state/zms-backing.img": "mock-only",
            "sys/block/zram0/backing_dev": (root / "dev/block/loop49").as_posix(),
            "sys/block/zram0/dev": "254:0", "sys/block/loop49/dev": "7:49",
            "sys/block/loop49/loop/backing_file": (self.state / "zms-backing.img").as_posix(),
            "sys/block/zram0/hybridswap_enable": "0", "sys/block/zram0/hybridswap_core_enable": "0",
            "sys/block/zram0/writeback_limit": "100000", "sys/block/zram0/writeback_limit_enable": "1",
            "sys/block/zram0/disksize": "17179869184", "sys/block/zram0/comp_algorithm": f"[{codec}]",
            "sys/block/zram0/mm_stat": "2000000000 1500000000 1200000000 0 0 0 0 0 0",
            "sys/block/zram0/io_stat": "0 0 0 0", "proc/meminfo": "MemAvailable: 2000000 kB\n",
            "proc/uptime": "10000.00 20000.00", "proc/sys/kernel/random/boot_id": "host-boot-1",
            "sys/class/backlight/panel0-backlight/brightness": "0",
            "sys/class/power_supply/battery/temp": "350", "sys/kernel/notes": "mock notes",
            "swap_config.conf": f"zram=true\nzram_size=16384\ncomp_algorithm={codec}\n",
            "day": "20261124", "flush_mode": "ok", "mock-result": "32768 4096 0 none 0 1",
            "proc/swaps": "Filename Type Size Used Priority\n" + (root / "dev/block/zram0").as_posix() + " partition 16777212 0 0\n",
            "dev/block/zram0": "", "dev/block/loop49": "",
        }
        for path, content in files.items():
            (root / path).write_text(content + ("" if content.endswith("\n") else "\n"), encoding="utf-8")
        config = (HERE / "crystal-writeback.conf").read_text(encoding="utf-8")
        (self.state / "crystal-writeback.conf").write_text(config.replace("auto=false", "auto=true") if auto else config)
        self.stats(0)
        self.sddc(codec == "lz4kds")
        replacements = [
            ("/data/adb/ace6-kernel-efficiency", self.state.as_posix()),
            ("/data/swap_config.conf", (root / "swap_config.conf").as_posix()),
            ("/sys/", (root / "sys").as_posix() + "/"),
            ("/proc/", (root / "proc").as_posix() + "/"),
            ("/dev/block/", (root / "dev/block").as_posix() + "/"),
            ("/dev/zram0", (root / "dev/zram0").as_posix()),
            ("/system/bin/getconf", "test_getconf"),
        ]
        for name in ("crystal_writeback.sh", "crystal_writeback_policy.sh", "crystal_writeback_daemon.sh", "start_crystal_writeback.sh"):
            text = (HERE / name).read_text(encoding="utf-8")
            mapping = dict(replacements)
            text = re.sub("|".join(re.escape(key) for key in sorted(mapping, key=len, reverse=True)),
                          lambda match: mapping[match.group()], text)
            if name == "crystal_writeback_policy.sh":
                text += r'''
# Host-only boundaries. Enrollment, journals, arithmetic and control flow above
# are the shipped implementation, not a Python reconstruction.
getprop() { [ "$1" != ro.product.model ] || { builtin printf 'PLQ110\n'; return; }; builtin printf '1\n'; }
sha256sum() { builtin printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  %s\n' "$1"; }
test_getconf() { builtin printf '4096\n'; }
flock() { return 0; }
date() { [ "${2:-}" != +%Y%m%d ] || { cat "$DIR/day"; return; }; command date "$@"; }
ace6_crystal_validate_backing() { return 0; }
ace6_crystal_loop_path() { [ "$1" = "$DIR/dev/block/loop49" ] && builtin printf '%s\n' "$1"; }
test_stats() {
 builtin printf 'enabled: 1\nret: 0\nphysical_write_pages: %s\nphysical_write_bytes: %s\nphysical_write_failed_pages: %s\nphysical_read_failed_pages: %s\ndirty_pages: %s\nbd_stat_write_pages: %s\n' \
  "$1" "$(( $1 * 4096 ))" "$2" "${5:-0}" "$3" "$4" > "$CW_NODE/zms_stat"
 builtin printf '0 0 %s\n' "$4" > "$CW_NODE/bd_stat"
}
test_flush() {
 builtin printf '%s\n' "$*" >> "$DIR/flush-calls"
 [ "$1" != --block ] || {
  case "$(cat "$DIR/flush_mode")" in fail) return 2 ;; dirty) return 0 ;; esac
  sed -i 's/^dirty_pages:.*/dirty_pages: 0/' "$CW_NODE/zms_stat"
 }
 return 0
}
CW_FLUSH=test_flush
printf() {
 if [ "$1" = 'idle\n' ]; then
  read -r logical physical failed kind errors rc < "$DIR/mock-result"
  left=$(cat "$CW_NODE/writeback_limit")
  builtin printf '%s\n' "$((left-logical))" > "$CW_NODE/writeback_limit"
  test_stats "$physical" "$failed" 1 "$physical"
  if [ "$kind" = physical_read_failure ]; then
   sed -i "s/^physical_read_failed_pages:.*/physical_read_failed_pages: $errors/" "$CW_NODE/zms_stat"
  elif [ "$kind" = io_write_failure ]; then
   builtin printf '0 %s 0 0\n' "$errors" > "$CW_NODE/io_stat"
  elif [ "$kind" != none ]; then
   sed -i "s/^$kind:.*/$kind: $errors/" "$CW_NODE/sddc_stat"
  fi
  builtin printf '%s\n' submitted >> "$DIR/submissions"
  builtin printf "$@"
  return "$rc"
 fi
 builtin printf "$@"
}
'''
            (root / name).write_text(text, encoding="utf-8", newline="\n")
        event = '#!/bin/bash\ncase "$1" in --watch) echo screen_off ;; --grace) exit "$(cat "${0%/*}/grace_rc")" ;; esac\n'
        (root / "native_writeback_event").write_text(event, newline="\n")
        (root / "native_writeback_event").chmod(0o755)
        (root / "grace_rc").write_text("0\n")

    def stats(self, physical, failed=0, dirty=0, bd=None, read_failed=0):
        (self.node / "zms_stat").write_text(
            f"enabled: 1\nret: 0\nphysical_write_pages: {physical}\nphysical_write_bytes: {physical*4096}\n"
            f"physical_write_failed_pages: {failed}\nphysical_read_failed_pages: {read_failed}\n"
            f"dirty_pages: {dirty}\nbd_stat_write_pages: {physical if bd is None else bd}\n")
        (self.node / "bd_stat").write_text(f"0 0 {physical if bd is None else bd}\n")

    def sddc(self, enabled, **errors):
        text = f"enabled: {int(enabled)}\n"
        if enabled:
            text += "".join(f"{name}: {errors.get(name, 0)}\n" for name in (
                "integrity_failures", "decode_failures", "flatten_failures", "delta_proof_failures"))
        (self.node / "sddc_stat").write_text(text)

    def run(self, body, rc=0, daemon=False):
        environment = dict(os.environ, HOST_TEST_DIR=self.root.as_posix())
        command = [str(self.bash)]
        if daemon:
            command += [(self.root / "crystal_writeback_daemon.sh").as_posix()]
        else:
            command += ["-c", 'DIR=$HOST_TEST_DIR; . "$DIR/crystal_writeback_policy.sh"; ' + body]
        result = subprocess.run(command, env=environment, capture_output=True, timeout=60)
        assert result.returncode == rc, (body, result.returncode, result.stdout.decode("utf-8", "replace"), result.stderr.decode("utf-8", "replace"))
        return result.stdout.decode("utf-8", "replace").strip()

    def daily(self):
        return (self.state / "native-writeback.daily").read_text().strip()


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    def case(name, codec="lz4kd", auto=True):
        fixture = Fixture(args.output / name, args.bash, codec, auto)
        results.append({"case": name, "passed": True})
        return fixture

    for name, change in (
        ("native_unlisted", lambda f: (f.node / "zms_stat").unlink()),
        ("le9uo_notes_unlisted", lambda f: (f.state / "known-crystal-kernels").write_text("b"*64+" lz4kd\n")),
        ("default_auto_false", lambda f: None),
        ("lz4kds_missing_integrity_stats", lambda f: (f.node / "sddc_stat").write_text("enabled: 1\ndecode_failures: 0\nflatten_failures: 0\ndelta_proof_failures: 0\n")),
    ):
        f = case(name, "lz4kds" if name.startswith("lz4kds") else "lz4kd", auto=name!="default_auto_false")
        change(f)
        subprocess.run([str(args.bash), (f.root / "start_crystal_writeback.sh").as_posix()],
                       check=True, timeout=60)
        assert not (f.state / "native-writeback.daily").exists()
        assert not list(f.state.glob("*log")) and not list(f.state.glob("*lock"))

    f = case("lz4kds_complete_stats_eligible", "lz4kds")
    f.run("cw_eligible")
    f = case("canonical_decimal_limits")
    f.run("cw_number 0 && ! cw_number 00 && cw_number 9007199254740991 && ! cw_number 9007199254740992 && cw_small 2147483647 && ! cw_small 2147483648")
    for name, path, value in (
        ("hot", "sys/class/power_supply/battery/temp", "401"),
        ("low_ram", "proc/meminfo", "MemAvailable: 524287 kB"),
        ("screen_on", "sys/class/backlight/panel0-backlight/brightness", "20"),
        ("driver_policy_enabled", "sys/block/zram0/hybridswap_enable", "1"),
        ("scene_writeback_disabled", "swap_config.conf", "zram=true\nzram_size=16384\ncomp_algorithm=lz4kd\nzram_writeback=false\n"),
    ):
        f = case(name)
        (f.root / path).write_text(value + "\n")
        f.run("cw_eligible", rc=1)

    f = case("logical_not_physical_restore")
    f.run("cw_load_config && cw_begin_round")
    (f.node / "writeback_limit").write_text("0\n")
    f.stats(4096, dirty=1)
    f.run("cw_load_config && cw_settle_pending")
    assert (f.node / "writeback_limit").read_text().strip() == "67232"
    assert f.daily() == "20261124 4096"
    assert (f.state / "crystal-writeback.pending").read_text().strip() == "none"

    f = case("shared_native_ledger_manual_gc")
    (f.state / "native-writeback.daily").write_text("20261124 1234\n")
    f.stats(100, bd=50)
    f.run("cw_load_config && cw_observe_physical")
    f.stats(150, bd=51)
    f.run("cw_load_config && cw_observe_physical")
    assert f.daily() == "20261124 1384"

    f = case("unchanged_physical_has_no_state_fsync")
    f.run("cw_load_config && cw_observe_physical")
    calls = (f.root / "flush-calls").read_bytes()
    f.run("cw_load_config && cw_observe_physical")
    assert (f.root / "flush-calls").read_bytes() == calls

    f = case("watermark_crash_conservative_double_charge")
    f.stats(10)
    f.run("cw_load_config && cw_observe_physical")
    old = (f.state / "crystal-writeback.physical").read_bytes()
    f.stats(20)
    f.run("cw_load_config && cw_observe_physical")
    (f.state / "crystal-writeback.physical").write_bytes(old)
    f.run("cw_load_config && cw_observe_physical")
    assert f.daily() == "20261124 30"

    f = case("fresh_boot_preserves_daily")
    f.stats(100)
    f.run("cw_load_config && cw_observe_physical")
    (f.root / "proc/sys/kernel/random/boot_id").write_text("host-boot-2\n")
    f.stats(7)
    f.run("cw_load_config && cw_observe_physical")
    assert f.daily() == "20261124 107"

    f = case("counter_regression_refused")
    f.stats(100)
    f.run("cw_load_config && cw_observe_physical")
    f.stats(1)
    f.run("cw_load_config && cw_observe_physical", rc=1)
    assert f.daily() == "20261124 100"

    f = case("pending_counter_reset_does_not_restore_old_limit")
    f.stats(100)
    f.run("cw_load_config && cw_begin_round")
    (f.node / "writeback_limit").write_text("32758\n")
    f.stats(0)
    f.run("cw_load_config && cw_settle_pending", rc=1)
    assert (f.node / "writeback_limit").read_text().strip() == "32758"

    f = case("physical_read_counter_required_before_cap")
    stat = f.node / "zms_stat"
    stat.write_text(re.sub(r"^physical_read_failed_pages:.*\n", "", stat.read_text(), flags=re.M))
    f.run("cw_load_config && cw_begin_round", rc=1)
    assert (f.node / "writeback_limit").read_text().strip() == "100000"
    assert not (f.state / "crystal-writeback.pending").exists()

    f = case("pending_physical_read_counter_reset_no_restore")
    f.stats(100, read_failed=7)
    f.run("cw_load_config && cw_begin_round")
    (f.node / "writeback_limit").write_text("32758\n")
    f.stats(110, read_failed=6)
    f.run("cw_load_config && cw_settle_pending", rc=1)
    assert (f.node / "writeback_limit").read_text().strip() == "32758"
    assert (f.state / "crystal-writeback.pending").read_text().startswith("v2 ")

    for name in ("legacy_v1_pending_refused", "v2_read_baseline_missing", "v2_read_baseline_malformed"):
        f = case(name)
        f.run("cw_load_config && cw_begin_round")
        pending = f.state / "crystal-writeback.pending"
        fields = pending.read_text().split()
        assert fields[0] == "v2" and len(fields) == 21
        if name == "legacy_v1_pending_refused":
            fields[0] = "v1"
            fields.pop()
        elif name == "v2_read_baseline_missing":
            fields.pop()
        else:
            fields[-1] = "unknown"
        pending.write_text(" ".join(fields) + "\n")
        original = pending.read_bytes()
        (f.node / "writeback_limit").write_text("22768\n")
        f.stats(1000)
        f.run("cw_load_config && cw_settle_pending", rc=1)
        f.run("", rc=1, daemon=True)
        assert (f.node / "writeback_limit").read_text().strip() == "22768"
        assert pending.read_bytes() == original
        assert (f.state / "crystal-writeback.blocked").exists()
        assert not (f.root / "submissions").exists()

    f = case("none_pending_trailing_fields_refused")
    (f.state / "crystal-writeback.pending").write_text("none unexpected\n")
    f.run("cw_load_config && cw_begin_round", rc=1)
    assert (f.node / "writeback_limit").read_text().strip() == "100000"

    f = case("existing_pending_not_overwritten")
    f.run("cw_load_config && cw_begin_round")
    pending = (f.state / "crystal-writeback.pending").read_bytes()
    f.run("cw_load_config && cw_begin_round", rc=1)
    assert (f.state / "crystal-writeback.pending").read_bytes() == pending

    f = case("daily_target_exhausted")
    (f.state / "native-writeback.daily").write_text("20261124 524288\n")
    f.run("cw_load_config && cw_begin_round", rc=2)
    assert (f.node / "writeback_limit").read_text().strip() == "100000"
    assert not (f.state / "crystal-writeback.pending").exists()

    f = case("cap_min_remaining_daily")
    (f.state / "native-writeback.daily").write_text("20261124 524188\n")
    f.run("cw_load_config && cw_begin_round")
    assert (f.node / "writeback_limit").read_text().strip() == "100"

    for name, mutate in (
        ("same_boot_armed_recovery", lambda f: None),
        ("cross_boot_pending_refused", lambda f: (f.root / "proc/sys/kernel/random/boot_id").write_text("host-boot-2\n")),
        ("foreign_backing_pending_refused", lambda f: (f.node / "backing_dev").write_text("/dev/block/mapper/foreign\n")),
        ("flush_failure_retains_journal", lambda f: (f.root / "flush_mode").write_text("fail\n")),
        ("false_success_dirty_retains_journal", lambda f: (f.root / "flush_mode").write_text("dirty\n")),
    ):
        f = case(name)
        f.run("cw_load_config && cw_begin_round")
        (f.node / "writeback_limit").write_text("22768\n")
        f.stats(1000, dirty=1)
        mutate(f)
        failed = name != "same_boot_armed_recovery"
        f.run("cw_load_config && cw_settle_pending", rc=int(failed))
        assert (f.node / "writeback_limit").read_text().strip() == (
            "22768" if name.startswith(("cross_", "foreign_")) else "90000")
        assert ((f.state / "crystal-writeback.pending").read_text().strip() != "none") == failed

    for name, current in (("restore_crash_idempotent", 90000), ("external_limit_change_refused", 89999)):
        f = case(name)
        f.run("cw_load_config && cw_begin_round")
        p = f.state / "crystal-writeback.pending"
        fields = p.read_text().split()
        fields[6:10] = ["restoring", "22768", "90000", "10000"]
        p.write_text(" ".join(fields)+"\n")
        (f.node / "writeback_limit").write_text(str(current)+"\n")
        f.stats(1000)
        f.run("cw_load_config && cw_settle_pending", rc=int(current!=90000))
        assert (f.node / "writeback_limit").read_text().strip() == str(current)

    f = case("prepared_cap_not_installed")
    f.run("cw_load_config && cw_begin_round")
    (f.node / "writeback_limit").write_text("100000\n")
    f.run("cw_load_config && cw_settle_pending")
    assert (f.node / "writeback_limit").read_text().strip() == "100000"

    for name, result, rc, codec in (
        ("quota_stop_is_not_physical_cap", "32768 4096 0 none 0 1", 0, "lz4kd"),
        ("partial_error_stops", "10000 1000 0 none 0 1", 1, "lz4kd"),
        ("quota_with_real_io_error_stops", "32768 4096 1 none 0 1", 1, "lz4kd"),
        ("quota_with_gc_read_error_stops", "32768 4096 0 physical_read_failure 1 1", 1, "lz4kd"),
        ("success_rc_with_gc_read_error_stops", "32768 4096 0 physical_read_failure 1 0", 1, "lz4kd"),
        ("quota_with_integrity_error_stops", "32768 4096 0 integrity_failures 1 1", 1, "lz4kds"),
        ("quota_with_decode_error_stops", "32768 4096 0 decode_failures 1 1", 1, "lz4kds"),
        ("quota_with_flatten_error_stops", "32768 4096 0 flatten_failures 1 1", 1, "lz4kds"),
        ("quota_with_proof_error_stops", "32768 4096 0 delta_proof_failures 1 1", 1, "lz4kds"),
        ("quota_with_io_stat_error_stops", "32768 4096 0 io_write_failure 1 1", 1, "lz4kd"),
    ):
        f = case(name, codec)
        (f.root / "mock-result").write_text(result+"\n")
        f.run("", rc=rc, daemon=True)
        assert (f.root / "submissions").read_text().strip() == "submitted"
        assert (f.state / "crystal-writeback.blocked").exists() == bool(rc)
        if not rc:
            assert "quota_stop=yes" in (f.state / "crystal-writeback.log").read_text()
            assert f.daily() == "20261124 4096"
            (f.root / "proc/uptime").write_text("10500.00 20000.00\n")
            f.run("", daemon=True)
            assert (f.root / "submissions").read_text().splitlines() == ["submitted"]
            assert f.daily() == "20261124 4096"
        else:
            assert "DONE" not in (f.state / "crystal-writeback.log").read_text()
            f.run("cw_load_config", rc=1)
            if "gc_read_error" in name:
                assert "quota_stop=yes" not in (f.state / "crystal-writeback.log").read_text()
                assert f.daily() == "20261124 4096"
                assert (f.node / "writeback_limit").read_text().strip() == "67232"
                fields = (f.state / "crystal-writeback.pending").read_text().split()
                assert fields[0] == "v2" and fields[6] == "restored" and fields[-1] == "0"

    f = case("screen_on_cancels_continuous_grace")
    (f.root / "grace_rc").write_text("1\n")
    f.run("", daemon=True)
    assert not (f.root / "submissions").exists()
    assert (f.node / "writeback_limit").read_text().strip() == "100000"
    assert "no writeback submitted" in (f.state / "crystal-writeback.log").read_text()

    f = case("clock_backwards_no_refill")
    (f.state / "native-writeback.daily").write_text("20261125 500000\n")
    f.run("cw_load_config && cw_observe_physical", rc=1)
    assert f.daily() == "20261125 500000"

    report = {"passed": True, "checks": len(results), "cases": results,
              "kernel_io_mocked": True, "event_timing_mocked": True,
              "android_runtime_verified": False, "device_used": False,
              "source_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in HERE.iterdir()
                                if p.suffix in (".sh", ".c", ".conf")}}
    (args.output / "host-checks.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({"passed": True, "checks": len(results)}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bash", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    main(parser.parse_args())
