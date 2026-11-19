import SwiftUI

/// Native workflow editor backed by the existing, validated Racket actions.
/// The macOS host never interprets or persists a workflow step itself.
/// Preview is read-only and privacy-preserving; no input/output enters traces.
@MainActor
final class WorkflowEditorModel: ObservableObject {
    @Published var names: [String] = []
    @Published var selected: String?
    @Published var steps: [ResultRow] = []
    @Published var trace: [ResultRow] = []
    @Published var nameDraft = ""
    @Published var stepDraft = ""
    @Published var inputDraft = ""
    @Published var notice = I18nService.shared.t("settings.wf.notice.init")
    @Published var busy = false
    @Published var page = 1
    @Published var stepPage = 1
    @Published var tracePage = 1
    @Published var confirmDelete = false

    private let api: FulcrumAPI
    private let separator = "\u{001F}"

    init(api: FulcrumAPI) { self.api = api }

    private func savedRows(page: Int) async throws -> [ResultRow] {
        try await api.search("wf list \(page)")
    }

    // A workflow can be on any of ten list pages; querying only page one
    // falsely reported a successfully created name as rejected.
    private func pageContaining(_ target: String) async -> Int? {
        for candidate in 1...10 {
            guard let rows = try? await savedRows(page: candidate) else { return nil }
            let saved = rows.filter { $0.kind == "Workflow" && $0.badge == "Saved" }
            if saved.contains(where: { $0.title == "Workflow · \(target)" }) {
                return candidate
            }
            if saved.count < 5 { return nil }
        }
        return nil
    }

    private func totalSteps(for name: String) async -> Int? {
        var total = 0
        for candidate in 1...4 {
            guard let rows = try? await api.search("wf show \(name) \(candidate)")
            else { return nil }
            let count = rows.filter { $0.kind == "Workflow" && $0.badge == "Step" }.count
            total += count
            if count < 5 { break }
        }
        return total
    }

    func refresh() async {
        let currentPage = page
        do {
            let found = try await savedRows(page: currentPage)
            guard page == currentPage else { return }
            names = found.filter { $0.kind == "Workflow" && $0.badge == "Saved" }
                .compactMap { row -> String? in
                    let prefix = "Workflow · "
                    return row.title.hasPrefix(prefix)
                        ? String(row.title.dropFirst(prefix.count)) : nil
                }
            if selected != nil { await refreshDetail() }
        } catch {
            notice = I18nService.shared.format(
                "settings.wf.notice.load-failed", error.localizedDescription)
        }
    }

    func select(_ name: String) {
        selected = name
        stepPage = 1
        tracePage = 1
        Task { await refreshDetail() }
    }

    func refreshDetail() async {
        guard let name = selected else { steps = []; trace = []; return }
        let currentStepPage = stepPage
        let currentTracePage = tracePage
        do {
            let stepRows = try await api.search("wf show \(name) \(currentStepPage)")
            let traceRows = try await api.search("wf trace \(name) \(currentTracePage)")
            guard selected == name && stepPage == currentStepPage &&
                    tracePage == currentTracePage else { return }
            steps = stepRows.filter { $0.kind == "Workflow" && $0.badge == "Step" }
            trace = traceRows.filter { $0.kind == "Workflow" && $0.badge == "Trace" }
        } catch {
            notice = I18nService.shared.format(
                "settings.wf.notice.detail-failed", error.localizedDescription)
        }
    }

    private func action(_ id: String, _ arg: String) async -> String? {
        guard !busy else { notice = I18nService.shared.t("settings.wf.notice.busy"); return nil }
        busy = true
        defer { busy = false }
        do {
            return try await api.runAction(id: id, arg: arg)
        } catch {
            notice = I18nService.shared.format(
                "settings.wf.notice.request-failed", error.localizedDescription)
            return nil
        }
    }

    func create() async {
        let draft = nameDraft.trimmingCharacters(in: .whitespacesAndNewlines)
        let name = draft.lowercased()
        guard !draft.isEmpty else { notice = I18nService.shared.t("settings.wf.enter-name"); return }
        if let existingPage = await pageContaining(name) {
            page = existingPage
            await refresh()
            select(name)
            notice = I18nService.shared.t("settings.wf.notice.exists")
            return
        }
        guard await action("workflow.create", draft) != nil else { return }
        guard let savedPage = await pageContaining(name) else {
            notice = I18nService.shared.t("settings.wf.notice.invalid-name")
            return
        }
        page = savedPage
        await refresh()
        nameDraft = ""
        select(name)
        notice = I18nService.shared.t("settings.wf.notice.created")
    }

    func append() async {
        guard let name = selected else { return }
        let text = stepDraft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { notice = I18nService.shared.t("settings.wf.enter-step"); return }
        let before = await totalSteps(for: name)
        guard await action("workflow.add", name + separator + text) != nil else { return }
        let after = await totalSteps(for: name)
        if let before, let after, after > before {
            stepPage = max(1, (after + 4) / 5)
            await refreshDetail()
            stepDraft = ""
            notice = I18nService.shared.t("settings.wf.notice.step-added")
        } else {
            await refreshDetail()
            notice = I18nService.shared.t("settings.wf.notice.step-unconfirmed")
        }
    }

    func remove(step: ResultRow) async {
        guard let name = selected else { return }
        let label = step.title.split(separator: " ").last.map(String.init) ?? ""
        guard let index = Int(label), index > 0 else {
            notice = I18nService.shared.t("settings.wf.notice.bad-index")
            return
        }
        let before = await totalSteps(for: name)
        guard await action("workflow.remove", name + separator + label) != nil else { return }
        let after = await totalSteps(for: name)
        if let before, let after, after < before {
            stepPage = min(stepPage, max(1, (after + 4) / 5))
            await refreshDetail()
            notice = I18nService.shared.t("settings.wf.notice.step-removed")
        } else {
            await refreshDetail()
            notice = I18nService.shared.t("settings.wf.notice.remove-unconfirmed")
        }
    }

    func delete() async {
        guard let name = selected else { return }
        guard await action("workflow.delete", name) != nil else { return }
        guard await pageContaining(name) == nil else {
            notice = I18nService.shared.t("settings.wf.notice.delete-unconfirmed")
            return
        }
        selected = nil
        steps = []
        trace = []
        page = 1
        await refresh()
        notice = I18nService.shared.t("settings.wf.notice.deleted")
    }

    func dryRun() async {
        guard let name = selected else { return }
        guard await action("workflow.dry", name + separator + inputDraft) != nil else { return }
        tracePage = 1
        await refreshDetail()
        if trace.contains(where: { $0.subtitle.hasPrefix("Status: failed") }) {
            notice = I18nService.shared.t("settings.wf.notice.dry-failed")
        } else {
            notice = I18nService.shared.t("settings.wf.notice.dry-ok")
        }
    }

    func copyResult() async {
        guard let name = selected else { return }
        guard steps.last?.subtitle == "copy" else {
            notice = I18nService.shared.t("settings.wf.notice.copy-only")
            return
        }
        guard let result = await action("workflow.run", name + separator + inputDraft)
        else { return }
        tracePage = 1
        await refreshDetail()
        notice = result == "copied"
            ? I18nService.shared.t("settings.wf.notice.copied")
            : I18nService.shared.t("settings.wf.notice.copy-unconfirmed")
    }

    func nextList() { page += 1; Task { await refresh() } }
    func previousList() { page = max(1, page - 1); Task { await refresh() } }
    func nextSteps() { stepPage += 1; Task { await refreshDetail() } }
    func previousSteps() { stepPage = max(1, stepPage - 1); Task { await refreshDetail() } }
    func nextTrace() { tracePage += 1; Task { await refreshDetail() } }
    func previousTrace() { tracePage = max(1, tracePage - 1); Task { await refreshDetail() } }

}

struct WorkflowEditorView: View {
    @ObservedObject var model: WorkflowEditorModel

    // Chrome strings ride the i18n single source; the parent SettingsView
    // re-renders this child on a language change, so labels follow live.
    private func t(_ key: String) -> String { I18nService.shared.t(key) }
    private func fmt(_ key: String, _ arg: Any) -> String {
        I18nService.shared.format(key, arg)
    }

    var body: some View {
        HStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 10) {
                Text(t("settings.nav.workflows"))
                    .font(.headline)
                List(model.names, id: \.self) { name in
                    Button(name) { model.select(name) }
                        .buttonStyle(.plain)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .accessibilityIdentifier("workflow-select-\(name)")
                }
                .listStyle(.plain)
                .accessibilityIdentifier("workflow-list")
                HStack {
                    Button("‹") { model.previousList() }.disabled(model.page <= 1)
                    Text("\(model.page)").accessibilityLabel("Workflow page \(model.page)")
                    Button("›") { model.nextList() }.disabled(model.names.count < 5)
                    Button(t("settings.wf.refresh")) { Task { await model.refresh() } }
                }
                HStack(spacing: 5) {
                    TextField(t("settings.wf.new-name"), text: $model.nameDraft)
                        .textFieldStyle(.roundedBorder)
                        .accessibilityIdentifier("workflow-create-name")
                    Button("+") { Task { await model.create() } }
                        .accessibilityIdentifier("workflow-create")
                        .disabled(model.busy)
                }
            }
            .frame(width: 175)
            .padding(12)

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 10) {
                    if let selected = model.selected {
                        HStack {
                            Text(selected).font(.title3.bold())
                            Spacer()
                            Button(t("settings.wf.delete"), role: .destructive) {
                                model.confirmDelete = true
                            }
                            .accessibilityIdentifier("workflow-delete")
                        }
                        Text(t("settings.wf.steps"))
                            .font(.caption).foregroundStyle(.secondary)
                        ForEach(model.steps, id: \.rowId) { row in
                            HStack {
                                VStack(alignment: .leading) {
                                    Text(row.title).font(.subheadline.bold())
                                    Text(row.subtitle).font(.caption)
                                        .textSelection(.enabled)
                                }
                                Spacer()
                                Button(t("settings.wf.remove")) { Task { await model.remove(step: row) } }
                                    .accessibilityIdentifier("workflow-remove-step")
                            }
                        }
                        HStack {
                            Button(t("settings.wf.prev")) { model.previousSteps() }
                                .disabled(model.stepPage <= 1)
                            Text(fmt("settings.wf.steps-page", model.stepPage))
                            Button(t("settings.wf.next")) { model.nextSteps() }
                                .disabled(model.steps.count < 5)
                        }.font(.caption)
                        HStack {
                            TextField(t("settings.wf.step-placeholder"),
                                      text: $model.stepDraft)
                                .accessibilityIdentifier("workflow-step-expression")
                            Button(t("settings.wf.add-step")) { Task { await model.append() } }
                                .disabled(model.busy || model.stepDraft.isEmpty)
                                .accessibilityIdentifier("workflow-add-step")
                        }
                        Divider()
                        Text(t("settings.wf.input")).font(.headline)
                        TextEditor(text: $model.inputDraft)
                            .frame(height: 65)
                            .border(Color.secondary.opacity(0.2))
                            .accessibilityIdentifier("workflow-input")
                        HStack {
                            Button(t("settings.wf.dry-run")) { Task { await model.dryRun() } }
                                .accessibilityIdentifier("workflow-dry-run")
                            Button(t("settings.wf.run-copy")) { Task { await model.copyResult() } }
                                .disabled(model.steps.last?.subtitle != "copy")
                                .accessibilityIdentifier("workflow-run-copy")
                        }
                        Text(t("settings.wf.clipboard-note"))
                            .font(.caption).foregroundStyle(.secondary)
                        Divider()
                        Text(t("settings.wf.trace")).font(.headline)
                        ForEach(model.trace, id: \.rowId) { row in
                            HStack {
                                Text(row.title).font(.caption.bold())
                                Spacer()
                                Text(row.subtitle).font(.caption)
                            }
                            .accessibilityIdentifier("workflow-trace-step")
                        }
                        HStack {
                            Button(t("settings.wf.prev")) { model.previousTrace() }
                                .disabled(model.tracePage <= 1)
                            Text(fmt("settings.wf.trace-page", model.tracePage))
                            Button(t("settings.wf.next")) { model.nextTrace() }
                                .disabled(model.trace.count < 5)
                        }.font(.caption)
                    } else {
                        Text(t("settings.wf.empty-list"))
                            .foregroundStyle(.secondary)
                    }
                    Text(model.notice)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                        .accessibilityIdentifier("workflow-status")
                }
                .padding(16)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .task { await model.refresh() }
        .confirmationDialog(t("settings.wf.delete-dialog"),
                            isPresented: $model.confirmDelete) {
            Button(t("settings.wf.delete-title"), role: .destructive) {
                Task { await model.delete() }
            }
        }
    }
}
