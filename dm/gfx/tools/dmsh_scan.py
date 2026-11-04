"""Candidate-line filtering for anchored MSL regexes with a required literal.
The original regex determines each match, including qualifiers and capture groups.
"""


def literal_lines(pattern, source, literal):
    at = 0
    while True:
        at = source.find(literal, at)
        if at < 0: return
        match = pattern.match(source, source.rfind('\n', 0, at) + 1)
        if match: yield match
        end = source.find('\n', match.end() if match else at)
        if end < 0: return
        at = end + 1
