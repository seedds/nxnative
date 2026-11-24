// The in-game Cheats screen: the in-game menu's Cheats row (GameMenu.swift) opens it over the game; one checkbox row per cheat of hle_cheats.c's loaded file (Documents/cheats).
import UIKit
import UniformTypeIdentifiers

// Engine text as Swift: UTF-8, else Windows-1252 (cheat-site files), as the Mac panel reads it
private func engineString(_ p: UnsafePointer<CChar>?) -> String {
    guard let p else { return "" }
    if let s = String(validatingCString: p) { return s }
    return String(data: Data(bytes: p, count: strlen(p)), encoding: .windowsCP1252) ?? String(cString: p)
}

// The on half of an on / off pair loses its " - on": the pair is one switch, its off half has no row
private func cheatDisplayName(_ i: Int32) -> String {
    let n = engineString(dm_cheat_name_at(i))
    guard dm_cheat_flags_at(i) & Int32(DM_CHEAT_PAIR_ON) != 0 else { return n }
    let base = n.replacingOccurrences(of: "(\\s*[-:_]\\s*|\\s+|\\s*[(\\[]\\s*)on\\s*[)\\]]?\\s*$", with: "", options: [.regularExpression, .caseInsensitive])
    return base.isEmpty ? n : base
}

private func hasFlag(_ i: Int32, _ f: Int32) -> Bool { dm_cheat_flags_at(i) & f != 0 }

// The "Cheats" row of the in-game menu: its subtitle counts the cheats that are on
func cheatsMenuItem() -> UIMenuElement {
    let n = dm_cheats_ncheats()
    let on = (0..<n).filter { dm_cheat_kind_at($0) == 0 && dm_cheat_on_at($0) != 0 && !hasFlag($0, Int32(DM_CHEAT_PAIR_OFF)) }.count
    let a = UIAction(title: "Cheats", image: UIImage(systemName: "wand.and.stars")) { _ in presentCheats() }
    a.subtitle = dm_cheats_found() == 0 ? "No cheats for this version" : dm_cheats_master() == 0 ? "Off" : "\(on) on"
    return a
}

private func presentCheats() {
    let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
    guard var vc = scenes.flatMap({ $0.windows }).first(where: { $0.isKeyWindow })?.rootViewController else { return }
    while let p = vc.presentedViewController { vc = p }
    let nav = UINavigationController(rootViewController: CheatsViewController(style: .insetGrouped))
    nav.modalPresentationStyle = .formSheet
    vc.present(nav, animated: true)
}

private final class CheatsViewController: UITableViewController, UIDocumentPickerDelegate {
    private enum Row { case master, reload, importFile, allOff, cheat(Int32), label(String), info(String) }
    private var sections: [(title: String?, footer: String?, rows: [Row])] = []
    private var stamp = ""
    private var timer: Timer?

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "Cheats"
        navigationItem.rightBarButtonItem = UIBarButtonItem(systemItem: .close, primaryAction: UIAction { [weak self] _ in self?.dismiss(animated: true) })
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "c")
        rebuild()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.refreshIfChanged() }   // a cheat killed at run time shows its reason
    }

    override func viewWillDisappear(_ animated: Bool) { super.viewWillDisappear(animated); timer?.invalidate(); timer = nil }

    private func engineStamp() -> String {
        let n = dm_cheats_ncheats()
        return "\(dm_cheats_found()) \(dm_cheats_master()) \(n) " + (0..<n).map { "\(dm_cheat_kind_at($0))\(dm_cheat_on_at($0))" }.joined()
    }

    private func refreshIfChanged() { if engineStamp() != stamp { rebuild() } }

    private func rebuild() {
        stamp = engineStamp()
        sections = []
        let build = engineString(dm_cheats_build()), version = engineString(dm_cheats_version())
        let label = version.isEmpty ? "build \(build)" : "\(version) (build \(build.prefix(8))…)"
        let n = dm_cheats_ncheats()
        var top: [Row] = []
        if dm_cheats_found() != 0 {
            let plain = (0..<n).filter { dm_cheat_kind_at($0) == 0 && !hasFlag($0, Int32(DM_CHEAT_PAIR_OFF)) }
            let on = plain.filter { dm_cheat_on_at($0) != 0 }.count
            top = [.master, .allOff]
            sections.append((label, "\(plain.count) cheats, \(on) on. Only cheats that change game data run on iOS; code patches show why they are off.", top))
        } else {
            var rows: [Row] = [.info("No cheats for \(label). Put \(build).txt (Atmosphère format) in this app's cheats folder in the Files app, or import it here.")]
            for i in 0..<dm_cheats_nothers() {
                let v = engineString(dm_cheats_other_version(i))
                rows.append(.info("\(engineString(dm_cheats_other(i))).txt is for another version of the game\(v.isEmpty ? "" : " (\(v))")."))
            }
            sections.append((nil, nil, rows))
        }
        var current: String? = nil, rows: [Row] = [], started = false
        for i in 0..<n where !hasFlag(i, Int32(DM_CHEAT_PAIR_OFF | DM_CHEAT_SECTION_START | DM_CHEAT_SECTION_END)) {
            let s = engineString(dm_cheat_section_at(i))
            if !started || s != (current ?? "") {
                if started && !rows.isEmpty { sections.append((current, nil, rows)) }
                current = s.isEmpty ? nil : s; rows = []; started = true
            }
            rows.append(dm_cheat_kind_at(i) == 3 ? .label(cheatDisplayName(i)) : .cheat(i))
        }
        if !rows.isEmpty { sections.append((current, nil, rows)) }
        sections.append((nil, "Folder: Files › On My iPad › \(Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String ?? "this app") › cheats", [.reload, .importFile]))
        tableView.reloadData()
    }

    override func numberOfSections(in tableView: UITableView) -> Int { sections.count }
    override func tableView(_ tableView: UITableView, numberOfRowsInSection s: Int) -> Int { sections[s].rows.count }
    override func tableView(_ tableView: UITableView, titleForHeaderInSection s: Int) -> String? { sections[s].title }
    override func tableView(_ tableView: UITableView, titleForFooterInSection s: Int) -> String? { sections[s].footer }

    override func tableView(_ tableView: UITableView, cellForRowAt ip: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "c", for: ip)
        var c = UIListContentConfiguration.subtitleCell()
        cell.accessoryType = .none; cell.accessoryView = nil; cell.selectionStyle = .default
        let masterOn = dm_cheats_master() != 0
        switch sections[ip.section].rows[ip.row] {
        case .master:
            c.text = "Cheats On"
            let sw = UISwitch(); sw.isOn = masterOn
            sw.addAction(UIAction { [weak self] a in dm_cheats_master_set((a.sender as? UISwitch)?.isOn == true ? 1 : 0); self?.rebuild() }, for: .valueChanged)
            cell.accessoryView = sw; cell.selectionStyle = .none
        case .allOff:
            c.text = "Turn All Off"; c.textProperties.color = .systemRed
        case .reload:
            c.text = "Reload Cheats"; c.secondaryText = "Reads the file again after it changed"; c.image = UIImage(systemName: "arrow.clockwise")
        case .importFile:
            c.text = "Import Cheat File…"; c.secondaryText = "A .txt in Atmosphère format"; c.image = UIImage(systemName: "square.and.arrow.down")
        case .label(let t):
            c.text = t; c.textProperties.color = .secondaryLabel; c.textProperties.font = .preferredFont(forTextStyle: .footnote); cell.selectionStyle = .none
        case .info(let t):
            c.text = t; c.textProperties.color = .secondaryLabel; cell.selectionStyle = .none
        case .cheat(let i):
            let kind = dm_cheat_kind_at(i), on = kind == 1 || (kind == 0 && dm_cheat_on_at(i) != 0)
            let why = engineString(dm_cheat_why_at(i)).trimmingCharacters(in: .whitespaces)
            c.text = cheatDisplayName(i)
            var sub: [String] = []
            if kind == 1 { sub.append("Master code (always on)") }
            if !why.isEmpty { sub.append(why.trimmingCharacters(in: CharacterSet(charactersIn: "()"))) }
            if hasFlag(i, Int32(DM_CHEAT_ONCE)) { sub.append("Applies once") }
            if hasFlag(i, Int32(DM_CHEAT_ASK)), let v = cheatValue(i) { sub.append("Value \(v)") }
            c.secondaryText = sub.isEmpty ? nil : sub.joined(separator: " · ")
            c.secondaryTextProperties.color = kind == 2 ? .systemOrange : .secondaryLabel
            c.image = UIImage(systemName: on ? "checkmark.square.fill" : "square")
            c.imageProperties.tintColor = kind == 0 && masterOn ? .tintColor : .tertiaryLabel
            c.textProperties.color = kind == 0 && masterOn ? .label : .secondaryLabel
            if kind != 0 { cell.selectionStyle = .none }
            if kind != 3 { cell.accessoryType = .detailButton }
        }
        cell.contentConfiguration = c
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt ip: IndexPath) {
        tableView.deselectRow(at: ip, animated: true)
        switch sections[ip.section].rows[ip.row] {
        case .cheat(let i) where dm_cheat_kind_at(i) == 0:
            if hasFlag(i, Int32(DM_CHEAT_ASK)) && dm_cheat_on_at(i) == 0 { askValue(i, thenToggle: true); return }
            dm_cheat_toggle(i); rebuild()
        case .allOff:
            for i in 0..<dm_cheats_ncheats() where dm_cheat_kind_at(i) == 0 && dm_cheat_on_at(i) != 0 && !hasFlag(i, Int32(DM_CHEAT_PAIR_OFF)) { dm_cheat_toggle(i) }
            rebuild()
        case .reload: dm_cheats_reload(); rebuild()
        case .importFile:
            let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.plainText, UTType(filenameExtension: "txt") ?? .plainText], asCopy: true)
            picker.delegate = self
            present(picker, animated: true)
        default: break
        }
    }

    // The ⓘ button: the note, what the cheat writes right now (a dry run), and the value field of an "ask" cheat
    override func tableView(_ tableView: UITableView, accessoryButtonTappedForRowWith ip: IndexPath) {
        guard case .cheat(let i) = sections[ip.section].rows[ip.row] else { return }
        if hasFlag(i, Int32(DM_CHEAT_ASK)) { askValue(i, thenToggle: false); return }
        var buf = [CChar](repeating: 0, count: 4096)
        _ = dm_cheat_preview(i, &buf, buf.count)
        var text = engineString(dm_cheat_note_at(i))
        let preview = buf.withUnsafeBufferPointer { engineString($0.baseAddress) }
        if !preview.isEmpty { text += (text.isEmpty ? "" : "\n\n") + "Right now:\n" + preview }
        let a = UIAlertController(title: cheatDisplayName(i), message: text.isEmpty ? nil : text, preferredStyle: .alert)
        a.addAction(UIAlertAction(title: "OK", style: .default))
        present(a, animated: true)
    }

    private func cheatValue(_ i: Int32) -> String? {
        var buf = [CChar](repeating: 0, count: 64)
        return dm_cheat_value_get(i, &buf, buf.count) != 0 ? buf.withUnsafeBufferPointer { engineString($0.baseAddress) } : nil
    }

    private func askValue(_ i: Int32, thenToggle: Bool) {
        let bounds = engineString(dm_cheat_value_bounds(i))
        let a = UIAlertController(title: cheatDisplayName(i), message: bounds.isEmpty ? nil : "Range \(bounds)", preferredStyle: .alert)
        a.addTextField { f in f.text = self.cheatValue(i); f.keyboardType = .numbersAndPunctuation }
        a.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        a.addAction(UIAlertAction(title: thenToggle ? "Turn On" : "Set", style: .default) { [weak self] _ in
            let t = a.textFields?.first?.text ?? ""
            if dm_cheat_value_set(i, t) != 0 { self?.alert("Not a value this cheat takes: \(t)"); return }
            if thenToggle { dm_cheat_toggle(i) }
            self?.rebuild()
        })
        present(a, animated: true)
    }

    private func alert(_ msg: String) {
        let a = UIAlertController(title: nil, message: msg, preferredStyle: .alert)
        a.addAction(UIAlertAction(title: "OK", style: .default))
        present(a, animated: true)
    }

    // An imported file: a build-ID name keeps its name, any other .txt becomes this build's; a file already there is kept as .bak
    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        guard let src = urls.first else { return }
        let dir = engineString(dm_cheats_dir_path()), build = engineString(dm_cheats_build())
        guard !dir.isEmpty, !build.isEmpty else { alert("This app has no cheats folder."); return }
        let stem = src.deletingPathExtension().lastPathComponent
        let isBuildID = stem.count == 16 && stem.allSatisfy(\.isHexDigit)
        let name = (isBuildID ? stem.uppercased() : build) + ".txt"
        let fm = FileManager.default, dst = URL(fileURLWithPath: dir).appendingPathComponent(name)
        do {
            try fm.createDirectory(atPath: dir, withIntermediateDirectories: true)
            if fm.fileExists(atPath: dst.path) {
                let bak = dst.appendingPathExtension("bak")
                try? fm.removeItem(at: bak)
                try fm.moveItem(at: dst, to: bak)
            }
            try fm.copyItem(at: src, to: dst)
        } catch { alert("Could not import \(src.lastPathComponent): \(error.localizedDescription)"); return }
        dm_cheats_reload(); rebuild()
        if isBuildID && stem.uppercased() != build.uppercased() { alert("\(name) is for another version of the game (this one is \(build)).") }
    }
}
