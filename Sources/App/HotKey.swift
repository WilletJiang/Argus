import AppKit
import Carbon

struct HotKeyPressState {
    private var held = false

    mutating func shouldActivate(for kind: UInt32) -> Bool {
        if kind == UInt32(kEventHotKeyReleased) { held = false; return false }
        guard kind == UInt32(kEventHotKeyPressed), !held else { return false }
        held = true
        return true
    }
}

final class HotKey {
    static let titles = ["⌘Space", "⌥Space", "⇧⌘Space"]
    private static let signature: OSType = 0x41524753
    private static let modifiers = [UInt32(cmdKey), UInt32(optionKey), UInt32(cmdKey | shiftKey)]
    private static let menuMasks: [NSEvent.ModifierFlags] = [.command, .option, [.command, .shift]]
    private var reference: EventHotKeyRef?
    private var handler: EventHandlerRef?
    private var registration: UInt32 = 0
    private var pressState = HotKeyPressState()
    private(set) var activeChoice: Int?
    private(set) var description = "Not registered"
    private(set) var notice = ""
    var onPress: (() -> Void)?
    var menuModifiers: NSEvent.ModifierFlags {
        activeChoice.map { Self.menuMasks[$0] } ?? .command
    }

    func matches(_ event: NSEvent) -> Bool {
        guard let activeChoice, event.keyCode == UInt16(kVK_Space) else { return false }
        return event.modifierFlags.intersection([.command, .option, .shift, .control]) == Self.menuMasks[activeChoice]
    }

    init() {
        let specifications = [
            EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed)),
            EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyReleased))
        ]
        let status = specifications.withUnsafeBufferPointer { buffer in
            InstallEventHandler(GetApplicationEventTarget(), { _, event, context in
                guard let event, let context else { return OSStatus(eventNotHandledErr) }
                let key = Unmanaged<HotKey>.fromOpaque(context).takeUnretainedValue()
                var identifier = EventHotKeyID()
                guard GetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID), nil,
                                        MemoryLayout<EventHotKeyID>.size, nil, &identifier) == noErr,
                      identifier.signature == HotKey.signature, identifier.id == key.registration else {
                    return OSStatus(eventNotHandledErr)
                }
                if key.pressState.shouldActivate(for: GetEventKind(event)) {
                    let registration = key.registration
                    DispatchQueue.main.async { [weak key] in
                        guard let key, key.registration == registration else { return }
                        key.onPress?()
                    }
                }
                return noErr
            }, buffer.count, buffer.baseAddress, Unmanaged.passUnretained(self).toOpaque(), &handler)
        }
        if status != noErr { notice = "Global shortcut listener unavailable (\(status)). Open search from the menu bar." }
    }

    @discardableResult func register(choice: Int) -> Bool {
        guard handler != nil else { return false }
        let requested = Self.titles.indices.contains(choice) ? choice : 0
        var array: Unmanaged<CFArray>?
        guard CopySymbolicHotKeys(&array) == noErr, let shortcuts = array?.takeRetainedValue() as? [[String: Any]] else {
            notice = "Could not check system shortcuts. Keeping the current binding."
            return reference != nil
        }
        let candidates = [requested] + Self.titles.indices.filter { $0 != requested }
        for candidate in candidates {
            guard !Self.isSystemReserved(candidate, shortcuts: shortcuts) else { continue }
            if activeChoice != candidate || reference == nil {
                var nextReference: EventHotKeyRef?
                let nextID = registration &+ 1
                let identifier = EventHotKeyID(signature: Self.signature, id: nextID)
                let status = RegisterEventHotKey(UInt32(kVK_Space), Self.modifiers[candidate], identifier,
                                                GetApplicationEventTarget(), OptionBits(kEventHotKeyExclusive), &nextReference)
                guard status == noErr, let nextReference else { continue }
                if let reference { UnregisterEventHotKey(reference) }
                reference = nextReference
                registration = nextID
                activeChoice = candidate
                pressState = HotKeyPressState()
            }
            description = Self.titles[candidate]
            notice = candidate == requested ? "Press \(description) again to hide. Type a new search after opening." :
                "\(Self.titles[requested]) is in use; using \(description). Disable the conflicting system shortcut to select it."
            return true
        }
        notice = reference == nil ? "All shortcuts are unavailable. Open search from the menu bar." : "New shortcut unavailable. Keeping \(description)."
        return reference != nil
    }

    static func isSystemReserved(_ choice: Int, shortcuts: [[String: Any]]) -> Bool {
        guard modifiers.indices.contains(choice) else { return true }
        let relevant = UInt32(cmdKey | optionKey | shiftKey | controlKey)
        return shortcuts.contains { item in
            guard item[kHISymbolicHotKeyEnabled as String] as? Bool == true,
                  (item[kHISymbolicHotKeyCode as String] as? NSNumber)?.uint32Value == UInt32(kVK_Space),
                  let flags = item[kHISymbolicHotKeyModifiers as String] as? NSNumber else { return false }
            return flags.uint32Value & relevant == modifiers[choice]
        }
    }

    deinit {
        if let reference { UnregisterEventHotKey(reference) }
        if let handler { RemoveEventHandler(handler) }
    }
}
