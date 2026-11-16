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
    @Published var notice = "Create a workflow or select one to edit its steps."
    @Published var busy = false
    @Published var page = 1
    @Published var stepPage = 1
    @Published var tracePage = 1
    @Published var confirmDelete = false

    private let api: FulcrumAPI
    private let separator = "\u{001F}"

    init(api: FulcrumAPI) { self.api = api }

    func refresh() async {
        do {
            let found = try await api.search("wf list \(page)")
            let newNames = found.filter { $0.kind == "Workflow" && $0.badge == "Saved" }
                .compactMap { row -> String? in
                    let prefix = "Workflow · "
                    return row.title.hasPrefix(prefix)
                        ? String(row.title.dropFirst(prefix.count)) : nil
                }
            names = newNames
            if let selected, !selected.isEmpty {
                await refreshDetail()
            }
        } catch {
            notice = "Could not load workflows: \(error.localizedDescription)"
        }
    }

    func select(_ name: String) {
        selected = name
        stepPage = 1
        tracePage = 1
        Task { await refreshDetail() }
    }

    func refreshDetail() async {
        guard let selected else { steps = []; trace = []; return }
        do {
            async let stepResults = api.search("wf show \(selected) \(stepPage)")
            async let traceResults = api.search("wf trace \(selected) \(tracePage)")
            steps = try await stepResults.filter { $0.kind == "Workflow" && $0.badge == "Step" }
            trace = try await traceResults.filter { $0.kind == "Workflow" && $0.badge == "Trace" }
        } catch {
            notice = "Could not load steps or trace: \(error.localizedDescription)"
        }
    }

    private func action(_ id: String, _ arg: String) async -> Bool {
        guard !busy else { return false }
        busy = true
        defer { busy = false }
        do {
            let result = try await api.runAction(id: id, arg: arg)
            return result == "stay" || result == "copied"
        } catch {
            notice = "Workflow request failed: \(error.localizedDescription)"
            return false
        }
    }

    func create() async {
        let draft = nameDraft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !draft.isEmpty else { notice = "Enter a workflow name."; return }
        let before = try? await api.search("wf list 1")
        guard await action("workflow.create", draft) else { return }
        page = 1
        await refresh()
        // Backend actions return "stay" even on validation errors; read back
        // the authoritative store instead of claiming an unverified save.
        let created = names.contains(draft.lowercased())
        notice = created ? "Workflow created." : "Not created. Use a lowercase name beginning with a letter; check for duplicates."
        if created { nameDraft = ""; select(draft.lowercased()) }
        _ = before
    }

    func append() async {
        guard let selected else { return }
        let text = stepDraft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { notice = "Enter a step expression."; return }
        let previous = steps.count
        guard await action("workflow.add", selected + separator + text) else { return }
        // Reload the last page for validation even when the visible page is full.
        stepPage = 1
        await refreshDetail()
        notice = steps.count > previous ? "Step added." :
            "Step request sent. Check its syntax and the step list; use next page when full."
        if steps.count > previous { stepDraft = "" }
    }

    func remove(step: ResultRow) async {
        guard let selected else { return }
        // Backend row title has a canonical 1-based global index.
        let label = step.title.split(separator: " ").last.map(String.init) ?? ""
        guard Int(label) != nil else { notice = "Invalid step index."; return }
        guard await action("workflow.remove", selected + separator + label) else { return }
        await refreshDetail()
        notice = "Step removal requested; inspect the updated step list."
    }

    func delete() async {
        guard let selected else { return }
        guard await action("workflow.delete", selected) else { return }
        self.selected = nil
        steps = []
        trace = []
        page = 1
        await refresh()
        notice = "Deletion requested; verify that the name is no longer listed."
    }

    func dryRun() async {
        guard let selected else { return }
        guard await action("workflow.dry", selected + separator + inputDraft) else { return }
        tracePage = 1
        await refreshDetail()
        notice = "Dry run complete or rejected. Inspect the redacted trace; no clipboard side effects."
    }

    func copyResult() async {
        guard let selected else { return }
        guard steps.last?.subtitle == "copy" else {
            notice = "For safety, run from this window only when the final step is copy. Use the launcher for paste."
            return
        }
        guard await action("workflow.run", selected + separator + inputDraft) else { return }
        tracePage = 1
        await refreshDetail()
        notice = "Execution requested. The final copy step delivers the result to the system clipboard."
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

    var body: some View {
        HStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 10) {
                Text("Workflows")
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
                    Button("Refresh") { Task { await model.refresh() } }
                }
                HStack(spacing: 5) {
                    TextField("new-name", text: $model.nameDraft)
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
                            Button("Delete", role: .destructive) {
                                model.confirmDelete = true
                            }
                            .accessibilityIdentifier("workflow-delete")
                        }
                        Text("Ordered steps — backend validates each expression.")
                            .font(.caption).foregroundStyle(.secondary)
                        ForEach(model.steps, id: \.rowId) { row in
                            HStack {
                                VStack(alignment: .leading) {
                                    Text(row.title).font(.subheadline.bold())
                                    Text(row.subtitle).font(.caption)
                                        .textSelection(.enabled)
                                }
                                Spacer()
                                Button("Remove") { Task { await model.remove(step: row) } }
                                    .accessibilityIdentifier("workflow-remove-step")
                            }
                        }
                        HStack {
                            Button("Previous") { model.previousSteps() }
                                .disabled(model.stepPage <= 1)
                            Text("Steps \(model.stepPage)")
                            Button("Next") { model.nextSteps() }
                                .disabled(model.steps.count < 5)
                        }.font(.caption)
                        HStack {
                            TextField("trim, lines unique, require text…",
                                      text: $model.stepDraft)
                                .accessibilityIdentifier("workflow-step-expression")
                            Button("Add step") { Task { await model.append() } }
                                .disabled(model.busy || model.stepDraft.isEmpty)
                                .accessibilityIdentifier("workflow-add-step")
                        }
                        Divider()
                        Text("Input").font(.headline)
                        TextEditor(text: $model.inputDraft)
                            .frame(height: 65)
                            .border(.quaternary)
                            .accessibilityIdentifier("workflow-input")
                        HStack {
                            Button("Dry run") { Task { await model.dryRun() } }
                                .accessibilityIdentifier("workflow-dry-run")
                            Button("Run to clipboard") { Task { await model.copyResult() } }
                                .disabled(model.steps.last?.subtitle != "copy")
                                .accessibilityIdentifier("workflow-run-copy")
                        }
                        Text("Use @clipboard to explicitly read the latest recorded clipboard text. No automatic execution.")
                            .font(.caption).foregroundStyle(.secondary)
                        Divider()
                        Text("Debug trace").font(.headline)
                        ForEach(model.trace, id: \.rowId) { row in
                            HStack {
                                Text(row.title).font(.caption.bold())
                                Spacer()
                                Text(row.subtitle).font(.caption)
                            }
                            .accessibilityIdentifier("workflow-trace-step")
                        }
                        HStack {
                            Button("Previous") { model.previousTrace() }
                                .disabled(model.tracePage <= 1)
                            Text("Trace \(model.tracePage)")
                            Button("Next") { model.nextTrace() }
                                .disabled(model.trace.count < 5)
                        }.font(.caption)
                    } else {
                        Text("Create a workflow or choose one from the list.")
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
        .confirmationDialog("Delete this workflow permanently?",
                            isPresented: $model.confirmDelete) {
            Button("Delete workflow", role: .destructive) {
                Task { await model.delete() }
            }
        }
    }
}
