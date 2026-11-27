"""Compare browser FAS bytes with the production Python tuning compiler."""
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from compile_fas_tuning import MODES, compile_tuning

BANK = {"powersave": {"base": 2.7, "30": 0.5}, "balance": 0, "performance": None}
CASES = [{}, *({"fasMode": mode} for mode in MODES),
         *({"fasMode": mode, "fasProfiles": BANK} for mode in MODES),
         {"fasMode": "fast", "fasMarginFps": {"base": 1.001, "165": 0.3, "60": 0}},
         {"fasMarginFps": 0.001}, {"fasMarginFps": -0.0},
         {"fasMarginFps": {"base": 30, "240": 236}},
         {"fasMarginFps": {"base": 1, **{str(i): 0.1 for i in range(15, 23)}}},
         *({"fasMode": mode} for mode in ("", None, 1, "unknown")),
         *({"fasMarginFps": margin} for margin in ("", None, [], {}, True, -1, 30.001, 0.0001,
             {"base": 1, "060": 1}, {"base": 1, "60.0": 1}, {"base": 1, "165": True},
             {"base": 1, "240": 236.001}, {"base": 1, **{str(i): 0 for i in range(15, 24)}}))]
JS = r'''
const fs=require("node:fs"), {frameFasProtocol}=require("./webroot/protocol.js");
const cases=JSON.parse(fs.readFileSync(0,"utf8"));
console.log(JSON.stringify(cases.map(value=>{
 try{return {hex:Buffer.from(frameFasProtocol(value).blob).toString("hex")};}
 catch(error){return {error:true};}
})));
'''


def main():
    completed = subprocess.run(["node", "-e", JS], input=json.dumps(CASES), text=True,
                               capture_output=True, cwd=HERE, check=True, timeout=15)
    results = json.loads(completed.stdout)
    for index, (settings, result) in enumerate(zip(CASES, results, strict=True)):
        mode = settings.get("fasMode", "balance")
        custom = {mode: {"margin_fps": settings["fasMarginFps"]}} if "fasMarginFps" in settings else {}
        if "fasProfiles" in settings:
            custom = {key: {"margin_fps": value}
                      for key, value in settings["fasProfiles"].items() if value is not None}
        try:
            blob, _ = compile_tuning(mode, {}, custom)
        except (ValueError, TypeError):
            assert result.get("error"), (index, settings, result)
        else:
            assert result.get("hex") == blob.hex(), (index, settings, result)
    print(f"FAS browser/Python compiler parity: {len(CASES)} mode, range, decimal and override cases passed")


if __name__ == "__main__":
    main()
