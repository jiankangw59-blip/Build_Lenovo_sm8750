"""Execute the actual generated setprocattr body with host permission/credential counters."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for name in ("clang", "lld", "hooks", "report"):
    parser.add_argument("--" + name, type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("adapter", repo / ".github/scripts/selinux_seqno.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
body = adapter.function(args.hooks.read_text(encoding="utf-8"), "selinux_setprocattr")
source = (repo / "scripts/test_selinux_attr.c").read_text(encoding="utf-8").replace("/* ACTUAL_SELINUX_SETPROCATTR */", body)
args.report.mkdir(parents=True, exist_ok=True)
file, obj, wasm = (args.report / ("attr-fixture" + suffix) for suffix in (".c", ".o", ".wasm"))
file.write_text(source, encoding="utf-8")
subprocess.run([str(args.clang), "--target=wasm32-unknown-unknown", "-std=gnu11", "-O2", "-ffreestanding", "-fno-builtin", "-nostdlib", "-c", str(file), "-o", str(obj)], check=True)
subprocess.run([str(args.lld), "-flavor", "wasm", "--no-entry", "--export-all", str(obj), "-o", str(wasm)], check=True)
script = """const fs=require('fs'); const i=new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(process.argv[1])),{});
const r=Object.entries(i.exports).filter(([n,f])=>n.startsWith('test_')&&typeof f==='function').map(([name,f])=>({name,failure_line:f()}));
console.log(JSON.stringify(r));if(!r.length||r.some(x=>x.failure_line))process.exit(1);"""
result = subprocess.run(["node", "-e", script, str(wasm)], capture_output=True, text=True, check=True)
tests = json.loads(result.stdout)
(args.report / "attr-results.json").write_text(json.dumps({"scope": "Actual LSM body; host stand-ins for policy, allocation and credentials; not device timing", "tests": tests}, indent=2), encoding="utf-8")
for test in tests:
    print(test["name"], "PASS")
