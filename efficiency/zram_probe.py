"""Test an isolated, newly-created ZRAM device. Never reset or write zram0."""
import argparse
import json
import pathlib
import re
import shlex
import subprocess
import time


def test(args):
    def adb(*argv, timeout=60):
        p = subprocess.run(["adb", "-s", args.serial, *argv], capture_output=True, timeout=timeout)
        if p.returncode: raise RuntimeError(p.stderr.decode("utf-8", "replace"))
        return p.stdout.decode("utf-8", "replace").replace("\r", "").strip()

    def root(code, timeout=60):
        return adb("shell", "su -c " + shlex.quote(code), timeout=timeout)

    if root("getprop ro.product.model") != "PLQ110":
        raise RuntimeError("This test manifest is for PLQ110 only")
    adb("push", str(args.binary.resolve()), "/data/local/tmp/ace6-zram-readback")
    root("chmod 755 /data/local/tmp/ace6-zram-readback")
    root("test -b /dev/block/zram0")
    index = int(root("cat /sys/class/zram-control/hot_add"))
    if not 0 < index < 1024: raise RuntimeError("Kernel returned an invalid test ZRAM index")
    device = f"/dev/block/zram{index}"
    sysfs = f"/sys/block/zram{index}"
    stage = "/data/local/tmp/ace6-zram-probe-" + str(time.time_ns())
    loop = None
    result = {"label": args.label, "serial": args.serial, "device": device,
              "mode": "writeback" if args.writeback else "memory", "rounds": [], "passed": False}

    def assert_unused():
        active = root("cat /proc/swaps")
        if any(line.split()[0] in (device, f"/dev/zram{index}")
               for line in active.splitlines()[1:] if line.split()):
            raise RuntimeError("Test device was activated externally; refusing block I/O or reset")

    try:
        assert_unused()
        for _ in range(20):
            if root(f"[ -b {device} ] && echo ready || true") == "ready": break
            time.sleep(0.1)
        else: raise RuntimeError("ueventd did not create the test block node")
        root(f"printf 'lz4kd\\n' > {sysfs}/comp_algorithm")
        if "[lz4kd]" not in root(f"cat {sysfs}/comp_algorithm"):
            raise RuntimeError("Pure lz4kd was not selected")
        if args.writeback:
            root(f"test -f {sysfs}/backing_dev")
            root(f"mkdir -m 700 {stage}; fallocate -l 67108864 {stage}/backing.img; chmod 600 {stage}/backing.img")
            root(f"chcon u:object_r:ksu_file:s0 {stage}/backing.img")
            loop = root("losetup -f")
            if not re.fullmatch(r"/dev/(?:block/)?loop\d+", loop):
                raise RuntimeError("Unexpected loop device path: " + loop)
            root(f"losetup {loop} {stage}/backing.img")
            root(f"printf '%s\\n' {loop} > {sysfs}/backing_dev")
        root(f"printf '67108864\\n' > {sysfs}/disksize")
        result["initial_mm_stat"] = root(f"cat {sysfs}/mm_stat")
        if args.writeback: result["initial_bd_stat"] = root(f"cat {sysfs}/bd_stat")
        for seed in [0x1234ABCD, 0x78EF019B]:
            assert_unused()
            commands = []
            for mode in ["write", "verify"]:
                commands.append(json.loads(root(f"/data/local/tmp/ace6-zram-readback {device} {mode} 8192 {seed}")))
            entry = {"seed": seed, "direct_io": commands}
            if args.writeback:
                entry["before_bd_stat"] = root(f"cat {sysfs}/bd_stat")
                root(f"printf 'all\\n' > {sysfs}/idle; printf 'idle\\n' > {sysfs}/writeback", timeout=90)
                entry["after_writeback_bd_stat"] = root(f"cat {sysfs}/bd_stat")
                if int(entry["after_writeback_bd_stat"].split()[2]) <= int(entry["before_bd_stat"].split()[2]):
                    raise RuntimeError("Writeback counter did not increase")
                entry["readback"] = json.loads(root(f"/data/local/tmp/ace6-zram-readback {device} verify 8192 {seed}"))
                entry["after_readback_bd_stat"] = root(f"cat {sysfs}/bd_stat")
                if int(entry["after_readback_bd_stat"].split()[1]) <= int(entry["after_writeback_bd_stat"].split()[1]):
                    raise RuntimeError("Readback did not reach the backing device")
            result["rounds"].append(entry)
        result["passed"] = True
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        # An externally activated device is left untouched, including its loop.
        try:
            assert_unused()
            root(f"printf '1\\n' > {sysfs}/reset; printf '{index}\\n' > /sys/class/zram-control/hot_remove")
            if loop:
                name = loop.rsplit("/", 1)[-1]
                backing_node = f"/sys/block/{name}/loop/backing_file"
                owner = root(f"if [ -e {backing_node} ]; then cat {backing_node}; else echo __detached__; fi")
                if owner == stage + "/backing.img":
                    root(f"losetup -d {loop}")
                elif owner != "__detached__":
                    raise RuntimeError("Test loop ownership changed; preserving its backing file")
            root(f"if [ -d {stage} ]; then rm -f {stage}/backing.img; rmdir {stage}; fi")
            result["cleanup"] = "complete"
        except Exception as error:
            result["cleanup"] = str(error)
            result["passed"] = False
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(json.dumps(result, indent=2))
        if result["cleanup"] != "complete" and "error" not in result:
            raise RuntimeError("Probe cleanup did not finish")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--writeback", action="store_true")
    test(parser.parse_args())
