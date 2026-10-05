import AppKit

final class FlippedView: NSView {
    override var isFlipped: Bool { true }
}

final class GlassContentView: NSView {
    static let cornerRadius: CGFloat = 34
    override var isFlipped: Bool { true }
    private let lightGradient = NSGradient(colorsAndLocations: (.white.withAlphaComponent(0.35), 0), (.white.withAlphaComponent(0.42), 0.40), (.white.withAlphaComponent(0.50), 1))!
    private let darkGradient = NSGradient(colorsAndLocations: (.black.withAlphaComponent(0.35), 0), (.black.withAlphaComponent(0.42), 0.40), (.black.withAlphaComponent(0.50), 1))!

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        // The parent window applies one continuous mask to the glass, overlay and controls.
        let gradient = effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua ? darkGradient : lightGradient
        gradient.draw(from: NSPoint(x: bounds.midX, y: 0), to: NSPoint(x: bounds.midX, y: bounds.height), options: [.drawsBeforeStartingLocation, .drawsAfterEndingLocation])
    }
}

final class ResultCell: NSTableCellView {
    private let icon = NSImageView()
    private let titleLabel = NSTextField(labelWithString: "")
    private let subtitleLabel = NSTextField(labelWithString: "")
    static let icons = NSCache<NSString, NSImage>()

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
        Self.icons.countLimit = 128
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    func configure(_ result: SearchResult) {
        titleLabel.stringValue = result.title
        subtitleLabel.stringValue = result.snippet.isEmpty ? result.subtitle : result.snippet.replacingOccurrences(of: "\n", with: " ")
        if [.application, .file, .folder, .content].contains(result.kind) {
            if let cached = Self.icons.object(forKey: result.target as NSString) { icon.image = cached }
            else {
                let path = result.kind == .application ? URL(fileURLWithPath: result.target).resolvingSymlinksInPath().path : result.target
                let image = Self.thumbnail(for: path)
                Self.icons.setObject(image, forKey: result.target as NSString)
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

final class ResultRow: NSTableRowView {
    static let horizontalInset: CGFloat = 4
    static let cornerRadius = GlassContentView.cornerRadius - SearchSurface.listInset - horizontalInset
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

final class SearchField: NSSearchField {
    override func becomeFirstResponder() -> Bool {
        let result = super.becomeFirstResponder()
        if let editor = currentEditor() { editor.selectedRange = NSRange(location: (editor.string as NSString).length, length: 0) }
        return result
    }
}
