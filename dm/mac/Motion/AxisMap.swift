import Foundation
import simd

struct AxisMap: Codable, Equatable {
    var x = 1
    var y = 2
    var z = 3
    func apply(_ v: SIMD3<Double>) -> SIMD3<Double> {
        func component(_ a: Int) -> Double { guard (1...3).contains(abs(a)) else { return 0 }; return v[abs(a)-1] * (a < 0 ? -1 : 1) }
        return SIMD3(component(x),component(y),component(z))
    }
    var valid: Bool { Set([abs(x),abs(y),abs(z)]) == Set([1,2,3]) }
}
