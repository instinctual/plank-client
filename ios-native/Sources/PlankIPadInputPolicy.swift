import Foundation
import CoreGraphics

/// One immutable transform owns presentation and full-source coordinates.
/// `rect` always fits the complete source aspect (or explicitly stretches the
/// full pad); local magnification changes only the visible normalized crop.
/// Black margins reject new contacts; an existing drag clamps to the crop edge.
struct PlankIPadViewport: Equatable {
    static let fullSourceCrop = CGRect(x: 0, y: 0, width: 1, height: 1)
    let rect: CGRect
    let width: Int
    let height: Int
    let visibleSourceCrop: CGRect
    /// Present the whole source in this frame and clip it to `rect`. This same
    /// scale/offset is used by both the CGImage and Metal presentation paths.
    let fullImageFrame: CGRect

    init?(bounds: CGRect, width: Int, height: Int, preserveAspectRatio: Bool = true,
          visibleSourceCrop: CGRect = Self.fullSourceCrop) {
        // Read raw origin/size so a negative CGRect cannot be standardized into
        // valid input. Also reject arithmetic overflow in otherwise finite data.
        guard bounds.origin.x.isFinite, bounds.origin.y.isFinite,
              bounds.size.width.isFinite, bounds.size.height.isFinite,
              bounds.size.width > 0, bounds.size.height > 0,
              bounds.maxX.isFinite, bounds.maxY.isFinite,
              width > 1, height > 1, width <= 65536, height <= 65536,
              visibleSourceCrop.origin.x.isFinite, visibleSourceCrop.origin.y.isFinite,
              visibleSourceCrop.size.width.isFinite, visibleSourceCrop.size.height.isFinite,
              visibleSourceCrop.origin.x >= 0, visibleSourceCrop.origin.y >= 0,
              visibleSourceCrop.size.width > 0, visibleSourceCrop.size.height > 0,
              visibleSourceCrop.maxX <= 1, visibleSourceCrop.maxY <= 1 else { return nil }
        let aspect = CGFloat(width) / CGFloat(height)
        let fitWidth: CGFloat, fitHeight: CGFloat
        if !preserveAspectRatio {
            fitWidth = bounds.width; fitHeight = bounds.height
        } else if bounds.width / bounds.height > aspect {
            fitHeight = bounds.height; fitWidth = fitHeight * aspect
        } else {
            fitWidth = bounds.width; fitHeight = fitWidth / aspect
        }
        let fitted = CGRect(x: bounds.minX + (bounds.width - fitWidth) / 2,
                            y: bounds.minY + (bounds.height - fitHeight) / 2,
                            width: fitWidth, height: fitHeight)
        let imageWidth = fitWidth / visibleSourceCrop.width
        let imageHeight = fitHeight / visibleSourceCrop.height
        let imageFrame = CGRect(x: fitted.minX - visibleSourceCrop.minX * imageWidth,
                                y: fitted.minY - visibleSourceCrop.minY * imageHeight,
                                width: imageWidth, height: imageHeight)
        guard fitWidth.isFinite, fitHeight.isFinite, fitWidth > 0, fitHeight > 0,
              fitted.minX.isFinite, fitted.minY.isFinite,
              fitted.maxX.isFinite, fitted.maxY.isFinite,
              imageFrame.origin.x.isFinite, imageFrame.origin.y.isFinite,
              imageWidth.isFinite, imageHeight.isFinite,
              imageFrame.maxX.isFinite, imageFrame.maxY.isFinite else { return nil }
        rect = fitted
        self.width = width; self.height = height
        self.visibleSourceCrop = visibleSourceCrop
        fullImageFrame = imageFrame
    }
    func normalized(_ point: CGPoint, held: Bool = false) -> (x: CGFloat, y: CGFloat)? {
        guard point.x.isFinite, point.y.isFinite,
              held || (point.x >= rect.minX && point.x <= rect.maxX &&
                       point.y >= rect.minY && point.y <= rect.maxY) else { return nil }
        let x = min(max((point.x - rect.minX) / rect.width, 0), 1)
        let y = min(max((point.y - rect.minY) / rect.height, 0), 1)
        return (visibleSourceCrop.minX + x * visibleSourceCrop.width,
                visibleSourceCrop.minY + y * visibleSourceCrop.height)
    }
    func map(_ point: CGPoint, held: Bool = false) -> (x: Int, y: Int)? {
        guard let (x, y) = normalized(point, held: held) else { return nil }
        return (Int((x * CGFloat(width - 1)).rounded()),
                Int((y * CGFloat(height - 1)).rounded()))
    }
    /// Full-source normalized coordinates to local points. A valid source point
    /// outside the visible crop maps outside `rect`, where presentation clips it.
    func point(normalized coordinate: CGPoint) -> CGPoint? {
        guard coordinate.x.isFinite, coordinate.y.isFinite,
              coordinate.x >= 0, coordinate.x <= 1,
              coordinate.y >= 0, coordinate.y <= 1 else { return nil }
        let point = CGPoint(x: rect.minX + (coordinate.x - visibleSourceCrop.minX) * fullImageFrame.width,
                            y: rect.minY + (coordinate.y - visibleSourceCrop.minY) * fullImageFrame.height)
        return point.x.isFinite && point.y.isFinite ? point : nil
    }
    /// Preserve the established endpoint convention: 0 and width/height - 1
    /// identify the full source endpoints. Host desktop origins are not offsets
    /// in this local source transform.
    func point(x: Int, y: Int) -> CGPoint? {
        guard x >= 0, x < width, y >= 0, y < height else { return nil }
        return point(normalized: CGPoint(x: CGFloat(x) / CGFloat(width - 1),
                                         y: CGFloat(y) / CGFloat(height - 1)))
    }
}

/// Stationary UIKit hover callbacks must not switch Linux's active pointer
/// source from uinput scrolling back to XTEST motion. The next position after
/// a pen/focus change is always sent, even if it matches an earlier position.
struct PlankIPadPointerMotion {
    private struct Position: Equatable {
        let x: Int, y: Int, width: Int, height: Int
    }
    private var last: Position?
    mutating func shouldSend(x: Int, y: Int, width: Int, height: Int, force: Bool = false) -> Bool {
        let next = Position(x: x, y: y, width: width, height: height)
        guard force || next != last else { return false }
        last = next
        return true
    }
    mutating func reset() { last = nil }
}

struct PlankIPadHeldInput {
    private(set) var buttons = Set<UInt8>()
    private(set) var keys: [UInt16: UInt8] = [:]
    mutating func button(_ number: UInt8, pressed: Bool) -> Bool {
        pressed ? buttons.insert(number).inserted : buttons.remove(number) != nil
    }
    mutating func key(_ code: UInt16, pressed: Bool, modifiers: UInt8) -> Bool {
        if pressed { keys[code] = modifiers; return true }
        return keys.removeValue(forKey: code) != nil
    }
    mutating func releaseButtons() -> [UInt8] {
        let result = buttons.sorted()
        buttons.removeAll()
        return result
    }
    mutating func release() -> (buttons: [UInt8], keys: [(UInt16, UInt8)]) {
        let result = (buttons.sorted(), keys.sorted { $0.key < $1.key }.map { ($0.key, $0.value) })
        buttons.removeAll(); keys.removeAll()
        return result
    }
}

struct PlankIPadWheel {
    enum Source { case discrete, continuous }
    private var x = 0.0
    private var y = 0.0
    mutating func add(x: Double, y: Double, source: Source) -> (vertical: Int16, horizontal: Int16) {
        guard x.isFinite, y.isFinite else { reset(); return (0, 0) }
        // UIKit reports view points, not the wire's 120 units per detent.
        // A discrete callback gets one bounded notch per nonzero axis, as in
        // the Mac adapter. Smooth input keeps fractions at 32 points/detent,
        // matching the existing Vision fallback's nominal scroll distance.
        if source == .discrete {
            reset()
            func notch(_ delta: Double) -> Int16 { delta == 0 ? 0 : (delta > 0 ? 120 : -120) }
            return (notch(y), notch(-x))
        }
        self.x += min(max(-x * 120 / 32, -32767), 32767)
        self.y += min(max(y * 120 / 32, -32767), 32767)
        let dx = min(max(self.x.rounded(.towardZero), -32767), 32767)
        let dy = min(max(self.y.rounded(.towardZero), -32767), 32767)
        self.x -= dx; self.y -= dy
        return (Int16(dy), Int16(dx))
    }
    mutating func reset() { x = 0; y = 0 }
}

/// Keep the original mapping until release, and balance aliases such as left
/// and right Shift. A mode change/focus loss retires this ledger explicitly.
struct PlankIPadKeyboardPolicy {
    struct Event: Equatable {
        let code: UInt16
        let pressed: Bool
        let modifiers: UInt8
    }
    private var held: [Int: UInt16] = [:]
    mutating func event(usage: Int, pressed: Bool, modifiers: UInt8,
                        mode: KeyboardFunctionKeyMode) -> Event? {
        if pressed {
            guard held[usage] == nil,
                  let code = plankIPadVirtualKey(for: usage, functionKeyMode: mode) else { return nil }
            let alreadyHeld = held.values.contains(code)
            held[usage] = code
            return alreadyHeld ? nil : Event(code: code, pressed: true, modifiers: modifiers)
        }
        guard let code = held.removeValue(forKey: usage), !held.values.contains(code) else { return nil }
        return Event(code: code, pressed: false, modifiers: modifiers)
    }
    mutating func reset() { held.removeAll() }
}

/// A squeeze is a discrete right-click, never a held mouse button. Only one
/// ended callback per timestamp is eligible; cancellation/changes do not click.
struct PlankIPadSqueezePolicy {
    private var lastTimestamp = -Double.infinity
    mutating func click(ended: Bool, timestamp: Double, enabled: Bool,
                        touching: Bool, heldButtons: Bool, hasPosition: Bool) -> Bool {
        guard ended, timestamp.isFinite, timestamp > lastTimestamp else { return false }
        lastTimestamp = timestamp
        return enabled && !touching && !heldButtons && hasPosition
    }
}
