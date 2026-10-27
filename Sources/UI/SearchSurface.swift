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
    static let cornerRadius: CGFloat = 34
    static let width: CGFloat = 620
    static let inputHeight: CGFloat = 68
    static let rowHeight: CGFloat = 52
    static let listInset: CGFloat = 10
    static let listTop: CGFloat = 8
    static let footerHeight: CGFloat = 28
    static let emptyRows = 2
    static let rowLimit = 3...8
    static let screenMargin: CGFloat = 24
    /// The body fades in over this much of the expansion so a collapsing list never shows a hard edge.
    static let bodyFadeDistance: CGFloat = 48

    static func height(rows: Int) -> CGFloat {
        rows <= 0 ? inputHeight : inputHeight + listTop + CGFloat(rows) * rowHeight + footerHeight
    }

    let panel: SearchPanel
    let field = SearchField()
    let table = ResultTable()
    let scroll = NSScrollView()
    let empty = NSTextField(labelWithString: "")
    let footer = NSTextField(labelWithString: "")
    let scopeLabel = NSTextField(labelWithString: "")
    /// The visible rounded surface. The window stays at its largest size and only this view resizes,
    /// so expansion never round-trips through the window server.
    let shape = FlippedView()
    private(set) var maxRows = rowLimit.upperBound
    private(set) var height = inputHeight
    private var scale: CGFloat = 1
    private let container = PanelContainer()
    private let glass = NSGlassEffectView()
    private let content = GlassContentView()
    private let body = FlippedView()
    private let divider = NSBox()
    private let captureBackground: BackdropAppearance.Capture?
    private let canCaptureBackground: () -> Bool
    private lazy var backdrop = BackdropAppearance(panel: panel, shape: shape, capture: captureBackground, canCapture: canCaptureBackground)

    func prepareAppearance(completion: @escaping () -> Void) {
        content.finishAppearanceTransition()
        backdrop.prepare { [weak self] in
            self?.content.finishAppearanceTransition()
            completion()
        }
    }
    func cancelAppearancePreparation() {
        backdrop.cancel()
        content.finishAppearanceTransition()
    }

    func resetAppearanceContext() {
        backdrop.invalidate()
        content.finishAppearanceTransition()
    }

    var maxHeight: CGFloat { Self.height(rows: maxRows) }

    init(captureBackground: BackdropAppearance.Capture? = nil, canCaptureBackground: @escaping () -> Bool = CGPreflightScreenCaptureAccess) {
        self.captureBackground = captureBackground
        self.canCaptureBackground = canCaptureBackground
        let initialHeight = Self.height(rows: maxRows)
        panel = SearchPanel(contentRect: NSRect(x: 0, y: 0, width: Self.width, height: initialHeight), styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        panel.level = .floating
        panel.collectionBehavior = [.moveToActiveSpace, .fullScreenAuxiliary, .transient, .ignoresCycle]
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        panel.animationBehavior = .none
        panel.alphaValue = 0
        container.frame = NSRect(x: 0, y: 0, width: Self.width, height: initialHeight)
        container.wantsLayer = true
        panel.contentView = container
        shape.frame = NSRect(x: 0, y: 0, width: Self.width, height: Self.inputHeight)
        shape.wantsLayer = true
        shape.layer?.cornerRadius = Self.cornerRadius
        shape.layer?.cornerCurve = .continuous
        shape.layer?.masksToBounds = true
        container.hitShape = shape
        container.addSubview(shape)
        glass.frame = shape.bounds
        glass.autoresizingMask = [.width, .height]
        glass.style = .clear
        glass.cornerRadius = Self.cornerRadius
        content.frame = glass.bounds
        content.autoresizingMask = [.width, .height]
        glass.contentView = content
        shape.addSubview(glass)

        let symbol = NSImageView(frame: NSRect(x: 25, y: 18, width: 32, height: 32))
        let font = NSFont.systemFont(ofSize: 30, weight: .regular)
        let image = NSImage(systemSymbolName: "magnifyingglass", accessibilityDescription: "Search")?.withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: font.pointSize, weight: .regular))
        symbol.image = image
        symbol.imageScaling = .scaleProportionallyUpOrDown
        symbol.contentTintColor = .secondaryLabelColor
        field.caretAlignmentView = symbol
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
            field.leadingAnchor.constraint(equalTo: shape.leadingAnchor, constant: 76),
            field.trailingAnchor.constraint(equalTo: shape.trailingAnchor, constant: -24),
            field.firstBaselineAnchor.constraint(equalTo: shape.topAnchor, constant: Self.inputHeight / 2 + font.capHeight / 2 + 0.5),
            field.heightAnchor.constraint(equalToConstant: (font.ascender * 2 - font.capHeight).rounded())
        ])

        body.alphaValue = 0
        body.isHidden = true
        content.addSubview(body)
        divider.frame = NSRect(x: 16, y: 0, width: Self.width - 32, height: 1)
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
        scroll.documentView = table
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.scrollerStyle = .overlay
        body.addSubview(scroll)
        empty.font = .systemFont(ofSize: 13)
        empty.textColor = .secondaryLabelColor
        empty.alignment = .center
        empty.isHidden = true
        body.addSubview(empty)
        scopeLabel.alignment = .right
        for label in [footer, scopeLabel] {
            label.font = .systemFont(ofSize: 11)
            label.textColor = .secondaryLabelColor
            body.addSubview(label)
        }
        layoutBody()
        updateAccessibility()
        shape.layoutSubtreeIfNeeded()
    }

    /// The list keeps its full capacity; the shape clips it, so changing the row count never re-tiles the table.
    private func layoutBody() {
        body.frame = NSRect(x: 0, y: Self.inputHeight, width: Self.width, height: maxHeight - Self.inputHeight)
        scroll.frame = NSRect(x: Self.listInset, y: Self.listTop, width: Self.width - Self.listInset * 2, height: Self.rowHeight * CGFloat(maxRows))
        table.frame.size.width = scroll.contentSize.width
        let listHeight = Self.rowHeight * CGFloat(Self.emptyRows)
        empty.frame = NSRect(x: 28, y: Self.listTop + (listHeight - 32) / 2, width: Self.width - 56, height: 32)
        footer.frame = NSRect(x: 24, y: 0, width: 330, height: 16)
        scopeLabel.frame = NSRect(x: Self.width - 230, y: 0, width: 206, height: 16)
        placeFooter()
    }

    private func placeFooter() {
        let y = height - Self.inputHeight - 23
        footer.setFrameOrigin(NSPoint(x: footer.frame.minX, y: y))
        scopeLabel.setFrameOrigin(NSPoint(x: scopeLabel.frame.minX, y: y))
    }

    /// Returns whether the visible outline changed, so the caller can refresh the window shadow once per frame.
    @discardableResult
    func setHeight(_ value: CGFloat) -> Bool {
        let backing = panel.backingScaleFactor > 0 ? panel.backingScaleFactor : 2
        let value = min(max((value * backing).rounded() / backing, Self.inputHeight), maxHeight)
        guard value != height else { return false }
        height = value
        shape.setFrameSize(NSSize(width: Self.width, height: value))
        let reveal = value - Self.inputHeight
        body.alphaValue = min(reveal / Self.bodyFadeDistance, 1)
        body.isHidden = reveal <= 0
        placeFooter()
        updateTransform()
        return true
    }

    @discardableResult
    func setPresence(_ alpha: CGFloat, scale: CGFloat) -> Bool {
        if panel.alphaValue != alpha { panel.alphaValue = alpha }
        guard scale != self.scale else { return false }
        self.scale = scale
        updateTransform()
        return true
    }

    /// Scales the surface about its own center. AppKit owns the shape layer's transform, so the scale lives on the
    /// container's sublayer transform, expressed relative to the container layer's anchor point.
    private func updateTransform() {
        guard let layer = container.layer else { return }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        if scale == 1 { layer.sublayerTransform = CATransform3DIdentity }
        else {
            let frame = shape.layer?.frame ?? shape.frame
            let anchor = CGPoint(x: layer.bounds.minX + layer.anchorPoint.x * layer.bounds.width, y: layer.bounds.minY + layer.anchorPoint.y * layer.bounds.height)
            let dx = frame.midX - anchor.x, dy = frame.midY - anchor.y
            var transform = CATransform3DMakeTranslation(dx, dy, 0)
            transform = CATransform3DScale(transform, scale, scale, 1)
            layer.sublayerTransform = CATransform3DTranslate(transform, -dx, -dy, 0)
        }
        CATransaction.commit()
    }

    func position() {
        let mouse = NSEvent.mouseLocation
        guard let screen = NSScreen.screens.first(where: { $0.frame.contains(mouse) }) ?? NSScreen.main else { return }
        let frame = screen.visibleFrame
        let top = frame.maxY - frame.height * 0.18
        let available = top - frame.minY - Self.screenMargin - Self.inputHeight - Self.listTop - Self.footerHeight
        let rows = min(max(Int(available / Self.rowHeight), Self.rowLimit.lowerBound), Self.rowLimit.upperBound)
        if rows != maxRows {
            maxRows = rows
            layoutBody()
        }
        panel.setFrame(NSRect(x: frame.midX - Self.width / 2, y: top - maxHeight, width: Self.width, height: maxHeight), display: false)
    }

    func updateAccessibility() {
        glass.style = .clear
        content.needsDisplay = true
        table.needsDisplay = true
    }
}
