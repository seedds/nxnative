import Foundation

public final class ZipExtractor {
    public static let shared = ZipExtractor()
    
    private init() {}
    
    /// Estimates the uncompressed size of the .zip file without extracting
    public func estimateUncompressedSize(zipURL: URL) -> Int64? {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/unzip")
        process.arguments = ["-l", zipURL.path]
        
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = Pipe()
        
        do {
            try process.run()
            process.waitUntilExit()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            guard let text = String(data: data, encoding: .utf8) else { return nil }
            
            let lines = text.components(separatedBy: .newlines)
            for line in lines.reversed() {
                let trimmed = line.trimmingCharacters(in: .whitespaces)
                if trimmed.contains("files") {
                    let parts = trimmed.components(separatedBy: .whitespaces).filter { !$0.isEmpty }
                    if let first = parts.first, let bytes = Int64(first) {
                        return bytes
                    }
                }
            }
        } catch {
            return nil
        }
        return nil
    }
    
    /// Checks if destination volume has sufficient free disk space
    public func hasSufficientSpace(forBytes requiredBytes: Int64, at destinationURL: URL) -> (hasSpace: Bool, available: Int64) {
        do {
            let values = try destinationURL.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeAvailableCapacityKey])
            let capImportant = values.volumeAvailableCapacityForImportantUsage
            let capNormal = values.volumeAvailableCapacity.map { Int64($0) }
            let available: Int64 = capImportant ?? capNormal ?? 0
            let safetyMargin: Int64 = 1024 * 1024 * 1024 // 1 GB safety buffer
            return (available > (requiredBytes + safetyMargin), available)
        } catch {
            return (true, 0)
        }
    }
    
    /// Extracts .zip directly into destination folder on the target volume
    public func extract(
        zipURL: URL,
        destinationFolder: URL,
        progressCallback: @escaping (String) -> Void,
        completion: @escaping (Result<URL, Error>) -> Void
    ) {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            
            // 1. Pre-flight disk space check
            if let uncompressed = self.estimateUncompressedSize(zipURL: zipURL) {
                let spaceCheck = self.hasSufficientSpace(forBytes: uncompressed, at: destinationFolder)
                if !spaceCheck.hasSpace {
                    let reqStr = ByteCountFormatter.string(fromByteCount: uncompressed, countStyle: .file)
                    let availStr = ByteCountFormatter.string(fromByteCount: spaceCheck.available, countStyle: .file)
                    let err = NSError(
                        domain: "ZipExtractor",
                        code: 1,
                        userInfo: [NSLocalizedDescriptionKey: "Insufficient disk space: ~\(reqStr) required, \(availStr) available. Please free space before extracting."]
                    )
                    DispatchQueue.main.async { completion(.failure(err)) }
                    return
                }
            }
            
            // 2. Prepare target game directory
            let gameFolderName = zipURL.deletingPathExtension().lastPathComponent
            let targetDir = destinationFolder.appendingPathComponent(gameFolderName)
            
            do {
                try FileManager.default.createDirectory(at: targetDir, withIntermediateDirectories: true)
            } catch {
                DispatchQueue.main.async { completion(.failure(error)) }
                return
            }
            
            DispatchQueue.main.async { progressCallback("Extracting \(zipURL.lastPathComponent)...") }
            
            // 3. Run unzip
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/unzip")
            process.arguments = ["-q", "-o", zipURL.path, "-d", targetDir.path]
            
            let pipe = Pipe()
            process.standardError = pipe
            
            do {
                try process.run()
                process.waitUntilExit()
                
                if process.terminationStatus == 0 {
                    DispatchQueue.main.async {
                        progressCallback("Extraction completed successfully.")
                        completion(.success(targetDir))
                    }
                } else {
                    let errData = pipe.fileHandleForReading.readDataToEndOfFile()
                    let errMsg = String(data: errData, encoding: .utf8) ?? "Failed to extract archive."
                    let err = NSError(domain: "ZipExtractor", code: Int(process.terminationStatus), userInfo: [NSLocalizedDescriptionKey: errMsg])
                    DispatchQueue.main.async { completion(.failure(err)) }
                }
            } catch {
                DispatchQueue.main.async { completion(.failure(error)) }
            }
        }
    }
}
