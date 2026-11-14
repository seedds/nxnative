import Foundation
import Combine

@MainActor final class SensorStore:ObservableObject {
    static let shared = SensorStore()
    @Published var sensors:[Sensor]=[]
    @Published var error: String?
    private var startupConnected=false
    private let url:URL
    private var baseline=DSUConfiguration()
    private var watcher:Timer?
    init(url:URL = DSUConfiguration.userURL) {
        self.url=url
        do {
            let config=try DSUConfiguration.load(from:url)
            baseline=config
            for d in config.devices {
                let s=Sensor(id:d.id);s.railSL=d.railSL ?? -1;s.railSR=d.railSR ?? -1;s.role=d.role ?? .full;s.buttons=d.buttons ?? false;s.name=d.name;s.player=d.player;s.host=d.host;s.port=String(d.port);s.slot=d.slot
                // Controls edit the game-frame map directly. Renderer conversion is separate.
                s.axes=AxisMap(x:d.axes[0],y:d.axes[1],z:d.axes[2]);s.gyroBias=SIMD3(d.bias[0],d.bias[1],d.bias[2]);s.calibrated=d.calibrated ?? d.enabled
                // Preserve existing game mapping, ignoring legacy renderer-only axes.
                sensors.append(s);observe(s)
            }
        } catch { self.error=String(describing:error) }
        watcher=Timer.scheduledTimer(withTimeInterval:1,repeats:true) { [weak self] _ in
            Task { @MainActor [weak self] in self?.reloadShared() }
        }
    }
    deinit { watcher?.invalidate() }

    func reloadShared() {
        guard !sensors.contains(where: { $0.calibrating }) else { return }
        do {
            let latest=try DSUConfiguration.load(from:url)
            guard latest != baseline else { return }
            apply(latest);error=nil
        } catch { self.error="Cannot reload shared configuration: " + String(describing:error) }
    }
    private func apply(_ config:DSUConfiguration) {
        let previous=Dictionary(uniqueKeysWithValues:baseline.devices.map { ($0.id,$0) })
        let existing=Dictionary(uniqueKeysWithValues:sensors.map { ($0.id,$0) })
        for s in sensors where !config.devices.contains(where: { $0.id==s.id }) { s.disconnect();s.onEdit=nil }
        sensors=config.devices.map { d in
            if let s=existing[d.id],previous[d.id]==d { return s }
            let s=existing[d.id] ?? Sensor(id:d.id)
            let reconnect=s.running || startupConnected
            s.onEdit=nil;s.disconnect()
            s.name=d.name;s.player=d.player;s.host=d.host;s.port=String(d.port);s.slot=d.slot
            s.role=d.role ?? .full;s.buttons=d.buttons ?? false;s.railSL=d.railSL ?? -1;s.railSR=d.railSR ?? -1
            s.axes=AxisMap(x:d.axes[0],y:d.axes[1],z:d.axes[2])

            s.gyroBias=SIMD3(d.bias[0],d.bias[1],d.bias[2]);s.calibrated=d.calibrated ?? d.enabled
            observe(s);if reconnect { s.connect() };return s
        }
        baseline=config
    }
    func observe(_ s:Sensor) { s.onEdit = { [weak self] in self?.save() } }
    func add() { let s=Sensor();s.player=(1...8).first { p in !sensors.contains { $0.player==p } } ?? 1;s.name="Phone " + String(s.player);sensors.append(s);observe(s);save() }
    func remove(_ s:Sensor) { s.disconnect();sensors.removeAll { $0.id==s.id };save() }
    func connectSavedOnce() { reloadShared();guard !startupConnected else { return };startupConnected=true;for s in sensors { s.connect() } }
    func configuration() throws -> DSUConfiguration {
        var devices:[DSUDevice]=[]
        for s in sensors {
            let slot=s.slot<0 ? (s.detectedSlot ?? 0) : s.slot
            guard let port=Int(s.port) else { throw DSUConfiguration.Failure.invalid }
            devices.append(DSUDevice(id:s.id,name:s.name,player:s.player,host:s.host,port:port,slot:slot,axes:[s.axes.x,s.axes.y,s.axes.z],bias:[s.gyroBias.x,s.gyroBias.y,s.gyroBias.z],enabled:s.calibrated,role:s.role,railSL:s.railSL,railSR:s.railSR,buttons:s.buttons,calibrated:s.calibrated,displayAxes:nil))
        }
        let config=DSUConfiguration(devices:devices);_ = try config.environment();return config
    }
    func save() {
        do {
            let next=try configuration()
            try next.save(to:url,expected:baseline)
            baseline=next;error=nil
        } catch DSUConfiguration.Failure.conflict {
            // Never overwrite another app with this process's stale snapshot.
            if let latest=try? DSUConfiguration.load(from:url) { apply(latest) }
            error="Configuration changed in another app. Latest settings loaded. Please repeat your edit."
        } catch { self.error="Cannot save motion configuration: " + String(describing:error) }
    }
    func discovered(host:String,port:String,slot:Int) {
        if let existing=sensors.first(where: { $0.host==host && $0.port==port && ($0.slot==slot || $0.slot == -1) }) { if !existing.running { existing.connect() };return }
        add();guard let s=sensors.last else { return };s.onEdit=nil;s.host=host;s.port=port;s.slot=slot;observe(s);save();s.connect()
    }
}
