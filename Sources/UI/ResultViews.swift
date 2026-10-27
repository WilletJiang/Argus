import AppKit

final class FlippedView: NSView {
    override var isFlipped: Bool { true }
}

/// The window stays at its largest size; hits outside the rounded surface must fall through to apps below.
final class PanelContainer: NSView {
    var hitShape: NSView?
    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? {
        guard let hitShape, hitShape.frame.contains(point) else { return nil }
        return super.hitTest(point)
    }
}

final class ResultCell: NSTableCellView {
    private let icon = NSImageView()
    private let titleLabel = NSTextField(labelWithString: "")
    private let subtitleLabel = NSTextField(labelWithString: "")
    /// A plain bounded cache: NSCache evicts under its own policy, which re-rendered icons on every keystroke.
    private static var icons: [String: NSImage] = [:]
    private static var iconOrder: [String] = []
    static let iconLimit = 256

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        titleLabel.font = .systemFont(ofSize: 16, weight: .medium)
        subtitleLabel.font = .systemFont(ofSize: 13)
        subtitleLabel.textColor = .secondaryLabelColor
        titleLabel.lineBreakMode = .byTruncatingTail
        subtitleLabel.lineBreakMode = .byTruncatingMiddle
        for label in [titleLabel, subtitleLabel] { label.maximumNumberOfLines = 1 }
        icon.imageScaling = .scaleProportionallyUpOrDown
        [icon, titleLabel, subtitleLabel].forEach { addSubview($0) }
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    /// `nil` turns the cell into an invisible placeholder that keeps its row view in the table.
    /// Hidden views are recycled by NSTableView, so placeholders stay in the hierarchy at zero alpha.
    func configure(_ result: SearchResult?) {
        guard let result else {
            alphaValue = 0
            setAccessibilityElement(false)
            return
        }
        alphaValue = 1
        setAccessibilityElement(true)
        titleLabel.stringValue = result.title
        subtitleLabel.stringValue = result.snippet.isEmpty ? result.subtitle : result.snippet.replacingOccurrences(of: "\n", with: " ")
        if [.application, .file, .folder, .content].contains(result.kind) {
            if let cached = Self.icons[result.target] { icon.image = cached }
            else {
                let path = result.kind == .application ? URL(fileURLWithPath: result.target).resolvingSymlinksInPath().path : result.target
                let image = Self.thumbnail(for: path)
                if Self.iconOrder.count >= Self.iconLimit { Self.icons[Self.iconOrder.removeFirst()] = nil }
                Self.icons[result.target] = image
                Self.iconOrder.append(result.target)
                icon.image = image
            }
        } else { icon.image = NSImage(systemSymbolName: result.symbol ?? result.kind.symbol, accessibilityDescription: result.kind.label) }
        setAccessibilityLabel(result.title + ", " + result.kind.label + ", " + result.subtitle)
    }

    private static func thumbnail(for path: String) -> NSImage {
        let size = NSSize(width: 28, height: 28)
        let image = NSImage(size: size)
        autoreleasepool {
            let source = NSWorkspace.shared.icon(forFile: path)
            guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 56, pixelsHigh: 56, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0), let context = NSGraphicsContext(bitmapImageRep: bitmap) else { return }
            NSGraphicsContext.saveGraphicsState()
            NSGraphicsContext.current = context
            source.draw(in: NSRect(x: 0, y: 0, width: 56, height: 56), from: .zero, operation: .sourceOver, fraction: 1)
            NSGraphicsContext.restoreGraphicsState()
            bitmap.size = size
            image.addRepresentation(bitmap)
        }
        return image
    }

    override func layout() {
        super.layout()
        icon.frame = NSRect(x: 17, y: 12, width: 28, height: 28)
        titleLabel.frame = NSRect(x: 64, y: 28, width: bounds.width - 80, height: 21)
        subtitleLabel.frame = NSRect(x: 64, y: 8, width: bounds.width - 80, height: 18)
    }
}

final class ResultTable: NSTableView {
    var onHover: ((Int) -> Void)?
    /// Rows past this index are placeholders kept only so result changes never add or remove row views.
    var liveRows = 0

    override func accessibilityRows() -> [any NSAccessibilityRow]? { super.accessibilityRows()?.filter(isLive) }
    override func accessibilityVisibleRows() -> [any NSAccessibilityRow]? { super.accessibilityVisibleRows()?.filter(isLive) }
    override func accessibilityRowCount() -> Int { liveRows }
    private func isLive(_ element: any NSAccessibilityRow) -> Bool { (element as? NSView).map { row(for: $0) < liveRows } ?? true }
    private var hoverArea: NSTrackingArea?
    private var lastMouse = NSPoint(x: CGFloat.nan, y: CGFloat.nan)

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        guard hoverArea == nil else { return }
        let area = NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect], owner: self)
        addTrackingArea(area)
        hoverArea = area
    }

    // Rows sliding under a stationary pointer must not steal the keyboard selection, so only real movement counts.
    override func mouseMoved(with event: NSEvent) {
        super.mouseMoved(with: event)
        let mouse = NSEvent.mouseLocation
        guard mouse != lastMouse else { return }
        lastMouse = mouse
        let row = row(at: convert(event.locationInWindow, from: nil))
        if row >= 0, row < liveRows { onHover?(row) }
    }
}

final class ResultRow: NSTableRowView {
    static let horizontalInset: CGFloat = 4
    static let cornerRadius = SearchSurface.cornerRadius - SearchSurface.listInset - horizontalInset
    private let selection = CALayer()

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        selection.cornerCurve = .continuous
        selection.cornerRadius = Self.cornerRadius
        selection.isHidden = true
        layer?.insertSublayer(selection, at: 0)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    // A neutral selection keeps semantic text colors; AppKit's emphasized style turns them white.
    override var interiorBackgroundStyle: NSView.BackgroundStyle { .normal }
    override var isSelected: Bool { didSet { updateSelection() } }

    private func updateSelection() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        selection.frame = bounds.insetBy(dx: Self.horizontalInset, dy: 2)
        selection.isHidden = !isSelected || selectionHighlightStyle == .none
        effectiveAppearance.performAsCurrentDrawingAppearance {
            selection.backgroundColor = NSColor.labelColor.withAlphaComponent(NSWorkspace.shared.accessibilityDisplayShouldIncreaseContrast ? 0.22 : 0.10).cgColor
        }
        CATransaction.commit()
    }
    override func drawSelection(in dirtyRect: NSRect) { updateSelection() }
    override func layout() { super.layout(); updateSelection() }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        updateSelection()
        needsDisplay = true
    }
}

extension SearchResult {
    func hasSamePresentation(as other: SearchResult) -> Bool {
        id == other.id && title == other.title && subtitle == other.subtitle && kind == other.kind && target == other.target && snippet == other.snippet && symbol == other.symbol
    }
}
