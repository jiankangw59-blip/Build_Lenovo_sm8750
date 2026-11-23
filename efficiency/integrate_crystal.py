"""Import only the pinned Crystal data plane, retaining the OEM kernel ABI."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent / "crystal"
DRIVER = "drivers/crystalfrostwork/crystal_hybridswap"
PIN = "13d291aef3d1d9607ad3fa5ff30dc23b2888a3da"
CRYSTAL_PROFILES = {
    "crystal": {"sddc": False, "compressor": "lz4kd", "lock": True, "kunit": False},
    "crystal-lz4kds-test": {"sddc": True, "compressor": "lz4kd", "lock": False, "kunit": True},
    "crystal-lz4kds": {"sddc": True, "compressor": "lz4kds", "lock": True, "kunit": False},
}
PROFILE_CHOICES = ("baseline", "kcompressd", "le9uo", *CRYSTAL_PROFILES)
SDDC_INTEGRITY = {
    "upstream_commit": PIN,
    "source_file": DRIVER + "/sddc/crystal_sddc.c",
    "source_sha256": "7f0b3dc9c0cc9a0ce54d3aa5dbc05aac2bb464f7778969281112981055a9af2d",
    "patch_sha256": "dea7c0e140926560adbe5fa6a30942c0f3b919c14690de613b49fdaf704e2554",
    "patched_sha256": "e51140d12d447b21dc61101f819ad764a60d84ca949c8956a9a55f8095f6aa05",
    "new_state_cases": 2, "expected_state_cases": 26, "expected_codec_cases": 6,
    "wire_format_changed": False, "exported_kmi_changed": False, "sddc_zms_native_enabled": False,
}


def integrity_patch_policy():
    """Check the reviewed overlay independently of the immutable upstream manifest."""
    review = json.loads((HERE / "review/sddc-integrity.json").read_text(encoding="utf-8"))
    if any(review.get(name) != value for name, value in SDDC_INTEGRITY.items()):
        raise RuntimeError("Unreviewed SDDC integrity policy")
    raw = (HERE / "sddc-integrity.patch").read_bytes()
    if hashlib.sha256(raw).hexdigest() != SDDC_INTEGRITY["patch_sha256"]:
        raise RuntimeError("SDDC integrity patch digest differs")
    text = raw.decode("utf-8")
    old = re.findall(r"^--- a/(.+)$", text, re.M)
    new = re.findall(r"^\+\+\+ b/(.+)$", text, re.M)
    if old != [SDDC_INTEGRITY["source_file"]] or new != old:
        raise RuntimeError("SDDC integrity patch destinations differ")
    return {**SDDC_INTEGRITY, "patch_file": "sddc-integrity.patch", "fuzz": 0}


def apply_integrity_patch(raw, scratch_parent):
    policy = integrity_patch_policy()
    if hashlib.sha256(raw).hexdigest() != policy["source_sha256"]:
        raise RuntimeError("SDDC integrity source differs from the reviewed pin")
    program = shutil.which("patch")
    if not program and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Git/usr/bin/patch.exe"
        if candidate.is_file():
            program = str(candidate)
    if not program:
        raise RuntimeError("GNU patch is required for the reviewed SDDC overlay")
    scratch_parent = scratch_parent.resolve()
    with tempfile.TemporaryDirectory(prefix=".crystal-sddc-integrity-", dir=scratch_parent) as temporary:
        root = Path(temporary).resolve()
        if root.parent != scratch_parent or not root.name.startswith(".crystal-sddc-integrity-"):
            raise RuntimeError("Unexpected SDDC patch scratch directory")
        target = root / policy["source_file"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(raw)
        command = [program, "--batch", "--forward", "--fuzz=0", "-p1", "-i", str(HERE / policy["patch_file"])]
        for extra in [["--dry-run"], []]:
            process = subprocess.run([*command, *extra], cwd=root, capture_output=True)
            if process.returncode:
                raise RuntimeError("SDDC integrity patch failed: " +
                                   (process.stdout + process.stderr).decode("utf-8", "replace")[-4000:])
        patched = target.read_bytes()
    if hashlib.sha256(patched).hexdigest() != policy["patched_sha256"]:
        raise RuntimeError("Patched SDDC source differs from the reviewed result")
    text = patched.decode("utf-8")
    if len(re.findall(r"\bKUNIT_CASE\(", text)) != policy["expected_state_cases"]:
        raise RuntimeError("Unexpected SDDC state KUnit case count")
    for name in ["crystal_sddc_delta_proof_identity_test", "crystal_sddc_resident_hash_error_test"]:
        if text.count("KUNIT_CASE(" + name + ")") != 1:
            raise RuntimeError("Missing SDDC integrity KUnit declaration: " + name)
    return patched, policy


def stage2_profile(profile):
    if profile not in PROFILE_CHOICES:
        raise ValueError("Unknown efficiency profile: " + profile)
    return "kcompressd" if profile in CRYSTAL_PROFILES else profile


def crystal_configuration(profile):
    settings = CRYSTAL_PROFILES[profile]
    values = {name: "n" for name in [
        "ZRAM", "ZSWAP", "CRYSTAL_HYBRIDSWAP_SDDC_ZMS_NATIVE",
        "CRYSTAL_HYBRIDSWAP_ZRAM_PIN_SIZE", "CRYSTAL_HYBRIDSWAP_ZRAM_MULTI_COMP",
        "CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS", "CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM",
        "CRYSTAL_HYBRIDSWAP_ERM_AVAIL_BUFFER_DEFAULT_ON",
    ]}
    values.update({name: "y" for name in [
        "ZSMALLOC", "CRYPTO_LZ4KD", "ARM64_4K_PAGES", "CRYSTAL_HYBRIDSWAP",
        "CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK", "CRYSTAL_HYBRIDSWAP_ZRAM_TRACK_ENTRY_ACTIME",
    ]})
    values.update({
        "CRYSTAL_HYBRIDSWAP_SDDC": "y" if settings["sddc"] else "n",
        "CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD": "y" if settings["sddc"] else "n",
        "CRYSTAL_HYBRIDSWAP_SDDC_KUNIT_TEST": "y" if settings["kunit"] else "n",
        "CRYSTAL_HYBRIDSWAP_ZRAM_LOCK_DEF_COMP": "y" if settings["lock"] else "n",
        "CRYSTAL_HYBRIDSWAP_ZRAM_DEF_COMP": '"' + settings["compressor"] + '"',
    })
    if settings["kunit"]:
        values["KUNIT"] = "y"
    return values


def unexpected_sddc_options(actual, profile):
    allowed = {"CONFIG_" + name + "=y" for name, value in crystal_configuration(profile).items()
               if value == "y" and "SDDC" in name} if profile in CRYSTAL_PROFILES else set()
    return [line for line in actual if re.fullmatch(r"CONFIG_.*(?:LZ4KDS|SDDC).*=[ym]", line)
            and line not in allowed]


def crystal_configuration_errors(actual, profile):
    values = dict(line[len("CONFIG_"):].split("=", 1) for line in actual if line.startswith("CONFIG_"))
    errors = []
    for name, expected in crystal_configuration(profile).items():
        # Kconfig may omit disabled child symbols entirely when their parent is off.
        if values.get(name, "n") != expected:
            errors.append(f"CONFIG_{name} must be {expected}, found {values.get(name, 'n')}")
    errors.extend("Unexpected SDDC option: " + line for line in unexpected_sddc_options(actual, profile))
    return errors


def source_manifest(profile):
    settings = CRYSTAL_PROFILES[profile]
    manifest = json.loads((HERE / "source-manifest.json").read_text(encoding="utf-8"))
    if manifest["commit"] != PIN:
        raise RuntimeError("Unreviewed Crystal source revision")
    if settings["sddc"]:
        extra = json.loads((HERE / "sddc-source-manifest.json").read_text(encoding="utf-8"))
        if extra["commit"] != PIN or extra["repository"] != manifest["repository"]:
            raise RuntimeError("SDDC source does not match the pinned Crystal revision")
        if manifest["files"].keys() & extra["files"].keys():
            raise RuntimeError("SDDC manifest overwrites an existing Crystal import")
        manifest["files"].update(extra["files"])
    return manifest


def integrate(kernel, report, profile="crystal"):
    settings = CRYSTAL_PROFILES[profile]
    manifest = source_manifest(profile)
    pending = {}
    for name, expected in manifest["files"].items():
        target = (kernel / name).resolve()
        source = (HERE / "source" / name).resolve()
        if not target.is_relative_to(kernel) or not source.is_relative_to(HERE / "source"):
            raise RuntimeError("Crystal source path escapes its tree")
        if target.exists():
            raise RuntimeError("Preserve the existing Crystal destination: " + name)
        raw = source.read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise RuntimeError("Crystal source digest differs: " + name)
        pending[name] = raw

    integrity = None
    if settings["sddc"]:
        name = SDDC_INTEGRITY["source_file"]
        pending[name], integrity = apply_integrity_patch(pending[name], kernel)
        codec_tests = pending[DRIVER + "/sddc/crystal_sddc_codec_test.c"].decode("utf-8")
        if len(re.findall(r"\bKUNIT_CASE\(", codec_tests)) != integrity["expected_codec_cases"]:
            raise RuntimeError("Unexpected SDDC codec KUnit case count")

    # The vendor hook already accepts this enum in 6.6.89, but its definition
    # is private to vmscan.c. Do not change that exported hook or a core layout.
    vmscan = (kernel / "mm/vmscan.c").read_text(encoding="utf-8")
    enum = re.search(r"enum scan_balance\s*\{([^}]+)\};", vmscan)
    names = re.findall(r"\bSCAN_[A-Z]+\b", enum[1]) if enum else []
    if names != ["SCAN_EQUAL", "SCAN_FRACT", "SCAN_ANON", "SCAN_FILE"]:
        raise RuntimeError("Unexpected OEM scan_balance definition")
    hook = (kernel / "include/trace/hooks/vmscan.h").read_text(encoding="utf-8")
    if "TP_PROTO(enum scan_balance *scan_type)" not in hook:
        raise RuntimeError("Unexpected OEM scan-type hook signature")

    # Resolve the charging gate through the public power-supply API rather
    # than importing an unrelated private symbol resolver.
    name = DRIVER + "/zram_drv.c"
    text = pending[name].decode("utf-8")
    anchor = "#include <linux/sysms_finder.h>\n"
    if text.count(anchor) != 1:
        raise RuntimeError("Crystal unused-header adaptation anchor differs")
    text = text.replace(anchor, "#include <linux/power_supply.h>\n")
    anchor = "static bool zram_zms_gc_should_run(const struct zms_stats *stats)\n"
    if text.count(anchor) != 1 or text.count("check_charging_state()") != 1:
        raise RuntimeError("Crystal charging-gate adaptation anchor differs")
    charging = """static bool crystal_battery_charging(void)
{
\tstruct power_supply *battery;
\tunion power_supply_propval status;
\tint ret;

\tbattery = power_supply_get_by_name("battery");
\tif (!battery)
\t\treturn false;
\tret = power_supply_get_property(battery, POWER_SUPPLY_PROP_STATUS, &status);
\tpower_supply_put(battery);
\treturn !ret && (status.intval == POWER_SUPPLY_STATUS_CHARGING ||
\t\t       status.intval == POWER_SUPPLY_STATUS_FULL);
}

"""
    text = text.replace(anchor, charging + anchor).replace("check_charging_state()", "crystal_battery_charging()")
    anchor = "INIT_DELAYED_WORK(&zram->zms_gc_periodic_work,"
    if text.count(anchor) != 1:
        raise RuntimeError("Crystal periodic-GC adaptation anchor differs")
    text = text.replace(anchor, "INIT_DEFERRABLE_WORK(&zram->zms_gc_periodic_work,")
    pending[name] = text.encode("utf-8")

    # Keep the native comparison and the two explicit SDDC variants distinct,
    # including after a later config merge. No host reconstruction is imported.
    name = DRIVER + "/zcomp.c"
    text = pending[name].decode("utf-8")
    anchor = '#include "zcomp.h"\n'
    if text.count(anchor) != 1:
        raise RuntimeError("Crystal compressor guard anchor differs")
    if not settings["sddc"]:
        guard = """
#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC) || \\
    IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD) || \\
    IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_ZMS_NATIVE)
#error "The PLQ110 Crystal candidate requires SDDC and LZ4KDS disabled"
#endif
"""
    else:
        guard = """
#if !IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC) || \\
    !IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD) || \\
    !IS_ENABLED(CONFIG_ARM64_4K_PAGES)
#error "The explicit PLQ110 LZ4KDS profile requires SDDC, its private codec and 4K pages"
#endif
#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_ZMS_NATIVE)
#error "This PLQ110 LZ4KDS trial requires flatten-to-ordinary ZMS writeback"
#endif
"""
        condition = "!" if settings["kunit"] else ""
        guard += (f"#if {condition}IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_KUNIT_TEST)\n"
                  f'#error "Unexpected SDDC KUnit selection for {profile}"\n#endif\n')
    pending[name] = text.replace(anchor, anchor + guard).encode("utf-8")

    links = {
        "drivers/Kconfig": '\nsource "' + DRIVER + '/Kconfig"\n',
        "drivers/Makefile": "\nobj-$(CONFIG_CRYSTAL_HYBRIDSWAP) += crystalfrostwork/\n",
    }
    for name, addition in links.items():
        raw = (kernel / name).read_bytes()
        if b"crystalfrostwork" in raw:
            raise RuntimeError("Preserve existing Crystal build linkage")
        pending[name] = raw + addition.encode("ascii")
    parent = "drivers/crystalfrostwork/Makefile"
    if (kernel / parent).exists():
        raise RuntimeError("Preserve the existing Crystal parent Makefile")
    pending[parent] = b"# SPDX-License-Identifier: GPL-2.0\nobj-$(CONFIG_CRYSTAL_HYBRIDSWAP) += crystal_hybridswap/\n"

    written = {}
    for name, raw in pending.items():
        target = kernel / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(raw)
        written[name] = hashlib.sha256(raw).hexdigest()
    result = {
        "source_repository": manifest["repository"], "source_commit": PIN, "profile": profile,
        "source_sha256": manifest["files"], "installed_sha256": written,
        "adaptations": ["Use public power-supply API for the charging gate; no private symbol finder",
                        "Make periodic GC deferrable while retaining PM, charging and dirty-flush paths",
                        "Apply an explicit compile-time policy guard for " + profile,
                        "Use the pinned private LZ4KDS codec" if settings["sddc"] else
                        "Use the existing Crypto API pure LZ4KD implementation",
                        "Validate the unchanged OEM scan enum and hook signature"],
        "configuration": crystal_configuration(profile),
        "native_sddc_zms": False, "sddc_kunit_requested": settings["kunit"],
        "sddc_integrity_patch": integrity,
        "excluded": ["proprietary framework", "hardware overrides", "module overlay", "sidecar allocator",
                     "native SDDC-to-ZMS"] + ([] if settings["sddc"] else
                                               ["SDDC implementation objects", "private delta codec objects"]),
        "passed": True, "build_verified": False, "runtime_verified": False,
    }
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result
