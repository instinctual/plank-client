#!/usr/bin/env python3
"""Run exact router/contact bodies with stand-in UIKit delivery and recording Host.

No device/GUI/Host is opened. This checks admission/lifetime, not palm rejection
by iPadOS or final remote delivery. Pass --output to retain private evidence.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]


def declaration(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 1
    cursor = opening + 1
    while depth:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
        cursor += 1
    return text[start:cursor]


SUPPORT = r'''
import Foundation
import CoreGraphics
import Combine

@MainActor final class PlankCoreClient {
    enum Event: Equatable {
        case pointer(Int,Int), button(UInt8,Bool), key(UInt16,Bool,UInt8)
        case pen(PlankNormalizedPen), scroll(Int16,Int16), text(String), reset
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
    func location(in view:PlankIPadInputView) -> CGPoint { point }
    func azimuthAngle(in view:PlankIPadInputView) -> CGFloat { 0 }
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
    func touchesBegan(_ touches:Set<UITouch>,with event:UIEvent?) {}
    func touchesMoved(_ touches:Set<UITouch>,with event:UIEvent?) {}
    func touchesEnded(_ touches:Set<UITouch>,with event:UIEvent?) {}
    func touchesCancelled(_ touches:Set<UITouch>,with event:UIEvent?) {}
}
@MainActor final class UIPanGestureRecognizer {
    enum State { case began,changed,ended,cancelled }
    var state:State = .changed
    var delta:CGPoint = .zero
    func translation(in view:PlankIPadInputView) -> CGPoint { delta }
    func setTranslation(_ point:CGPoint,in view:PlankIPadInputView) { delta = point }
    func location(in view:PlankIPadInputView) -> CGPoint { .init(x:500,y:500) }
}
'''
VIEW_HEADER = r'''
@MainActor final class PlankIPadInputView: UIView {
    let client:PlankCoreClient, router:PlankIPadInputRouter
    var activeTouch:UITouch?
    var handTouch:UITouch?
    var handPoint:CGPoint?
    let zoomController:PlankIPadZoomController? = nil
    var zoomState:PlankIPadZoomState? = nil
    var pointerButtons = Set<UInt8>()
    var wheel = PlankIPadWheel()
    var keyboardPolicy = PlankIPadKeyboardPolicy()
    var discreteScroll:UIPanGestureRecognizer? = UIPanGestureRecognizer()
    var wheelReceived = 0, wheelBlocked = 0, wheelEvents = 0
    var wheelVertical = 0, wheelHorizontal = 0
    let viewport = PlankIPadViewport(bounds:.init(x:0,y:0,width:1000,height:1000),width:1000,height:1000)
    init(client:PlankCoreClient,router:PlankIPadInputRouter) { self.client = client; self.router = router }
    func resumeKeyboard() {}
    func suspendKeyboard() {}
    func refreshSoftwareKeyboard() {}
    func publishWheelDiagnostics() {}
    func publishWheelDiagnosticsIfDue() {}
    func testScroll(_ recognizer:UIPanGestureRecognizer) { scrolled(recognizer) }
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    output = args.output or pathlib.Path(tempfile.mkdtemp(prefix="plank-finger-input-"))
    output.mkdir(parents=True, exist_ok=True)
    source = (REPO / "ios-native/Sources/PlankIPadInput.swift").read_text()
    queue = (REPO / "visionos-native/Sources/Services/PlankInputQueue.swift").read_text()
    bookmark = (REPO / "visionos-native/Sources/Models/HostBookmark.swift").read_text()
    markers = ["func clearContact()", "func clearPointerContact()", "func retireDirectContact()",
               "fileprivate func updateRemoteContact()", "private func beginHand(",
               "private func moveHand(", "private func endHand(",
               "override func touchesBegan(", "override func touchesMoved(", "override func touchesEnded(",
               "override func touchesCancelled(", "private func sendPencil(",
               "private func updatePointerButtons(", "@objc private func scrolled("]
    bodies = [declaration(source, marker) for marker in markers]
    # @objc is unavailable for the stand-in recognizer, the method body is exact.
    view = VIEW_HEADER + "\n".join(body.removeprefix("@objc ") for body in bodies) + "\n}\n"
    extracted = SUPPORT + declaration(queue, "struct PlankNormalizedPen:") + "\n"
    extracted += declaration(bookmark, "enum KeyboardFunctionKeyMode:") + "\n"
    extracted += declaration(source, "@MainActor final class PlankIPadInputRouter:") + "\n" + view
    fixture = output / "ProductionExtract.swift"
    fixture.write_text(extracted)
    inputs = ["ios-native/Sources/PlankIPadInputPolicy.swift",
              "ios-native/Sources/PlankIPadZoom.swift",
              "ios-native/Sources/PlankIPadPencilPolicy.swift", "ios-native/Sources/PlankIPadKeys.swift",
              "apple-native/Shared/PlankControlKeyOwnership.swift", "ios-native/Tests/FingerInputChecks.swift"]
    binary = output / "FingerInputChecks"
    command = ["xcrun", "swiftc", "-swift-version", "6", "-strict-concurrency=complete", "-Onone",
               str(fixture), *(str(REPO / path) for path in inputs), "-o", str(binary)]
    compile_result = subprocess.run(command, text=True, capture_output=True)
    (output / "compile.log").write_text(compile_result.stdout + compile_result.stderr)
    if compile_result.returncode:
        print(compile_result.stdout + compile_result.stderr)
        raise SystemExit(compile_result.returncode)
    result = subprocess.run([str(binary)], text=True, capture_output=True)
    (output / "run.log").write_text(result.stdout + result.stderr)
    receipt = {"status":"PASS" if result.returncode == 0 else "FAIL", "exitCode":result.returncode,
        "stdout":result.stdout, "command":command,
        "sources":{path:hashlib.sha256((REPO / path).read_bytes()).hexdigest() for path in inputs + [
            "ios-native/Sources/PlankIPadInput.swift", "visionos-native/Sources/Services/PlankInputQueue.swift",
            "visionos-native/Sources/Models/HostBookmark.swift", "ios-native/Tests/run-finger-input-checks.py"]},
        "exactExtracts": {marker:hashlib.sha256(body.encode()).hexdigest() for marker, body in zip(markers,bodies)},
        "routerExtractSHA256":hashlib.sha256(declaration(source,"@MainActor final class PlankIPadInputRouter:").encode()).hexdigest(),
        "limitations":["UITouch/UIEvent/recognizer delivery and view hierarchy are stand-ins; genuine OS palm/cancellation delivery is not established.",
            "Host records router offers only; final transport/remote release receipt is not established.",
            "Software/hardware responder ownership and UIKit touch gesture arbitration are not exercised."]}
    (output / "verification.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(result.stdout + result.stderr)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
