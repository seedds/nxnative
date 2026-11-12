import Foundation
import simd

enum DSU {
    static func crc(_ bytes: [UInt8]) -> UInt32 {
        var c: UInt32 = 0xffffffff
        for b in bytes { c ^= UInt32(b); for _ in 0..<8 { c = (c >> 1) ^ (c & 1 == 1 ? 0xedb88320 : 0) } }
        return ~c
    }
    static func number(_ b: [UInt8], _ o: Int, _ n: Int) -> UInt64 {
        (0..<n).reduce(0) { $0 | UInt64(b[o+$1]) << (8*$1) }
    }
    static func packet(type: UInt32, payload: [UInt8], id: UInt32) -> Data {
        var b: [UInt8] = Array("DSUC".utf8)
        func add(_ v: UInt64, _ n: Int) { for i in 0..<n { b.append(UInt8(truncatingIfNeeded: v >> (8*i))) } }
        add(1001,2); add(UInt64(payload.count+4),2); add(0,4); add(UInt64(id),4); add(UInt64(type),4); b += payload
        let c = crc(b); for i in 0..<4 { b[8+i] = UInt8(truncatingIfNeeded: c >> (8*i)) }
        return Data(b)
    }
    static func connectedSlot(_ data: Data) -> UInt8? {
        var b = [UInt8](data)
        guard b.count >= 32,Array(b.prefix(4)) == Array("DSUS".utf8),number(b,4,2)==1001 else { return nil }
        let size=Int(number(b,6,2))+16
        guard size>=32,size<=b.count else { return nil };b=Array(b.prefix(size))
        let expected=UInt32(number(b,8,4));for i in 8..<12 { b[i]=0 }
        let type=number(b,16,4)
        guard crc(b)==expected,(type==0x100001 || type==0x100002),b[20]<4,b[21]==2 else { return nil }
        return b[20]
    }
    static func describe(_ data: Data, selectedSlot: Int) -> String {
        var b = [UInt8](data)
        let hex = b.prefix(24).map { String(format:"%02x",$0) }.joined(separator:" ")
        guard b.count >= 20 else { return "Short packet: \(b.count) bytes · " + hex }
        let magic = String(bytes:b.prefix(4),encoding:.ascii) ?? "?"
        let type = number(b,16,4), size = Int(number(b,6,2))+16
        var detail = String(format:"%d bytes · %@ · type 0x%06llx",b.count,magic,type)
        guard magic == "DSUS", size >= 20, size <= b.count else { return detail + " · rejected header/length · " + hex }
        b = Array(b.prefix(size)); let expected = UInt32(number(b,8,4)); for i in 8..<12 { b[i] = 0 }
        guard crc(b) == expected else { return detail + " · rejected CRC" }
        detail += " · CRC OK"
        guard number(b,4,2) == 1001 else { return detail + " · unsupported protocol" }
        if type == 0x100000, b.count >= 22 { return detail + " · server version \(number(b,20,2))" }
        if (type == 0x100001 || type == 0x100002), b.count >= 32 {
            detail += " · slot \(b[20]) · state \(b[21]) · model \(b[22])"
            if selectedSlot >= 0 && Int(b[20]) != selectedSlot { detail += " · OTHER SLOT (selected \(selectedSlot))" }
        }
        if type == 0x100002, b.count >= 100 {
            detail += String(format:" · buttons %02x %02x · home %d · touch %d",b[36],b[37],b[38],b[39])
            let values = stride(from:76,to:100,by:4).map { Double(Float(bitPattern:UInt32(number(b,$0,4)))) }
            detail += " · acc " + values.prefix(3).map { String(format:"%.3f",$0) }.joined(separator:", ")
            detail += " · gyro " + values.suffix(3).map { String(format:"%.3f",$0) }.joined(separator:", ")
            if decode(data) == nil { detail += " · motion rejected (inactive/nonfinite)" }
        }
        return detail
    }
    struct Sample {
        var slot: UInt8; var server: UInt32; var sequence: UInt32; var timestamp: UInt64
        var acceleration: SIMD3<Double>; var gyro: SIMD3<Double>
    }
    static func decode(_ data: Data) -> Sample? {
        var b = [UInt8](data)
        guard b.count >= 100, Array(b[0..<4]) == Array("DSUS".utf8), number(b,4,2) == 1001 else { return nil }
        let size = Int(number(b,6,2))+16
        guard size >= 100, size <= b.count else { return nil }; b = Array(b.prefix(size))
        let expected = UInt32(number(b,8,4)); for i in 8..<12 { b[i] = 0 }
        guard crc(b) == expected, number(b,16,4) == 0x100002, b[21] == 2, b[31] == 1 else { return nil }
        func f(_ o: Int) -> Double { Double(Float(bitPattern: UInt32(number(b,o,4)))) }
        let a = SIMD3(f(76),f(80),f(84)), g = SIMD3(f(88),f(92),f(96))
        guard [a.x,a.y,a.z,g.x,g.y,g.z].allSatisfy({ $0.isFinite }) else { return nil }
        return Sample(slot:b[20],server:UInt32(number(b,12,4)),sequence:UInt32(number(b,32,4)),timestamp:number(b,68,8),acceleration:a,gyro:g)
    }
}

struct Orientation {
    var attitude = simd_quatd(angle: 0, axis: SIMD3(0,1,0))
    var reference = simd_quatd(angle: 0, axis: SIMD3(0,1,0))
    var last: UInt64?
    mutating func update(_ s: DSU.Sample) {
        defer { last = s.timestamp }
        guard let t = last, s.timestamp > t else { return }
        let dt = Double(s.timestamp-t)/1_000_000
        guard dt < 0.5 else { return }
        // DSU pitch/yaw/roll mapped to display X/Y/Z. Raw values remain visible.
        let w = s.gyro * (.pi/180), speed = simd_length(w)
        if speed > 0.00001 { attitude = simd_normalize(attitude * simd_quatd(angle:speed*dt,axis:w/speed)) }
    }
    mutating func tare() { reference = attitude }
    var displayed: simd_quatd { reference.inverse * attitude }
    mutating func resetClock() { last = nil }
}
