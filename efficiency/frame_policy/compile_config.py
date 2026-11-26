"""Compile explicit Scene CPU/GPU limits and a verified libgui symbol into ABI 1."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

SYMBOLS = (
    "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferiPNS_24SurfaceQueueBufferOutputE",
    "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferi",
)


def probe_offset(blob):
    if blob[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", blob, 18)[0] != 183:
        raise ValueError("Expected Android ARM64 ELF64 libgui")
    programs, sections = struct.unpack_from("<QQ", blob, 32)
    program_size, program_count, section_size, section_count = struct.unpack_from("<HHHH", blob, 54)
    headers = [struct.unpack_from("<IIQQQQIIQQ", blob, sections + i * section_size)
               for i in range(section_count)]
    symbols = {}
    for header in headers:
        if header[1] != 11:
            continue
        strings = headers[header[6]]
        table = blob[strings[4]:strings[4] + strings[5]]
        for offset in range(header[4], header[4] + header[5], header[9]):
            name, info, other, index, value, length = struct.unpack_from("<IBBHQQ", blob, offset)
            end = table.find(b"\0", name)
            text = table[name:end].decode("ascii", "replace")
            if text in SYMBOLS and index and length >= 16:
                symbols[text] = value
    for symbol in SYMBOLS:
        if symbol not in symbols:
            continue
        address = symbols[symbol]
        for i in range(program_count):
            kind, flags, offset, virtual, physical, size, memory, alignment = struct.unpack_from(
                "<IIQQQQQQ", blob, programs + i * program_size)
            if kind == 1 and flags & 1 and virtual <= address and address + 16 <= virtual + size:
                return offset + address - virtual, symbol
    raise ValueError("No supported executable Surface::queueBuffer symbol")


def bounded_limits(value):
    minimum, maximum = value
    if not 0 <= minimum <= maximum <= 10000000 or not maximum:
        raise ValueError(f"Invalid frequency bounds {value}")
    return minimum, maximum


def compile_config(profile, libgui, controls, observe):
    cpus = controls["cpus"]
    if not 1 <= len(cpus) <= 4:
        raise ValueError("Expected one to four distinct CPU policies")
    limiter = profile["features"]["limiter"]["limiters"][controls["scene_limiter"]]
    if len(limiter["cpus"]) != len(cpus):
        raise ValueError("The Scene limiter and CPU topology differ")
    enabled = (1 if controls["cpu_enabled"] else 0) | (2 if controls["gpu_enabled"] else 0)
    flags = enabled | (4 if observe else 0)
    fps = controls["target_fps"]
    if not 15 <= fps <= 240:
        raise ValueError("Target FPS must be explicit and between 15 and 240")
    offset, symbol = probe_offset(libgui)
    device = controls["gpu_device"].encode("ascii")
    if len(device) >= 64 or b"\0" in device:
        raise ValueError("GPU platform device name must fit in 63 ASCII bytes")
    gpu_scene = bounded_limits(controls["gpu_scene_khz"])
    gpu_game = bounded_limits(controls["gpu_game_khz"])
    data = struct.pack("<IIIIQ16s64sIIII", 1, flags, len(cpus), fps, offset,
                       libgui[offset:offset + 16], device, *gpu_scene, *gpu_game)
    policies = []
    for cpu, scene in zip(cpus, limiter["cpus"]):
        scene_limits = bounded_limits((scene["min"], scene["max"]))
        game_limits = bounded_limits(cpu["game_khz"])
        data += struct.pack("<IIIIII", cpu["cpu"], 0, *scene_limits, *game_limits)
        policies.append({"cpu": cpu["cpu"], "scene_khz": scene_limits, "game_khz": game_limits})
    if len({p["cpu"] for p in policies}) != len(policies):
        raise ValueError("Repeated CPU policy")
    data += bytes((4 - len(cpus)) * 24 + 32)
    assert len(data) == 248
    report = {
        "abi": 1, "observe": observe, "symbol": symbol, "offset": offset,
        "libgui_sha256": hashlib.sha256(libgui).hexdigest(), "cpus": policies,
        "gpu_scene_khz": gpu_scene, "gpu_game_khz": gpu_game,
        "scene_limiter": controls["scene_limiter"], "target_fps": fps,
        "dynamic_scene_limiter_feedback_ported": False,
        "scene_gpu_mod_percent_ported": False,
        "configuration_sha256": hashlib.sha256(data).hexdigest(),
    }
    return data, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene-profile", required=True, type=Path)
    parser.add_argument("--libgui", required=True, type=Path)
    parser.add_argument("--controls", required=True, type=Path)
    parser.add_argument("--observe", action="store_true")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    blob, report = compile_config(
        json.loads(args.scene_profile.read_text(encoding="utf-8")),
        args.libgui.read_bytes(), json.loads(args.controls.read_text(encoding="utf-8")), args.observe)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
