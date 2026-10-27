"""Runtime events (dm/host/dm_events.c, docs/frontends.md, Runtime events): every event the code can emit and every field its lines carry is listed in the docs section, and every listed event is
emitted somewhere. An event or field the docs do not list, a listed one nothing emits, a call site that names a value the docs do not know, or a DM_EVENTS missing from the switch table fails here."""
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[3]
SRC = (REPO / 'dm/host/dm_events.c').read_text()
DOCS = (REPO / 'docs/frontends.md').read_text()
SECTION = DOCS[DOCS.index('## Runtime events'):]
ROWS = {m.group(1): m.group(2) for m in re.finditer(r'^\| `(\w+)` \| (.*?) \| .* \|$', SECTION, re.M) if m.group(1) not in ('ev',)}


def names():   # the NAMES table of dm_events.c
    table = re.search(r'NAMES\[\] = \{(.*?)\};', SRC, re.S)
    assert table, 'no NAMES table in dm_events.c'
    return re.findall(r'"(\w+)"', table.group(1))


def sources():
    out = []
    for d in ('dm/host', 'dm/gfx', 'dm/mac', 'dm/ios/Sources'):
        out += [p for p in (REPO / d).rglob('*') if p.suffix in ('.c', '.m', '.h') and 'tests' not in p.parts and p.name != 'dm_events.c']
    return out


class RuntimeEvents(unittest.TestCase):
    def test_every_event_is_documented_and_emitted(self):
        emitted = names()
        self.assertEqual(sorted(emitted), sorted(ROWS), 'dm_events.c NAMES and the docs table list different events')
        calls = {'frame_loop': 'dm_event_frame_loop(', 'shaders': 'dm_event_shaders(', 'red': 'dm_log_red(', 'end': 'dm_event_end(', 'exit': 'dm_event_exit(',
                 'controller': 'dm_pad_event(', 'thermal': 'dm_event_thermal(', 'save': 'dm_event_save('}
        self.assertEqual(sorted(calls), sorted(emitted), 'a new event needs its emitting call named here')
        for ev, call in calls.items():
            used = [p for p in sources() if call in p.read_text()]
            self.assertTrue(used, f'{ev}: nothing calls {call}')
        # `controller` has one emitting function every front end calls (hle_hid.c), and both front ends call it
        self.assertIn('dm_event_controller(', (REPO / 'dm/host/hle_hid.c').read_text())
        for fe in ('dm/host/front_mac.m', 'dm/ios/Sources/DmGamepad.m'):
            self.assertIn('dm_pad_event(', (REPO / fe).read_text(), fe)
        for fe in ('dm/host/front_mac.m', 'dm/ios/Sources/front_ios.m'):
            self.assertIn('dm_event_thermal(', (REPO / fe).read_text(), fe)
        # `red` is emitted by the LOG macros: dm.h and nvn_mtl.m must route through them
        self.assertIn('DM_LOG_TAGGED', (REPO / 'dm/host/dm.h').read_text())

    def test_every_field_is_documented(self):
        fields = set(re.findall(r'\\"(\w+)\\":', SRC))
        self.assertTrue({'ev', 't', 'elapsed', 'phase', 'state', 'done', 'total', 'text', 'reason', 'code', 'signal', 'dropped', 'player', 'name', 'kind', 'type', 'level', 'save', 'mount', 'commit'} <= fields, fields)
        for f in fields:
            self.assertIn(f'`{f}`', SECTION, f'the field {f} that dm_events.c writes is not in the docs')

    def test_values_the_calls_name_are_documented(self):
        text = '\n'.join(p.read_text() for p in sources())
        for call in ('dm_event_shaders(', 'dm_event_end(', 'dm_event_exit('):
            for m in re.finditer(re.escape(call) + r'([^;]*);', text):
                for lit in re.findall(r'"(\w+)"', m.group(1)):
                    self.assertIn(f'`{lit}`', SECTION, f'{call} names "{lit}", which the docs do not list')

    def test_switch_is_listed(self):
        self.assertRegex((REPO / 'docs/env_switches.tsv').read_text(), r'(?m)^DM_EVENTS\t\w+\t\S')


if __name__ == '__main__':
    unittest.main()
