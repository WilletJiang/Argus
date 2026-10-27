import AppKit

/// Nonactivating panels can remain key after an outside click, so focus notifications alone are insufficient.
final class PanelClickMonitor {
    private weak var panel: NSWindow?
    private weak var shape: NSView?
    private let keepsOpen: (NSWindow) -> Bool
    private let dismiss: () -> Void
    private var local: Any?
    private var global: Any?

    init(panel: NSWindow, shape: NSView, keepsOpen: @escaping (NSWindow) -> Bool, dismiss: @escaping () -> Void) {
        self.panel = panel
        self.shape = shape
        self.keepsOpen = keepsOpen
        self.dismiss = dismiss
    }

    func start() {
        guard local == nil else { return }
        let clicks: NSEvent.EventTypeMask = [.leftMouseDown, .rightMouseDown, .otherMouseDown]
        local = NSEvent.addLocalMonitorForEvents(matching: clicks) { [weak self] event in
            self?.handleLocalClick(event)
            return event
        }
        global = NSEvent.addGlobalMonitorForEvents(matching: clicks) { [weak self] _ in self?.dismiss() }
    }

    func handleLocalClick(_ event: NSEvent) {
        guard let panel, panel.isVisible, let window = event.window else { return }
        if window === panel, let shape {
            let point = shape.convert(event.locationInWindow, from: nil)
            let outline = NSBezierPath(roundedRect: shape.bounds, xRadius: SearchSurface.cornerRadius, yRadius: SearchSurface.cornerRadius)
            if outline.contains(point) { return }
        } else if window.parent === panel || keepsOpen(window) { return }
        dismiss()
    }

    func stop() {
        if let local { NSEvent.removeMonitor(local); self.local = nil }
        if let global { NSEvent.removeMonitor(global); self.global = nil }
    }

    deinit { stop() }
}
