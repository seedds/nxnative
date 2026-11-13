import Foundation
import Combine

@MainActor final class SensorStore:ObservableObject {
    static let shared = SensorStore()
    @Published var sensors:[Sensor]=[]
    @Published var error: String?
    private var startupConnected=false
    private let url:URL
    init(url:URL = DSUConfiguration.userURL) {
        self.url=url
        do {
            let config=try DSUConfiguration.load(from:url)
            for d in config.devices {
                let s=Sensor(id:d.id);s.railSL=d.railSL ?? -1;s.railSR=d.railSR ?? -1;s.role=d.role ?? .full;s.buttons=d.buttons ?? false;s.name=d.name;s.player=d.player;s.host=d.host;s.port=String(d.port);s.slot=d.slot
                // Switch x/right,y/forward,z/up -> preview x/right,y/up,z/back.
                s.axes=AxisMap(x:d.axes[0],y:d.axes[2],z:-d.axes[1]);s.gyroBias=SIMD3(d.bias[0],d.bias[1],d.bias[2]);s.calibrated=d.calibrated ?? d.enabled
                if let axes=d.displayAxes,axes.count==3 { s.axes=AxisMap(x:axes[0],y:axes[1],z:axes[2]) }
                sensors.append(s);observe(s)
            }
        } catch { self.error=String(describing:error) }
    }
    func observe(_ s:Sensor) { s.onEdit = { [weak self] in self?.save() } }
    func add() { let s=Sensor();s.player=(1...8).first { p in !sensors.contains { $0.player==p } } ?? 1;s.name="Phone " + String(s.player);sensors.append(s);observe(s);save() }
    func remove(_ s:Sensor) { s.disconnect();sensors.removeAll { $0.id==s.id };save() }
    func connectSavedOnce() { guard !startupConnected else { return };startupConnected=true;for s in sensors { s.connect() } }
    func configuration() throws -> DSUConfiguration {
        var devices:[DSUDevice]=[]
        for s in sensors {
            let slot=s.slot<0 ? (s.detectedSlot ?? 0) : s.slot
            guard let port=Int(s.port) else { throw DSUConfiguration.Failure.invalid }
            devices.append(DSUDevice(id:s.id,name:s.name,player:s.player,host:s.host,port:port,slot:slot,axes:[s.axes.x,-s.axes.z,s.axes.y],bias:[s.gyroBias.x,s.gyroBias.y,s.gyroBias.z],enabled:s.calibrated,role:s.role,railSL:s.railSL,railSR:s.railSR,buttons:s.buttons,calibrated:s.calibrated,displayAxes:[s.axes.x,s.axes.y,s.axes.z]))
        }
        let config=DSUConfiguration(devices:devices);_ = try config.environment();return config
    }
    func save() {
        do { try configuration().save(to:url);error=nil } catch { self.error="Cannot save motion configuration: " + String(describing:error) }
    }
    func discovered(host:String,port:String,slot:Int) {
        if let existing=sensors.first(where: { $0.host==host && $0.port==port && ($0.slot==slot || $0.slot == -1) }) { if !existing.running { existing.connect() };return }
        add();guard let s=sensors.last else { return };s.onEdit=nil;s.host=host;s.port=port;s.slot=slot;observe(s);save();s.connect()
    }
}
