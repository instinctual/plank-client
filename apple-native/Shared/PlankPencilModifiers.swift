import Foundation

// Original five-key pad preferences and their legacy migration vocabulary.
// Expanded consented bindings use PlankControlKeyCatalog separately.
enum PlankPencilModifier: UInt16, CaseIterable, Sendable {
    case shift = 0x10, control = 0x11, option = 0x12, command = 0x5B, space = 0x20
    var title: String {
        switch self { case .shift: "Shift"; case .control: "Ctrl"; case .option: "Option"; case .command: "⌘"; case .space: "Space" }
    }
    var accessibilityTitle: String { self == .command ? "Command" : title }
    var mask: UInt8 {
        switch self { case .shift: 1; case .control: 2; case .option: 4; case .command: 8; case .space: 0 }
    }
}
struct PlankPencilModifierState {
    private(set) var held = Set<PlankPencilModifier>()
    mutating func accept(_ key: PlankPencilModifier, pressed: Bool) throws {
        if pressed {
            guard held.insert(key).inserted else { throw PlankPencilWireError.invalid }
        } else {
            guard held.remove(key) != nil else { throw PlankPencilWireError.invalid }
        }
    }
    mutating func retire() -> [PlankPencilModifier] {
        let keys = held.sorted { $0.rawValue < $1.rawValue }; held.removeAll(); return keys
    }
}

// The shortcut pad and local keyboard share the Host's key state. Only the
// last owner releases a key; physical keyboard repeat remains available.
struct PlankPencilKeyOwnership {
    enum Source: Hashable { case local, pad }
    struct Event: Equatable { let code: UInt16; let pressed: Bool; let modifiers: UInt8 }
    private var owners: [UInt16: Set<Source>] = [:]
    private static func mask(_ code: UInt16) -> UInt8 {
        if code == 0x5C { return 8 }
        return PlankPencilModifier(rawValue:code)?.mask ?? 0
    }
    mutating func update(code: UInt16, pressed: Bool, modifiers: UInt8 = 0, source: Source) -> Event? {
        let tracked = PlankControlKeyCatalog.supports(code)
        let previous = owners[code] ?? []
        var next = previous
        if tracked {
            if pressed { next.insert(source) } else { next.remove(source) }
            owners[code] = next.isEmpty ? nil : next
        }
        let localMask = owners.reduce(UInt8(0)) { result, entry in
            result | (entry.value.contains(.local) ? Self.mask(entry.key) : 0)
        }
        // Pad events derive physical modifiers from held key ownership, not
        // a stale flags snapshot from a cancelled key or software character.
        var localModifiers = source == .local ? modifiers & 15 : localMask
        if source == .local, Self.mask(code) != 0 {
            // Cancellation may carry the flags from the original down.
            if pressed { localModifiers |= Self.mask(code) }
            else if !owners.contains(where:{ $0.value.contains(.local) && Self.mask($0.key) == Self.mask(code) }) {
                localModifiers &= ~Self.mask(code)
            }
        }
        let padMask = owners.reduce(UInt8(0)) { result, entry in
            result | (entry.value.contains(.pad) ? Self.mask(entry.key) : 0)
        }
        let event = Event(code:code,pressed:pressed,modifiers:localModifiers | padMask)
        guard tracked else { return event }
        if pressed {
            return previous.isEmpty || (source == .local && previous.contains(.local)) ? event : nil
        }
        return next.isEmpty && (previous.contains(source) || (source == .local && previous.isEmpty)) ? event : nil
    }
    mutating func retirePad() -> [Event] {
        owners.keys.sorted {
            let left = Self.mask($0), right = Self.mask($1)
            return left == right ? $0 < $1 : (left == 0 || (right != 0 && left > right))
        }.filter { owners[$0]?.contains(.pad) == true }.compactMap {
            update(code:$0,pressed:false,source:.pad)
        }
    }
}
