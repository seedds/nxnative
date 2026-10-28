import Foundation
import AppKit

public struct ConnectedAppleDevice: Identifiable, Equatable {
    public let id: String
    public let name: String
    public let model: String
    public let osVersion: String
    public let isPaired: Bool
}

public struct DeveloperSigningTeam: Identifiable, Equatable {
    public let id: String
    public let name: String
}

public enum DeploymentErrorCategory: Equatable {
    case signatureVerificationFailed
    case deviceLocked
    case developerModeDisabled
    case insufficientStorage
    case deviceDisconnected
    case invalidIPA
    case generic(String)
}

public struct DeploymentProgressInfo: Equatable {
    public enum Phase: String {
        case idle = "Aguardando"
        case extracting = "Extraindo Pacote (.ipa)"
        case signing = "Preparando Assinatura"
        case connecting = "Conectando ao iPhone"
        case transferring = "Transferindo para o iPhone"
        case installing = "Instalando no iOS"
        case completed = "Instalação Concluída"
        case failed = "Erro na Instalação"
        case cancelled = "Envio Cancelado"
    }

    public var phase: Phase = .idle
    public var progressFraction: Double = 0.0 // 0.0 ... 1.0
    public var statusMessage: String = ""
    public var detailMessage: String = ""
    public var logLines: [String] = []
    public var isRunning: Bool = false
    public var targetDeviceName: String = ""
    public var elapsedSeconds: Int = 0
    public var errorCategory: DeploymentErrorCategory? = nil
    public var rawError: String? = nil
}

public final class DeviceDeploymentService: ObservableObject {
    public static let shared = DeviceDeploymentService()

    @Published public var connectedDevices: [ConnectedAppleDevice] = []
    @Published public var signingTeams: [DeveloperSigningTeam] = []
    @Published public var isDetecting: Bool = false
    @Published public var deploymentStatus: String? = nil
    @Published public var activeDeployment: DeploymentProgressInfo = DeploymentProgressInfo()

    private var currentRunningProcess: Process?
    private var elapsedTimer: Timer?
    private var completionCallback: ((Bool, String) -> Void)?

    private init() {}

    public func refreshDevicesAndTeams() {
        isDetecting = true
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let devices = self?.fetchConnectedDevices() ?? []
            let teams = self?.fetchSigningIdentities() ?? []

            DispatchQueue.main.async {
                self?.connectedDevices = devices
                self?.signingTeams = teams
                self?.isDetecting = false
            }
        }
    }

    public func dismissDeploymentStatus() {
        if !activeDeployment.isRunning {
            activeDeployment = DeploymentProgressInfo()
        }
    }

    public func cancelDeployment() {
        guard activeDeployment.isRunning else { return }
        stopElapsedTimer()
        currentRunningProcess?.terminate()
        currentRunningProcess = nil

        activeDeployment.isRunning = false
        activeDeployment.phase = .cancelled
        activeDeployment.statusMessage = "Envio cancelado pelo usuário."
        activeDeployment.detailMessage = "O processo de instalação foi interrompido com segurança."

        let cb = completionCallback
        completionCallback = nil
        cb?(false, "Cancelado pelo usuário.")
    }

    public func shareViaAirDrop(fileURL: URL) {
        DispatchQueue.main.async {
            if let service = NSSharingService(named: .sendViaAirDrop) {
                if service.canPerform(withItems: [fileURL]) {
                    service.perform(withItems: [fileURL])
                }
            } else {
                NSWorkspace.shared.activateFileViewerSelecting([fileURL])
            }
        }
    }

    public func installApp(
        appBundleURL: URL,
        on device: ConnectedAppleDevice,
        completion: @escaping (Bool, String) -> Void
    ) {
        // Prevent concurrent deployments
        if activeDeployment.isRunning {
            completion(false, "Já existe uma instalação em andamento.")
            return
        }

        self.completionCallback = completion

        // Initialize active deployment state
        activeDeployment = DeploymentProgressInfo(
            phase: .extracting,
            progressFraction: 0.05,
            statusMessage: "Iniciando instalação no \(device.name)...",
            detailMessage: "Preparando arquivos do jogo para envio...",
            logLines: ["==> Iniciando implantação de \(appBundleURL.lastPathComponent) no \(device.name)..."],
            isRunning: true,
            targetDeviceName: device.name,
            elapsedSeconds: 0,
            errorCategory: nil,
            rawError: nil
        )

        startElapsedTimer()

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            var targetAppURL = appBundleURL

            // 1. If given an .ipa archive, extract Payload/*.app with cache support
            if appBundleURL.pathExtension.lowercased() == "ipa" {
                let (appURL, extractError) = self.prepareExtractedApp(from: appBundleURL)
                guard let extractedURL = appURL else {
                    DispatchQueue.main.async {
                        self.failDeployment(
                            category: .invalidIPA,
                            title: "Arquivo .ipa inválido",
                            detail: extractError ?? "Não foi possível extrair a pasta Payload/*.app do arquivo .ipa."
                        )
                    }
                    return
                }
                targetAppURL = extractedURL
            }

            guard self.activeDeployment.isRunning else { return }

            // 2. Prepare code signature
            DispatchQueue.main.async {
                self.activeDeployment.phase = .signing
                self.activeDeployment.progressFraction = 0.35
                self.activeDeployment.statusMessage = "Verificando assinatura do app..."
                self.activeDeployment.detailMessage = "Garantindo integridade do bundle antes do envio..."
            }

            self.applyCodeSignatureIfNeeded(targetAppURL: targetAppURL)

            guard self.activeDeployment.isRunning else { return }

            // 3. Execute xcrun devicectl device install app asynchronously with live line streaming
            DispatchQueue.main.async {
                self.activeDeployment.phase = .transferring
                self.activeDeployment.progressFraction = 0.45
                self.activeDeployment.statusMessage = "Enviando para \(device.name)..."
                self.activeDeployment.detailMessage = "Conectando ao dispositivo e transferindo arquivos..."
            }

            self.executeDeviceCtlInstall(targetAppURL: targetAppURL, device: device)
        }
    }

    // MARK: - App Extraction with Fast Cache
    private func prepareExtractedApp(from ipaURL: URL) -> (URL?, String?) {
        let fm = FileManager.default
        let values = try? ipaURL.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey])
        let size = values?.fileSize ?? 0
        let mtime = Int(values?.contentModificationDate?.timeIntervalSince1970 ?? 0)

        let safeName = ipaURL.deletingPathExtension().lastPathComponent.replacingOccurrences(of: " ", with: "_")
        let cacheDir = fm.urls(for: .cachesDirectory, in: .userDomainMask).first!
            .appendingPathComponent("nxnative/deploy_cache/\(safeName)_\(size)_\(mtime)")

        let payloadDir = cacheDir.appendingPathComponent("Payload")

        // Check if cache already contains a valid extracted .app
        if fm.fileExists(atPath: payloadDir.path),
           let items = try? fm.contentsOfDirectory(atPath: payloadDir.path),
           let existingApp = items.first(where: { $0.hasSuffix(".app") }) {
            let appURL = payloadDir.appendingPathComponent(existingApp)
            let plistURL = appURL.appendingPathComponent("Info.plist")
            if fm.fileExists(atPath: plistURL.path) {
                DispatchQueue.main.async {
                    self.activeDeployment.logLines.append("[OK] App extraído encontrado em cache local.")
                    self.activeDeployment.progressFraction = 0.30
                }
                return (appURL, nil)
            }
        }

        // Needs extraction
        try? fm.createDirectory(at: cacheDir, withIntermediateDirectories: true)

        DispatchQueue.main.async {
            self.activeDeployment.logLines.append("==> Extraindo \(ipaURL.lastPathComponent) (\(self.formatByteSize(Int64(size))))...")
            self.activeDeployment.statusMessage = "Extraindo pacote .ipa (\(self.formatByteSize(Int64(size))))..."
            self.activeDeployment.detailMessage = "Aguarde a descompactação dos arquivos do jogo..."
            self.activeDeployment.progressFraction = 0.15
        }

        // Try using /usr/bin/ditto -x -k first (fast multithreaded unpacker on macOS)
        let ditto = Process()
        ditto.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        ditto.arguments = ["-x", "-k", ipaURL.path, cacheDir.path]
        try? ditto.run()
        ditto.waitUntilExit()

        // Fallback to /usr/bin/unzip if ditto failed
        if ditto.terminationStatus != 0 {
            let unzip = Process()
            unzip.executableURL = URL(fileURLWithPath: "/usr/bin/unzip")
            unzip.arguments = ["-q", "-o", ipaURL.path, "-d", cacheDir.path]
            try? unzip.run()
            unzip.waitUntilExit()
        }

        guard let items = try? fm.contentsOfDirectory(atPath: payloadDir.path),
              let appFolder = items.first(where: { $0.hasSuffix(".app") }) else {
            return (nil, "O arquivo .ipa não possui uma pasta Payload/*.app válida.")
        }

        let appURL = payloadDir.appendingPathComponent(appFolder)
        DispatchQueue.main.async {
            self.activeDeployment.logLines.append("[OK] Pacote extraído com sucesso: \(appFolder)")
            self.activeDeployment.progressFraction = 0.30
        }
        return (appURL, nil)
    }

    // MARK: - Code Signing
    private func applyCodeSignatureIfNeeded(targetAppURL: URL) {
        let teams = fetchSigningIdentities()
        let sign = Process()
        sign.executableURL = URL(fileURLWithPath: "/usr/bin/codesign")

        if let team = teams.first {
            sign.arguments = ["--force", "--deep", "--sign", team.id, targetAppURL.path]
            DispatchQueue.main.async {
                self.activeDeployment.logLines.append("==> Assinando com identidade de desenvolvimento: \(team.name)")
            }
        } else {
            // Apply ad-hoc signature
            sign.arguments = ["--force", "--deep", "--sign", "-", targetAppURL.path]
            DispatchQueue.main.async {
                self.activeDeployment.logLines.append("==> Assinando bundle ad-hoc (--sign -)...")
            }
        }

        try? sign.run()
        sign.waitUntilExit()
    }

    // MARK: - Asynchronous devicectl Execution with Real-Time Pipe Streaming
    private func executeDeviceCtlInstall(targetAppURL: URL, device: ConnectedAppleDevice) {
        let process = Process()
        let pipeOut = Pipe()
        let pipeErr = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
        process.arguments = ["devicectl", "device", "install", "app", "--device", device.id, targetAppURL.path, "--verbose"]
        process.standardOutput = pipeOut
        process.standardError = pipeErr

        self.currentRunningProcess = process

        var fullOutput = ""
        let lock = NSLock()

        let handleOutputData: (Data) -> Void = { [weak self] data in
            guard let self = self, !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            lock.lock()
            fullOutput += text
            lock.unlock()

            let lines = text.components(separatedBy: .newlines)
            for line in lines {
                let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
                guard !trimmed.isEmpty else { continue }

                DispatchQueue.main.async {
                    self.activeDeployment.logLines.append(trimmed)
                    if self.activeDeployment.logLines.count > 60 {
                        self.activeDeployment.logLines.removeFirst()
                    }
                    self.parseDeviceCtlLine(trimmed)
                }
            }
        }

        pipeOut.fileHandleForReading.readabilityHandler = { handle in
            handleOutputData(handle.availableData)
        }
        pipeErr.fileHandleForReading.readabilityHandler = { handle in
            handleOutputData(handle.availableData)
        }

        process.terminationHandler = { [weak self] proc in
            pipeOut.fileHandleForReading.readabilityHandler = nil
            pipeErr.fileHandleForReading.readabilityHandler = nil

            let remOut = pipeOut.fileHandleForReading.readDataToEndOfFile()
            let remErr = pipeErr.fileHandleForReading.readDataToEndOfFile()
            handleOutputData(remOut)
            handleOutputData(remErr)

            lock.lock()
            let output = fullOutput
            lock.unlock()

            DispatchQueue.main.async {
                self?.handleDeviceCtlTermination(status: proc.terminationStatus, output: output, device: device)
            }
        }

        do {
            try process.run()
        } catch {
            pipeOut.fileHandleForReading.readabilityHandler = nil
            pipeErr.fileHandleForReading.readabilityHandler = nil
            DispatchQueue.main.async {
                self.failDeployment(
                    category: .generic(error.localizedDescription),
                    title: "Erro ao iniciar devicectl",
                    detail: error.localizedDescription
                )
            }
        }
    }

    private func parseDeviceCtlLine(_ line: String) {
        let lower = line.lowercased()

        if lower.contains("tunnel connection") {
            activeDeployment.progressFraction = max(activeDeployment.progressFraction, 0.52)
            activeDeployment.detailMessage = "Túnel USB / Rede estabelecido com o iPhone..."
        } else if lower.contains("developer disk image") {
            activeDeployment.progressFraction = max(activeDeployment.progressFraction, 0.62)
            activeDeployment.detailMessage = "Serviços de desenvolvimento ativos no iOS..."
        } else if lower.contains("usage assertion") {
            activeDeployment.progressFraction = max(activeDeployment.progressFraction, 0.72)
            activeDeployment.detailMessage = "Transferindo pacote do jogo para o aparelho..."
        } else if lower.contains("staging") || lower.contains("uploading") {
            activeDeployment.progressFraction = max(activeDeployment.progressFraction, 0.80)
            activeDeployment.detailMessage = "Transferindo arquivos para a memória do iPhone..."
        } else if lower.contains("install") || lower.contains("extracting") {
            activeDeployment.phase = .installing
            activeDeployment.progressFraction = max(activeDeployment.progressFraction, 0.88)
            activeDeployment.statusMessage = "Finalizando instalação no iOS..."
            activeDeployment.detailMessage = "O iOS está registrando o aplicativo no sistema..."
        }
    }

    private func handleDeviceCtlTermination(status: Int32, output: String, device: ConnectedAppleDevice) {
        stopElapsedTimer()
        currentRunningProcess = nil
        activeDeployment.isRunning = false

        if status == 0 {
            activeDeployment.phase = .completed
            activeDeployment.progressFraction = 1.0
            activeDeployment.statusMessage = "Port instalado com sucesso em \(device.name)!"
            activeDeployment.detailMessage = "O aplicativo já está instalado e disponível na tela de início do seu iPhone."
            activeDeployment.logLines.append("[SUCCESS] Instalação concluída com sucesso!")

            let cb = completionCallback
            completionCallback = nil
            cb?(true, activeDeployment.statusMessage)
            return
        }

        // Parse Failure Category
        if output.contains("0xe8008014") || output.contains("ApplicationVerificationFailed") || output.contains("invalid signature") || output.contains("integridade não pôde ser verificada") {
            failDeployment(
                category: .signatureVerificationFailed,
                title: "Falha na Verificação de Assinatura (iOS)",
                detail: "O iOS rejeitou o app porque ele requer uma assinatura com conta Apple Developer válida, ou deve ser instalado via AirDrop / AltStore / Sideloadly.",
                raw: output
            )
        } else if output.contains("passcodeRequired") || output.contains("locked") {
            failDeployment(
                category: .deviceLocked,
                title: "iPhone Bloqueado",
                detail: "Desbloqueie a tela do \(device.name) com seu código de acesso e tente novamente.",
                raw: output
            )
        } else if output.contains("Developer Mode") || output.contains("developerModeStatus") {
            failDeployment(
                category: .developerModeDisabled,
                title: "Modo Desenvolvedor Necessário",
                detail: "Ative o Modo Desenvolvedor no aparelho em Ajustes > Privacidade e Segurança > Modo de Desenvolvedor e reinicie o iPhone.",
                raw: output
            )
        } else if output.contains("no space") || output.contains("storage") || output.contains("disc full") {
            failDeployment(
                category: .insufficientStorage,
                title: "Armazenamento Insuficiente",
                detail: "Não há espaço livre suficiente na memória do \(device.name) para instalar este jogo.",
                raw: output
            )
        } else if output.contains("tunnel") || output.contains("disconnected") || output.contains("offline") {
            failDeployment(
                category: .deviceDisconnected,
                title: "Conexão Perdida com o iPhone",
                detail: "Verifique se o cabo USB está firme ou se ambos os aparelhos estão na mesma rede Wi-Fi.",
                raw: output
            )
        } else {
            let summary = output.trimmingCharacters(in: .whitespacesAndNewlines)
            failDeployment(
                category: .generic(summary),
                title: "Falha na Instalação no \(device.name)",
                detail: summary.isEmpty ? "Erro desconhecido do devicectl." : summary,
                raw: output
            )
        }
    }

    private func failDeployment(category: DeploymentErrorCategory, title: String, detail: String, raw: String? = nil) {
        stopElapsedTimer()
        currentRunningProcess = nil
        activeDeployment.isRunning = false
        activeDeployment.phase = .failed
        activeDeployment.errorCategory = category
        activeDeployment.statusMessage = title
        activeDeployment.detailMessage = detail
        activeDeployment.rawError = raw
        activeDeployment.logLines.append("[ERROR] \(title): \(detail)")

        let cb = completionCallback
        completionCallback = nil
        cb?(false, title)
    }

    // MARK: - Timer Management
    private func startElapsedTimer() {
        stopElapsedTimer()
        elapsedTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            DispatchQueue.main.async {
                guard let self = self, self.activeDeployment.isRunning else { return }
                self.activeDeployment.elapsedSeconds += 1
            }
        }
    }

    private func stopElapsedTimer() {
        elapsedTimer?.invalidate()
        elapsedTimer = nil
    }

    // MARK: - Utilities
    private func formatByteSize(_ bytes: Int64) -> String {
        let formatter = ByteCountFormatter()
        formatter.allowedUnits = [.useGB, .useMB]
        formatter.countStyle = .file
        return formatter.string(fromByteCount: bytes)
    }

    public func findBundleId(for titleId: String, on device: ConnectedAppleDevice) -> String? {
        let process = Process()
        let pipe = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
        process.arguments = ["devicectl", "device", "info", "apps", "--device", device.id]
        process.standardOutput = pipe
        process.standardError = Pipe()
        guard (try? process.run()) != nil else { return nil }
        process.waitUntilExit()

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        let out = String(decoding: data, as: UTF8.self)
        let tidLower = titleId.lowercased()

        for line in out.components(separatedBy: .newlines) {
            let lower = line.lowercased()
            if lower.contains(tidLower) {
                let tokens = line.split(whereSeparator: { $0.isWhitespace })
                if tokens.count >= 2 {
                    return String(tokens[1])
                }
            }
        }
        return nil
    }

    public func revealInFinder(fileURL: URL) {
        NSWorkspace.shared.activateFileViewerSelecting([fileURL])
    }

    private func fetchConnectedDevices() -> [ConnectedAppleDevice] {
        let process = Process()
        let pipe = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
        process.arguments = ["devicectl", "list", "devices", "--json-output", "-"]
        process.standardOutput = pipe
        process.standardError = Pipe()

        guard (try? process.run()) != nil else { return [] }
        process.waitUntilExit()
        guard process.terminationStatus == 0 else { return [] }

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        let raw = String(decoding: data, as: UTF8.self)

        guard let jsonStartIndex = raw.firstIndex(of: "{") else {
            return parseDeviceTable(raw)
        }

        let jsonString = String(raw[jsonStartIndex...])
        guard let jsonData = jsonString.data(using: .utf8),
              let json = try? JSONSerialization.jsonObject(with: jsonData) as? [String: Any],
              let res = json["result"] as? [String: Any],
              let list = res["devices"] as? [[String: Any]] else {
            return parseDeviceTable(raw)
        }

        var results: [ConnectedAppleDevice] = []
        for d in list {
            let devProps = (d["deviceProperties"] as? [String: Any]) ?? ((d["properties"] as? [String: Any])?["state"] as? [String: Any]) ?? [:]
            let hwProps = (d["hardwareProperties"] as? [String: Any]) ?? ((d["properties"] as? [String: Any])?["hardware"] as? [String: Any]) ?? [:]

            let name = (devProps["name"] as? String) ?? "iPhone"
            let udid = (hwProps["udid"] as? String) ?? (d["identifier"] as? String) ?? ""
            let model = (hwProps["marketingName"] as? String) ?? "Apple Device"

            var os = "iOS"
            if let osNum = devProps["osVersionNumber"] as? String {
                os = osNum
            } else if let osDict = devProps["osVersionNumber"] as? [String: Any], let s = osDict["stringValue"] as? String {
                os = s
            }

            guard !udid.isEmpty else { continue }

            results.append(
                ConnectedAppleDevice(
                    id: udid,
                    name: name,
                    model: model,
                    osVersion: os,
                    isPaired: true
                )
            )
        }

        return results.isEmpty ? parseDeviceTable(raw) : results
    }

    private func parseDeviceTable(_ text: String) -> [ConnectedAppleDevice] {
        var devices: [ConnectedAppleDevice] = []
        let lines = text.components(separatedBy: .newlines)
        for line in lines {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty || trimmed.hasPrefix("Name") || trimmed.hasPrefix("---") { continue }
            let parts = line.split(whereSeparator: { $0.isWhitespace })
            guard parts.count >= 3 else { continue }
            let name = String(parts[0])
            var id = ""
            for p in parts {
                if p.count >= 16 && (p.contains("-") || p.allSatisfy({ $0.isHexDigit })) {
                    id = String(p).replacingOccurrences(of: "(UDID)", with: "")
                    break
                }
            }
            if !id.isEmpty {
                devices.append(ConnectedAppleDevice(id: id, name: name, model: "Apple Device", osVersion: "iOS", isPaired: true))
            }
        }
        return devices
    }

    private func fetchSigningIdentities() -> [DeveloperSigningTeam] {
        let process = Process()
        let pipe = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/security")
        process.arguments = ["find-identity", "-p", "codesigning", "-v"]
        process.standardOutput = pipe

        guard (try? process.run()) != nil else { return [] }
        process.waitUntilExit()

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        let out = String(decoding: data, as: UTF8.self)

        var teams: [DeveloperSigningTeam] = []
        for line in out.components(separatedBy: .newlines) {
            if line.contains("Apple Development:") || line.contains("iPhone Developer:") {
                let parts = line.components(separatedBy: "\"")
                if parts.count >= 2 {
                    let teamName = parts[1]
                    teams.append(DeveloperSigningTeam(id: teamName, name: teamName))
                }
            }
        }
        return teams
    }
}
