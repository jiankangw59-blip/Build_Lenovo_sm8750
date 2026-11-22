#!/usr/bin/env python3
"""Execute the real conversion core with deterministic SID-table retry failures."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ("clang", "lld", "services", "report"):
        ap.add_argument("--" + name, type=Path, required=True)
    a = ap.parse_args()
    repo = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("adapter", repo / ".github/scripts/selinux_seqno.py")
    adapter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapter)
    core = adapter.function(a.services.read_text(encoding="utf-8"), "security_context_to_sid_core")
    assert core.count("memcpy(scontext2, scontext, scontext_len)") == 1
    assert core.index("memcpy(scontext2") > core.index("if (rc == -ESTALE)")
    assert core.index("context_destroy(&context);", core.index("if (rc == -ESTALE)")) < core.index("memcpy(scontext2")
    assert "scontext2[scontext_len] = '\\0';" in core
    generated = (repo / "scripts/test_selinux_retry.c").read_text(encoding="utf-8")
    generated = generated.replace("/* ACTUAL_CONTEXT_TO_SID_CORE */", core)
    a.report.mkdir(parents=True, exist_ok=True)
    cfile, obj, wasm = (a.report / ("retry-fixture" + suffix) for suffix in (".c", ".o", ".wasm"))
    cfile.write_text(generated, encoding="utf-8")
    subprocess.run([str(a.clang), "--target=wasm32-unknown-unknown", "-std=gnu11", "-O2",
                    "-ffreestanding", "-fno-builtin", "-nostdlib", "-c", str(cfile), "-o", str(obj)], check=True)
    subprocess.run([str(a.lld), "-flavor", "wasm", "--no-entry", "--export-all",
                    str(obj), "-o", str(wasm)], check=True)
    script = """const fs = require('fs');
const instance = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(process.argv[1])), {});
const tests = Object.entries(instance.exports).filter(([n,f]) => n.startsWith('test_') && typeof f === 'function')
  .map(([name,f]) => ({name, failure_line:f()}));
console.log(JSON.stringify(tests));
if (!tests.length || tests.some(t => t.failure_line !== 0)) process.exit(1);
"""
    result = subprocess.run(["node", "-e", script, str(wasm)], capture_output=True, text=True, check=True)
    tests = json.loads(result.stdout)
    (a.report / "retry-results.json").write_text(json.dumps({
        "scope": "Actual conversion C with mocked allocation, RCU and SID calls; not device fault injection",
        "tests": tests,
    }, indent=2) + "\n")
    for test in tests:
        print(test["name"], "PASS")


if __name__ == "__main__":
    main()
