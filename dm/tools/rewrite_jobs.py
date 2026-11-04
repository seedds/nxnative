"""CPU allocations for concurrently rewritten modules, refreshed between analysis passes."""
import os
from pathlib import Path
import struct


def text_size(path):
    """Scheduling estimate only; nso.load remains responsible for validating the module."""
    with open(path, 'rb') as stream: head = stream.read(0x30)
    if len(head) >= 0x30:
        if head[:4] == b'NSO0': return struct.unpack_from('<I', head, 0x18)[0]
        if head[0x10:0x14] == b'NRO0': return struct.unpack_from('<I', head, 0x24)[0]
    return Path(path).stat().st_size


def workers(limit):
    path = os.environ.get('DM_REWRITE_BUDGET')
    if not path: return limit
    with open(path) as stream: value = int(stream.read())
    if value < 1: raise ValueError('rewrite CPU allocation must be positive')
    return min(limit, value)


class Budget:
    def __init__(self, directory, sizes, total):
        self.directory, self.sizes, self.total = Path(directory), sizes, total
        self.directory.mkdir(parents=True, exist_ok=True)
        self.shares = {}

    def update(self, active):
        """Keep surviving shares, reserve one per new module, distribute freed slots by text size.

        Shares only grow while a module runs: an existing fork pool can keep its old
        allocation safely until the next pass reads the new one. Replacing a completed
        module with a queued one happens before any freed slots are distributed.
        """
        shares = {name: self.shares.get(name, 1) for name in active}
        if sum(shares.values()) > self.total: raise ValueError('rewrite CPU budget exceeded')
        for _ in range(self.total - sum(shares.values())):
            if not shares: break
            name = max(shares, key=lambda n: self.sizes[n] / (shares[n] + 1))
            shares[name] += 1
        for name in self.shares.keys() - shares.keys(): (self.directory / name).unlink()
        for name, count in shares.items():
            if self.shares.get(name) == count: continue
            tmp = self.directory / (name + '.tmp'); tmp.write_text(str(count))
            tmp.replace(self.directory / name)
        self.shares = shares
        return dict(shares)
