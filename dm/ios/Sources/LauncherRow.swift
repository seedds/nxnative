// One launcher setting as a row (a menu, a slider with its percentage, a toggle), shared by the iOS launcher (Launcher.swift) and the Mac app's settings window (dm/mac/Launcher.swift).
// A row whose setting is fixed by an env file or by the launch environment is shown but disabled.
import SwiftUI

extension LauncherModel {
    func binding(_ s: Setting) -> Binding<String> { Binding(get: { self.values[s.env] ?? s.def }, set: { self.values[s.env] = $0 }) }
}

struct PlayerSection: View {
    @ObservedObject var m: LauncherModel
    @State private var editing = false
    @State private var renameID: String?
    @State private var nickname = ""
    @State private var editError: String?

    var body: some View {
        if m.users.count > 1 || m.canEditUsers {
            Section {
                if m.users.count > 1 {
                    Picker("Who's playing?", selection: $m.selectedUser) {
                        ForEach(m.users) { user in Text(verbatim: user.label).tag(user.index) }
                    }
                    .pickerStyle(.menu)
                    .disabled(m.userLocked)
                } else if let user = m.pickedUser {
                    LabeledContent("Who's playing?", value: user.nickname)
                }
                if m.canEditUsers {
                    HStack(spacing: 16) {
                        Button("Create User…") { beginEdit(nil) }.disabled(!m.canCreateUser)
                        if let user = m.pickedUser { Button("Rename User…") { beginEdit(user) } }
                    }
                    .buttonStyle(.borderless)
                    .sheet(isPresented: $editing) { editor }
                }
            }
        }
    }

    private func beginEdit(_ user: LauncherUser?) {
        renameID = user?.id
        nickname = user?.nickname ?? ""
        editError = nil
        editing = true
    }

    private func saveEdit() {
        guard editing else { return }
        editError = m.editUser(renameID: renameID, nickname: nickname)
        if editError == nil { editing = false }
    }

    private var editor: some View {
        NavigationStack {
            Form {
                TextField("Nickname", text: $nickname)
                    .onSubmit(saveEdit)
                    .onChange(of: nickname) { _, _ in editError = nil }
                if let error = editError ?? (nickname.isEmpty ? nil : LauncherModel.nicknameProblem(nickname)) {
                    Text(verbatim: error).foregroundStyle(.red)
                }
            }
            .formStyle(.grouped)
            .navigationTitle(renameID == nil ? "Create User" : "Rename User")
#if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
#endif
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { editing = false } }
                ToolbarItem(placement: .confirmationAction) {
                    Button(renameID == nil ? "Create" : "Save", action: saveEdit)
                        .disabled(LauncherModel.nicknameProblem(nickname) != nil)
                }
            }
        }
#if os(macOS)
        .frame(minWidth: 380, minHeight: 200)
#endif
    }
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
            case "choice":
                Picker(selection: m.binding(s)) {
                    ForEach(s.options ?? [], id: \.[0]) { Text($0[1]).tag($0[0]) }
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
