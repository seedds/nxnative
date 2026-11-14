"""docs/env_switches.tsv lists every environment switch the code reads (DM_* and the project's other names, such as NXHOST), each with a class and a meaning
(dm/tools/env_switches.py writes it; class and meaning are kept by hand). A switch the code reads that the table does not list, a row for a switch nothing reads
any more, a row without a meaning, a DM_* name the docs quote that nothing reads and the tool's RETIRED does not list, or a doc line that quotes a RETIRED name
without saying it is gone fails here. The other tests check the tool's rules on small texts."""
from pathlib import Path
import subprocess
import sys
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import env_switches as es


class EnvSwitches(unittest.TestCase):
    def test_table_is_current_and_complete(self):
        r = subprocess.run([sys.executable, str(REPO / 'dm/tools/env_switches.py'), '--check'], cwd=REPO, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout, '', 'a warning: ' + r.stdout)

    def test_non_dm_read_forms(self):
        def reads(p, text): return es.file_reads(p, text)[0]
        self.assertEqual(reads('dm/x.c', 'getenv("NXAOT_TRACE"); getenv("HOME"); getenv("VK_ICD_FILENAMES")'), {'NXAOT_TRACE'})
        self.assertEqual(reads('dm/x.cpp', 'std::getenv("NXL_FAKE_HOST")'), {'NXL_FAKE_HOST'})
        self.assertEqual(reads('dm/x.py', 'os.environ.get("J", "8"); os.environ["WORK"]; os.getenv("REGRESS_SKIP"); os.environ["PATH"]'), {'J', 'WORK', 'REGRESS_SKIP'})
        self.assertEqual(reads('dm/x.cs', 'Environment.GetEnvironmentVariable("DMSH_STACK")'), {'DMSH_STACK'})
        self.assertEqual(reads('dm/launcher/x.cpp', 'env.get("NXHOST", &v); env.get("APPDATA", &v)'), {'NXHOST'})
        self.assertEqual(reads('dm/other/x.cpp', 'env.get("NXHOST", &v)'), set())   # env.get is the launcher's Env only
        run_scripts = 'exec "${NXHOST:-$host}"\nif "%NXPACK%"=="" set "NXRUN_AOT=%%c"\necho %NXRUN_AOT%\nstrftime("%Y%m%d")\n${PYTHON:-python3} $ORIGIN'
        self.assertEqual(reads('scripts/x.py', run_scripts), {'NXHOST', 'NXPACK', 'PYTHON'})
        self.assertEqual(reads('dm/tools/x.py', run_scripts), set())   # $NAME and %NAME% count only in the scripts scripts/*.py write

    def test_dm_names_the_code_defines(self):
        _, _, defined = es.file_reads('dm/x.h', '#define DM_GAME_FPS 60\nenum { DM_JIT_GATE = 7,  // a stop\n  DM_JIT_GATE_AT = 0x100 };\n// DM_METAL_CAPTURE=1 makes')
        self.assertEqual(defined, {'DM_GAME_FPS', 'DM_JIT_GATE', 'DM_JIT_GATE_AT'})

    def test_port_switches_have_rows(self):
        rows = {l.split('\t')[0] for l in (REPO / 'docs/env_switches.tsv').read_text().split('\n')[1:] if l}
        for n in ('NXHOST', 'NXPACK', 'NXAOT', 'NXAOT_PROFILE_WEIGHTS', 'NXL_FAKE_HOST'): self.assertIn(n, rows)
        for n in ('PACK_MODE', 'AOT_WORKERS', 'HOME', 'PATH', 'NXRUN_AOT'): self.assertNotIn(n, rows)   # settings-block constants, the OS's, run.cmd's own

    def test_retired(self):
        reads, _ = es.scan()
        for n in es.RETIRED: self.assertNotIn(n, reads, f'{n} is read again')
        self.assertEqual(es.retired_problems(reads), [])
        line = 'with `DM_FRAME_STEP` set'
        self.assertIsNone(es.re.search(es.RETIRED_WORDS, line))
        self.assertIsNotNone(es.re.search(es.RETIRED_WORDS, line + ', which was removed in 79d82f7b'))


if __name__ == '__main__':
    unittest.main()
