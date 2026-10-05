import AppKit
import Quartz

final class ResultPreview: NSObject, QLPreviewPanelDataSource, QLPreviewPanelDelegate {
    private var url: URL?
    private var popover: NSPopover?
    private weak var controlledPanel: QLPreviewPanel?
    var hasItem: Bool { url != nil }
    var isVisible: Bool { popover?.isShown == true || controlledPanel?.isVisible == true }

    func beginControl(_ panel: QLPreviewPanel) {
        controlledPanel = panel
        panel.dataSource = self
        panel.delegate = self
        panel.reloadData()
    }

    func endControl(_ panel: QLPreviewPanel) {
        if panel.dataSource === self { panel.dataSource = nil }
        if panel.delegate === self { panel.delegate = nil }
        if controlledPanel === panel { controlledPanel = nil }
    }

    func show(_ result: SearchResult, relativeTo rect: NSRect, of view: NSView) {
        closePopover()
        if [.file, .folder, .content, .application].contains(result.kind) {
            url = URL(fileURLWithPath: result.target).resolvingSymlinksInPath()
            guard let panel = QLPreviewPanel.shared() else { return }
            if controlledPanel === panel { panel.reloadData() }
            panel.makeKeyAndOrderFront(nil)
            return
        }
        closeQuickLook()
        let controller = NSViewController()
        let label = NSTextField(wrappingLabelWithString: result.snippet.isEmpty ? result.subtitle : result.snippet)
        label.font = .systemFont(ofSize: 14)
        label.frame = NSRect(x: 20, y: 20, width: 420, height: 180)
        controller.view = NSView(frame: NSRect(x: 0, y: 0, width: 460, height: 220))
        controller.view.addSubview(label)
        let popover = NSPopover()
        popover.animates = !NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        popover.contentViewController = controller
        popover.behavior = .transient
        self.popover = popover
        popover.show(relativeTo: rect, of: view, preferredEdge: .maxX)
    }

    func close() {
        closePopover()
        closeQuickLook()
    }

    private func closeQuickLook() {
        url = nil
        guard let panel = controlledPanel else { return }
        panel.orderOut(nil)
        panel.updateController()
    }

    private func closePopover() { popover?.close(); popover = nil }
    func numberOfPreviewItems(in panel: QLPreviewPanel!) -> Int { url == nil ? 0 : 1 }
    func previewPanel(_ panel: QLPreviewPanel!, previewItemAt index: Int) -> QLPreviewItem! { url as NSURL? }
    deinit { close() }
}
