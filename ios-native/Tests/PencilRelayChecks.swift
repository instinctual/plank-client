import Foundation
@main struct PencilRelayChecks {
    static func main() throws {
        let hover = PlankNormalizedPen(phase:.hover,x:0.5,y:1,pressureOrDistance:0.25,tilt:45,rotation:359)
        var down = hover; down.phase = .down; down.pressureOrDistance = 0.9
        var move = down; move.phase = .move; move.x = 0.8
        var up = move; up.phase = .up; up.pressureOrDistance = 0
        for message: PlankPencilMessage in [.pen(hover),.pen(down),.pen(move),.pen(up),.configuration(width:5120,height:2880,active:true),.rightClick(x:1,y:0),.ping,.pong,.end] {
            let decoded = try PlankPencilMessage.decode(message.encoded()); assert(decoded == message)
            var wrong = try message.encoded(); wrong[4] = 1
            rejects { _ = try PlankPencilMessage.decode(wrong) }
            rejects { _ = try PlankPencilMessage.decode(message.encoded() + Data([0])) }
        }
        for invalid in [Float.nan,Float.infinity,-0.01,1.01] {
            var p = down; p.x = invalid; rejects { _ = try PlankPencilMessage.pen(p).encoded() }
        }
        var p = up; p.pressureOrDistance = 1; rejects { _ = try PlankPencilMessage.pen(p).encoded() }
        for key in PlankPencilModifier.allCases {
            for pressed in [true,false] {
                let message = PlankPencilMessage.modifier(key,pressed:pressed)
                let decoded = try PlankPencilMessage.decode(message.encoded()); assert(decoded == message)
            }
        }
        rejects { _ = try PlankPencilMessage.decode(Data([0x50,0x4c,0x50,0x4e,3,7,0x41,0,1])) }
        rejects { _ = try PlankPencilMessage.decode(Data([0x50,0x4c,0x50,0x4e,3,7,0x10,0,2])) }
        // Generic key chords are one ordered frame; never fall back to only
        // their supported modifiers when a trigger or count is invalid.
        let chord: [PlankPencilKeyEvent] = [.init(code:0x11,pressed:true,modifiers:2),.init(code:0x5A,pressed:true,modifiers:2),
            .init(code:0x5A,pressed:false,modifiers:2),.init(code:0x11,pressed:false,modifiers:0)]
        let decodedChord = try PlankPencilMessage.decode(PlankPencilMessage.keys(chord).encoded()); assert(decodedChord == .keys(chord))
        rejects { _ = try PlankPencilMessage.keys([]).encoded() }
        rejects { _ = try PlankPencilMessage.keys([.init(code:0xFFFF,pressed:true)]).encoded() }
        rejects { _ = try PlankPencilMessage.keys([.init(code:0x41,pressed:true,modifiers:16)]).encoded() }
        rejects { _ = try PlankPencilMessage.keys(Array(repeating:chord[0],count:49)).encoded() }
        var malformed = try PlankPencilMessage.keys(chord).encoded(); malformed[9] = 2
        rejects { _ = try PlankPencilMessage.decode(malformed) }
        var generic = PlankPencilKeyState()
        try generic.accept(chord); assert(generic.held.isEmpty)
        try generic.accept([.init(code:0x41,pressed:true)])
        rejects { try generic.accept([.init(code:0x42,pressed:true),.init(code:0x41,pressed:true)]) }
        assert(generic.held == [0x41], "A rejected tail must not commit the earlier key")
        rejects { try generic.accept([.init(code:0x41,pressed:false),.init(code:0x42,pressed:false)]) }
        assert(generic.held == [0x41], "A rejected release tail must not commit any release")
        assert(generic.retire() == [.init(code:0x41,pressed:false)] && generic.held.isEmpty)
        try generic.accept([.init(code:0x11,pressed:true,modifiers:2),.init(code:0x10,pressed:true,modifiers:3),.init(code:0x5A,pressed:true,modifiers:3)])
        let cancelled = generic.retire()
        assert(cancelled.first == .init(code:0x5A,pressed:false,modifiers:3))
        assert(cancelled.last?.modifiers == 0 && generic.held.isEmpty)
        let maximumBatch = PlankControlKeyCatalog.entries.prefix(48).map { PlankPencilKeyEvent(code:$0.code,pressed:true) }
        let maximumMessage = PlankPencilMessage.keys(maximumBatch)
        let maximumDecoded = try PlankPencilMessage.decode(maximumMessage.encoded())
        let maximumSize = try maximumMessage.encoded().count
        assert(maximumDecoded == maximumMessage && maximumSize == 199)
        let keyMailbox = PlankPencilMailbox(limit:1)
        assert(keyMailbox.offer(.keys(chord)).accepted)
        assert(!keyMailbox.offer(.keys(chord)).accepted)
        rejects { _ = try keyMailbox.take() }
        let mixed = PlankPencilMailbox()
        for value: PlankPencilMessage in [.modifier(.shift,pressed:true),.pen(down),.pen(move),.modifier(.control,pressed:true),.pen(move),.pen(up),.modifier(.shift,pressed:false),.modifier(.control,pressed:false)] {
            assert(mixed.offer(value).accepted)
        }
        var mixedValues: [PlankPencilMessage] = []
        while let value = try mixed.take() { mixedValues.append(value) }
        assert(mixedValues.count == 8, "Motion coalescing must not cross a modifier edge")
        var modifiers = PlankPencilModifierState()
        rejects { try modifiers.accept(.shift,pressed:false) }
        try modifiers.accept(.shift,pressed:true)
        rejects { try modifiers.accept(.shift,pressed:true) }
        assert(modifiers.retire() == [.shift] && modifiers.retire().isEmpty)
        let mailbox = PlankPencilMailbox(limit:4)
        assert(mailbox.offer(.pen(hover)).wake)
        assert(!mailbox.offer(.pen(hover)).wake)
        assert(mailbox.offer(.pen(down)).accepted)
        for _ in 0..<10_000 { assert(mailbox.offer(.pen(move)).accepted) }
        assert(mailbox.offer(.pen(up)).accepted)
        var result: [PlankPencilMessage] = []
        while let next = try mailbox.take() { result.append(next) }
        assert(result == [.pen(hover),.pen(down),.pen(move),.pen(up)])
        let overflow = PlankPencilMailbox(limit:2)
        assert(overflow.offer(.pen(down)).accepted && overflow.offer(.pen(up)).accepted)
        assert(!overflow.offer(.pen(down)).accepted)
        rejects { _ = try overflow.take() } // Fail closed, never silently lose an edge.
        var state = PlankPencilStrokeState()
        rejects { try state.accept(move) }
        try state.accept(down)
        rejects { try state.accept(down) }
        try state.accept(move)
        let terminals = state.retire(); assert(terminals.map(\.phase) == [.cancel,.leave])
        assert(state.retire().isEmpty)
        rejects { try state.accept(move) } // Retired contact requires a fresh down.
        try state.accept(down); try state.accept(up)
        let atomicSender = PlankInputQueue()
        let queueChord = chord.map { PlankControlKeyEvent(code:$0.code,pressed:$0.pressed,modifiers:$0.modifiers) }
        for _ in 0..<255 { atomicSender.append(.key(code:0x41,pressed:true,modifiers:0)) }
        assert(!atomicSender.offerControlKeys(queueChord))
        assert(atomicSender.drain().count == 255, "Rejected chord cannot enqueue a prefix")
        assert(atomicSender.offerControlKeys(queueChord))
        let admitted = atomicSender.drain().compactMap { event -> PlankControlKeyEvent? in
            guard case let .key(code,pressed,modifiers) = event else { return nil }
            return .init(code:code,pressed:pressed,modifiers:modifiers)
        }
        assert(admitted == queueChord)
        let completeKeyboard = PlankControlKeyCatalog.entries.map { PlankControlKeyEvent(code:$0.code,pressed:false) }
        assert(completeKeyboard.count > PlankPencilProtocol.maximumKeyEdges)
        assert(atomicSender.offerControlKeys(completeKeyboard), "Direct cleanup may retire more keys than a sharing frame")
        assert(atomicSender.drain().count == completeKeyboard.count)
        for _ in 0..<257 { atomicSender.append(.key(code:0x41,pressed:true,modifiers:0)) }
        assert(atomicSender.offerControlKeys([]), "An ownership-only transaction requires no queue capacity")
        assert(!atomicSender.offerControlKeys([.init(code:0x41,pressed:false)]))
        assert(atomicSender.drain().count == 257)
        atomicSender.stop(); assert(!atomicSender.offerControlKeys(queueChord) && !atomicSender.offerControlKeys([]))
#if PLANK_PENCIL_RELAY_RECEIVER
        let sender = PlankInputQueue()
        assert(sender.offerPencilRelayPen(down))
        for _ in 0..<10_000 { assert(sender.offerPencilRelayPen(move)) }
        assert(sender.offerPencilRelayPen(up))
        assert(sender.drain().count == 3)
        for _ in 0..<128 {
            assert(sender.offerPencilRelayPen(down)); assert(sender.offerPencilRelayPen(up))
        }
        assert(!sender.offerPencilRelayPen(down))
        assert(!sender.offerPencilRelayKey(code:0x10,pressed:true,modifiers:1))
        sender.stop(); assert(!sender.offerPencilRelayPen(hover))
#endif
        print("Pencil relay codec, malformed data, edge ordering, bounded coalescing and fresh-contact checks passed")
    }
    static func rejects(_ operation: () throws -> Void) {
        do { try operation(); fatalError("Invalid input accepted") } catch {}
    }
}
