"""Run the production shell with fault-injected controller/rename boundaries.

The stubs model device commands, not the shell transaction implementation.
Tests run on Linux/Actions and never access an Android device or /data/adb.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent

CONTROLLER = r'''
import json,os,pathlib,shutil,struct,sys
s=pathlib.Path(os.environ['TRANSACTION_STATE'])
def log(text):
 with (s/'calls').open('a') as f:f.write(text+'\n')
def status():
 print('abi=4\nconfigured=1\nlast_error=0\nfeatures='+ (s/'features').read_text().strip())
def clean():
 if (s/'cleanup-blocked').exists() or (s/'legacy-running').exists():return 1
 print('legacy_was_running='+str(int((s/'legacy-stop-pending').exists())))
 return 0
a=sys.argv[1];log(a)
if a=='unpack-settings':
 source=pathlib.Path(os.environ['TRANSACTION_FIXTURES'])/sys.stdin.read().strip()
 for name in ('settings.json','config.bin','games.bin','scene.bin','fas.bin','enabled','scene-required'):
  shutil.copyfile(source/name,pathlib.Path(sys.argv[2])/name)
elif a=='check-fas':
 value=pathlib.Path(sys.argv[2]).read_bytes()
 if 'fas_tuning' not in (s/'features').read_text().strip().split(',') or len(value)!=80:sys.exit(1)
 abi,mode,base,count=struct.unpack_from('<4I',value)
 if abi!=1 or mode>=4 or base>30000 or count>8:sys.exit(1)
elif a=='suspend-legacy':
 if (s/'legacy-running').exists():
  boot=pathlib.Path('/proc/sys/kernel/random/boot_id').read_text().strip()
  (s/'legacy-stop-pending').write_text('FP_LEGACY_STOP_V1 '+boot+' 100 100\n')
  (s/'legacy-running').unlink()
 sys.exit(clean())
elif a=='legacy-clean':sys.exit(clean())
elif a=='disable':
 if (s/'disable-blocked').exists():sys.exit(1)
 (s/'runtime').write_text('none')
elif a in ('load','load-scene','load-rules','load-fas'):
 identity=json.loads((pathlib.Path(sys.argv[2]).parent/'settings.json').read_text())['identity']
 log(a+':'+identity)
 if a=='load-fas' and (s/('fail-fas-'+identity)).exists():
  (s/('fail-fas-'+identity)).unlink();sys.exit(1)
 if a=='load' and (s/('fail-load-'+identity)).exists():
  (s/('fail-load-'+identity)).unlink();sys.exit(1)
 if a=='load':(s/'runtime').write_text(identity)
 if a=='load-fas':(s/'runtime-fas').write_bytes(pathlib.Path(sys.argv[2]).read_bytes())
else:sys.exit(2)
status()
'''


class Transactions(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="frame-transaction-")
        self.root = Path(self.temporary.name)
        self.module, self.state = self.root / "module", self.root / "state"
        self.bin, self.fixtures = self.root / "bin", self.root / "fixtures"
        for directory in (self.module, self.state, self.bin, self.fixtures):
            directory.mkdir(mode=0o700)
        self.environment = {**os.environ, "TRANSACTION_STATE": str(self.state),
                            "TRANSACTION_FIXTURES": str(self.fixtures),
                            "PATH": str(self.bin) + os.pathsep + os.environ["PATH"]}
        source = (HERE / "config.sh").read_text(encoding="utf-8")
        # Replace only device boundaries. The actual transaction functions/cases
        # are executed verbatim by the real POSIX shell and util-linux flock.
        source = source.replace("state=/data/adb/frame-policy", f"state='{self.state}'")
        source = source.replace("legacy=/data/adb/modules/fas_rs/event-control.sh",
                                f"legacy='{self.module / 'legacy.sh'}'")
        source = source.replace("/dev/frame_policy", "/dev/null")
        source = source.replace("sha256sum /system/lib64/libgui.so", f"sha256sum '{self.root / 'libgui'}'")
        (self.module / "config.sh").write_text(source, encoding="utf-8")
        (self.root / "libgui").write_bytes(b"test libgui")
        self.script(self.module / "framectl", CONTROLLER)
        (self.module / "legacy.sh").write_text('#!/bin/sh\n'
            ': > "$TRANSACTION_STATE/legacy-running"\n'
            'printf "legacy-start\\n" >> "$TRANSACTION_STATE/calls"\n')
        self.script(self.bin / "getprop", "print('PLQ110')")
        self.script(self.bin / "cat", f"""
import os,sys,pathlib
if len(sys.argv)==2 and sys.argv[1]=='/dev/null':
 s=pathlib.Path(os.environ['TRANSACTION_STATE'])
 print('abi=4\\nconfigured=1\\nlast_error=0\\nfeatures='+(s/'features').read_text().strip())
else:os.execv({shutil.which('cat')!r},['cat']+sys.argv[1:])
""")
        self.script(self.bin / "mv", f"""
import os,pathlib,sys
s=pathlib.Path(os.environ['TRANSACTION_STATE'])
if pathlib.Path(sys.argv[-1])==s/'current' and (s/'fail-commit').exists():
 (s/'fail-commit').unlink();sys.exit(1)
os.execv({shutil.which('mv')!r},['mv']+sys.argv[1:])
""")
        self.script(self.bin / "flock", f"""
import os,pathlib,sys
(pathlib.Path(os.environ['TRANSACTION_STATE'])/'lock-requested').touch()
os.execv({shutil.which('flock')!r},['flock']+sys.argv[1:])
""")
        (self.state / "features").write_text("cpu,gpu_file_credentials,game_relations,fps_per_game,scene_table,fas_tuning")
        (self.state / "runtime").write_text("none")
        self.generation("defaults", enabled=False, destination=self.module / "defaults")
        (self.module / "defaults/libgui.sha256").write_text(hashlib.sha256(b"test libgui").hexdigest() + "\n")
        self.generation("A")
        self.generation("B")
        self.generation("off", enabled=False)

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def script(path, text):
        path.write_text("#!" + sys.executable + "\n" + text.lstrip(), encoding="utf-8")
        path.chmod(0o700)

    def generation(self, identity, enabled=True, destination=None):
        target = destination or self.fixtures / identity
        target.mkdir()
        (target / "settings.json").write_text(json.dumps({"identity": identity, "enabled": enabled}) + "\n")
        (target / "enabled").write_text("1\n" if enabled else "0\n")
        (target / "scene-required").write_text("0\n")
        (target / "config.bin").write_bytes(struct.pack("<2I", 4, 1) + bytes(584))
        (target / "games.bin").write_bytes(b"rules")
        (target / "scene.bin").write_bytes(b"scene")
        mode = {"A": 0, "B": 2}.get(identity, 1)
        (target / "fas.bin").write_bytes(struct.pack("<4I", 1, mode, [3000, 1000, 300, 0][mode], 0) + bytes(64))
        return target

    def select(self, identity, parent=None):
        target = self.state / ("generation." + identity)
        shutil.copytree(self.fixtures / identity, target)
        if parent is not None:
            (target / "parent-generation").write_text(parent + "\n")
        (self.state / "current").write_text(target.name + "\n")
        (self.state / "runtime").write_text(identity)
        (self.state / "runtime-fas").write_bytes((target / "fas.bin").read_bytes())
        return target

    def current(self):
        pointer = self.state / "current"
        name = pointer.read_text().strip() if pointer.exists() else "defaults"
        return self.module / name if name == "defaults" else self.state / name

    def run_command(self, *arguments, incoming="", success=True):
        completed = subprocess.run(["sh", str(self.module / "config.sh"), *arguments],
                                   input=incoming, text=True, capture_output=True,
                                   env=self.environment, timeout=15)
        if success:
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        else:
            self.assertNotEqual(completed.returncode, 0)
        return completed

    def save(self, identity, success=True):
        digest = hashlib.sha256((self.current() / "settings.json").read_bytes()).hexdigest()
        return self.run_command("save", digest, incoming=identity, success=success)

    def test_first_save_can_rollback_defaults_and_toggle_back(self):
        (self.state / "legacy-running").touch()
        (self.state / "paused").write_text("keep user preference")
        self.save("A")
        self.assertEqual((self.state / "runtime").read_text(), "A")
        self.assertEqual((self.current() / "parent-generation").read_text().strip(), "defaults")
        self.run_command("rollback")
        self.assertEqual((self.state / "runtime").read_text(), "none")
        self.assertTrue((self.state / "legacy-running").exists())
        self.run_command("rollback")
        self.assertEqual((self.state / "runtime").read_text(), "A")
        self.assertFalse((self.state / "legacy-running").exists())
        self.assertEqual((self.state / "paused").read_text(), "keep user preference")
        self.assertEqual((self.state / "runtime-fas").read_bytes(), (self.fixtures / "A/fas.bin").read_bytes())

    def test_failed_rollback_restores_current_runtime_and_pointer(self):
        self.select("B")
        original = self.select("A", parent="generation.B")
        (self.state / "fail-load-B").touch()
        self.run_command("rollback", success=False)
        self.assertEqual(self.current(), original)
        self.assertEqual((self.state / "runtime").read_text(), "A")
        self.assertEqual((original / "parent-generation").read_text().strip(), "generation.B")
        self.assertEqual((self.state / "runtime-fas").read_bytes(), (original / "fas.bin").read_bytes())

    def test_failed_pointer_commit_restores_runtime_without_rewriting_history(self):
        original = self.select("A", parent="defaults")
        (self.state / "fail-commit").touch()
        self.save("B", success=False)
        self.assertEqual(self.current(), original)
        self.assertEqual((self.state / "runtime").read_text(), "A")
        self.assertEqual((original / "parent-generation").read_text().strip(), "defaults")

    def test_restore_failure_disables_native_before_confirmed_legacy_restore(self):
        self.select("B")
        original = self.select("A", parent="generation.B")
        (self.state / "suspended-legacy-boot").write_text(Path("/proc/sys/kernel/random/boot_id").read_text())
        (self.state / "fail-load-B").touch()
        (self.state / "fail-load-A").touch()
        self.run_command("rollback", success=False)
        self.assertEqual(self.current(), original)
        self.assertEqual((self.state / "runtime").read_text(), "none")
        self.assertTrue((self.state / "legacy-running").exists())
        calls = (self.state / "calls").read_text().splitlines()
        self.assertLess(max(i for i, c in enumerate(calls) if c == "disable"), calls.index("legacy-start"))

    def test_unconfirmed_handoff_remains_visible_and_never_blindly_restarts(self):
        (self.state / "legacy-running").touch()
        (self.state / "cleanup-blocked").touch()
        self.save("A", success=False)
        self.assertTrue((self.state / "legacy-stop-pending").exists())
        self.assertFalse((self.state / "suspended-legacy-boot").exists())
        self.assertEqual((self.state / "runtime").read_text(), "none")
        self.assertIn("module_handoff=pending", self.run_command("status").stdout)
        self.save("off", success=False)
        self.assertFalse((self.state / "legacy-running").exists())
        (self.state / "cleanup-blocked").unlink()
        self.save("off")
        self.assertFalse((self.state / "legacy-stop-pending").exists())
        self.assertTrue((self.state / "legacy-running").exists())
        self.assertEqual((self.state / "runtime").read_text(), "none")

    def test_unsupported_scene_is_rejected_before_control_mutation(self):
        original = self.select("A")
        (self.state / "features").write_text("cpu,gpu_file_credentials")
        (self.fixtures / "B/scene-required").write_text("1\n")
        self.save("B", success=False)
        self.assertEqual(self.current(), original)
        self.assertEqual((self.state / "runtime").read_text(), "A")
        calls = (self.state / "calls").read_text().splitlines()
        self.assertNotIn("disable", calls)
        self.assertNotIn("suspend-legacy", calls)

    def test_missing_fas_capability_is_rejected_before_stopping_legacy_or_native(self):
        original = self.select("A")
        (self.state / "legacy-running").touch()
        (self.state / "features").write_text("cpu,gpu_file_credentials,game_relations,fps_per_game,scene_table,fas_tuning_pending")
        self.save("B", success=False)
        self.assertEqual(self.current(), original)
        self.assertEqual((self.state / "runtime").read_text(), "A")
        self.assertTrue((self.state / "legacy-running").exists())
        calls = (self.state / "calls").read_text().splitlines()
        self.assertIn("check-fas", calls)
        self.assertNotIn("disable", calls)
        self.assertNotIn("suspend-legacy", calls)

    def test_invalid_fas_blob_is_rejected_without_controller_mutation(self):
        original = self.select("A")
        (self.fixtures / "B/fas.bin").write_bytes(b"")
        self.save("B", success=False)
        self.assertEqual(self.current(), original)
        self.assertEqual((self.state / "runtime-fas").read_bytes(), (original / "fas.bin").read_bytes())
        calls = (self.state / "calls").read_text().splitlines()
        self.assertNotIn("disable", calls)
        self.assertNotIn("suspend-legacy", calls)

    def test_fas_ioctl_failure_restores_entire_previous_generation(self):
        original = self.select("A")
        (self.state / "fail-fas-B").touch()
        self.save("B", success=False)
        self.assertEqual(self.current(), original)
        self.assertEqual((self.state / "runtime").read_text(), "A")
        self.assertEqual((self.state / "runtime-fas").read_bytes(), (original / "fas.bin").read_bytes())
        calls = (self.state / "calls").read_text().splitlines()
        self.assertLess(calls.index("check-fas"), calls.index("suspend-legacy"))
        self.assertIn("load-fas:B", calls)
        self.assertIn("load-fas:A", calls)

    def test_old_generation_without_fas_uses_balance_without_rewriting_source(self):
        source = self.select("B")
        (source / "fas.bin").unlink()
        original = self.select("A", parent="generation.B")
        self.run_command("rollback")
        self.assertEqual((self.state / "runtime-fas").read_bytes(), (self.module / "defaults/fas.bin").read_bytes())
        self.assertEqual((self.current() / "fas.bin").read_bytes(), (self.module / "defaults/fas.bin").read_bytes())
        self.assertFalse((source / "fas.bin").exists())
        self.assertEqual((self.current() / "parent-generation").read_text().strip(), original.name)

    def test_teardown_stops_native_and_waits_for_confirmed_handoff_before_restore(self):
        self.select("A")
        boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        (self.state / "applied-boot").write_text(boot + "\n")
        (self.state / "legacy-stop-pending").write_text("FP_LEGACY_STOP_V1 " + boot + " 100 100\n")
        (self.state / "cleanup-blocked").touch()
        self.run_command("teardown", success=False)
        self.assertEqual((self.state / "runtime").read_text(), "none")
        self.assertTrue((self.state / "legacy-stop-pending").exists())
        self.assertFalse((self.state / "legacy-running").exists())
        (self.state / "cleanup-blocked").unlink()
        self.run_command("teardown")
        self.assertTrue((self.state / "legacy-running").exists())
        self.assertFalse((self.state / "legacy-stop-pending").exists())
        calls = (self.state / "calls").read_text().splitlines()
        self.assertLess(max(i for i, call in enumerate(calls) if call == "disable"), calls.index("legacy-start"))

    def test_boot_selects_current_only_after_the_real_flock_is_held(self):
        self.select("A")
        target = self.state / "generation.B"
        shutil.copytree(self.fixtures / "B", target)
        with (self.state / "config.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            boot = subprocess.Popen(["sh", str(self.module / "config.sh"), "boot"],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, env=self.environment)
            try:
                deadline = time.monotonic() + 4
                while not (self.state / "lock-requested").exists() and time.monotonic() < deadline:
                    time.sleep(0.01)  # Host test synchronization, never module runtime.
                self.assertTrue((self.state / "lock-requested").exists())
                (self.state / "current").write_text("generation.B\n")
                (self.state / "runtime").write_text("B")
                fcntl.flock(lock, fcntl.LOCK_UN)
                stdout, stderr = boot.communicate(timeout=10)
                self.assertEqual(boot.returncode, 0, stdout + stderr)
                self.assertEqual((self.state / "runtime").read_text(), "B")
            finally:
                if boot.poll() is None:
                    boot.kill()
                    boot.communicate()


if __name__ == "__main__":
    unittest.main(verbosity=2)
