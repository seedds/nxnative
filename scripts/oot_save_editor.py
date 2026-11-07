#!/usr/bin/env python3
"""Edit confirmed bottle fields in copied Ocarina saves. Python standard library only."""
import argparse
import base64
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import secrets
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'dm/tools'))
from save_editor.oot_items import Slot, sha


def serve(slot, port, schema):
    active = [slot]
    def catalog():
        return {"fields": active[0].catalog(), "source": sha(active[0].progress + active[0].caption)}
    token = secrets.token_urlsafe(32)
    template = (Path(__file__).resolve().parents[1] / 'dm/tools/save_editor/index.html').read_text()
    page = template.replace('__TOKEN__', token).encode()

    class Handler(BaseHTTPRequestHandler):
        def reply(self, status, body, kind='application/json'):
            self.send_response(status)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'")
            self.end_headers()
            self.wfile.write(body)

        def allowed(self):
            origin = f'http://127.0.0.1:{self.server.server_port}'
            return (self.headers.get('Host') == origin[7:] and
                    self.headers.get('Origin', origin) == origin)

        def do_GET(self):
            if not self.allowed():
                self.reply(403, b'{}')
            elif self.path == '/':
                self.reply(200, page, 'text/html; charset=utf-8')
            elif self.path == '/items':
                self.reply(200, json.dumps(catalog()).encode())
            else:
                self.reply(404, b'{}')

        def do_POST(self):
            if not self.allowed() or self.headers.get('X-Editor-Token') != token:
                return self.reply(403, b'{}')
            if self.path not in ('/export', '/open'):
                return self.reply(404, b'{}')
            try:
                size = int(self.headers.get('Content-Length', '0'))
                if not 0 < size <= 4 * 1024 * 1024:
                    raise ValueError('Invalid request length')
                request = json.loads(self.rfile.read(size))
                if not isinstance(request, dict):
                    raise ValueError('Request must be an object')
                if self.path == '/open':
                    progress = base64.b64decode(request['progress'], validate=True)
                    caption = base64.b64decode(request['caption'], validate=True)
                    candidate = Slot(progress, caption, schema)
                    active[0] = candidate
                    self.reply(200, json.dumps(catalog()).encode())
                else:
                    if request.get('source') != catalog()['source']:
                        raise ValueError('Source changed in another tab. Reload before exporting.')
                    self.reply(200, active[0].archive(request.get('edits')), 'application/zip')
            except (ValueError, TypeError, KeyError) as error:
                self.reply(400, json.dumps({'error': str(error)}).encode())

    server = HTTPServer(('127.0.0.1', port), Handler)
    server.timeout = 1
    print(f'Editor: http://127.0.0.1:{server.server_port}/ (Ctrl-C to stop)', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--schema', type=Path, required=True, help='Extracted GameDataList.json')
    p.add_argument('--slot', type=Path, required=True, help='Folder containing progress.sav and caption.sav; read only')
    p.add_argument('--set', action='append', default=[], metavar='BottleA=Fairy')
    p.add_argument('--out', type=Path, help='Create a new ZIP; never overwrite an existing file')
    p.add_argument('--serve', action='store_true', help='Open a local editor server')
    p.add_argument('--port', type=int, default=0, help='Local port; default selects a free port')
    args = p.parse_args()
    try:
        if args.serve and (args.out or args.set):
            raise ValueError('--serve cannot be combined with --set or --out')
        if args.set and not args.out:
            raise ValueError('--set requires --out')
        files = [args.slot / n for n in ('progress.sav', 'caption.sav')]
        before = [f.read_bytes() for f in files]
        schema = json.loads(args.schema.read_text())
        slot = Slot(*before, schema)
        if before != [f.read_bytes() for f in files]:
            raise ValueError('Source changed while reading; retry with a stable slot copy')
        if args.serve:
            serve(slot, args.port, schema)
        elif args.out:
            edits = {}
            for setting in args.set:
                name, value = setting.split('=', 1)
                if name in edits:
                    raise ValueError(f'Duplicate edit: {name}')
                edits[name] = value
            archive = slot.archive(edits)
            with args.out.open('xb') as f:
                f.write(archive)
            print(f'Exported {args.out}. Original saves unchanged. In-game validation pending.')
        else:
            print(json.dumps(slot.catalog(), indent=2))
    except (OSError, ValueError, KeyError, TypeError) as error:
        p.exit(1, f'Error: {error}\n')


if __name__ == '__main__':
    main()
