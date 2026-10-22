import Foundation

/// The same finite Windows-key vocabulary used by the Apple hardware adapters.
/// Persisted bindings and consented pad input cannot introduce arbitrary codes.
enum PlankControlKeyCatalog {
    struct Entry: Identifiable, Equatable, Sendable {
        let code: UInt16
        let title: String
        var id: UInt16 { code }
    }
    static let entries: [Entry] = {
        let named: [(UInt16, String)] = [
            (0x10,"Shift"),(0x11,"Ctrl"),(0x12,"Option"),(0x5B,"Command"),(0x5C,"Right Command"),
            (0x20,"Space"),(0x1B,"Esc"),(0x09,"Tab"),(0x0D,"Return"),(0x08,"Backspace"),
            (0x14,"Caps Lock"),(0x2C,"Print Screen"),(0x91,"Scroll Lock"),(0x13,"Pause"),
            (0x2D,"Insert"),(0x2E,"Delete"),(0x24,"Home"),(0x23,"End"),
            (0x21,"Page Up"),(0x22,"Page Down"),(0x25,"Left"),(0x26,"Up"),(0x27,"Right"),(0x28,"Down"),
            (0x90,"Num Lock"),(0x6F,"Keypad /"),(0x6A,"Keypad *"),(0x6D,"Keypad −"),
            (0x6B,"Keypad +"),(0x6E,"Keypad ."),(0x5D,"Menu"),
            (0xBD,"−"),(0xBB,"="),(0xDB,"["),(0xDD,"]"),(0xDC,"\\"),
            (0xBA,";"),(0xDE,"'"),(0xC0,"`"),(0xBC,","),(0xBE,"."),(0xBF,"/")
        ]
        return named.map { Entry(code:$0.0,title:$0.1) }
            + (UInt16(0x41)...0x5A).map { Entry(code:$0,title:String(UnicodeScalar(Int($0))!)) }
            + (UInt16(0x30)...0x39).map { Entry(code:$0,title:String(UnicodeScalar(Int($0))!)) }
            + (UInt16(0x70)...0x87).map { Entry(code:$0,title:"F\(Int($0 - 0x70) + 1)") }
            + (UInt16(0x60)...0x69).map { Entry(code:$0,title:"Keypad \(Int($0 - 0x60))") }
    }()
    private static let codes = Set(entries.map(\.code))
    static func supports(_ code: UInt16) -> Bool { codes.contains(code) }
    static func title(for code: UInt16) -> String { entries.first { $0.code == code }?.title ?? "Unsupported key" }
    static func modifierMask(_ code: UInt16) -> UInt8 {
        switch code { case 0x10: 1; case 0x11: 2; case 0x12: 4; case 0x5B,0x5C: 8; default: 0 }
    }
    static let modifiers: [(code: UInt16, mask: UInt8)] = [(0x10,1),(0x11,2),(0x12,4),(0x5B,8)]
}

struct PlankControlBinding: Codable, Hashable, Sendable {
    var code: UInt16
    var modifiers: UInt8
    init(code: UInt16, modifiers: UInt8 = 0) { self.code = code; self.modifiers = modifiers }
    var isValid: Bool { PlankControlKeyCatalog.supports(code) && modifiers & ~UInt8(15) == 0 }
    var title: String {
        let names = PlankControlKeyCatalog.modifiers.filter {
            modifiers & $0.mask != 0 && $0.code != code
        }.map { PlankControlKeyCatalog.title(for:$0.code) }
        return (names + [PlankControlKeyCatalog.title(for:code)]).joined(separator:" + ")
    }
    fileprivate var orderedCodes: [UInt16] {
        PlankControlKeyCatalog.modifiers.filter { modifiers & $0.mask != 0 && $0.code != code }.map(\.code) + [code]
    }
}

enum PlankControlBehavior: String, Codable, CaseIterable, Sendable {
    case hold, tap
    var title: String { self == .hold ? "Hold" : "Tap" }
}

struct PlankControlKeyEvent: Equatable, Sendable {
    let code: UInt16
    let pressed: Bool
    let modifiers: UInt8
    init(code: UInt16, pressed: Bool, modifiers: UInt8 = 0) {
        self.code = code; self.pressed = pressed; self.modifiers = modifiers
    }
}

/// Pure, bounded ownership. Preview never changes state. A caller reserves the
/// complete ordered batch before applying it, so rejection cannot strand half
/// a shortcut. Each contact owns its original binding until it ends.
struct PlankControlKeyOwnership {
    enum Owner: Hashable, Sendable {
        case hardware(UInt16), control(UUID), software(UUID)
        fileprivate var isControl: Bool { if case .control = self { return true }; return false }
        fileprivate var sortKey: String {
            switch self { case .hardware(let code): "0-\(code)"; case .control(let id): "1-\(id)"; case .software(let id): "2-\(id)" }
        }
    }
    typealias Event = PlankControlKeyEvent
    static let maximumOwners = 512
    fileprivate struct State {
        var bindings: [Owner: [UInt16]] = [:]
        var keys: [UInt16: Set<Owner>] = [:]
        var hardwareModifiers: UInt8 = 0
        var modifiers: UInt8 {
            keys.reduce(hardwareModifiers) { $0 | PlankControlKeyCatalog.modifierMask($1.key) }
        }
    }
    struct Change {
        let events: [Event]
        fileprivate let revision: UInt64
        fileprivate let ledgerID: UUID
        fileprivate let next: State
    }
    private var state = State()
    private var revision: UInt64 = 0
    private let ledgerID = UUID()
    var heldCodes: Set<UInt16> { Set(state.keys.keys) }
    var controlOwners: Set<UUID> {
        Set(state.bindings.keys.compactMap { if case .control(let id) = $0 { id } else { nil } })
    }
    var activeModifiers: UInt8 { state.modifiers }

    private func changed(_ operation: (inout State, inout [Event]) -> Bool) -> Change? {
        guard revision != .max else { return nil }
        var next = state, events: [Event] = []
        guard operation(&next,&events) else { return nil }
        return Change(events:events,revision:revision,ledgerID:ledgerID,next:next)
    }
    private static func begin(_ owner: Owner, codes: [UInt16], state: inout State, events: inout [Event]) -> Bool {
        guard state.bindings[owner] == nil, state.bindings.count < maximumOwners else { return false }
        state.bindings[owner] = codes
        for code in codes {
            let wasEmpty = state.keys[code]?.isEmpty != false
            state.keys[code,default:[]].insert(owner)
            if wasEmpty { events.append(Event(code:code,pressed:true,modifiers:state.modifiers)) }
        }
        return true
    }
    private static func end(_ owner: Owner, state: inout State, events: inout [Event]) -> Bool {
        guard let codes = state.bindings.removeValue(forKey:owner) else { return false }
        for code in codes.reversed() {
            state.keys[code]?.remove(owner)
            if state.keys[code]?.isEmpty == true {
                state.keys.removeValue(forKey:code)
                events.append(Event(code:code,pressed:false,modifiers:state.modifiers))
            }
        }
        return true
    }
    func previewBegin(owner: Owner, binding: PlankControlBinding) -> Change? {
        guard binding.isValid else { return nil }
        return changed { Self.begin(owner,codes:binding.orderedCodes,state:&$0,events:&$1) }
    }
    func previewEnd(owner: Owner) -> Change? {
        changed { Self.end(owner,state:&$0,events:&$1) }
    }
    func previewHardware(code: UInt16, pressed: Bool, modifiers: UInt8) -> Change? {
        guard PlankControlKeyCatalog.supports(code), modifiers & ~UInt8(15) == 0 else { return nil }
        return changed { next, events in
            let owner = Owner.hardware(code)
            next.hardwareModifiers = modifiers
            let mask = PlankControlKeyCatalog.modifierMask(code)
            if pressed {
                if mask != 0 { next.hardwareModifiers |= mask }
                if next.bindings[owner] != nil {
                    // Physical repeats are allowed; controls never synthesize them.
                    events.append(Event(code:code,pressed:true,modifiers:next.modifiers))
                    return true
                }
                return Self.begin(owner,codes:[code],state:&next,events:&events)
            }
            guard next.bindings[owner] != nil else { return false }
            if mask != 0, !next.bindings.keys.contains(where: {
                guard case .hardware(let held) = $0 else { return false }
                return held != code && PlankControlKeyCatalog.modifierMask(held) == mask
            }) { next.hardwareModifiers &= ~mask }
            return Self.end(owner,state:&next,events:&events)
        }
    }
    func previewTap(binding: PlankControlBinding) -> Change? {
        guard binding.isValid else { return nil }
        return changed { next, events in
            let owner = Owner.software(UUID())
            guard Self.begin(owner,codes:binding.orderedCodes,state:&next,events:&events) else { return false }
            return Self.end(owner,state:&next,events:&events)
        }
    }
    func previewRetireControls() -> Change? {
        changed { next, events in
            let owners = next.bindings.keys.filter(\.isControl).sorted { $0.sortKey < $1.sortKey }
            for owner in owners { _ = Self.end(owner,state:&next,events:&events) }
            return !owners.isEmpty
        }
    }
    func previewRetireAll() -> Change? {
        changed { next, events in
            guard !next.bindings.isEmpty || next.hardwareModifiers != 0 else { return false }
            next.hardwareModifiers = 0
            // Triggers end before modifiers, including independent hardware owners.
            let codes = next.keys.keys.sorted {
                let lm = PlankControlKeyCatalog.modifierMask($0), rm = PlankControlKeyCatalog.modifierMask($1)
                return lm == rm ? $0 < $1 : (lm == 0 || (rm != 0 && lm > rm))
            }
            for code in codes {
                next.keys.removeValue(forKey:code)
                events.append(Event(code:code,pressed:false,modifiers:next.modifiers))
            }
            next.bindings.removeAll()
            return true
        }
    }
    @discardableResult mutating func apply(_ change: Change) -> Bool {
        guard canApply(change) else { return false }
        state = change.next; revision += 1; return true
    }
    func canApply(_ change: Change) -> Bool {
        change.ledgerID == ledgerID && change.revision == revision && revision != .max
    }
    @discardableResult mutating func begin(owner: Owner, binding: PlankControlBinding, admit: ([Event]) -> Bool) -> Bool {
        guard let change = previewBegin(owner:owner,binding:binding), admit(change.events) else { return false }
        return apply(change)
    }
    @discardableResult mutating func end(owner: Owner, admit: ([Event]) -> Bool) -> Bool {
        guard let change = previewEnd(owner:owner), admit(change.events) else { return false }
        return apply(change)
    }
    @discardableResult mutating func tap(binding: PlankControlBinding, admit: ([Event]) -> Bool) -> Bool {
        guard let change = previewTap(binding:binding), admit(change.events) else { return false }
        return apply(change)
    }
    @discardableResult mutating func retireControls(admit: ([Event]) -> Bool) -> Bool {
        guard let change = previewRetireControls(), admit(change.events) else { return false }
        return apply(change)
    }
    @discardableResult mutating func retireAll(admit: ([Event]) -> Bool) -> Bool {
        guard let change = previewRetireAll(), admit(change.events) else { return false }
        return apply(change)
    }
}

/// A refused edge invalidates the input epoch rather than leaving a ghost
/// owner after the UI discards its touch. The caller closes its connection
/// before local state is cleared. Closing requests do not prove Host receipt.
struct PlankControlKeySession {
    private(set) var ownership = PlankControlKeyOwnership()
    private(set) var accepting = true
    @discardableResult mutating func admit(_ change: PlankControlKeyOwnership.Change,
        offer: ([PlankControlKeyEvent]) -> Bool,
        closeOnRejection: ([PlankControlKeyEvent]) -> Void) -> Bool {
        guard accepting, ownership.canApply(change) else { return false }
        if !change.events.isEmpty, !offer(change.events) {
            let retirement = ownership.previewRetireAll()?.events ?? []
            accepting = false
            closeOnRejection(retirement)
            ownership = .init()
            return false
        }
        return ownership.apply(change)
    }
    mutating func reopen() {
        guard !accepting else { return }
        ownership = .init(); accepting = true
    }
}
