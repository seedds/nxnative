# NXRemote Companion App

A native macOS companion application built with SwiftUI for **nxnative WebRTC Remote Play**.

---

## Features

- **LAN IP Detection**: Automatically detects your active Wi-Fi / Ethernet local IP address (`en0` / `en1`).
- **QR Code Generator**: Displays an on-screen QR Code generated with CoreImage that mobile phones can scan to connect directly.
- **Protocol Toggle**: Easy toggle between standard **HTTP (port 8888)** and secure **HTTPS (port 8443)** for gyroscope permissions.
- **Resolution / Quality Selector**: Choose between **480p (Default)** for ultra-low latency mobile streaming and **720p (HD 60 FPS)** for higher visual clarity.
- **Multi-Player Slot Linker**: Generate direct links for Player 1, Player 2, Player 3, or Player 4.
- **Game Launcher**: Drag and drop any `.app` game built with nxnative to launch it with WebRTC streaming enabled.

---

## How to Build

To recompile `NXRemote` from `main.swift`:

```bash
cd tools/nxremote
swiftc -O -target arm64-apple-macos14.0 -framework Cocoa -framework SwiftUI -framework CoreImage main.swift -o NXRemote
```

For full details on the underlying WebRTC video, audio, and gamepad architecture, see [dm/host/webrtc/README.md](../../dm/host/webrtc/README.md).
