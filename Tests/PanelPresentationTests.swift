import AppKit

@main
enum PanelPresentationTests {
    static func wait(_ seconds: Double) {
        let deadline = Date(timeIntervalSinceNow: seconds)
        while Date() < deadline { RunLoop.main.run(until: min(deadline, Date(timeIntervalSinceNow: 0.005))) }
    }

    static func check(_ condition: Bool, _ message: @autoclosure () -> String = "", line: Int = #line) {
        guard !condition else { return }
        fputs("Panel presentation test failed at line \(line): \(message())\n", stderr)
        exit(1)
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
        check(right >= left, "The optical alignment test must capture visible glyphs")
        let sx = view.bounds.width / CGFloat(width), sy = view.bounds.height / CGFloat(height)
        let inkTop = CGFloat(top) * sy, inkBottom = CGFloat(bottom + 1) * sy
        return NSRect(x: view.bounds.minX + CGFloat(left) * sx, y: view.isFlipped ? view.bounds.minY + inkTop : view.bounds.maxY - inkBottom, width: CGFloat(right - left + 1) * sx, height: inkBottom - inkTop)
    }

    static func springTests() {
        var spring = Spring(0, precision: 0.001)
        spring.retarget(1, response: 0.3, at: 0)
        check(spring.isRunning)
        var stepped = spring
        for frame in 1...9 { stepped.advance(to: Double(frame) * 0.01) }
        spring.advance(to: 0.09)
        check(abs(spring.value - stepped.value) < 1e-12, "The closed form must not depend on frame cadence")
        check(spring.value > 0 && spring.value < 1 && spring.velocity > 0)
        let reversedAt = spring.value, velocity = spring.velocity
        spring.retarget(0, response: 0.3, at: 0.09)
        check(spring.value == reversedAt && spring.velocity == velocity, "Retargeting must keep both position and velocity")
        spring.advance(to: 0.1)
        check(spring.value > reversedAt, "Momentum must carry briefly through a reversal instead of stopping dead")
        var repeated = spring, reference = spring
        repeated.retarget(0, response: 0.3, at: 0.1)
        repeated.advance(to: 0.2); reference.advance(to: 0.2)
        check(repeated.value == reference.value, "Repeating the current target must not restart the motion")
        spring.advance(to: 3)
        check(spring.value == 0 && spring.velocity == 0 && !spring.isRunning, "Springs must settle exactly on target")
        spring.retarget(1, response: 0.3, at: 3, immediately: true)
        check(spring.value == 1 && !spring.isRunning)
        var rest = Spring(68, precision: 0.2)
        rest.retarget(484, response: PanelMotion.heightResponse, at: 0)
        var last = rest.value
        for frame in 1...60 {
            rest.advance(to: Double(frame) / 60)
            check(rest.value >= last && rest.value <= 484, "A critically damped expansion from rest must never overshoot")
            last = rest.value
        }
        check(!rest.isRunning && rest.value == 484, "Expansion must settle within one second")
    }

    static func completionTests() {
        check(PanelController.completion(of: "saf", with: "Safari") == "ari")
        check(PanelController.completion(of: "SAF", with: "Safari") == "ari")
        check(PanelController.completion(of: "cafe", with: "Café Menu") == " Menu", "Diacritic-insensitive prefixes must complete on the original title")
        check(PanelController.completion(of: "备忘", with: "备忘录") == "录")
        check(PanelController.completion(of: "Safari", with: "Safari") == nil)
        check(PanelController.completion(of: "ari", with: "Safari") == nil)
        check(PanelController.completion(of: "", with: "Safari") == nil)
    }

    static func main() throws {
        if ScanWorker.runIfRequested() { return }
        springTests()
        completionTests()

        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        AppearanceMode.system.apply()
        check(app.appearance == nil, "System appearance must inherit the current macOS appearance")
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
        let window = controller.panel
        let table = find(ResultTable.self, in: window.contentView!)!
        let scroll = find(NSScrollView.self, in: window.contentView!)!
        let windowFrame = window.frame
        let shape = controller.shape
        let inputHeight = SearchSurface.inputHeight
        check(window.contentView!.subviews == [shape], "The window holds exactly one rounded surface")
        check(windowFrame.height >= SearchSurface.height(rows: SearchSurface.rowLimit.lowerBound) && windowFrame.height <= SearchSurface.height(rows: SearchSurface.rowLimit.upperBound))
        check(shape.frame.minY == 0 && shape.frame.height == inputHeight, "A query without results must stay collapsed at the top edge, got \(shape.frame)")
        let glass = shape.subviews.compactMap { $0 as? NSGlassEffectView }
        check(glass.count == 1, "One continuous glass surface must own both input and results")
        let symbol = find(NSImageView.self, in: glass[0].contentView!)!
        check(controller.field.isDescendant(of: glass[0].contentView!), "Input controls must belong to the native glass content layer")
        check(controller.panel.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .aqua)
        check(glass[0].style == .clear, "The surface uses the transparent liquid glass")
        let fieldFrame = controller.field.frame
        let fieldBaseline = controller.field.alignmentRect(forFrame: fieldFrame).minY + controller.field.firstBaselineOffsetFromTop
        let textCenter = fieldBaseline - controller.field.font!.capHeight / 2
        check(abs(textCenter - symbol.frame.midY) < 0.51 && abs(textCenter - inputHeight / 2) < 0.51, "The complete symbol must align with the text cap-height center")
        controller.panel.makeFirstResponder(nil)
        let symbolInk = shape.convert(renderedInkBounds(symbol), from: symbol)
        for query in ["", "Search", "聚焦搜索"] {
            controller.field.stringValue = query
            let textInk = shape.convert(renderedInkBounds(controller.field), from: controller.field)
            check(abs(textInk.midY - symbolInk.midY) <= 1 && abs(textInk.midY - inputHeight / 2) <= 1, "Rendered symbol and English, Chinese or placeholder ink must be visually centered")
        }
        controller.field.stringValue = "Safari"
        controller.panel.makeFirstResponder(controller.field)
        check(shape.layer?.masksToBounds == true && shape.layer?.cornerRadius == GlassContentView.cornerRadius)
        check(shape.layer?.cornerCurve == .continuous)
        check(ResultRow.cornerRadius + SearchSurface.listInset + ResultRow.horizontalInset == GlassContentView.cornerRadius, "Inner selection corners must follow the outer radius and inset")
        let probe = CALayer()
        probe.frame = shape.bounds.insetBy(dx: -8, dy: -8)
        probe.backgroundColor = NSColor.red.cgColor
        shape.layer!.addSublayer(probe)
        let width = Int(shape.bounds.width), height = Int(shape.bounds.height)
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        pixels.withUnsafeMutableBytes { buffer in
            let context = CGContext(data: buffer.baseAddress, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            shape.layer!.render(in: context)
        }
        probe.removeFromSuperlayer()
        for (x, y) in [(0, 0), (width - 1, 0), (0, height - 1), (width - 1, height - 1)] {
            check(pixels[(y * width + x) * 4 + 3] == 0, "Surface clipping must remove rectangular corner overflow")
        }
        check(pixels[((height / 2) * width + width / 2) * 4 + 3] > 250, "Clipping probe must actually render its opaque center")
        let editor = controller.field.currentEditor()
        func hit(_ id: String, score: Double = 1000, kind: SearchKind = .application, target: String = "/Applications/Safari.app") -> SearchResult {
            SearchResult(id: id, title: id, subtitle: "App", kind: kind, target: target, score: score)
        }
        func hits(_ count: Int) -> [SearchResult] { (0..<count).map { $0 == 0 ? hit("Safari") : hit("App\($0)") } }
        publish([], -1)
        publish([], 1)
        check(table.liveRows == 0)
        publish([hit("Safari")], 2)
        let firstCell = table.view(atColumn: 0, row: 0, makeIfNecessary: true)!
        firstCell.layoutSubtreeIfNeeded()
        let firstRow = table.rowView(atRow: 0, makeIfNecessary: true)!
        firstRow.layoutSubtreeIfNeeded()
        let selectionLayer = firstRow.layer!.sublayers!.first { $0.cornerRadius == ResultRow.cornerRadius }!
        check(selectionLayer.cornerCurve == shape.layer!.cornerCurve && selectionLayer.frame == firstRow.bounds.insetBy(dx: ResultRow.horizontalInset, dy: 2) && !selectionLayer.isHidden, "Selection must render the same continuous corner curve with its concentric inset")
        for label in firstCell.subviews.compactMap({ $0 as? NSTextField }) {
            let textFrame = shape.convert(label.alignmentRect(forFrame: label.frame), from: firstCell)
            let queryFrame = controller.field.alignmentRect(forFrame: controller.field.frame)
            check(abs(textFrame.minX - queryFrame.minX) < 0.51, "Result title and subtitle must align with the query text origin")
        }
        publish(hits(8), 3)
        check(table.liveRows == 8)
        check(firstCell === table.view(atColumn: 0, row: 0, makeIfNecessary: true), "Supplemental results must preserve unchanged cell views")
        check(window.frame == windowFrame, "Progressive batches must never resize the window")
        check(controller.field.currentEditor() === editor, "Updates must preserve the field editor")
        let tailRow = table.rowView(atRow: 3, makeIfNecessary: true)
        var scoreOnly = hit("Safari"); scoreOnly.score = 2000
        publish([scoreOnly], 4)
        check(firstCell === table.view(atColumn: 0, row: 0, makeIfNecessary: true))
        check(tailRow != nil && tailRow === table.rowView(atRow: 3, makeIfNecessary: false), "Fewer results must reuse row views in place instead of removing them")
        check(table.view(atColumn: 0, row: 3, makeIfNecessary: false)?.alphaValue == 0, "Rows past the results are invisible placeholders")
        check(!controller.tableView(table, shouldSelectRow: 3) && table.accessibilityRowCount() == 1, "Placeholders are neither selectable nor exposed to accessibility")

        for mode in [AppearanceMode.dark, .light] {
            mode.apply()
            let expected: NSAppearance.Name = mode == .dark ? .darkAqua : .aqua
            let appearanceDeadline = Date(timeIntervalSinceNow: 0.4)
            while controller.field.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) != expected, Date() < appearanceDeadline { wait(0.01) }
            let fieldAppearance = controller.field.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua])
            check(fieldAppearance == expected, "Field appearance \(String(describing: fieldAppearance)) must follow \(expected.rawValue)")
            let visibleCell = table.view(atColumn: 0, row: 0, makeIfNecessary: true)!
            check(visibleCell.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == expected)
            check(window.frame == windowFrame && controller.field.frame == fieldFrame, "Appearance changes must preserve panel geometry")
            check(controller.field.currentEditor() === editor, "Appearance changes must preserve the field editor")
            check((visibleCell as? ResultCell)?.accessibilityLabel()?.hasPrefix("Safari") == true, "Appearance changes must keep the same result")
            let title = visibleCell.subviews.compactMap { $0 as? NSTextField }.first!
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
            check(!ink.isEmpty && (mode == .light ? ink.max()! < 0.2 : ink.min()! > 0.8), "Selected result titles must render dark in light mode and light in dark mode")
            let overlay = GlassContentView(frame: NSRect(x: 0, y: 0, width: 100, height: 100))
            overlay.appearance = NSAppearance(named: expected)
            let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 100, pixelsHigh: 100, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
            memset(bitmap.bitmapData!, 0, bitmap.bytesPerRow * 100)
            overlay.cacheDisplay(in: overlay.bounds, to: bitmap)
            let color = bitmap.colorAt(x: 50, y: 50)!.usingColorSpace(.deviceRGB)!
            check(color.alphaComponent > 0.35 && color.alphaComponent < 0.51, "Contrast protection must retain a transparent background")
            check(mode == .dark ? color.redComponent < 0.01 : color.redComponent > 0.99, "Light mode must use light contrast protection, dark mode dark protection")
        }

        // Late batches of one query keep the visible top hit unless a newcomer is a full tier better.
        controller.field.stringValue = "stable"
        controller.controlTextDidChange(Notification(name: NSControl.textDidChangeNotification))
        publish([hit("Top", score: 850), hit("Second", score: 650)], 1)
        publish([hit("Late", score: 900), hit("Top", score: 850), hit("Second", score: 650)], 2)
        check(table.liveRows == 3 && table.selectedRow == 0 && (table.view(atColumn: 0, row: 0, makeIfNecessary: true) as! ResultCell).accessibilityLabel()?.hasPrefix("Top") == true, "A same-tier late result must not displace the visible top hit")
        table.onHover?(1)
        check(table.selectedRow == 1, "Pointer movement over a row selects it")
        publish([hit("Web", score: 1200, kind: .web), hit("Late", score: 900), hit("Top", score: 850), hit("Second", score: 650)], 3)
        check((table.view(atColumn: 0, row: 0, makeIfNecessary: true) as! ResultCell).accessibilityLabel()?.hasPrefix("Web") == true, "A clearly better late result takes the top")
        check((table.view(atColumn: 0, row: table.selectedRow, makeIfNecessary: true) as! ResultCell).accessibilityLabel()?.hasPrefix("Late") == true, "A row chosen by the pointer stays chosen across batches")

        controller.field.stringValue = "missing"
        var searchStarts = 0
        coordinator.onResults = { _, milliseconds in if milliseconds < 0 { searchStarts += 1 } }
        controller.field.sendAction(controller.field.action, to: controller.field.target)
        controller.controlTextDidChange(Notification(name: NSControl.textDidChangeNotification))
        check(searchStarts == 1, "Native action and text notifications for the same input must submit only one query")
        coordinator.onResults = nil
        check(!controller.tableView(table, shouldSelectRow: 0), "Pending old rows must be non-actionable")
        publish([], 1)
        controller.field.stringValue = "new query"
        controller.controlTextDidChange(Notification(name: NSControl.textDidChangeNotification))
        publish([hit("New")], 2)
        wait(0.25)
        check(table.liveRows == 1 && !scroll.isHidden, "An obsolete empty-state timer must not erase a newer result")
        check(controller.tableView(table, shouldSelectRow: 0))
        check(window.frame == windowFrame)

        guard let textView = controller.field.currentEditor() as? NSTextView else { check(false, "The search field must own a field editor"); return }
        var pending = 0
        coordinator.onResults = { _, milliseconds in if milliseconds < 0 { pending += 1 } }
        textView.setMarkedText("拼", selectedRange: NSRange(location: 1, length: 0), replacementRange: NSRange(location: NSNotFound, length: 0))
        check(textView.hasMarkedText())
        controller.controlTextDidChange(Notification(name: NSControl.textDidChangeNotification))
        check(pending == 0, "Uncommitted IME input must not launch a search")
        check(!controller.control(controller.field, textView: textView, doCommandBy: #selector(NSResponder.insertNewline(_:))))
        check(!controller.control(controller.field, textView: textView, doCommandBy: #selector(NSResponder.cancelOperation(_:))))
        textView.unmarkText()
        coordinator.onResults = nil

        controller.hide()
        wait(0.05)
        let interruptedAlpha = controller.panel.alphaValue
        controller.toggle()
        check(controller.isPresented && abs(controller.panel.alphaValue - interruptedAlpha) < 0.12, "Reopening mid-fade continues from the current opacity")
        wait(0.5)
        check(controller.panel.isVisible && controller.panel.alphaValue == 1)
        check(window.frame == windowFrame && controller.panel.childWindows?.isEmpty != false)
        check(CATransform3DIsIdentity(window.contentView!.layer!.sublayerTransform))
        controller.hide()
        wait(0.4)
        check(!controller.panel.isVisible && shape.frame.height == inputHeight, "A hidden panel resets to the collapsed surface")

        // Height follows the result count, animates continuously and never moves the window or the input.
        controller.show(query: "Safari")
        wait(0.3)
        check(shape.frame.height == inputHeight, "A pending query must not expand into blank rows")
        func sample(until expected: CGFloat) -> [CGFloat] {
            var heights = [shape.frame.height]
            for _ in 0..<60 {
                wait(1.0 / 60)
                heights.append(shape.frame.height)
                check(window.frame == windowFrame, "Expansion must not resize or move the window")
                check(shape.frame.minY == 0 && controller.field.frame == fieldFrame, "Expansion must keep the top edge and the input fixed")
                check(glass[0].frame == shape.bounds && glass[0].contentView!.frame.size == shape.bounds.size, "Glass and clipping must share each intermediate size")
                if heights.last == expected && heights.count > 3 { break }
            }
            return heights
        }
        let reduceMotion = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        publish(hits(3), 1)
        let three = SearchSurface.height(rows: 3)
        var heights = sample(until: three)
        check(heights == heights.sorted() && heights.last == three, "Expansion must grow monotonically to three rows: \(heights)")
        if !reduceMotion { check(Set(heights.filter { $0 > inputHeight && $0 < three }).count >= 3, "Expansion must interpolate instead of jumping: \(heights)") }
        publish(hits(1), 2)
        let one = SearchSurface.height(rows: 1)
        heights = sample(until: one)
        check(heights == heights.sorted(by: >) && heights.last == one, "Fewer results must shrink the surface: \(heights)")
        publish(hits(40), 3)
        heights = sample(until: windowFrame.height)
        check(heights.last == windowFrame.height, "Many results fill the surface up to the window: \(heights)")
        if !reduceMotion {
            publish(hits(1), 4)
            wait(0.06)
            let before = shape.frame.height
            publish(hits(5), 5)
            check(abs(shape.frame.height - before) <= 1, "Retargeting must continue from the current height without jumping")
        }
        publish(hits(2), 6)
        _ = sample(until: SearchSurface.height(rows: 2))

        // Escape clears the query first and only closes an empty panel.
        check(controller.control(controller.field, textView: textView, doCommandBy: #selector(NSResponder.cancelOperation(_:))))
        check(controller.isPresented && controller.field.stringValue.isEmpty && controller.typedQuery.isEmpty, "The first Escape must clear the query")
        _ = sample(until: inputHeight)
        check(shape.frame.height == inputHeight, "Clearing the query collapses the surface")
        check(controller.control(controller.field, textView: textView, doCommandBy: #selector(NSResponder.cancelOperation(_:))))
        check(!controller.isPresented, "The second Escape closes the panel")
        wait(0.4)

        // Inline completion follows the selected top hit and never fights the user's edits.
        controller.show(query: "")
        guard let typing = controller.field.currentEditor() as? NSTextView else { check(false, "Reopened field must own a field editor"); return }
        let none = NSRange(location: NSNotFound, length: 0)
        typing.insertText("saf", replacementRange: none)
        check(controller.typedQuery == "saf")
        publish([hit("Safari"), hit("Safar Tool")], 1)
        check(controller.field.stringValue == "safari" && typing.selectedRange == NSRange(location: 3, length: 3) && controller.completion == "ari", "The top hit completes inline with the remainder selected, got \(controller.field.stringValue) \(typing.selectedRange)")
        typing.insertText("a", replacementRange: none)
        check(controller.typedQuery == "safa" && controller.field.stringValue == "safari" && typing.selectedRange == NSRange(location: 4, length: 2), "Typing into a completion keeps it without waiting for results")
        typing.deleteBackward(nil)
        check(controller.field.stringValue == "safa" && controller.completion == nil, "Delete removes only the completion")
        publish([hit("Safari")], 2)
        check(controller.field.stringValue == "safa", "A deleted completion must not come back for the same input")
        typing.insertText("r", replacementRange: none)
        check(controller.completion == "i")
        publish([hit("Safari"), hit("Safar Tool")], 1)
        check(controller.control(controller.field, textView: typing, doCommandBy: #selector(NSResponder.moveDown(_:))))
        check(controller.field.stringValue == "safar" && controller.completion == nil, "Leaving the top hit removes its completion")
        check(controller.control(controller.field, textView: typing, doCommandBy: #selector(NSResponder.moveUp(_:))))
        check(controller.field.stringValue == "safari" && controller.completion == "i", "Returning to the top hit restores its completion")
        check(controller.control(controller.field, textView: typing, doCommandBy: #selector(NSResponder.moveRight(_:))))
        check(controller.field.stringValue == "safari" && controller.typedQuery == "safari" && controller.completion == nil && typing.selectedRange == NSRange(location: 6, length: 0), "Right arrow accepts the completion")
        typing.setMarkedText("pin", selectedRange: NSRange(location: 3, length: 0), replacementRange: none)
        publish([hit("safaripin yin")], 2)
        check(typing.hasMarkedText() && controller.completion == nil, "Composition must never receive a completion")
        typing.unmarkText()
        controller.field.stringValue = "saf"
        controller.controlTextDidChange(Notification(name: NSControl.textDidChangeNotification))
        publish([hit("Safari")], 1)
        check(window.contentView!.hitTest(NSPoint(x: 40, y: 20)) != nil, "Clicks on the rounded surface must reach the field")
        check(window.contentView!.hitTest(NSPoint(x: 40, y: shape.frame.maxY + 16)) == nil, "Clicks below the surface must pass through the spare window")
        controller.hide(immediately: true)
        controller.show()
        check(table.liveRows == 1, "Reopening the same query must keep the last results")
        guard let reopened = controller.field.currentEditor() as? NSTextView, reopened.selectedRange.length > 0 else { check(false, "Reopening must select the previous query"); return }
        reopened.insertText("s", replacementRange: none)
        check(controller.typedQuery == "s" && controller.completion == "afari" && controller.field.stringValue == "safari", "Typing over the reopened selection completes even when it is a prefix of the old query")

        // Return before the first batch opens the top hit as soon as it arrives.
        controller.show(query: "deferred")
        check(controller.control(controller.field, textView: typing, doCommandBy: #selector(NSResponder.insertNewline(_:))))
        check(controller.isPresented, "Return before any result must wait instead of opening stale rows")
        publish([hit("Deferred", kind: .action, target: "argus-action:unsupported-test")], 1)
        check(!controller.isPresented && !window.isVisible, "The deferred Return opens the first result and hides immediately")
        wait(0.1)
        controller.hide(immediately: true)
        publish([hit("Hidden")], 1)
        check(!controller.isPresented && !controller.panel.isVisible)
        print("Panel presentation tests passed: closed-form springs with momentum, fixed window with surface-only expansion, result-count height, stable late batches, hover selection, two-step Escape, inline completion, deferred Return, light/dark contrast, optical alignment, corner clipping, stable cells/editor, stale rows/timers, IME")
    }
}
