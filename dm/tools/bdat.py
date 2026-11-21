#!/usr/bin/env python3
# Monolith's BDAT tables, version 1 (Xenoblade Chronicles 2 and Definitive Edition, and their Switch 2 Editions): read, change fields in place,
# rewrite strings, append rows. scripts/bdat_port.py ports Switch mods with it.
# File: u32 table count, u32 file size, u32 offset of each table; the tables follow back to back.
# Table (offsets from its start; layout from XbTool's BdatTable.cs and BdatTools.cs, checked on common.bdat of XC2 2.1.0 and 3.0.1):
#   "BDAT", u16 flags (bit 1: names and strings scrambled), u16 names, u16 row size, u16 hash table, u16 hash slots, u16 rows, u16 row count,
#   u16 first row ID, u16 ?, u16 key, u32 strings, u32 strings length, u16 members, u16 member count.
#   Members: 6 bytes each (u16 info, u16 ?, u16 name); info: u8 type (1 scalar, 2 array, 3 flag), then for a scalar or array u8 value type,
#   u16 position in the row (and u16 count for an array). Value types: 1 u8, 2 u16, 3 u32, 4 s8, 5 s16, 6 s32, 7 string (u32 offset in the
#   table), 8 f32. Rows follow at the rows offset, the strings right after them. The scramble runs over byte pairs, each key byte advanced by
#   the scrambled byte, over the names (to the hash table) and over the strings.
import struct

SCALAR, ARRAY, FLAG = 1, 2, 3
VT = {1: ('B', 1), 2: ('H', 2), 3: ('I', 4), 4: ('b', 1), 5: ('h', 2), 6: ('i', 4), 7: ('S', 4), 8: ('f', 4)}
HEAD = struct.Struct('<HHHHHHHHHHIIHH')   # from offset 4


class Error(ValueError):
    pass


def descramble(buf, key, start, length):
    a, b = (~key >> 8) & 0xff, ~key & 0xff
    for i in range(start, start + length - (length & 1), 2):
        da, db = buf[i], buf[i + 1]
        buf[i] ^= a; buf[i + 1] ^= b
        a = (a + da) & 0xff; b = (b + db) & 0xff


def scramble(buf, key, start, length):
    a, b = (~key >> 8) & 0xff, ~key & 0xff
    for i in range(start, start + length - (length & 1), 2):
        buf[i] ^= a; buf[i + 1] ^= b
        a = (a + buf[i]) & 0xff; b = (b + buf[i + 1]) & 0xff


class Table:
    def __init__(self, raw):
        """raw: the table's bytes as stored (scrambled when its flag says so)"""
        if raw[:4] != b'BDAT' or len(raw) < 36: raise Error('not a BDAT table')
        (self.flags, self.names_off, self.row_size, self.hash_off, self.hash_len, self.rows_off, self.count, self.base_id, self.f14,
         self.key, self.str_off, self.str_len, self.mem_off, self.mem_count) = HEAD.unpack_from(raw, 4)
        if self.str_off + self.str_len > len(raw) or self.rows_off + self.count * self.row_size > self.str_off:
            raise Error('table sections out of bounds')
        t = bytearray(raw[:self.str_off + self.str_len])
        if self.flags & 2:
            descramble(t, self.key, self.names_off, self.hash_off - self.names_off)
            descramble(t, self.key, self.str_off, self.str_len)
        self.t = t   # plain
        self.name = self.cstr(self.names_off)
        self.members = []; seen = {}
        for i in range(self.mem_count):
            info, _, name_off = struct.unpack_from('<HHH', t, self.mem_off + i * 6)
            n = self.cstr(name_off); k = seen.get(n, 0); seen[n] = k + 1
            if k: n = f'{n}_{k}'
            typ = t[info]
            if typ == FLAG: continue   # a bit of another member, which carries it
            if typ not in (SCALAR, ARRAY) or t[info + 1] not in VT: raise Error(f'{self.name}: member {n} of unknown type')
            pos = struct.unpack_from('<H', t, info + 2)[0]
            cnt = struct.unpack_from('<H', t, info + 4)[0] if typ == ARRAY else 1
            self.members.append((n, t[info + 1], pos, cnt))
        self.by_name = {m[0]: m for m in self.members}

    def cstr(self, o):
        e = self.t.index(0, o)
        return bytes(self.t[o:e]).decode('utf-8', 'replace')

    def ids(self):
        return range(self.base_id, self.base_id + self.count)

    def row(self, rid):
        """{member: value}; a string is ('str', text), an array a tuple"""
        base = self.rows_off + (rid - self.base_id) * self.row_size; out = {}
        for n, vt, pos, cnt in self.members:
            fmt, size = VT[vt]; vals = []
            for k in range(cnt):
                o = base + pos + k * size
                vals.append(('str', self.cstr(struct.unpack_from('<I', self.t, o)[0])) if fmt == 'S' else struct.unpack_from('<' + fmt, self.t, o)[0])
            out[n] = vals[0] if cnt == 1 else tuple(vals)
        return out

    def rows(self):
        return [(rid, self.row(rid)) for rid in self.ids()]

    def string_users(self, so):
        n = 0
        for r in range(self.count):
            base = self.rows_off + r * self.row_size
            for _, vt, pos, cnt in self.members:
                if vt == 7: n += sum(1 for k in range(cnt) if struct.unpack_from('<I', self.t, base + pos + 4 * k)[0] == so)
        return n

    def set(self, rid, member, value):
        """Writes one field (a number, a tuple for an array, ('str', text) for a string: over the old one when it fits and no other field
        uses it, else at the end of the strings); returns None, or why it was not written"""
        n, vt, pos, cnt = self.by_name[member]
        fmt, size = VT[vt]; base = self.rows_off + (rid - self.base_id) * self.row_size + pos
        if fmt == 'S':
            if cnt != 1 or not (isinstance(value, tuple) and len(value) == 2 and value[0] == 'str'): return 'not a single string'
            so = struct.unpack_from('<I', self.t, base)[0]
            old = self.cstr(so).encode('utf-8'); new = value[1].encode('utf-8')
            if len(new) <= len(old) and self.string_users(so) == 1:
                self.t[so:so + len(old) + 1] = new + b'\0' * (len(old) + 1 - len(new))
            else:   # appended: the table grows by the string, nothing after it moves inside the table
                end = self.str_off + self.str_len
                pad = (-(end + len(new) + 1)) % 16   # keep the table a multiple of 16 bytes
                self.t[end:end] = new + b'\0' * (1 + pad); self.str_len += len(new) + 1 + pad
                struct.pack_into('<I', self.t, base, end)
            return None
        vals = value if cnt > 1 else (value,)
        if len(vals) != cnt: return f'array of {len(vals)} values for {cnt}'
        for k, v in enumerate(vals):
            if isinstance(v, tuple): return 'not a number'
            struct.pack_into('<' + fmt, self.t, base + k * size, v)
        return None

    def append(self, rows):
        """Appends rows ({member: value}, missing members 0) after the last; their strings go at the end of the strings. Every string
        offset of the old rows moves by the rows' size"""
        add = len(rows) * self.row_size
        if self.count + len(rows) > 0xffff: raise Error(f'{self.name}: too many rows')
        strings = bytearray(self.t[self.str_off:self.str_off + self.str_len])
        new_str_off = self.str_off + add
        for r in range(self.count):   # old rows: their string offsets
            base = self.rows_off + r * self.row_size
            for _, vt, pos, cnt in self.members:
                if vt == 7:
                    for k in range(cnt):
                        o = base + pos + 4 * k
                        struct.pack_into('<I', self.t, o, struct.unpack_from('<I', self.t, o)[0] + add)
        body = bytearray()
        for row in rows:
            r = bytearray(self.row_size)
            for n, vt, pos, cnt in self.members:
                fmt, size = VT[vt]; v = row.get(n, 0 if cnt == 1 else (0,) * cnt)
                vals = v if cnt > 1 else (v,)
                for k, x in enumerate(vals):
                    if fmt == 'S':
                        text = (x[1] if isinstance(x, tuple) else x if isinstance(x, str) else '').encode('utf-8')
                        so = new_str_off + len(strings); strings += text + b'\0'
                        struct.pack_into('<I', r, pos + 4 * k, so)
                    else:
                        struct.pack_into('<' + fmt, r, pos + k * size, x)
            body += r
        end = self.rows_off + self.count * self.row_size
        while (new_str_off + len(strings)) % 16: strings.append(0)   # tables start 16-byte aligned in the Switch 2 Editions' files
        self.t = self.t[:end] + body + self.t[end:self.str_off] + strings
        self.str_off, self.str_len = new_str_off, len(strings); self.count += len(rows)

    def raw(self):
        """The table as stored: header updated, names and strings scrambled again with the same key"""
        t = bytearray(self.t)
        HEAD.pack_into(t, 4, self.flags, self.names_off, self.row_size, self.hash_off, self.hash_len, self.rows_off, self.count, self.base_id,
                       self.f14, self.key, self.str_off, self.str_len, self.mem_off, self.mem_count)
        if self.flags & 2:
            scramble(t, self.key, self.names_off, self.hash_off - self.names_off)
            scramble(t, self.key, self.str_off, self.str_len)
        return bytes(t)


class File:
    def __init__(self, data):
        if len(data) < 8: raise Error('not a BDAT file')
        n = struct.unpack_from('<I', data, 0)[0]
        if 8 + 4 * n > len(data): raise Error('table offsets out of bounds')
        offs = list(struct.unpack_from(f'<{n}I', data, 8)) + [len(data)]
        self.tables = [Table(data[offs[i]:offs[i + 1]]) for i in range(n)]
        self.by_name = {t.name: t for t in self.tables}
        self.raws = [data[offs[i]:offs[i + 1]] for i in range(n)]

    def __getitem__(self, name): return self.by_name[name]
    def __contains__(self, name): return name in self.by_name

    def bytes(self, changed):
        """The file again: tables in changed (a set of names) rebuilt, the others as they were read"""
        head = 8 + 4 * len(self.tables); blobs = []
        for t, raw in zip(self.tables, self.raws): blobs.append(t.raw() if t.name in changed else raw)
        offs, at = [], head
        for b in blobs: offs.append(at); at += len(b)
        return struct.pack(f'<II{len(blobs)}I', len(blobs), at, *offs) + b''.join(blobs)


def load(path):
    with open(path, 'rb') as f: return File(f.read())
