import Foundation
import CoreGraphics

@main enum Checks {
    static func main() {
        precondition(PlankIPadDisplayOptions.defaultSize == .wuxga)
        precondition(PlankIPadDisplayOptions.choices(current: .wuxga) == [.wuxga, .tall2560])
        precondition(PlankIPadDisplayOptions.choices(current: .ultraHD) == [.wuxga, .tall2560, .ultraHD],
                     "Existing bookmarks must remain selectable without rewriting their mode")
        let small = SpatialDisplaySize.wuxga.pixelSize, large = SpatialDisplaySize.tall2560.pixelSize
        precondition(small.width * large.height == large.width * small.height)
        let viewport = PlankIPadViewport(bounds: CGRect(x: 0, y: 0, width: 1000, height: 1000), width: 1920, height: 1080)!
        precondition(viewport.rect == CGRect(x: 0, y: 218.75, width: 1000, height: 562.5))
        precondition(viewport.map(CGPoint(x: 500, y: 100)) == nil, "Letterbox must not start a click")
        precondition(viewport.map(CGPoint(x: 0, y: 218.75))!.x == 0)
        precondition(viewport.map(CGPoint(x: 1000, y: 781.25))!.x == 1919)
        precondition(viewport.map(CGPoint(x: 1000, y: 781.25))!.y == 1079)
        let edge = viewport.map(CGPoint(x: 2000, y: -100), held: true)!
        precondition(edge.x == 1919 && edge.y == 0, "Held drags clamp at the video edge")
        precondition(viewport.map(CGPoint(x: CGFloat.nan, y: 10), held: true) == nil)
        precondition(PlankIPadViewport(bounds: .zero, width: 1920, height: 1080) == nil)
        precondition(PlankIPadViewport(bounds: CGRect(x: 0, y: 0, width: 500, height: 500), width: 100000, height: 1080) == nil)
        let portrait = PlankIPadViewport(bounds: CGRect(x: 10, y: 20, width: 500, height: 1000), width: 1000, height: 500)!
        precondition(portrait.map(CGPoint(x: 260, y: 520))!.x == 500)
        precondition(portrait.map(CGPoint(x: 260, y: 520))!.y == 250)
        var pointerMotion = PlankIPadPointerMotion()
        precondition(pointerMotion.shouldSend(x: 500, y: 250, width: 1920, height: 1080))
        for _ in 0..<100 {
            precondition(!pointerMotion.shouldSend(x: 500, y: 250, width: 1920, height: 1080),
                         "Stationary hover must not interrupt a sequence of wheel events")
        }
        precondition(pointerMotion.shouldSend(x: 501, y: 250, width: 1920, height: 1080),
                     "One-pixel motion must retain its precision")
        precondition(pointerMotion.shouldSend(x: 501, y: 250, width: 1920, height: 1080, force: true),
                     "Contact/button positioning must survive a remote cursor warp")
        precondition(pointerMotion.shouldSend(x: 501, y: 250, width: 2560, height: 1600))
        pointerMotion.reset()
        precondition(pointerMotion.shouldSend(x: 501, y: 250, width: 2560, height: 1600),
                     "Pen handoff or focus reset must restore the mouse's position")
        var held = PlankIPadHeldInput()
        precondition(held.button(1, pressed: true))
        precondition(!held.button(1, pressed: true), "Repeated down must not create two held contacts")
        precondition(held.button(3, pressed: true))
        precondition(held.key(65, pressed: true, modifiers: 1))
        precondition(held.releaseButtons() == [1, 3])
        precondition(held.keys[65] == 1, "Pencil contact must preserve held Space/shortcut keys")
        precondition(held.button(1, pressed: true))
        precondition(held.button(3, pressed: true))
        let releases = held.release()
        precondition(releases.buttons == [1, 3] && releases.keys.count == 1)
        precondition(releases.keys[0].0 == 65 && releases.keys[0].1 == 1)
        precondition(held.release().buttons.isEmpty && held.keys.isEmpty)
        precondition(!held.button(1, pressed: false), "Stale up after cancellation must not create contact")
        precondition(!held.key(65, pressed: false, modifiers: 1))
        var wheel = PlankIPadWheel()
        precondition(wheel.add(x: 0, y: 0.1, source: .continuous).vertical == 0)
        precondition(wheel.add(x: 0, y: 0.1, source: .continuous).vertical == 0)
        precondition(wheel.add(x: 0, y: 0.1, source: .continuous).vertical == 1,
                     "Small smooth samples accumulate in wire units")
        wheel.reset()
        precondition(wheel.add(x: 32, y: -32, source: .continuous).vertical == -120)
        precondition(wheel.add(x: 32, y: 0, source: .continuous).horizontal == -120)
        precondition(wheel.add(x: 0, y: 0.01, source: .discrete).vertical == 120,
                     "A fractional physical notch must reach the Host")
        precondition(wheel.add(x: -1000, y: -1000, source: .discrete).vertical == -120,
                     "Accelerated physical input is bounded to one notch per callback")
        precondition(wheel.add(x: -0.1, y: 0, source: .discrete).horizontal == 120)
        precondition(wheel.add(x: 0, y: 0, source: .discrete).vertical == 0)
        precondition(wheel.add(x: .infinity, y: .nan, source: .continuous).vertical == 0)
        precondition(wheel.add(x: 0, y: Double.greatestFiniteMagnitude, source: .continuous).vertical == 32767)
        precondition(wheel.add(x: 0, y: 0, source: .continuous).vertical == 0,
                     "Saturation cannot replay a delayed tail")
        wheel.reset()
        precondition(wheel.add(x: 0, y: 0.1, source: .continuous).vertical == 0)
        wheel.reset()
        precondition(wheel.add(x: 0, y: 0.2, source: .continuous).vertical == 0,
                     "Focus or geometry reset discards pending smooth motion")
        precondition(plankIPadVirtualKey(for: 0x2C, functionKeyMode: .pc) == 0x20)
        for hid in 0x3A...0x45 {
            precondition(plankIPadVirtualKey(for: hid, functionKeyMode: .pc) == UInt16(0x70 + hid - 0x3A))
            precondition(plankIPadVirtualKey(for: hid, functionKeyMode: .appleExtended) == UInt16(0x70 + hid - 0x3A))
        }
        precondition(plankIPadVirtualKey(for: 0x68, functionKeyMode: .pc) == 0x2C)
        precondition(plankIPadVirtualKey(for: 0x69, functionKeyMode: .pc) == 0x91)
        precondition(plankIPadVirtualKey(for: 0x6A, functionKeyMode: .pc) == 0x13)
        precondition(plankIPadVirtualKey(for: 0x68, functionKeyMode: .appleExtended) == 0x7C)
        precondition(plankIPadVirtualKey(for: 0x73, functionKeyMode: .appleExtended) == 0x87)
        var keyboard = PlankIPadKeyboardPolicy()
        precondition(keyboard.event(usage: 0x2C, pressed: true, modifiers: 0, mode: .pc)?.pressed == true)
        precondition(keyboard.event(usage: 0x2C, pressed: true, modifiers: 0, mode: .pc) == nil,
                     "UIKit and raw events must not duplicate the same key")
        precondition(keyboard.event(usage: 0x2C, pressed: false, modifiers: 0, mode: .pc)?.pressed == false)
        precondition(keyboard.event(usage: 0x68, pressed: true, modifiers: 1, mode: .appleExtended)?.code == 0x7C)
        precondition(keyboard.event(usage: 0x68, pressed: false, modifiers: 0, mode: .pc)?.code == 0x7C,
                     "Release retains the down mapping")
        precondition(keyboard.event(usage: 0xE1, pressed: true, modifiers: 1, mode: .pc) != nil)
        precondition(keyboard.event(usage: 0xE5, pressed: true, modifiers: 1, mode: .pc) == nil)
        precondition(keyboard.event(usage: 0xE1, pressed: false, modifiers: 1, mode: .pc) == nil)
        precondition(keyboard.event(usage: 0xE5, pressed: false, modifiers: 0, mode: .pc)?.pressed == false)
        _ = keyboard.event(usage: 0x2C, pressed: true, modifiers: 0, mode: .pc)
        keyboard.reset()
        precondition(keyboard.event(usage: 0x2C, pressed: false, modifiers: 0, mode: .pc) == nil)
        var squeeze = PlankIPadSqueezePolicy()
        precondition(!squeeze.click(ended: false, timestamp: 1, enabled: true, touching: false, heldButtons: false, hasPosition: true))
        precondition(squeeze.click(ended: true, timestamp: 1, enabled: true, touching: false, heldButtons: false, hasPosition: true))
        precondition(!squeeze.click(ended: true, timestamp: 1, enabled: true, touching: false, heldButtons: false, hasPosition: true))
        precondition(!squeeze.click(ended: true, timestamp: 3, enabled: false, touching: false, heldButtons: false, hasPosition: true))
        precondition(!squeeze.click(ended: true, timestamp: 4, enabled: true, touching: true, heldButtons: false, hasPosition: true))
        precondition(!squeeze.click(ended: true, timestamp: 5, enabled: true, touching: false, heldButtons: true, hasPosition: true))
        precondition(!squeeze.click(ended: true, timestamp: 6, enabled: true, touching: false, heldButtons: false, hasPosition: false))
        precondition(!squeeze.click(ended: true, timestamp: .nan, enabled: true, touching: false, heldButtons: false, hasPosition: true))
        precondition(PlankIPadSoftwareKeyboard.commands(for: "Az9 !") == [
            .key(0x41, 1), .key(0x5A, 0), .key(0x39, 0), .key(0x20, 0), .key(0x31, 1)
        ])
        precondition(PlankIPadSoftwareKeyboard.commands(for: "\n\t\u{8}\u{7f}\u{1b}") == [
            .key(0x0D, 0), .key(0x09, 0), .key(0x08, 0), .key(0x08, 0), .key(0x1B, 0)
        ])
        precondition(PlankIPadSoftwareKeyboard.commands(for: "\r\n") == [.key(0x0D, 0)],
                     "A CRLF commit must not press Return twice")
        precondition(PlankIPadSoftwareKeyboard.commands(for: "🙂é") == [.text("🙂"), .text("é")])
        precondition(PlankIPadSoftwareKeyboard.commands(for: "a\n🙂b") == [
            .key(0x41, 0), .key(0x0D, 0), .text("🙂"), .key(0x42, 0)
        ], "Mixed Unicode and key input must retain callback order")
        precondition(PlankIPadSoftwareKeyboard.commands(for: "").isEmpty)
        print("iPad viewport, held-input, wheel, live keyboard mapping/ownership and squeeze checks passed")
    }
}
