"""Offline applet close requests, using the host's injected JavaScript and no game files."""
import json
from pathlib import Path
import shutil
import subprocess
import unittest


@unittest.skipUnless(shutil.which('node'), 'Node.js is required to execute the web shim')
class WebShimTests(unittest.TestCase):
    def bridge(self, action):
        source = (Path(__file__).resolve().parents[2] / 'host/web_apple.m').read_text()
        # The bridge precedes DOM styling and input setup. Execute that actual prefix in a window context.
        script = source.split('@R"JS(', 1)[1].split('// Sounds follow the host', 1)[0] + '})();'
        harness = """
const vm = require('vm');
const messages = [];
let nativeCloses = 0;
const page = {webkit: {messageHandlers: {dm: {postMessage: m => messages.push(m)}}},
              addEventListener: () => {}, close: () => nativeCloses++,
              console: {log: () => {}, warn: () => {}, error: () => {}}};
page.window = page;
vm.createContext(page);
vm.runInContext(SCRIPT, page);
const before = messages.length;
vm.runInContext(ACTION, page);
process.stdout.write(JSON.stringify({before, messages, nativeCloses}));
""".replace('SCRIPT', json.dumps(script)).replace('ACTION', json.dumps(action))
        run = subprocess.run(['node', '-e', harness], capture_output=True, text=True, check=True)
        return json.loads(run.stdout)

    def test_page_close_ends_the_applet_only_when_called(self):
        self.assertEqual(self.bridge('close()'), {'before': 0, 'messages': [{'k': 'end', 'r': 4}], 'nativeCloses': 0})

    def test_explicit_end_applet_keeps_its_exit_reason(self):
        self.assertEqual(self.bridge('nx.endApplet()'), {'before': 0, 'messages': [{'k': 'end', 'r': 0}], 'nativeCloses': 0})


if __name__ == '__main__':
    unittest.main()
