#!/usr/bin/env python3
"""Check exact native contact/view bodies with stand-in UIKit and recording peers.

No application build, device, GUI, socket, Host or installed settings is touched.
This checks local state and offer ordering, not physical gesture delivery.
"""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
MODULE = importlib.util.spec_from_file_location("finger_fixture", REPO / "ios-native/Tests/run-finger-input-checks.py")
finger_fixture = importlib.util.module_from_spec(MODULE)
MODULE.loader.exec_module(finger_fixture)
declaration = finger_fixture.declaration

SUPPORT = r'''
import Foundation
import CoreGraphics
import Combine

struct PlankFrameDimensions: Equatable { let width:Int, height:Int }
enum PlankPencilWireError: Error { case invalid }
@MainActor final class PlankCoreClient {
    enum Event: Equatable {
        case pointer(Int,Int), button(UInt8,Bool), key(UInt16,Bool,UInt8)
        case pen(PlankNormalizedPen), scroll(Int16,Int16), text(String), reset
        case view(CGRect)
    }
    var events:[Event] = []
    var acceptsNormalizedPen = true
    func movePointer(x:Int,y:Int,width:Int,height:Int) { events.append(.pointer(x,y)) }
    func setMouseButton(number:UInt8,pressed:Bool) { events.append(.button(number,pressed)) }
    func offerControlKeys(_ batch:[PlankControlKeyEvent]) -> Bool {
        events += batch.map { .key($0.code,$0.pressed,$0.modifiers) }; return true
    }
    func sendPen(_ packet:PlankNormalizedPen) { events.append(.pen(packet)) }
    func sendText(_ text:String) { events.append(.text(text)) }
    func scroll(vertical:Int16,horizontal:Int16) { events.append(.scroll(vertical,horizontal)) }
    func reset() { events.append(.reset) }
}
final class UITouch: Hashable {
    enum TouchType { case direct, indirectPointer, pencil }
    let type:TouchType
    weak var view:UIView?
    var point = CGPoint(x:500,y:500)
    var timestamp:Double = 1, force:CGFloat = 1, maximumPossibleForce:CGFloat = 2
    var altitudeAngle:CGFloat = .pi / 2
    init(_ type:TouchType) { self.type = type }
    func location(in view:UIView) -> CGPoint { point }
    func azimuthAngle(in view:UIView) -> CGFloat { 0 }
    static func == (lhs:UITouch,rhs:UITouch) -> Bool { lhs === rhs }
    func hash(into hasher:inout Hasher) { hasher.combine(ObjectIdentifier(self)) }
}
final class UIEvent {
    struct ButtonMask: OptionSet { let rawValue:Int }
    var buttonMask:ButtonMask
    init(_ buttons:Int = 0) { buttonMask = .init(rawValue:buttons) }
    func coalescedTouches(for touch:UITouch) -> [UITouch]? { nil }
}
@MainActor class UIView {
    var frame = CGRect(x:0,y:0,width:1000,height:1000)
    var bounds: CGRect { CGRect(origin:.zero,size:frame.size) }
    func touchesBegan(_ touches:Set<UITouch>,with event:UIEvent?) {}
    func touchesMoved(_ touches:Set<UITouch>,with event:UIEvent?) {}
    func touchesEnded(_ touches:Set<UITouch>,with event:UIEvent?) {}
    func touchesCancelled(_ touches:Set<UITouch>,with event:UIEvent?) {}
    func setNeedsLayout() {}
}
@MainActor final class PlankIPadVideoView: UIView {
    let client:PlankCoreClient
    var viewport:PlankIPadViewport?
    var onViewport:((PlankIPadViewport?)->Void)?
    init(client:PlankCoreClient) { self.client = client }
    func setViewport(_ viewport:PlankIPadViewport?) {
        self.viewport = viewport
        client.events.append(.view(viewport?.visibleSourceCrop ?? .zero))
        onViewport?(viewport)
    }
}
@MainActor final class CALayer {
    var frame = CGRect.zero { didSet { onFrame?(frame) } }
    var onFrame:((CGRect)->Void)?
}
@MainActor final class CAShapeLayer {
    var path:CGPath?
    var fillColor:CGColor?, strokeColor:CGColor?
    var opacity:Float = 1
}
@MainActor enum CATransaction {
    static func begin() {}
    static func setDisableActions(_ disabled:Bool) {}
    static func commit() {}
}
struct UIColor {
    let cgColor:CGColor
    init(white:Double,alpha:Double) { cgColor = CGColor(gray:white,alpha:alpha) }
    init(red:Double,green:Double,blue:Double,alpha:Double) {
        cgColor = CGColor(red:red,green:green,blue:blue,alpha:alpha)
    }
}
struct UIBezierPath {
    let cgPath:CGPath
    init(rect:CGRect) { cgPath = CGPath(rect:rect,transform:nil) }
}
@MainActor final class PlankIPadPencilImageView: UIView {
    let imageLayer = CALayer()
'''

DIRECT_HEADER = r'''
@MainActor final class PlankIPadInputView: UIView {
    let client:PlankCoreClient, router:PlankIPadInputRouter
    let zoomController:PlankIPadZoomController?
    var zoomState:PlankIPadZoomState?
    var zoomObservation:AnyCancellable?
    let video:PlankIPadVideoView
    var dimensions:PlankFrameDimensions? = .init(width:1000,height:1000)
    var activeTouch:UITouch?
    var handTouch:UITouch?
    var handPoint:CGPoint?
    var pointerButtons = Set<UInt8>()
    var wheel = PlankIPadWheel()
    var keyboardPolicy = PlankIPadKeyboardPolicy()
    init(client:PlankCoreClient,router:PlankIPadInputRouter,zoomController:PlankIPadZoomController? = nil) {
        self.client = client; self.router = router; self.zoomController = zoomController
        zoomState = zoomController?.state; video = PlankIPadVideoView(client:client)
        super.init()
        zoomObservation = zoomController?.$state.sink { [weak self] state in
            MainActor.assumeIsolated { self?.applyZoomState(state) }
        }
    }
    var testViewport:PlankIPadViewport? { viewport }
    func resumeKeyboard() {}
    func suspendKeyboard() {}
    func refreshSoftwareKeyboard() {}
    func publishWheelDiagnostics() {}
'''

RELAY_HEADER = r'''
@MainActor final class PlankIPadPencilRelay {
    enum Event:Equatable { case pen(PlankNormalizedPen), key(UInt16,Bool,UInt8), view(CGRect) }
    var events:[Event] = []
    var active = true, adjustingPad = false, sharing = true, hasConfiguration = true
    var status = ""
    var width = 1000, height = 1000
    var inputEpoch:UInt64 = 0
    var padSettings = PlankIPadPencilPadSettings()
    struct ImageReceiver {
        var imageSessionActive = true
        func configureDimensions(width:Int,height:Int) {}
    }
    var imageReceiver = ImageReceiver()
    var controlKeys = PlankControlKeyOwnership()
    weak var pad:PlankIPadPencilPadView?
    func send(_ packet:PlankNormalizedPen) { events.append(.pen(packet)) }
    func offerControlKeys(_ batch:[PlankControlKeyEvent]) -> Bool {
        events += batch.map { .key($0.code,$0.pressed,$0.modifiers) }; return true
    }
    func forceRetire() { retireInput() }
    func configureForTest(width:Int,height:Int,active:Bool) {
        applyConfiguration(width:width,height:height,active:active)
    }
'''

SHARED_HEADER = r'''
@MainActor final class PlankIPadPencilPadView: UIView {
    let relay:PlankIPadPencilRelay
    let zoomController:PlankIPadZoomController?
    var zoomState:PlankIPadZoomState?
    var zoomObservation:AnyCancellable?
    var policy = PlankIPadPencilPolicy()
    var touch:UITouch?
    var handTouch:UITouch?
    var handPoint:CGPoint?
    var mappingSignature:String?
    var receivedDowns = 0, acceptedDowns = 0, acceptedMoves = 0, acceptedUps = 0
    let cursor = CAShapeLayer(), fill = CAShapeLayer(), outline = CAShapeLayer()
    let imageView = PlankIPadPencilImageView()
    var onPresentation:((PlankIPadViewport?)->Void)?
    init(relay:PlankIPadPencilRelay,zoomController:PlankIPadZoomController? = nil) {
        self.relay = relay; self.zoomController = zoomController; zoomState = zoomController?.state
        super.init(); relay.pad = self
        imageView.imageLayer.onFrame = { [weak self] _ in
            guard let self else { return }
            self.relay.events.append(.view(self.viewport?.visibleSourceCrop ?? .zero))
            self.onPresentation?(self.viewport)
        }
        zoomObservation = zoomController?.$state.sink { [weak self] state in
            MainActor.assumeIsolated { self?.applyZoomState(state) }
        }
    }
    var testViewport:PlankIPadViewport? { viewport }
    func mark(_ point:CGPoint) {}
    func logContactIfDue() {}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    output = args.output or pathlib.Path(tempfile.mkdtemp(prefix="plank-zoom-input-"))
    output.mkdir(parents=True, exist_ok=True)
    direct_source = (REPO / "ios-native/Sources/PlankIPadInput.swift").read_text()
    shared_source = (REPO / "ios-native/Sources/PlankIPadPencilRelay.swift").read_text()
    image_source = (REPO / "ios-native/Sources/PlankIPadPencilImage.swift").read_text()
    queue = (REPO / "visionos-native/Sources/Services/PlankInputQueue.swift").read_text()
    bookmark = (REPO / "visionos-native/Sources/Models/HostBookmark.swift").read_text()
    direct_markers = ["private var viewport:", "func updateDimensions(", "func clearContact()",
        "func clearPointerContact()", "func retireDirectContact()", "private func applyZoomState(",
        "private func updateVideoViewport()", "fileprivate func updateRemoteContact()", "private func beginHand(",
        "private func moveHand(", "private func endHand(", "override func touchesBegan(",
        "override func touchesMoved(", "override func touchesEnded(", "override func touchesCancelled(",
        "private func sendPencil(", "private func updatePointerButtons("]
    # Select from the native PadView only, because Relay has unrelated lifecycle methods.
    pad_source = shared_source[shared_source.index("@MainActor final class PlankIPadPencilPadView:"):]
    shared_markers = ["private var viewport:", "private func refreshPresentation()", "func retire()",
        "func retireForTransition()", "func resetViewAfterTransition(",
        "private func finishRemoteContact()", "private func retireHover()", "private func applyZoomState(",
        "private func updateRemoteContact()", "private func beginHand(", "private func moveHand(",
        "private func endHand(", "private func send(", "override func touchesBegan(",
        "override func touchesMoved(", "override func touchesEnded(", "override func touchesCancelled("]
    relay_markers = ["var canDraw:", "var canNavigateImage:", "func beginControl(", "func endControl(",
        "func tapControl(", "func releaseControls()", "private func retireInput(", "private func applyConfiguration("]
    bodies = {}
    def extracts(source, markers, label):
        result = []
        for marker in markers:
            body = declaration(source, marker)
            bodies[label + ":" + marker] = body
            result.append(body)
        return "\n".join(result)
    extracted = SUPPORT + extracts(image_source, ["func setViewport("], "image") + "\n}\n"
    extracted += declaration(queue, "struct PlankNormalizedPen:") + "\n"
    extracted += declaration(bookmark, "enum KeyboardFunctionKeyMode:") + "\n"
    router = declaration(direct_source, "@MainActor final class PlankIPadInputRouter:")
    extracted += router + "\n" + DIRECT_HEADER + extracts(direct_source, direct_markers, "direct") + "\n}\n"
    extracted += RELAY_HEADER + extracts(shared_source, relay_markers, "relay") + "\n}\n"
    extracted += SHARED_HEADER + extracts(pad_source, shared_markers, "sharing") + "\n}\n"
    fixture = output / "ProductionExtract.swift"
    fixture.write_text(extracted)
    inputs = ["ios-native/Sources/PlankIPadInputPolicy.swift", "ios-native/Sources/PlankIPadZoom.swift",
        "ios-native/Sources/PlankIPadPencilPolicy.swift", "ios-native/Sources/PlankIPadKeys.swift",
        "ios-native/Sources/PlankIPadPencilPadSettings.swift",
        "apple-native/Shared/PlankControlKeyOwnership.swift",
        "apple-native/Shared/PlankPencilModifiers.swift",
        "ios-native/Tests/ZoomInputIntegrationChecks.swift"]
    binary = output / "ZoomInputIntegrationChecks"
    command = ["xcrun", "swiftc", "-swift-version", "6", "-strict-concurrency=complete", "-Onone",
        str(fixture), *(str(REPO / path) for path in inputs), "-o", str(binary)]
    compiled = subprocess.run(command, text=True, capture_output=True)
    (output / "compile.log").write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr)
        raise SystemExit(compiled.returncode)
    result = subprocess.run([str(binary)], text=True, capture_output=True)
    (output / "run.log").write_text(result.stdout + result.stderr)
    tracked = inputs + ["ios-native/Sources/PlankIPadInput.swift", "ios-native/Sources/PlankIPadPencilRelay.swift",
        "ios-native/Sources/PlankIPadPencilImage.swift", "visionos-native/Sources/Services/PlankInputQueue.swift",
        "visionos-native/Sources/Models/HostBookmark.swift",
        "ios-native/Tests/run-zoom-input-integration-checks.py"]
    receipt = {"status":"PASS" if result.returncode == 0 else "FAIL", "exitCode":result.returncode,
        "stdout":result.stdout, "command":command,
        "sources":{p:hashlib.sha256((REPO / p).read_bytes()).hexdigest() for p in tracked},
        "exactExtracts":{marker:hashlib.sha256(body.encode()).hexdigest() for marker,body in bodies.items()},
        "routerExtractSHA256":hashlib.sha256(router.encode()).hexdigest(),
        "limitations":["UIKit delivery, peer/client offers, layer transactions and test construction are stand-ins.",
            "Physical OS touch arbitration, compositor alignment, remote transport/release receipt and device latency remain unqualified.",
            "Native controller subscriptions are set up by the fixture; the emitted-state application methods are exact production bodies."]}
    (output / "verification.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(result.stdout + result.stderr)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
