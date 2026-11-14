"""scripts/port_docker.sh with a stand-in `docker` and `df` on PATH: which images it builds, skips, rebuilds and removes, its free-space stop,
the order of `verify --low-disk`, the static or shared nxaot for the host stage, and that `clean` touches only the images it labelled.
No Docker daemon is used: the stand-in keeps its images in a JSON file and logs every call."""
import hashlib
import json
import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / "scripts" / "port_docker.sh"
BY = "org.nxnative.port-docker.built-by"
SHA = "org.nxnative.port-docker.dockerfile-sha256"

FAKE_DOCKER = r'''#!/usr/bin/env python3
import json, os, sys
st = os.environ["FAKE_STATE"]
images_path = os.path.join(st, "images.json")
images = json.load(open(images_path))
free_path = os.path.join(st, "free_kb")
a = sys.argv[1:]
def save():
    json.dump(images, open(images_path, "w"))
def add_free(kb):
    free = int(open(free_path).read()) + kb
    open(free_path, "w").write(str(free))
with open(os.path.join(st, "calls.log"), "a") as f:
    f.write(" ".join(a) + "\t" + ",".join(sorted(images)) + "\n")
def opt(name):
    return a[a.index(name) + 1] if name in a else None
if a[:1] == ["info"]:
    print("/fake/docker-root")
elif a[:2] == ["image", "inspect"]:
    name = a[-1]
    if name not in images:
        sys.exit(1)
    fmt = opt("--format")
    if "Labels" in fmt:
        print(json.dumps(images[name]["labels"]) if images[name]["labels"] else "null")
    else:
        print(images[name]["mb"] * 1048576)
elif a[:1] == ["build"]:
    if os.environ.get("FAKE_BUILD_FAIL"):
        sys.exit(1)
    labels = dict(a[i + 1].split("=", 1) for i, x in enumerate(a) if x == "--label")
    mb = int(os.environ.get("FAKE_IMAGE_MB", "1800"))
    images[opt("-t")] = {"labels": labels, "mb": mb}
    save()
    add_free(-mb * 1024)
elif a[:2] == ["builder", "prune"]:
    pass
elif a[:2] == ["image", "rm"]:
    name = a[2][:-7] if a[2].endswith(":latest") else a[2]
    if name not in images:
        sys.exit(1)
    add_free(images.pop(name)["mb"] * 1024)
    save()
elif a[:2] == ["image", "ls"]:
    filters = [a[i + 1] for i, x in enumerate(a) if x == "--filter"]
    if "dangling=true" in filters:
        sys.exit(0)
    fmt = opt("--format") or ""
    for name, im in images.items():
        ok = True
        for flt in filters:
            if flt.startswith("label="):
                k, _, v = flt[6:].partition("=")
                ok = ok and im["labels"].get(k) == v
        if ok:
            tag = name if ":" in name else name + ":latest"
            print(tag + (" (%d MB)" % im["mb"] if "Size" in fmt else ""))
elif a[:1] == ["run"]:
    joined = " ".join(a)
    if "ldconfig" in joined:
        if os.environ.get("FAKE_LIBLLVM") == "1":
            print("\tlibLLVM.so.18.1 (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/libLLVM.so.18.1")
            sys.exit(0)
        sys.exit(1)
    work = next(v.split(":")[0] for i, v in enumerate(a) if a[i - 1] == "-v" and v.endswith(":/work"))
    mode = a[a.index("/work/inner.sh") + 1]
    fail = os.environ.get("FAKE_FAIL_STEP", "")
    steps = {"aot": ["aot-configure", "aot-build", "packs", "nxaot-copy", "aot-ctest"],
             "aot-static": ["nxaot-static-configure", "nxaot-static-build", "nxaot-static-copy"],
             "host": ["host-nxaot-libs", "host-configure", "host-build", "host-ctest", "host-ctest-arena"]}[mode]
    with open(os.path.join(work, "results.tsv"), "a") as r:
        for s in steps:
            rc = 8 if s == fail else 0
            r.write("%s\t%d\t1\n" % (s, rc))
            with open(os.path.join(work, "logs", s + ".log"), "w") as log:
                if "ctest" in s:
                    log.write("100% tests passed, 0 tests failed out of 27\n" if rc == 0 else
                              "96% tests passed, 1 tests failed out of 27\nThe following tests FAILED:\n\t 3 - t_x (Failed)\n")
    if mode != "host":
        open(os.path.join(work, "nxaot"), "w").write("")
else:
    sys.exit("fake docker: unexpected call: " + " ".join(a))
'''

FAKE_DF = r'''#!/bin/sh
echo "Filesystem 1024-blocks Used Available Capacity Mounted on"
echo "/dev/fake 40000000 30000000 $(cat "$FAKE_STATE/free_kb") 99% /"
'''


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class PortDocker(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="port_docker_test."))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        # a repository skeleton: the script and the two Dockerfiles (copies, so a test can change one)
        self.repo = self.tmp / "repo"
        (self.repo / "scripts").mkdir(parents=True)
        (self.repo / "docker").mkdir()
        shutil.copy(SCRIPT, self.repo / "scripts" / "port_docker.sh")
        for n in ("Dockerfile.build", "Dockerfile.aot"):
            shutil.copy(REPO / "docker" / n, self.repo / "docker" / n)
        self.state = self.tmp / "state"
        self.state.mkdir()
        bindir = self.tmp / "bin"
        bindir.mkdir()
        for name, text in (("docker", FAKE_DOCKER), ("df", FAKE_DF)):
            p = bindir / name
            p.write_text(text)
            p.chmod(p.stat().st_mode | stat.S_IXUSR)
        self.env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ["PATH"], FAKE_STATE=str(self.state),
                        TMPDIR=str(self.tmp / "tmpdir"))
        for k in ("PORT_DOCKER_MARGIN_MB", "PORT_DOCKER_BUILD_MB", "PORT_DOCKER_AOT_MB"):
            self.env.pop(k, None)
        self.work = self.tmp / "work"
        self.set_images({})
        self.set_free_mb(10000)

    def set_images(self, images):
        (self.state / "images.json").write_text(json.dumps(images))

    def images(self):
        return json.loads((self.state / "images.json").read_text())

    def set_free_mb(self, mb):
        (self.state / "free_kb").write_text(str(mb * 1024))

    def ours(self, dockerfile, mb=1800):
        return {"labels": {BY: "port_docker.sh", SHA: sha(self.repo / "docker" / dockerfile)}, "mb": mb}

    def run_script(self, *args, **env):
        e = dict(self.env, **env)
        return subprocess.run(["sh", str(self.repo / "scripts" / "port_docker.sh"), *args], env=e, capture_output=True, text=True, timeout=60)

    def calls(self):
        p = self.state / "calls.log"
        return [line.split("\t") for line in p.read_text().splitlines()] if p.exists() else []

    def commands(self, prefix):
        return [c for c, _ in self.calls() if c.startswith(prefix)]

    def test_syntax(self):
        self.assertEqual(subprocess.run(["sh", "-n", str(SCRIPT)]).returncode, 0)

    def test_usage(self):
        r = self.run_script()
        self.assertEqual(r.returncode, 2)
        self.assertIn("usage: scripts/port_docker.sh", r.stderr)
        self.assertEqual(self.run_script("images", "nope").returncode, 2)

    def test_missing_image_builds_labelled(self):
        r = self.run_script("images", "build")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        builds = self.commands("build ")
        self.assertEqual(len(builds), 1)
        self.assertIn("--label %s=%s" % (SHA, sha(self.repo / "docker" / "Dockerfile.build")), builds[0])
        self.assertIn("--label %s=port_docker.sh" % BY, builds[0])
        self.assertIn("-t nxnative-build", builds[0])
        self.assertEqual(len(self.commands("builder prune")), 1)
        self.assertEqual(list(self.images()), ["nxnative-build"])

    def test_current_image_skips(self):
        self.set_images({"nxnative-build": self.ours("Dockerfile.build"), "nxnative-aot": self.ours("Dockerfile.aot")})
        r = self.run_script("images", "all")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.commands("build "), [])
        self.assertEqual(self.commands("image rm"), [])
        self.assertIn("nxnative-build: current", r.stdout)

    def test_unlabelled_image_is_used_not_rebuilt(self):
        self.set_images({"nxnative-build": {"labels": {}, "mb": 1800}})
        r = self.run_script("images", "build")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.commands("build "), [])
        self.assertIn("built outside this script", r.stdout)

    def test_changed_dockerfile_rebuilds(self):
        self.set_images({"nxnative-aot": self.ours("Dockerfile.aot")})
        with open(self.repo / "docker" / "Dockerfile.aot", "a") as f:
            f.write("# changed\n")
        r = self.run_script("images", "aot")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        cmds = [c for c, _ in self.calls()]
        rm = cmds.index("image rm nxnative-aot")
        build = next(i for i, c in enumerate(cmds) if c.startswith("build "))
        self.assertLess(rm, build)  # the stale image goes first, so its space counts
        self.assertEqual(self.images()["nxnative-aot"]["labels"][SHA], sha(self.repo / "docker" / "Dockerfile.aot"))

    def test_low_space_stops_with_message(self):
        self.set_images({"other/project:1": {"labels": {}, "mb": 700}, "nxnative-aot": self.ours("Dockerfile.aot")})
        self.set_free_mb(1500)
        r = self.run_script("images", "build")
        self.assertEqual(r.returncode, 3)
        self.assertIn("[RED] not enough disk space to build nxnative-build", r.stderr)
        self.assertIn("1500 MB free", r.stderr)
        self.assertIn("2924 MB", r.stderr)  # 1900 MB image + 1024 MB margin
        self.assertIn("nxnative-aot:latest", r.stderr)  # ours, with the command that removes it
        self.assertIn("scripts/port_docker.sh clean", r.stderr)
        self.assertIn("other/project:1", r.stderr)
        self.assertEqual(self.commands("build "), [])
        self.assertEqual(self.commands("image rm"), [])
        self.assertEqual(self.run_script("images", "build", PORT_DOCKER_MARGIN_MB="0", PORT_DOCKER_BUILD_MB="1400").returncode, 0)

    def test_status(self):
        self.set_images({"nxnative-build": self.ours("Dockerfile.build")})
        self.set_free_mb(3500)
        r = self.run_script("status", "--work", str(self.work))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stdout, r"nxnative-build +current, 1800 MB")
        self.assertRegex(r.stdout, r"nxnative-aot +missing")
        self.assertIn("fits: verify\n", r.stdout)

    def test_verify_low_disk_order(self):
        self.set_free_mb(3200)  # room for one image (+ margin), not two
        r = self.run_script("verify", "--low-disk", "--work", str(self.work))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        seq = []
        for cmd, held in self.calls():
            for kind, key in (("build ", "-t "), ("run ", None), ("image rm ", None)):
                if cmd.startswith(kind):
                    seq.append((kind.strip(), cmd, held))
        steps = [(k, ("inner.sh " + c.split("inner.sh ")[1].split()[0]) if "inner.sh" in c else os.path.basename(c.split()[-1])) for k, c, _ in seq]
        self.assertEqual(steps, [("build", "docker"), ("run", "inner.sh aot"), ("image rm", "nxnative-aot"),
                                 ("build", "docker"), ("run", "inner.sh host")])
        self.assertIn("-t nxnative-aot", seq[0][1])
        self.assertIn("-t nxnative-build", seq[3][1])
        for _, cmd, held in seq:  # never both images at once
            self.assertNotEqual(held, "nxnative-aot,nxnative-build", cmd)
        # the build image did not exist at the decision: shared nxaot, from Dockerfile.build's packages
        self.assertIn("nxaot for the host stage: shared", r.stdout)
        self.assertIn("llvm-18", r.stdout)
        self.assertIn("total: 10 passed, 0 failed, 0 not run", r.stdout)
        run = seq[1][1]
        self.assertIn("-u %d:%d -e HOME=/tmp -v %s:/src -w /src --tmpfs /src/build:exec,uid=%d,gid=%d -v %s:/work"
                      % (os.getuid(), os.getgid(), self.repo, os.getuid(), os.getgid(), self.work), run)
        self.assertTrue((self.repo / "build").is_dir())
        self.assertTrue((self.work / "packs").is_dir())

    def test_verify_low_disk_keeps_aot_image_it_did_not_build(self):
        self.set_images({"nxnative-aot": self.ours("Dockerfile.aot")})
        r = self.run_script("verify", "--low-disk", "--work", str(self.work))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.commands("image rm"), [])
        self.assertIn("was not built by this run: kept", r.stdout)

    def test_verify_static_nxaot_without_libllvm(self):
        self.set_images({"nxnative-build": self.ours("Dockerfile.build"), "nxnative-aot": self.ours("Dockerfile.aot")})
        r = self.run_script("verify", "--work", str(self.work), FAKE_LIBLLVM="0")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("static, -DNXAOT_LLVM_STATIC=ON (nxnative-build's ldconfig has no libLLVM.so.18.1)", r.stdout)
        modes = [c.split("inner.sh ")[1].split()[0] for c in self.commands("run ") if "inner.sh" in c]
        self.assertEqual(modes, ["aot", "aot-static", "host"])
        self.assertEqual(self.commands("build "), [])
        r = self.run_script("verify", "--work", str(self.work), FAKE_LIBLLVM="1")
        self.assertIn("shared (nxnative-build's ldconfig lists libLLVM.so.18.1)", r.stdout)

    def test_verify_failure_exits_nonzero(self):
        self.set_images({"nxnative-build": self.ours("Dockerfile.build"), "nxnative-aot": self.ours("Dockerfile.aot")})
        r = self.run_script("verify", "--work", str(self.work), FAKE_LIBLLVM="1", FAKE_FAIL_STEP="host-ctest-arena")
        self.assertEqual(r.returncode, 1)
        self.assertIn("[RED] FAIL host-ctest-arena (exit 8", r.stderr)
        self.assertIn("1 tests failed out of 27", r.stdout)
        self.assertIn("total: 9 passed, 1 failed, 0 not run", r.stdout)

    def test_clean_only_labelled(self):
        self.set_images({"nxnative-build": self.ours("Dockerfile.build"), "nxnative-aot": {"labels": {}, "mb": 1900},
                         "other/project:1": {"labels": {"org.example": "x"}, "mb": 500}})
        self.work.mkdir()
        (self.work / ".port_docker").write_text("")
        (self.work / "packs").mkdir()
        r = self.run_script("clean", "--work", str(self.work))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.commands("image rm"), ["image rm nxnative-build:latest"])
        self.assertEqual(sorted(self.images()), ["nxnative-aot", "other/project:1"])
        self.assertFalse(self.work.exists())
        self.assertFalse([c for c, _ in self.calls() if c.startswith(("container", "volume", "rm ", "system"))])
        # a folder the script did not make stays
        other = self.tmp / "mine"
        other.mkdir()
        r = self.run_script("clean", "--work", str(other))
        self.assertTrue(other.exists())
        self.assertIn("left alone", r.stderr)


if __name__ == "__main__":
    unittest.main()
