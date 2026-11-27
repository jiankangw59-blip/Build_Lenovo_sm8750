"""Compile Scene CPU margins, GPU governor settings and a verified libgui symbol."""
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


def gpu_percent(profile, preset, visiting=()):
    if preset in visiting:
        raise ValueError("Cycle in Scene presets")
    value = None
    for command in profile["presets"][preset]:
        if command[0] == "@preset":
            nested = gpu_percent(profile, command[1], visiting + (preset,))
            if nested is not None:
                value = nested
        elif command[0] == "$mod_percent":
            value = int(command[1])
    if value is not None and not 1 <= value <= 200:
        raise ValueError("Scene GPU governor percentage is outside the supported range")
    return value


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
    rates = controls.get("fps_candidates", [fps])
    if (not 1 <= len(rates) <= 8 or
            any(type(rate) is not int or not 15 <= rate <= fps for rate in rates) or
            rates != sorted(set(rates)) or rates[-1] != fps):
        raise ValueError("FPS grades must be sorted, unique and end at target_fps")
    offset, symbol = probe_offset(libgui)
    device = controls["gpu_device"].encode("ascii")
    if len(device) >= 64 or b"\0" in device:
        raise ValueError("GPU platform device name must fit in 63 ASCII bytes")
    gpu_scene = bounded_limits(controls["gpu_scene_khz"])
    gpu_game = bounded_limits(controls["gpu_game_khz"])
    gpu_idle = bounded_limits(controls.get("gpu_idle_khz", gpu_scene))
    if profile["alias"].get("mod_percent") != "/sys/class/kgsl/kgsl-3d0/devfreq/mod_percent":
        raise ValueError("The GPU governor control path is not supported by this client")
    scene_percent = gpu_percent(profile, "common_app")
    game_percent = gpu_percent(profile, "common_gaming")
    if scene_percent is None or game_percent is None:
        raise ValueError("Scene has no explicit normal/game GPU governor policy")
    data = struct.pack("<IIIIQ16s64s8IiI", 4, flags, len(cpus), fps, offset,
                       libgui[offset:offset + 16], device, *gpu_scene, *gpu_game, *gpu_idle,
                       scene_percent, game_percent, -1, len(rates))
    policies = []
    idle = profile["features"]["limiter"]["limiters"]["idle"]["cpus"]
    if len(idle) != len(cpus):
        raise ValueError("Scene idle policy topology differs")
    for cpu, scene, screen_off in zip(cpus, limiter["cpus"], idle):
        scene_limits = bounded_limits((scene["min"], scene["max"]))
        game_limits = bounded_limits(cpu["game_khz"])
        idle_limits = bounded_limits((screen_off["min"], screen_off["max"]))
        tokens = scene["margins"].split()
        margins = [(0, int(tokens[0]))]
        margins.extend(tuple(map(int, item.split(":"))) for item in tokens[1:])
        if len(margins) > 8 or any(not 0 < percent <= 1000 for _, percent in margins):
            raise ValueError("Scene margins exceed the supported range")
        if any(a[0] >= b[0] for a, b in zip(margins, margins[1:])):
            raise ValueError("Scene margin thresholds must be increasing")
        data += struct.pack("<10I", cpu["cpu"], 0, *scene_limits, *game_limits,
                            *idle_limits, len(margins), 0)
        for margin in margins:
            data += struct.pack("<II", *margin)
        data += bytes((8 - len(margins)) * 8)
        policies.append({"cpu": cpu["cpu"], "scene_khz": scene_limits, "game_khz": game_limits,
                         "idle_khz": idle_limits, "margins": margins})
    if len({p["cpu"] for p in policies}) != len(policies):
        raise ValueError("Repeated CPU policy")
    data += bytes((4 - len(cpus)) * 104)
    data += struct.pack("<8I", *rates, *([0] * (8 - len(rates))))
    assert len(data) == 592
    report = {
        "abi": 4, "observe": observe, "symbol": symbol, "offset": offset,
        "libgui_sha256": hashlib.sha256(libgui).hexdigest(), "cpus": policies,
        "gpu_scene_khz": gpu_scene, "gpu_game_khz": gpu_game,
        "scene_limiter": controls["scene_limiter"], "target_fps": fps,
        "fps_candidates": rates, "cadence_qualification": "event-driven, two 500 ms stable windows",
        "dynamic_scene_limiter_feedback_ported": True,
        "scene_feedback_backend": "event-driven native CPU cycle counters",
        "scene_gpu_mod_percent_ported": True,
        "gpu_scene_percent": scene_percent, "gpu_game_percent": game_percent,
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
