import AppKit

final class PanelController: NSObject, NSSearchFieldDelegate, NSTableViewDataSource, NSTableViewDelegate {
    private let surface = SearchSurface()
    private let coordinator: SearchCoordinator
    private let motion: PanelMotion
    private let resultPreview = ResultPreview()
    var panel: SearchPanel { surface.panel }
    var field: SearchField { surface.field }
    var isPresented: Bool { motion.isPresented }
    var onSettings: (() -> Void)?
    var isToggleShortcut: ((NSEvent) -> Bool)?
    private var results: [SearchResult] = []
    private var scope = SearchScope.all
    private var monitor: Any?
    private var refreshWork: DispatchWorkItem?
    private var emptyWork: DispatchWorkItem?
    private var queryGeneration = 0
    private var submittedQuery = ""
    private var hasReceivedQueryResults = false
    private var selectedResultID: String?
    private var settingSelection = false
    #if ARGUS_BENCHMARK
    private var queryStarted = 0.0
    var onMetric: ((String, Double) -> Void)?
    #endif

    init(coordinator: SearchCoordinator) {
        self.coordinator = coordinator
        motion = PanelMotion(surface: surface)
        super.init()
        panel.resultPreview = resultPreview
        field.delegate = self
        field.target = self
        field.action = #selector(searchFieldAction)
        surface.table.dataSource = self
        surface.table.delegate = self
        surface.table.target = self
        surface.table.doubleAction = #selector(activateSelected)
        coordinator.onResults = { [weak self] values, milliseconds in self?.update(values, milliseconds: milliseconds) }
        NotificationCenter.default.addObserver(self, selector: #selector(lostFocus), name: NSWindow.didResignKeyNotification, object: panel)
        NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(accessibilityChanged), name: NSWorkspace.accessibilityDisplayOptionsDidChangeNotification, object: nil)
        updateFooter()
    }

    func show(query: String? = nil) {
        #if ARGUS_BENCHMARK
        let started = ProcessInfo.processInfo.systemUptime
        #endif
        let preservesSelection = (query == nil || query == field.stringValue) && scope == .all
        if let query { field.stringValue = query }
        scope = .all
        let opening = !panel.isVisible
        if opening { surface.position() }
        motion.present(header: true, results: !queryIsEmpty)
        installMonitor()
        search(preservingSelection: preservesSelection)
        panel.makeKeyAndOrderFront(nil)
        panel.makeFirstResponder(field)
        if query == nil, let editor = field.currentEditor() {
            editor.selectedRange = NSRange(location: 0, length: (editor.string as NSString).length)
        }
        #if ARGUS_BENCHMARK
        queryStarted = started
        onMetric?("showReadyMs", (ProcessInfo.processInfo.systemUptime - started) * 1000)
        #endif
    }

    func toggle() { isPresented ? hide() : show() }

    #if ARGUS_BENCHMARK
    func setBenchmarkQuery(_ query: String) {
        let started = ProcessInfo.processInfo.systemUptime
        field.stringValue = query
        search()
        queryStarted = started
    }
    #endif

    func hide(immediately: Bool = false) {
        refreshWork?.cancel(); refreshWork = nil
        emptyWork?.cancel(); emptyWork = nil
        queryGeneration += 1
        hasReceivedQueryResults = false
        resultPreview.close()
        motion.present(header: false, results: false, immediately: immediately)
        if let monitor { NSEvent.removeMonitor(monitor); self.monitor = nil }
    }

    func updateStatus(_ text: String) {
        refreshWork?.cancel()
        guard isPresented, !queryIsEmpty, text.hasPrefix("Index ready") else { return }
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.isPresented, !self.hasMarkedText else { return }
            self.search(preservingSelection: true)
        }
        refreshWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25, execute: work)
    }

    private var queryIsEmpty: Bool { field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
    private var hasMarkedText: Bool { (field.currentEditor() as? NSTextView)?.hasMarkedText() ?? false }

    func controlTextDidChange(_ obj: Notification) {
        guard !hasMarkedText, field.stringValue != submittedQuery else { return }
        search()
    }

    @objc private func searchFieldAction() {
        guard !hasMarkedText, field.stringValue != submittedQuery else { return }
        search()
    }

    func control(_ control: NSControl, textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
        guard !textView.hasMarkedText() else { return false }
        switch commandSelector {
        case #selector(NSResponder.moveDown(_:)): moveSelection(1); return true
        case #selector(NSResponder.moveUp(_:)): moveSelection(-1); return true
        case #selector(NSResponder.insertNewline(_:)): activateSelected(); return true
        case #selector(NSResponder.cancelOperation(_:)): hide(); return true
        case #selector(NSResponder.insertTab(_:)): showActions(); return true
        default: return false
        }
    }

    private func installMonitor() {
        guard monitor == nil else { return }
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            guard let self, self.panel.isKeyWindow else { return event }
            if self.isToggleShortcut?(event) == true {
                if !event.isARepeat { self.toggle() }
                return nil
            }
            let modifiers = event.modifierFlags.intersection([.command, .option, .shift, .control])
            guard modifiers == .command else { return event }
            if let number = Int(event.charactersIgnoringModifiers ?? ""), let scope = SearchScope(rawValue: number - 1) {
                self.scope = scope; self.search(); return nil
            }
            switch event.charactersIgnoringModifiers {
            case ",": self.onSettings?(); return nil
            case "y": self.preview(); return nil
            case "l", "\r": self.revealSelected(); return nil
            default: return event
            }
        }
    }

    private func search(preservingSelection: Bool = false) {
        submittedQuery = field.stringValue
        queryGeneration += 1
        emptyWork?.cancel(); emptyWork = nil
        hasReceivedQueryResults = false
        if !preservingSelection { selectedResultID = nil }
        updateFooter()
        if queryIsEmpty {
            motion.present(header: isPresented, results: false)
        } else {
            if results.isEmpty { surface.empty.stringValue = ""; surface.empty.isHidden = true }
            motion.present(header: isPresented, results: true)
        }
        #if ARGUS_BENCHMARK
        queryStarted = ProcessInfo.processInfo.systemUptime
        #endif
        coordinator.search(field.stringValue, scope: scope)
    }

    private func update(_ values: [SearchResult], milliseconds: Double) {
        guard isPresented, !queryIsEmpty else { return }
        guard milliseconds >= 0 else { return }
        if values.isEmpty {
            scheduleEmptyState()
            return
        }
        emptyWork?.cancel(); emptyWork = nil
        #if ARGUS_BENCHMARK
        let updateStart = ProcessInfo.processInfo.systemUptime
        #endif
        hasReceivedQueryResults = true
        applyResults(values)
        surface.empty.isHidden = true
        surface.scroll.isHidden = false
        updateFooter()
        #if ARGUS_BENCHMARK
        if queryStarted > 0 {
            onMetric?("resultUpdateMs", (ProcessInfo.processInfo.systemUptime - updateStart) * 1000)
            onMetric?("engineSearchMs", milliseconds)
            onMetric?("firstResultsMs", (ProcessInfo.processInfo.systemUptime - queryStarted) * 1000)
            queryStarted = 0
        }
        #endif
    }

    private func scheduleEmptyState() {
        guard emptyWork == nil else { return }
        let generation = queryGeneration
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.isPresented, self.queryGeneration == generation, !self.hasReceivedQueryResults else { return }
            self.applyResults([])
            self.surface.scroll.isHidden = true
            self.surface.empty.stringValue = "No results found"
            self.surface.empty.isHidden = false
            self.surface.footer.stringValue = "Try another name or type web followed by a search"
        }
        emptyWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2, execute: work)
    }

    private func applyResults(_ values: [SearchResult]) {
        let old = results
        results = values
        let table = surface.table
        settingSelection = true
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0
            context.allowsImplicitAnimation = false
            table.beginUpdates()
            if values.count < old.count { table.removeRows(at: IndexSet(integersIn: values.count..<old.count), withAnimation: []) }
            if values.count > old.count { table.insertRows(at: IndexSet(integersIn: old.count..<values.count), withAnimation: []) }
            table.endUpdates()
            for row in 0..<min(old.count, values.count) where !old[row].hasSamePresentation(as: values[row]) {
                if let cell = table.view(atColumn: 0, row: row, makeIfNecessary: false) as? ResultCell { cell.configure(values[row]) }
                else { table.reloadData(forRowIndexes: IndexSet(integer: row), columnIndexes: IndexSet(integer: 0)) }
            }
            if !values.isEmpty {
                let row = selectedResultID.flatMap { id in values.firstIndex { $0.id == id } } ?? 0
                if table.selectedRow != row { table.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false) }
            }
        }
        settingSelection = false
    }

    private func updateFooter() {
        surface.footer.stringValue = "↵ Open    ⌘Y Preview    Tab Actions"
        surface.scopeLabel.stringValue = scope == .all ? "" : scope.title + " · ⌘1 All"
    }

    private func moveSelection(_ delta: Int) {
        guard hasReceivedQueryResults, !results.isEmpty else { return }
        let table = surface.table
        let row = min(max(table.selectedRow + delta, 0), results.count - 1)
        selectedResultID = results[row].id
        table.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
        table.scrollRowToVisible(row)
        if resultPreview.isVisible { preview() }
    }

    private var selected: SearchResult? { hasReceivedQueryResults && results.indices.contains(surface.table.selectedRow) ? results[surface.table.selectedRow] : nil }

    @objc private func activateSelected() {
        guard let result = selected else { return }
        Preferences.shared.record(result)
        hide()
        ActionExecutor.execute(result) { [weak self] error in
            if let error { self?.show(); self?.surface.footer.stringValue = error }
        }
    }

    private func revealSelected() { if let result = selected { hide(); ActionExecutor.reveal(result) } }
    private func preview() {
        guard let result = selected else { return }
        resultPreview.show(result, relativeTo: surface.table.rect(ofRow: max(0, surface.table.selectedRow)), of: surface.table)
    }

    private func showActions() {
        guard let result = selected else { return }
        let menu = NSMenu()
        menu.addItem(withTitle: "Open", action: #selector(activateSelected), keyEquivalent: "").target = self
        if [.file, .folder, .content, .application].contains(result.kind) {
            menu.addItem(withTitle: "Preview", action: #selector(previewAction), keyEquivalent: "").target = self
            menu.addItem(withTitle: "Reveal in Finder", action: #selector(revealAction), keyEquivalent: "").target = self
        } else if !result.snippet.isEmpty { menu.addItem(withTitle: "Show Details", action: #selector(previewAction), keyEquivalent: "").target = self }
        let rect = surface.table.rect(ofRow: max(0, surface.table.selectedRow))
        menu.popUp(positioning: nil, at: NSPoint(x: rect.maxX - 130, y: rect.midY), in: surface.table)
    }

    @objc private func previewAction() { preview() }
    @objc private func revealAction() { revealSelected() }
    @objc private func lostFocus() {
        DispatchQueue.main.async { [weak self] in
            guard let self, self.isPresented, !self.panel.isKeyWindow else { return }
            if self.resultPreview.isVisible { return }
            if NSApp.keyWindow?.parent == self.panel { return }
            self.hide()
        }
    }
    @objc private func accessibilityChanged() { surface.updateAccessibility() }
    func numberOfRows(in tableView: NSTableView) -> Int { results.count }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let identifier = NSUserInterfaceItemIdentifier("cell")
        let cell = tableView.makeView(withIdentifier: identifier, owner: self) as? ResultCell ?? ResultCell(frame: .zero)
        cell.identifier = identifier; cell.configure(results[row]); return cell
    }
    func tableView(_ tableView: NSTableView, rowViewForRow row: Int) -> NSTableRowView? { ResultRow() }
    func tableView(_ tableView: NSTableView, shouldSelectRow row: Int) -> Bool { hasReceivedQueryResults }
    func tableViewSelectionDidChange(_ notification: Notification) {
        if !settingSelection { selectedResultID = selected?.id }
    }

    deinit {
        if let monitor { NSEvent.removeMonitor(monitor) }
        NotificationCenter.default.removeObserver(self)
        NSWorkspace.shared.notificationCenter.removeObserver(self)
    }
}
