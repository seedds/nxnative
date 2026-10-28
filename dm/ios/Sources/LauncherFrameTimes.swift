// The launcher's Frame times section (Launcher.swift): a switch that records the next play's frame times, and the recordings, each summed up and
// charted. The host writes a timing row per frame when DM_NVN_TSV names a file (dm/gfx/nvn/hle_nvn.c, the rows scripts/bench.py reads; it opens the
// file at the first present, after the launcher is gone); Play passes it a new file in Documents/frame-times/ with DM_NVN_TSV_CALLS=0 (timing rows
// only). The numbers are counted as bench.py counts them (nearest-rank percentiles).
import Charts
import SwiftUI

enum FrameTimes {
    /// One frame's timing rows: @t_s (seconds from the start), @frame_ms (start to start), @presented_ms (-1: never shown)
    struct Frame { var number: Int; var time: Double; var frameMs: Double; var presentedMs: Double? }

    static func parse(_ text: String) -> [Frame] {
        var rows: [Int: [String: Double]] = [:]
        for line in text.split(whereSeparator: \.isNewline) {
            let c = line.split(separator: "\t", omittingEmptySubsequences: false)
            guard c.count == 3, c[1].hasPrefix("@"), let n = Int(c[0]), let v = Double(c[2]) else { continue }
            rows[n, default: [:]][String(c[1])] = v
        }
        return rows.keys.sorted().compactMap { n in
            let r = rows[n]!
            guard let t = r["@t_s"], let f = r["@frame_ms"] else { return nil }
            return Frame(number: n, time: t, frameMs: f, presentedMs: r["@presented_ms"])
        }
    }

    static func percentile(_ sorted: [Double], _ p: Double) -> Double {
        sorted[min(sorted.count - 1, max(0, Int((p / 100 * Double(sorted.count)).rounded(.up)) - 1))]
    }

    struct Summary {
        static let hitchMs = 50.0   // a frame this long is felt as a hitch
        var frames, dropped, hitches: Int
        var seconds, p50, p99, worst: Double
        var fps: Double { seconds > 0 ? Double(frames) / seconds : 0 }
        init?(_ frames: [Frame]) {
            let ft = frames.map(\.frameMs).filter { $0 > 0 }.sorted()
            guard let first = frames.first, let last = frames.last, !ft.isEmpty else { return nil }
            self.frames = frames.count
            dropped = frames.filter { $0.presentedMs == -1 }.count
            hitches = ft.filter { $0 > Self.hitchMs }.count
            seconds = last.time - first.time
            p50 = FrameTimes.percentile(ft, 50); p99 = FrameTimes.percentile(ft, 99); worst = ft.last!
        }
    }

    static let recordKey = "frameTimes.record"
    static var folder: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("frame-times") }

    static func recordings() -> [URL] {
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.pathExtension == "tsv" }.sorted { $0.lastPathComponent > $1.lastPathComponent }
    }

    static func size(_ file: URL) -> Int64 { ((try? FileManager.default.attributesOfItem(atPath: file.path))?[.size] as? NSNumber)?.int64Value ?? 0 }

    /// The rows a recording keeps once it is done: the host writes about 45 timing rows a frame (a 2-minute play is about 9 MB), the curve and its
    /// numbers read these (and the GPU time, for a later look); the format stays the host's, so scripts/bench.py still reads a kept file
    static let kept: Set<Substring> = ["@t_s", "@frame_ms", "@presented_ms", "@gpu_ms"]
    static let compactedMark = "# compacted by the launcher"

    /// A recording made smaller after its play: only the kept rows. Every recording is done when the launcher shows (the host writes one only
    /// while the game runs, after the launcher). False when it was already compact or cannot be read
    @discardableResult static func compact(_ file: URL) -> Bool {
        guard let text = try? String(contentsOf: file, encoding: .utf8), !text.hasPrefix(compactedMark) else { return false }
        var out = compactedMark + ": only " + kept.sorted().joined(separator: " ") + " are kept\nframe\tname\tvalue\n"
        for line in text.split(whereSeparator: \.isNewline) {
            let c = line.split(separator: "\t", maxSplits: 2, omittingEmptySubsequences: false)
            if c.count == 3, kept.contains(c[1]) { out += line; out += "\n" }
        }
        do { try out.write(to: file, atomically: true, encoding: .utf8); return true } catch { return false }
    }

    /// At Play: when the switch is on, a new file named by the start time, given to the host
    static func startRecording() {
        guard UserDefaults.standard.bool(forKey: recordKey) else { return }
        let f = DateFormatter(); f.locale = Locale(identifier: "en_US_POSIX"); f.dateFormat = "yyyy-MM-dd HH-mm-ss"
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            let file = folder.appendingPathComponent(f.string(from: Date())).appendingPathExtension("tsv")
            setenv("DM_NVN_TSV", file.path, 1); setenv("DM_NVN_TSV_CALLS", "0", 1)
            fputs("launcher: recording frame times to \(file.lastPathComponent)\n", stderr)
        } catch { fputs("[RED] launcher: no frame-times folder: \(error.localizedDescription)\n", stderr) }
    }
}

struct FrameTimesSection: View {
    @AppStorage(FrameTimes.recordKey) private var record = false
    @State private var files = FrameTimes.recordings()
    @State private var sizes: [URL: Int64] = [:]
    @State private var confirmDeleteAll = false

    private func reload() {
        files = FrameTimes.recordings()
        sizes = Dictionary(uniqueKeysWithValues: files.map { ($0, FrameTimes.size($0)) })
    }
    private var total: Int64 { sizes.values.reduce(0, +) }
    private static func bytes(_ n: Int64) -> String { ByteCountFormatter.string(fromByteCount: n, countStyle: .file) }

    var body: some View {
        Section {
            Toggle("Record frame times", isOn: $record)
            ForEach(files, id: \.self) { f in FrameTimesRow(file: f, size: sizes[f].map(Self.bytes)) }
                .onDelete { idx in
                    for i in idx { try? FileManager.default.removeItem(at: files[i]) }
                    reload()
                }
            if !files.isEmpty {
                Button("Delete all recordings", role: .destructive) { confirmDeleteAll = true }
                    .confirmationDialog("Delete all \(files.count) recordings (\(Self.bytes(total)))?", isPresented: $confirmDeleteAll, titleVisibility: .visible) {
                        Button("Delete all", role: .destructive) {
                            for f in files { try? FileManager.default.removeItem(at: f) }
                            reload()
                        }
                    }
            }
        } header: {
            Text("Frame times")
        } footer: {
            VStack(alignment: .leading, spacing: 4) {
                Text(record ? "The next play is recorded; its curve shows here the next time the app opens."
                            : "Records each frame's time while you play, to see where the game slows down or stutters.")
                if !files.isEmpty {
                    Text("\(files.count == 1 ? "1 recording takes" : "\(files.count) recordings take") \(Self.bytes(total)). A recording is about 4 MB a minute while the game runs and is cut to about a twelfth when the app opens again; swipe one to delete it, or delete the ones you no longer need.")
                }
            }
        }
        .task {
            reload()
            let list = files
            let changed = await Task.detached { list.filter { FrameTimes.compact($0) }.count }.value
            if changed > 0 { fputs("launcher: compacted \(changed) frame-time recording(s)\n", stderr); reload() }
        }
    }
}

private struct FrameTimesRow: View {
    let file: URL
    let size: String?
    @State private var frames: [FrameTimes.Frame]? = nil
    @State private var open = false
    @AppStorage("frameTimes.fpsView") private var fpsView = true

    var body: some View {
        DisclosureGroup(isExpanded: $open) {
            if let frames, let s = FrameTimes.Summary(frames), let first = frames.first, let last = frames.last {
                Picker("", selection: $fpsView) {
                    Text("FPS").tag(true)
                    Text("Frame time").tag(false)
                }
                .pickerStyle(.segmented).labelsHidden().frame(maxWidth: 260)
                Group {
                    if fpsView {
                        let pts = fpsPoints(frames)
                        Chart {
                            ForEach(pts, id: \.number) { p in
                                LineMark(x: .value("s", p.time), y: .value("fps", p.fps)).interpolationMethod(.linear)
                            }
                            ForEach(frames.filter { $0.frameMs > FrameTimes.Summary.hitchMs }, id: \.number) { f in
                                PointMark(x: .value("s", f.time), y: .value("fps", 1000 / f.frameMs)).foregroundStyle(.red).symbolSize(14)
                            }
                            RuleMark(y: .value("fps", s.fps)).foregroundStyle(.secondary.opacity(0.6)).lineStyle(StrokeStyle(lineWidth: 1, dash: [3, 3]))
                        }
                        .chartYScale(domain: 0...max(65, (pts.map(\.fps).max() ?? 60) + 5))
                        .chartYAxisLabel("fps")
                    } else {
                        Chart {
                            ForEach(thinned(frames), id: \.number) { f in
                                LineMark(x: .value("s", f.time), y: .value("ms", f.frameMs)).interpolationMethod(.linear)
                            }
                            ForEach(frames.filter { $0.frameMs > FrameTimes.Summary.hitchMs }, id: \.number) { f in
                                PointMark(x: .value("s", f.time), y: .value("ms", f.frameMs)).foregroundStyle(.red).symbolSize(14)
                            }
                            RuleMark(y: .value("ms", s.p50)).foregroundStyle(.secondary.opacity(0.6)).lineStyle(StrokeStyle(lineWidth: 1, dash: [3, 3]))
                        }
                        .chartYAxisLabel("ms")
                    }
                }
                .chartXScale(domain: first.time...max(last.time, first.time + 1))
                .chartXAxisLabel("s")
                .frame(height: 160)
                Text(fpsView ? "Each frame's rate (1000 ÷ its time in ms); the dashed line is the average, red dots are hitches over 50 ms. A dip is a slow frame."
                             : "How long each frame took (16.7 ms is 60 fps); the dashed line is the median, red dots are hitches over 50 ms.")
                    .font(.caption).foregroundStyle(.secondary)
            } else if frames != nil {
                Text("No frames in this recording").foregroundStyle(.secondary)
            } else {
                ProgressView()
            }
        } label: {
            VStack(alignment: .leading, spacing: 2) {
                HStack {
                    Text(file.deletingPathExtension().lastPathComponent)
                    if let size { Text(size).font(.caption).monospacedDigit().foregroundStyle(.secondary) }
                }
                if let frames, let s = FrameTimes.Summary(frames) {
                    Text(String(format: "%d frames · %.1f fps · median %.1f ms · 1%% low %.1f ms · worst %.0f ms · %d hitches",
                                s.frames, s.fps, s.p50, s.p99, s.worst, s.hitches))
                        .font(.caption).monospacedDigit().foregroundStyle(.secondary)
                }
            }
        }
        .task(id: file) {
            let f = file
            frames = await Task.detached { FrameTimes.parse((try? String(contentsOf: f, encoding: .utf8)) ?? "") }.value
        }
    }

    /// Each frame's rate (1000 / its frame time), at most about 2,000 points: every n-th frame, each the slowest of its stretch, so a dip is never
    /// thinned away (the frame-time line keeps the longest frame the same way)
    private func fpsPoints(_ frames: [FrameTimes.Frame]) -> [(number: Int, time: Double, fps: Double)] {
        thinned(frames.filter { $0.frameMs > 0 }).map { ($0.number, $0.time, 1000 / $0.frameMs) }
    }

    /// At most about 2,000 points on the line: every n-th frame, each the slowest of its stretch, so a hitch is never thinned away
    private func thinned(_ frames: [FrameTimes.Frame]) -> [FrameTimes.Frame] {
        let step = max(1, frames.count / 2000)
        guard step > 1 else { return frames }
        return stride(from: 0, to: frames.count, by: step).map { i in frames[i..<min(frames.count, i + step)].max { $0.frameMs < $1.frameMs }! }
    }
}
