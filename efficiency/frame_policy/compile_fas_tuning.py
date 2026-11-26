"""Compile the installed FAS mode margins for the independent integer controller.

This builds tuning data only; it does not activate a controller or change a phone.
"""
import argparse
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import struct
import tomllib

MODES = ("powersave", "balance", "performance", "fast")


def milli(value):
    if type(value) not in (int, float):
        raise ValueError("FPS margin must be a nonnegative number")
    number = Decimal(str(value))
    if not number.is_finite() or number < 0 or number * 1000 != (number * 1000).to_integral_value():
        raise ValueError("FPS margin must be finite with at most three decimal places")
    return int(number * 1000)


def compile_tuning(mode, standard, user):
    if mode not in MODES:
        raise ValueError("Unknown FAS mode")
    config = {**standard.get(mode, {}), **user.get(mode, {})}
    if config.get("core_temp_thresh", "disabled") != "disabled":
        raise ValueError("Custom FAS temperature-offset control is not integrated")
    margin = config.get("margin_fps", (3.0, 1.0, 0.3, 0)[MODES.index(mode)])
    if isinstance(margin, dict):
        if "base" not in margin:
            raise ValueError("Advanced margin_fps requires base and explicit FPS keys")
        base = milli(margin["base"])
        overrides = []
        for key, value in margin.items():
            if key == "base":
                continue
            if not isinstance(key, str) or not key.isascii() or not key.isdigit() or str(int(key)) != key:
                raise ValueError("Expected a canonical integer FPS override key")
            fps, value = int(key), milli(value)
            if not 15 <= fps <= 240 or value > fps * 1000 - 4000:
                raise ValueError("FPS override margin exceeds the supported range")
            overrides.append((fps, value))
        overrides.sort()
    else:
        base, overrides = milli(margin), []
    if base > 30000 or len(overrides) > 8:
        raise ValueError("FAS margin or override count exceeds the supported range")
    blob = struct.pack("<4I", 1, MODES.index(mode), base, len(overrides))
    blob += b"".join(struct.pack("<2I", *item) for item in overrides) + bytes((8 - len(overrides)) * 8)
    assert len(blob) == 80
    return blob, {"abi": 1, "mode": mode, "base_margin_mfps": base, "overrides": overrides,
                  "sha256": hashlib.sha256(blob).hexdigest(), "device_modified": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=MODES, required=True)
    parser.add_argument("--standard-games", type=Path)
    parser.add_argument("--user-games", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    load = lambda path: tomllib.loads(path.read_text(encoding="utf-8")) if path else {}
    data, report = compile_tuning(args.mode, load(args.standard_games), load(args.user_games))
    args.output.write_bytes(data)
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps(report))
