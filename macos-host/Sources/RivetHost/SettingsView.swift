import SwiftUI
import AppKit
import UniformTypeIdentifiers
import Carbon.HIToolbox
import ServiceManagement

/// One `settings-ui` row after parsing: a setting, its section and the
/// control the host should paint for it.
struct SettingsSchemaRow: Identifiable {
    let key: String
    let section: String
    let control: String
    let options: [String]
    var value: String
    var id: String { key }
}

struct SettingsSchemaSection {
    let id: String
    var rows: [SettingsSchemaRow]
}

/// One sidebar destination: a page of the schema under its group.
struct SettingsSchemaPage: Identifiable {
    let id: String
    let group: String
    var sections: [SettingsSchemaSection]
}

/// Settings window state: one string per backend key, written through the
/// settings-set RPC as the user edits. The backend owns validation (hotkey
/// grammar, numeric ranges, provider names); a refusal surfaces on its row
/// and the form reloads the backend's truth instead of keeping a value that
/// never landed. The query-syntax rows in the launcher keep working — this
/// window is the discoverable face of the same settings store.
///
/// Settings v2 (design/DESIGN.md "偏好设置 v2 宿主契约"): the sidebar, the
/// pages, their sections and rows all come from the backend's `settings-ui`
/// data; Commands and Workflows keep their native editors.
@MainActor
final class SettingsModel: ObservableObject {
    @Published var values: [String: String] = [:]
    @Published var error: String?
    @Published var notice: String?
    /// A refusal under its own setting row; survives the reload that follows.
    @Published var rowErrors: [String: String] = [:]
    @Published var pages: [SettingsSchemaPage] = []
    @Published var aiStatus = ""
    @Published var startAtLogin = SMAppService.mainApp.status == .enabled
    @Published var loginItemError: String?
    @Published var aiKeyDraft = ""
    @Published var aiKeyNotice = ""
    // Extras: the list pages. Kept in the model so revisiting a page is
    // immediate before the refresh lands.
    @Published var namedLists: [String: [[String]]] = [:]
    @Published var plugins: [[String]] = []
    @Published var snippets: [[String]] = []
    @Published var quicklinks: [[String]] = []
    // The row a search hit jumped to, highlighted for a moment.
    @Published var highlightKey: String?
    // Per-command bindings (#48): what is bindable, what is bound, and the
    // id currently recording a hotkey (drives the inline "Press keys…" UI).
    @Published var commands: [[String]] = []
    @Published var bindings: [[String]] = []
    @Published var recordingId: String?
    @Published var bindingError: String?

    private let api: FulcrumAPI
    let workflowEditor: WorkflowEditorModel
    /// Host side effects that must follow a write (language table, the
    /// snippet expansion engine); theme and hotkey arrive via State pushes.
    private let onChanged: (String, String) -> Void

    init(api: FulcrumAPI, onChanged: @escaping (String, String) -> Void) {
        self.api = api
        self.workflowEditor = WorkflowEditorModel(api: api)
        self.onChanged = onChanged
    }

    /// `settings-ui` rows: `("page" <page> <group>)` opens each page, then
    /// `("section" <page> <section>)` and `("setting" <key> <page> <section>
    /// <control> <options> <value>)` follow in display order.
    static func parseSchema(_ rows: [[String]]) -> [SettingsSchemaPage] {
        var pages: [SettingsSchemaPage] = []
        for row in rows {
            if row.first == "page", row.count >= 3 {
                pages.append(SettingsSchemaPage(id: row[1], group: row[2], sections: []))
                continue
            }
            guard !pages.isEmpty else { continue }
            if row.first == "section", row.count >= 3, row[1] == pages[pages.count - 1].id {
                pages[pages.count - 1].sections.append(SettingsSchemaSection(id: row[2], rows: []))
                continue
            }
            guard row.first == "setting", row.count >= 7, row[2] == pages[pages.count - 1].id else {
                continue
            }
            var page = pages[pages.count - 1]
            // A row whose section line never arrived still gets its section.
            if page.sections.last?.id != row[3] {
                page.sections.append(SettingsSchemaSection(id: row[3], rows: []))
            }
            let options = row[5].isEmpty ? [] : row[5].split(separator: ",").map(String.init)
            page.sections[page.sections.count - 1].rows.append(
                SettingsSchemaRow(key: row[1], section: row[3], control: row[4],
                                  options: options, value: row[6]))
            pages[pages.count - 1] = page
        }
        return pages
    }

    func load() async {
        if let rows = try? await api.settingsList() {
            var loaded: [String: String] = [:]
            for row in rows {
                // settings-list rows carry the value in the title column.
                loaded[row.id] = row.title
            }
            values = loaded
        }
        if let schema = try? await api.settingsUI(), !schema.isEmpty {
            pages = Self.parseSchema(schema)
        }
        await refreshAiStatus()
        refreshLoginItem()
    }

    /// A page change retires whatever the previous one reported, then
    /// loads what the new one shows beside its schema rows.
    func showPage(_ page: String) async {
        error = nil
        notice = nil
        if page == "commands" { await loadBindings() }
        await loadExtras(page)
    }

    /// The list a page needs beside its schema rows; no-op for pages
    /// without one.
    func loadExtras(_ page: String) async {
        switch page {
        case "apps":
            for kind in ["app-folders", "apps"] {
                if let items = try? await api.namedListItems(kind: kind) { namedLists[kind] = items }
            }
        case "extensions":
            if let items = try? await api.pluginsList() { plugins = items }
        case "snippets":
            if let items = try? await api.snippetList() { snippets = items }
        case "quicklinks":
            if let items = try? await api.quicklinkList() { quicklinks = items }
        default:
            break
        }
    }

    // Per-machine OS login item: never sent to the backend or synced.
    // macOS may require user approval in System Settings.
    func refreshLoginItem() {
        startAtLogin = SMAppService.mainApp.status == .enabled
    }

    func setStartAtLogin(_ enabled: Bool) {
        do {
            if enabled {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
            refreshLoginItem()
            loginItemError = enabled && !startAtLogin
                ? I18nService.shared.t("settings.login.approve.mac") : nil
        } catch {
            refreshLoginItem()
            loginItemError = I18nService.shared.format(
                "settings.login.failed.mac", error.localizedDescription)
        }
    }

    func refreshAiStatus() async {
        aiStatus = (try? await api.aiStatus()) ?? ""
    }

    /// The backend reports one of three states; the two refusals carry a
    /// reason, not a localized sentence.
    func aiStatusText(t: (String) -> String) -> String {
        if aiStatus == "ready" { return t("settings.ai-ready") }
        if aiStatus == "no provider configured" { return t("settings.ai-status.no-provider") }
        if aiStatus.hasPrefix("no API key for ") {
            return t("settings.ai-status.no-key") + " " + String(aiStatus.dropFirst("no API key for ".count))
        }
        return aiStatus
    }

    func set(_ key: String, _ value: String) {
        guard values[key] != value else { return }
        values[key] = value
        Task {
            do {
                let accepted = try await api.settingsSet(key: key, value: value)
                if accepted {
                    error = nil
                    rowErrors[key] = nil
                    onChanged(key, value)
                    if key == "ai-provider" {
                        await refreshAiStatus()
                    }
                } else {
                    self.rowErrors[key] = key == "hotkey"
                        ? I18nService.shared.t("settings.hotkey.unsupported")
                        : I18nService.shared.format("settings.value.unsupported", key)
                    await load()
                }
            } catch {
                self.rowErrors[key] = I18nService.shared.format("settings.write.failed", key)
                await load()
            }
        }
    }

    func saveAiKey() {
        let key = aiKeyDraft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !key.isEmpty else { return }
        Task {
            let status = (try? await api.runAction(id: "ai.key", arg: key)) ?? "error"
            aiKeyNotice = status == "ok"
                ? I18nService.shared.t("settings.ai-key.saved")
                : status
            aiKeyDraft = ""
            await refreshAiStatus()
        }
    }

    // ---- list pages ---------------------------------------------------------

    // The backend leads a refusal with a stable code word.
    private func refusal(_ raw: String, fallback: String,
                         codes: [String: String]) -> String {
        let code = raw.split(separator: ":", maxSplits: 1).first.map(String.init) ?? raw
        if let key = codes[code] { return I18nService.shared.t(key) }
        return I18nService.shared.format(fallback, raw)
    }

    private static let listCodes = [
        "duplicate-name": "settings.list.duplicate",
        "path-required": "settings.list.path-required",
        "name-required": "settings.list.name-required"
    ]

    func saveNamedList(kind: String, id: String, name: String, path: String,
                       enabled: String) async -> String? {
        do {
            _ = try await api.namedListSet(kind: kind, id: id, name: name, path: path,
                                           extra: "", enabled: enabled)
            error = nil
            await loadExtras("apps")
            return nil
        } catch {
            return refusal("\(error)", fallback: "settings.list.failed", codes: Self.listCodes)
        }
    }

    func removeNamedList(kind: String, id: String) async {
        do {
            _ = try await api.namedListRemove(kind: kind, id: id)
            error = nil
        } catch {
            self.error = refusal("\(error)", fallback: "settings.list.failed", codes: Self.listCodes)
        }
        await loadExtras("apps")
    }

    func setPlugin(id: String, enabled: Bool) async {
        do {
            _ = try await api.pluginSetEnabled(id: id, enabled: enabled ? "on" : "off")
            error = nil
        } catch {
            self.error = I18nService.shared.format("settings.col.failed", "\(error)")
        }
        await loadExtras("extensions")
    }

    func saveCollection(snippets: Bool, id: String, name: String, keyword: String,
                        body: String) async -> String? {
        do {
            if snippets {
                _ = try await api.snippetSave(id: id, name: name, keyword: keyword, text: body)
            } else {
                _ = try await api.quicklinkSave(id: id, name: name, keyword: keyword, url: body)
            }
            error = nil
            await loadExtras(snippets ? "snippets" : "quicklinks")
            return nil
        } catch {
            return I18nService.shared.format("settings.col.failed", "\(error)")
        }
    }

    func removeCollection(snippets: Bool, id: String) async {
        do {
            if snippets {
                _ = try await api.snippetDelete(id: id)
            } else {
                _ = try await api.quicklinkDelete(id: id)
            }
            error = nil
        } catch {
            self.error = I18nService.shared.format("settings.col.failed", "\(error)")
        }
        await loadExtras(snippets ? "snippets" : "quicklinks")
    }

    /// Clearing history is irreversible for the user; callers confirm first.
    func clearHistory(clipboard: Bool) async {
        do {
            if clipboard {
                let count = try await api.clipboardClear()
                error = nil
                notice = I18nService.shared.format("settings.clipboard.cleared", count)
            } else {
                _ = try await api.historyClear()
                error = nil
                notice = I18nService.shared.t("settings.privacy.cleared")
            }
        } catch {
            self.error = I18nService.shared.format("settings.col.failed", "\(error)")
        }
    }

    /// Writes the chosen categories to a file the host picked. The backend
    /// never sees a path.
    func exportConfiguration(categories: String, to url: URL) async -> Bool {
        do {
            let payload = try await api.settingsExport(categories: categories)
            let data = try Data(payload.utf8)
            try data.write(to: url, options: .atomic)
            error = nil
            notice = I18nService.shared.format("settings.sync.exported", url.path)
            return true
        } catch {
            self.error = I18nService.shared.t("settings.sync.file-failed")
            return false
        }
    }

    /// Import, step one: what the file would replace, as display lines
    /// ("<category>: <count>"). Nothing changes yet; `nil` on a refused or
    /// unreadable file.
    func importSummary(payload: String) async -> String? {
        do {
            let plan = try await api.settingsImport(payload: payload, apply: "off")
            guard plan.hasPrefix("plan:") else { return nil }
            // "plan:<category>:<count>,…" — each category has a display name.
            let lines = plan.dropFirst("plan:".count).split(separator: ",").compactMap { part -> String? in
                let fields = part.split(separator: ":", maxSplits: 1).map(String.init)
                guard fields.count == 2 else { return nil }
                return "\(I18nService.shared.t("settings.transfer.\(fields[0])")): \(fields[1])"
            }
            return lines.isEmpty ? nil : lines.joined(separator: "\n")
        } catch {
            return nil
        }
    }

    /// Import, step two: apply the file and read everything back.
    func applyImport(payload: String) async -> String? {
        do {
            _ = try await api.settingsImport(payload: payload, apply: "on")
        } catch {
            return I18nService.shared.format("settings.sync.import-failed", "\(error)")
        }
        error = nil
        notice = I18nService.shared.t("settings.sync.imported")
        // The lists and rows now hold other content: read everything back.
        namedLists = [:]
        plugins = []
        snippets = []
        quicklinks = []
        await load()
        return nil
    }

    // ---- per-command bindings (#48) ---------------------------------------

    func loadBindings() async {
        commands = (try? await api.bindingsCommands()) ?? []
        bindings = (try? await api.bindingsList()) ?? []
    }

    private func currentBinding(_ id: String) -> [String]? {
        bindings.first(where: { $0.first == id })
    }

    func saveBinding(id: String, alias: String, hotkey: String) {
        Task {
            do {
                let ok = try await api.bindingSet(id: id, alias: alias, hotkey: hotkey)
                if ok {
                    bindingError = nil
                } else {
                    // The backend refuses duplicates, the summon hotkey, and
                    // unknown commands; say so instead of blinking.
                    bindingError = I18nService.shared.t("settings.commands.rejected")
                }
                await loadBindings()
            } catch {
                bindingError = "\(id): \(error)"
            }
        }
    }

    func clearBinding(id: String) {
        Task {
            _ = try? await api.bindingClear(id: id)
            await loadBindings()
        }
    }

    /// A recorded combo lands here from HotkeyRecorder; empty spec means
    /// the user cancelled with Esc.
    func recordFinished(id: String, spec: String?) {
        recordingId = nil
        guard let spec, !spec.isEmpty else { return }
        let alias = currentBinding(id)?[1] ?? ""
        saveBinding(id: id, alias: alias, hotkey: spec)
    }

    func isOn(_ key: String) -> Bool {
        values[key] == "on"
    }

    func toggle(_ key: String) -> Binding<Bool> {
        Binding(get: { self.isOn(key) },
                set: { self.set(key, $0 ? "on" : "off") })
    }

    func choice(_ key: String, default fallback: String = "") -> Binding<String> {
        Binding(get: { self.values[key] ?? fallback },
                set: { self.set(key, $0) })
    }

    func number(_ key: String, default fallback: Int) -> Binding<Int> {
        Binding(get: { Int(self.values[key] ?? "") ?? fallback },
                set: { self.set(key, String($0)) })
    }

    /// Briefly marks a row a search hit jumped to.
    func highlight(_ key: String) {
        highlightKey = key
        Task {
            try? await Task.sleep(for: .seconds(1.8))
            if highlightKey == key { highlightKey = nil }
        }
    }
}

/// A text field that commits on ⏎ and on losing focus — never per
/// keystroke, so the backend validates whole values (a half-typed hotkey
/// would be refused on every character otherwise).
private struct SettingField: View {
    let title: String
    let key: String
    var placeholder = ""
    @ObservedObject var model: SettingsModel
    @State private var draft = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField(title, text: $draft, prompt: Text(placeholder))
            .focused($focused)
            .onAppear { draft = model.values[key] ?? "" }
            .onChange(of: model.values[key]) { _, current in
                if !focused { draft = current ?? "" }
            }
            .onSubmit { commit() }
            .onChange(of: focused) { _, now in
                if !now { commit() }
            }
    }

    private func commit() {
        let value = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        if value != (model.values[key] ?? "") {
            model.set(key, value)
        }
    }
}

/// One bindable command row: alias field, hotkey display + record button.
private struct CommandBindingRow: View {
    @ObservedObject var model: SettingsModel
    let command: [String]
    @State private var aliasDraft = ""
    @State private var recorder: HotkeyRecorder?

    var body: some View {
        let id = command.first ?? ""
        let binding = model.bindings.first(where: { $0.first == id })
        let alias = binding?[1] ?? ""
        let hotkey = binding?[2] ?? ""
        HStack(spacing: 10) {
            Text(command.count > 1 ? command[1] : id)
                .font(.system(size: 12))
                .frame(maxWidth: .infinity, alignment: .leading)
            TextField(I18nService.shared.t("settings.commands.alias"), text: $aliasDraft)
                .textFieldStyle(.roundedBorder)
                .frame(width: 90)
                .onAppear { aliasDraft = alias }
                .onChange(of: alias) { _, current in aliasDraft = current }
                .onSubmit {
                    let trimmed = aliasDraft.trimmingCharacters(in: .whitespaces)
                    if trimmed != alias { model.saveBinding(id: id, alias: trimmed, hotkey: hotkey) }
                }
            if model.recordingId == id {
                Button(I18nService.shared.t("settings.commands.recording")) {}
                    .controlSize(.small)
                    .disabled(true)
            } else {
                Button(hotkey.isEmpty ? I18nService.shared.t("settings.commands.record")
                                      : Hotkey.display(hotkey)) {
                    model.recordingId = id
                    recorder = HotkeyRecorder { spec in
                        model.recordFinished(id: id, spec: spec)
                        recorder = nil
                    }
                    recorder?.start()
                }
                .controlSize(.small)
            }
            if !hotkey.isEmpty || !alias.isEmpty {
                Button(I18nService.shared.t("settings.commands.clear")) { model.clearBinding(id: id) }
                    .controlSize(.small)
            }
        }
        .padding(.vertical, 1)
    }
}

/// Captures the next modifier+key press inside this window as a hotkey
/// spec. A local monitor is enough — the user records in the settings
/// window — so no Input Monitoring permission is involved. Esc cancels.
@MainActor
final class HotkeyRecorder {
    private let finish: (String?) -> Void
    private var monitor: Any?

    init(finish: @escaping (String?) -> Void) { self.finish = finish }

    func start() {
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .flagsChanged]) { [weak self] event in
            guard let self else { return event }
            if event.type == .flagsChanged { return nil }  // modifiers alone: keep waiting
            if event.keyCode == UInt16(kVK_Escape) {
                self.stop(); self.finish(nil); return nil
            }
            let mask = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
                .subtracting([.capsLock, .function])
            guard mask != [] else { return event }  // bare key: ignore, keep waiting
            guard let spec = Hotkey.spec(forKeyCode: UInt32(event.keyCode),
                                         modifiers: Self.carbonMask(mask)) else {
                return event  // unmappable key: keep waiting
            }
            self.stop()
            self.finish(spec)
            return nil
        }
    }

    func cancel() {
        stop()
        finish(nil)
    }

    private func stop() {
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
    }

    private static func carbonMask(_ flags: NSEvent.ModifierFlags) -> UInt32 {
        var mask: UInt32 = 0
        if flags.contains(.control) { mask |= UInt32(controlKey) }
        if flags.contains(.option) { mask |= UInt32(optionKey) }
        if flags.contains(.shift) { mask |= UInt32(shiftKey) }
        if flags.contains(.command) { mask |= UInt32(cmdKey) }
        return mask
    }
}

/// Global summon shortcut editor. Recording produces only canonical values
/// accepted by the shared backend grammar, so macOS users never need to know
/// or type the transport spelling (for example, `alt+space`).
private struct GlobalHotkeySetting: View {
    let title: String
    @ObservedObject var model: SettingsModel
    @State private var recorder: HotkeyRecorder?
    @State private var isRecording = false

    var body: some View {
        LabeledContent(title) {
            if isRecording {
                Button(I18nService.shared.t("settings.commands.recording")) {}
                    .disabled(true)
            } else {
                Button(Hotkey.display(model.values["hotkey"] ?? "alt+space")) {
                    isRecording = true
                    recorder = HotkeyRecorder { spec in
                        isRecording = false
                        if let spec, !spec.isEmpty {
                            model.set("hotkey", spec)
                        }
                        recorder = nil
                    }
                    recorder?.start()
                }
                .help(I18nService.shared.t("settings.hotkey.placeholder"))
                .accessibilityIdentifier("settings-record-hotkey")
            }
        }
        .onDisappear {
            if isRecording { recorder?.cancel() }
            recorder = nil
            isRecording = false
        }
    }
}

// MARK: - dialogs

/// An irreversible step the host asks about once; the destructive button is
/// never the default.
private struct ConfirmRequest: Identifiable {
    let id = UUID()
    let title: String
    let message: String
    let confirmLabel: String
    let action: () -> Void
}

/// Application folders / searchable applications editor.
private struct NamedListForm: Identifiable {
    let kind: String  // "app-folders" or "apps"
    let entryId: String
    let name: String
    let path: String
    let enabled: String
    var id: String { kind + "/" + entryId }
}

private struct NamedListSheet: View {
    @ObservedObject var model: SettingsModel
    let form: NamedListForm
    @Environment(\.colorScheme) private var scheme
    @Environment(\.dismiss) private var dismiss
    @State private var name: String = ""
    @State private var path: String = ""
    @State private var error: String?

    private var folders: Bool { form.kind == "app-folders" }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(I18nService.shared.t(form.entryId.isEmpty ? "settings.list.add-title"
                                                            : "settings.list.edit-title"))
                .font(.headline)
            field(I18nService.shared.t("settings.list.name"), text: $name)
            if folders {
                Text(I18nService.shared.t("settings.list.path"))
                    .font(.system(size: 12))
                HStack {
                    TextField("", text: $path)
                        .textFieldStyle(.roundedBorder)
                        .accessibilityIdentifier("settings-list-path")
                    Button(I18nService.shared.t("settings.browse")) { browse() }
                }
            }
            if let error {
                Text(error)
                    .font(.system(size: 11))
                    .foregroundStyle(FulcrumTheme.danger(scheme))
            }
            HStack {
                Spacer()
                Button(I18nService.shared.t("settings.cancel")) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button(I18nService.shared.t("settings.save")) { save() }
                    .buttonStyle(.borderedProminent)
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 440)
        .onAppear {
            name = form.name
            path = form.path
        }
    }

    private func field(_ label: String, text: Binding<String>) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label).font(.system(size: 12))
            TextField("", text: text).textFieldStyle(.roundedBorder)
        }
    }

    private func browse() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url { path = url.path }
    }

    private func save() {
        let trimmedName = name.trimmingCharacters(in: .whitespacesAndNewlines)
        let trimmedPath = path.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmedName.isEmpty {
            error = I18nService.shared.t("settings.list.name-required")
            return
        }
        if folders && trimmedPath.isEmpty {
            error = I18nService.shared.t("settings.list.path-required")
            return
        }
        // An edit keeps the entry's switch where it was.
        Task {
            let refused = await model.saveNamedList(kind: form.kind, id: form.entryId,
                                                    name: trimmedName, path: folders ? trimmedPath : "",
                                                    enabled: form.enabled.isEmpty ? "on" : form.enabled)
            if let refused { error = refused } else { dismiss() }
        }
    }
}

/// Snippet / quicklink editor. The backend has no group field for these
/// lists, so the host keeps them flat, like the prototype's collection cards.
private struct CollectionForm: Identifiable {
    let snippets: Bool
    let entryId: String
    let name: String
    let keyword: String
    let body: String
    var id: String { (snippets ? "snippets/" : "quicklinks/") + entryId }
}

/// A quicklink target: an HTTP(S) URL, a file URL, or an absolute path. The
/// backend stays the authority; this only spares the user a round trip.
private func plausibleTarget(_ text: String) -> Bool {
    for prefix in ["http://", "https://", "file://", "\\\\", "/"] where text.hasPrefix(prefix) {
        return true
    }
    return text.count >= 3 && text.first!.isLetter
        && text.dropFirst().first == ":"
        && (text.dropFirst(2).first == "\\" || text.dropFirst(2).first == "/")
}

private struct CollectionSheet: View {
    @ObservedObject var model: SettingsModel
    let form: CollectionForm
    @Environment(\.colorScheme) private var scheme
    @Environment(\.dismiss) private var dismiss
    @State private var name: String = ""
    @State private var keyword: String = ""
    @State private var body_ = ""
    @State private var error: String?
    private let i18n = I18nService.shared

    private var titleKey: String {
        switch (form.snippets, form.entryId.isEmpty) {
        case (true, true): return "settings.col.add-snippet"
        case (true, false): return "settings.col.edit-snippet"
        case (false, true): return "settings.col.add-quicklink"
        case (false, false): return "settings.col.edit-quicklink"
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(i18n.t(titleKey)).font(.headline)
            field(i18n.t("settings.col.name"), text: $name, help: "")
            field(i18n.t("settings.col.keyword"), text: $keyword, help: i18n.t("settings.col.keyword-help"))
            VStack(alignment: .leading, spacing: 2) {
                Text(i18n.t(form.snippets ? "settings.col.body" : "settings.col.url"))
                    .font(.system(size: 12))
                TextEditor(text: $body_)
                    .font(.system(size: 12))
                    .frame(height: form.snippets ? 110 : 60)
                    .overlay(RoundedRectangle(cornerRadius: 5).stroke(Color.secondary.opacity(0.25)))
                if !form.snippets {
                    Text(i18n.t("settings.col.url-help"))
                        .font(.system(size: 11)).foregroundStyle(.secondary)
                }
            }
            if let error {
                Text(error)
                    .font(.system(size: 11))
                    .foregroundStyle(FulcrumTheme.danger(scheme))
            }
            HStack {
                Spacer()
                Button(i18n.t("settings.cancel")) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button(i18n.t("settings.save")) { save() }
                    .buttonStyle(.borderedProminent)
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 480)
        .onAppear {
            name = form.name
            keyword = form.keyword
            body_ = form.body
        }
    }

    private func field(_ label: String, text: Binding<String>, help: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label).font(.system(size: 12))
            TextField("", text: text).textFieldStyle(.roundedBorder)
            if !help.isEmpty {
                Text(help).font(.system(size: 11)).foregroundStyle(.secondary)
            }
        }
    }

    private func save() {
        let values = [name, keyword, body_].map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
        if values.contains(where: \.isEmpty) {
            error = i18n.t("settings.col.required")
            return
        }
        if !form.snippets && !plausibleTarget(values[2]) {
            error = i18n.t("settings.col.invalid-url")
            return
        }
        Task {
            let refused = await model.saveCollection(snippets: form.snippets, id: form.entryId,
                                                     name: values[0], keyword: values[1], body: values[2])
            if let refused { error = refused } else { dismiss() }
        }
    }
}

/// Export: choose what to include, then where to save.
private struct ExportSheet: View {
    @ObservedObject var model: SettingsModel
    @Environment(\.dismiss) private var dismiss
    @State private var appearance = true
    @State private var features = true
    @State private var commands = true
    @State private var content = true
    private let i18n = I18nService.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(i18n.t("settings.sync.export")).font(.headline)
            Toggle(i18n.t("settings.transfer.appearance"), isOn: $appearance)
            Toggle(i18n.t("settings.transfer.features"), isOn: $features)
            Toggle(i18n.t("settings.transfer.commands"), isOn: $commands)
            Toggle(i18n.t("settings.transfer.content"), isOn: $content)
            Text(i18n.t("settings.sync.transfer-help"))
                .font(.system(size: 11)).foregroundStyle(.secondary)
            HStack {
                Spacer()
                Button(i18n.t("settings.cancel")) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button(i18n.t("settings.sync.export")) { export_() }
                    .buttonStyle(.borderedProminent)
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 380)
    }

    private func export_() {
        var categories: [String] = []
        if appearance { categories.append("appearance") }
        if features { categories.append("features") }
        if commands { categories.append("commands") }
        if content { categories.append("content") }
        guard !categories.isEmpty else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = "fulcrum-settings.json"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        Task {
            if await model.exportConfiguration(categories: categories.joined(separator: ","), to: url) {
                dismiss()
            }
        }
    }
}

// MARK: - settings window

struct SettingsView: View {
    @ObservedObject var model: SettingsModel
    @State private var selection = "general"
    @State private var query = ""
    @FocusState private var searchFocused: Bool
    @State private var namedListForm: NamedListForm?
    @State private var collectionForm: CollectionForm?
    @State private var showExport = false
    @State private var confirm: ConfirmRequest?
    // Import: the file's text and what the backend says it would replace.
    @State private var importPayload = ""
    @State private var importSummaryText = ""
    @Environment(\.colorScheme) private var scheme
    private let i18n = I18nService.shared

    private struct SidebarGroup: Identifiable {
        let id: String
        let pages: [SettingsSchemaPage]
    }

    // Translation reads the observable backend setting, so switching language
    // immediately re-renders the visible settings surface. "system" resolves
    // the OS locale the same way I18nService does.
    private func t(_ key: String) -> String {
        let lang = model.values["language"] ?? "system"
        if lang == "system" {
            let preferred = Locale.preferredLanguages.first ?? "en"
            return GeneratedI18n.tr(preferred.hasPrefix("zh") ? "zh" : "en", key)
        }
        return GeneratedI18n.tr(lang, key)
    }

    // A key with no text in the shared table paints nothing.
    private func text(_ key: String) -> String {
        GeneratedI18n.table[key] == nil ? "" : t(key)
    }

    private func format(_ key: String, _ args: Any...) -> String {
        var out = t(key)
        for (index, arg) in args.enumerated() {
            out = out.replacingOccurrences(of: "{\(index)}", with: String(describing: arg))
        }
        return out
    }

    private var page: SettingsSchemaPage? { model.pages.first { $0.id == selection } }

    /// Sidebar order: groups as first seen, their pages in schema order.
    private var groups: [SidebarGroup] {
        var order: [String] = []
        var byGroup: [String: [SettingsSchemaPage]] = [:]
        for item in model.pages {
            if byGroup[item.group] == nil { order.append(item.group) }
            byGroup[item.group, default: []].append(item)
        }
        return order.map { SidebarGroup(id: $0, pages: byGroup[$0] ?? []) }
    }

    struct Hit: Identifiable {
        let page: String
        let key: String
        let label: String
        let path: String
        var id: String { page + "/" + key }
    }

    /// Local filter over the labels and hints the window already holds —
    /// no RPC. A result reads "label · page / section".
    private var hits: [Hit] {
        let needle = query.trimmingCharacters(in: .whitespaces).lowercased()
        guard !needle.isEmpty else { return [] }
        var found: [Hit] = []
        for item in model.pages {
            for section in item.sections {
                for row in section.rows {
                    let label = t("settings.\(row.key)")
                    let hint = t("settings.hint.\(row.key)")
                    let haystack = (label + " " + hint + " " + row.key).lowercased()
                    if haystack.contains(needle) {
                        found.append(Hit(page: item.id, key: row.key, label: label,
                                         path: "\(t("settings.nav.\(item.id)")) / \(t("settings.section.\(section.id)"))"))
                    }
                }
            }
        }
        return found
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                sidebar
                    .frame(width: 208)
                Divider()
                Group {
                    if !query.isEmpty {
                        searchResults
                    } else if selection == "commands" {
                        commandsTab
                    } else if selection == "workflows" {
                        WorkflowEditorView(model: model.workflowEditor)
                    } else {
                        schemaTab
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            if let error = model.error {
                Text(error)
                    .font(.system(size: 11))
                    .foregroundStyle(FulcrumTheme.danger(scheme))
                    .lineLimit(2)
                    .textSelection(.enabled)
                    .padding(.horizontal, 16)
                    .padding(.bottom, 10)
            } else if let notice = model.notice {
                Text(notice)
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
                    .textSelection(.enabled)
                    .padding(.horizontal, 16)
                    .padding(.bottom, 10)
            }
        }
        .frame(width: 760, height: 640)
        // Cmd+F focuses the settings search from anywhere in the window.
        .background {
            Button("") { searchFocused = true }
                .keyboardShortcut("f", modifiers: .command)
                .frame(width: 0, height: 0)
                .opacity(0)
        }
        .task(id: selection) { await model.showPage(selection) }
        .sheet(item: $namedListForm) { form in NamedListSheet(model: model, form: form) }
        .sheet(item: $collectionForm) { form in CollectionSheet(model: model, form: form) }
        .sheet(isPresented: $showExport) { ExportSheet(model: model) }
        .sheet(isPresented: Binding(get: { !importSummaryText.isEmpty },
                                    set: { if !$0 { importSummaryText = "" } })) {
            importSummary
        }
        .alert(confirm?.title ?? "", isPresented: Binding(get: { confirm != nil },
                                                          set: { if !$0 { confirm = nil } }),
               presenting: confirm) { request in
            Button(request.confirmLabel, role: .destructive) { request.action() }
            Button(i18n.t("settings.cancel"), role: .cancel) {}
        } message: { request in
            Text(request.message)
        }
        .task { await model.load() }
    }

    // MARK: sidebar

    private var sidebar: some View {
        ScrollViewReader { proxy in
            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    TextField(t("settings.search.label"), text: $query)
                        .textFieldStyle(.roundedBorder)
                        .focused($searchFocused)
                        .accessibilityIdentifier("settings-search")
                        .onExitCommand { if !query.isEmpty { query = "" } }
                        .padding(.bottom, 10)
                    ForEach(groups) { group in
                        Text(t("settings.group.\(group.id)"))
                            .font(.system(size: 11, weight: .semibold))
                            .foregroundStyle(.secondary)
                            .padding(.top, 10)
                            .padding(.bottom, 3)
                            .padding(.leading, 6)
                        ForEach(group.pages) { item in
                            Button {
                                selection = item.id
                            } label: {
                                Text(t("settings.nav.\(item.id)"))
                                    .frame(maxWidth: .infinity, alignment: .leading)
                                    .padding(.vertical, 5)
                                    .padding(.horizontal, 6)
                                    .background(
                                        selection == item.id
                                            ? FulcrumTheme.accent.opacity(0.14)
                                            : Color.clear)
                                    .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                            .id(item.id)
                            .accessibilityIdentifier("settings-sidebar-\(item.id)")
                        }
                    }
                }
                .padding(12)
            }
            .onChange(of: selection) { _, page in
                withAnimation { proxy.scrollTo(page) }
            }
        }
    }

    // MARK: search results

    private var searchResults: some View {
        let found = hits
        return ScrollView {
            VStack(alignment: .leading, spacing: 6) {
                Text(format("settings.search.results", found.count))
                    .font(.headline)
                    .padding(.bottom, 4)
                if found.isEmpty {
                    Text(t("settings.search.empty")).font(.system(size: 12, weight: .semibold))
                    Text(t("settings.search.empty-help"))
                        .font(.system(size: 11)).foregroundStyle(.secondary)
                }
                ForEach(found) { hit in
                    Button {
                        jump(to: hit)
                    } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(hit.label)
                            Text(hit.path)
                                .font(.system(size: 11)).foregroundStyle(.secondary)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(8)
                        .background(Color.primary.opacity(0.05), in: RoundedRectangle(cornerRadius: 6))
                    }
                    .buttonStyle(.plain)
                    .accessibilityIdentifier("settings-search-result-\(hit.page)-\(hit.key)")
                }
            }
            .padding(20)
        }
    }

    /// Selecting a hit leaves search, shows its page and highlights the row.
    private func jump(to hit: Hit) {
        query = ""
        selection = hit.page
        model.highlight(hit.key)
    }

    // MARK: schema pages

    private var schemaTab: some View {
        Form {
            if let page {
                if !text("settings.page.\(page.id)").isEmpty {
                    Section {
                        Text(t("settings.page.\(page.id)"))
                            .font(.system(size: 12)).foregroundStyle(.secondary)
                    }
                }
                ForEach(page.sections, id: \.id) { section in
                    Section(t("settings.section.\(section.id)")) {
                        ForEach(section.rows) { row in
                            // Start at login is device-local and never in the
                            // schema; it sits before the hotkey row.
                            if row.key == "hotkey" { startupRows }
                            schemaRow(row)
                        }
                    }
                }
                extras(page.id)
            }
        }
        .formStyle(.grouped)
    }

    private var startupRows: some View {
        Group {
            Toggle(t("settings.start-at-login"), isOn: Binding(
                get: { model.startAtLogin },
                set: { model.setStartAtLogin($0) }))
                .accessibilityIdentifier("settings-start-at-login")
            Text(t("settings.start-at-login-local"))
                .font(.system(size: 11)).foregroundStyle(.secondary)
            if let error = model.loginItemError {
                Text(error)
                    .font(.system(size: 11))
                    .foregroundStyle(FulcrumTheme.danger(scheme))
                    .textSelection(.enabled)
                Button(i18n.t("settings.login.open.mac")) {
                    SMAppService.openSystemSettingsLoginItems()
                }
            }
        }
    }

    @ViewBuilder
    private func schemaRow(_ row: SettingsSchemaRow) -> some View {
        let title = t("settings.\(row.key)")
        Group {
            switch row.control {
            case "toggle":
                Toggle(isOn: model.toggle(row.key)) { Text(title) }
            case "select":
                Picker(title, selection: choiceBinding(row)) {
                    // The empty option is the "Off" provider; its key
                    // spelling is `off`.
                    ForEach(row.options, id: \.self) { option in
                        Text(t("settings.opt.\(row.key).\(option.isEmpty ? "off" : option))")
                            .tag(option)
                    }
                }
            case "number":
                numberRow(row, title: title)
            case "hotkey":
                GlobalHotkeySetting(title: title, model: model)
            default:
                SettingField(title: title, key: row.key,
                             placeholder: text("settings.\(row.key).placeholder"), model: model)
            }
            if !text("settings.hint.\(row.key)").isEmpty {
                Text(t("settings.hint.\(row.key)"))
                    .font(.system(size: 11)).foregroundStyle(.secondary)
            }
            if let error = model.rowErrors[row.key] {
                Text(error)
                    .font(.system(size: 11))
                    .foregroundStyle(FulcrumTheme.danger(scheme))
                    .textSelection(.enabled)
            }
        }
        .listRowBackground(model.highlightKey == row.key
                           ? FulcrumTheme.accent.opacity(0.14) : nil)
    }

    /// The backend's value, or the first option while a value is missing.
    private func choiceBinding(_ row: SettingsSchemaRow) -> Binding<String> {
        let fallback = row.options.first ?? ""
        return Binding(get: {
            let current = self.model.values[row.key] ?? row.value
            return row.options.contains(current) ? current : fallback
        }, set: { self.model.set(row.key, $0) })
    }

    private func numberRow(_ row: SettingsSchemaRow, title: String) -> some View {
        // options = the validator's inclusive (min max).
        let low = row.options.count >= 2 ? Int(row.options[0]) ?? 0 : 0
        let high = row.options.count >= 2 ? Int(row.options[1]) ?? 1_000_000 : 1_000_000
        let binding = model.number(row.key, default: Int(row.value) ?? low)
        return Stepper(value: binding, in: low...max(low, high)) {
            HStack {
                Text(title)
                Spacer()
                TextField("", value: binding)
                    .multilineTextAlignment(.trailing)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 100)
            }
        }
    }

    // MARK: page extras

    @ViewBuilder
    private func extras(_ page: String) -> some View {
        switch page {
        case "apps":
            namedListSection(kind: "app-folders")
            namedListSection(kind: "apps")
        case "extensions":
            pluginsSection
        case "snippets":
            collectionSection(snippets: true)
        case "quicklinks":
            collectionSection(snippets: false)
        case "ai":
            aiKeySection
        case "clipboard":
            Section {
                Button(i18n.t("settings.clipboard.clear")) {
                    confirm = ConfirmRequest(
                        title: i18n.t("settings.clipboard.clear"),
                        message: i18n.t("settings.clipboard.clear-confirm"),
                        confirmLabel: i18n.t("settings.clipboard.clear")) {
                        Task { await model.clearHistory(clipboard: true) }
                    }
                }
                .accessibilityIdentifier("settings-clipboard-clear")
            }
        case "privacy":
            Section {
                Text(t("settings.privacy.stays"))
                    .font(.system(size: 11)).foregroundStyle(.secondary)
                Button(i18n.t("settings.privacy.clear")) {
                    confirm = ConfirmRequest(
                        title: i18n.t("settings.privacy.clear"),
                        message: i18n.t("settings.privacy.clear-confirm"),
                        confirmLabel: i18n.t("settings.privacy.clear")) {
                        Task { await model.clearHistory(clipboard: false) }
                    }
                }
                .accessibilityIdentifier("settings-privacy-clear")
            }
        case "sync":
            transferSection
            Section(t("settings.updates")) {
                Button(t("settings.check-updates-now")) {
                    UpdateController.shared?.checkForUpdates()
                    AppDelegate.shared?.showUpdatePanel()
                }
                .accessibilityIdentifier("settings-update-check")
                Text(t("settings.updates-note.mac"))
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }
        default:
            EmptyView()
        }
    }

    private func namedListSection(kind: String) -> some View {
        let folders = kind == "app-folders"
        let items = model.namedLists[kind] ?? []
        return Section {
            Text(t(folders ? "settings.list.app-folders-help" : "settings.list.apps-help"))
                .font(.system(size: 11)).foregroundStyle(.secondary)
            if items.isEmpty {
                Text(t("settings.list.empty")).font(.system(size: 11)).foregroundStyle(.secondary)
            }
            ForEach(items, id: \.first) { item in
                namedListRow(kind: kind, item: item)
            }
            if folders {
                Button(i18n.t("settings.list.add")) {
                    namedListForm = NamedListForm(kind: kind, entryId: "", name: "", path: "", enabled: "")
                }
                .accessibilityIdentifier("settings-list-add")
            }
        } header: {
            Text(t(folders ? "settings.list.app-folders" : "settings.list.apps"))
        }
    }

    @ViewBuilder
    private func namedListRow(kind: String, item: [String]) -> some View {
        if item.count >= 5 {
            let id = item[0], name = item[1], path = item[2], enabled = item[4]
            if kind == "apps" {
                Toggle(isOn: Binding(
                    get: { enabled == "on" },
                    set: { on in
                        Task {
                            _ = await model.saveNamedList(kind: kind, id: id, name: name,
                                                          path: path, enabled: on ? "on" : "off")
                        }
                    })) { Text(name) }
            } else {
                HStack(spacing: 8) {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(name)
                        Text(path).font(.system(size: 11)).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button(i18n.t("settings.list.edit")) {
                        namedListForm = NamedListForm(kind: kind, entryId: id, name: name,
                                                      path: path, enabled: enabled)
                    }
                    Button(i18n.t("settings.list.remove")) {
                        confirm = ConfirmRequest(
                            title: i18n.t("settings.list.remove"),
                            message: i18n.format("settings.list.remove-confirm", name),
                            confirmLabel: i18n.t("settings.list.remove")) {
                            Task { await model.removeNamedList(kind: kind, id: id) }
                        }
                    }
                }
            }
        }
    }

    private var pluginsSection: some View {
        Section {
            Text(t("settings.ext.help"))
                .font(.system(size: 11)).foregroundStyle(.secondary)
            if model.plugins.isEmpty {
                Text(t("settings.ext.empty")).font(.system(size: 11)).foregroundStyle(.secondary)
            }
            ForEach(model.plugins, id: \.first) { item in
                // Rows: (id name version commands id "plugin" "" ""); a load
                // error has an empty id and its message in the name column.
                if item.count >= 4 {
                    if item[0].isEmpty {
                        Text(item[1])
                            .font(.system(size: 11))
                            .foregroundStyle(FulcrumTheme.danger(scheme))
                    } else {
                        let disabled = (model.values["plugins-disabled"] ?? "")
                            .split(separator: ",").map(String.init)
                        Toggle(isOn: Binding(
                            get: { !disabled.contains(item[0]) },
                            set: { on in Task { await model.setPlugin(id: item[0], enabled: on) } })
                        ) {
                            VStack(alignment: .leading, spacing: 1) {
                                Text(item[1])
                                Text(format("settings.ext.commands", item[3])
                                     + (item[2].isEmpty ? "" : " · \(item[2])"))
                                    .font(.system(size: 11)).foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            }
        }
    }

    private func collectionSection(snippets: Bool) -> some View {
        let items = snippets ? model.snippets : model.quicklinks
        return Section {
            if items.isEmpty {
                Text(t(snippets ? "settings.col.empty-snippets" : "settings.col.empty-quicklinks"))
                    .font(.system(size: 11)).foregroundStyle(.secondary)
            }
            ForEach(items, id: \.first) { item in
                if item.count >= 4 {
                    let id = item[0], name = item[1]
                    HStack(spacing: 8) {
                        VStack(alignment: .leading, spacing: 1) {
                            Text(name)
                            if !item[3].isEmpty {
                                Text(item[2] + (item[2].isEmpty ? "" : " · ") + item[3])
                                    .font(.system(size: 11)).foregroundStyle(.secondary)
                                    .lineLimit(1)
                            }
                        }
                        Spacer()
                        Button(i18n.t("settings.col.edit")) {
                            collectionForm = CollectionForm(snippets: snippets, entryId: id,
                                                             name: item[1], keyword: item[2], body: item[3])
                        }
                        Button(i18n.t("settings.col.delete")) {
                            confirm = ConfirmRequest(
                                title: i18n.t("settings.col.delete"),
                                message: i18n.format("settings.col.delete-confirm", name),
                                confirmLabel: i18n.t("settings.col.delete")) {
                                Task { await model.removeCollection(snippets: snippets, id: id) }
                            }
                        }
                    }
                }
            }
            Button(i18n.t(snippets ? "settings.col.add-snippet" : "settings.col.add-quicklink")) {
                collectionForm = CollectionForm(snippets: snippets, entryId: "", name: "",
                                                keyword: "", body: "")
            }
            .accessibilityIdentifier(snippets ? "settings-list-add" : "settings-quicklink-add")
        } header: {
            Text(t(snippets ? "settings.nav.snippets" : "settings.nav.quicklinks"))
        }
    }

    private var aiKeySection: some View {
        Section(t("settings.ai-api-key")) {
            HStack {
                SecureField(t("settings.ai-api-key"), text: $model.aiKeyDraft,
                            prompt: Text(t("settings.ai-key.placeholder")))
                    .onSubmit { model.saveAiKey() }
                    .accessibilityIdentifier("settings-ai-key")
                Button(t("settings.save")) { model.saveAiKey() }
                    .disabled(model.aiKeyDraft.trimmingCharacters(in: .whitespaces).isEmpty)
                    .accessibilityIdentifier("settings-ai-key-save")
            }
            HStack(spacing: 6) {
                Image(systemName: model.aiStatus == "ready"
                      ? "checkmark.circle.fill" : "exclamationmark.circle")
                    .foregroundStyle(model.aiStatus == "ready" ? .green : .secondary)
                Text(model.aiStatusText(t: t))
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
            }
            if !model.aiKeyNotice.isEmpty {
                Text(model.aiKeyNotice)
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }
            Text(t("settings.hint.ai-keys"))
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
        }
    }

    private var transferSection: some View {
        Section {
            Text(t("settings.sync.transfer-help"))
                .font(.system(size: 11)).foregroundStyle(.secondary)
            HStack {
                Button(t("settings.sync.export")) { showExport = true }
                    .accessibilityIdentifier("settings-sync-export")
                Button(t("settings.sync.import")) { import_() }
                    .accessibilityIdentifier("settings-sync-import")
            }
        } header: {
            Text(t("settings.page.sync"))
        }
    }

    /// Import, step one: pick a file and ask the backend what it replaces.
    private func import_() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        guard let data = try? Data(contentsOf: url), let payload = String(data: data, encoding: .utf8) else {
            model.error = i18n.t("settings.sync.file-failed")
            return
        }
        Task {
            if let summary = await model.importSummary(payload: payload) {
                importPayload = payload
                importSummaryText = summary
            } else {
                model.error = i18n.format("settings.sync.import-failed", url.lastPathComponent)
            }
        }
    }

    /// Import, step two: the summary, then one deliberate apply.
    private var importSummary: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(t("settings.sync.import-title")).font(.headline)
            Text(importSummaryText)
                .font(.system(size: 12))
                .textSelection(.enabled)
            HStack {
                Spacer()
                Button(i18n.t("settings.cancel")) { importSummaryText = "" }
                    .keyboardShortcut(.cancelAction)
                Button(t("settings.sync.import-apply")) {
                    let payload = importPayload
                    importSummaryText = ""
                    Task { _ = await model.applyImport(payload: payload) }
                }
            }
        }
        .padding(20)
        .frame(width: 420)
    }

    // MARK: commands

    private var commandsTab: some View {
        Form {
            Section {
                Text(t("settings.hint.commands"))
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                if let bindingError = model.bindingError {
                    Text(bindingError)
                        .font(.system(size: 11))
                        .foregroundStyle(FulcrumTheme.danger(scheme))
                        .textSelection(.enabled)
                }
            }
            Section(t("settings.nav.commands")) {
                ForEach(model.commands, id: \.self) { command in
                    CommandBindingRow(model: model, command: command)
                }
            }
        }
        .formStyle(.grouped)
    }
}
