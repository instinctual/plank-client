import Foundation

@main struct PlankMacPencilTests {
    static func main() {
        for source in PlankMacTabletSource.allCases {
            // Verify all source combinations, including a preference change
            // made by another window while the current session owns USB HID.
            for saved in PlankMacTabletSource.allCases {
                assert(PlankMacTabletSource.allowsPencil(sessionActive:true,
                    sessionSource:source,savedSource:saved) == (source == .pencil))
                assert(PlankMacTabletSource.allowsPencil(sessionActive:false,
                    sessionSource:source,savedSource:saved) == (saved == .pencil))
            }
        }
        assert(!PlankMacTabletSource.pencil.usesUSB && !PlankMacTabletSource.pencil.usesRegisteredRelay)
        assert(PlankMacTabletSource.usb.usesUSB && PlankMacTabletSource.relay.usesRegisteredRelay)
        let queue = PlankInputQueue()
        queue.append(.pointer(x:10,y:20,maximumX:100,maximumY:100))
        assert(queue.nativeMouseOwnsPointer)
        let hover = PlankNormalizedPen(phase:.hover,x:0.25,y:0.5,pressureOrDistance:0.2,tilt:0,rotation:0)
        assert(queue.offerPencilRelayKey(code:0x20,pressed:true,modifiers:0))
        assert(queue.nativeMouseOwnsPointer, "A shortcut alone cannot select the remote pen cursor")
        assert(queue.offerPencilRelayPen(hover))
        assert(!queue.nativeMouseOwnsPointer, "Accepted Pencil hover takes remote cursor presentation")
        let down = PlankNormalizedPen(phase:.down,x:0.3,y:0.6,pressureOrDistance:0.8,tilt:30,rotation:180)
        assert(queue.offerPencilRelayPen(down))
        assert(queue.offerPencilRelayKey(code:0x20,pressed:false,modifiers:0))
        let events = queue.drain()
        assert(events.count == 5)
        if case let .pen(p) = events[3] { assert(p == down) } else { assertionFailure("Pressure/tilt record changed") }
        queue.append(.pointer(x:20,y:30,maximumX:100,maximumY:100))
        assert(queue.nativeMouseOwnsPointer, "A later real mouse movement returns native cursor ownership")
        queue.append(.pen(.init(phase:.leave,x:0,y:0,pressureOrDistance:0,tilt:0,rotation:0)))
        assert(queue.nativeMouseOwnsPointer, "Retiring a pen cannot steal presentation from an active mouse")
        _ = queue.drain()
        assert(queue.offerPencilRelayRightClick(x:30,y:40,maximumX:100,maximumY:100))
        assert(!queue.nativeMouseOwnsPointer, "Pencil squeeze retains the remote pen cursor")
        let click = queue.drain()
        assert(click.count == 3)
        if case let .pointer(x,y,_,_) = click[0] { assert(x == 30 && y == 40) } else { assertionFailure() }
        if case let .button(number,pressed) = click[1] { assert(number == 3 && pressed) } else { assertionFailure() }
        if case let .button(number,pressed) = click[2] { assert(number == 3 && !pressed) } else { assertionFailure() }
        queue.append(.pointer(x:20,y:30,maximumX:100,maximumY:100)); _ = queue.drain()
        for _ in 0..<256 { assert(queue.offerPencilRelayKey(code:0x10,pressed:true,modifiers:1)) }
        assert(!queue.offerPencilRelayRightClick(x:30,y:40,maximumX:100,maximumY:100))
        assert(queue.diagnostics.depth == 256, "Refused squeeze cannot queue only its button-down edge")
        assert(!queue.offerPencilRelayPen(down))
        assert(queue.nativeMouseOwnsPointer, "Refused Pencil input cannot claim a cursor")
        queue.stop()
        assert(!queue.offerPencilRelayPen(hover) && !queue.offerPencilRelayKey(code:0x10,pressed:false,modifiers:0))
        assert(queue.nativeMouseOwnsPointer && queue.drain().isEmpty, "Old generation cannot replay")
        print("Mac Pencil source exclusivity, immutable session choice, bounded ordering and cursor ownership passed")
    }
}
