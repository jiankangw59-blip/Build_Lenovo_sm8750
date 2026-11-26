"""Exercise production PIDFD handoff with real private Linux child processes.

Compile-time path overrides keep every file inside a temporary directory. The
real client writes/validates the record, sends SIGTERM and waits on a pidfd;
neither Android processes nor /data/adb are accessed.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SUPERVISOR = r'''
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
 sigset_t signals; int received;
 if (argc != 3) return 2;
 sigemptyset(&signals); sigaddset(&signals, SIGTERM);
 if (sigprocmask(SIG_BLOCK, &signals, NULL)) return 3;
 puts("ready"); fflush(stdout);
 if (sigwait(&signals, &received)) return 4;
 FILE *result = fopen(argv[2], "w");
 if (!result) return 5;
 fprintf(result, "%d\n", access(argv[1], F_OK) == 0); fclose(result);
 return 0;
}
'''


class Handoff(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="frame-handoff-")
        cls.root = Path(cls.temporary.name)
        cls.state, cls.events = cls.root / "state", cls.root / "events"
        cls.state.mkdir(mode=0o700)
        cls.events.mkdir()
        (cls.events / "run").mkdir()
        cls.fake = cls.root / "fas-eventd"
        source = cls.root / "supervisor.c"
        source.write_text(SUPERVISOR)
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(cls.fake)], check=True)
        cls.controller = cls.root / "framectl"
        definitions = [f'-DLEGACY_STATE="{cls.state}"', f'-DLEGACY_EVENTS="{cls.events}"',
                       f'-DLEGACY_EXECUTABLE="{cls.fake}"']
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", *definitions, str(HERE.parent / "framectl.c"),
                        "-o", str(cls.controller)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.children = []
        for directory in (self.state, self.events / "run"):
            for path in directory.iterdir():
                path.unlink()
        fault = self.events / "cleanup-unconfirmed"
        fault.unlink(missing_ok=True)
        self.pending = self.state / "legacy-stop-pending"
        self.boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()

    def tearDown(self):
        for child in self.children:
            if child.poll() is None:
                child.terminate()
            child.communicate(timeout=4)

    def supervisor(self):
        result = self.state / ("signal-observed-" + str(len(self.children)))
        child = subprocess.Popen([str(self.fake), str(self.pending), str(result)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.children.append(child)
        self.assertEqual(child.stdout.readline().strip(), "ready")
        (self.events / "run/pid").write_text(str(child.pid) + "\n")
        return child, result

    def record(self, child, boot=None):
        # Independent identity source: Linux stat's starttime field, not a PID
        # alone. This is only for exercising recovery of an existing record.
        tail = Path(f"/proc/{child.pid}/stat").read_text().rsplit(")", 1)[1].split()
        self.pending.write_text(f"FP_LEGACY_STOP_V1 {boot or self.boot} {child.pid} {tail[19]}\n")

    def command(self, name, success=True):
        result = subprocess.run([str(self.controller), name], capture_output=True, text=True, timeout=5)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def test_record_is_published_before_real_sigterm_and_clean_is_read_only(self):
        child, observed = self.supervisor()
        self.assertIn("legacy_was_running=1", self.command("suspend-legacy").stdout)
        child.communicate(timeout=3)
        self.assertEqual(observed.read_text().strip(), "1")
        content = self.pending.read_bytes()
        self.assertIn(f"{self.boot} {child.pid} ".encode(), content)
        self.assertEqual(self.pending.stat().st_mode & 0o777, 0o600)
        self.assertIn("legacy_was_running=1", self.command("legacy-clean").stdout)
        self.assertEqual(self.pending.read_bytes(), content)

    def test_worker_journal_retains_pending_until_cleanup_is_confirmed(self):
        child, _ = self.supervisor()
        journal = self.events / "run/worker-pending"
        journal.write_text("nonce: not acknowledged")
        self.command("suspend-legacy", success=False)
        child.communicate(timeout=3)
        self.assertTrue(self.pending.exists())
        self.command("legacy-clean", success=False)
        journal.unlink()
        self.assertIn("legacy_was_running=1", self.command("legacy-clean").stdout)

    def test_pending_identity_blocks_clean_even_if_pidfile_is_missing(self):
        child, _ = self.supervisor()
        self.record(child)
        (self.events / "run/pid").unlink()
        self.command("legacy-clean", success=False)
        self.assertIsNone(child.poll())
        child.terminate()
        child.communicate(timeout=3)
        self.command("legacy-clean")

    def test_explicit_retry_stops_only_the_original_recorded_identity(self):
        child, observed = self.supervisor()
        self.record(child)
        identity = self.pending.read_bytes()
        self.command("suspend-legacy")
        child.communicate(timeout=3)
        self.assertEqual(observed.read_text().strip(), "1")
        self.assertEqual(self.pending.read_bytes(), identity)

    def test_previous_boot_record_is_archived_before_a_new_handoff(self):
        child, observed = self.supervisor()
        self.record(child, "00000000-0000-0000-0000-000000000000")
        old = self.pending.read_bytes()
        self.command("suspend-legacy")
        child.communicate(timeout=3)
        self.assertEqual((self.state / "legacy-stop-pending.previous-boot").read_bytes(), old)
        self.assertIn(self.boot.encode(), self.pending.read_bytes())
        self.assertEqual(observed.read_text().strip(), "1")

    def test_fault_latch_refuses_stop_and_leaves_running_supervisor_intact(self):
        child, _ = self.supervisor()
        (self.events / "cleanup-unconfirmed").touch()
        self.command("suspend-legacy", success=False)
        self.assertIsNone(child.poll())
        self.assertFalse(self.pending.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
