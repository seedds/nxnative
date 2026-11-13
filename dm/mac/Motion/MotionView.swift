import SwiftUI
import Network
import SceneKit
import simd

@MainActor final class Sensor: ObservableObject, Identifiable {
    let id:UUID
    init(id:UUID = UUID()) { self.id=id }
    var onEdit: (() -> Void)?
    @Published var axes = AxisMap() { didSet { orientation = Orientation(); onEdit?() } }
    @Published var name = "Phone" { didSet { onEdit?() } }
    @Published var player = 1 { didSet { onEdit?() } }
    @Published var host = "192.168.1.100" { didSet { onEdit?() } }
    @Published var port = "26760" { didSet { onEdit?() } }
    @Published var slot = -1 { didSet { onEdit?() } }
    @Published var detectedSlot: Int?
    @Published var status = "Disconnected"
    @Published var running = false
    @Published var confirmed = false
    private var startedAt = Date()
    @Published var samples = 0
    @Published var received = 0
    @Published var sent = 0
    @Published var lastPacket = "No packets received"
    @Published var packetLog: [String] = []
    private var lastReply = Date.distantPast
    @Published var acceleration = SIMD3<Double>.zero
    @Published var gyro = SIMD3<Double>.zero
    @Published var orientation = Orientation()
    @Published var calibrating = false
    @Published var capture = MotionCapture()
    @Published var autoCalibration = AutoCalibration()
    var gyroBias = SIMD3<Double>.zero
    var flatGravity = SIMD3<Double>.zero
    var calibrated = false
    @Published var flatCentered = false
    func recenter(flat:Bool) {
        orientation.tare();orientation.resetClock();flatCentered=flat;onEdit?()
    }
    var displayRotation: simd_quatd {
        let base=simd_quatd(angle:Double.pi/2,axis:SIMD3(1,0,0))
        return calibrated && !flatCentered ? base * orientation.displayed : orientation.displayed
    }
    func applyCalibration(map:AxisMap,bias:SIMD3<Double>,gravity:SIMD3<Double>,preview:Orientation) {
        axes=map;gyroBias=bias;flatGravity=gravity;orientation=preview;calibrated=true;flatCentered=false;onEdit?()
    }
    private var connection: NWConnection?
    private var timer: Timer?
    private var lastSeen = Date.distantPast
    private var previous: (UInt32, UInt32)?
    private let clientID = UInt32.random(in: 1...UInt32.max)
    func connect() {
        disconnect()
        guard !host.trimmingCharacters(in:.whitespaces).isEmpty, let p = UInt16(port), p > 0, let endpoint = NWEndpoint.Port(rawValue:p) else { status = "Invalid IP or port"; return }
        detectedSlot = nil
        received = 0; sent = 0; packetLog = []; lastPacket = "No packets received"; lastReply = .distantPast
        running = true; confirmed = false; startedAt = Date(); lastSeen = .distantPast
        status = "Contacting phone — not confirmed"; samples = 0; previous = nil; orientation.resetClock()
        let c = NWConnection(host:NWEndpoint.Host(host),port:endpoint,using:.udp); connection = c
        c.stateUpdateHandler = { [weak self, weak c] state in
            Task { @MainActor in
                guard let self, let c, self.connection === c else { return }
                switch state {
                case .ready: self.status = "Waiting for a valid reply — not connected yet"; self.subscribe(); self.receive(c)
                case .failed(let e): self.disconnect(); self.status = e.localizedDescription
                default: break
                }
            }
        }
        c.start(queue:DispatchQueue(label:"dsu.(id)"))
        timer = Timer.scheduledTimer(withTimeInterval:1,repeats:true) { [weak self] _ in Task { @MainActor in
            guard let self else { return }; self.subscribe()
            let now = Date()
            if self.received == 0 && now.timeIntervalSince(self.startedAt) >= 6 {
                self.status = "NO RESPONSE YET — still sending requests. Check IP, port, slot and Local Network permission."
            } else if self.received > 0 && now.timeIntervalSince(self.lastReply) > 3 {
                self.confirmed = false; self.status = "SIGNAL LOST — no reply for 3 seconds"
            } else if self.received > 0 && self.samples == 0 {
                self.status = "REPLY RECEIVED — inspect packet details below"
            } else if self.samples > 0 && now.timeIntervalSince(self.lastSeen) > 3 {
                self.confirmed = false; self.status = "SIGNAL LOST — no valid motion for 3 seconds"; self.orientation.resetClock()
            }
        } }
    }
    func subscribe() {
        let payload: [UInt8] = slot < 0 ? [0,0,0,0,0,0,0,0] : [1,UInt8(slot),0,0,0,0,0,0]
        send(type:0x100000,payload:[])
        send(type:0x100001,payload:[4,0,0,0,0,1,2,3])
        send(type:0x100002,payload:payload)
    }
    func send(type: UInt32, payload: [UInt8]) {
        sent += 1
        connection?.send(content: DSU.packet(type:type,payload:payload,id:clientID),completion:.contentProcessed { [weak self] error in
            if let error { Task { @MainActor in self?.status = "Send error: " + error.localizedDescription } }
        })
    }
    func inspect(_ data: Data) {
        received += 1; lastReply = Date()
        let detail = DSU.describe(data, selectedSlot:slot)
        lastPacket = detail
        packetLog.insert(detail,at:0); if packetLog.count > 16 { packetLog.removeLast() }
    }
    func receive(_ c: NWConnection) {
        c.receiveMessage { [weak self, weak c] data, _, _, error in
            Task { @MainActor in
                guard let self, let c, self.connection === c else { return }
                if let error { self.disconnect(); self.status = error.localizedDescription; return }
                if let data { self.inspect(data) }
                if let data, let s = DSU.decode(data), self.slot < 0 || Int(s.slot) == self.slot {
                    if self.slot < 0 {
                        if self.detectedSlot == nil || Date().timeIntervalSince(self.lastSeen) > 3 {
                            self.detectedSlot = Int(s.slot); self.previous = nil; self.orientation.resetClock(); self.onEdit?()
                        }
                        // Lock to one live stream: never mix the orientations of different slots.
                        if self.detectedSlot != Int(s.slot) { self.receive(c); return }
                    }
                    let old = self.previous
                    let delta = old.map { s.sequence &- $0.1 } ?? 1
                    if old == nil || old!.0 != s.server || (delta > 0 && delta < 0x80000000) || Date().timeIntervalSince(self.lastSeen)>3 {
                        if old?.0 != s.server { self.orientation.resetClock() }
                        self.previous = (s.server,s.sequence); self.lastSeen = Date(); self.samples += 1
                        self.acceleration = s.acceleration; self.gyro = s.gyro; if self.calibrating { self.autoCalibration.add(s) }
                        var mapped = s; mapped.gyro = self.axes.apply(s.gyro-self.gyroBias)
                        if self.axes.valid { self.orientation.update(mapped) }; self.confirmed = true; self.status = "CONNECTED — receiving DSU slot " + String(s.slot)
                    }
                }
                self.receive(c)
            }
        }
    }
    func disconnect() { timer?.invalidate(); timer=nil; connection?.cancel(); connection=nil; running=false; confirmed=false; status="Disconnected" }
}

struct MotionBox: NSViewRepresentable {
    var rotation: simd_quatd
    var arrowAxis: SIMD3<Double>? = nil
    var arrowDirection: Double = 1
    var obliqueView = false
    func makeNSView(context:Context) -> SCNView {
        let v = SCNView(); let scene=SCNScene(); v.scene=scene; v.backgroundColor=NSColor(calibratedWhite:0.07,alpha:1)
        let node=SCNNode();node.name="sensor";scene.rootNode.addChildNode(node)
        func part(_ geometry:SCNGeometry,_ position:SCNVector3,_ color:NSColor,_ angle:Float=0) {
            geometry.firstMaterial?.diffuse.contents=color
            geometry.firstMaterial?.roughness.contents=0.65
            let n=SCNNode(geometry:geometry);n.position=position;n.eulerAngles.y=CGFloat(angle);node.addChildNode(n)
        }
        part(SCNBox(width:2.8,height:0.32,length:1.15,chamferRadius:0.25),SCNVector3(0,0,-0.1),.lightGray)
        part(SCNBox(width:0.85,height:0.38,length:1.65,chamferRadius:0.32),SCNVector3(-1.03,-0.02,0.48),.lightGray,-0.22)
        part(SCNBox(width:0.85,height:0.38,length:1.65,chamferRadius:0.32),SCNVector3(1.03,-0.02,0.48),.lightGray,0.22)
        // Four dark direction buttons on the left, four colored action buttons right.
        for (x,z) in [(-0.98,-0.40),(-0.98,0.06),(-1.21,-0.17),(-0.75,-0.17)] {
            part(SCNBox(width:0.19,height:0.10,length:0.19,chamferRadius:0.04),SCNVector3(x,0.22,z),.darkGray)
        }
        for (i,p) in [(0,SCNVector3(0.98,0.23,-0.40)),(1,SCNVector3(0.98,0.23,0.06)),(2,SCNVector3(0.75,0.23,-0.17)),(3,SCNVector3(1.21,0.23,-0.17))] {
            part(SCNSphere(radius:0.105),p,[NSColor.systemYellow,.systemBlue,.systemGreen,.systemRed][i])
        }
        for x:Float in [-0.21,0.21] { part(SCNBox(width:0.23,height:0.08,length:0.12,chamferRadius:0.04),SCNVector3(x,0.22,-0.05),.white) }
        for x:Float in [-0.52,0.52] { part(SCNCylinder(radius:0.17,height:0.12),SCNVector3(x,0.22,0.36),.darkGray) }
        for x:Float in [-0.9,0.9] { part(SCNBox(width:0.6,height:0.17,length:0.18,chamferRadius:0.07),SCNVector3(x,0.05,-0.72),.darkGray) }
        // Bright shoulder edge and two grips are readable even without text.
        part(SCNBox(width:0.65,height:0.08,length:0.10,chamferRadius:0.03),SCNVector3(0,0.18,-0.66),.white)
        let camera=SCNNode(); camera.name="camera"; camera.camera=SCNCamera(); camera.camera?.usesOrthographicProjection=true; camera.camera?.orthographicScale=2.15; camera.position=SCNVector3(0,3.4,4.8); camera.look(at:SCNVector3Zero); scene.rootNode.addChildNode(camera)
        v.autoenablesDefaultLighting=true; return v
    }
    func updateNSView(_ view:SCNView,context:Context) {
        if let camera=view.scene?.rootNode.childNode(withName:"camera",recursively:false) {
            camera.position=obliqueView ? SCNVector3(3.5,2.8,4.8) : SCNVector3(0,3.4,4.8)
            camera.look(at:SCNVector3Zero)
        }
        updateArrow(view)
        view.scene?.rootNode.childNode(withName:"sensor",recursively:false)?.simdOrientation = simd_quatf(ix:Float(rotation.imag.x),iy:Float(rotation.imag.y),iz:Float(rotation.imag.z),r:Float(rotation.real))
    }
}
extension MotionBox {
    func updateArrow(_ view:SCNView) {
        guard let scene=view.scene else { return }
        scene.rootNode.childNode(withName:"guide",recursively:false)?.removeFromParentNode()
        guard let axis=arrowAxis else { return }
        let guide=SCNNode();guide.name="guide";guide.simdOrientation=simd_quatf(angle:Float.pi/2,axis:SIMD3<Float>(1,0,0));scene.rootNode.addChildNode(guide)
        let n=SIMD3<Float>(Float(axis.x),Float(axis.y),Float(axis.z))
        let u=simd_normalize(simd_cross(n,abs(n.x)<0.9 ? SIMD3<Float>(1,0,0):SIMD3<Float>(0,0,1)))
        let v=simd_cross(n,u);let sign=Float(arrowDirection)
        func point(_ t:Float)->SIMD3<Float> { (u*cos(t)+v*sin(t))*1.8 }
        for i in 0..<30 {
            let a=sign*(Float(i)/30*2-0.7),b=sign*(Float(i+1)/30*2-0.7)
            let from=point(a),to=point(b),d=to-from
            let g=SCNCylinder(radius:0.035,height:CGFloat(simd_length(d)));g.firstMaterial?.diffuse.contents=NSColor.systemOrange
            let part=SCNNode(geometry:g);part.simdPosition=(from+to)/2;part.simdOrientation=simd_quatf(from:SIMD3<Float>(0,1,0),to:simd_normalize(d));guide.addChildNode(part)
        }
        let t=sign*1.3,tip=point(t),tangent=simd_normalize((-u*sin(t)+v*cos(t))*sign)
        let cone=SCNCone(topRadius:0,bottomRadius:0.15,height:0.35);cone.firstMaterial?.diffuse.contents=NSColor.systemOrange
        let head=SCNNode(geometry:cone);head.simdPosition=tip+tangent*0.1;head.simdOrientation=simd_quatf(from:SIMD3<Float>(0,1,0),to:tangent);guide.addChildNode(head)
    }
}
struct AxisPicker: View {
    var title:String
    @Binding var selection:Int
    var body: some View { Picker(title,selection:$selection) { ForEach([1,-1,2,-2,3,-3],id: \.self) { a in LocalizedText((a<0 ? "−" : "+") + ["X / pitch","Y / yaw","Z / roll"][abs(a)-1]).tag(a) } } }
}
@available(macOS 14.0, *)
struct SensorCard: View {
    @ObservedObject var sensor:Sensor
    @State private var showCalibration = false
    @State private var showRecenter = false
    var index:Int
    var remove:()->Void
    func vector(_ v:SIMD3<Double>) -> String { String(format:"% .3f   % .3f   % .3f",v.x,v.y,v.z) }
    var body:some View {
        VStack(alignment:.leading,spacing:12) {
            HStack { TextField(L("Name"),text:$sensor.name).font(.title2.bold()); Picker(L("Player"),selection:$sensor.player) { ForEach(1..<9) { LocalizedText(String($0)).tag($0) } }.frame(width:120); Button(L("Remove"),action:remove) }
            HStack {
                TextField(L("Phone IP"),text:$sensor.host).frame(minWidth:130)
                TextField(L("Port"),text:$sensor.port).frame(width:75)
                Picker(L("DSU slot"),selection:$sensor.slot) { LocalizedText("Auto").tag(-1); ForEach(0..<4) { LocalizedText("\($0)").tag($0) } }.frame(width:125)
            }.disabled(sensor.running)
            HStack {
                Button(L(sensor.running ? "Disconnect":"Connect")) { if sensor.running { sensor.disconnect() } else { sensor.connect() } }
                Button(L("Calibrate…")) { showCalibration = true }.disabled(!sensor.confirmed)
                Button(L("Recenter")) { showRecenter=true }.disabled(!sensor.confirmed)
                Spacer(); LocalizedText("\(sensor.samples) samples").monospacedDigit()
            }
            DisclosureGroup(L("Axis mapping (saved for this phone)")) {
                AxisPicker(title:"Display X · tilt toward you",selection:$sensor.axes.x)
                AxisPicker(title:"Display Y · turn flat on table",selection:$sensor.axes.y)
                AxisPicker(title:"Display Z · lean left/right",selection:$sensor.axes.z)
                LocalizedText("Input X = DSU pitch, Y = yaw, Z = roll. Choose each input once. Minus reverses direction. Mapping changes reset orientation.").font(.caption)
                if !sensor.axes.valid { LocalizedText("Choose three different input axes.").foregroundStyle(.red) }
                Button(L("Swap X / Y")) { let a=sensor.axes; sensor.axes=AxisMap(x:a.y,y:a.x,z:a.z) }
            }
            MotionBox(rotation:sensor.displayRotation).frame(height:210).clipShape(RoundedRectangle(cornerRadius:12))
            LocalizedText("Buttons face you · white shoulder edge = top · two grips = bottom").font(.caption)
            HStack { Circle().fill(sensor.confirmed ? Color.green : (sensor.running ? Color.orange : Color.red)).frame(width:10,height:10); LocalizedText(sensor.status).fontWeight(.semibold) }
            LocalizedText("Sent: \(sensor.sent) requests · Received: \(sensor.received) packets").monospacedDigit()
            LocalizedText(sensor.lastPacket).font(.system(.caption,design:.monospaced)).textSelection(.enabled)
            DisclosureGroup(L("Recent packets (all types and slots)")) {
                ScrollView { LocalizedText(sensor.packetLog.joined(separator:"\n\n")).font(.system(.caption2,design:.monospaced)).textSelection(.enabled).frame(maxWidth:.infinity,alignment:.leading) }.frame(height:150)
            }
            LocalizedText("Acceleration (g): \(vector(sensor.acceleration))").font(.system(.caption,design:.monospaced))
            LocalizedText("Gyro (°/s):       \(vector(sensor.gyro))").font(.system(.caption,design:.monospaced))
        }.padding(18).background(.background,in:RoundedRectangle(cornerRadius:16))
        .sheet(isPresented:$showCalibration) { CalibrationView(sensor:sensor) }
        .confirmationDialog(L("Recenter without changing axes"),isPresented:$showRecenter,titleVisibility:.visible) {
            Button(L("Flat on table")) { sensor.recenter(flat:true) }
            Button(L("Holding like a wheel")) { sensor.recenter(flat:false) }
            Button(L("Cancel"),role:.cancel) {}
        } message: { LocalizedText("Hold still, then choose the current pose. Axis mapping and gyro bias stay unchanged.") }
    }
}
@available(macOS 14.0, *)
public struct DSUMotionView:View {
    public init() {}
    @ObservedObject private var store = SensorStore.shared
    @Environment(\.dismiss) private var dismiss
    @ObservedObject private var language = AppLanguage.shared
    @StateObject private var discovery = Discovery()
    public var body:some View {
        VStack(alignment:.leading,spacing:14) {
            HStack { VStack(alignment:.leading) { LocalizedText("Phone Motion Setup").font(.largeTitle.bold()); LocalizedText("Phone motion over Cemuhook UDP · local network only").foregroundStyle(.secondary) }; Spacer(); Button(L("Add connection")) { store.add() } }
            Picker(L("Language"),selection:$language.code) { LocalizedText("Português").tag("pt-BR"); LocalizedText("English").tag("en"); LocalizedText("Español").tag("es"); LocalizedText("繁體中文").tag("zh-Hant") }.frame(width:260)
            HStack { Toggle(L("Automatic discovery"),isOn:$discovery.enabled).onChange(of:discovery.enabled) { _,on in if on { discovery.start() } else { discovery.stop() } }; TextField(L("UDP port"),text:$discovery.port).frame(width:85).disabled(discovery.enabled); LocalizedText(discovery.status).font(.caption) }
            LocalizedText("Open DSUController on your phone. Enter its IP, port and slot. Use Calibrate for three guided movements. Hold the phone horizontally like a controller. Orientation integrates the gyroscope and may drift.").font(.callout).foregroundStyle(.secondary)
            if let error=store.error { LocalizedText(error).foregroundStyle(.red) }
            else { LocalizedText("Changes saved automatically. Close this window, check the connection indicators, then Play.").font(.caption).foregroundStyle(.secondary) }
            ScrollView { LazyVGrid(columns:[GridItem(.adaptive(minimum:420),spacing:16)],spacing:16) {
                ForEach(Array(store.sensors.enumerated()),id: \.element.id) { i,s in SensorCard(sensor:s,index:i+1) { s.disconnect(); store.remove(s) } }
            } }
            HStack { Spacer(); Button(L("Done")) { store.save(); if store.error == nil { discovery.stop(); dismiss() } }.keyboardShortcut(.defaultAction) }
        }.padding(22).frame(minWidth:520,minHeight:580).background(Color(nsColor:.windowBackgroundColor))
        .onAppear { store.connectSavedOnce();discovery.found = { host,slot in store.discovered(host:host,port:discovery.port,slot:slot) } }
        .onDisappear { discovery.stop() }
    }
}
