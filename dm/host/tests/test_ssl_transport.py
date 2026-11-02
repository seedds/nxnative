#!/usr/bin/env python3
"""Generate local test-only credentials, serve real TLS, and test the production guest TLS hooks."""
import argparse
import contextlib
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading
import time


def command(args, folder):
    subprocess.run(args, cwd=folder, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    with tempfile.TemporaryDirectory(prefix="nxnative-tls-") as folder:
        root = Path(folder)
        os.chmod(root, 0o700)
        command(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "1", "-subj", "/CN=nxnative ephemeral TLS test CA"], folder)
        command(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", "server.key", "-out", "server.csr", "-subj", "/CN=localhost"], folder)
        (root / "extensions.cnf").write_text("subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
        command(["openssl", "x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-out", "server.pem", "-days", "1", "-extfile", "extensions.cnf"], folder)
        command(["openssl", "x509", "-in", "ca.pem", "-outform", "der", "-out", "ca.der"], folder)
        command(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", "client.key", "-out", "client.csr", "-subj", "/CN=nxnative ephemeral TLS client"], folder)
        (root / "client-extensions.cnf").write_text("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n")
        command(["openssl", "x509", "-req", "-in", "client.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-out", "client.pem", "-days", "1", "-extfile", "client-extensions.cnf"], folder)
        command(["openssl", "pkcs12", "-export", "-inkey", "client.key", "-in", "client.pem", "-out", "client.p12", "-passout", "pass:test-only"], folder)
        for key in root.glob("*.key"):
            key.chmod(0o600)
        (root / "client.p12").chmod(0o600)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = tls.maximum_version = ssl.TLSVersion.TLSv1_2
        tls.set_alpn_protocols(["http/1.1", "h2"])
        tls.load_cert_chain(root / "server.pem", root / "server.key")
        tls.load_verify_locations(root / "ca.pem")
        tls.verify_mode = ssl.CERT_OPTIONAL
        names = []
        tls.set_servername_callback(lambda connection, name, context: names.append(name))
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.bind(("127.0.0.1", 0))
        server.listen(16)
        server.settimeout(0.1)
        stop = threading.Event()
        workers = []

        def serve(connection):
            with contextlib.suppress(ssl.SSLError, OSError, TimeoutError):
                connection.settimeout(3)
                time.sleep(0.025)  # Make the client's nonblocking handshake take a real readiness path.
                with tls.wrap_socket(connection, server_side=True) as stream:
                    time.sleep(0.12)
                    stream.sendall(b"HELLO")
                    message = stream.recv(4)
                    if message == b"ECHO":
                        stream.sendall(b"AB")
                        time.sleep(0.01)
                        stream.sendall(b"CD")
                        stream.unwrap()  # Send a real close_notify; the client must report orderly TLS EOF.
                    elif message == b"WAIT":
                        time.sleep(1)
                    elif message == b"ABRT":
                        raw = stream.detach()  # Intentionally omit close_notify to test authenticated-stream truncation.
                        os.close(raw)
                    elif message == b"MTLS":
                        certificate = stream.getpeercert()
                        common_names = [value for rdn in certificate.get("subject", ()) for name, value in rdn if name == "commonName"]
                        stream.sendall(b"CERT" if common_names == ["nxnative ephemeral TLS client"] else b"FAIL")
                    elif message == b"BULK":
                        time.sleep(0.15)
                        size = 2 * 1024 * 1024
                        received = bytearray()
                        while len(received) < size:
                            part = stream.recv(min(65536, size - len(received)))
                            if not part:
                                break
                            received.extend(part)
                        expected = (bytes(range(251)) * ((size + 250) // 251))[:size]
                        stream.sendall(b"GOOD" if received == expected else b"FAIL")
            connection.close()

        def accept():
            while not stop.is_set():
                try:
                    connection, _ = server.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                thread = threading.Thread(target=serve, args=(connection,), daemon=True)
                workers.append(thread)
                thread.start()

        listener = threading.Thread(target=accept, daemon=True)
        listener.start()
        try:
            result = subprocess.run([str(executable), str(root / "ca.pem"), str(root / "ca.der"), str(server.getsockname()[1]), str(root / "client.p12"), str(root / "system_data")], timeout=45)
            if result.returncode:
                return result.returncode
            if "localhost" not in names:
                raise AssertionError("server never received the expected SNI")
            print("native TLS server: hostname/SNI observed; no user credentials or system trust store used")
            return 0
        finally:
            stop.set()
            server.close()
            listener.join(1)
            for worker in workers:
                worker.join(0.2)


if __name__ == "__main__":
    raise SystemExit(main())
