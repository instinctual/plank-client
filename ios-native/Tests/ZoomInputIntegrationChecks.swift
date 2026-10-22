import Foundation
import Combine

// Exact native contact, mapping, presentation and retirement bodies are extracted
// by run-zoom-input-integration-checks.py. UIKit/layers/peer offers are stand-ins.
@main enum ZoomInputIntegrationChecks {
    @MainActor static func main() {
        direct()
        sharing()
        sharingAvailability()
        print("PASS: direct/shared release ordering, deferred crop, image/input mapping, held modifiers, local Hand, configured image navigation during paused remote admission, focus-only view preservation, blocked remote pen/keys, stale-contact discard and geometry/source resets")
    }

    @MainActor static func direct() {
        let suite = "plank-zoom-direct-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName:suite)!
        defer { defaults.removePersistentDomain(forName:suite) }
        let client = PlankCoreClient()
        let router = PlankIPadInputRouter(client:client,defaults:defaults)
        let zoom = PlankIPadZoomController()
        let view = PlankIPadInputView(client:client,router:router,zoomController:zoom)
        router.surface = view; router.enabled = true
        let owner = UUID()
        precondition(router.beginControl(owner:owner,binding:.init(code:0x20)))
        router.key(0x10,pressed:true,modifiers:1)
        let epoch = router.inputEpoch
        view.video.onViewport = { viewport in
            precondition(view.testViewport?.visibleSourceCrop == viewport?.visibleSourceCrop,
                "Input and rendered video must already share the emitted immutable crop")
        }

        let mouse = UITouch(.indirectPointer)
        mouse.view = view
        view.touchesBegan([mouse],with:UIEvent(5))
        precondition(zoom.remoteContactActive && router.hasHeldButtons)
        let before = client.events.count
        zoom.requestMode(.double)
        precondition(zoom.state.mode == .fit && client.events.count == before)
        view.video.onViewport = { viewport in
            precondition(view.activeTouch == nil && view.pointerButtons.isEmpty && !router.hasHeldButtons,
                "All mouse ups and contact fields precede applying the deferred view")
            precondition(view.testViewport?.visibleSourceCrop == viewport?.visibleSourceCrop)
        }
        view.touchesEnded([mouse],with:UIEvent())
        let tail = Array(client.events.dropFirst(before))
        let viewIndex = tail.firstIndex(where: { if case .view = $0 { return true }; return false })!
        for index in tail.indices where {
            if case .button(_,false) = tail[index] { return true }; return false
        }() { precondition(index < viewIndex) }
        precondition(tail.filter { if case .button = $0 { return true }; return false } == [.button(1,false),.button(2,false)])
        precondition(zoom.state.mode == .double && !zoom.remoteContactActive)
        precondition(router.inputEpoch == epoch && router.heldControlIDs == [owner])
        precondition(!tail.contains(where: { if case .key(_,false,_) = $0 { return true }; return false }))

        // A separately held pointer button also defers, without a UITouch.
        view.video.onViewport = nil
        router.button(3,pressed:true)
        zoom.requestMode(.fit)
        precondition(zoom.state.mode == .double && zoom.remoteContactActive)
        let releaseStart = client.events.count
        router.button(3,pressed:false)
        precondition(client.events[releaseStart] == .button(3,false))
        precondition(zoom.state.mode == .fit && !zoom.remoteContactActive)

        // Pen up/leave, then cleared native contact, then synchronous geometry.
        let pen = UITouch(.pencil); pen.view = view; pen.timestamp = 10
        view.touchesBegan([pen],with:nil)
        precondition(router.pencilTouching && zoom.remoteContactActive)
        let penStart = client.events.count
        zoom.requestMode(.double)
        view.video.onViewport = { viewport in
            precondition(view.activeTouch == nil && !router.pencilTouching)
            precondition(view.testViewport?.visibleSourceCrop == viewport?.visibleSourceCrop)
        }
        pen.timestamp = 11
        view.touchesEnded([pen],with:nil)
        let penTail = Array(client.events.dropFirst(penStart))
        let phases = penTail.compactMap { event -> PlankNormalizedPen.Phase? in
            if case .pen(let packet) = event { return packet.phase }; return nil
        }
        precondition(phases.suffix(2) == [.up,.leave])
        precondition({ if case .view = penTail.last! { return true }; return false }())
        precondition(router.inputEpoch == epoch && router.heldControlIDs == [owner])
        precondition(!penTail.contains(where: { if case .key(_,false,_) = $0 { return true }; return false }))

        // Existing remote finger ownership cannot become Hand without a new begin.
        view.video.onViewport = nil
        zoom.requestMode(.fit)
        let finger = UITouch(.direct); finger.view = view
        view.touchesBegan([finger],with:nil)
        zoom.requestMode(.double); zoom.requestHand(true)
        precondition(view.activeTouch === finger && view.handTouch == nil && zoom.state.mode == .fit)
        finger.point.x += 10
        view.touchesMoved([finger],with:nil)
        view.touchesEnded([finger],with:nil)
        precondition(zoom.state.handEnabled && zoom.state.mode == .double)
        let afterFinger = client.events
        view.touchesMoved([finger],with:nil)
        precondition(client.events == afterFinger && view.handTouch == nil)

        // Every native contact beginning in Hand stays local, including an
        // extra finger/Pencil or a margin contact when finger clicks are allowed.
        let primaryHand = UITouch(.direct); primaryHand.view = view
        let extraHand = UITouch(.direct); extraHand.view = view
        let extraPencil = UITouch(.pencil); extraPencil.view = view; extraPencil.timestamp = 15
        let outsideHand = UITouch(.direct); outsideHand.view = view; outsideHand.point.x = -50
        let multipleStart = client.events
        view.touchesBegan([primaryHand],with:nil)
        view.touchesBegan([extraHand],with:nil)
        view.touchesBegan([extraPencil],with:nil)
        view.touchesBegan([outsideHand],with:nil)
        precondition(view.handTouch === primaryHand && view.activeTouch == nil && !router.hasHeldButtons)
        precondition(client.events == multipleStart)
        view.touchesEnded([primaryHand],with:nil)
        extraHand.point.x += 10; extraPencil.timestamp = 16
        view.touchesMoved([extraHand,extraPencil,outsideHand],with:nil)
        view.touchesEnded([extraHand,extraPencil,outsideHand],with:nil)
        precondition(client.events == multipleStart && view.handTouch == nil)

        // Deliberate local navigation precedes Ignore finger filtering.
        router.setIgnoreFingerInput(true)
        let hand = UITouch(.direct); hand.view = view
        let handStart = client.events.count
        view.touchesBegan([hand],with:nil)
        precondition(view.handTouch === hand && view.activeTouch == nil && !zoom.remoteContactActive)
        hand.point.x += 20; view.touchesMoved([hand],with:nil)
        let center = zoom.state.center
        hand.point.x += 20; view.touchesMoved([hand],with:nil)
        precondition(abs(zoom.state.center.x - (center.x - 0.01)) < 0.0000001,
            "Each Hand callback uses incremental translation")
        view.touchesEnded([hand],with:nil)
        let handTail = Array(client.events.dropFirst(handStart))
        precondition(!handTail.contains(where: {
            if case .pointer = $0 { return true }
            if case .button = $0 { return true }
            if case .pen = $0 { return true }
            return false
        }))
        precondition(router.inputEpoch == epoch && router.heldControlIDs == [owner])

        let localPen = UITouch(.pencil); localPen.view = view; localPen.timestamp = 20
        view.touchesBegan([localPen],with:nil)
        precondition(view.handTouch === localPen && !router.pencilTouching)
        zoom.requestMode(.fit)
        localPen.point.y += 40
        view.touchesMoved([localPen],with:nil); view.touchesEnded([localPen],with:nil)
        precondition(view.activeTouch == nil && view.handTouch == nil,
            "A local contact keeps its original ownership when Hand is switched off")

        // Artist overlay forwards real Pencil contacts even while Hand is enabled.
        zoom.requestMode(.double); zoom.requestHand(true)
        let artistOverlay = UIView()
        let forwarded = UITouch(.pencil); forwarded.view = artistOverlay; forwarded.timestamp = 30
        view.touchesBegan([forwarded],with:nil)
        precondition(view.activeTouch === forwarded && view.handTouch == nil && router.pencilTouching)
        forwarded.timestamp = 31; view.touchesEnded([forwarded],with:nil)
        precondition(router.heldControlIDs == [owner])
        precondition(router.endControl(owner:owner))
        router.key(0x10,pressed:false,modifiers:0)

        // Forced source replacement discards queued voluntary view requests.
        zoom.requestMode(.fit); router.setIgnoreFingerInput(false)
        let forced = UITouch(.direct); forced.view = view
        view.touchesBegan([forced],with:nil)
        zoom.requestMode(.double)
        view.updateDimensions(.init(width:1200,height:1000))
        precondition(zoom.state == .fit && !zoom.remoteContactActive && view.activeTouch == nil)
        let forcedEvents = client.events
        forced.point.x += 40; view.touchesMoved([forced],with:nil); view.touchesEnded([forced],with:nil)
        precondition(client.events == forcedEvents && router.inputEpoch > epoch)
    }

    @MainActor static func sharing() {
        let relay = PlankIPadPencilRelay()
        let zoom = PlankIPadZoomController()
        let view = PlankIPadPencilPadView(relay:relay,zoomController:zoom)
        let space = UUID(), shift = UUID()
        precondition(relay.beginControl(owner:space,binding:.init(code:0x20)))
        precondition(relay.beginControl(owner:shift,binding:.init(code:0x10)))
        let epoch = relay.inputEpoch
        let pen = UITouch(.pencil); pen.view = view; pen.timestamp = 50
        view.touchesBegan([pen],with:nil)
        precondition(zoom.remoteContactActive && view.policy.touching)
        let start = relay.events.count
        zoom.requestMode(.double)
        precondition(zoom.state.mode == .fit && relay.events.count == start)
        view.onPresentation = { viewport in
            precondition(view.touch == nil && !view.policy.touching,
                "Sharing clears contact after up/leave, before applied crop presentation")
            precondition(view.imageView.frame == viewport?.rect)
            precondition(view.imageView.imageLayer.frame == viewport?.fullImageFrame.offsetBy(
                dx:-(viewport?.rect.minX ?? 0),dy:-(viewport?.rect.minY ?? 0)),
                "The full retained image positions from the exact input viewport")
        }
        pen.timestamp = 51; view.touchesEnded([pen],with:nil)
        let tail = Array(relay.events.dropFirst(start))
        let phases = tail.compactMap { event -> PlankNormalizedPen.Phase? in
            if case .pen(let packet) = event { return packet.phase }; return nil
        }
        precondition(phases == [.up,.leave])
        precondition({ if case .view = tail.last! { return true }; return false }())
        precondition(relay.inputEpoch == epoch && relay.controlKeys.controlOwners == [space,shift])
        precondition(!tail.contains(where: { if case .key(_,false,_) = $0 { return true }; return false }))

        // Sharing always ignores remote fingers, but Hand accepts them locally.
        view.onPresentation = nil
        zoom.requestHand(true)
        let finger = UITouch(.direct); finger.view = view
        let localStart = relay.events.count
        view.touchesBegan([finger],with:nil)
        precondition(view.handTouch === finger && view.touch == nil)
        let extra = UITouch(.pencil); extra.view = view; extra.timestamp = 55
        view.touchesBegan([extra],with:nil)
        precondition(view.handTouch === finger && view.touch == nil && !view.policy.touching)
        finger.point.y += 30; view.touchesMoved([finger],with:nil)
        view.touchesEnded([finger],with:nil)
        precondition(!relay.events.dropFirst(localStart).contains(where: { if case .pen = $0 { return true }; return false }))

        // Overlay-owned Pencil remains a remote stroke and defers local Hand pan.
        let overlay = UIView()
        let forwarded = UITouch(.pencil); forwarded.view = overlay; forwarded.timestamp = 60
        view.touchesBegan([forwarded],with:nil)
        precondition(view.touch === forwarded && view.policy.touching)
        let navigation = UITouch(.direct); navigation.view = view
        view.touchesBegan([navigation],with:nil)
        let heldCenter = zoom.state.center
        navigation.point.x += 20; view.touchesMoved([navigation],with:nil)
        navigation.point.x += 20; view.touchesMoved([navigation],with:nil)
        precondition(zoom.state.center == heldCenter && zoom.remoteContactActive)
        view.touchesEnded([navigation],with:nil)
        forwarded.timestamp = 61; view.touchesEnded([forwarded],with:nil)
        precondition(abs(zoom.state.center.x - (heldCenter.x - 40 / 968 * 0.5)) < 0.0000001)
        precondition(relay.inputEpoch == epoch && relay.controlKeys.controlOwners == [space,shift])

        // Forced relay admission retires pen and held controls before resetting view.
        zoom.requestHand(false)
        let forced = UITouch(.pencil); forced.view = view; forced.timestamp = 70
        view.touchesBegan([forced],with:nil)
        zoom.requestMode(.fit)
        view.onPresentation = { _ in
            precondition(view.touch == nil && !view.policy.touching && relay.controlKeys.controlOwners.isEmpty)
        }
        relay.forceRetire()
        precondition(zoom.state == .fit && !zoom.remoteContactActive && relay.inputEpoch == epoch + 1)
        let retired = relay.events
        forced.timestamp = 71; view.touchesMoved([forced],with:nil); view.touchesEnded([forced],with:nil)
        precondition(relay.events == retired)
        precondition(retired.filter { if case .key(_,false,_) = $0 { return true }; return false }.count == 2)
    }

    @MainActor static func sharingAvailability() {
        let relay = PlankIPadPencilRelay(), zoom = PlankIPadZoomController()
        let view = PlankIPadPencilPadView(relay:relay,zoomController:zoom)
        relay.active = false
        precondition(relay.canNavigateImage && !relay.canDraw,
            "A configured image remains locally navigable while Mac input is paused")
        relay.sharing = false; precondition(!relay.canNavigateImage); relay.sharing = true
        relay.hasConfiguration = false; precondition(!relay.canNavigateImage); relay.hasConfiguration = true
        relay.adjustingPad = true; precondition(!relay.canNavigateImage); relay.adjustingPad = false
        relay.imageReceiver.imageSessionActive = false; precondition(!relay.canNavigateImage); relay.imageReceiver.imageSessionActive = true

        zoom.requestMode(.double); zoom.requestHand(true)
        let finger = UITouch(.direct); finger.view = view
        view.touchesBegan([finger],with:nil)
        precondition(view.handTouch === finger)
        let center = zoom.state.center
        finger.point.x += 40; view.touchesMoved([finger],with:nil)
        view.touchesEnded([finger],with:nil)
        precondition(zoom.state.center.x < center.x && !zoom.remoteContactActive)
        let pen = UITouch(.pencil); pen.view = view; pen.timestamp = 80
        zoom.requestHand(false); view.touchesBegan([pen],with:nil)
        precondition(view.touch == nil && !view.policy.touching)
        precondition(!relay.beginControl(owner:UUID(),binding:.init(code:0x20)))
        precondition(!relay.tapControl(binding:.init(code:0x10)))
        precondition(!relay.events.contains { if case .pen = $0 { return true }; if case .key = $0 { return true }; return false },
            "Local navigation cannot grant remote pen or key admission")

        // Real production configuration handling retires held input on focus
        // loss but preserves the applied crop; queued requests cannot replay.
        relay.configureForTest(width:1000,height:1000,active:true)
        let current = zoom.state, revision = zoom.revision
        let space = UUID(); precondition(relay.beginControl(owner:space,binding:.init(code:0x20)))
        view.touchesBegan([pen],with:nil); precondition(view.policy.touching)
        zoom.requestMode(.fit)
        let start = relay.events.count
        relay.configureForTest(width:1000,height:1000,active:false)
        precondition(zoom.state == current && zoom.revision == revision && !zoom.remoteContactActive)
        precondition(relay.canNavigateImage && !relay.canDraw && relay.controlKeys.controlOwners.isEmpty)
        let tail = Array(relay.events.dropFirst(start))
        let phases: [PlankNormalizedPen.Phase] = tail.compactMap {
            if case .pen(let packet) = $0 { return packet.phase }; return nil
        }
        precondition(phases == [.cancel,.leave])
        precondition(tail.contains { if case .key(0x20,false,_) = $0 { return true }; return false })
        pen.timestamp = 81; view.touchesMoved([pen],with:nil); view.touchesEnded([pen],with:nil)
        precondition(relay.events.count == start + tail.count && zoom.state == current)
        zoom.requestCenter(CGPoint(x:0.7,y:0.6))
        let pausedView = zoom.state
        relay.configureForTest(width:1000,height:1000,active:true)
        precondition(zoom.state == pausedView && relay.canDraw)
        pen.timestamp = 82; view.touchesBegan([pen],with:nil)
        precondition(view.policy.touching && zoom.remoteContactActive)
        pen.timestamp = 83; view.touchesEnded([pen],with:nil)
        relay.configureForTest(width:1200,height:1000,active:true)
        precondition(zoom.state == .fit, "Actual source dimensions still reset the view")
        zoom.requestMode(.double); relay.hasConfiguration = false
        relay.configureForTest(width:1200,height:1000,active:false)
        precondition(zoom.state == .fit, "A first configuration cannot inherit an old peer's view")
    }
}
