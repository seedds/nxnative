#!/usr/bin/env python3
"""Exercise immutable route configuration and real scoped TLS using ephemeral fixtures."""
import argparse
import contextlib
import hashlib
import os
from pathlib import Path
import re
import socket
import ssl
import subprocess
import tempfile
import threading

from test_ssl_transport import command


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    executable = parser.parse_args().executable.resolve()
    with tempfile.TemporaryDirectory(prefix="nxnative-routes-") as folder:
        root = Path(folder)
        os.chmod(root, 0o700)
        (root / 'ca.cnf').write_text('[req]\ndistinguished_name=dn\nx509_extensions=ca\nprompt=no\n[dn]\nCN=Original ephemeral route test CA\n[ca]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n')
        command(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "1", "-config", "ca.cnf"], folder)
        command(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", "server.key", "-out", "server.csr", "-subj", "/CN=localhost"], folder)
        (root / "extensions.cnf").write_text("subjectAltName=DNS:localhost,DNS:api.console.invalid\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n")
        command(["openssl", "x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-out", "server.pem", "-days", "1", "-extfile", "extensions.cnf"], folder)
        command(["openssl", "x509", "-in", "ca.pem", "-outform", "der", "-out", "ca.der"], folder)
        for path in root.iterdir():
            path.chmod(0o600)
        digest = hashlib.sha256((root / "ca.der").read_bytes()).hexdigest()
        header = "NXNEXTENDO_ROUTES_1\n"
        line = f"api.console.invalid\t127.0.0.1\tlocalhost\tca.der\t{digest}\n"
        text = header + line + f"wrong.console.invalid\t127.0.0.1\twrong.invalid\tca.der\t{digest}\n" + "nncs2-lp1.n.n.srv.nintendo.net\t127.0.0.1\t-\t-\t-\n"
        config = root / "routes.tsv"
        config.write_text(text)
        config.chmod(0o600)
        cases = [("-", 1), (str(root / "missing.tsv"), 1), (str(config), 0)]

        def add(name, value, status, mode=0o600):
            path = root / name
            path.write_bytes(value if isinstance(value, bytes) else value.encode())
            path.chmod(mode)
            cases.append((str(path), status))

        add("empty.tsv", header, 2)
        add("bad-header.tsv", "NXNEXTENDO_ROUTES_2\n" + line, 2)
        add("duplicate.tsv", text + line.replace("api.console.invalid", "API.CONSOLE.INVALID."), 2)
        add("wildcard.tsv", header + line.replace("api.console.invalid", "*.console.invalid"), 2)
        add("multicast.tsv", header + line.replace("127.0.0.1", "224.1.2.3"), 2)
        add("bad-address.tsv", header + line.replace("127.0.0.1", "localhost"), 2)
        add("numeric-host.tsv", header + line.replace("api.console.invalid", "127.0.0.1."), 2)
        add("bad-peer.tsv", header + line.replace("localhost", "https://localhost"), 2)
        add("not-tls.tsv", header + line.replace("localhost", "-"), 2)
        add("bad-hash.tsv", text.replace(digest, "0" * 64), 5)
        add("missing-ca.tsv", text.replace("ca.der", "missing.der"), 5)
        add("traversal-ca.tsv", text.replace("ca.der", "../ca.der"), 5)
        add("writable.tsv", text, 3, 0o666)
        add("nul.tsv", text.encode() + b"\x00", 2)
        add("huge.tsv", header + "#" * 65536, 4)
        add("many.tsv", header + "".join(line.replace("api.console.invalid", f"api{i}.console.invalid") for i in range(129)), 4)
        (root / "not.der").write_bytes(b"not a certificate")
        bad_digest = hashlib.sha256((root / "not.der").read_bytes()).hexdigest()
        add("invalid-der.tsv", (header + line).replace("ca.der", "not.der").replace(digest, bad_digest), 5)
        (root / "alias.der").symlink_to(root / "ca.der")
        add("symlink-ca.tsv", text.replace("ca.der", "alias.der"), 5)
        alias = root / "alias.tsv"
        alias.symlink_to(config)
        cases.append((str(alias), 3))
        v2 = root / 'v2.tsv'
        v2text = 'NXNEXTENDO_ROUTES_2\n' + ''.join('exact\t' + row + '\n' for row in text.splitlines()[1:])
        v2text = v2text.replace('api.console.invalid\t127.0.0.1\tlocalhost', 'api.console.invalid\t127.0.0.1\t@host')
        v2text += 'suffix\tscope.invalid\t127.0.0.2\t-\t-\t-\nlabel\tnat*.scope.invalid\t127.0.0.3\t-\t-\t-\nexact\tspecial.scope.invalid\t127.0.0.4\t-\t-\t-\n'
        add('v2.tsv', v2text, 0)
        add('v2-multi-star.tsv', v2text.replace('nat*.scope', 'nat**.scope'), 2)
        add('v2-bad-kind.tsv', v2text.replace('suffix\t', 'regex\t'), 2)
        add('v2-duplicate.tsv', v2text + 'suffix\tSCOPE.INVALID.\t127.0.0.2\t-\t-\t-\n', 2)
        add('v2-no-ca.tsv', v2text.replace('@host\tca.der', '@host\t-'), 5)
        add('pin-renamed-peer.tsv', v2text.replace('exact\tapi.console.invalid', 'pinned-exact\tapi.console.invalid').replace('@host\t', 'localhost\t'), 2)
        add('pin-wildcard.tsv', v2text.replace('exact\tapi.console.invalid', 'pinned-exact\t*.console.invalid'), 2)
        add('pin-duplicate.tsv', v2text + f'pinned-exact\tapi.console.invalid\t127.0.0.1\t@host\tca.der\t{digest}\n', 2)
        add('pin-empty.tsv', v2text.replace('exact\tapi.console.invalid', 'pinned-exact\tapi.console.invalid').replace(f'@host\tca.der\t{digest}', '@host\t-\t-'), 5)

        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = tls.maximum_version = ssl.TLSVersion.TLSv1_2
        tls.load_cert_chain(root / "server.pem", root / "server.key")
        names = []
        tls.set_servername_callback(lambda connection, name, context: names.append(name))
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(4)
        listener.settimeout(0.1)
        stop = threading.Event()

        def serve(listener=listener, tls=tls):
            while not stop.is_set():
                try:
                    connection, _ = listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    return
                with connection, contextlib.suppress(OSError, ssl.SSLError):
                    connection.settimeout(3)
                    with tls.wrap_socket(connection, server_side=True) as stream:
                        stream.sendall(b"HELLO")
                        if stream.recv(4) == b"PING":
                            stream.sendall(b"PONG")

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        # A separate self-signed endpoint has deliberately no SAN. Enrollment
        # binds its exact DER to one logical host; no network TOFU is performed.
        (root/'pin-endpoint.cnf').write_text('basicConstraints=critical,CA:TRUE\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid\n')
        command(['openssl','x509','-req','-in','server.csr','-signkey','server.key','-days','1','-extfile','pin-endpoint.cnf','-out','pin.pem'], folder)
        command(['openssl','x509','-in','pin.pem','-outform','der','-out','pin.der'], folder)
        command(['openssl','x509','-req','-in','server.csr','-signkey','server.key','-days','1','-set_serial','1234567','-out','other-pin.pem'], folder)
        command(['openssl','x509','-in','other-pin.pem','-outform','der','-out','other-pin.der'], folder)
        pin_tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        pin_tls.minimum_version = pin_tls.maximum_version = ssl.TLSVersion.TLSv1_2
        pin_tls.load_cert_chain(root/'pin.pem', root/'server.key')
        pin_tls.set_servername_callback(lambda connection, name, context: names.append(name))
        pin_listener = socket.socket(); pin_listener.bind(('127.0.0.1',0)); pin_listener.listen(4); pin_listener.settimeout(0.1)
        pin_thread = threading.Thread(target=serve,args=(pin_listener,pin_tls),daemon=True); pin_thread.start()
        pin_configs=[]
        for name, certificate, reject in [('pin','pin.der',False),('pin-mismatch','other-pin.der',True),('pin-handoff','pin.der',False)]:
            pin_digest=hashlib.sha256((root/certificate).read_bytes()).hexdigest()
            value=v2text.replace(f'exact\tapi.console.invalid\t127.0.0.1\t@host\tca.der\t{digest}', f'pinned-exact\tapi.console.invalid\t127.0.0.1\t@host\t{certificate}\t{pin_digest}')
            if name == 'pin-handoff':
                alias=f'pinned-address\t127.0.0.1\t127.0.0.1\tapi.console.invalid\t{certificate}\t{pin_digest}\n'
                value += alias
                # Each invalid configuration must fail atomically before DNS/TLS.
                add('alias-unpaired.tsv','NXNEXTENDO_ROUTES_2\n'+alias,2)
                add('alias-wrong-target.tsv',value.replace('pinned-address\t127.0.0.1\t127.0.0.1','pinned-address\t127.0.0.1\t127.0.0.2'),2)
                add('alias-other-identity.tsv',value.replace('pinned-address\t127.0.0.1\t127.0.0.1\tapi.console.invalid','pinned-address\t127.0.0.1\t127.0.0.1\twrong.console.invalid'),2)
                add('alias-other-certificate.tsv',value.replace(alias,alias.replace(f'{certificate}\t{pin_digest}',f'ca.der\t{digest}')),2)
                add('alias-self-name.tsv',value.replace(alias,alias.replace('api.console.invalid','@host')),2)
                add('alias-hostname.tsv',value.replace(alias,alias.replace('pinned-address\t127.0.0.1\t','pinned-address\tother.console.invalid\t')),2)
            path=root/(name+'.tsv'); path.write_text(value); path.chmod(0o600)
            (root/certificate).chmod(0o600); pin_configs.append((path,reject,name == 'pin-handoff'))
        total = 0
        try:
            for path, status in cases:
                args = [str(executable), path, str(status)]
                if path in (str(config), str(v2)):
                    args.append(str(listener.getsockname()[1]))
                    if path == str(v2): args.append('v2')
                result = subprocess.run(args, text=True, capture_output=True, timeout=15)
                if result.returncode:
                    raise AssertionError(f"route case {Path(path).name}: {result.stdout}\n{result.stderr}")
                match = re.search(r"routes: (\d+) checks, 0 failures", result.stdout)
                if not match:
                    raise AssertionError(result.stdout)
                total += int(match[1])
            for path,reject,handoff in pin_configs:
                result=subprocess.run([str(executable),str(path),'0',str(pin_listener.getsockname()[1]),'reject-pin' if reject else 'handoff' if handoff else 'v2'],text=True,capture_output=True,timeout=15)
                if result.returncode: raise AssertionError(f'enrolled leaf {path.name}: {result.stdout}\n{result.stderr}')
                match = re.search(r'routes: (\d+) checks, 0 failures',result.stdout)
                if match is None: raise AssertionError(result.stdout)
                total += int(match[1])
            recovery = subprocess.run([str(executable), '-', '1', str(config)], text=True, capture_output=True, timeout=15)
            if recovery.returncode: raise AssertionError(recovery.stdout + recovery.stderr)
            if not {'localhost', 'wrong.invalid', 'api.console.invalid'}.issubset(names):
                raise AssertionError("actual TLS did not observe both configured SNI names")
            print(f"Nextendo route policy: {len(cases)+len(pin_configs)} configurations, {total} checks, 0 failures; scoped-CA and explicitly enrolled leaf TLS verified, no user credentials")
        finally:
            stop.set()
            listener.close()
            thread.join(1)
            pin_listener.close(); pin_thread.join(1)


if __name__ == "__main__":
    main()
