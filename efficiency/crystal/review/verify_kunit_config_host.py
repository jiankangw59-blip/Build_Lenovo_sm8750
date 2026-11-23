"""Patch-chain and exact C test/snapshot branch checks; XArray/locks mocked."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
TARGET = Path("drivers/crystalfrostwork/crystal_hybridswap/sddc/crystal_sddc.c")
CASE = "crystal_sddc_wb_sparse_state_test"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract(text, name):
    start = re.search(r"^(?:static )?(?:void|bool) " + name + r"\(", text, re.M).start()
    brace = text.index("{", start)
    level, end = 1, brace + 1
    while level:
        level += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


PREFIX = r'''
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long size_t;
typedef _Bool bool;
#define NULL ((void *)0)
#define IS_ENABLED(x) (x)
#define GFP_KERNEL 0
#define CRYSTAL_SDDC_TEST_PAGE_HASH 0x13579bdfU
#define CRYSTAL_SDDC_NONE 0
#define CRYSTAL_SDDC_DELTA 3
#define XA_ZERO_ENTRY ((void *)1)
struct crystal_sddc_cookie { u32 id, generation; };
struct crystal_sddc_snapshot {
 u64 mutation_seq; struct crystal_sddc_cookie ref;
 u32 ref_size, target_size, page_hash; u8 kind;
};
struct crystal_sddc_slot_state {
 struct crystal_sddc_cookie ref; u32 ref_size, target_size, page_hash; u8 kind;
};
struct crystal_sddc_wb_state {
 u64 mutation_seq; struct crystal_sddc_cookie ref;
 u32 ref_size, target_size, wire_size, page_hash; u8 kind;
};
struct xarray { void *entry; };
struct crystal_sddc { unsigned nr_slots; u64 *mutation_seq; struct xarray wb_states; };
struct zram { struct crystal_sddc *sddc; };
struct crystal_sddc_test_ctx { struct zram zram; struct crystal_sddc sddc; u64 mutation_seq[2]; };
struct kunit { void *priv; unsigned failures; };
#define KUNIT_EXPECT_EQ(t,a,b) do { if ((a)!=(b)) (t)->failures++; } while(0)
#define KUNIT_EXPECT_PTR_EQ(t,a,b) KUNIT_EXPECT_EQ(t,a,b)
#define KUNIT_EXPECT_TRUE(t,a) KUNIT_EXPECT_EQ(t,!!(a),1)
#define KUNIT_ASSERT_EQ(t,a,b) do { if ((a)!=(b)) { (t)->failures++; return; } } while(0)
#define KUNIT_ASSERT_NOT_NULL(t,a) do { if (!(a)) { (t)->failures++; return; } } while(0)
static void *memset(void *data, int value, size_t size) {
 unsigned char *p=data; while(size--) *p++=(unsigned char)value; return data;
}
static struct crystal_sddc_wb_state allocated;
static void *kzalloc(size_t size, int flags) { (void)size; (void)flags; memset(&allocated,0,sizeof(allocated)); return &allocated; }
static void kfree(void *pointer) { (void)pointer; }
static int xa_insert(struct xarray *xa, u32 index, void *entry, int flags) {
 (void)index; (void)entry; (void)flags; xa->entry=XA_ZERO_ENTRY; return 0;
}
static int xa_empty(struct xarray *xa) { return !xa->entry; }
static int xa_is_err(void *entry) { (void)entry; return 0; }
static struct crystal_sddc_slot_state *crystal_sddc_state_load_locked(struct crystal_sddc *s, u32 index) {
 (void)s; (void)index; return NULL;
}
static void *crystal_sddc_wb_state_load_raw(struct crystal_sddc *s,u32 index) { (void)index; return s->wb_states.entry; }
static bool crystal_sddc_wb_state_install_locked(struct crystal_sddc *s,u32 index,struct crystal_sddc_wb_state *state) {
 (void)index; if(s->wb_states.entry!=XA_ZERO_ENTRY) return 0; s->wb_states.entry=state; return 1;
}
static void *crystal_sddc_wb_state_erase_locked(struct crystal_sddc *s,u32 index) {
 (void)index; void *entry=s->wb_states.entry; s->wb_states.entry=NULL; return entry;
}
static void crystal_sddc_slot_lock(struct zram *z,u32 index) { (void)z; (void)index; }
static void crystal_sddc_slot_unlock(struct zram *z,u32 index) { (void)z; (void)index; }
static bool crystal_sddc_cookie_equal(const struct crystal_sddc_cookie *a,const struct crystal_sddc_cookie *b) {
 return a->id==b->id && a->generation==b->generation;
}
'''
SUFFIX = r'''
__declspec(dllexport) unsigned run_case(void) {
 struct crystal_sddc_test_ctx ctx={0}; struct kunit test={.priv=&ctx};
 ctx.zram.sddc=&ctx.sddc; ctx.sddc.nr_slots=2; ctx.sddc.mutation_seq=ctx.mutation_seq;
 crystal_sddc_wb_sparse_state_test(&test); return test.failures;
}
'''


def run(args):
    meta_path = HERE / "sddc-kunit-config.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    original = (args.integrity_source / TARGET).read_bytes()
    assert hashlib.sha256(original).hexdigest() == meta["base_after_integrity_sha256"]
    patch = HERE.parent / "sddc-kunit-config.patch"
    assert sha(patch) == meta["patch_sha256"]
    args.output.mkdir(parents=True, exist_ok=False)
    fixture = args.output / "fixture"
    dest = fixture / TARGET
    dest.parent.mkdir(parents=True)
    dest.write_bytes(original)
    applied = subprocess.run([str(args.patch), "-p1", "--fuzz=0", "--batch", "--forward", "-i", str(patch)],
                             cwd=fixture, capture_output=True)
    assert applied.returncode == 0, applied.stderr.decode("utf-8", "replace")
    assert sha(dest) == meta["patched_sha256"]
    before, after = original.decode(), dest.read_text(encoding="utf-8")
    old_case, new_case = extract(before, CASE), extract(after, CASE)
    assert before.replace(old_case, new_case, 1) == after, "Production code changed"
    names = re.findall(r"^\s*KUNIT_CASE\((\w+)\)", after, re.M)
    assert len(names) == 26 and len(set(names)) == 26
    assert "kunit_skip" not in new_case
    modes = []
    for stage, source in (("before", before), ("after", after)):
        body = "\n".join(extract(source, name) for name in (
            "crystal_sddc_snapshot_locked", "crystal_sddc_snapshot_matches_locked", CASE))
        for enabled in (0, 1):
            name = stage + "-native-" + str(enabled)
            harness = args.output / (name + ".c")
            harness.write_text(PREFIX + body + SUFFIX, encoding="utf-8")
            dll_path = args.output / (name + ".dll")
            command = [str(args.compiler), "-shared", "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                       "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fuse-ld=lld", "-nostdlib",
                       "-DCONFIG_CRYSTAL_HYBRIDSWAP_SDDC_ZMS_NATIVE=" + str(enabled),
                       str(harness), "-Wl,/NOENTRY", "-Wl,/NODEFAULTLIB", "-o", str(dll_path)]
            compiled = subprocess.run(command, capture_output=True)
            (args.output / (name + ".build.stderr.txt")).write_bytes(compiled.stderr)
            assert compiled.returncode == 0, compiled.stderr.decode("utf-8", "replace")
            dll = ctypes.CDLL(str(dll_path.resolve()))
            dll.run_case.restype = ctypes.c_uint
            failures = dll.run_case()
            assert (failures > 0) == (stage == "before" and not enabled), (name, failures)
            modes.append({"source": stage, "native_enabled": enabled, "assertion_failures": failures,
                          "extracted_c_sha256": sha(harness), "library_sha256": sha(dll_path)})
    # The stats overlay changes another function; its source stage also accepts
    # this exact zero-fuzz, test-only patch without changing either old patch.
    if args.stats_source:
        stats_meta = json.loads((HERE / "sddc-integrity-stats.json").read_text(encoding="utf-8"))
        stats_source = args.stats_source / TARGET
        assert sha(stats_source) == stats_meta["files"][TARGET.as_posix()]["after_stats_sha256"]
        other_fixture = args.output / "stats-fixture"
        other = other_fixture / TARGET
        other.parent.mkdir(parents=True)
        shutil.copyfile(stats_source, other)
        p = subprocess.run([str(args.patch), "-p1", "--fuzz=0", "--batch", "--forward", "-i", str(patch)],
                           cwd=other_fixture, capture_output=True)
        assert p.returncode == 0
        meta["optional_after_stats_input_sha256"] = sha(stats_source)
        meta["after_stats_and_kunit_config_sha256"] = sha(other)
    meta["host_exact_case_and_snapshot_branch_verified"] = True
    meta["host_kernel_locks_and_xarray_mocked"] = True
    meta["host_modes"] = modes
    report = {"passed": True, "patch_fuzz": 0, "only_test_body_changed": True,
              "state_cases": 26, "mode_results": modes, "kernel_kunit_executed": False,
              "kernel_io_performed": False, "device_used": False, "metadata": meta}
    (args.output / "kunit-config-checks.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({"passed": True, "mode_results": modes, "patch_sha256": meta["patch_sha256"]}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--integrity-source", type=Path, required=True)
    parser.add_argument("--stats-source", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--patch", type=Path, default=Path(shutil.which("patch") or "patch"))
    parser.add_argument("--compiler", type=Path, default=Path(r"C:\Program Files\LLVM\bin\clang.exe"))
    run(parser.parse_args())
