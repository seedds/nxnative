// Private socket inherited by the engine and its wrapper. No names, preferences or
// enumeration indices are used to identify hardware across the two processes.
import Foundation
import Darwin

final class ControllerLaunchChannel {
    let fd: Int32
    var snapshot: (([LauncherController]) -> Void)?
    var identified: ((String, Bool) -> Void)?
    var failed: ((String) -> Void)?
    private let queue = DispatchQueue(label: "nxnative.controller-launch")
    private let reply = DispatchSemaphore(value: 0)
    private let lock = NSLock()
    private var accepted = false
    private var finished = false

    init(fd: Int32) { self.fd = fd }

    func cancel() {
        lock.lock(); finished = true; lock.unlock()
        shutdown(fd, SHUT_RDWR)
    }

    func listen() {
        queue.async { [self] in
            var buffer = Data()
            var bytes = [UInt8](repeating: 0, count: 4096)
            while true {
                let n = recv(fd, &bytes, bytes.count, 0)
                if n < 0 && errno == EINTR { continue }
                if n <= 0 { fail("The controller connection closed. Quit and open the game again."); return }
                buffer.append(contentsOf: bytes.prefix(n))
                if buffer.count > 262144 { fail("The controller response could not be read."); return }
                while let end = buffer.firstIndex(of: 10) {
                    let line = buffer.subdata(in: buffer.startIndex..<end)
                    buffer.removeSubrange(buffer.startIndex...end)
                    guard let object = try? JSONSerialization.jsonObject(with: line) as? [String: Any] else {
                        fail("The controller response could not be read."); return
                    }
                    if let rows = object["controllers"], let data = try? JSONSerialization.data(withJSONObject: rows),
                       let devices = try? JSONDecoder().decode([LauncherController].self, from: data) {
                        DispatchQueue.main.async { self.snapshot?(devices) }
                    } else if let id = object["identified"] as? String, let ok = object["ok"] as? Bool {
                        DispatchQueue.main.async { self.identified?(id, ok) }
                    } else if object["started"] as? Bool == true {
                        lock.lock(); accepted = true; finished = true; lock.unlock()
                        reply.signal()
                        return
                    } else { fail(object["error"] as? String ?? "The controller setup could not be applied."); return }
                }
            }
        }
    }

    private func fail(_ message: String) {
        lock.lock(); let report = !finished; finished = true; lock.unlock()
        if report {
            fputs("[RED] launcher: \(message)\n", stderr)
            DispatchQueue.main.async { self.failed?(message) }
            reply.signal()
        }
    }

    private func sendMessage(_ message: [String: Any]) -> Bool {
        guard var data = try? JSONSerialization.data(withJSONObject: message) else { return false }
        data.append(10)
        return data.withUnsafeBytes { bytes -> Bool in
            var offset = 0
            while offset < bytes.count {
                let n = send(fd, bytes.baseAddress!.advanced(by: offset), bytes.count - offset, 0)
                if n < 0 && errno == EINTR { continue }
                if n <= 0 { return false }
                offset += n
            }
            return true
        }
    }

    func identify(_ id: String) -> Bool { sendMessage(["identify": id]) }

    func start(environment: [String: String], choices: [[String: Any]], explicit: Bool) -> Bool {
        let sent = sendMessage(["environment": environment, "choices": choices, "explicit": explicit])
        guard sent, reply.wait(timeout: .now() + 5) == .success else { return false }
        lock.lock(); defer { lock.unlock() }
        return accepted
    }
}
