"""Import only the pinned Crystal data plane, retaining the OEM kernel ABI."""
import hashlib
import json
from pathlib import Path
import re


HERE = Path(__file__).resolve().parent / "crystal"
DRIVER = "drivers/crystalfrostwork/crystal_hybridswap"
PIN = "13d291aef3d1d9607ad3fa5ff30dc23b2888a3da"


def integrate(kernel, report):
    manifest = json.loads((HERE / "source-manifest.json").read_text(encoding="utf-8"))
    if manifest["commit"] != PIN:
        raise RuntimeError("Unreviewed Crystal source revision")
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

    # The first device candidate must not accidentally compile the experimental
    # codec or similarity-reference machinery, even after a config merge.
    name = DRIVER + "/zcomp.c"
    text = pending[name].decode("utf-8")
    anchor = '#include "zcomp.h"\n'
    if text.count(anchor) != 1:
        raise RuntimeError("Crystal compressor guard anchor differs")
    guard = """
#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC) || \\
    IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD) || \\
    IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_ZMS_NATIVE)
#error "The PLQ110 Crystal candidate requires SDDC and LZ4KDS disabled"
#endif
"""
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
        "source_repository": manifest["repository"], "source_commit": PIN,
        "source_sha256": manifest["files"], "installed_sha256": written,
        "adaptations": ["Use public power-supply API for the charging gate; no private symbol finder",
                        "Make periodic GC deferrable while retaining PM, charging and dirty-flush paths",
                        "Compile-time reject SDDC, private delta codec and native SDDC ZMS",
                        "Use the existing Crypto API pure LZ4KD implementation",
                        "Validate the unchanged OEM scan enum and hook signature"],
        "excluded": ["proprietary framework", "SDDC implementation objects",
                     "private delta codec objects", "hardware overrides", "module overlay"],
        "passed": True, "build_verified": False, "runtime_verified": False,
    }
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result
