"""Experimental Ocarina bottle-content edits; sources are never overwritten."""
import hashlib
import io
import json
import struct
import zipfile

REVISION = 2982123
BOTTLES = dict(zip(('BottleA', 'BottleB', 'BottleC', 'BottleD', 'BottleE'),
                   (2101927798, 517805642, 3087823016, 3837899260, 912136531)))
INLINE = {'Bool', 'Int', 'UInt', 'Float', 'Enum'}
SIZES = {'Bool': 4, 'Int': 4, 'UInt': 4, 'Float': 4, 'Enum': 4,
         'Int64': 8, 'UInt64': 8, 'Vector3': 12, 'String16': 16,
         'String32': 32, 'String64': 64, 'WString16': 32}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Save:
    def __init__(self, data, schema, index):
        self.data = data
        self.records = {}
        require(40 <= len(data) <= 8 * 1024 * 1024, 'Invalid save size')
        magic, revision, end = struct.unpack_from('<III', data)
        require(magic == 0x01020304 and revision == REVISION,
                'Unsupported save magic or revision')
        require(48 <= end <= len(data) and end % 8 == 0 and
                end == schema['MetaData']['SaveDataOffsetPos'][index],
                'Invalid record table or mismatched schema')
        lookup = {x['Hash']: (kind, x) for kind, rows in schema['Data'].items() for x in rows}
        tail_hash = schema['MetaData']['SaveTypeHash'][index]
        tail = None
        ranges = []

        def take(pos, size):
            require(end <= pos <= len(data) and 0 <= size <= len(data) - pos,
                    'Payload outside save bounds')
            ranges.append((pos, pos + size))

        def payload(kind, pos):
            if kind.endswith('Array'):
                take(pos, 4)
                count = struct.unpack_from('<I', data, pos)[0]
                require(count <= 1000000, 'Invalid array length')
                base = kind[:-5]
                pos += 4
                if base == 'Bool':
                    take(pos, ((count + 31) // 32) * 4)
                elif base == 'Binary':
                    for _ in range(count):
                        pos = payload(base, pos)
                else:
                    require(base in SIZES, 'Unsupported array type')
                    take(pos, count * SIZES[base])
            elif kind == 'Binary':
                take(pos, 4)
                size = struct.unpack_from('<I', data, pos)[0]
                take(pos + 4, size)
                return pos + 4 + size
            else:
                require(kind in SIZES, 'Unsupported payload type')
                take(pos, SIZES[kind])

        for off in range(40, end, 8):
            key, value = struct.unpack_from('<II', data, off)
            if key == 0:
                continue
            if key == tail_hash:
                require(off == end - 8, 'Misplaced tail record')
                tail = value
                continue
            require(key not in self.records and key in lookup, 'Duplicate or unknown save field')
            kind, meta = lookup[key]
            require(meta['SaveFileIndex'] == index, 'Field belongs to another save type')
            self.records[key] = (off + 4, value, kind, meta)
            if kind not in INLINE:
                payload(kind, value)
        require(tail is not None, 'Missing tail record')
        if tail:
            require(end <= tail <= len(data) - 8 and (len(data) - tail) % 8 == 0, 'Invalid sparse tail')
        for off in range(tail if tail else len(data), len(data), 8):
            key = struct.unpack_from('<Q', data, off)[0]
            if off == len(data) - 8:
                require(key == 0, 'Missing sparse terminator')
            else:
                require(key in lookup and lookup[key][0] == 'Bool64bitKey', 'Unknown sparse key')
        if tail:
            ranges.append((tail, len(data)))
        # Every payload byte must be understood before permitting a write.
        cursor = end
        for start, stop in sorted(ranges):
            require(start == cursor, 'Overlapping or unexplained payload bytes')
            cursor = stop
        require(cursor == len(data), 'Unexplained trailing bytes')


class Slot:
    def __init__(self, progress, caption, schema):
        self.progress, self.caption = progress, caption
        self.save = Save(progress, schema, 0)
        Save(caption, schema, 2)
        self.fields = {}
        for name, key in BOTTLES.items():
            require(key in self.save.records, f'Missing {name}')
            off, value, kind, meta = self.save.records[key]
            labels, values = meta.get('RawValues', []), meta.get('Values', [])
            require(kind == 'Enum' and len(labels) == len(values) and labels and
                    len(set(labels)) == len(labels) and len(set(values)) == len(values) and
                    all(isinstance(x, str) for x in labels), 'Invalid bottle enum schema')
            choices = dict(zip(labels, values))
            require(value in values, f'Unknown current value for {name}')
            self.fields[name] = {'value': labels[values.index(value)], 'choices': choices, 'offset': off}

    def catalog(self):
        return {name: {'value': f['value'], 'choices': list(f['choices'])}
                for name, f in self.fields.items()}

    def edit(self, edits):
        require(isinstance(edits, dict), 'Edits must be an object')
        result = bytearray(self.progress)
        changes = []
        for name, value in edits.items():
            require(name in self.fields, f'Unsupported item field: {name}')
            f = self.fields[name]
            require(isinstance(value, str) and value in f['choices'], f'Invalid content for {name}')
            if value != f['value']:
                struct.pack_into('<I', result, f['offset'], f['choices'][value])
                changes.append({'field': name, 'before': f['value'], 'after': value, 'offset': f['offset']})
        return bytes(result), changes

    def archive(self, edits):
        progress, changes = self.edit(edits)
        manifest = {'format': 'nxnative-oot-item-edit-v1', 'revision': REVISION,
                    'scope': 'Bottle contents only; ownership unchanged; in-game validation pending',
                    'changes': changes, 'source_progress_sha256': sha(self.progress),
                    'export_progress_sha256': sha(progress), 'caption_sha256': sha(self.caption)}
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
            z.writestr('slot/progress.sav', progress)
            z.writestr('slot/caption.sav', self.caption)
            z.writestr('edit-manifest.json', json.dumps(manifest, indent=2) + '\n')
        return buf.getvalue()
