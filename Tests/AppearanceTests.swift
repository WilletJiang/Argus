import AppKit

enum AppearanceTests {
    static func run() {
        let check = PanelPresentationTests.check
        func image(_ color: NSColor, text: Bool = false) -> CGImage {
            let context = CGContext(data: nil, width: 96, height: 16, bitsPerComponent: 8, bytesPerRow: 384,
                                    space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            context.setFillColor(color.cgColor)
            context.fill(CGRect(x: 0, y: 0, width: 96, height: 16))
            if text {
                context.setFillColor(NSColor.black.cgColor)
                for x in stride(from: 0, to: 96, by: 8) { context.fill(CGRect(x: x, y: 4, width: 2, height: 8)) }
            }
            return context.makeImage()!
        }
        for value in [0.0, 0.2, 0.8, 1.0] {
            let measured = BackdropAppearance.brightness(of: image(NSColor(srgbRed: value, green: value, blue: value, alpha: 1)))!
            check(abs(measured - value) < 0.02, "Sample must preserve sRGB brightness", #line)
        }
        check(BackdropAppearance.brightness(of: image(.white, text: true))! > 0.9, "Dark text on a white page must not select dark appearance", #line)
        check(BackdropAppearance.brightness(of: image(.clear)) == nil, "Unavailable pixels must not be treated as a dark backdrop", #line)

        let band = CGRect(x: 120, y: 700, width: 620, height: 44)
        check(BackdropAppearance.captureRect(screenRect: band, desktopTop: 900) == CGRect(x: 120, y: 156, width: 620, height: 44), "Capture uses global top-left screen coordinates", #line)
        check(BackdropAppearance.captureRect(screenRect: band.offsetBy(dx: -1440, dy: 900), desktopTop: 900) == CGRect(x: -1320, y: -744, width: 620, height: 44), "Left and upper displays retain their global offsets", #line)

        let panel = NSPanel(contentRect: CGRect(x: 100, y: 100, width: 620, height: 68), styleMask: .borderless, backing: .buffered, defer: false)
        panel.isReleasedWhenClosed = false
        let shape = NSView(frame: panel.contentView!.bounds)
        panel.contentView!.addSubview(shape)
        var callbacks: [(CGImage?, Error?) -> Void] = []
        let backdrop = BackdropAppearance(panel: panel, shape: shape, capture: { _, callback in callbacks.append(callback) }, canCapture: { true }, timeout: 0.05)
        let savedMode = Preferences.shared.appearanceMode
        Preferences.shared.appearanceMode = .adaptive
        var completions = 0
        backdrop.prepare { completions += 1 }
        callbacks[0](image(.white), nil)
        PanelPresentationTests.wait(0.01)
        check(completions == 1 && panel.appearance?.name == .aqua, "A white opening sample must apply light appearance before completion", #line)
        backdrop.prepare { completions += 1 }
        backdrop.cancel()
        callbacks[1](image(.black), nil)
        PanelPresentationTests.wait(0.01)
        check(completions == 1 && panel.appearance?.name == .aqua, "A cancelled or obsolete sample must not change appearance or reopen search", #line)
        backdrop.prepare { completions += 1 }
        PanelPresentationTests.wait(0.08)
        check(completions == 2, "A capture timeout must release presentation", #line)
        callbacks[2](image(.black), nil)
        PanelPresentationTests.wait(0.01)
        check(completions == 2 && panel.appearance?.name == .aqua, "A late image after timeout must be ignored", #line)
        backdrop.prepare { completions += 1 }
        callbacks[3](image(.black), nil)
        PanelPresentationTests.wait(0.01)
        check(completions == 3 && panel.appearance?.name == .darkAqua, "Reopening must take a fresh sample", #line)
        for (value, expected) in [(0.45, NSAppearance.Name.darkAqua), (0.55, .aqua)] {
            backdrop.prepare { completions += 1 }
            callbacks.last!(image(NSColor(srgbRed: value, green: value, blue: value, alpha: 1)), nil)
            PanelPresentationTests.wait(0.01)
            check(panel.appearance?.name == expected, "Each opening must classify the current background without history bias", #line)
        }
        backdrop.invalidate()
        check(panel.appearance == nil, "Leaving a Space must clear cached appearance", #line)
        Preferences.shared.appearanceMode = savedMode

        let caret = SearchCaretView(frame: NSRect(x: 0, y: 0, width: 4, height: 40))
        for mode in [NSAppearance.Name.aqua, .darkAqua] {
            caret.appearance = NSAppearance(named: mode)
            let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 8, pixelsHigh: 80, bitsPerSample: 8,
                                          samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                                          bytesPerRow: 32, bitsPerPixel: 32)!
            memset(bitmap.bitmapData!, 0, bitmap.bytesPerRow * bitmap.pixelsHigh)
            NSGraphicsContext.saveGraphicsState()
            NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
            NSGraphicsContext.current!.cgContext.scaleBy(x: 2, y: 2)
            caret.draw(caret.bounds)
            NSGraphicsContext.restoreGraphicsState()
            let stem = bitmap.colorAt(x: 4, y: 40)!.usingColorSpace(.deviceRGB)!
            let outside = bitmap.colorAt(x: 0, y: 40)!
            check(stem.alphaComponent > 0.9 && (mode == .aqua ? stem.redComponent < 0.1 : stem.redComponent > 0.9), "Caret must be black on light and white on dark", #line)
            check(outside.alphaComponent == 0, "Caret must not draw a halo or shadow", #line)
        }
        print("Appearance tests passed: backdrop brightness, text resistance, snapshot cancellation/timeout, multiple displays, black/white caret without a halo")
    }
}
