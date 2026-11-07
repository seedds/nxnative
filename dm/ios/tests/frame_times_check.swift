// Runs LauncherFrameTimes.swift's parsing, numbers and compaction on a synthetic recording in the host's format (test_frame_times.py)
import Foundation

@main struct FrameTimesCheck {
    static var failed = 0
    static func check(_ ok: Bool, _ what: String) { if !ok { failed += 1; print("FAIL: \(what)") } }

    static func main() throws {
        let dir = URL(fileURLWithPath: CommandLine.arguments[1])
        // 600 frames at 60 fps, frame 300 a 100 ms hitch, frame 400 never shown; each frame with the timing rows the host writes, kept or not
        var text = "# kind: nvn-per-frame  value = call count when name is an NVN function name; names starting with @ are timing probes (ms / count)\nframe\tname\tvalue\n"
        var t = 0.0
        for n in 1...600 {
            let ms = n == 300 ? 100.0 : 16.667
            t += ms / 1000
            text += "\(n)\t@late_ms\t0.000\n\(n)\t@frame_ms\t\(String(format: "%.3f", ms))\n\(n)\t@draws\t677.000\n\(n)\t@t_s\t\(String(format: "%.3f", t))\n"
            text += "\(n)\t@gpu_ms\t12.000\n\(n)\t@presented_ms\t\(n == 400 ? "-1.000" : String(format: "%.3f", ms))\n\(n)\t@link_gap_ms\t16.700\n"
        }
        let file = dir.appendingPathComponent("rec.tsv")
        try text.write(to: file, atomically: true, encoding: .utf8)

        guard let before = FrameTimes.Summary(FrameTimes.parse(text)) else { print("FAIL: no summary"); exit(1) }
        check(before.frames == 600, "600 frames, got \(before.frames)")
        check(before.hitches == 1, "1 hitch, got \(before.hitches)")
        check(before.dropped == 1, "1 frame never shown, got \(before.dropped)")
        check(abs(before.p50 - 16.667) < 0.01, "median 16.667 ms, got \(before.p50)")
        check(before.worst == 100, "worst 100 ms, got \(before.worst)")
        check(FrameTimes.percentile([1, 2, 3, 4], 50) == 2 && FrameTimes.percentile([1, 2, 3, 4], 99) == 4, "nearest-rank percentiles as bench.py")

        let size = FrameTimes.size(file)
        check(FrameTimes.compact(file), "a full recording is compacted")
        let kept = try String(contentsOf: file, encoding: .utf8)
        check(kept.hasPrefix(FrameTimes.compactedMark), "the compacted file is marked")
        check(!kept.contains("@draws") && !kept.contains("@late_ms") && kept.contains("@gpu_ms"), "only the kept rows remain")
        check(FrameTimes.size(file) < size * 7 / 10, "compacted: 4 of 7 rows a frame kept (\(FrameTimes.size(file)) of \(size) bytes; a real recording has about 45)")
        guard let after = FrameTimes.Summary(FrameTimes.parse(kept)) else { print("FAIL: no summary after"); exit(1) }
        check(after.frames == before.frames && after.hitches == before.hitches && after.dropped == before.dropped && after.p50 == before.p50 && after.worst == before.worst && after.seconds == before.seconds,
              "the numbers are the same after compaction")
        check(!FrameTimes.compact(file), "a compacted recording is left alone")
        check(FrameTimes.Summary(FrameTimes.parse("frame\tname\tvalue\n")) == nil, "an empty recording has no numbers")
        print("frame times: \(failed) failed")
        exit(failed == 0 ? 0 : 1)
    }
}
