import AppKit
import Quartz

final class SearchPanel: NSPanel {
    var resultPreview: ResultPreview?
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }
    override func acceptsPreviewPanelControl(_ panel: QLPreviewPanel!) -> Bool { resultPreview?.hasItem == true }
    override func beginPreviewPanelControl(_ panel: QLPreviewPanel!) { resultPreview?.beginControl(panel) }
    override func endPreviewPanelControl(_ panel: QLPreviewPanel!) { resultPreview?.endControl(panel) }
}

final class SearchSurface {
    static let width: CGFloat = 620
    static let inputHeight: CGFloat = 68
    static let expandedHeight: CGFloat = 312
    static let rowHeight: CGFloat = 52
    static let listInset: CGFloat = 10
    let panel: SearchPanel
    let field = SearchField()
    let table = NSTableView()
    let scroll = NSScrollView()
    let empty = NSTextField(labelWithString: "")
    let footer = NSTextField(labelWithString: "")
    let scopeLabel = NSTextField(labelWithString: "")
    private let root = FlippedView()
    private let glass = NSGlassEffectView()
    private let content = GlassContentView()
    private let body = FlippedView()

    init() {
        panel = SearchPanel(contentRect: NSRect(x: 0, y: 0, width: Self.width, height: Self.inputHeight), styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        panel.level = .floating
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient, .ignoresCycle]
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        panel.animationBehavior = .none
        panel.alphaValue = 0
        root.frame = NSRect(x: 0, y: 0, width: Self.width, height: Self.inputHeight)
        root.wantsLayer = true
        root.layer?.cornerRadius = GlassContentView.cornerRadius
        root.layer?.cornerCurve = .continuous
        root.layer?.masksToBounds = true
        panel.contentView = root
        glass.frame = root.bounds
        glass.autoresizingMask = [.width, .height]
        glass.style = .clear
        glass.cornerRadius = GlassContentView.cornerRadius
        content.frame = glass.bounds
        content.autoresizingMask = [.width, .height]
        glass.contentView = content
        root.addSubview(glass)

        let symbol = NSImageView(frame: NSRect(x: 25, y: 18, width: 32, height: 32))
        let font = NSFont.systemFont(ofSize: 30, weight: .regular)
        let image = NSImage(systemSymbolName: "magnifyingglass", accessibilityDescription: "Search")?.withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: font.pointSize, weight: .regular))
        symbol.image = image
        symbol.imageScaling = .scaleProportionallyUpOrDown
        symbol.contentTintColor = .secondaryLabelColor
        field.font = font
        field.placeholderString = "Search"
        field.isBordered = false
        field.drawsBackground = false
        field.focusRingType = .none
        field.cell?.isScrollable = true
        (field.cell as? NSSearchFieldCell)?.searchButtonCell = nil
        field.sendsSearchStringImmediately = true
        field.maximumRecents = 0
        field.setAccessibilityLabel("Search")
        // SF Symbols have text alignment insets. Keep the complete magnifier centered in a fixed box.
        content.addSubview(symbol)
        field.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(field)
        NSLayoutConstraint.activate([
            field.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 76),
            field.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -24),
            field.firstBaselineAnchor.constraint(equalTo: root.topAnchor, constant: Self.inputHeight / 2 + font.capHeight / 2),
            field.heightAnchor.constraint(equalToConstant: (font.ascender * 2 - font.capHeight).rounded())
        ])

        body.frame = NSRect(x: 0, y: Self.inputHeight, width: Self.width, height: Self.expandedHeight - Self.inputHeight)
        body.alphaValue = 0
        body.isHidden = true
        content.addSubview(body)
        let divider = NSBox(frame: NSRect(x: 16, y: 0, width: Self.width - 32, height: 1))
        divider.boxType = .separator
        body.addSubview(divider)
        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("result"))
        column.width = Self.width - Self.listInset * 2
        table.addTableColumn(column)
        table.headerView = nil
        table.rowHeight = Self.rowHeight
        table.intercellSpacing = .zero
        table.backgroundColor = .clear
        table.style = .plain
        table.selectionHighlightStyle = .regular
        table.setAccessibilityLabel("Search results")
        scroll.frame = NSRect(x: Self.listInset, y: 8, width: Self.width - Self.listInset * 2, height: Self.rowHeight * 4)
        scroll.documentView = table
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.scrollerStyle = .overlay
        table.frame.size.width = scroll.contentSize.width
        body.addSubview(scroll)
        empty.frame = NSRect(x: 28, y: 65, width: Self.width - 56, height: 32)
        empty.font = .systemFont(ofSize: 13)
        empty.textColor = .secondaryLabelColor
        empty.alignment = .center
        empty.isHidden = true
        body.addSubview(empty)
        footer.frame = NSRect(x: 24, y: body.bounds.height - 23, width: 330, height: 16)
        scopeLabel.frame = NSRect(x: Self.width - 230, y: body.bounds.height - 23, width: 206, height: 16)
        scopeLabel.alignment = .right
        for label in [footer, scopeLabel] {
            label.font = .systemFont(ofSize: 11)
            label.textColor = .secondaryLabelColor
            body.addSubview(label)
        }
        updateAccessibility()
        root.layoutSubtreeIfNeeded()
    }

    func setExpansion(_ progress: CGFloat) {
        let height = (Self.inputHeight + (Self.expandedHeight - Self.inputHeight) * progress).rounded()
        let frame = panel.frame
        if frame.height != height {
            panel.setFrame(NSRect(x: frame.minX, y: frame.maxY - height, width: Self.width, height: height), display: false)
            root.frame.size = panel.contentRect(forFrameRect: panel.frame).size
            glass.frame = root.bounds
            content.frame = root.bounds
            root.layoutSubtreeIfNeeded()
        }
        body.alphaValue = progress
        body.isHidden = progress == 0
    }

    func position() {
        let mouse = NSEvent.mouseLocation
        guard let screen = NSScreen.screens.first(where: { $0.frame.contains(mouse) }) ?? NSScreen.main else { return }
        let frame = screen.visibleFrame
        let top = frame.maxY - frame.height * 0.18
        panel.setFrameOrigin(NSPoint(x: frame.midX - Self.width / 2, y: top - panel.frame.height))
    }

    func updateAccessibility() {
        glass.style = NSWorkspace.shared.accessibilityDisplayShouldReduceTransparency ? .regular : .clear
        content.needsDisplay = true
        table.needsDisplay = true
    }
}
