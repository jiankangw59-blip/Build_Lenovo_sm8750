#!/usr/bin/env python3
"""Execute the injected C against schema/fallback fixtures in host WebAssembly."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--lld", type=Path, required=True)
    parser.add_argument("--services", type=Path, required=True,
                        help="services.c produced by this branch's apply step")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("adapter", repo / ".github/scripts/selinux_seqno.py")
    adapter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapter)
    source = args.services.read_text(encoding="utf-8")
    generated = (repo / "scripts/test_selinux_ids.c").read_text(encoding="utf-8")
    generated = generated.replace("/* ACTUAL_COMPAT_HELPERS */",
                                  (repo / ".github/scripts/selinux_ids_compat.c").read_text(encoding="utf-8"))
    generated = generated.replace("/* ACTUAL_NATIVE_AND_GUARDED_PARSERS */",
                                  adapter.function(source, "string_to_context_struct") + "\n" +
                                  adapter.function(source, "string_to_context_struct_user_ids"))
    kernel = args.services.resolve().parents[3]
    rules = (kernel / "drivers/kernelsu/selinux/rules.c").read_text(encoding="utf-8")
    generated = generated.replace("/* ACTUAL_BATCH_CLASSIFIER */",
                                  "#define KSU_SEPOLICY_CMD_NORMAL_PERM 1\n"
                                  "#define KSU_SEPOLICY_CMD_XPERM 2\n"
                                  "#define KSU_SEPOLICY_CMD_TYPE 4\n"
                                  "#define KSU_SEPOLICY_CMD_TYPE_ATTR 5\n" +
                                  adapter.function(rules, "sepol_cmd_preserves_user_ids"))
    args.report.mkdir(parents=True, exist_ok=True)
    (args.report / "host-fixture.c").write_text(generated, encoding="utf-8")
    obj = args.report / "host-fixture.o"
    wasm = args.report / "host-fixture.wasm"
    subprocess.run([str(args.clang), "--target=wasm32-unknown-unknown", "-std=gnu11", "-O2",
                    "-ffreestanding", "-fno-builtin", "-nostdlib", "-c",
                    str(args.report / "host-fixture.c"), "-o", str(obj)], check=True)
    subprocess.run([str(args.lld), "-flavor", "wasm", "--no-entry", "--export-all",
                    str(obj), "-o", str(wasm)], check=True)
    script = """const fs = require('fs');
const instance = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(process.argv[1])), {});
const results = Object.entries(instance.exports).filter(([name, fn]) => name.startsWith('test_') && typeof fn === 'function')
  .map(([name, fn]) => ({name, failure_line: fn()}));
console.log(JSON.stringify(results));
if (!results.length || results.some(r => r.failure_line !== 0)) process.exit(1);
"""
    execution = subprocess.run(["node", "-e", script, str(wasm)], capture_output=True, text=True)
    if not execution.stdout.strip():
        raise RuntimeError("SELinux ID fixture did not produce results: " + execution.stderr)
    results = json.loads(execution.stdout)
    (args.report / "results.json").write_text(json.dumps({
        "scope": "Real C helpers with host policy/allocator stand-ins; not a kernel build or device test",
        "tests": results,
    }, indent=2) + "\n", encoding="utf-8")
    for result in results:
        print(result["name"], "PASS" if not result["failure_line"] else result["failure_line"])
    if execution.returncode or not results or any(result["failure_line"] for result in results):
        raise RuntimeError("SELinux ID contract fixture failed; see results.json")
    subprocess.run([sys.executable, str(repo / "scripts/test_selinux_retry.py"),
                    "--clang", str(args.clang), "--lld", str(args.lld),
                    "--services", str(args.services), "--report", str(args.report / "retry")], check=True)



if __name__ == "__main__":
    main()
