import Foundation

@main enum AutomaticImageChecks {
    static func settle() async { try? await Task.sleep(for:.milliseconds(40)) }
    @MainActor static func main() async throws {
        var checks:[String:Bool]=[:]
        func check(_ name:String,_ passed:Bool) { precondition(passed,name); checks[name]=passed }
        let relay=PlankIPadPencilRelay(), image=relay.imageReceiver
        check("idle_has_no_image_listener_or_authority",!image.enabled && !image.imageSessionActive && NWListener.made.isEmpty)
        relay.start(); let penListener=NWListener.made.last!
        let endpoint=NWEndpoint.service(name:"Registered Pencil 2",type:"_plank-pencil._tcp",domain:"local.",interface:nil)
        penListener.serviceRegistrationUpdateHandler?(.add(endpoint)); await settle()
        let listener=NWListener.made.last!
        check("sharing_automatically_starts_matching_image_service",image.enabled && !image.imageSessionActive && listener.service?.name == "Registered Pencil 2" && listener.service?.noAutoRename == true)
        let stranger=NWConnection(); listener.newConnectionHandler?(stranger); await settle()
        check("unapproved_connection_rejected_without_geometry_change",stranger.cancelled && PlankPencilImagePeer.made.isEmpty && relay.inputEpoch == 0)
        penListener.newConnectionHandler?(NWConnection()); await settle()
        let pen=PlankPencilRelayPeer.made.last!
        let binding=PlankPencilSessionBinding(peerKey:Data(repeating:2,count:32),transcriptHash:Data(repeating:3,count:64))
        pen.binding=binding; pen.event(.ready); await settle()
        pen.incoming.messages=[.configuration(640,480,true)]; pen.event(.messagesAvailable); await settle()
        check("approved_pen_configuration_does_not_start_image_mode",relay.canDraw && !image.imageSessionActive && !relay.canNavigateImage)
        let pad=PlankIPadPencilPadView(); relay.pad=pad
        let owner=UUID(); check("held_artist_control_starts",relay.beginControl(owner:owner,binding:.init(code:0x20)))
        let epoch=relay.inputEpoch
        var order:[String]=[]
        pad.onRetire={ order.append(image.imageSessionActive ? "retire-image" : "retire-pad") }
        pad.onLayout={
            let releases=pen.sent.flatMap { message->[PlankPencilKeyEvent] in if case let .keys(keys)=message { return keys }; return [] }
            precondition(releases.last?.pressed == false,"Key release must precede new geometry")
            order.append(image.imageSessionActive ? "layout-image" : "layout-pad")
        }
        listener.newConnectionHandler?(NWConnection()); await settle()
        let media=PlankPencilImagePeer.made.last!
        check("image_peer_uses_exact_current_pen_binding",media.binding == binding && !image.imageSessionActive)
        media.event(.ready); await settle()
        check("authenticated_image_retires_old_input_before_new_geometry",order == ["retire-pad","layout-image"] && relay.inputEpoch == epoch+1 && image.imageSessionActive && relay.canNavigateImage && !pen.closed)
        let view=PlankIPadPencilImageView(); image.subscribe(view)
        let descriptor=PlankPencilImageDescriptor(desktopID:UUID(),geometryGeneration:1,frameNumber:1,sourceWidth:640,sourceHeight:480,encodedWidth:64,encodedHeight:48)
        let frame=PlankPencilImageFrame(descriptor:descriptor,jpegData:Data([1]),receivedAtUptimeNanoseconds:DispatchTime.now().uptimeNanoseconds)
        media.event(.frame(frame)); await settle(); await settle()
        check("current_image_is_presented_without_ipad_switch",view.image != nil && image.status == "Receiving reference image")
        let beforeIntensity=relay.inputEpoch
        image.setIntensity(0.7)
        check("dimming_is_local_and_does_not_retire_input",view.intensity == 0.7 && relay.inputEpoch == beforeIntensity)
        // Expiry keeps the authenticated image lifetime's geometry stable.
        try await Task.sleep(for:.milliseconds(1850))
        check("stale_frame_clears_without_geometry_flapping",view.image == nil && image.imageSessionActive && relay.inputEpoch == beforeIntensity)
        order=[]; media.event(.ended("Sender off")); await settle()
        check("sender_off_restores_pad_and_keeps_listener_and_pen",order == ["retire-image","layout-pad"] && !image.imageSessionActive && image.enabled && !listener.cancelled && !pen.closed && !relay.canNavigateImage)
        listener.newConnectionHandler?(NWConnection()); await settle()
        let second=PlankPencilImagePeer.made.last!; second.event(.ready); await settle()
        check("sender_can_restart_without_ipad_action",image.imageSessionActive && second !== media && second.binding == binding)
        media.event(.ready); media.event(.ended("Stale")); media.event(.frame(frame)); await settle()
        check("retired_peer_callbacks_cannot_end_new_image",image.imageSessionActive && !second.closed)
        image.revoke()
        let rejected=NWConnection(); listener.newConnectionHandler?(rejected); await settle()
        check("pen_revocation_clears_image_and_blocks_new_media",!image.imageSessionActive && rejected.cancelled && second.closed && view.image == nil)
        image.authorize(.init(peerKey:Data(),transcriptHash:Data()))
        listener.newConnectionHandler?(NWConnection()); await settle()
        check("invalid_binding_cannot_authorize_media",PlankPencilImagePeer.made.last === second)
        let oldReady=listener.stateUpdateHandler, oldAccept=listener.newConnectionHandler
        relay.stop(); second.event(.ready); oldReady?(.ready); let late=NWConnection(); oldAccept?(late); await settle()
        check("stop_sharing_removes_listener_and_ignores_stale_callbacks",!image.enabled && !image.imageSessionActive && listener.cancelled && late.cancelled && view.image == nil && !relay.sharing)
        relay.start(); let restartedPen=NWListener.made.last!
        restartedPen.serviceRegistrationUpdateHandler?(.add(endpoint)); await settle()
        check("new_sharing_lifetime_is_automatically_ready",image.enabled && !image.imageSessionActive && NWListener.made.last !== listener)
        relay.stop()
        print(String(decoding:try JSONSerialization.data(withJSONObject:checks,options:[.prettyPrinted,.sortedKeys]),as:UTF8.self))
    }
}
