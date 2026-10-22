import Foundation
import Combine

// Executed by run-finger-input-checks.py against the exact production router
// and view contact bodies. UIKit event delivery and the Host are stand-ins.
@main enum FingerInputChecks {
    @MainActor static func main() {
        let suite = "plank-finger-checks-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName:suite)!
        defer { defaults.removePersistentDomain(forName:suite) }
        let client = PlankCoreClient()
        let input = PlankIPadInputRouter(client:client,defaults:defaults)
        let view = PlankIPadInputView(client:client,router:input)
        input.surface = view; input.enabled = true
        precondition(!input.ignoreFingerInput)

        let artist = UUID()
        precondition(input.beginControl(owner:artist,binding:.init(code:0x20)))
        input.key(0x10,pressed:true,modifiers:1)
        let finger = UITouch(.direct)
        view.touchesBegan([finger],with:nil)
        precondition(view.activeTouch === finger && input.hasHeldButtons)
        let epoch = input.inputEpoch
        let beforeToggle = client.events
        var published:[Bool] = []
        let observation = input.$ignoreFingerInput.dropFirst().sink { published.append($0) }
        input.setIgnoreFingerInput(true)
        precondition(Array(client.events.dropFirst(beforeToggle.count)) == [.button(1,false)],
            "Enabling mid-drag retires exactly the finger's mouse down")
        precondition(view.activeTouch == nil && !input.hasHeldButtons)
        precondition(input.inputEpoch == epoch && input.heldControlIDs == [artist],
            "The toggle must not retire Artist/hardware ownership")
        precondition(published == [true] && defaults.bool(forKey:"plank.ipad.ignoreFingerInput"))
        let afterToggle = client.events
        input.setIgnoreFingerInput(true)
        precondition(client.events == afterToggle && published == [true], "Same-value writes are a no-op")
        finger.point = .init(x:600,y:600)
        view.touchesMoved([finger],with:nil); view.touchesEnded([finger],with:nil)
        view.touchesCancelled([finger],with:nil)
        precondition(client.events == afterToggle, "Retired finger callbacks cannot resend motion/up")

        let reader = PlankIPadInputRouter(client:PlankCoreClient(),defaults:UserDefaults(suiteName:suite)!)
        precondition(reader.ignoreFingerInput, "The preference persists across router instances")
        precondition(input.endControl(owner:artist))
        input.key(0x10,pressed:false,modifiers:0)
        precondition(Array(client.events.dropFirst(afterToggle.count)) == [.key(0x20,false,1),.key(0x10,false,0)],
            "Artist/hardware releases retain their original ordered ownership")

        // A blocked fresh finger must be filtered before pointer() retires hover.
        precondition(input.pen(.hover,point:.init(x:400,y:400),viewport:view.viewport!,timestamp:10,
                               force:0,maximumForce:0,altitude:.pi/2,azimuth:0))
        let hovering = client.events
        let palm = UITouch(.direct)
        view.touchesBegan([palm],with:nil); view.touchesMoved([palm],with:nil)
        view.touchesEnded([palm],with:nil); view.touchesCancelled([palm],with:nil)
        precondition(client.events == hovering && view.activeTouch == nil,
            "Ignored finger/palm must not change the Pencil hover/pointer source")
        input.retirePencil()

        // Physical mouse buttons and wheel retain their accepted paths.
        let mouse = UITouch(.indirectPointer)
        let beforeMouse = client.events.count
        view.touchesBegan([mouse],with:UIEvent(1))
        let mouseHeld = client.events
        input.setIgnoreFingerInput(false); input.setIgnoreFingerInput(true)
        precondition(client.events == mouseHeld && view.activeTouch === mouse,
            "A preference change does not release an active physical pointer")
        mouse.point = .init(x:510,y:520)
        view.touchesMoved([mouse],with:UIEvent(1)); view.touchesEnded([mouse],with:UIEvent())
        let mouseEvents = Array(client.events.dropFirst(beforeMouse))
        precondition(mouseEvents.filter { if case .button = $0 { true } else { false } } == [.button(1,true),.button(1,false)])
        precondition(mouseEvents.filter { if case .pointer = $0 { true } else { false } }.count == 3)
        let wheel = view.discreteScroll!
        wheel.delta = .init(x:0,y:0.01)
        view.testScroll(wheel)
        precondition(client.events.last == .scroll(120,0), "Finger filtering must not suppress physical notches")
        input.button(2,pressed:true); input.button(2,pressed:false)
        precondition(client.events.suffix(2) == [.button(2,true),.button(2,false)])

        // Pencil contact and pressure must survive changing the setting.
        let pen = UITouch(.pencil)
        pen.timestamp = 20
        view.touchesBegan([pen],with:nil)
        precondition(input.pencilTouching && view.activeTouch === pen)
        let penHeld = client.events
        input.setIgnoreFingerInput(false); input.setIgnoreFingerInput(true)
        precondition(client.events == penHeld && input.pencilTouching && view.activeTouch === pen)
        pen.timestamp = 21; pen.force = 1.5
        view.touchesMoved([pen],with:nil)
        pen.timestamp = 22
        view.touchesEnded([pen],with:nil)
        precondition(!input.pencilTouching && view.activeTouch == nil)
        precondition(client.events.contains(where: { if case .pen(let packet) = $0 {
            packet.phase == .move && packet.pressureOrDistance == 0.75
        } else { false } }))

        input.setIgnoreFingerInput(false)
        precondition(!defaults.bool(forKey:"plank.ipad.ignoreFingerInput"))
        let beforeFresh = client.events.count
        let fresh = UITouch(.direct)
        view.touchesBegan([fresh],with:nil)
        fresh.point = .init(x:300,y:300)
        view.touchesMoved([fresh],with:nil); view.touchesEnded([fresh],with:nil)
        let freshEvents = Array(client.events.dropFirst(beforeFresh))
        precondition(freshEvents.filter { if case .button = $0 { true } else { false } } == [.button(1,true),.button(1,false)])
        precondition(freshEvents.filter { if case .pointer = $0 { true } else { false } }.count == 3,
            "Re-enabling finger input admits a fresh contact")

        // The disabled desktop/scene gate still refuses input with either setting.
        input.enabled = false
        let disabled = client.events
        view.touchesBegan([UITouch(.direct)],with:nil)
        input.setIgnoreFingerInput(true)
        view.touchesBegan([UITouch(.indirectPointer)],with:UIEvent(1))
        view.touchesBegan([UITouch(.pencil)],with:nil)
        precondition(client.events == disabled && view.activeTouch == nil)
        withExtendedLifetime(observation) {}
        print("PASS: persistent finger-only admission, balanced mid-contact retirement, late callback rejection, no-op changes, fresh re-enable, retained Artist/hardware/Pencil/mouse/wheel ownership and disabled-scene gate")
    }
}
