import SwiftUI
import simd

struct MotionCapture {
    var integral = SIMD3<Double>.zero
    var travel = SIMD3<Double>.zero
    var last: UInt64?
    var samples = 0
    mutating func add(_ s: DSU.Sample) {
        defer { last = s.timestamp }
        guard let last, s.timestamp > last else { return }
        let dt = Double(s.timestamp-last)/1_000_000
        guard dt > 0, dt < 0.2 else { return }
        integral += s.gyro*dt
        travel += SIMD3(abs(s.gyro.x),abs(s.gyro.y),abs(s.gyro.z))*dt
        samples += 1
    }
    var axis: Int? {
        let ranked = (0..<3).sorted { travel[$0] > travel[$1] }
        let a = ranked[0], b = ranked[1]
        guard samples >= 8,travel[a] >= 20,travel[a] > travel[b]*1.8,abs(integral[a]) > 15,abs(integral[a]) > travel[a]*0.55 else { return nil }
        return (a+1) * (integral[a] >= 0 ? 1 : -1)
    }
}

struct AutoCalibration {
    var step = 0
    var pickedUp = false
    var flatReady = false
    var flatMeasured = false
    mutating func confirmFlat() {
        guard flatMeasured else { return }
        flatReady=true;stillSeconds=0;sum = .zero;count=0;progress=0;message="Pick up your phone."
    }
    var flatGravity = SIMD3<Double>.zero
    var preview = Orientation()
    var previewReady = false
    var phase: Int {
        if step == 0 { return !flatReady ? -1 : (pickedUp ? 1 : 0) }
        if step == 7 { return 14 }
        return settling ? step*2 : step*2+1
    }
    var progress = 0.0
    var bias = SIMD3<Double>.zero
    var capture = MotionCapture()
    var captured: [Int] = []
    var stillSeconds = 0.0
    var sum = SIMD3<Double>.zero
    var count = 0
    var last: UInt64?
    var settling = true
    var firstSign: Int?
    var forwardAxis: Int?
    var message = "Hold still in the steering-wheel grip"
    var result: AxisMap? { captured.count == 3 ? AxisMap(x:captured[1],y:captured[0],z:captured[2]) : nil }
    mutating func add(_ sample: DSU.Sample) {
        if step == 7 {
            guard let map=result else { return }
            if !previewReady {
                defer { last=sample.timestamp }
                guard let last,sample.timestamp>last else { return }
                let dt=Double(sample.timestamp-last)/1_000_000
                if dt<0.2 && simd_length(sample.gyro-bias)<4 { stillSeconds += dt } else { stillSeconds=0 }
                if stillSeconds>0.6 { preview=Orientation();previewReady=true;message="Move freely to test. Press OK to save, or Start over." }
                return
            }
            var s=sample;s.gyro=map.apply(sample.gyro-bias);preview.update(s);return
        }
        defer { last = sample.timestamp }
        guard let last, sample.timestamp > last else { return }
        let dt = Double(sample.timestamp-last)/1_000_000
        guard dt < 0.2 else { stillSeconds=0;capture=MotionCapture();return }
        if step == 0 {
            if !flatReady {
                // Stillness and approximately one g establish a baseline, not proof
                // of a tabletop or compass heading. User follows the flat pose.
                if flatMeasured {
                    if simd_length(sample.gyro)>3 { flatMeasured=false;stillSeconds=0;sum = .zero;flatGravity = .zero;count=0;progress=0 }
                    return
                }
                let g=simd_length(sample.acceleration)
                if simd_length(sample.gyro)<3 && g>0.8 && g<1.2 {
                    stillSeconds += dt;sum += sample.gyro;flatGravity += sample.acceleration;count += 1;progress=min(1,stillSeconds/1.2)
                    if stillSeconds>=1.2 {
                        bias=sum/Double(count);flatGravity /= Double(count);flatMeasured=true
                        stillSeconds=0;sum = .zero;count=0;progress=1;message="Phone is still. Confirm it is flat on the table, then Continue."
                    }
                } else { stillSeconds=0;sum = .zero;flatGravity = .zero;count=0;progress=0 }
                return
            }
            if !pickedUp {
                if simd_length(sample.gyro)>8 { pickedUp=true;message="Now HOLD STILL in your normal steering-wheel grip." }
                return
            }
            if simd_length(sample.gyro) < 3 {
                stillSeconds += dt;sum += sample.gyro;count += 1;progress=min(1,stillSeconds/1.0)
                if stillSeconds >= 1 { step=1;settling=false;progress=0;stillSeconds=0;message="Turn clockwise first, then back, like the animation" }
            } else { stillSeconds=0;sum = .zero;count=0;progress=0 }
            return
        }
        var s=sample;s.gyro -= bias
        if settling {
            if simd_length(s.gyro)<4 { stillSeconds += dt } else { stillSeconds=0 }
            progress=min(1,stillSeconds/0.55)
            if stillSeconds>0.55 { settling=false;stillSeconds=0;capture=MotionCapture();firstSign=nil;message="Follow the first direction shown, then sweep back" }
            return
        }
        capture.add(s)
        let ranked=(0..<3).sorted { capture.travel[$0]>capture.travel[$1] }
        let a=ranked[0]
        progress=min(1,capture.travel[a]/50)
        guard capture.travel[a]>=50,let sign=capture.axis else { return }
        if step % 2 == 1 {
            guard !captured.contains(where:{abs($0)==abs(sign)}) else { capture=MotionCapture();progress=0;message="Wrong axis. Follow this demonstration.";return }
            forwardAxis=sign
        } else {
            guard let forwardAxis,sign == -forwardAxis else { capture=MotionCapture();progress=0;message="Direction not confirmed. Move only in the direction shown.";return }
            captured.append(forwardAxis)
        }
        step += 1;progress=0;settling=true;stillSeconds=0;capture=MotionCapture()
        message=step==7 ? "Return to your normal grip and HOLD STILL to center the live test." : "Recognized. Return to center, then HOLD STILL."
    }
}

@available(macOS 14.0, *)
struct CalibrationView: View {
    @ObservedObject var sensor: Sensor
    @Environment(\.dismiss) private var dismiss
    @State private var started = Date()
    let titles=["Hold still", "Steer RIGHT / clockwise", "Steer LEFT / counterclockwise", "Tilt TOP away", "Tilt TOP toward you", "RIGHT hand away", "LEFT hand away", "Test live"]
    let instructions=["Hold still", "From center, steer RIGHT only, as seen by you. Stop after a broad stroke. Do not reverse until asked.", "From center, steer LEFT only, as seen by you. Stop after a broad stroke.", "From center, tilt the TOP edge AWAY only. Stop there.", "From center, tilt the TOP edge TOWARD you only. Stop there.", "From center, push your RIGHT hand AWAY only. Stop there.", "From center, push your LEFT hand AWAY only. Stop there.", "Move freely and check all directions before OK."]
    var body: some View {
        let step=sensor.autoCalibration.step
        let phase=sensor.autoCalibration.phase
        let paused = step==0 || sensor.autoCalibration.settling
        let heading = step==0 ? (!sensor.autoCalibration.flatReady ? "Place Flat and Click Continue" : (sensor.autoCalibration.pickedUp ? "Hold still" : "Hold like a wheel")) : (step==7 ? "You Got It! Move Freely To Test" : (paused ? "Hold still" : (step<=2 ? "Steer" : (step<=4 ? "Tilt" : "Rotate"))))
        VStack(alignment:.leading,spacing:16) {
            LocalizedText("Calibrate " + sensor.name).font(.title.bold())
            HStack { ForEach(Array((-1..<15).filter { $0 != 2 }), id: \.self) { i in
                let small = i == -1 || i==1 || (i>1 && i%2==0 && i<14)
                LocalizedText(small ? "Ⅱ" : String(i==0 ? 0:(i==14 ? 7:(i-1)/2))).font(.headline).frame(width:small ? 23:38,height:small ? 23:38).background(i<phase ? Color.green : (i==phase ? Color.orange:Color.gray.opacity(0.3)),in:Circle())
                if i<14 { Rectangle().fill(i<phase ? Color.green:Color.gray.opacity(0.3)).frame(height:3) }
            } }


            TimelineView(.animation(minimumInterval:1.0/30)) { context in
                let t=context.date.timeIntervalSince(started)
                let direction = step%2==1 ? 1.0 : -1.0
                let a = direction * min(t.truncatingRemainder(dividingBy:2.5)/1.5,1.0)*1.1
                let base=simd_quatd(angle:Double.pi/2,axis:SIMD3(1,0,0))
                let axis = step<=2 ? SIMD3<Double>(0,1,0) : (step<=4 ? SIMD3<Double>(1,0,0):SIMD3<Double>(0,0,1))
                let q = step==0 && !sensor.autoCalibration.flatReady ? simd_quatd(angle:0,axis:SIMD3(1,0,0)) : (step==7 ? base * sensor.autoCalibration.preview.displayed : (paused ? base : base * simd_quatd(angle:a,axis:axis)))
                MotionBox(rotation:q, arrowAxis: paused || step==7 ? nil : axis, arrowDirection:direction, obliqueView:step==3 || step==4).frame(height:320)
                    .overlay(alignment:.top) { LocalizedText(sensor.confirmed ? heading : "Waiting").font(.title2.bold()).foregroundStyle(.white).padding(16) }
                    .overlay {
                        if step==0 && !sensor.autoCalibration.flatReady && sensor.autoCalibration.flatMeasured && sensor.confirmed && t >= 2 {
                            VStack(spacing:10) {
                                Image(systemName:"checkmark.circle.fill").font(.system(size:38))
                                Button(L("Continue")) { sensor.autoCalibration.confirmFlat() }
                                    .font(.title.bold()).buttonStyle(.borderedProminent).controlSize(.extraLarge)
                                    .disabled(!sensor.confirmed || !sensor.autoCalibration.flatMeasured)
                            }.padding(24).background(.ultraThinMaterial,in:RoundedRectangle(cornerRadius:20)).offset(y:35)
                        }
                        if step==0 && sensor.autoCalibration.flatReady && !sensor.autoCalibration.pickedUp {
                            let approach=min(t.truncatingRemainder(dividingBy:3)/1.5,1.0)
                            HStack {
                                Image(systemName:"hand.point.up.left.fill").scaleEffect(x:-1,y:1)
                                Spacer().frame(width:CGFloat(220-approach*95))
                                Image(systemName:"hand.point.up.left.fill")
                            }.font(.system(size:46)).foregroundStyle(.white.opacity(0.85)).offset(y:CGFloat(65-approach*22)).allowsHitTesting(false)
                        }
                    }
                    .clipShape(RoundedRectangle(cornerRadius:12))
            }
            LocalizedText(!sensor.confirmed ? "Keep the phone app open and wait for motion data." : (step==0 ? (!sensor.autoCalibration.flatReady ? "Place the phone screen-up on a flat, stable table. When the bar fills, click Continue to confirm its position." : (sensor.autoCalibration.pickedUp ? "Hold still until the bar fills." : "Pick up your phone and hold it like the controller shown.")) : (step==7 ? sensor.autoCalibration.message : (paused ? "Return to your starting grip and hold still. Wait for the next arrow." : "Make one broad movement following the arrow, then stop.")))).foregroundStyle(.secondary).fixedSize(horizontal:false,vertical:true)
            ProgressView(value:step==7 ? 1:sensor.autoCalibration.progress)

            HStack {
                Button(L("Start over")) { sensor.autoCalibration=AutoCalibration();started=Date() }
                Spacer();Button(L("Cancel")) { dismiss() }

                if step==7 { Button(L("OK")) { if let map=sensor.autoCalibration.result { sensor.applyCalibration(map:map.fromDisplayToGame,bias:sensor.autoCalibration.bias,gravity:sensor.autoCalibration.flatGravity,preview:sensor.autoCalibration.preview) };dismiss() }.keyboardShortcut(.defaultAction).disabled(!sensor.autoCalibration.previewReady) }
            }
        }.padding(24).frame(width:780)
        .onChange(of:sensor.autoCalibration.phase) { _,_ in started=Date() }
        .onAppear { sensor.autoCalibration=AutoCalibration();sensor.calibrating=true }
        .onDisappear { sensor.calibrating=false }
    }
}
