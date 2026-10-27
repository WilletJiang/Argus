import AppKit

final class PanelController: NSObject, NSSearchFieldDelegate, NSTableViewDataSource, NSTableViewDelegate {
    private let surface: SearchSurface
    private let coordinator: SearchCoordinator
    private let motion: PanelMotion
    private let resultPreview = ResultPreview()
    var panel: SearchPanel { surface.panel }
    var field: SearchField { surface.field }
    var shape: NSView { surface.shape }
    var isPresented: Bool { motion.isPresented }
    var onSettings: (() -> Void)?
    var isToggleShortcut: ((NSEvent) -> Bool)?
    private var results: [SearchResult] = []
    private var scope = SearchScope.all
    private var monitor: Any?
    private var clickMonitor: PanelClickMonitor!
    private var showingActions = false
    private var refreshWork: DispatchWorkItem?
    private var emptyWork: DispatchWorkItem?
    private var queryGeneration = 0
    private var submittedQuery = ""
    /// What the user typed. The field may additionally show a selected inline completion after it.
    private(set) var typedQuery = ""
    private(set) var completion: String?
    private var allowsCompletion = false
    private var applyingCompletion = false
    private var replacesSelectionOnShow = false
    private var activatesWhenReady = false
    private var awaitsFirstBatch = false
    private var hasReceivedQueryResults = false
    private var selectedResultID: String?
    private var settingSelection = false
    #if ARGUS_BENCHMARK
    private var queryStarted = 0.0
    var onMetric: ((String, Double) -> Void)?
    #endif

    init(coordinator: SearchCoordinator, surface: SearchSurface = SearchSurface()) {
        self.surface = surface
        self.coordinator = coordinator
        motion = PanelMotion(surface: surface)
        super.init()
        clickMonitor = PanelClickMonitor(panel: panel, shape: shape, keepsOpen: { [weak self] window in
            self?.showingActions == true || self?.resultPreview.owns(window) == true
        }, dismiss: { [weak self] in self?.hide() })
        panel.resultPreview = resultPreview
        field.delegate = self
        field.target = self
        field.action = #selector(searchFieldAction)
        surface.table.dataSource = self
        surface.table.delegate = self
        surface.table.target = self
        surface.table.doubleAction = #selector(activateSelected)
        surface.table.onHover = { [weak self] row in
            guard let self, row != self.surface.table.selectedRow else { return }
            self.select(row, scroll: false)
        }
        coordinator.onResults = { [weak self] values, milliseconds in self?.update(values, milliseconds: milliseconds) }
        NotificationCenter.default.addObserver(self, selector: #selector(lostFocus), name: NSWindow.didResignKeyNotification, object: panel)
        NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(accessibilityChanged), name: NSWorkspace.accessibilityDisplayOptionsDidChangeNotification, object: nil)
        NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(activeSpaceChanged), name: NSWorkspace.activeSpaceDidChangeNotification, object: nil)
        updateFooter()
    }

    func show(query: String? = nil) {
        #if ARGUS_BENCHMARK
        let started = ProcessInfo.processInfo.systemUptime
        #endif
        let preservesSelection = (query == nil || query == typedQuery) && scope == .all
        if let query, query != typedQuery {
            setQuery(query)
            // Rows for a different query must not open the surface at their height.
            if !results.isEmpty { applyResults([]) }
        }
        scope = .all
        let opening = !panel.isVisible
        if opening {
            surface.position()
            if surface.table.numberOfRows != tableRows { surface.table.noteNumberOfRowsChanged() }
            motion.prepareAppearance()
        }
        if !motion.isPreparing { motion.present(true, height: targetHeight) }
        installMonitor()
        clickMonitor.start()
        search(preservingSelection: preservesSelection)
        panel.makeKeyAndOrderFront(nil)
        panel.makeFirstResponder(field)
        if query == nil, let editor = field.currentEditor() {
            editor.selectedRange = NSRange(location: 0, length: (editor.string as NSString).length)
            replacesSelectionOnShow = editor.selectedRange.length > 0
        }
        if opening {
            surface.prepareAppearance { [weak self] in
                guard let self, self.motion.isPreparing else { return }
                self.motion.present(true, height: self.targetHeight)
            }
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
        setQuery(query)
        search()
        queryStarted = started
    }
    #endif

    func hide(immediately: Bool = false) {
        clickMonitor.stop()
        surface.cancelAppearancePreparation()
        refreshWork?.cancel(); refreshWork = nil
        emptyWork?.cancel(); emptyWork = nil
        queryGeneration += 1
        hasReceivedQueryResults = false
        activatesWhenReady = false
        awaitsFirstBatch = false
        setCompletion(nil)
        allowsCompletion = false
        resultPreview.close()
        motion.present(false, height: motion.heightTarget, immediately: immediately)
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

    private var queryIsEmpty: Bool { typedQuery.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
    private var editor: NSTextView? { field.currentEditor() as? NSTextView }
    private var hasMarkedText: Bool { editor?.hasMarkedText() ?? false }

    /// The surface height for the current state. While a query is pending, the previous height holds
    /// so the panel never collapses and re-expands between keystrokes.
    private var targetHeight: CGFloat {
        if queryIsEmpty { return SearchSurface.inputHeight }
        if !surface.empty.isHidden { return SearchSurface.height(rows: SearchSurface.emptyRows) }
        if results.isEmpty { return motion.heightTarget }
        return SearchSurface.height(rows: min(results.count, surface.maxRows))
    }

    private func resize() {
        if motion.isPresented && !motion.isPreparing { motion.present(true, height: targetHeight) }
    }

    private func setQuery(_ query: String) {
        completion = nil
        allowsCompletion = false
        field.stringValue = query
        typedQuery = query
    }

    func controlTextDidChange(_ obj: Notification) { textChanged() }
    @objc private func searchFieldAction() { textChanged() }

    private func textChanged() {
        guard !applyingCompletion, !hasMarkedText else { return }
        let text = field.stringValue
        // Our own completion edits and duplicate action/notification pairs change nothing the user typed.
        if text == typedQuery + (completion ?? "") { return }
        let length = (text as NSString).length
        let caretAtEnd = editor.map { $0.selectedRange == NSRange(location: length, length: 0) } ?? true
        // Only typing at the end earns a completion; deleting or editing in the middle never re-adds one.
        // Typing over the text selected on reopen counts as typing even when it shortens the old query.
        let extends = length > (typedQuery as NSString).length && text.hasPrefix(typedQuery)
        // Deleting the selected completion is not typing forward; replacing the whole query on reopen is.
        let forward = caretAtEnd && length > 0 && (extends || replacesSelectionOnShow)
        replacesSelectionOnShow = false
        completion = nil
        allowsCompletion = forward
        typedQuery = text
        guard text != submittedQuery else { return }
        search()
        if forward { refreshCompletion() }
    }

    func control(_ control: NSControl, textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
        guard !textView.hasMarkedText() else { return false }
        switch commandSelector {
        case #selector(NSResponder.moveDown(_:)): moveSelection(1); return true
        case #selector(NSResponder.moveUp(_:)): moveSelection(-1); return true
        case #selector(NSResponder.insertNewline(_:)): activateSelected(); return true
        case #selector(NSResponder.cancelOperation(_:)): cancel(); return true
        case #selector(NSResponder.insertTab(_:)): showActions(); return true
        case #selector(NSResponder.moveRight(_:)), #selector(NSResponder.moveToEndOfLine(_:)),
             #selector(NSResponder.moveToRightEndOfLine(_:)), #selector(NSResponder.moveToEndOfParagraph(_:)),
             #selector(NSResponder.moveToEndOfDocument(_:)):
            guard completion != nil else { return false }
            acceptCompletion(); return true
        case #selector(NSResponder.moveLeft(_:)):
            if completion != nil { allowsCompletion = false; setCompletion(nil) }
            return false
        default: return false
        }
    }

    /// Escape clears the query first, then the scope, and only then closes the panel.
    private func cancel() {
        if !field.stringValue.isEmpty {
            setQuery("")
            search()
        } else if scope != .all {
            scope = .all
            search()
        } else { hide() }
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
        submittedQuery = typedQuery
        queryGeneration += 1
        emptyWork?.cancel(); emptyWork = nil
        let keepRows = preservingSelection && !results.isEmpty
        hasReceivedQueryResults = keepRows
        activatesWhenReady = false
        awaitsFirstBatch = !queryIsEmpty && !keepRows
        if !preservingSelection { selectedResultID = nil }
        updateFooter()
        if !keepRows, !queryIsEmpty, surface.height <= SearchSurface.inputHeight, motion.heightTarget <= SearchSurface.inputHeight, !results.isEmpty {
            // Rows left over from an earlier query must not flash when the surface opens again.
            applyResults([])
            surface.empty.isHidden = true
            surface.scroll.isHidden = false
        }
        resize()
        #if ARGUS_BENCHMARK
        queryStarted = ProcessInfo.processInfo.systemUptime
        #endif
        coordinator.search(typedQuery, scope: scope)
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
        let ordered = hasReceivedQueryResults ? ResultRanking.stabilized(values, after: results) : values
        hasReceivedQueryResults = true
        awaitsFirstBatch = false
        applyResults(ordered)
        surface.empty.isHidden = true
        surface.scroll.isHidden = false
        updateFooter()
        resize()
        refreshCompletion()
        #if ARGUS_BENCHMARK
        if queryStarted > 0 {
            onMetric?("resultUpdateMs", (ProcessInfo.processInfo.systemUptime - updateStart) * 1000)
            onMetric?("engineSearchMs", milliseconds)
            onMetric?("firstResultsMs", (ProcessInfo.processInfo.systemUptime - queryStarted) * 1000)
            queryStarted = 0
        }
        #endif
        if activatesWhenReady { activatesWhenReady = false; activateSelected() }
    }

    private func scheduleEmptyState() {
        guard emptyWork == nil else { return }
        let generation = queryGeneration
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.isPresented, self.queryGeneration == generation, !self.hasReceivedQueryResults else { return }
            self.activatesWhenReady = false
            self.awaitsFirstBatch = false
            self.applyResults([])
            self.setCompletion(nil)
            self.surface.scroll.isHidden = true
            self.surface.empty.stringValue = "No results found"
            self.surface.empty.isHidden = false
            self.surface.footer.stringValue = "Try another name or type web followed by a search"
            self.resize()
        }
        emptyWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2, execute: work)
    }

    /// The table always holds at least one screenful of rows. Fewer results turn the tail into hidden
    /// placeholders, so a keystroke only reconfigures cells instead of inserting and removing row views.
    private var rowCapacity: Int { surface.maxRows }
    private var tableRows: Int { max(results.count, rowCapacity) }

    private func applyResults(_ values: [SearchResult]) {
        let old = results
        let oldRows = surface.table.numberOfRows
        results = values
        let newRows = tableRows
        let table = surface.table
        table.liveRows = values.count
        settingSelection = true
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0
            context.allowsImplicitAnimation = false
            if oldRows != newRows {
                table.beginUpdates()
                if newRows < oldRows { table.removeRows(at: IndexSet(integersIn: newRows..<oldRows), withAnimation: []) }
                if newRows > oldRows { table.insertRows(at: IndexSet(integersIn: oldRows..<newRows), withAnimation: []) }
                table.endUpdates()
            }
            for row in 0..<min(oldRows, newRows) {
                let previous = old.indices.contains(row) ? old[row] : nil
                let next = values.indices.contains(row) ? values[row] : nil
                if let previous, let next, previous.hasSamePresentation(as: next) { continue }
                if previous == nil && next == nil { continue }
                if let cell = table.view(atColumn: 0, row: row, makeIfNecessary: false) as? ResultCell { cell.configure(next) }
            }
            if values.isEmpty, table.selectedRow >= 0 { table.deselectAll(nil) }
            if !values.isEmpty {
                let row = selectedResultID.flatMap { id in values.firstIndex { $0.id == id } } ?? 0
                if table.selectedRow != row { table.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false) }
            }
        }
        settingSelection = false
    }

    // MARK: Inline completion

    static func completion(of typed: String, with title: String) -> String? {
        guard !typed.isEmpty,
              let range = title.range(of: typed, options: [.anchored, .caseInsensitive, .diacriticInsensitive, .widthInsensitive]),
              range.upperBound < title.endIndex else { return nil }
        return String(title[range.upperBound...])
    }

    /// Completes toward the top row while it is selected. Rows from the previous keystroke are used as a
    /// prediction until the new batch arrives, so the suggestion does not blink while typing.
    private func refreshCompletion() {
        guard !applyingCompletion else { return }
        var suffix: String?
        if allowsCompletion, surface.empty.isHidden, surface.table.selectedRow == 0, let top = results.first {
            suffix = Self.completion(of: typedQuery, with: top.title)
        }
        setCompletion(suffix)
    }

    private func setCompletion(_ suffix: String?) {
        guard suffix != completion else { return }
        guard let editor, !editor.hasMarkedText() else { completion = nil; return }
        let typedLength = (typedQuery as NSString).length
        let shownLength = ((completion ?? "") as NSString).length
        // Only touch text that is still exactly ours: the typed query followed by the selected completion.
        guard editor.string == typedQuery + (completion ?? ""),
              editor.selectedRange == NSRange(location: typedLength, length: shownLength) else { completion = nil; return }
        let replacement = suffix ?? ""
        let range = NSRange(location: typedLength, length: shownLength)
        applyingCompletion = true
        if editor.shouldChangeText(in: range, replacementString: replacement) {
            editor.replaceCharacters(in: range, with: replacement)
            editor.didChangeText()
            editor.setSelectedRange(NSRange(location: typedLength, length: (replacement as NSString).length))
            completion = suffix
        } else { completion = nil }
        applyingCompletion = false
    }

    private func acceptCompletion() {
        guard completion != nil, let editor else { return }
        completion = nil
        allowsCompletion = false
        editor.setSelectedRange(NSRange(location: (editor.string as NSString).length, length: 0))
        typedQuery = field.stringValue
        if surface.table.selectedRow == 0 { selectedResultID = results.first?.id }
        search(preservingSelection: true)
    }

    // MARK: Selection and actions

    private func updateFooter() {
        surface.footer.stringValue = "↵ Open    ⌘Y Preview    Tab Actions"
        surface.scopeLabel.stringValue = scope == .all ? "" : scope.title + " · ⌘1 All"
    }

    private func moveSelection(_ delta: Int) {
        guard hasReceivedQueryResults, !results.isEmpty else { return }
        select(min(max(surface.table.selectedRow + delta, 0), results.count - 1), scroll: true)
    }

    private func select(_ row: Int, scroll: Bool) {
        guard hasReceivedQueryResults, results.indices.contains(row) else { return }
        let table = surface.table
        selectedResultID = results[row].id
        if table.selectedRow != row { table.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false) }
        if scroll { table.scrollRowToVisible(row) }
        refreshCompletion()
        if resultPreview.isVisible { preview() }
    }

    private var selected: SearchResult? { hasReceivedQueryResults && results.indices.contains(surface.table.selectedRow) ? results[surface.table.selectedRow] : nil }

    @objc private func activateSelected() {
        guard let result = selected else {
            // Return pressed before the first batch arrived: open the top hit as soon as it lands.
            if awaitsFirstBatch { activatesWhenReady = true }
            return
        }
        Preferences.shared.record(result)
        hide(immediately: true)
        ActionExecutor.execute(result) { [weak self] error in
            if let error { self?.show(); self?.surface.footer.stringValue = error }
        }
    }

    private func revealSelected() { if let result = selected { hide(immediately: true); ActionExecutor.reveal(result) } }
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
        showingActions = true
        defer { showingActions = false }
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
    @objc private func activeSpaceChanged() {
        hide(immediately: true)
        surface.resetAppearanceContext()
    }
    func numberOfRows(in tableView: NSTableView) -> Int { tableRows }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let identifier = NSUserInterfaceItemIdentifier("cell")
        let cell = tableView.makeView(withIdentifier: identifier, owner: self) as? ResultCell ?? ResultCell(frame: .zero)
        cell.identifier = identifier
        cell.configure(results.indices.contains(row) ? results[row] : nil)
        return cell
    }
    func tableView(_ tableView: NSTableView, rowViewForRow row: Int) -> NSTableRowView? {
        let identifier = NSUserInterfaceItemIdentifier("row")
        if let reused = tableView.makeView(withIdentifier: identifier, owner: self) as? ResultRow { return reused }
        let created = ResultRow(frame: .zero)
        created.identifier = identifier
        return created
    }
    func tableView(_ tableView: NSTableView, shouldSelectRow row: Int) -> Bool { hasReceivedQueryResults && results.indices.contains(row) }
    func tableViewSelectionDidChange(_ notification: Notification) {
        guard !settingSelection else { return }
        selectedResultID = selected?.id
        refreshCompletion()
    }

    deinit {
        if let monitor { NSEvent.removeMonitor(monitor) }
        NotificationCenter.default.removeObserver(self)
        NSWorkspace.shared.notificationCenter.removeObserver(self)
    }
}
