import AppKit

@main
enum PanelPresentationTests {
    static func wait(_ seconds: Double) {
        let deadline = Date(timeIntervalSinceNow: seconds)
        while Date() < deadline { RunLoop.main.run(until: min(deadline, Date(timeIntervalSinceNow: 0.01))) }
    }

    static func find<T: NSView>(_ type: T.Type, in view: NSView) -> T? {
        if let found = view as? T { return found }
        for child in view.subviews { if let found = find(type, in: child) { return found } }
        if let glass = view as? NSGlassEffectView, let content = glass.contentView { return find(type, in: content) }
        return nil
    }

    static func renderedInkBounds(_ view: NSView) -> NSRect {
        let width = Int((view.bounds.width * 2).rounded(.up))
        let height = Int((view.bounds.height * 2).rounded(.up))
        let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: width, pixelsHigh: height, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
        bitmap.size = view.bounds.size
        memset(bitmap.bitmapData!, 0, bitmap.bytesPerRow * height)
        view.cacheDisplay(in: view.bounds, to: bitmap)
        var left = width, top = height, right = -1, bottom = -1
        for y in 0..<height {
            for x in 0..<width where bitmap.colorAt(x: x, y: y)!.alphaComponent > 0.1 {
                left = min(left, x); right = max(right, x)
                top = min(top, y); bottom = max(bottom, y)
            }
        }
        precondition(right >= left, "The optical alignment test must capture visible glyphs")
        let sx = view.bounds.width / CGFloat(width), sy = view.bounds.height / CGFloat(height)
        let inkTop = CGFloat(top) * sy, inkBottom = CGFloat(bottom + 1) * sy
        return NSRect(x: view.bounds.minX + CGFloat(left) * sx, y: view.isFlipped ? view.bounds.minY + inkTop : view.bounds.maxY - inkBottom, width: CGFloat(right - left + 1) * sx, height: inkBottom - inkTop)
    }

    static func main() throws {
        if ScanWorker.runIfRequested() { return }
        var fade = FadeTrack()
        fade.retarget(1, at: 0, immediately: false)
        fade.advance(to: 0.09)
        precondition(abs(fade.value - 0.5) < 0.001)
        fade.retarget(0, at: 0.09, immediately: false)
        precondition(abs(fade.value - 0.5) < 0.001, "Reversals must preserve current opacity")
        fade.advance(to: 0.12)
        let partial = fade.value
        fade.retarget(0, at: 0.12, immediately: false)
        fade.advance(to: 0.18)
        precondition(partial > 0 && partial < 0.5 && fade.value == 0 && !fade.isRunning, "Repeated targets must not restart a fade")
        fade.retarget(1, at: 1, immediately: true)
        precondition(fade.value == 1 && !fade.isRunning)

        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        AppearanceMode.system.apply()
        precondition(app.appearance == nil, "System appearance must inherit the current macOS appearance")
        AppearanceMode.light.apply()
        let fixture = FileManager.default.temporaryDirectory.appendingPathComponent("argus-presentation-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: fixture, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: fixture) }
        let index = FileIndex(home: fixture, storage: fixture.appendingPathComponent("index"), applications: [])
        let providers = SystemProviders()
        let coordinator = SearchCoordinator(index: index, providers: providers)
        let controller = PanelController(coordinator: coordinator)
        let publish = coordinator.onResults!
        coordinator.onResults = nil
        defer { controller.hide(immediately: true); providers.cancelSearch(); index.stop() }
        controller.show(query: "Safari")
        let table = find(NSTableView.self, in: controller.panel.contentView!)!
        let scroll = find(NSScrollView.self, in: controller.panel.contentView!)!
        let headerFrame = controller.panel.frame
        let root = controller.panel.contentView!
        let glass = root.subviews.compactMap { $0 as? NSGlassEffectView }
        precondition(glass.count == 1, "One continuous glass surface must own both input and results")
        let symbol = find(NSImageView.self, in: glass[0].contentView!)!
        precondition(controller.field.isDescendant(of: glass[0].contentView!), "Input controls must belong to the native glass content layer")
        precondition(controller.panel.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .aqua)
        precondition(glass[0].style == (NSWorkspace.shared.accessibilityDisplayShouldReduceTransparency ? .regular : .clear))
        let fieldFrame = controller.field.frame
        let fieldBaseline = controller.field.alignmentRect(forFrame: fieldFrame).minY + controller.field.firstBaselineOffsetFromTop
        let textCenter = fieldBaseline - controller.field.font!.capHeight / 2
        precondition(abs(textCenter - symbol.frame.midY) < 0.51 && abs(textCenter - SearchSurface.inputHeight / 2) < 0.51, "The complete symbol must align with the text cap-height center")
        controller.panel.makeFirstResponder(nil)
        let symbolInk = root.convert(renderedInkBounds(symbol), from: symbol)
        for query in ["", "Search", "聚焦搜索"] {
            controller.field.stringValue = query
            let textInk = root.convert(renderedInkBounds(controller.field), from: controller.field)
            precondition(abs(textInk.midY - symbolInk.midY) <= 1 && abs(textInk.midY - SearchSurface.inputHeight / 2) <= 1, "Rendered symbol and English, Chinese or placeholder ink must be visually centered")
        }
        controller.field.stringValue = "Safari"
        controller.panel.makeFirstResponder(controller.field)
        precondition(root.layer?.masksToBounds == true && root.layer?.cornerRadius == GlassContentView.cornerRadius)
        precondition(root.layer?.cornerCurve == .continuous)
        precondition(ResultRow.cornerRadius + SearchSurface.listInset + ResultRow.horizontalInset == GlassContentView.cornerRadius, "Inner selection corners must follow the outer radius and inset")
        let probe = CALayer()
        probe.frame = root.bounds.insetBy(dx: -8, dy: -8)
        probe.backgroundColor = NSColor.red.cgColor
        root.layer!.addSublayer(probe)
        let width = Int(root.bounds.width), height = Int(root.bounds.height)
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        pixels.withUnsafeMutableBytes { buffer in
            let context = CGContext(data: buffer.baseAddress, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            root.layer!.render(in: context)
        }
        probe.removeFromSuperlayer()
        for (x, y) in [(0, 0), (width - 1, 0), (0, height - 1), (width - 1, height - 1)] {
            precondition(pixels[(y * width + x) * 4 + 3] == 0, "Whole-window clipping must remove rectangular corner overflow")
        }
        precondition(pixels[((height / 2) * width + width / 2) * 4 + 3] > 250, "Clipping probe must actually render its opaque center")
        let editor = controller.field.currentEditor()
        func hit(_ id: String) -> SearchResult { SearchResult(id: id, title: id, subtitle: "App", kind: .application, target: "/Applications/Safari.app", score: 1000) }
        publish([], -1)
        publish([], 1)
        precondition(table.numberOfRows == 0)
        publish([hit("Safari")], 2)
        let firstCell = table.view(atColumn: 0, row: 0, makeIfNecessary: true)!
        firstCell.layoutSubtreeIfNeeded()
        let firstRow = table.rowView(atRow: 0, makeIfNecessary: true)!
        firstRow.layoutSubtreeIfNeeded()
        let selectionLayer = firstRow.layer!.sublayers!.first { $0.cornerRadius == ResultRow.cornerRadius }!
        precondition(selectionLayer.cornerCurve == root.layer!.cornerCurve && selectionLayer.frame == firstRow.bounds.insetBy(dx: ResultRow.horizontalInset, dy: 2) && !selectionLayer.isHidden, "Selection must render the same continuous corner curve with its concentric inset")
        for label in firstCell.subviews.compactMap({ $0 as? NSTextField }) {
            let textFrame = root.convert(label.alignmentRect(forFrame: label.frame), from: firstCell)
            let queryFrame = controller.field.alignmentRect(forFrame: controller.field.frame)
            precondition(abs(textFrame.minX - queryFrame.minX) < 0.51, "Result title and subtitle must align with the query text origin")
        }
        publish((0..<8).map { $0 == 0 ? hit("Safari") : hit("App\($0)") }, 3)
        precondition(table.numberOfRows == 8)
        precondition(firstCell === table.view(atColumn: 0, row: 0, makeIfNecessary: true), "Supplemental results must preserve unchanged cell views")
        precondition(controller.panel.frame == headerFrame, "Progressive batches must not resize the window")
        precondition(controller.field.currentEditor() === editor, "Updates must preserve the field editor")
        var scoreOnly = hit("Safari"); scoreOnly.score = 2000
        publish([scoreOnly], 4)
        precondition(firstCell === table.view(atColumn: 0, row: 0, makeIfNecessary: true))

        for mode in [AppearanceMode.dark, .light] {
            mode.apply()
            wait(0.05)
            let expected: NSAppearance.Name = mode == .dark ? .darkAqua : .aqua
            precondition(controller.field.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == expected)
            precondition(firstCell.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == expected)
            precondition(controller.panel.frame == headerFrame && controller.field.frame == fieldFrame, "Appearance changes must preserve panel geometry")
            precondition(controller.field.currentEditor() === editor && firstCell === table.view(atColumn: 0, row: 0, makeIfNecessary: true), "Appearance changes must preserve the editor and result cells")
            let title = firstCell.subviews.compactMap { $0 as? NSTextField }.first!
            let titleBitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(title.bounds.width * 2), pixelsHigh: Int(title.bounds.height * 2), bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
            titleBitmap.size = title.bounds.size
            memset(titleBitmap.bitmapData!, 0, titleBitmap.bytesPerRow * titleBitmap.pixelsHigh)
            title.cacheDisplay(in: title.bounds, to: titleBitmap)
            var ink: [CGFloat] = []
            for y in 0..<titleBitmap.pixelsHigh {
                for x in 0..<titleBitmap.pixelsWide {
                    let pixel = titleBitmap.colorAt(x: x, y: y)!.usingColorSpace(.deviceRGB)!
                    if pixel.alphaComponent > 0.9 { ink.append(pixel.redComponent) }
                }
            }
            precondition(!ink.isEmpty && (mode == .light ? ink.max()! < 0.2 : ink.min()! > 0.8), "Selected result titles must render dark in light mode and light in dark mode")
            let overlay = GlassContentView(frame: NSRect(x: 0, y: 0, width: 100, height: 100))
            overlay.appearance = NSAppearance(named: expected)
            let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 100, pixelsHigh: 100, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
            memset(bitmap.bitmapData!, 0, bitmap.bytesPerRow * 100)
            overlay.cacheDisplay(in: overlay.bounds, to: bitmap)
            let color = bitmap.colorAt(x: 50, y: 50)!.usingColorSpace(.deviceRGB)!
            precondition(color.alphaComponent > 0.35 && color.alphaComponent < 0.51, "Contrast protection must retain a transparent background")
            precondition(mode == .dark ? color.redComponent < 0.01 : color.redComponent > 0.99, "Light mode must use light contrast protection, dark mode dark protection")
        }

        controller.field.stringValue = "missing"
        var searchStarts = 0
        coordinator.onResults = { _, milliseconds in if milliseconds < 0 { searchStarts += 1 } }
        controller.field.sendAction(controller.field.action, to: controller.field.target)
        controller.controlTextDidChange(Notification(name: NSControl.textDidChangeNotification))
        precondition(searchStarts == 1, "Native action and text notifications for the same input must submit only one query")
        coordinator.onResults = nil
        precondition(!controller.tableView(table, shouldSelectRow: 0), "Pending old rows must be non-actionable")
        publish([], 1)
        controller.field.stringValue = "new query"
        controller.controlTextDidChange(Notification(name: NSControl.textDidChangeNotification))
        publish([hit("New")], 2)
        wait(0.25)
        precondition(table.numberOfRows == 1 && !scroll.isHidden, "An obsolete empty-state timer must not erase a newer result")
        precondition(controller.tableView(table, shouldSelectRow: 0))
        precondition(controller.panel.frame == headerFrame)

        if let textView = controller.field.currentEditor() as? NSTextView {
            var pending = 0
            coordinator.onResults = { _, milliseconds in if milliseconds < 0 { pending += 1 } }
            textView.setMarkedText("拼", selectedRange: NSRange(location: 1, length: 0), replacementRange: NSRange(location: NSNotFound, length: 0))
            precondition(textView.hasMarkedText())
            controller.controlTextDidChange(Notification(name: NSControl.textDidChangeNotification))
            precondition(pending == 0, "Uncommitted IME input must not launch a search")
            precondition(!controller.control(controller.field, textView: textView, doCommandBy: #selector(NSResponder.insertNewline(_:))))
            precondition(!controller.control(controller.field, textView: textView, doCommandBy: #selector(NSResponder.cancelOperation(_:))))
            textView.unmarkText()
            coordinator.onResults = nil
        }

        controller.hide()
        wait(0.05)
        let interruptedAlpha = controller.panel.alphaValue
        controller.toggle()
        precondition(controller.isPresented && abs(controller.panel.alphaValue - interruptedAlpha) < 0.12)
        wait(0.25)
        precondition(controller.panel.isVisible && controller.panel.alphaValue == 1)
        precondition(controller.panel.frame == headerFrame && controller.panel.childWindows?.isEmpty != false)
        controller.hide()
        wait(0.25)
        precondition(!controller.panel.isVisible)
        controller.show(query: "Safari")
        wait(0.25)
        precondition(controller.panel.frame == headerFrame, "Reopening must restore the same result geometry")
        controller.field.stringValue = ""
        precondition(controller.field.sendAction(controller.field.action, to: controller.field.target), "Native clear-button actions must reach the search controller")
        wait(0.25)
        precondition(controller.panel.isVisible && controller.panel.frame.height == SearchSurface.inputHeight && controller.panel.frame.maxY == headerFrame.maxY)
        controller.field.stringValue = "Safari"
        controller.controlTextDidChange(Notification(name: NSControl.textDidChangeNotification))
        var heights: [CGFloat] = [controller.panel.frame.height]
        for _ in 0..<16 {
            wait(0.015)
            heights.append(controller.panel.frame.height)
            precondition(controller.panel.frame.maxY == headerFrame.maxY, "Expansion must preserve the top edge")
            precondition(controller.field.frame == fieldFrame, "Expansion must not move or resize the input")
            precondition(root.bounds.size == controller.panel.frame.size && glass[0].frame == root.bounds && glass[0].contentView!.frame.size == root.bounds.size, "Window, glass and clipping must share each intermediate size")
        }
        precondition(heights == heights.sorted() && heights.last == SearchSurface.expandedHeight)
        if !NSWorkspace.shared.accessibilityDisplayShouldReduceMotion {
            precondition(Set(heights.filter { $0 > SearchSurface.inputHeight && $0 < SearchSurface.expandedHeight }).count >= 3, "Expansion must interpolate instead of jumping between endpoints")
        }
        controller.field.stringValue = ""
        controller.controlTextDidChange(Notification(name: NSControl.textDidChangeNotification))
        wait(0.06)
        let partialHeight = controller.panel.frame.height
        controller.field.stringValue = "Safari"
        controller.controlTextDidChange(Notification(name: NSControl.textDidChangeNotification))
        if !NSWorkspace.shared.accessibilityDisplayShouldReduceMotion {
            precondition(controller.panel.frame.height <= partialHeight && controller.panel.frame.height > SearchSurface.inputHeight, "Reversal must continue from the interpolated size without jumping to an endpoint")
        }
        wait(0.25)
        precondition(controller.panel.frame == headerFrame)
        controller.hide(immediately: true)
        publish([hit("Hidden")], 1)
        precondition(!controller.isPresented && !controller.panel.isVisible)
        print("Panel presentation tests passed: light/dark appearance and transparent contrast masks, native glass content hierarchy, rendered English/Chinese/placeholder optical alignment, result text alignment, intermediate geometry and reversal, rounded corner clipping, stable cells/editor, stale rows/timers, IME, native clear action")
    }
}
