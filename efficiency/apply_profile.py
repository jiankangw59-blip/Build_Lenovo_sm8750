"""Apply a pinned, reviewed series. A failed or incomplete port stops the build."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

from integrate_rekernel import integrate
from integrate_crystal import (CRYSTAL_PROFILES, PROFILE_CHOICES, crystal_configuration,
                               crystal_configuration_errors, integrate as integrate_crystal,
                               integrity_installed_hashes, kunit_boot_errors, stage2_profiles, unexpected_sddc_options)
from idle_refinement import (PROFILE as IDLE_PROFILE, check_source_hashes as check_idle_hashes,
                             refinement_policy as idle_policy, source_report as idle_source_report)

HERE = pathlib.Path(__file__).resolve().parent


def run(argv, cwd):
    subprocess.run(argv, cwd=cwd, check=True)


def apply(kernel, report, profile="baseline"):
    manifest = json.loads((HERE / "patch-series.json").read_text(encoding="utf-8"))
    makefile = (kernel / "Makefile").read_text(encoding="utf-8")
    numbers = [re.search(r"^" + key + r"\s*=\s*(\d+)", makefile, re.M).group(1)
               for key in ["VERSION", "PATCHLEVEL", "SUBLEVEL"]]
    version = ".".join(numbers)
    if version != manifest["base_kernel_version"]:
        raise RuntimeError("Efficiency patch series requires audited base " + manifest["base_kernel_version"])
    report.mkdir(parents=True, exist_ok=True)
    applied = []
    patches = list(manifest["patches"])
    idle = idle_policy() if profile == IDLE_PROFILE else None
    followup_names = stage2_profiles(profile)
    if followup_names:
        followups = json.loads((HERE / "stage2-series.json").read_text(encoding="utf-8"))
        patches.extend(followups[name] for name in followup_names)
    if idle and (len(patches) != 22 or [item["file"] for item in patches[-3:]] != [
            "patches/kcompressd-6.6.89.patch", "patches/le9uo-6.6.89.patch", idle["patch_file"]] or
            patches[-1]["sha256"] != idle["patch_sha256"]):
        raise RuntimeError("Idle refinement requires the exact final three-patch order")
    for item in patches:
        path = HERE / item["file"]
        if hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
            raise RuntimeError("Patch checksum mismatch: " + item["file"])
        touched = set(re.findall(r"^\+\+\+ b/(.+)$", path.read_text(encoding="utf-8"), re.M))
        if not touched:
            raise RuntimeError("Patch has no bounded destination paths: " + item["file"])
        targets = [(kernel / name).resolve() for name in touched]
        if not all(target.is_relative_to(kernel) for target in targets):
            raise RuntimeError("Patch destination escapes the kernel tree")
        before = {str(target.relative_to(kernel)): hashlib.sha256(target.read_bytes()).hexdigest()
                  if target.exists() else None for target in targets}
        if idle and item["file"] == idle["patch_file"]:
            check_idle_hashes(before, "before", idle)
            idle_source_report(kernel, "before", idle)
        # Apply relative to this source tree and verify actual byte changes,
        # independently of Git repository discovery in nested CI directories.
        command = ["patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i", str(path)]
        run([*command, "--dry-run"], kernel)
        run(command, kernel)
        after = {str(target.relative_to(kernel)): hashlib.sha256(target.read_bytes()).hexdigest()
                 for target in targets}
        if idle and item["file"] == idle["patch_file"]:
            check_idle_hashes(after, "after", idle)
            idle_source_report(kernel, "after", idle)
        if not any(before[name] != digest for name, digest in after.items()):
            raise RuntimeError("Patch returned success without changing source: " + item["file"])
        applied.append({**item, "before": before, "after": after})
    integrate(kernel, report / "rekernel-integration.json")
    if profile in CRYSTAL_PROFILES:
        integrate_crystal(kernel, report / "crystal-integration.json", profile)
    result = {"version": version, "profile": profile, "source": manifest["base_commit"], "applied": applied,
              "build_verified": False, "runtime_verified": False}
    if idle:
        result["cpuidle_refinement"] = idle
    (report / "efficiency-patches.json").write_text(json.dumps(result, indent=2), encoding="utf-8")


def configure(kernel, profile="baseline"):
    fragment = (HERE / "ace6-baseline.config").read_text(encoding="utf-8")
    # The compressor patch adds a choice; select it only after all other config edits.
    defconfig = kernel / "arch/arm64/configs/gki_defconfig"
    # A runtime mask update skips ordered WQs in 6.6. Set the native boot
    # parameter before pools are created, while preserving the OEM command line.
    cmdlines = re.findall(r'^CONFIG_CMDLINE="(.*)"$', defconfig.read_text(encoding="utf-8"), re.M)
    if not cmdlines:
        raise RuntimeError("Missing OEM default kernel command line")
    cmdline = re.sub(r"(?:^|\s)workqueue\.unbound_cpus=\S+", "", cmdlines[-1]).strip()
    cmdline += " workqueue.unbound_cpus=0-5"
    if profile == "crystal-lz4kds-test":
        for name in ["kunit.enable", "kunit.filter_glob"]:
            cmdline = re.sub(r"(?:^|\s)" + re.escape(name) + r"=\S+", "", cmdline).strip()
        cmdline += " kunit.enable=1 kunit.filter_glob=crystal-sddc-*"
    run(["scripts/config", "--file", str(defconfig), "--set-str", "CMDLINE",
         cmdline], kernel)
    for option in ["ZRAM", "ZSMALLOC"]:
        run(["scripts/config", "--file", str(defconfig), "--enable", option], kernel)
    for option in ["ZRAM_DEF_COMP_LZO", "ZRAM_DEF_COMP_LZORLE", "ZRAM_DEF_COMP_LZ4",
                   "ZRAM_DEF_COMP_LZ4HC", "ZRAM_DEF_COMP_LZ4K", "ZRAM_DEF_COMP_LZ4K_OPLUS",
                   "ZRAM_DEF_COMP_DEFLATE", "ZRAM_DEF_COMP_842", "ZRAM_DEF_COMP_ZSTD"]:
        run(["scripts/config", "--file", str(defconfig), "--disable", option], kernel)
    for line in fragment.splitlines():
        m = re.fullmatch(r"CONFIG_([A-Z0-9_]+)=(.*)", line)
        if m:
            option, value = m.groups()
            if value == "y": args = ["--enable", option]
            elif value == "m": args = ["--module", option]
            elif value.startswith('"'): args = ["--set-str", option, value[1:-1]]
            else: args = ["--set-val", option, value]
            run(["scripts/config", "--file", str(defconfig), *args], kernel)
        else:
            m = re.fullmatch(r"# CONFIG_([A-Z0-9_]+) is not set", line)
            if m: run(["scripts/config", "--file", str(defconfig), "--disable", m.group(1)], kernel)
    if "le9uo" in stage2_profiles(profile):
        # A soft 5% clean-cache floor; avoid upstream's two hard 15% floors.
        for name, value in [("ANON_MIN_RATIO", 0), ("CLEAN_LOW_RATIO", 5), ("CLEAN_MIN_RATIO", 0)]:
            run(["scripts/config", "--file", str(defconfig), "--set-val", name, str(value)], kernel)
    if "kcompressd" in stage2_profiles(profile):
        # Match the accepted native-ZRAM baseline; this is not a ZSWAP port.
        run(["scripts/config", "--file", str(defconfig), "--disable", "ZSWAP"], kernel)
    if profile in CRYSTAL_PROFILES:
        for name, value in crystal_configuration(profile).items():
            if value in ("y", "n"):
                selection = ["--enable" if value == "y" else "--disable", name]
            else:
                selection = ["--set-str", name, value.strip('"')]
            run(["scripts/config", "--file", str(defconfig), *selection], kernel)


def verify(kernel, report, profile="baseline"):
    idle = idle_source_report(kernel) if profile == IDLE_PROFILE else None
    actual = (kernel / "out/.config").read_text(encoding="utf-8").splitlines()
    required = [line for line in (HERE / "ace6-baseline.config").read_text(encoding="utf-8").splitlines()
                if line.startswith("CONFIG_")]
    required += ["CONFIG_ZRAM=y", "CONFIG_ZSMALLOC=y"]
    if profile in CRYSTAL_PROFILES:
        required = [line for line in required if not line.startswith("CONFIG_ZRAM")]
    missing = [line for line in required if line not in actual]
    if profile in CRYSTAL_PROFILES:
        missing.extend(crystal_configuration_errors(actual, profile))
        missing.extend(kunit_boot_errors(actual, profile))
    forbidden = ["CONFIG_UKSM=y", "CONFIG_IOSCHED_BFQ=y", "CONFIG_BORE_SCHEDULER=y",
                 "CONFIG_HZ_1000=y", "CONFIG_REKERNEL=m", "CONFIG_REKERNEL_LEGACY_NETLINK=y"]
    selected = [line for line in forbidden if line in actual]
    if "le9uo" in stage2_profiles(profile):
        for line in ["CONFIG_ANON_MIN_RATIO=0", "CONFIG_CLEAN_LOW_RATIO=5", "CONFIG_CLEAN_MIN_RATIO=0"]:
            if line not in actual:
                missing.append(line)
        vmscan = (kernel / "mm/vmscan.c").read_text(encoding="utf-8")
        if not re.search(r"^bool sysctl_workingset_protection\s+__read_mostly\s*=\s*true;", vmscan, re.M):
            missing.append("le9uo workingset_protection must default to 1")
    if "kcompressd" in stage2_profiles(profile):
        if "# CONFIG_ZSWAP is not set" not in actual:
            missing.append("Kcompressd 6.6 native route requires CONFIG_ZSWAP=n")
        page_io = (kernel / "mm/page_io.c").read_text(encoding="utf-8")
        for marker in ["set_user_nice(current, 19)", "if (cpu < 6)", "READ_ONCE(vm_kcompressd)"]:
            if marker not in page_io:
                missing.append("Kcompressd runtime policy: " + marker)
    if profile in CRYSTAL_PROFILES:
        if "# CONFIG_ZRAM is not set" not in actual:
            missing.append("Crystal must replace the native ZRAM provider")
        source = kernel / "drivers/crystalfrostwork/crystal_hybridswap"
        if not all((source / name).is_file() for name in ["zms.c", "zram_drv.c", "zcomp.c"]):
            missing.append("Crystal/ZMS source is missing")
        if CRYSTAL_PROFILES[profile]["sddc"]:
            for name, expected in integrity_installed_hashes(profile).items():
                path = kernel / name
                if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                    missing.append("Actual Crystal source lacks the reviewed overlay chain: " + name)
    cmdline = next((line for line in actual if line.startswith('CONFIG_CMDLINE="')), "")
    if "workqueue.unbound_cpus=0-5" not in cmdline:
        missing.append("Native early workqueue CPU mask boot parameter")
    if "CONFIG_CMDLINE_FORCE=y" in actual:
        selected.append("Kernel command line must extend the bootloader arguments")
    selected += unexpected_sddc_options(actual, profile)
    if not re.search(r"^#define\s+BBR_VERSION\s+3\b", (kernel / "net/ipv4/tcp_bbr.c").read_text(encoding="utf-8"), re.M):
        missing.append("BBRv3 implementation source")
    module_source = (kernel / "kernel/module/main.c").read_text(encoding="utf-8")
    if "custom_module_blacklist" in module_source:
        selected.append("custom_module_blacklist in module loader")
    if "-O3" in (kernel / "Makefile").read_text(encoding="utf-8"):
        # Check compiler flags rather than comments or compiler feature probes.
        if re.search(r"^KBUILD_CFLAGS\s*\+?=.*-O3", (kernel / "Makefile").read_text(encoding="utf-8"), re.M):
            selected.append("KBUILD_CFLAGS uses -O3")
    if "-O3" in (kernel / "lib/lz4kd/Makefile").read_text(encoding="utf-8"):
        selected.append("LZ4KD overrides the global -O2 policy")
    erofs = (kernel / "fs/erofs/zdata.c").read_text(encoding="utf-8")
    if "z_erofs_decompress_pcluster(&be, io->eio ? -EIO : 0);" not in erofs:
        missing.append("6.6 EROFS queue must propagate only physical I/O failure between clusters")
    if "GFP_KERNEL | __GFP_NOFAIL" not in erofs:
        missing.append("6.6 EROFS synchronous bounce allocation retry")
    result = {"missing": missing, "forbidden": selected, "passed": not (missing or selected)}
    if idle:
        result["cpuidle_refinement"] = idle
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    if not result["passed"]: raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["apply", "configure", "verify"])
    parser.add_argument("--kernel", type=pathlib.Path, required=True)
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--profile", choices=PROFILE_CHOICES, default="baseline")
    args = parser.parse_args()
    kernel = args.kernel.resolve()
    if args.action == "configure": configure(kernel, args.profile)
    elif args.action == "apply": apply(kernel, args.report.resolve(), args.profile)
    else: verify(kernel, args.report.resolve(), args.profile)
