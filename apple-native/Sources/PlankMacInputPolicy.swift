import AppKit

// Window points and Host pixels are different spaces. Letterboxing never
// becomes clickable desktop area, while an existing drag clamps at the edge.
enum PlankMacCoordinates {
    static func canvas(in bounds: CGRect, width: Int, height: Int) -> CGRect {
        guard width > 0, height > 0, bounds.width > 0, bounds.height > 0 else { return .zero }
        let scale = min(bounds.width / Double(width), bounds.height / Double(height))
        let size = CGSize(width: Double(width) * scale, height: Double(height) * scale)
        return CGRect(x: bounds.midX - size.width / 2, y: bounds.midY - size.height / 2, width: size.width, height: size.height)
    }
    static func remote(_ point: CGPoint, canvas: CGRect, width: Int, height: Int, dragging: Bool) -> (Int, Int)? {
        guard canvas.width > 0, canvas.height > 0, width > 1, height > 1,
              dragging || canvas.contains(point) else { return nil }
        return (min(max(Int(((point.x - canvas.minX) / canvas.width * Double(width - 1)).rounded()), 0), width - 1),
                min(max(Int(((point.y - canvas.minY) / canvas.height * Double(height - 1)).rounded()), 0), height - 1))
    }
}
enum PlankMacKeys {
    // macOS physical virtual-key codes -> protocol Windows virtual keys.
    static let table: [UInt16: UInt16] = [
        0:0x41,1:0x53,2:0x44,3:0x46,4:0x48,5:0x47,6:0x5a,7:0x58,8:0x43,9:0x56,11:0x42,
        12:0x51,13:0x57,14:0x45,15:0x52,16:0x59,17:0x54,18:0x31,19:0x32,20:0x33,21:0x34,
        22:0x36,23:0x35,24:0xbb,25:0x39,26:0x37,27:0xbd,28:0x38,29:0x30,30:0xdd,31:0x4f,
        32:0x55,33:0xdb,34:0x49,35:0x50,36:0x0d,37:0x4c,38:0x4a,39:0xde,40:0x4b,41:0xba,
        42:0xdc,43:0xbc,44:0xbf,45:0x4e,46:0x4d,47:0xbe,48:0x09,49:0x20,50:0xc0,51:0x08,
        53:0x1b,65:0x6e,67:0x6a,69:0x6b,71:0x90,75:0x6f,76:0x0d,78:0x6d,82:0x60,
        83:0x61,84:0x62,85:0x63,86:0x64,87:0x65,88:0x66,89:0x67,91:0x68,92:0x69,
        96:0x74,97:0x75,98:0x76,99:0x72,100:0x77,101:0x78,103:0x7a,109:0x79,111:0x7b,
        114:0x2d,115:0x24,116:0x21,117:0x2e,118:0x73,119:0x23,120:0x71,121:0x22,122:0x70,
        123:0x25,124:0x27,125:0x28,126:0x26]
    static func mouseButton(_ macNumber: Int) -> UInt8? { [0: UInt8(1), 1: 3, 2: 2, 3: 4, 4: 5][macNumber] }
    static func modifiers(_ flags: NSEvent.ModifierFlags) -> UInt8 {
        (flags.contains(.shift) ? 1 : 0) | (flags.contains(.control) ? 2 : 0) |
            (flags.contains(.option) ? 4 : 0) | (flags.contains(.command) ? 8 : 0)
    }
    static func staysLocal(_ event: NSEvent) -> Bool {
        event.modifierFlags.contains(.command) &&
            ([12,13,43].contains(event.keyCode) || (event.keyCode == 3 && event.modifierFlags.contains(.control)))
    }
}


// Teardown waits for queued release messages to reach the endpoint before
// stopping its input sender. A terminal transport still gets a bounded exit.
final class PlankMacWacomReleaseBarrier: @unchecked Sendable {
    private let condition = NSCondition()
    private var queued: UInt64 = 0
    private var sent: UInt64 = 0
    func didQueue() { condition.lock(); queued &+= 1; condition.unlock() }
    func didSend() { condition.lock(); sent &+= 1; condition.broadcast(); condition.unlock() }
    func wait(seconds: TimeInterval) -> Bool {
        let end = Date(timeIntervalSinceNow: seconds)
        condition.lock(); defer { condition.unlock() }
        while sent < queued {
            if !condition.wait(until: end) { return sent >= queued }
        }
        return true
    }
}


/// Preserve fractional trackpad motion instead of truncating every event.
/// Legacy wheel notches retain the existing 120-unit protocol scale.
struct PlankMacScrollAccumulator {
    private var verticalRemainder = 0.0
    private var horizontalRemainder = 0.0
    mutating func reset() { verticalRemainder = 0; horizontalRemainder = 0 }
    mutating func take(event: NSEvent) -> (Int16, Int16) {
        if event.phase.contains(.began) { reset() }
        // AppKit has already applied the user's natural scrolling preference.
        // The horizontal sign matches Cocoa_HandleMouseWheel in the Mac client.
        let result = take(vertical: event.deltaY, horizontal: -event.deltaX,
            precise: event.hasPreciseScrollingDeltas)
        if event.phase.contains(.cancelled) || event.momentumPhase.contains(.ended) { reset() }
        return result
    }
    mutating func take(vertical: Double, horizontal: Double, precise: Bool) -> (Int16, Int16) {
        guard vertical.isFinite, horizontal.isFinite else { reset(); return (0, 0) }
        if !precise { reset() }
        // NSEvent.deltaX/Y use the same line-equivalent values as the
        // established Mac client's Cocoa/SDL input path, not scrollingDelta's
        // precise pixel/point values. One protocol detent is 120 units.
        // Conventional mice must produce a tick even for a fractional event;
        // precise devices keep fractions. Match the Mac acceleration cap of
        // one detent per event so accelerated input cannot jump many pages.
        func units(_ delta: Double) -> Double {
            let lines = precise ? delta : delta.rounded(.awayFromZero)
            return min(max(lines, -1), 1) * 120
        }
        return (Self.axis(units(vertical), remainder: &verticalRemainder),
                Self.axis(units(horizontal), remainder: &horizontalRemainder))
    }
    private static func axis(_ delta: Double, remainder: inout Double) -> Int16 {
        let total = delta + remainder
        guard total.isFinite else { remainder = 0; return 0 }
        let whole = total.rounded(.towardZero)
        // Saturation discards excess rather than replaying a huge delayed tail.
        remainder = total - whole
        return Int16(min(max(whole, Double(Int16.min)), Double(Int16.max)))
    }
}

// Cursor visibility follows the pointed-at canvas, not its keyboard focus or
// the output containing the Host cursor. A pen can move on the other output
// while the physical mouse remains parked here.
enum PlankMacPointerPresentation {
    enum Mode { case local, mouse, hidden }
    static func mode(appActive: Bool, onCanvas: Bool, mouseOwns: Bool,
                     mouseArtwork: Bool, currentHostPosition: Bool, overlayOwns: Bool) -> Mode {
        guard appActive, onCanvas else { return .local }
        if mouseOwns { return mouseArtwork ? .mouse : .hidden }
        return currentHostPosition || overlayOwns ? .hidden : .local
    }
}
