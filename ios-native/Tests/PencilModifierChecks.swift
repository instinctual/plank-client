import Foundation

@main enum PencilModifierChecks {
    static func main() {
        var keys = PlankPencilKeyOwnership()
        typealias Event = PlankPencilKeyOwnership.Event
        precondition(keys.update(code:0x10,pressed:true,modifiers:1,source:.local) == Event(code:0x10,pressed:true,modifiers:1))
        precondition(keys.update(code:0x10,pressed:true,source:.pad) == nil)
        precondition(keys.update(code:0x10,pressed:false,modifiers:0,source:.local) == nil)
        precondition(keys.update(code:0x10,pressed:false,source:.pad) == Event(code:0x10,pressed:false,modifiers:0))
        precondition(keys.update(code:0x20,pressed:true,source:.pad) == Event(code:0x20,pressed:true,modifiers:0))
        precondition(keys.update(code:0x20,pressed:true,source:.local) == nil)
        precondition(keys.retirePad().isEmpty, "Hiding the pad must not release a physical Space")
        precondition(keys.update(code:0x20,pressed:true,source:.local) == Event(code:0x20,pressed:true,modifiers:0), "Preserve physical repeat")
        precondition(keys.update(code:0x20,pressed:false,source:.local) == Event(code:0x20,pressed:false,modifiers:0))
        _ = keys.update(code:0x12,pressed:true,modifiers:4,source:.local)
        precondition(keys.update(code:0x10,pressed:true,source:.pad) == Event(code:0x10,pressed:true,modifiers:5))
        precondition(keys.update(code:0x41,pressed:true,modifiers:4,source:.local) == Event(code:0x41,pressed:true,modifiers:5))
        precondition(keys.retirePad() == [Event(code:0x10,pressed:false,modifiers:4)])
        // Cancellation's stale flag for Alt must not affect later pad events.
        _ = keys.update(code:0x12,pressed:false,modifiers:4,source:.local)
        _ = keys.update(code:0x41,pressed:false,modifiers:4,source:.local)
        precondition(keys.update(code:0x11,pressed:true,source:.pad) == Event(code:0x11,pressed:true,modifiers:2))
        precondition(keys.retirePad() == [Event(code:0x11,pressed:false,modifiers:0)])
        precondition(keys.retirePad().isEmpty)
        // Left/right physical Command keys survive pad Command release.
        _ = keys.update(code:0x5C,pressed:true,modifiers:8,source:.local)
        _ = keys.update(code:0x5B,pressed:true,source:.pad)
        precondition(keys.retirePad() == [Event(code:0x5B,pressed:false,modifiers:8)])
        precondition(keys.update(code:0x5C,pressed:false,modifiers:0,source:.local) == Event(code:0x5C,pressed:false,modifiers:0))
        print("PASS shortcut ownership: both hold orders, last-owner release, physical repeat, merged masks, cancellation and left/right Command")
    }
}
