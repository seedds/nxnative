"""Compile the game's real controller sheet and exercise its session-only drafts on macOS."""
from pathlib import Path
import shutil
import socket
import json
import subprocess
import sys
import tempfile
import unittest

MAC = Path(__file__).resolve().parents[3] / 'dm/mac'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs the macOS Swift toolchain')
class ControllerSettingsTests(unittest.TestCase):
    def test_session_choices(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'check'
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'swiftc', '-swift-version', '5', '-parse-as-library',
                                '-module-cache-path', str(Path(tmp) / 'module-cache'), '-o', str(exe),
                                str(MAC / 'ControllerSettings.swift'), str(MAC / 'ControllerLaunchChannel.swift'), str(MAC / 'Launcher.swift'),
                                str(MAC.parent / 'ios/Sources/LauncherSettings.swift'), str(MAC.parent / 'ios/Sources/LauncherRow.swift'),
                                str(MAC / 'tests/controller_settings_check.swift')],
                               capture_output=True, text=True, timeout=300)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('controller settings: 0 failed', r.stdout)
            server = Path(tmp) / 'registry'
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'clang', '-fobjc-arc', '-Wall', '-Werror', '-Wno-unused-function',
                                '-o', str(server), str(MAC.parent / 'host/tests/t_controller_setup_mac.m'),
                                '-framework', 'Foundation', '-framework', 'AppKit', '-framework', 'GameController'], capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(server)], capture_output=True, text=True, timeout=30)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            host, ui = socket.socketpair()
            with host, ui:
                with subprocess.Popen([str(server), '--server', str(host.fileno())], pass_fds=(host.fileno(),),
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as process:
                    host.close()
                    r = subprocess.run([str(exe), '--channel', str(ui.fileno())], pass_fds=(ui.fileno(),),
                                       capture_output=True, text=True, timeout=15)
                    ui.close()
                    out, err = process.communicate(timeout=10)
                    self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                    self.assertEqual(process.returncode, 0, out + err)
                    self.assertIn('controller channel: OK', r.stdout)


            # Exercise a request larger than the engine's read buffer, then cancellation.
            for apply in (True, False):
                host, ui = socket.socketpair()
                with host, ui, subprocess.Popen([str(server), '--server', str(host.fileno())], pass_fds=(host.fileno(),),
                                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as process:
                    host.close()
                    ui.settimeout(5)
                    received = b''
                    while b'\n' not in received:
                        received += ui.recv(4096)
                    self.assertIn('controllers', json.loads(received.split(b'\n')[0]))
                    if apply:
                        request = json.dumps({'environment': {'NX_CONTROLLER_TEST_START': 'from-window', 'NX_CONTROLLER_TEST_PADDING': 'x' * 12000},
                                              'choices': [], 'explicit': False}).encode() + b'\n'
                        # An Identify reply must never consume Play or end the connection.
                        identify = b'{"identify":"disconnected"}\n'
                        ui.sendall(identify + request[:317])
                        reply = ui.makefile('rb')
                        self.assertEqual(json.loads(reply.readline()), {'identified': 'disconnected', 'ok': False})
                        for offset in range(317, len(request), 317):
                            ui.sendall(request[offset:offset + 317])
                        self.assertEqual(json.loads(reply.readline()), {'started': True})
                        reply.close()
                    ui.shutdown(socket.SHUT_RDWR)
                    ui.close()
                    out, err = process.communicate(timeout=5)
                    self.assertEqual(process.returncode, 0 if apply else 1, out + err)


if __name__ == '__main__':
    unittest.main()
