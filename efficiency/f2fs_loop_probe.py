"""Validate F2FS on a newly allocated private loop image, then remove it."""
import argparse
import hashlib
import json
import pathlib
import re
import shlex
import subprocess
import time


def test(args):
    def adb(*argv, timeout=90):
        p = subprocess.run(["adb", "-s", args.serial, *argv], capture_output=True, timeout=timeout)
        if p.returncode:
            raise RuntimeError((p.stdout + p.stderr).decode("utf-8", "replace")[-4000:])
        return p.stdout.decode("utf-8", "replace").replace("\r", "").strip()

    def root(code):
        return adb("shell", "su -c " + shlex.quote(code))

    if root("getprop ro.product.model") != "PLQ110":
        raise RuntimeError("This test baseline is for PLQ110")
    if root("getprop sys.boot_completed") != "1":
        raise RuntimeError("Wait for boot completion before filesystem testing")
    stage = "/data/local/tmp/ace6-f2fs-probe-" + str(time.time_ns())
    adb("shell", "mkdir -m 700 " + stage)
    payload = args.output.parent / "f2fs-probe-payload.bin"
    payload.parent.mkdir(parents=True, exist_ok=True)
    data = (b"Ace6 F2FS compression and readback probe.\n" * 128)
    data = (data * (4194304 // len(data) + 1))[:4194304]
    payload.write_bytes(data)
    expected = hashlib.sha256(data).hexdigest()
    for path, name in [(payload, "payload.bin"), (args.binary.resolve(), "probe")]:
        adb("push", str(path), stage + "/" + name)
        if root("sha256sum " + stage + "/" + name).split()[0] != hashlib.sha256(path.read_bytes()).hexdigest():
            raise RuntimeError("Staged test input differs from its host file")

    script = r'''set -eu
cd STAGE
chmod 700 probe
loop=''
mounted=0
held=0
cleanup() {
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$mounted" = 1 ]; then
        if umount "$PWD/mnt"; then mounted=0; else status=1; fi
    fi
    if [ "$held" = 1 ]; then exec 8<&-; held=0; fi
    if [ -n "$loop" ] && [ "$mounted" = 0 ]; then
        if [ ! -e "/sys/block/${loop##*/}/loop/backing_file" ]; then
            loop=''
        elif [ "$(cat "/sys/block/${loop##*/}/loop/backing_file")" = "$PWD/backing.img" ]; then
            if losetup -d "$loop"; then loop=''; else status=1; fi
        else
            echo 'Loop ownership changed; preserve its backing file' >&2
            status=1
        fi
    fi
    if [ -z "$loop" ] && [ "$mounted" = 0 ]; then
        rm -f "$PWD/backing.img"
        rmdir "$PWD/mnt" 2>/dev/null || true
        echo '===CLEANUP==='
        echo complete
    else
        echo '===CLEANUP==='
        echo incomplete
    fi
    exit "$status"
}
trap cleanup EXIT HUP INT TERM
mkdir mnt
fallocate -l 536870912 backing.img
chmod 600 backing.img
# Loop workers use the kernel domain. KSU's existing file type grants its I/O;
# parent directory mode 0700 and file mode 0600 preserve private access.
chcon u:object_r:ksu_file:s0 backing.img
# Format only the freshly created regular image file, before attaching a loop.
[ -f "$PWD/backing.img" ] && [ ! -L "$PWD/backing.img" ]
make_f2fs -t 0 -O extra_attr,flexible_inline_xattr,compression -l ace6_probe "$PWD/backing.img" > mkfs.log 2>&1 || {
    cat mkfs.log >&2; exit 1;
}
loop=$(losetup -f)
name=${loop##*/}
case "$name" in loop*) ;; *) echo bad-loop >&2; exit 1 ;; esac
number=${name#loop}
case "$number" in ''|*[!0-9]*) echo bad-loop-number >&2; exit 1 ;; esac
[ -b "$loop" ]
losetup "$loop" "$PWD/backing.img"
[ "$(cat "/sys/block/$name/loop/backing_file")" = "$PWD/backing.img" ]
# Android mount may enable loop autoclear. Hold a reference across the remount.
exec 8<"$loop"
held=1
options=background_gc=on,atgc,gc_merge,compress_algorithm=lz4,compress_mode=fs
mount -t f2fs -o "$options" "$loop" "$PWD/mnt"
mounted=1
echo '===MOUNT==='
awk -v target="$PWD/mnt" '$2 == target {print}' /proc/mounts
echo '===FEATURES==='
cat "/sys/fs/f2fs/$name/features"
echo '===ATGC==='
cat "/sys/fs/f2fs/$name/atgc_enabled"
device=$(cat "/sys/block/$name/dev")
thread="f2fs_gc-$device"
pid=''
for p in /proc/[0-9]*; do
    read -r comm < "$p/comm" 2>/dev/null || continue
    if [ "$comm" = "$thread" ]; then pid=${p##*/}; break; fi
done
[ -n "$pid" ]
echo '===GC==='
./probe scheduler "$pid"
touch mnt/payload.bin
chattr +c mnt/payload.bin
dd if=payload.bin of=mnt/payload.bin bs=1048576 conv=fsync 2> dd.log
echo '===COMPRESSED==='
./probe compression mnt/payload.bin
umount "$PWD/mnt"
mounted=0
mount -t f2fs -o "$options" "$loop" "$PWD/mnt"
mounted=1
echo '===READBACK==='
sha256sum mnt/payload.bin
echo '===COMPRESSED_READBACK==='
./probe compression mnt/payload.bin
'''.replace("cd STAGE", "cd " + stage)
    result = {"label": args.label, "serial": args.serial, "stage": stage,
              "payload_sha256": expected, "existing_volumes_formatted": False,
              "existing_mount_options_modified": False, "passed": False}
    process = subprocess.run(["adb", "-s", args.serial, "shell", "su -c " + shlex.quote(script)],
                             capture_output=True, timeout=120)
    stdout = process.stdout.decode("utf-8", "replace").replace("\r", "")
    stderr = process.stderr.decode("utf-8", "replace").replace("\r", "")
    args.output.with_suffix(".log").write_text(stdout + stderr, encoding="utf-8")
    sections = {name: value.strip() for name, value in
                re.findall(r"^===([A-Z_]+)===\n(.*?)(?=^===|\Z)", stdout, re.M | re.S)}
    result["exit_code"] = process.returncode
    result["cleanup"] = sections.get("CLEANUP", "not observed")
    try:
        result["mount"] = sections["MOUNT"]
        result["features"] = sections["FEATURES"]
        result["atgc_enabled"] = sections["ATGC"]
        result["gc_scheduler"] = json.loads(sections["GC"])
        result["compression"] = json.loads(sections["COMPRESSED"])
        result["compression_after_remount"] = json.loads(sections["COMPRESSED_READBACK"])
        result["readback_sha256"] = sections["READBACK"].split()[0]
        result["passed"] = (process.returncode == 0 and result["cleanup"] == "complete"
                            and result["readback_sha256"] == expected
                            and all(flag in result["mount"] for flag in [",atgc", ",gc_merge"])
                            and result["compression_after_remount"]["compression_counter"] > 0)
    except (KeyError, ValueError) as error:
        result["error"] = str(error)
    result["stderr"] = stderr[-2000:]
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return result["passed"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--label", required=True)
    raise SystemExit(0 if test(parser.parse_args()) else 1)
