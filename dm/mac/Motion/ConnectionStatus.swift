import SwiftUI

struct PhoneRolePicker: View {
    @ObservedObject var sensor: Sensor
    var body: some View {
        VStack(alignment:.leading) {
        Picker("Controller role",selection:$sensor.role) {
            ForEach(DSURole.allCases,id: \.self) { role in Text(role.title).tag(role) }
        }.help("Explicit role, not DSU slot or model. Left uses D-pad and L stick; Right uses face buttons and R stick. No rail buttons are synthesized. Motion-only does not change controller style.")
        if sensor.role != .full {
            Picker("SL source",selection:$sensor.railSL) { Text("Not assigned").tag(-1);ForEach(DSU.Inputs.buttonNames,id: \.0) { Text($0.1).tag($0.0) } }
            Picker("SR source",selection:$sensor.railSR) { Text("Not assigned").tag(-1);ForEach(DSU.Inputs.buttonNames,id: \.0) { Text($0.1).tag($0.0) } }
            Text("Choose the received button shown when pressing SL or SR on the phone.").font(.caption)
        }
        }
    }
}

struct PhoneInputState: View {
    @ObservedObject var sensor: Sensor
    var body: some View {
        VStack(alignment:.leading,spacing:2) {
            Text(sensor.confirmed ? "Buttons: " + sensor.inputs.horizontalPreview(role:sensor.role,railSL:sensor.railSL,railSR:sensor.railSR).pressedNames : "Buttons: — (no live input)")
            Text(String(format:"Sticks (X,Y): L (%+d,%+d)  R (%+d,%+d)",sensor.inputs.leftStick.x,sensor.inputs.leftStick.y,sensor.inputs.rightStick.x,sensor.inputs.rightStick.y))
            Text("Centered at 0 · range −128…127 · Y up positive. DSU model does not identify left/right controller role.")
                .foregroundStyle(.secondary)
        }.font(.system(.caption2,design:.monospaced)).monospacedDigit()
    }
}

/// Pre-play monitoring uses saved endpoints only. Discovery belongs to Setup.
@available(macOS 14.0, *)
public struct PhoneMotionConnections: View {
    @ObservedObject private var store = SensorStore.shared
    public init() {}
    public var body: some View {
        Group {
            if store.sensors.isEmpty { LocalizedText("No saved phone controllers. Open Phone Motion Setup to add one.").foregroundStyle(.secondary) }
            ForEach(store.sensors) { sensor in PhoneMotionConnectionRow(sensor:sensor) }
            if let error=store.error { Text(error).foregroundStyle(.red) }
        }.onAppear { store.connectSavedOnce() }
    }
}
@available(macOS 14.0, *)
private struct PhoneMotionConnectionRow: View {
    @ObservedObject var sensor:Sensor
    var state:String {
        if !sensor.confirmed { return sensor.running ? "Connecting — no recent samples" : "Disconnected" }
        return sensor.calibrated ? "Receiving motion · ready" : "Receiving motion · calibration required"
    }
    var body: some View {
        VStack(alignment:.leading,spacing:4) {
            HStack {
                Circle().fill(sensor.confirmed ? Color.green : Color.red).frame(width:8,height:8).accessibilityLabel(L(state))
                Text(sensor.name); Spacer(); Text(L("Player")+" "+String(sensor.player)).foregroundStyle(.secondary)
            }
            LocalizedText(state).font(.caption).foregroundStyle(.secondary)
            Toggle(L("Use phone buttons and sticks"),isOn:$sensor.buttons).toggleStyle(.switch).controlSize(.small)
            PhoneRolePicker(sensor:sensor)
            PhoneInputState(sensor:sensor)
            Text(String(format:"X %+.2f   Y %+.2f   Z %+.2f · %d samples",sensor.gyro.x,sensor.gyro.y,sensor.gyro.z,sensor.samples))
                .font(.system(.caption2,design:.monospaced)).foregroundStyle(.tertiary).monospacedDigit()
        }.accessibilityElement(children:.combine)
    }
}
