"""Private ZRAM writeback/flush/cold-read test; never write zram0."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import uuid

SCHEMA = "zram-probe-four-way-v1"
PAIRS_SCHEMA = "sddc-pairs-2k-v1"
INPUT_BYTES = 32 * 1024 * 1024
BACKING_BYTES = 128 * 1024 * 1024
SEEDS = [0x1234ABCD, 0x78EF019B]


def named_stats(text):
    result = {}
    for line in text.splitlines():
        match = re.fullmatch(r"([A-Za-z0-9_]+):\s*(-?\d+)", line.strip())
        if match:
            if match[1] in result:
                raise RuntimeError("Duplicate statistics field: " + match[1])
            result[match[1]] = int(match[2])
    return result


def preflight(root, allowed_notes, backend, codec, expected_main_codec, expected_main_sddc):
    """Read-only: even reading hot_add is forbidden before this returns."""
    if root("getprop ro.product.model") != "PLQ110":
        raise RuntimeError("This test applies only to PLQ110")
    notes = root("sha256sum /sys/kernel/notes").split()[0]
    if notes not in allowed_notes:
        raise RuntimeError("Kernel notes are not in the explicit test allowlist")
    zms = root("test -r /sys/block/zram0/zms_stat && echo yes || echo no") == "yes"
    marker = root("test -r /sys/block/zram0/hybridswap_crystal_stat && echo yes || echo no") == "yes"
    if backend == "crystal" and not (zms and marker):
        raise RuntimeError("Crystal/ZMS markers missing; no device writes attempted")
    if backend == "native" and (zms or marker):
        raise RuntimeError("Native control requires the native ZRAM provider")
    algorithm = root("cat /sys/block/zram0/comp_algorithm")
    if f"[{expected_main_codec}]" not in algorithm:
        raise RuntimeError("Main codec differs from the explicit protected baseline")
    if codec not in re.findall(r"[a-z0-9-]+", algorithm):
        raise RuntimeError("Requested private codec was not compiled into this provider")
    sddc = None
    if backend == "crystal":
        sddc = named_stats(root("cat /sys/block/zram0/sddc_stat"))
        if sddc.get("enabled") != expected_main_sddc:
            raise RuntimeError("Main SDDC state differs from the explicit protected baseline")
    if root("test -r /sys/class/zram-control/hot_add -a -w /sys/class/zram-control/hot_remove && echo yes || echo no") != "yes":
        raise RuntimeError("Isolated device creation/removal is unavailable")
    available = int(root("awk '$1==\"MemAvailable:\" {print $2}' /proc/meminfo"))
    if available < 1024 * 1024:
        raise RuntimeError("Preserve at least 1 GiB available RAM")
    free_kb = int(root("df -Pk /data/adb | awk 'END {print $4}'"))
    if free_kb < 1152 * 1024:
        raise RuntimeError("Need 128 MiB backing plus 1 GiB free storage")
    return {"kernel_notes_sha256": notes, "boot_id": root("cat /proc/sys/kernel/random/boot_id"),
            "backend": backend, "private_codec": codec, "protected_main_codec": expected_main_codec,
            "main_sddc_state": sddc, "available_ram_kb": available, "free_storage_kb": free_kb}


def loop_stats(text):
    values = list(map(int, text.split()))
    if len(values) < 11:
        raise RuntimeError("Incomplete loop block statistics")
    return {"raw": values, "read_ios": values[0], "read_sectors": values[2],
            "write_ios": values[4], "write_sectors": values[6],
            "sector_bytes": 512, "flush_ios": values[15] if len(values) >= 17 else None}


class Target:
    def __init__(self, serial):
        self.serial = serial

    def adb(self, *argv, timeout=60):
        p = subprocess.run(["adb", "-s", self.serial, *argv], capture_output=True, timeout=timeout)
        if p.returncode:
            raise RuntimeError((p.stdout + p.stderr).decode("utf-8", "replace")[-4000:])
        return p.stdout.decode("utf-8", "replace").replace("\r", "").strip()

    def root(self, code, timeout=60):
        return self.adb("shell", "su -c " + shlex.quote(code), timeout=timeout)


class PrivateRound:
    def __init__(self, root, stage, number, backend, codec, expected_sddc):
        self.root, self.stage, self.backend = root, stage, backend
        self.codec, self.expected_sddc = codec, expected_sddc
        self.backing = f"{stage}/round-{number}.img"
        self.index = self.loop = self.inode = self.identity = None
        self.removed = False

    def assert_owned(self, require_backing=False):
        if self.index is None or not 0 < self.index < 1024:
            raise RuntimeError("No safe private ZRAM ownership")
        if self.root(f"stat -Lc %i {self.sysfs}") != self.inode or self.root(f"cat {self.sysfs}/dev") != self.identity:
            raise RuntimeError("Private ZRAM identity changed externally")
        swaps = self.root("for d in $(awk 'NR>1 {print $1}' /proc/swaps); do stat -Lc '%t:%T' \"$d\"; done; true")
        active = {tuple(int(x, 16) for x in line.split(":")) for line in swaps.splitlines() if line}
        if tuple(map(int, self.identity.split(":"))) in active:
            raise RuntimeError("Private device became active swap; preserve it")
        current = self.root(f"cat {self.sysfs}/backing_dev")
        if current not in ("", "none"):
            if self.loop is None or current != self.loop:
                raise RuntimeError("Foreign backing device; preserve it")
            if self.root(f"cat /sys/block/{self.loop_name}/loop/backing_file") != self.backing:
                raise RuntimeError("Private loop ownership changed")
        elif require_backing:
            raise RuntimeError("Private backing disappeared")
        if int(self.root(f"cat {self.sysfs}/disksize")) not in (0, INPUT_BYTES):
            raise RuntimeError("Private capacity changed externally")

    def snapshot(self):
        self.assert_owned(require_backing=True)
        snap = {"mm_stat": list(map(int, self.root(f"cat {self.sysfs}/mm_stat").split())),
                "bd_stat": list(map(int, self.root(f"cat {self.sysfs}/bd_stat").split())),
                "io_stat": list(map(int, self.root(f"cat {self.sysfs}/io_stat").split())),
                "loop": loop_stats(self.root(f"cat /sys/block/{self.loop_name}/stat")),
                "writeback_limit_logical_slots": int(self.root(f"cat {self.sysfs}/writeback_limit"))}
        if self.backend == "crystal":
            snap["zms"] = named_stats(self.root(f"cat {self.sysfs}/zms_stat"))
            snap["sddc"] = named_stats(self.root(f"cat {self.sysfs}/sddc_stat"))
            if snap["zms"].get("enabled") != 1 or snap["zms"].get("ret") != 0:
                raise RuntimeError("ZMS is not active on the private device")
            if snap["sddc"].get("enabled") != self.expected_sddc:
                raise RuntimeError("Unexpected private SDDC runtime state")
        return snap

    def wait_node(self, device):
        self.root(f"""attempt=0
while [ ! -b {device} ] && [ "$attempt" -lt 30 ]; do
 /system/bin/sleep 0.1
 attempt=$((attempt + 1))
done
test -b {device}
""")

    def prepare(self):
        # Android's losetup rejects paths reaching the kernel's 64-byte
        # loop_file_name field, even when the underlying file is valid.
        if len(self.backing.encode("utf-8")) >= 64:
            raise RuntimeError("Choose a private backing path shorter than 64 bytes")
        self.index = int(self.root("cat /sys/class/zram-control/hot_add"))
        if not 0 < self.index < 1024:
            raise RuntimeError("hot_add returned unsafe index; never reset zram0")
        self.sysfs = f"/sys/block/zram{self.index}"
        self.device = f"/dev/block/zram{self.index}"
        self.inode = self.root(f"stat -Lc %i {self.sysfs}")
        self.identity = self.root(f"cat {self.sysfs}/dev")
        self.assert_owned()
        if int(self.root(f"cat {self.sysfs}/disksize")) != 0:
            raise RuntimeError("New device was initialized externally")
        self.wait_node(self.device)
        algorithms = self.root(f"cat {self.sysfs}/comp_algorithm")
        if f"[{self.codec}]" not in algorithms:
            # Only an unused index > 0 reaches this write. A locked same-value
            # Crystal selection is skipped; locked other-codec builds fail.
            self.root(f"printf '%s\\n' {self.codec} > {self.sysfs}/comp_algorithm")
        if f"[{self.codec}]" not in self.root(f"cat {self.sysfs}/comp_algorithm"):
            raise RuntimeError("Requested private codec did not become active")
        self.root(f"""set -e
umask 077
set -C
: > {self.backing}
fallocate -l {BACKING_BYTES} {self.backing}
chown 0:0 {self.backing}
chmod 600 {self.backing}
chcon u:object_r:ksu_file:s0 {self.backing}
test "$(stat -c %s {self.backing})" = {BACKING_BYTES}
""")
        self.backing_metadata = self.root(f"stat -c '%u:%g %a %s' {self.backing}")
        self.backing_label = self.root(f"ls -Zd {self.backing}")
        if self.backing_metadata != f"0:0 600 {BACKING_BYTES}" or "u:object_r:ksu_file:s0" not in self.backing_label:
            raise RuntimeError("Private backing permissions or SELinux label differ")
        self.loop = self.root("/system/bin/losetup -f")
        if not re.fullmatch(r"/dev/(?:block/)?loop\d+", self.loop):
            raise RuntimeError("Unexpected loop path")
        self.loop_name = self.loop.rsplit("/", 1)[-1]
        self.loop = "/dev/block/" + self.loop_name
        self.wait_node(self.loop)
        self.root(f"/system/bin/losetup {self.loop} {self.backing}")
        if self.root(f"cat /sys/block/{self.loop_name}/loop/backing_file") != self.backing:
            raise RuntimeError("Loop did not bind the owned file")
        self.assert_owned()
        self.root(f"""set -e
printf '%s\\n' {self.loop} > {self.sysfs}/backing_dev
printf '%s\\n' {INPUT_BYTES} > {self.sysfs}/disksize
printf '%s\\n' 8192 > {self.sysfs}/writeback_limit
printf '%s\\n' 1 > {self.sysfs}/writeback_limit_enable
""")

    def cleanup(self):
        if self.index is None:
            return
        self.assert_owned()
        self.root(f"set -e; printf '1\\n' > {self.sysfs}/reset; printf '%s\\n' {self.index} > /sys/class/zram-control/hot_remove")
        self.removed = True
        if self.loop:
            owner = self.root(f"if [ -r /sys/block/{self.loop_name}/loop/backing_file ]; then cat /sys/block/{self.loop_name}/loop/backing_file; else echo __detached__; fi")
            if owner == self.backing:
                self.root(f"/system/bin/losetup -d {self.loop}")
                detached = self.root(f"if [ -r /sys/block/{self.loop_name}/loop/backing_file ]; then cat /sys/block/{self.loop_name}/loop/backing_file; else echo __detached__; fi")
                if detached != "__detached__":
                    raise RuntimeError("Loop detach is pending or owner changed; preserve the file")
            elif owner != "__detached__":
                raise RuntimeError("Loop owner changed; preserve its data")
        self.root(f"set -e; test ! -L {self.backing}; rm -f {self.backing}")


def measure_round(private, program, seed, entry, pattern="mixed", require_conversions=False):
    entry.update(seed=seed, logical_input_bytes=INPUT_BYTES, passed=False,
                 device=private.device, initial=private.snapshot(),
                 backing_metadata=private.backing_metadata, backing_label=private.backing_label)
    entry["fill"] = json.loads(private.root(f"{program} {private.device} write {seed} {pattern}", timeout=90))
    entry["before_writeback"] = private.snapshot()
    private.assert_owned(require_backing=True)
    private.root(f"set -e; printf 'all\\n' > {private.sysfs}/idle; printf 'idle\\n' > {private.sysfs}/writeback", timeout=90)
    entry["after_writeback_before_flush"] = private.snapshot()
    # The fill-time fdatasync precedes writeback and cannot prove ZMS is on disk.
    # No pattern page is read until this second, explicit flush succeeds.
    entry["post_writeback_flush"] = json.loads(private.root(
        f"{program} {private.device} flush {seed} {pattern}", timeout=90))
    entry["after_flush_before_first_read"] = private.snapshot()
    entry["first_full_readback"] = json.loads(private.root(
        f"{program} {private.device} verify {seed} {pattern}", timeout=90))
    entry["after_first_read"] = private.snapshot()
    before = entry["before_writeback"]
    flushed = entry["after_flush_before_first_read"]
    after = entry["after_first_read"]
    if private.backend == "crystal":
        writes = flushed["zms"]["physical_write_pages"] - before["zms"]["physical_write_pages"]
        reads = after["zms"]["physical_read_pages"] - flushed["zms"]["physical_read_pages"]
        clean = flushed["zms"]["dirty_pages"] == 0
        failed = after["zms"]["physical_write_failed_pages"] or after["zms"]["physical_read_failed_pages"]
        packed = flushed["zms"]["packed_bytes"]
        stored = flushed["zms"]["stored_bytes"]
    else:
        writes = flushed["bd_stat"][2] - before["bd_stat"][2]
        reads = after["bd_stat"][1] - flushed["bd_stat"][1]
        clean, failed = True, False
        packed, stored = None, None
    loop_write_sectors = after["loop"]["write_sectors"] - entry["initial"]["loop"]["write_sectors"]
    loop_read_sectors = after["loop"]["read_sectors"] - flushed["loop"]["read_sectors"]
    checks = {
        "known_pattern_complete": entry["first_full_readback"].get("passed") is True
            and entry["first_full_readback"].get("bytes") == INPUT_BYTES
            and entry["first_full_readback"].get("pattern_schema") == (SCHEMA if pattern == "mixed" else PAIRS_SCHEMA)
            and entry["fill"]["fnv1a64"] == entry["first_full_readback"]["fnv1a64"],
        "explicit_post_writeback_flush": entry["post_writeback_flush"].get("passed") is True,
        "all_zms_dirty_pages_flushed": clean,
        "backend_writes_increased": writes > 0,
        "first_read_reached_backend": reads > 0 and loop_read_sectors > 0,
        "private_loop_writes_increased": loop_write_sectors > 0,
        "no_backend_failed_io": not failed and after["io_stat"][:2] == [0, 0],
    }
    if private.backend == "crystal":
        sddc = after["sddc"]
        samples = [entry[key]["sddc"] for key in ("initial", "before_writeback",
                   "after_writeback_before_flush", "after_flush_before_first_read", "after_first_read")]
        entry["sddc_conversions_observed"] = any(s["aliases"] + s["deltas"] +
            s["wb_aliases"] + s["wb_deltas"] > 0 for s in samples)
        entry["sddc_packed_writeback_observed"] = any(s["wb_aliases"] + s["wb_deltas"] > 0 for s in samples)
        checks["no_sddc_decode_failure"] = sddc["decode_failures"] == 0
        if require_conversions:
            checks["sddc_conversions_observed"] = entry["sddc_conversions_observed"]
    entry["measurements"] = {
        "logical_input_bytes": INPUT_BYTES,
        "compressed_payload_bytes_before_writeback": before["mm_stat"][1],
        "stored_payload_bytes_after_flush": stored, "packed_bytes_after_flush": packed,
        "backend_physical_write_pages_delta": writes,
        "backend_physical_read_pages_delta": reads,
        "private_loop_write_sectors_including_internal_io": loop_write_sectors,
        "private_loop_read_sectors_during_first_read": loop_read_sectors,
        "private_loop_write_bytes_including_internal_io": loop_write_sectors * 512,
        "private_loop_read_bytes_during_first_read": loop_read_sectors * 512,
        "private_loop_flush_ios_including_internal_io": (after["loop"]["flush_ios"] -
            entry["initial"]["loop"]["flush_ios"]) if after["loop"]["flush_ios"] is not None else None,
        "scope": "Backend/loop logical I/O including its flush/GC; not UFS NAND physical writes",
    }
    entry.update(checks=checks, passed=all(checks.values()))
    return entry


def run(args):
    result = {"passed": False, "device_mutations_started": False, "devices_created": 0,
              "runtime_test_performed": False, "runtime_test_attempted": False,
              "backend": args.backend, "codec": args.codec,
              "pattern_schema": SCHEMA if args.pattern == "mixed" else PAIRS_SCHEMA,
              "rounds": [], "ufs_nand_writes_measured": False,
              "power_savings_verified": False, "sddc_lifecycle_test_performed": False}
    target = Target(args.serial)
    root = target.root
    stage = None
    cleanup_ok = True

    def main_config():
        code = "cat /sys/block/zram0/disksize /sys/block/zram0/comp_algorithm /sys/block/zram0/backing_dev"
        if args.backend == "crystal":
            code += "; cat /sys/block/zram0/sddc_stat | head -1"
        return root(code)

    try:
        result["preflight"] = preflight(root, args.expected_notes, args.backend, args.codec,
                                         args.expected_main_codec, args.expected_main_sddc)
        if args.check_only:
            result.update(passed=True, phase="read_only_preflight")
            return result
        blob = args.binary.read_bytes()
        if blob[:6] != b"\x7fELF\x02\x01" or int.from_bytes(blob[18:20], "little") != 183:
            raise RuntimeError("Provide the compiled Android ARM64 helper")
        binary_sha = hashlib.sha256(blob).hexdigest()
        before_main = main_config()
        result["main_zram_configuration_before"] = before_main
        result["device_mutations_started"] = True
        stage = "/data/adb/ace6ct-" + uuid.uuid4().hex[:20]
        root(f"umask 077; mkdir -m 700 {stage}")
        program = stage + "/block-probe"
        target.adb("push", str(args.binary), program)
        if root(f"sha256sum {program}").split()[0] != binary_sha:
            raise RuntimeError("Staged helper checksum mismatch")
        root(f"set -e; chmod 700 {program}; chcon u:object_r:ksu_file:s0 {program}")
        result["helper_sha256"] = binary_sha
        for number, seed in enumerate(SEEDS):
            private = PrivateRound(root, stage, number, args.backend, args.codec, args.expected_sddc)
            entry = {"seed": seed, "passed": False}
            result["rounds"].append(entry)
            try:
                private.prepare()
                result["runtime_test_attempted"] = True
                measure_round(private, program, seed, entry, args.pattern, args.require_sddc_conversions)
                result["runtime_test_performed"] = True
                if not entry["passed"]:
                    raise RuntimeError("Private writeback/flush/cold-read criteria failed")
            except Exception as error:
                entry["error"] = str(error)
                raise
            finally:
                if private.index is not None and 0 < private.index < 1024:
                    result["devices_created"] += 1
                    entry["device"] = getattr(private, "device", None)
                try:
                    private.cleanup()
                    entry["cleanup"] = "complete"
                except Exception as error:
                    cleanup_ok = False
                    entry["cleanup"] = str(error)
                    raise
        result["main_zram_configuration_after"] = main_config()
        result["main_zram_configuration_unchanged"] = result["main_zram_configuration_after"] == before_main
        result["same_boot_after_test"] = root("cat /proc/sys/kernel/random/boot_id") == result["preflight"]["boot_id"]
        result["passed"] = (len(result["rounds"]) == len(SEEDS)
                            and all(x["passed"] for x in result["rounds"])
                            and result["main_zram_configuration_unchanged"]
                            and result["same_boot_after_test"])
    except Exception as error:
        result["error"] = str(error)
    finally:
        if "main_zram_configuration_before" in result and "main_zram_configuration_after" not in result:
            try:
                result["main_zram_configuration_after"] = main_config()
                result["main_zram_configuration_unchanged"] = (result["main_zram_configuration_after"] ==
                    result["main_zram_configuration_before"])
                result["same_boot_after_test"] = (root("cat /proc/sys/kernel/random/boot_id") ==
                    result["preflight"]["boot_id"])
            except Exception as error:
                result["protected_baseline_readback_error"] = str(error)
        if stage and cleanup_ok:
            try:
                root(f"set -e; test ! -L {stage}; rm -f {stage}/block-probe; rmdir {stage}")
                result["staging_cleanup"] = "complete"
            except Exception as error:
                result["staging_cleanup"] = str(error)
                result["passed"] = False
        elif stage:
            result["preserved_staging_directory"] = stage
            result["passed"] = False
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--backend", choices=["crystal", "native"], required=True)
    parser.add_argument("--codec", choices=["lz4kd", "lz4kds"], default="lz4kd")
    parser.add_argument("--pattern", choices=["mixed", "sddc-pairs"], default="mixed")
    parser.add_argument("--require-sddc-conversions", action="store_true")
    parser.add_argument("--expected-main-codec", choices=["lz4kd", "lz4kds"], default="lz4kd")
    parser.add_argument("--expected-sddc", type=int, choices=[0, 1], default=0,
                        help="Required SDDC state on the new private device")
    parser.add_argument("--expected-main-sddc", type=int, choices=[0, 1], default=0)
    parser.add_argument("--expected-notes", action="append", required=True)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.check_only and not args.binary:
        parser.error("Provide --binary for a writeback test")
    if any(not re.fullmatch(r"[0-9a-f]{64}", value) for value in args.expected_notes):
        parser.error("Each notes entry must be a full lowercase SHA256")
    if args.backend == "native" and (args.codec != "lz4kd" or args.expected_sddc or args.expected_main_sddc):
        parser.error("Native control is pure LZ4KD with SDDC absent")
    if args.codec == "lz4kds" and not args.expected_sddc:
        parser.error("LZ4KDS requires explicit --expected-sddc 1")
    if args.require_sddc_conversions and (args.backend != "crystal" or not args.expected_sddc or args.pattern != "sddc-pairs"):
        parser.error("Conversion checks need Crystal, SDDC=1 and --pattern sddc-pairs")
    result = run(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
