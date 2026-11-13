import SwiftUI

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
            Text(String(format:"X %+.2f   Y %+.2f   Z %+.2f · %d samples",sensor.gyro.x,sensor.gyro.y,sensor.gyro.z,sensor.samples))
                .font(.system(.caption2,design:.monospaced)).foregroundStyle(.tertiary).monospacedDigit()
        }.accessibilityElement(children:.combine)
    }
}
