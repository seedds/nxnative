import Foundation
import Network
import Darwin

enum LocalNetwork {
    static func hosts() -> [String] {
        var first: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&first) == 0 else { return [] }
        defer { freeifaddrs(first) }
        var result = Set<String>()
        var next = first
        while let item = next {
            defer { next = item.pointee.ifa_next }
            let flags = Int32(item.pointee.ifa_flags)
            guard flags & IFF_UP != 0, flags & IFF_LOOPBACK == 0,
                  let a = item.pointee.ifa_addr, let m = item.pointee.ifa_netmask,
                  a.pointee.sa_family == UInt8(AF_INET) else { continue }
            let name = String(cString:item.pointee.ifa_name)
            guard name.hasPrefix("en") || name.hasPrefix("bridge") else { continue }
            let ip = a.withMemoryRebound(to:sockaddr_in.self,capacity:1) { UInt32(bigEndian:$0.pointee.sin_addr.s_addr) }
            let mask = m.withMemoryRebound(to:sockaddr_in.self,capacity:1) { UInt32(bigEndian:$0.pointee.sin_addr.s_addr) }
            let start = ip & mask, end = start | ~mask
            // Never expand a large corporate/VPN subnet into an unbounded scan.
            guard end > start, end-start <= 1023 else { continue }
            for h in (start+1)..<end where h != ip {
                result.insert([24,16,8,0].map { String((h >> $0)&255) }.joined(separator:"."))
            }
        }
        return result.sorted()
    }
}

@MainActor final class Discovery: ObservableObject {
    @Published var enabled = false
    @Published var port = "26760"
    @Published var status = "Automatic discovery off"
    var found: ((String,Int)->Void)?
    private var task: Task<Void,Never>?
    private var probes: [UUID:NWConnection] = [:]
    private var reported = Set<String>()
    func start() {
        stop(); enabled = true; reported = []
        guard let p = UInt16(port),p>0 else { enabled=false;status="Invalid discovery port";return }
        task = Task { [weak self] in
            guard let self else { return }
            while !Task.isCancelled {
                let hosts=LocalNetwork.hosts()
                self.status=hosts.isEmpty ? "No small local IPv4 subnet found (limit 1024 addresses). Use manual connection." : "Searching \(hosts.count) local addresses on UDP \(p)…"
                for i in stride(from:0,to:hosts.count,by:24) {
                    if Task.isCancelled { return }
                    for host in hosts[i..<min(i+24,hosts.count)] { self.probe(host,p) }
                    try? await Task.sleep(for:.seconds(1))
                }
                if Task.isCancelled { return }
                self.status="Found \(self.reported.count) active controller(s). Searching again in 15 seconds."
                try? await Task.sleep(for:.seconds(15))
            }
        }
    }
    func probe(_ host:String,_ port:UInt16) {
        let id=UUID(), c=NWConnection(host:NWEndpoint.Host(host),port:NWEndpoint.Port(rawValue:port)!,using:.udp)
        probes[id]=c
        c.stateUpdateHandler = { [weak self,weak c] state in Task { @MainActor in
            guard let self,let c,self.probes[id] === c else { return }
            if case .ready = state {
                c.send(content:DSU.packet(type:0x100001,payload:[4,0,0,0,0,1,2,3],id:0x4d4c4142),completion:.contentProcessed { _ in })
                self.receive(c,id,host,port)
            }
        } }
        c.start(queue:DispatchQueue(label:"dsu.discovery"))
        Task { [weak self,weak c] in
            try? await Task.sleep(for:.seconds(1))
            c?.cancel();self?.probes.removeValue(forKey:id)
        }
    }
    func receive(_ c:NWConnection,_ id:UUID,_ host:String,_ port:UInt16) {
        c.receiveMessage { [weak self,weak c] data,_,_,error in Task { @MainActor in
            guard let self,let c,self.enabled,self.probes[id] === c else { return }
            if let data,let slot=DSU.connectedSlot(data) {
                let key=host+":"+String(port)+":"+String(slot)
                if self.reported.insert(key).inserted { self.found?(host,Int(slot)) }
            }
            if error == nil { self.receive(c,id,host,port) }
        } }
    }
    func stop() { task?.cancel();task=nil;for c in probes.values { c.cancel() };probes=[:];enabled=false;status="Automatic discovery off" }
}
