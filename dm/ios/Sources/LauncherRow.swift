// One launcher setting as a row (a menu, a slider with its percentage or its unit, a toggle), shared by the iOS launcher (Launcher.swift) and the Mac app's settings window (dm/mac/Launcher.swift).
// A row whose setting is fixed by an env file or by the launch environment is shown but disabled.
import SwiftUI

extension LauncherModel {
    func binding(_ s: Setting) -> Binding<String> { Binding(get: { self.values[s.env] ?? s.def }, set: { self.values[s.env] = $0 }) }
}

struct SettingRow: View {
    let s: Setting
    @ObservedObject var m: LauncherModel

    private var locked: Bool { m.fixed.contains(s.env) }
    private var note: String {
        guard locked else { return s.note }
        return (s.note.isEmpty ? "" : s.note + " · ") + (m.fromLaunch.contains(s.env) ? "Set at launch" : "Set by an env file")
    }
    private var rangeValue: Binding<Double> {
        Binding(get: { Double(m.values[s.env] ?? s.def) ?? s.max ?? 1 },
                set: { m.values[s.env] = String(format: "%.2f", $0) })
    }
    private var percentage: String { m.rangeLabel(s, rangeValue.wrappedValue) }

    private var settingLabel: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(s.label)
            if !note.isEmpty { Text(note).font(.footnote).foregroundStyle(.secondary) }
        }
    }

    var body: some View {
        Group {
            switch s.kind {
            case "choice" where s.optionNeeds != nil:
                // Options with a rule (option_needs, Output resolution's sizes): every option is listed, and one whose rule does not hold is greyed out with what it needs
                // ("4K — needs 8 GB of Game memory"). A menu of buttons, as a menu picker cannot disable one entry
                HStack {
                    settingLabel
                    Spacer()
                    Menu {
                        ForEach(s.options ?? [], id: \.[0]) { o in
                            Button { m.values[s.env] = o[0] } label: {
                                let text = m.needLabel(s, o[0]).map { "\(o[1]) — \($0)" } ?? o[1]
                                if (m.values[s.env] ?? s.def) == o[0] { Label(text, systemImage: "checkmark") } else { Text(text) }
                            }
                            .disabled(!m.offers(s, o[0]))
                        }
                    } label: {
                        Text((s.options ?? []).first { $0[0] == (m.values[s.env] ?? s.def) }?[1] ?? (m.values[s.env] ?? s.def))
                    }
                    .fixedSize()
                    .accessibilityLabel(s.label)
                }
            case "choice":
                Picker(selection: m.binding(s)) {
                    ForEach(m.options(s), id: \.[0]) { Text($0[1]).tag($0[0]) }
                } label: {
                    settingLabel
                }
                .pickerStyle(.menu)
            case "range":
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        settingLabel
                        Spacer()
                        Text(percentage).monospacedDigit().foregroundStyle(.secondary)
                    }
                    Slider(value: rangeValue, in: (s.min ?? 0)...(s.max ?? 1), step: s.step ?? 0.05)
                        .accessibilityLabel(s.label)
                        .accessibilityValue(percentage)
                }
            default:
                Toggle(isOn: Binding(get: { (m.values[s.env] ?? s.def) == s.onValue }, set: { m.values[s.env] = $0 ? s.onValue : s.offValue })) {
                    settingLabel
                }
            }
        }
        .disabled(locked)
    }
}
