"""Exercise the production aux client with private fake sysfs/ioctl boundaries."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
WRAPPER = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <unistd.h>
#include "scene_aux_uapi.h"
int __real_open(const char *path,int flags,...);
int __real_openat(int directory,const char *path,int flags,...);
int __real_fstatfs(int fd,struct statfs *out);
int __wrap_open(const char *path,int flags,...) {
 mode_t mode=0;
 if(flags&O_CREAT){va_list a;va_start(a,flags);mode=va_arg(a,int);va_end(a);}
 if(!strncmp(path,"/sys/devices/system/cpu/",24)) {
  char mapped[1024];snprintf(mapped,sizeof(mapped),"%s/%s",getenv("AUX_NODES"),strrchr(path,'/')+1);
  return __real_open(mapped,flags,mode);
 }
 return __real_open(path,flags,mode);
}
int __wrap_openat(int directory,const char *path,int flags,...) {
 mode_t mode=0;
 if(flags&O_CREAT){va_list a;va_start(a,flags);mode=va_arg(a,int);va_end(a);}
 if(getenv("AUX_DENY_GPU_WRITE") && (!strcmp(path,"min_pwrlevel") || !strcmp(path,"default_pwrlevel")) &&
    (flags&O_ACCMODE)==O_RDWR){fputs("AUX_INJECTED_GPU_EACCES\n",stderr);errno=EACCES;return -1;}
 return __real_openat(directory,path,flags,mode);
}
int __wrap_fstatfs(int fd,struct statfs *out) {
 int result=__real_fstatfs(fd,out);
 if(!result)out->f_type=getenv("AUX_BAD_FS")?0:0x62656572;
 return result;
}
int __wrap_ioctl(int fd,unsigned long op,...) {
 va_list a;va_start(a,op);struct fp_scene_aux_request *request=va_arg(a,void *);va_end(a);(void)fd;
 if(op!=FP_SCENE_AUX){errno=EINVAL;return -1;}
 if(request->abi!=1 || request->flags || request->reserved || request->length!=sizeof(struct fp_scene_aux_table)){errno=EINVAL;return -1;}
 struct fp_scene_aux_table *t=(void *)(uintptr_t)request->user_ptr;
 FILE *f=fopen(getenv("AUX_RESULT"),"wb");if(!f)return -1;
 fwrite(t,sizeof(*t),1,f);fclose(f);
 if(getenv("AUX_IOCTL_FAIL")){errno=EBUSY;return -1;}
 return 0;
}
'''


class AuxClient(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="frame-aux-client-")
        cls.root = Path(cls.temporary.name)
        cls.node, cls.binary = (cls.root / x for x in ("policy", "framectl"))
        source = cls.root / "wrapper.c"
        source.write_text(WRAPPER)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-fsanitize=undefined",
                        f'-DFRAME_POLICY_DEVICE="{cls.node}"', f'-DFRAME_GPU_ROOT="{cls.root / "gpu"}"',
                        "-I", str(HERE.parent), str(HERE.parent / "framectl.c"), str(source),
                        "-Wl,--wrap=open,--wrap=openat,--wrap=fstatfs,--wrap=ioctl",
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.node.write_text("features=cpu,scene_table,scene_aux\nscene_table_epoch=77\naux_abi=1\naux_table_bytes=26584\naux_feedback_supported=1\n")
        self.nodes = self.root / "nodes"
        self.nodes.mkdir(exist_ok=True)
        for name in ("up_rate_limit_us", "down_rate_limit_us", "hispeed_freq", "boost", "enable", "min_cpus", "max_cpus"):
            (self.nodes / name).write_text("0\n")
        self.result = self.root / "result.bin"
        self.result.unlink(missing_ok=True)
        self.environment = {**os.environ, "AUX_NODES": str(self.nodes), "AUX_RESULT": str(self.result)}
        self.disk = bytearray(26584)
        struct.pack_into("<4I", self.disk, 0, 1, 0, 1, 1)
        struct.pack_into("<28i", self.disk, 56, *([-1] * 28))
        struct.pack_into("<4Ii3I", self.disk, 216, 31, 1000, 2000, 0, -2, 1, 0, 2)
        self.path = self.root / "aux.bin"
        self.gpu = self.root / "gpu"
        self.gpu.mkdir(exist_ok=True)
        devfreq = self.gpu / "devfreq"
        if devfreq.is_symlink(): devfreq.unlink()
        elif devfreq.exists(): devfreq.rmdir()
        devfreq.mkdir()
        for name, value in (("num_pwrlevels","12\n"),("min_pwrlevel","11\n"),("default_pwrlevel","11\n")):
            scalar=self.gpu/name
            if scalar.is_symlink():scalar.unlink()
            scalar.write_text(value)

    def call(self, command, data=None, **env):
        self.path.write_bytes(self.disk if data is None else data)
        return subprocess.run([str(self.binary), command, str(self.path)], capture_output=True,
                              env={**self.environment, **env}, timeout=10)

    def test_preflight_opens_required_resources_without_ioctl_or_disk_runtime_edits(self):
        result = self.call("check-aux")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.result.exists())
        self.assertEqual(self.path.read_bytes(), self.disk)

    def test_load_fills_live_epoch_and_all_seven_fd_slots_without_changing_disk_policy(self):
        result = self.call("load-aux")
        self.assertEqual(result.returncode, 0, result.stderr)
        runtime = self.result.read_bytes()
        self.assertEqual(struct.unpack_from("<Q", runtime, 48), (77,))
        self.assertTrue(all(fd >= 0 for fd in struct.unpack_from("<7i", runtime, 56)))
        self.assertEqual(runtime[168:], self.disk[168:])
        self.assertEqual(self.path.read_bytes(), self.disk)

    def test_missing_resource_or_non_sysfs_fails_before_ioctl(self):
        (self.nodes / "boost").unlink()
        self.assertNotEqual(self.call("check-aux").returncode, 0)
        self.assertFalse(self.result.exists())
        (self.nodes / "boost").write_text("0\n")
        self.assertNotEqual(self.call("load-aux", AUX_BAD_FS="1").returncode, 0)
        self.assertFalse(self.result.exists())
        (self.nodes / "boost").write_text("not-a-scalar\n")
        self.assertNotEqual(self.call("check-aux").returncode, 0)
        self.assertFalse(self.result.exists())

    def test_exact_capability_required_but_empty_clear_is_compatible_with_old_kernel(self):
        self.node.write_text("features=scene_auxiliary\n")
        self.assertNotEqual(self.call("check-aux").returncode, 0)
        empty = bytearray(26584)
        struct.pack_into("<I", empty, 0, 1)
        struct.pack_into("<28i", empty, 56, *([-1] * 28))
        self.assertEqual(self.call("load-aux", empty).returncode, 0)
        self.assertFalse(self.result.exists())
        self.node.write_text("features=scene_aux\nscene_table_epoch=0\naux_abi=1\naux_table_bytes=26584\naux_feedback_supported=1\n")
        self.assertEqual(self.call("load-aux", empty).returncode, 0)
        self.assertEqual(self.result.read_bytes(), empty)

    def test_invalid_disk_tokens_fds_sizes_and_ioctl_error_are_not_hidden(self):
        for offset, value in ((0, 2), (44, 1), (48, 1), (56, 0), (216, 32), (17624, 1)):
            bad = bytearray(self.disk)
            struct.pack_into("<I", bad, offset, value)
            self.assertNotEqual(self.call("load-aux", bad).returncode, 0)
            self.assertFalse(self.result.exists())
        self.assertNotEqual(self.call("load-aux", self.disk[:-1]).returncode, 0)
        self.node.write_text("features=scene_aux\nscene_table_epoch=0\naux_abi=1\naux_table_bytes=26584\naux_feedback_supported=1\n")
        self.assertNotEqual(self.call("load-aux").returncode, 0)
        self.node.write_text("features=scene_aux\nscene_table_epoch=77\naux_abi=1\naux_table_bytes=26584\naux_feedback_supported=1\n")
        self.assertNotEqual(self.call("load-aux", AUX_IOCTL_FAIL="1").returncode, 0)
        self.assertTrue(self.result.exists())

    def test_exact_final_abi_size_and_feedback_preflight_prevent_partial_kernel_handoff(self):
        for fields in ("", "aux_abi=2\naux_table_bytes=26584\n", "aux_abi=1\naux_table_bytes=16296\n",
                       "aux_abi=1\naux_table_bytes=26584junk\n", "aux_abi=1\naux_abi=1\naux_table_bytes=26584\n"):
            self.node.write_text("features=scene_aux\nscene_table_epoch=77\n" + fields)
            self.assertNotEqual(self.call("check-aux").returncode, 0)
            self.assertFalse(self.result.exists())
        full = bytearray(self.disk)
        struct.pack_into("<I", full, 168, 1)  # known source option makes this a full-feedback policy.
        struct.pack_into("<I", full, 172, 1)
        for feedback in ("", "aux_feedback_supported=0\n", "aux_feedback_supported=2\n"):
            self.node.write_text("features=scene_aux\nscene_table_epoch=77\naux_abi=1\naux_table_bytes=26584\n" + feedback)
            self.assertNotEqual(self.call("check-aux", full).returncode, 0)
            self.assertFalse(self.result.exists())
        self.node.write_text("features=scene_aux\nscene_table_epoch=77\naux_abi=1\naux_table_bytes=26584\naux_feedback_supported=1\n")
        self.assertEqual(self.call("check-aux", full).returncode, 0)
        self.assertFalse(self.result.exists())

    def test_gpu_fixed_resource_preflight_never_writes_and_false_does_not_require_kgsl(self):
        enabled=bytearray(self.disk)
        struct.pack_into("<I",enabled,168,1<<10)
        struct.pack_into("<I",enabled,212,1)
        before={p.name:p.read_bytes() for p in self.gpu.iterdir() if p.is_file()}
        self.assertEqual(self.call("check-aux",enabled).returncode,0)
        self.assertEqual(before,{p.name:p.read_bytes() for p in self.gpu.iterdir() if p.is_file()})
        self.assertFalse(self.result.exists())
        denied=self.call("check-aux",enabled,AUX_DENY_GPU_WRITE="1")
        self.assertIn(b"AUX_INJECTED_GPU_EACCES",denied.stderr)
        self.assertNotEqual(denied.returncode,0)
        (self.gpu/"min_pwrlevel").unlink()
        self.assertNotEqual(self.call("check-aux",enabled).returncode,0)
        self.assertEqual(self.call("check-aux").returncode,0)
        self.assertFalse(self.result.exists())

    def test_gpu_n_and_devfreq_gate_match_the_original_dynamic_topology(self):
        enabled=bytearray(self.disk);struct.pack_into("<I",enabled,168,1<<10);struct.pack_into("<I",enabled,212,1)
        for bad in ("0\n","-1\n","4294967296\n","12junk\n"):
            (self.gpu/"num_pwrlevels").write_text(bad)
            self.assertNotEqual(self.call("check-aux",enabled).returncode,0)
        (self.gpu/"num_pwrlevels").write_text("2\n")
        (self.gpu/"min_pwrlevel").write_text("1\n");(self.gpu/"default_pwrlevel").write_text("2\n")
        self.assertNotEqual(self.call("check-aux",enabled).returncode,0)
        (self.gpu/"devfreq").rmdir()
        self.assertEqual(self.call("check-aux",enabled).returncode,0) # Original no-default gate.
        (self.gpu/"devfreq").symlink_to(self.nodes,target_is_directory=True)
        (self.gpu/"default_pwrlevel").write_text("1\n")
        self.assertEqual(self.call("check-aux",enabled).returncode,0) # Class devfreq symlink follows stat.
        (self.gpu/"default_pwrlevel").unlink();(self.gpu/"default_pwrlevel").symlink_to(self.nodes/"boost")
        self.assertNotEqual(self.call("check-aux",enabled).returncode,0) # Final scalar symlink forbidden.
        self.assertFalse(self.result.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
