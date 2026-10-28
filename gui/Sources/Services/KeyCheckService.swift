import Foundation
import CommonCrypto

public enum KeyCheckOutcome: Equatable, Sendable {
    case fits(firmware: String, revision: Int)
    case unreadableKeys
    case noHeaderKey
    case wrongHeaderKey(fileName: String)
    case outdatedMasterKey(generation: Int, neededKey: String, suggestedFirmware: String)
    case cannotCheck(fileName: String)
}

public final class KeyCheckService {
    public static let shared = KeyCheckService()

    public static let firmwareRevisions = [
        "1.0.0", "3.0.0", "3.0.1", "4.0.0", "5.0.0", "6.0.0", "6.2.0", "7.0.0",
        "8.1.0", "9.0.0", "9.1.0", "12.1.0", "13.0.0", "14.0.0", "15.0.0", "16.0.0",
        "17.0.0", "18.0.0", "19.0.0", "20.0.0"
    ]

    private init() {}

    public func verify(keysURL: URL, gameFiles: [URL]) -> KeyCheckOutcome {
        guard let keysText = try? String(contentsOf: keysURL, encoding: .utf8) else {
            return .unreadableKeys
        }

        let keys = parseKeys(keysText)
        guard let headerKey = keys["header_key"], headerKey.count == 32 else {
            return .noHeaderKey
        }

        for file in gameFiles {
            let ext = file.pathExtension.lowercased()
            guard ext == "nsp" || ext == "nsz" else {
                continue
            }

            let container = readContainer(file.path)
            switch container {
            case .unsupported:
                return .cannotCheck(fileName: file.lastPathComponent)
            case .pfs0(let ncas):
                var openedAtLeastOne = false
                var highestRevision = 0
                var missingKeyName: String?

                for nca in ncas {
                    guard let decrypted = xts(nca.header, key: headerKey, encrypt: false),
                          let req = needs(header: decrypted) else {
                        continue
                    }

                    openedAtLeastOne = true
                    if req.revision > highestRevision {
                        highestRevision = req.revision
                    }

                    if !hasKey(req.key, in: keys) {
                        missingKeyName = req.key
                    }
                }

                if !openedAtLeastOne && !ncas.isEmpty {
                    return .wrongHeaderKey(fileName: file.lastPathComponent)
                }

                if let missing = missingKeyName {
                    let gen = highestRevision + 1
                    let fw = gen <= Self.firmwareRevisions.count ? Self.firmwareRevisions[gen - 1] : "Unknown"
                    return .outdatedMasterKey(generation: gen, neededKey: missing, suggestedFirmware: fw)
                }

                let fw = (highestRevision < Self.firmwareRevisions.count) ? Self.firmwareRevisions[highestRevision] : "Current"
                return .fits(firmware: fw, revision: highestRevision)
            }
        }

        return .cannotCheck(fileName: gameFiles.first?.lastPathComponent ?? "None")
    }

    private func parseKeys(_ text: String) -> [String: [UInt8]] {
        var keys: [String: [UInt8]] = [:]
        for line in text.split(whereSeparator: \.isNewline) {
            let parts = line.split(separator: "=", maxSplits: 1)
            guard parts.count == 2 else { continue }
            let name = parts[0].trimmingCharacters(in: .whitespaces).lowercased()
            if let value = hexToBytes(parts[1].trimmingCharacters(in: .whitespaces)), !name.isEmpty {
                keys[name] = value
            }
        }
        return keys
    }

    private func hexToBytes(_ s: String) -> [UInt8]? {
        let chars = Array(s.utf8)
        guard !chars.isEmpty, chars.count % 2 == 0 else { return nil }
        var out: [UInt8] = []
        out.reserveCapacity(chars.count / 2)
        for i in stride(from: 0, to: chars.count, by: 2) {
            guard let hi = hexNibble(chars[i]), let lo = hexNibble(chars[i + 1]) else { return nil }
            out.append(hi << 4 | lo)
        }
        return out
    }

    private func hexNibble(_ c: UInt8) -> UInt8? {
        switch c {
        case 0x30...0x39: return c - 0x30
        case 0x41...0x46: return c - 0x41 + 10
        case 0x61...0x66: return c - 0x61 + 10
        default: return nil
        }
    }

    private func xts(_ data: [UInt8], key: [UInt8], sector: UInt64 = 0, sectorSize: Int = 0x200, encrypt: Bool) -> [UInt8]? {
        guard key.count == 32, sectorSize > 0, sectorSize % 16 == 0, data.count % sectorSize == 0 else { return nil }
        var out: [UInt8] = []
        out.reserveCapacity(data.count)
        for (i, start) in stride(from: 0, to: data.count, by: sectorSize).enumerated() {
            var tweak = [UInt8](repeating: 0, count: 16)
            var n = sector + UInt64(i)
            for b in (8..<16).reversed() { tweak[b] = UInt8(n & 0xFF); n >>= 8 }
            guard let s = xtsSector(Array(data[start..<start + sectorSize]), dataKey: Array(key[0..<16]), tweakKey: Array(key[16..<32]), tweak: tweak, encrypt: encrypt) else { return nil }
            out += s
        }
        return out
    }

    private func xtsSector(_ data: [UInt8], dataKey: [UInt8], tweakKey: [UInt8], tweak: [UInt8], encrypt: Bool) -> [UInt8]? {
        guard data.count % 16 == 0, var t = ecb(tweak, key: tweakKey, encrypt: true) else { return nil }
        var masks: [UInt8] = []
        masks.reserveCapacity(data.count)
        for _ in 0..<(data.count / 16) {
            masks += t
            var carry: UInt8 = 0
            for j in 0..<16 { let c = t[j] >> 7; t[j] = t[j] << 1 | carry; carry = c }
            if carry != 0 { t[0] ^= 0x87 }
        }
        guard let y = ecb(zip(data, masks).map { $0 ^ $1 }, key: dataKey, encrypt: encrypt) else { return nil }
        return zip(y, masks).map { $0 ^ $1 }
    }

    private func ecb(_ data: [UInt8], key: [UInt8], encrypt: Bool) -> [UInt8]? {
        var out = [UInt8](repeating: 0, count: data.count)
        var moved = 0
        let status = CCCrypt(CCOperation(encrypt ? kCCEncrypt : kCCDecrypt), CCAlgorithm(kCCAlgorithmAES), CCOptions(kCCOptionECBMode), key, key.count, nil, data, data.count, &out, out.count, &moved)
        return status == kCCSuccess && moved == data.count ? out : nil
    }

    private func needs(header h: [UInt8]) -> (revision: Int, key: String)? {
        guard h.count >= 0x400 else { return nil }
        let magic = String(bytes: h[0x200..<0x204], encoding: .ascii) ?? ""
        guard ["NCA3", "NCA2"].contains(magic) else { return nil }
        let generation = Int(max(h[0x206], h[0x220]))
        let revision = generation == 0 ? 0 : generation - 1
        let suffix = String(format: "%02x", revision)
        if h[0x230..<0x240].contains(where: { $0 != 0 }) {
            return (revision, "titlekek_" + suffix)
        }
        let area = ["application", "ocean", "system"]
        guard Int(h[0x207]) < area.count else { return nil }
        return (revision, "key_area_key_\(area[Int(h[0x207])])_" + suffix)
    }

    private func hasKey(_ key: String, in keys: [String: [UInt8]]) -> Bool {
        if keys[key]?.count == 16 { return true }
        guard let suffix = key.split(separator: "_").last else { return false }
        return keys["master_key_" + suffix]?.count == 16
    }

    private enum ContainerResult {
        case pfs0([(header: [UInt8], ncz: Bool)])
        case unsupported
    }

    private func readContainer(_ path: String) -> ContainerResult {
        guard let f = FileHandle(forReadingAtPath: path) else { return .unsupported }
        defer { try? f.close() }

        func read(_ offset: UInt64, _ count: Int) -> [UInt8]? {
            guard (try? f.seek(toOffset: offset)) != nil,
                  let d = try? f.read(upToCount: count), d.count == count else { return nil }
            return Array(d)
        }

        guard let magic = read(0, 4), String(bytes: magic, encoding: .ascii) == "PFS0",
              let countBytes = read(4, 4) else { return .unsupported }

        let count = Int(countBytes[0]) | Int(countBytes[1]) << 8 | Int(countBytes[2]) << 16 | Int(countBytes[3]) << 24
        guard count > 0, count < 256 else { return .unsupported }

        var headers: [(header: [UInt8], ncz: Bool)] = []
        let entrySize = 24
        guard let table = read(16, count * entrySize) else { return .unsupported }

        for i in 0..<min(count, 8) {
            let off = i * entrySize
            let ncaOffset = UInt64(table[off]) | UInt64(table[off+1]) << 8 | UInt64(table[off+2]) << 16 | UInt64(table[off+3]) << 24 |
                            UInt64(table[off+4]) << 32 | UInt64(table[off+5]) << 40 | UInt64(table[off+6]) << 48 | UInt64(table[off+7]) << 56
            if let ncaHeader = read(ncaOffset, 0x400) {
                headers.append((header: ncaHeader, ncz: false))
            }
        }
        return headers.isEmpty ? .unsupported : .pfs0(headers)
    }
}
