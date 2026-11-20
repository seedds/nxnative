#!/usr/bin/env python3
# Area labels for an issue, from its text (.github/workflows/issue-areas.yml applies them): the labels pr_areas.py gives pull requests, so that one filter finds both. An issue has no
# changed paths. Its game is a title ID (or a name from the games table in its title); its area is a word in its title, and its platform a word in its title or the bug form's answer.
# The rest of the body is not searched for areas: a report names scripts/build_ipa_from_nsp.py because that is how it was packed, not because the bug is there.
# Its kind (bug, enhancement, new game) is the form it was filed with, or a title that says so; an issue that says neither stays without one for a person to sort: symptom words would match
# "Crash Bandicoot".
# The workflow runs this file from main and gives it only the issue's title and body, and every label comes from the rules here, so an issue's text cannot make up a label.
# Usage: issue_areas.py [--root DIR] < issue.json   ({"title": ..., "body": ...}; DIR is main's checkout, default this repository)  → one line per label: name, color, description (tab-separated)
import argparse, json, pathlib, re, sys

import pr_areas

TITLE_ID = r'\b0[14]00[0-9A-Fa-f]{12}\b'
MAX_BODY_GAMES = 5   # more title IDs than this in the body is a list of games, not the issue's game
WORDS = {   # area → words in the title (regular expression, any case)
    'host': r'\bhost\b|\bhle_\w+|\bSVCs?\b',
    'nvn': r'\b(h_)?nvn\w*',
    'vulkan': r'\b(vulkan|vk)\b',
    'opengl': r'\b(opengl|gl)\b',
    'textures': r'\b(BC[1-7]|ASTC|texture (layout|format)s?)\b',
    'rewriter': r'\b(patch_nso|sdk_patches|nso_\w+|rewriter)\b',
    'packing': r'\b(shader[ _]split|shader extraction|nx_extract|build_ipa\w*|pack_app|NSP script)\b',
    'ios': r'\b(iOS|iPadOS|iPhone\w*|iPad|IPA)\b',
    'mac': r'\b(Mac|macOS|MacBook)\b',
    'tools': r'\b(dm/tools|scripts/|\w+\.py\b)',
    'docs': r'\b(\w+\.md|docs?|documentation)\b',
    'ci': r'\bCI\b|\.github/',
    pr_areas.CHAIN[0]: r'\b(DMSH_CHAIN|dmsh_\w+|shader chain|translator)\b',
}
AREA_LABELS = {a[0]: a[1:3] for a in pr_areas.AREAS} | {pr_areas.CHAIN[0]: pr_areas.CHAIN[1:]}
KIND_LABELS = pr_areas.KINDS | {pr_areas.NEW_GAME[0]: pr_areas.NEW_GAME[1:]}
FORMS = {'What happened': 'bug', 'What is missing': 'enhancement', 'Survey and what happened': 'new game'}   # a heading only that form has (.github/ISSUE_TEMPLATE) → its kind
TITLE_KINDS = {   # kind → how a title without a form starts (regular expression, any case)
    'enhancement': r'^((scope )?proposal|feature request|suggestion|design|roadmap)\s*:|^feat\(',
    'new game': r'^(request:|\[patch\]|\[bring-?up\])',
}


def base_id(title_id):   # an update's or a DLC's title ID names its base game; the game labels and the profiles use the base's (a multi-program title's programs keep their own)
    v = int(title_id, 16)
    if v & 0x1000: v &= ~0x1FFF
    elif v & 0xFFF == 0x800: v &= ~0xFFF
    return f'{v:016X}'


def ids_in(text):
    return {base_id(m) for m in re.findall(TITLE_ID, text)}


def field(body, heading):   # the answer under a form's heading
    m = re.search(rf'^### {re.escape(heading)}\s*\n+(.+)', body, re.M)
    return m.group(1) if m else ''


def words(text):   # lower case, punctuation gone; a version stays one word ("1.4.3"), so named_games does not read it as a sequel
    return ' '.join(re.findall(r'\w+', re.sub(r'(?<=\d)\.(?=\d)', '_', text.lower())))


def named_games(title, names):   # games the title names, longest name first so that "Hades II" is not also "Hades"; "Hades 2" is neither. Reporters drop "The Legend of Zelda: "; other parts of a name are not distinctive ("Definitive Edition")
    forms = {}
    for gid, name in names.items():
        if name.endswith(' release'): continue   # "<name>, Asian release": the name alone means the base game
        forms[words(name)] = gid
        forms[words(name.removeprefix('The Legend of Zelda: '))] = gid
    found, text = set(), words(title)
    for form in sorted(forms, key=len, reverse=True):
        text, n = re.subn(rf'\b{re.escape(form)}\b(?! (\d|ii)\b)', ' ', text)
        if n: found.add(forms[form])
    return found


def games(title, body, names):
    ids = ids_in(title + '\n' + field(body, 'Game, title ID and version'))   # what the issue says it is about; the body may also compare it with others
    if not ids:
        ids = ids_in(body)
        if len(ids) > MAX_BODY_GAMES: ids = set()
    return ids or named_games(title, names)


def kind(title, body):
    for heading, name in FORMS.items():
        if re.search(rf'^### {re.escape(heading)}\s*$', body, re.M): return name
    for name, rx in TITLE_KINDS.items():
        if re.match(rx, title.strip(), re.I): return name
    return None


def labels(title, body, root):
    built = field(body, 'How it was built and run')
    out, names = {}, pr_areas.game_names(root)
    for area, rx in WORDS.items():
        if re.search(rx, title, re.I | re.A) or (area in ('ios', 'mac') and re.search(rx, built, re.I | re.A)):
            out[area] = AREA_LABELS[area]
    if name := kind(title, body): out[name] = KIND_LABELS[name]
    for gid in sorted(games(title, body, names)):
        out[f'game: {gid}'] = (pr_areas.GAME_COLOR, names.get(gid, 'A game')[:100])
    return out


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description="area labels for an issue from its title and body (JSON on standard input)")
    ap.add_argument('--root', default=str(pathlib.Path(__file__).resolve().parents[2]), help="main's checkout (default: this repository)")
    a = ap.parse_args()
    issue = json.load(sys.stdin)
    for name, (color, desc) in labels(issue['title'], issue.get('body') or '', pathlib.Path(a.root)).items():
        print(f'{name}\t{color}\t{desc}')
