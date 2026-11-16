"""dm/host/tests/run_wine_tests.sh with a stand-in `docker` on PATH: its usage errors, the container it asks for (its own paths, the run directory Wine needs, the
packs read-only, DM_REPO), and the summary it makes of run_host_tests.sh's: the "Wine, not Windows" lines first, a FAIL whose every failed check a known-difference row
explains (with that row's evidence in the log) as DIFFERS, any other failure kept as a FAIL, a pass that skipped its checks as SKIP, and the exit status.
Then dm/host/tests/run_host_tests.sh's Wine switches on a stand-in Windows tree, whose programs are shell scripts run through a stand-in `wine`: WINE runs each program
through it with Z: paths, NOT_RUN keeps a run from starting, and without them a tree runs as before. No Docker daemon and no Wine are used."""
import json
import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
RUNNER = REPO / "dm" / "host" / "tests" / "run_wine_tests.sh"
HOST_RUNNER = REPO / "dm" / "host" / "tests" / "run_host_tests.sh"

FAKE_DOCKER = r'''#!/usr/bin/env python3
import json, os, sys
a = sys.argv[1:]
st = os.environ["FAKE_STATE"]
with open(os.path.join(st, "calls.json"), "a") as f:
    f.write(json.dumps(a) + "\n")
if a[:2] == ["image", "inspect"]:
    sys.exit(1 if os.environ.get("FAKE_NO_IMAGE") else 0)
if a[:1] != ["run"]:
    sys.exit("fake docker: unexpected call: " + " ".join(a))
i = a.index("inner")
repo, build, logs, packs, known, limit = a[i + 1:i + 7]
scenario = json.load(open(os.environ["FAKE_SCENARIO"]))
lines = []
for name, (result, body) in scenario.items():
    with open(os.path.join(logs, name + ".log"), "w") as log:
        log.write("date: x\ncommit: y\ncommand: z\n\n" + body + "\nexit: 0 (0 s)\n")
    lines.append(result)
with open(os.path.join(logs, "summary.txt"), "w") as s:
    s.write("date: x\ncommit: y\ncommand: run_host_tests.sh\nplatform: win64\n\n" + "\n".join(lines) + "\n\n%d runs: 0 passed, 0 failed\n0 script tests: 0 passed, 0 failed, 0 skipped\n" % len(lines))
open(os.path.join(logs, ".wine_version"), "w").write("wine-9.0 (fake)\n")
sys.exit(1 if any(l.startswith(("FAIL", "TIMEOUT")) for l in lines) else 0)
'''

KNOWN = (
    "# test\tkind\tcheck\tevidence\treason\tversions\n"
    "t_tz\tdiffers\tthe zone name\t-\t_tzset keeps two letters\twine-9.0 wine-11.0\n"
    "t_dev\tdiffers\tdevice up\tlacks host allocations\twinevulkan reports no import\twine-9.0\n"
    "t_crash\tdiffers\t-\tstub!\ta stub that crashes it\tany\n"
    "t_new\tdiffers\tthe zone name\t-\ta row of another Wine\twine-11.0\n"   # the fake run is wine-9.0: this row does not apply
    "t_py\tnot-run\t-\t-\tneeds a Windows Python\tany\n"
)


class RunWineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="run_wine_tests_test."))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        # a repository skeleton (the runner finds the table beside itself and the repository three levels up), a Windows tree and a packs folder
        self.repo = self.tmp / "repo"
        tests = self.repo / "dm" / "host" / "tests"
        tests.mkdir(parents=True)
        shutil.copy(RUNNER, tests / "run_wine_tests.sh")
        (tests / "wine_differences.tsv").write_text(KNOWN)
        self.build = self.tmp / "win64"
        self.build.mkdir()
        (self.build / "host_tests.tsv").write_text("# list\n# platform: win64\nt_a\tt_a.exe\t-\t-\n")
        self.packs = self.tmp / "packs"
        self.packs.mkdir()
        self.logs = self.tmp / "logs"
        self.state = self.tmp / "state"
        self.state.mkdir()
        bindir = self.tmp / "bin"
        bindir.mkdir()
        p = bindir / "docker"
        p.write_text(FAKE_DOCKER)
        p.chmod(p.stat().st_mode | stat.S_IXUSR)
        self.env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ["PATH"], FAKE_STATE=str(self.state), FAKE_SCENARIO=str(self.tmp / "scenario.json"))
        self.env.pop("NXAOT_PACKS", None)

    def run_runner(self, scenario, *args, **env):
        (self.tmp / "scenario.json").write_text(json.dumps(scenario))
        cmd = ["sh", str(self.repo / "dm" / "host" / "tests" / "run_wine_tests.sh")] + list(args)
        return subprocess.run(cmd, env=dict(self.env, **env), capture_output=True, text=True, timeout=60)

    def calls(self):
        f = self.state / "calls.json"
        return [json.loads(l) for l in f.read_text().splitlines()] if f.exists() else []

    def summary(self):
        return (self.logs / "summary.txt").read_text()

    def test_syntax(self):
        for script in (RUNNER, HOST_RUNNER):
            self.assertEqual(subprocess.run(["sh", "-n", str(script)]).returncode, 0, script)

    def test_usage(self):
        r = self.run_runner({}, "--bogus", str(self.build))
        self.assertEqual(r.returncode, 2)
        self.assertIn("usage:", r.stderr)
        self.assertEqual(self.run_runner({}).returncode, 2)
        self.assertEqual(self.run_runner({}, "--packs").returncode, 2)
        self.assertEqual(self.run_runner({}, str(self.tmp / "none")).returncode, 2)

    def test_linux_tree_refused(self):
        (self.build / "host_tests.tsv").write_text("# list\n# platform: linux\n")
        r = self.run_runner({}, str(self.build), str(self.logs))
        self.assertEqual(r.returncode, 2)
        self.assertIn("not a Windows tree", r.stderr)
        self.assertEqual([c for c in self.calls() if c[0] == "run"], [])

    def test_missing_image(self):
        r = self.run_runner({}, str(self.build), str(self.logs), FAKE_NO_IMAGE="1")
        self.assertEqual(r.returncode, 2)
        self.assertIn("docker build -t nxnative-wine -f docker/Dockerfile.wine docker", r.stderr)

    def test_container(self):
        self.run_runner({"t_a": ["PASS t_a (0 s)", "t_a: 3 checks, 0 failed"]}, "--packs", str(self.packs), str(self.build), str(self.logs))
        run = next(c for c in self.calls() if c[0] == "run")
        uid = os.getuid()
        self.assertIn("--rm", run)
        self.assertIn("%d:%d" % (uid, os.getgid()), run)
        self.assertIn("/run/user/%d:uid=%d,gid=%d,mode=700" % (uid, uid, os.getgid()), run)
        self.assertIn("DM_REPO=Z:%s" % self.repo, run)
        for folder in (self.repo, self.build, self.logs):
            self.assertIn("%s:%s" % (folder, folder), run)
        self.assertIn("%s:%s:ro" % (self.packs, self.packs), run)
        self.assertEqual(run[run.index("-w") + 2], "nxnative-wine")
        i = run.index("inner")
        self.assertEqual(run[i + 1:i + 7], [str(self.repo), str(self.build), str(self.logs), str(self.packs),
                                             str(self.repo / "dm/host/tests/wine_differences.tsv"), "120"])

    def test_summary_classes(self):
        scenario = {
            "t_ok": ["PASS t_ok (0 s)", "t_ok: 5 checks, 0 failed"],
            "t_skip": ["PASS t_skip (0 s)", "== t_skip: 0 checks, 0 failed (skipped: no Vulkan device) =="],
            "t_tz": ["FAIL t_tz (exit 1, 0 s; see t_tz.log)", "0123:fixme:thread:Something FAIL-looking stub\nok   a\nFAIL: the zone name\nt_tz: 2 checks, 1 failed"],
            "t_tz2": ["FAIL t_tz2 (exit 1, 0 s; see t_tz2.log)", "FAIL: the zone name\n"],
            "t_dev": ["FAIL t_dev (exit 1, 0 s; see t_dev.log)", "[RED] device 0 lacks host allocations importable\ndevice up    ✗\n"],
            "t_crash": ["FAIL t_crash (exit 5, 0 s; see t_crash.log)", "0099:fixme:x:Y stub!\nwine: Unhandled page fault\n"],
            "t_py": ["NOT RUN t_py: needs a Windows Python", ""],
            "t_new": ["FAIL t_new (exit 1, 0 s; see t_new.log)", "FAIL: the zone name\n"],
        }
        r = self.run_runner(scenario, str(self.build), str(self.logs))
        s = self.summary()
        first = s.splitlines()[:2]
        self.assertTrue(first[0].startswith("Wine, not Windows: "), first[0])
        self.assertIn("wine-9.0 (fake)", first[0])
        self.assertEqual(first[1], "wine: wine-9.0 (fake)")
        self.assertIn("\nPASS t_ok (0 s)\n", s)
        self.assertIn('\nSKIP t_skip (0 s): "== t_skip: 0 checks, 0 failed (skipped: no Vulkan device) =="\n', s)
        self.assertIn("\nDIFFERS t_tz (exit 1, 0 s; see t_tz.log): under Wine: _tzset keeps two letters (holds for wine-9.0 wine-11.0)\n", s)
        self.assertIn("\nFAIL t_tz2 (", s)   # no row names this test
        self.assertIn("\nDIFFERS t_dev (", s)   # the check line and its evidence on another line
        self.assertIn("\nDIFFERS t_crash (", s)   # no failed check line, the evidence of a check - row (a row for any Wine)
        self.assertIn("\nFAIL t_new (", s)   # its only row holds for another Wine (wine-11.0), not this run's wine-9.0
        self.assertIn("\nNOT RUN t_py: needs a Windows Python\n", s)
        self.assertIn("8 tests: 1 passed, 2 failed, 3 differ under Wine, 1 skipped, 1 not run", s)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertFalse((self.logs / ".wine_version").exists())

    def test_new_failure_stays(self):
        scenario = {
            "t_tz": ["FAIL t_tz (exit 1, 0 s; see t_tz.log)", "FAIL: the zone name\nFAIL: something new\n"],
            "t_dev": ["FAIL t_dev (exit 1, 0 s; see t_dev.log)", "device up    ✗\n"],   # the check without its evidence
        }
        r = self.run_runner(scenario, str(self.build), str(self.logs))
        s = self.summary()
        self.assertIn("\nFAIL t_tz (", s)
        self.assertIn("\nFAIL t_dev (", s)
        self.assertIn("2 failed, 0 differ under Wine", s)
        self.assertEqual(r.returncode, 1)

    def test_only_differences_pass(self):
        scenario = {"t_ok": ["PASS t_ok (0 s)", "fine"], "t_tz": ["FAIL t_tz (exit 1, 0 s; see t_tz.log)", "FAIL: the zone name\n"]}
        r = self.run_runner(scenario, str(self.build), str(self.logs))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("1 passed, 0 failed, 1 differ under Wine", self.summary())


FAKE_PROGRAM = r'''#!/bin/sh
echo "$(basename "$0") args: $*"
echo "TMPDIR=${TMPDIR:-unset} WINETMPDIR=${WINETMPDIR:-unset} NXAOT_PACKS=${NXAOT_PACKS:-unset}"
'''

FAKE_WINE = r'''#!/bin/sh
echo "through wine"
exec sh "$@"
'''


class HostRunnerWineSwitch(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="run_host_tests_wine."))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.build = self.tmp / "win64"
        self.build.mkdir()
        for n in ("t_a.exe", "t_b.exe", "t_aot_targets.exe"):
            p = self.build / n
            p.write_text(FAKE_PROGRAM)
            p.chmod(p.stat().st_mode | stat.S_IXUSR)
        self.wine = self.tmp / "wine"
        self.wine.write_text(FAKE_WINE)
        self.wine.chmod(self.wine.stat().st_mode | stat.S_IXUSR)
        self.logs = self.tmp / "logs"
        self.env = dict(os.environ)
        for k in ("WINE", "NOT_RUN", "NXAOT_PACKS", "TIMEOUT"):
            self.env.pop(k, None)

    def tree(self, platform):
        (self.build / "host_tests.tsv").write_text(
            "# list\n# platform: %s\nt_a\tt_a.exe\t-\ttmpdir\nt_b\tt_b.exe\t--flag\t-\nt_aot_targets\tt_aot_targets.exe\t-\t-\n" % platform)

    def run_host(self, **env):
        return subprocess.run(["sh", str(HOST_RUNNER), str(self.build), str(self.logs)], env=dict(self.env, **env), capture_output=True, text=True, timeout=60)

    def log(self, name):
        return (self.logs / (name + ".log")).read_text()

    def test_wine_switch(self):
        self.tree("win64")
        packs = self.tmp / "packs"
        packs.mkdir()
        not_run = self.tmp / "not_run.tsv"
        not_run.write_text("t_b\tnot under the stand-in\n")
        r = self.run_host(WINE=str(self.wine), NOT_RUN=str(not_run), NXAOT_PACKS=str(packs))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        a = self.log("t_a")
        self.assertIn("through wine\nt_a.exe args: \n", a)
        # WINETMPDIR beside TMPDIR: Wine 11 gives a program TMPDIR only from WINETMPDIR (a Unix TMPDIR becomes WINE_HOST_TMPDIR)
        self.assertIn("TMPDIR=Z:%s/tmp WINETMPDIR=Z:%s/tmp NXAOT_PACKS=Z:%s" % (self.build, self.build, packs), a)
        self.assertIn("command: cd Z:%s && TMPDIR=Z:%s/tmp WINETMPDIR=Z:%s/tmp " % (self.build, self.build, self.build), a)
        self.assertIn("WINETMPDIR=unset", self.log("t_aot_targets"))   # a test without the tmpdir column gets neither
        self.assertIn(" %s ./t_a.exe" % self.wine, a)
        self.assertIn("t_aot_targets.exe args: --corpus Z:%s/dm/tools/aot/corpus" % REPO, self.log("t_aot_targets"))
        self.assertFalse((self.logs / "t_b.log").exists())
        s = (self.logs / "summary.txt").read_text()
        self.assertIn("\nNOT RUN t_b: not under the stand-in\n", s)
        self.assertIn("wine: %s (WINE): the programs ran under Wine\n" % self.wine, s)
        self.assertIn("packs: Z:%s\n" % packs, s)
        self.assertIn("2 runs: 2 passed, 0 failed", s)
        self.assertIn("1 not run (NOT_RUN)", s)

    def test_linux_tree_ignores_wine(self):
        # WINE applies to a Windows tree only: a Linux tree's programs run directly, with Unix paths, and the summary has no wine line
        self.tree("linux")
        r = self.run_host(WINE=str(self.wine))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        a = self.log("t_a")
        self.assertNotIn("through wine", a)
        self.assertIn("TMPDIR=%s/tmp " % self.build, a); self.assertIn("WINETMPDIR=unset", a)   # no WINETMPDIR outside Wine
        s = (self.logs / "summary.txt").read_text()
        self.assertNotIn("wine:", s)
        self.assertNotIn("NOT RUN", s)
        self.assertIn("3 runs: 3 passed, 0 failed\n0 script tests: 0 passed, 0 failed, 0 skipped\n", s)


if __name__ == "__main__":
    unittest.main()
