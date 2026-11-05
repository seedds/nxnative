"""Compile the production helper and save sync; never touch the user's saves/cloud."""
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[3]
TITLE = "0100000000000000"
HARNESS = r'''
#include "save_cleanup.h"
#include "save_sync.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void put(const char *value) {
    char path[4096];
    snprintf(path, sizeof path, "%s/data", getenv("DM_SAVE"));
    FILE *f = fopen(path, "w");
    if (!f || fputs(value, f) < 0 || fclose(f)) _exit(80);
}
int main(int argc, char **argv) {
    int helper = dm_save_cleanup_main(argc, argv);
    if (helper >= 0) return helper;
    if (argc == 4 && !strcmp(argv[1], "read")) {
        ss_meta meta;
        return ss_read(argv[2], argv[3], &meta) ? 1 : 0;
    }
    if (argc != 2) return 81;
    if (!strcmp(argv[1], "disabled")) {
        unsetenv("DM_SAVE_SYNC");
        if (dm_save_cleanup_start(NULL, NULL)) return 82;
        setenv("DM_SAVE_SYNC", "", 1);
        return dm_save_cleanup_start(NULL, NULL);
    }
    if (!strcmp(argv[1], "failure")) {
        if (dm_save_cleanup_start("/no/such/executable", "0100000000000000") != ENOENT) return 83;
        // Failed starts must not consume the duplicate guard.
    }
    if (dm_save_cleanup_start(argv[0], "0100000000000000")) return 84;
    if (dm_save_cleanup_start("/no/such/executable", "0100000000000000")) return 85;
    if (!strcmp(argv[1], "sender")) {
        save_sync_mount(getenv("DM_SAVE"), "0100000000000000");
        put("sender");
        save_sync_committed();
        if (!save_sync_flush(10000)) return 86;
    }
    put("final");
    // No pipe lifetime may substitute for NOTE_EXIT, even with a live sender.
    for (int fd = 3, max = getdtablesize(); fd < max; fd++) close(fd);
    puts("ready"); fflush(stdout);
    if (!strcmp(argv[1], "immediate")) _exit(23);
    int command = getchar();
    if (command == 'x') _exit(23);
    return command == 'r' ? 0 : 87;
}
'''


@unittest.skipUnless(sys.platform == "darwin", "requires macOS kqueue and CommonCrypto")
class SaveCleanupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="dm-save-cleanup-build-")
        directory = Path(cls.build.name)
        source = directory / "owner.c"
        source.write_text(HARNESS)
        cls.exe = directory / "owner"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wno-deprecated-declarations", "-I", str(ROOT / "dm/mac"),
            "-I", str(ROOT / "dm/host"), str(source),
            str(ROOT / "dm/mac/save_cleanup.c"), str(ROOT / "dm/host/save_sync.c"),
            "-o", str(cls.exe),
        ], check=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dm-save-cleanup-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.save = self.root / "save"
        self.save.mkdir()
        self.cloud = self.root / "cloud"
        self.cloud.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("DM_")}
        self.env.update(DM_SAVE=str(self.save), DM_SAVE_SYNC=str(self.cloud),
                        HOME=str(self.root), TMPDIR=str(self.root))
        self.bundle = self.cloud / TITLE / "save.dmsync"

    def read_bundle(self):
        with tempfile.TemporaryDirectory(dir=self.root) as unpack:
            subprocess.run([str(self.exe), "read", str(self.bundle), unpack],
                           env=self.env, check=True, capture_output=True)
            return (Path(unpack) / "save/data").read_text()

    def owner(self, mode):
        p = subprocess.Popen([str(self.exe), mode], env=self.env,
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, start_new_session=True)
        def cleanup():
            if p.poll() is None:
                p.kill()
            p.communicate(timeout=15)
        self.addCleanup(cleanup)
        stdout = p.stdout
        assert stdout is not None
        self.assertTrue(select.select([stdout], [], [], 15)[0], "owner did not become ready")
        self.assertEqual(stdout.readline(), "ready\n")
        return p

    def finish(self, p, command=None, sig=None, expected=0, group=False):
        if sig:
            if group:
                os.killpg(p.pid, sig)
            else:
                p.send_signal(sig)
        out, err = p.communicate(command, timeout=15)
        self.assertEqual(p.returncode, expected, (out, err))
        # communicate also waits for the helper to close its inherited stdout.
        self.assertTrue(self.bundle.is_file(), err)
        self.assertEqual(self.read_bundle(), "final")

    def test_normal_return(self):
        p = self.owner("normal")
        self.assertFalse(self.bundle.exists())
        self.finish(p, "r", expected=0)

    def test_fatal_exit(self):
        self.finish(self.owner("fatal"), "x", expected=23)

    def test_sigterm(self):
        self.finish(self.owner("signal"), sig=signal.SIGTERM, expected=-signal.SIGTERM)

    def test_group_sigterm(self):
        self.finish(self.owner("signal"), sig=signal.SIGTERM,
                    expected=-signal.SIGTERM, group=True)

    def test_group_sigint(self):
        self.finish(self.owner("signal"), sig=signal.SIGINT,
                    expected=-signal.SIGINT, group=True)

    def test_group_sighup(self):
        self.finish(self.owner("signal"), sig=signal.SIGHUP,
                    expected=-signal.SIGHUP, group=True)

    def test_registration_ready_before_immediate_exit(self):
        self.finish(self.owner("immediate"), expected=23)

    def test_sender_and_closed_pipes_do_not_trigger_live_cleanup(self):
        p = self.owner("sender")
        self.assertEqual(self.read_bundle(), "sender")
        before = self.bundle.read_bytes()
        # Longer than save_sync.c's sender debounce: an early helper would send final.
        time.sleep(3.5)
        self.assertIsNone(p.poll())
        self.assertEqual(self.bundle.read_bytes(), before)
        self.assertEqual((self.save / "data").read_text(), "final")
        self.finish(p, "x", expected=23)

    def test_disabled(self):
        subprocess.run([str(self.exe), "disabled"], env=self.env, check=True, timeout=15)
        self.assertFalse(self.bundle.exists())

    def test_spawn_failure_can_retry(self):
        self.finish(self.owner("failure"), "r")

    def test_malformed_helper_invocation(self):
        p = subprocess.run([str(self.exe), "--dm-save-cleanup"], env=self.env, timeout=15)
        self.assertEqual(p.returncode, 1)
        self.assertFalse(self.bundle.exists())


if __name__ == "__main__":
    unittest.main()
