import Foundation
import AppKit

public final class DiagnosticsService {
    public static let shared = DiagnosticsService()

    private init() {}

    public func generateAndCopyReport(lastLogLines: [String] = []) -> String {
        var sections: [String] = []

        sections.append("### NxNative Launcher Diagnostic Report")
        sections.append("- **App Version:** 1.0.0 (Native Apple Silicon)")
        sections.append("- **macOS Version:** \(ProcessInfo.processInfo.operatingSystemVersionString)")
        sections.append("- **Hardware:** \(getHardwareModel()) (\(ProcessInfo.processInfo.activeProcessorCount) cores, \(getPhysicalMemoryGB()) RAM)")

        let gitInfo = getGitInfo()
        sections.append("- **Repository:** `\(gitInfo.branch)` @ `\(gitInfo.commit)`")

        let pythonInfo = getPythonEnvironment()
        sections.append("- **Python Runtime:** \(pythonInfo)")

        let dotnetInfo = getDotnetSDKs()
        sections.append("- **.NET SDKs:** \(dotnetInfo)")

        if !lastLogLines.isEmpty {
            sections.append("\n#### Recent Build Log (Sanitized, Last \(min(lastLogLines.count, 50)) Lines)")
            sections.append("```text")
            let sanitized = lastLogLines.suffix(50).map { sanitizeLine($0) }
            sections.append(sanitized.joined(separator: "\n"))
            sections.append("```")
        }

        let fullReport = sections.joined(separator: "\n")
        copyToClipboard(fullReport)
        return fullReport
    }

    private func sanitizeLine(_ line: String) -> String {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        var out = line.replacingOccurrences(of: home, with: "~")

        // Redact any hex keys or prod.keys assignments
        if out.contains("=") {
            let parts = out.components(separatedBy: "=")
            if parts.count == 2 {
                let val = parts[1].trimmingCharacters(in: .whitespaces)
                if val.count >= 32 && val.range(of: "^[0-9a-fA-F]+$", options: .regularExpression) != nil {
                    out = "\(parts[0])= [REDACTED_HEX_KEY]"
                }
            }
        }
        return out
    }

    private func getHardwareModel() -> String {
        var size: Int = 0
        sysctlbyname("machdep.cpu.brand_string", nil, &size, nil, 0)
        var machine = [CChar](repeating: 0, count: size)
        sysctlbyname("machdep.cpu.brand_string", &machine, &size, nil, 0)
        let brand = String(cString: machine)
        return brand.isEmpty ? "Apple Silicon" : brand
    }

    private func getPhysicalMemoryGB() -> String {
        let mem = ProcessInfo.processInfo.physicalMemory
        let gb = Double(mem) / (1024.0 * 1024.0 * 1024.0)
        return String(format: "%.1f GB Unified", gb)
    }

    private func getGitInfo() -> (branch: String, commit: String) {
        let repoDir = "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative"
        let branch = runProcess(executable: "/usr/bin/git", args: ["-C", repoDir, "rev-parse", "--abbrev-ref", "HEAD"]).trimmingCharacters(in: .whitespacesAndNewlines)
        let commit = runProcess(executable: "/usr/bin/git", args: ["-C", repoDir, "rev-parse", "--short", "HEAD"]).trimmingCharacters(in: .whitespacesAndNewlines)
        return (branch.isEmpty ? "main" : branch, commit.isEmpty ? "unknown" : commit)
    }

    private func getPythonEnvironment() -> String {
        let out = runProcess(executable: "/usr/bin/python3", args: ["--version"]).trimmingCharacters(in: .whitespacesAndNewlines)
        return out.isEmpty ? "python3 (system)" : out
    }

    private func getDotnetSDKs() -> String {
        let out = runProcess(executable: "/usr/local/share/dotnet/dotnet", args: ["--list-sdks"]).trimmingCharacters(in: .whitespacesAndNewlines)
        if out.isEmpty {
            let userHome = FileManager.default.homeDirectoryForCurrentUser.path
            let homeDotnet = "\(userHome)/.dotnet/dotnet"
            let homeOut = runProcess(executable: homeDotnet, args: ["--list-sdks"]).trimmingCharacters(in: .whitespacesAndNewlines)
            return homeOut.isEmpty ? "None detected" : homeOut.replacingOccurrences(of: "\n", with: ", ")
        }
        return out.replacingOccurrences(of: "\n", with: ", ")
    }

    private func runProcess(executable: String, args: [String]) -> String {
        guard FileManager.default.fileExists(atPath: executable) else { return "" }
        let p = Process()
        let pipe = Pipe()
        p.executableURL = URL(fileURLWithPath: executable)
        p.arguments = args
        p.standardOutput = pipe
        p.standardError = Pipe()
        guard (try? p.run()) != nil else { return "" }
        p.waitUntilExit()
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        return String(decoding: data, as: UTF8.self)
    }

    private func copyToClipboard(_ text: String) {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(text, forType: .string)
    }
}
