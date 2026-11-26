"""Exercise the real FAS client dispatch through an intercepted Linux ioctl.

The device path and ioctl boundary are replaced at build time. No Android
device, /dev/frame_policy or /data/adb is read or written by these host tests.
"""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
WRAPPER = r'''
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "fas_tuning_uapi.h"
int __wrap_ioctl(int fd, unsigned long request, ...) {
 va_list arguments;
 (void)fd;
 if (request != FP_FAS_TUNING || _IOC_NR(request) != 0x84 || _IOC_SIZE(request) != 80) {
  errno = ENOTTY; return -1;
 }
 va_start(arguments, request);
 struct fp_fas_tuning *value = va_arg(arguments, struct fp_fas_tuning *);
 va_end(arguments);
 FILE *log = fopen(getenv("FAS_IOCTL_LOG"), "wb");
 if (!log) return -1;
 fprintf(log, "%lu\n", request);
 if (fwrite(value, 1, sizeof(*value), log) != sizeof(*value)) { fclose(log); return -1; }
 fclose(log);
 if (getenv("FAS_IOCTL_FAIL")) { errno = EIO; return -1; }
 return 0;
}
'''


class FasIoctl(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="frame-fas-ioctl-")
        cls.root = Path(cls.temporary.name)
        cls.device, cls.log = cls.root / "native-status", cls.root / "ioctl.log"
        wrapper = cls.root / "wrapper.c"
        wrapper.write_text(WRAPPER, encoding="utf-8")
        cls.controller = cls.root / "framectl"
        subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", f'-DFRAME_POLICY_DEVICE="{cls.device}"',
                        "-I", str(HERE.parent), str(HERE.parent / "framectl.c"), str(wrapper),
                        "-Wl,--wrap=ioctl", "-o", str(cls.controller)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.device.write_text("abi=4\nconfigured=1\nfeatures=cpu,fas_tuning,gpu\nlast_error=0\n")
        self.log.unlink(missing_ok=True)
        self.blob = self.root / "fas.bin"
        self.blob.write_bytes(self.tuning())

    @staticmethod
    def tuning(mode=1):
        return struct.pack("<4I", 1, mode, 1000, 0) + bytes(64)

    def call(self, command="load-fas", success=True, fail=False):
        environment = {**os.environ, "FAS_IOCTL_LOG": str(self.log)}
        if fail:
            environment["FAS_IOCTL_FAIL"] = "1"
        result = subprocess.run([str(self.controller), command, str(self.blob)],
                                capture_output=True, env=environment, timeout=5)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return result

    def test_four_modes_reach_real_dispatch_with_exact_80_bytes(self):
        for mode in range(4):
            with self.subTest(mode=mode):
                value = self.tuning(mode)
                self.blob.write_bytes(value)
                self.call()
                number, payload = self.log.read_bytes().split(b"\n", 1)
                self.assertEqual(int(number), 0x40504684)
                self.assertEqual(payload, value)

    def test_check_is_read_only_and_requires_exact_feature_token(self):
        self.call("check-fas")
        self.assertFalse(self.log.exists())
        for feature in ("cpu,gpu", "cpu,fas_tuning_pending", "cpu,my_fas_tuning"):
            with self.subTest(feature=feature):
                self.device.write_text("features=" + feature + "\n")
                self.call("check-fas", success=False)
                self.call(success=False)
                self.assertFalse(self.log.exists())

    def test_invalid_bytes_fail_before_ioctl(self):
        values = [self.tuning()[:-1], self.tuning() + b"\x00", b"", self.tuning(4)]
        for offset, value in ((0, 0), (8, 30001), (12, 9), (76, 1)):
            changed = bytearray(self.tuning())
            struct.pack_into("<I", changed, offset, value)
            values.append(bytes(changed))
        for index, value in enumerate(values):
            with self.subTest(index=index):
                self.blob.write_bytes(value)
                self.call(success=False)
                self.call("check-fas", success=False)
                self.assertFalse(self.log.exists())

    def test_ioctl_failure_is_reported(self):
        result = self.call(success=False, fail=True)
        self.assertTrue(self.log.exists())
        self.assertIn(b"update frame policy", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
