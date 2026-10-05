import AppKit
import Carbon

@main
enum HotKeyTests {
    static func main() {
        let pressed = UInt32(kEventHotKeyPressed), released = UInt32(kEventHotKeyReleased)
        var state = HotKeyPressState()
        precondition(state.shouldActivate(for: pressed))
        precondition(!state.shouldActivate(for: pressed))
        precondition(!state.shouldActivate(for: 0))
        precondition(!state.shouldActivate(for: pressed))
        precondition(!state.shouldActivate(for: released))
        precondition(state.shouldActivate(for: pressed))
        precondition(!state.shouldActivate(for: released))
        precondition(!state.shouldActivate(for: released))
        precondition(state.shouldActivate(for: pressed))

        func shortcut(_ flags: UInt32, enabled: Bool = true, code: Int = kVK_Space) -> [String: Any] {
            [kHISymbolicHotKeyCode as String: NSNumber(value: code),
             kHISymbolicHotKeyModifiers as String: NSNumber(value: flags),
             kHISymbolicHotKeyEnabled as String: enabled]
        }
        let fixtures = [shortcut(UInt32(cmdKey)), shortcut(UInt32(cmdKey | shiftKey), enabled: false),
                        shortcut(UInt32(optionKey), code: kVK_Return), [:]]
        precondition(HotKey.isSystemReserved(0, shortcuts: fixtures))
        precondition(!HotKey.isSystemReserved(1, shortcuts: fixtures))
        precondition(!HotKey.isSystemReserved(2, shortcuts: fixtures))
        precondition(HotKey.isSystemReserved(2, shortcuts: [shortcut(UInt32(cmdKey | shiftKey | alphaLock))]))
        precondition(!HotKey.isSystemReserved(0, shortcuts: [shortcut(UInt32(cmdKey | optionKey))]))
        precondition(HotKey.isSystemReserved(-1, shortcuts: []))

        var array: Unmanaged<CFArray>?
        precondition(CopySymbolicHotKeys(&array) == noErr)
        let actual = array!.takeRetainedValue() as! [[String: Any]]
        let key = HotKey()
        if key.register(choice: 0) {
            let original = key.activeChoice!
            precondition(!HotKey.isSystemReserved(original, shortcuts: actual))
            precondition(key.register(choice: original) && key.activeChoice == original)
            precondition(!key.notice.isEmpty && key.description == HotKey.titles[original])
            func event(_ flags: NSEvent.ModifierFlags, code: Int = kVK_Space) -> NSEvent {
                NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: 0,
                    windowNumber: 0, context: nil, characters: "　", charactersIgnoringModifiers: "　",
                    isARepeat: false, keyCode: UInt16(code))!
            }
            precondition(key.matches(event(key.menuModifiers)), "Shortcut matching must use virtual key codes even when the input source emits a full-width space")
            precondition(!key.matches(event([])) && !key.matches(event(key.menuModifiers, code: kVK_Return)))
            precondition(!key.matches(event([.control, .command, .option, .shift])))
            if original == 1, HotKey.isSystemReserved(0, shortcuts: actual), !HotKey.isSystemReserved(2, shortcuts: actual) {
                var blocker: EventHotKeyRef?
                let status = RegisterEventHotKey(UInt32(kVK_Space), UInt32(cmdKey | shiftKey),
                    EventHotKeyID(signature: 0x54455354, id: 1), GetApplicationEventTarget(), OptionBits(kEventHotKeyExclusive), &blocker)
                if status == noErr, let blocker {
                    defer { UnregisterEventHotKey(blocker) }
                    precondition(key.register(choice: 2) && key.activeChoice == original, "A failed replacement must preserve the working binding")
                    print("Occupied replacement preserves the previous binding")
                }
            }
            print("Native hotkey registered without system conflict: \(key.description)")
        } else { print("Native registration check skipped: all supported combinations are occupied") }
        print("Hotkey tests passed: modifier mapping, reserved/disabled/malformed keys, hold suppression, release/repress, native registration/replacement")
    }
}
