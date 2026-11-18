import Cocoa
import SwiftUI
import CoreImage.CIFilterBuiltins
import UniformTypeIdentifiers

// Helper: Deteksi IP LAN Wi-Fi / Ethernet
func getLocalIPAddress() -> String {
    var address: String = "127.0.0.1"
    var ifaddr: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&ifaddr) == 0, let firstAddr = ifaddr else { return address }
    for ptr in sequence(first: firstAddr, next: { $0.pointee.ifa_next }) {
        let interface = ptr.pointee
        let addrFamily = interface.ifa_addr.pointee.sa_family
        if addrFamily == UInt8(AF_INET) {
            let name = String(cString: interface.ifa_name)
            if name == "en0" || name == "en1" {
                var hostname = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                getnameinfo(interface.ifa_addr, socklen_t(interface.ifa_addr.pointee.sa_len),
                            &hostname, socklen_t(hostname.count),
                            nil, socklen_t(0), NI_NUMERICHOST)
                let ip = String(cString: hostname)
                if !ip.isEmpty && ip != "127.0.0.1" {
                    address = ip
                    break
                }
            }
        }
    }
    freeifaddrs(ifaddr)
    return address
}

// Helper: Generator QR Code Native CoreImage
func generateQRCode(from string: String) -> NSImage? {
    let context = CIContext()
    let filter = CIFilter.qrCodeGenerator()
    filter.message = Data(string.utf8)
    filter.correctionLevel = "M"
    if let outputImage = filter.outputImage {
        let transformed = outputImage.transformed(by: CGAffineTransform(scaleX: 10, y: 10))
        if let cgImage = context.createCGImage(transformed, from: transformed.extent) {
            return NSImage(cgImage: cgImage, size: NSSize(width: 170, height: 170))
        }
    }
    return nil
}

struct ContentView: View {
    @State private var appPath: String = {
        let defaultApp = "/Applications/Mario Kart World.app"
        return FileManager.default.fileExists(atPath: defaultApp) ? defaultApp : ""
    }()
    @State private var isTargeted = false
    @State private var enableWebRTC = true
    @State private var controllerOnly = false
    @State private var useHTTPS = false
    @State private var showGameSettings = false
    @State private var portString = "8888"
    @State private var localIP: String = getLocalIPAddress()
    @State private var isGameRunning = false
    @State private var activeProcess: Process? = nil
    @State private var copiedAlert = false

    var fullURL: String {
        let mode = controllerOnly ? "controller" : "stream"
        if useHTTPS {
            return "https://\(localIP):8443/?mode=\(mode)&auto=1"
        }
        return "http://\(localIP):\(portString)/?mode=\(mode)&auto=1"
    }

    var displayURL: String {
        if useHTTPS {
            return "https://\(localIP):8443"
        }
        return "http://\(localIP):\(portString)"
    }

    var appIcon: NSImage? {
        guard !appPath.isEmpty, FileManager.default.fileExists(atPath: appPath) else { return nil }
        return NSWorkspace.shared.icon(forFile: appPath)
    }

    var appName: String {
        if appPath.isEmpty { return "Drop Game App (.app) here" }
        return URL(fileURLWithPath: appPath).deletingPathExtension().lastPathComponent
    }

    var body: some View {
        VStack(spacing: 0) {
            // Header Bar
            HStack {
                HStack(spacing: 8) {
                    Image(systemName: "gamecontroller.fill")
                        .foregroundColor(.cyan)
                        .font(.system(size: 18))
                    Text("NXRemote Hub")
                        .font(.headline)
                        .fontWeight(.bold)
                }
                Spacer()
                if isGameRunning {
                    HStack(spacing: 6) {
                        Circle().fill(Color.green).frame(width: 8, height: 8)
                        Text("Game Running")
                            .font(.caption)
                            .foregroundColor(.green)
                    }
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(Color.green.opacity(0.15))
                    .cornerRadius(6)
                }
            }
            .padding(.horizontal, 20)
            .padding(.top, 16)
            .padding(.bottom, 12)

            Divider()

            ScrollView {
                VStack(spacing: 16) {
                    // Drop Zone / Game Selector
                    ZStack {
                        RoundedRectangle(cornerRadius: 12)
                            .fill(isTargeted ? Color.cyan.opacity(0.15) : Color(NSColor.controlBackgroundColor))
                            .overlay(
                                RoundedRectangle(cornerRadius: 12)
                                    .strokeBorder(isTargeted ? Color.cyan : Color.gray.opacity(0.3), style: StrokeStyle(lineWidth: isTargeted ? 2 : 1, dash: [6]))
                            )

                        HStack(spacing: 16) {
                            if let icon = appIcon {
                                Image(nsImage: icon)
                                    .resizable()
                                    .frame(width: 54, height: 54)
                            } else {
                                Image(systemName: "square.and.arrow.down")
                                    .font(.system(size: 32))
                                    .foregroundColor(.gray)
                            }

                            VStack(alignment: .leading, spacing: 4) {
                                Text(appName)
                                    .font(.system(size: 14, weight: .bold))
                                if !appPath.isEmpty {
                                    Text(appPath)
                                        .font(.caption2)
                                        .foregroundColor(.secondary)
                                        .lineLimit(1)
                                        .truncationMode(.middle)
                                } else {
                                    Text("Drag & drop an extracted .app or click Browse")
                                        .font(.caption)
                                        .foregroundColor(.secondary)
                                }
                            }

                            Spacer()

                            Button("Browse...") {
                                selectApp()
                            }
                            .buttonStyle(.bordered)
                        }
                        .padding(14)
                    }
                    .frame(height: 82)
                    .onDrop(of: [.fileURL], isTargeted: $isTargeted) { providers in
                        guard let provider = providers.first else { return false }
                        _ = provider.loadObject(ofClass: URL.self) { url, _ in
                            if let url = url, url.pathExtension == "app" {
                                DispatchQueue.main.async {
                                    self.appPath = url.path
                                }
                            }
                        }
                        return true
                    }

                    // WebRTC Options
                    VStack(alignment: .leading, spacing: 10) {
                        Text("Remote Control Settings")
                            .font(.subheadline)
                            .fontWeight(.semibold)

                        Toggle(isOn: $enableWebRTC) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Enable WebRTC Remote Server")
                                    .font(.system(size: 13, weight: .medium))
                                Text("Allows smartphones and tablets on LAN to connect as wireless controllers.")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }
                        }
                        .toggleStyle(.switch)

                        if enableWebRTC {
                            Toggle(isOn: $controllerOnly) {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("🎮 Controller Only Mode")
                                        .font(.system(size: 13, weight: .medium))
                                    Text("Watch game on Mac while phone acts as steering wheel & gamepad (saves phone battery).")
                                        .font(.caption)
                                        .foregroundColor(.secondary)
                                }
                            }
                            .toggleStyle(.switch)

                            Toggle(isOn: $useHTTPS) {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("🔒 Use HTTPS (Port 8443 - Required for Mobile Gyro)")
                                        .font(.system(size: 13, weight: .medium))
                                    Text("Android Chrome & Safari require HTTPS to activate motion/gyro sensors.")
                                        .font(.caption)
                                        .foregroundColor(.secondary)
                                }
                            }
                            .toggleStyle(.switch)

                            HStack {
                                Text("Server Port:")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                                TextField("Port", text: $portString)
                                    .textFieldStyle(.roundedBorder)
                                    .frame(width: 80)
                                Spacer()
                            }
                        }

                        Divider().padding(.vertical, 4)

                        Toggle(isOn: $showGameSettings) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Show game settings window before launch")
                                    .font(.system(size: 13, weight: .medium))
                                Text("Displays the game's native resolution/graphics/language settings window.")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }
                        }
                        .toggleStyle(.checkbox)
                    }
                    .padding(14)
                    .background(Color(NSColor.controlBackgroundColor))
                    .cornerRadius(12)

                    // QR Code & URL Section
                    if enableWebRTC {
                        VStack(spacing: 12) {
                            HStack {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("Scan to Connect Phone")
                                        .font(.subheadline)
                                        .fontWeight(.semibold)
                                    Text("Point your phone's camera at the QR code below:")
                                        .font(.caption)
                                        .foregroundColor(.secondary)
                                }
                                Spacer()
                                Button("Refresh IP") {
                                    localIP = getLocalIPAddress()
                                }
                                .font(.caption)
                            }

                            HStack(spacing: 20) {
                                if let qr = generateQRCode(from: fullURL) {
                                    Image(nsImage: qr)
                                        .resizable()
                                        .interpolation(.none)
                                        .scaledToFit()
                                        .frame(width: 140, height: 140)
                                        .padding(6)
                                        .background(Color.white)
                                        .cornerRadius(8)
                                        .shadow(radius: 2)
                                }

                                VStack(alignment: .leading, spacing: 10) {
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text("Local Wi-Fi Address:")
                                            .font(.caption2)
                                            .foregroundColor(.secondary)
                                        Text(displayURL)
                                            .font(.system(size: 13, weight: .semibold, design: .monospaced))
                                            .foregroundColor(.cyan)
                                    }

                                    HStack(spacing: 8) {
                                        Button(action: {
                                            NSPasteboard.general.clearContents()
                                            NSPasteboard.general.setString(fullURL, forType: .string)
                                            copiedAlert = true
                                            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                                                copiedAlert = false
                                            }
                                        }) {
                                            Label(copiedAlert ? "Copied!" : "Copy Link", systemImage: copiedAlert ? "checkmark" : "doc.on.doc")
                                        }
                                        .buttonStyle(.bordered)

                                        Button(action: {
                                            if let url = URL(string: fullURL) {
                                                NSWorkspace.shared.open(url)
                                            }
                                        }) {
                                            Label("Open", systemImage: "safari")
                                        }
                                        .buttonStyle(.bordered)
                                    }

                                    Text("💡 Tilt your phone left/right to steer like a real wheel!")
                                        .font(.caption2)
                                        .foregroundColor(.orange)
                                }
                                Spacer()
                            }
                        }
                        .padding(14)
                        .background(Color(NSColor.controlBackgroundColor))
                        .cornerRadius(12)
                    }
                }
                .padding(20)
            }

            Divider()

            // Footer / Play Button
            HStack {
                Button("Quit") {
                    if let proc = activeProcess, proc.isRunning {
                        proc.terminate()
                    }
                    NSApplication.shared.terminate(nil)
                }
                .buttonStyle(.bordered)

                Spacer()

                Button(action: {
                    if isGameRunning {
                        stopGame()
                    } else {
                        launchGame()
                    }
                }) {
                    HStack(spacing: 6) {
                        Image(systemName: isGameRunning ? "stop.fill" : "play.fill")
                        Text(isGameRunning ? "Stop Game" : "Launch & Play")
                            .fontWeight(.bold)
                    }
                    .frame(minWidth: 140)
                    .padding(.vertical, 4)
                }
                .buttonStyle(.borderedProminent)
                .tint(isGameRunning ? .red : .cyan)
                .disabled(appPath.isEmpty)
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 14)
        }
        .frame(width: 520, height: 600)
    }

    func selectApp() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [UTType.application]

        if panel.runModal() == .OK, let url = panel.url {
            self.appPath = url.path
        }
    }

    func launchGame() {
        guard !appPath.isEmpty else { return }

        let proc = Process()
        let launcherPath = (appPath as NSString).appendingPathComponent("Contents/MacOS/nxnative")
        let dmhostPath = (appPath as NSString).appendingPathComponent("Contents/MacOS/dmhost")

        if FileManager.default.fileExists(atPath: launcherPath) {
            proc.executableURL = URL(fileURLWithPath: launcherPath)
        } else if FileManager.default.fileExists(atPath: dmhostPath) {
            proc.executableURL = URL(fileURLWithPath: dmhostPath)
        } else {
            proc.executableURL = URL(fileURLWithPath: "/usr/bin/open")
            proc.arguments = [appPath]
        }

        var env = ProcessInfo.processInfo.environment
        env["DM_LAUNCHER"] = showGameSettings ? "1" : "0"
        if enableWebRTC {
            env["DM_WEBRTC"] = "1"
            env["DM_WEBRTC_PORT"] = portString
            if controllerOnly {
                env["DM_CONTROLLER_ONLY"] = "1"
            }
        }
        proc.environment = env

        do {
            try proc.run()
            self.activeProcess = proc
            self.isGameRunning = true

            // Watch process termination
            DispatchQueue.global(qos: .background).async {
                proc.waitUntilExit()
                DispatchQueue.main.async {
                    self.isGameRunning = false
                    self.activeProcess = nil
                }
            }
        } catch {
            print("Failed to run game:", error)
        }
    }

    func stopGame() {
        if let proc = activeProcess, proc.isRunning {
            proc.terminate()
        }
        self.activeProcess = nil
        self.isGameRunning = false
    }
}

// App Entry Point
class AppDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow!

    func applicationDidFinishLaunching(_ notification: Notification) {
        let contentView = ContentView()

        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 520, height: 600),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        window.center()
        window.title = "NXRemote Controller Hub"
        window.contentView = NSHostingView(rootView: contentView)
        window.makeKeyAndOrderFront(nil)
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return true
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.run()
