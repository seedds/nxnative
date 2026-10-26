import Foundation

public final class PatchService {
    public static let shared = PatchService()
    
    private init() {}
    
    public func checkPatchStatuses(nxnativePath: String) -> [EnginePatchItem] {
        let root = URL(fileURLWithPath: nxnativePath)
        let fm = FileManager.default
        
        return EnginePatchRegistry.patches.map { def in
            let fileURL = root.appendingPathComponent(def.relativePath)
            guard fm.fileExists(atPath: fileURL.path),
                  let content = try? String(contentsOf: fileURL, encoding: .utf8) else {
                return EnginePatchItem(id: def.id, name: def.name, summary: def.summary, relativePath: def.relativePath, isApplied: false)
            }
            let applied = content.contains(def.checkToken)
            return EnginePatchItem(id: def.id, name: def.name, summary: def.summary, relativePath: def.relativePath, isApplied: applied)
        }
    }
    
    @discardableResult
    public func applyPatchesIfNeeded(
        nxnativePath: String,
        logger: ((String, Bool) -> Void)? = nil
    ) -> (applied: Int, skipped: Int, errors: [String]) {
        let root = URL(fileURLWithPath: nxnativePath)
        let fm = FileManager.default
        
        var appliedCount = 0
        var skippedCount = 0
        var errorMessages: [String] = []
        
        for def in EnginePatchRegistry.patches {
            let fileURL = root.appendingPathComponent(def.relativePath)
            guard fm.fileExists(atPath: fileURL.path) else {
                let msg = "Skipped \(def.name): file not found at \(def.relativePath)"
                logger?(msg, false)
                continue
            }
            
            guard let content = try? String(contentsOf: fileURL, encoding: .utf8) else {
                let msg = "Error reading \(def.relativePath)"
                errorMessages.append(msg)
                logger?(msg, false)
                continue
            }
            
            // If already contains token, it's either merged upstream or previously applied
            if content.contains(def.checkToken) {
                skippedCount += 1
                logger?("[Patch] \(def.name): Already applied or upstream merged.", false)
                continue
            }
            
            // Perform non-destructive transformation
            guard let modified = def.applyTransform(content) else {
                let msg = "[Patch] Warning: Pattern anchor not found for \(def.name) in \(def.relativePath)"
                errorMessages.append(msg)
                logger?(msg, false)
                continue
            }
            
            do {
                try modified.write(to: fileURL, atomically: true, encoding: .utf8)
                appliedCount += 1
                logger?("[Patch] Successfully applied: \(def.name)", true)
            } catch {
                let msg = "[Patch] Failed to write \(def.relativePath): \(error.localizedDescription)"
                errorMessages.append(msg)
                logger?(msg, false)
            }
        }
        
        return (appliedCount, skippedCount, errorMessages)
    }
}
