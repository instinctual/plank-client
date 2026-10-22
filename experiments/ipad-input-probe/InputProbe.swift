// SPDX-License-Identifier: GPL-3.0-or-later
import UIKit

@main
final class ProbeAppDelegate: UIResponder, UIApplicationDelegate {}

final class ProbeSceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    private var controller: ProbeController?
    func scene(_ scene: UIScene, willConnectTo session: UISceneSession,
               options connectionOptions: UIScene.ConnectionOptions) {
        guard let scene = scene as? UIWindowScene else { return }
        let controller = ProbeController()
        let window = UIWindow(windowScene: scene)
        window.rootViewController = controller
        self.controller = controller; self.window = window
        window.makeKeyAndVisible()
    }
    func sceneWillResignActive(_ scene: UIScene) { controller?.setActive(false) }
    func sceneDidEnterBackground(_ scene: UIScene) { controller?.setActive(false) }
    func sceneDidBecomeActive(_ scene: UIScene) { controller?.setActive(true) }
    func sceneDidDisconnect(_ scene: UIScene) { controller?.setActive(false) }
}

final class ProbeController: UIViewController {
    private let canvas = ProbeCanvas()
    private let readings = UILabel()
    private var timer: Timer?

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemBackground
        let title = UILabel()
        title.text = "PLANK Input Test"
        title.font = .preferredFont(forTextStyle: .title1)
        let instructions = UILabel()
        instructions.text = "Draw, tap and hold a drag. Move the mouse and scroll over the canvas. Rotate or background the app: Held should return to 0."
        instructions.numberOfLines = 0
        instructions.font = .preferredFont(forTextStyle: .body)
        let scope = UILabel()
        scope.text = "Local test • blue: Pencil • orange: pointer • gray: finger"
        scope.font = .preferredFont(forTextStyle: .caption1)
        scope.textColor = .secondaryLabel; scope.numberOfLines = 0
        readings.font = .monospacedSystemFont(ofSize: 14, weight: .regular)
        readings.numberOfLines = 0
        readings.accessibilityIdentifier = "input-readings"
        readings.text = canvas.readings
        let reset = makeButton("Reset", action: #selector(resetTest))
        let cancel = makeButton("End contact", action: #selector(endContact))
        let share = makeButton("Share counts", action: #selector(shareCounts(_:)))
        let buttons = UIStackView(arrangedSubviews: [reset, cancel, share])
        buttons.axis = .horizontal; buttons.spacing = 10; buttons.distribution = .fillEqually
        let stack = UIStackView(arrangedSubviews: [title, instructions, scope, readings, buttons, canvas])
        stack.axis = .vertical; stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor, constant: 20),
            stack.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -20),
            stack.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 16),
            stack.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor, constant: -16),
            buttons.heightAnchor.constraint(equalToConstant: 44),
            canvas.heightAnchor.constraint(greaterThanOrEqualToConstant: 100)
        ])
        canvas.setContentHuggingPriority(.defaultLow, for: .vertical)
        startTimer()
    }
    private func makeButton(_ title: String, action: Selector) -> UIButton {
        let button = UIButton(type: .system)
        var config = UIButton.Configuration.tinted()
        config.title = title; button.configuration = config
        button.addTarget(self, action: action, for: .touchUpInside)
        return button
    }
    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated); canvas.becomeFirstResponder()
    }
    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated); canvas.cancelAll(reason: "view hidden")
    }
    override func viewWillTransition(to size: CGSize,
                                     with coordinator: UIViewControllerTransitionCoordinator) {
        canvas.cancelAll(reason: "rotation")
        super.viewWillTransition(to: size, with: coordinator)
    }
    func setActive(_ active: Bool) {
        canvas.accepting = active
        if active { startTimer(); canvas.becomeFirstResponder() }
        else { timer?.invalidate(); timer = nil; canvas.cancelAll(reason: "inactive") }
        refreshReadings()
    }
    private func startTimer() {
        guard timer == nil else { return }
        // Diagnostics update at 8 Hz, independent of individual pen reports.
        timer = Timer.scheduledTimer(timeInterval: 0.125, target: self,
                                     selector: #selector(refreshReadings), userInfo: nil, repeats: true)
    }
    @objc private func refreshReadings() { readings.text = canvas.readings }
    @objc private func resetTest() { canvas.reset(); refreshReadings() }
    @objc private func endContact() { canvas.cancelAll(reason: "manual"); refreshReadings() }
    @objc private func shareCounts(_ sender: UIButton) {
        var report = canvas.report // Preserve evidence before the share sheet's cleanup.
        canvas.cancelAll(reason: "share")
        report["heldAfterShareCleanup"] = canvas.report["held"]
        guard let data = try? JSONSerialization.data(withJSONObject: report,
                                                    options: [.prettyPrinted, .sortedKeys]),
              let text = String(data: data, encoding: .utf8) else { return }
        let sheet = UIActivityViewController(activityItems: [text], applicationActivities: nil)
        sheet.popoverPresentationController?.sourceView = sender
        sheet.popoverPresentationController?.sourceRect = sender.bounds
        present(sheet, animated: true)
    }
}

final class ProbeCanvas: UIView {
    private struct Stroke { let id: Int; let color: UIColor; var points: [CGPoint] }
    private var ledger = ContactLedger()
    private var touchIDs: [ObjectIdentifier: Int] = [:]
    private var nextID = 0
    private var strokes: [Stroke] = []
    private var lastSize: CGSize = .zero
    private var pencilHover: CGPoint?
    private var pointerHover: CGPoint?
    private var pencilHoverEvents = 0
    private var pointerHoverEvents = 0
    private var pencilSamples = 0
    private var pointerSamples = 0
    private var fingerSamples = 0
    private var coalescedSamples = 0
    private var estimatedUpdates = 0
    private var pressure = 0.0
    private var altitude = 0.0
    private var azimuth = 0.0
    private var buttonMask = 0
    private var wheelEvents = 0
    private var wheelX = 0.0
    private var wheelY = 0.0
    private var markerY: CGFloat = 0.5
    private var keys = Set<Int>()
    private var keyDowns = 0
    private var keyUps = 0
    private var keyCancellations = 0
    var accepting = true
    override var canBecomeFirstResponder: Bool { true }

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .secondarySystemBackground
        layer.cornerRadius = 16; clipsToBounds = true
        isMultipleTouchEnabled = true
        let pencil = UIHoverGestureRecognizer(target: self, action: #selector(pencilHovered(_:)))
        pencil.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.pencil.rawValue)]
        pencil.cancelsTouchesInView = false; addGestureRecognizer(pencil)
        let pointer = UIHoverGestureRecognizer(target: self, action: #selector(pointerHovered(_:)))
        pointer.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]
        pointer.cancelsTouchesInView = false; addGestureRecognizer(pointer)
        let scroll = UIPanGestureRecognizer(target: self, action: #selector(scrolled(_:)))
        scroll.allowedScrollTypesMask = .all
        scroll.allowedTouchTypes = [] // Scrolling only; never steal Pencil/finger drags.
        scroll.cancelsTouchesInView = false; addGestureRecognizer(scroll)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }
    override func layoutSubviews() {
        super.layoutSubviews()
        if lastSize != .zero && lastSize != bounds.size { cancelAll(reason: "canvas resized") }
        lastSize = bounds.size
    }
    private func record(_ touch: UITouch, id: Int, phase: ContactLedger.Phase) {
        let position = touch.location(in: self)
        guard position.x.isFinite, position.y.isFinite,
              ledger.accept(id: id, phase: phase, timestamp: touch.timestamp) else { return }
        let color: UIColor
        switch touch.type {
        case .pencil:
            pencilSamples += 1; color = .systemBlue
            pressure = touch.maximumPossibleForce > 0 ?
                Double(touch.force / touch.maximumPossibleForce) : 0
            altitude = Double(touch.altitudeAngle * 180 / .pi)
            azimuth = Double(touch.azimuthAngle(in: self) * 180 / .pi)
        case .indirectPointer: pointerSamples += 1; color = .systemOrange
        default: fingerSamples += 1; color = .systemGray
        }
        if phase == .cancel { return }
        if !strokes.contains(where: { $0.id == id }) {
            if strokes.count >= 32 { strokes.removeFirst() }
            strokes.append(Stroke(id: id, color: color, points: []))
        }
        guard let index = strokes.firstIndex(where: { $0.id == id }) else { return }
        if strokes[index].points.count >= 1024 { strokes[index].points.removeFirst(128) }
        strokes[index].points.append(position)
    }
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard accepting else { return }
        becomeFirstResponder(); buttonMask = event?.buttonMask.rawValue ?? 0
        for touch in touches {
            nextID += 1; touchIDs[ObjectIdentifier(touch)] = nextID
            record(touch, id: nextID, phase: .down)
        }
        setNeedsDisplay()
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard accepting else { return }
        buttonMask = event?.buttonMask.rawValue ?? 0
        for touch in touches {
            guard let id = touchIDs[ObjectIdentifier(touch)] else { continue }
            let samples = event?.coalescedTouches(for: touch) ?? []
            coalescedSamples += samples.count
            // Ledger rejects repeated terminal samples and out-of-order timestamps.
            for sample in (samples + [touch]).sorted(by: { $0.timestamp < $1.timestamp }) {
                record(sample, id: id, phase: .move)
            }
        }
        setNeedsDisplay()
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        finish(touches, event: event, phase: .up)
    }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        finish(touches, event: event, phase: .cancel)
    }
    private func finish(_ touches: Set<UITouch>, event: UIEvent?, phase: ContactLedger.Phase) {
        guard accepting else { return }
        buttonMask = event?.buttonMask.rawValue ?? 0
        for touch in touches {
            guard let id = touchIDs.removeValue(forKey: ObjectIdentifier(touch)) else { continue }
            record(touch, id: id, phase: phase)
        }
        setNeedsDisplay()
    }
    override func touchesEstimatedPropertiesUpdated(_ touches: Set<UITouch>) {
        // Count late estimates separately; never invent another down/move transition.
        if accepting { estimatedUpdates += touches.count }
    }
    @objc private func pencilHovered(_ gesture: UIHoverGestureRecognizer) {
        guard accepting else { return }
        pencilHoverEvents += 1
        pencilHover = [.ended, .cancelled, .failed].contains(gesture.state) ? nil : gesture.location(in: self)
        setNeedsDisplay()
    }
    @objc private func pointerHovered(_ gesture: UIHoverGestureRecognizer) {
        guard accepting else { return }
        pointerHoverEvents += 1
        pointerHover = [.ended, .cancelled, .failed].contains(gesture.state) ? nil : gesture.location(in: self)
        setNeedsDisplay()
    }
    @objc private func scrolled(_ gesture: UIPanGestureRecognizer) {
        guard accepting else { return }
        let delta = gesture.translation(in: self)
        gesture.setTranslation(.zero, in: self)
        guard delta.x.isFinite, delta.y.isFinite, delta != .zero else { return }
        wheelEvents += 1; wheelX += Double(delta.x); wheelY += Double(delta.y)
        markerY = min(0.95, max(0.05, markerY + delta.y / max(1, bounds.height)))
        setNeedsDisplay()
    }
    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard accepting else { return }
        for press in presses {
            if let key = press.key, keys.insert(key.keyCode.rawValue).inserted { keyDowns += 1 }
        }
    }
    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        for press in presses {
            if let key = press.key, keys.remove(key.keyCode.rawValue) != nil { keyUps += 1 }
        }
    }
    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        for press in presses {
            if let key = press.key, keys.remove(key.keyCode.rawValue) != nil { keyCancellations += 1 }
        }
    }
    func cancelAll(reason: String) {
        ledger.cancelAll(reason: reason); touchIDs.removeAll()
        keyCancellations += keys.count; keys.removeAll(); buttonMask = 0
        pencilHover = nil; pointerHover = nil; setNeedsDisplay()
    }
    func reset() {
        cancelAll(reason: "reset"); ledger = ContactLedger(); strokes.removeAll()
        pencilHoverEvents = 0; pointerHoverEvents = 0; pencilSamples = 0
        pointerSamples = 0; fingerSamples = 0; coalescedSamples = 0; estimatedUpdates = 0
        pressure = 0; altitude = 0; azimuth = 0; wheelEvents = 0; wheelX = 0; wheelY = 0
        keyDowns = 0; keyUps = 0; keyCancellations = 0; markerY = 0.5
        setNeedsDisplay()
    }
    var readings: String {
        "Held: \(ledger.held)  Down/up/cancel: \(ledger.downs)/\(ledger.ups)/\(ledger.cancellations)\n" +
        "Pencil/pointer/finger: \(pencilSamples)/\(pointerSamples)/\(fingerSamples)  Hover: \(pencilHoverEvents)/\(pointerHoverEvents)\n" +
        String(format: "Force ratio: %.2f  Altitude/azimuth: %.0f°/%.0f° (API readings)\n", pressure, altitude, azimuth) +
        "Wheel: \(wheelEvents)  Button mask: \(buttonMask)  Keys held: \(keys.count)\n" +
        String(format: "Max contact sample gap: %.1f ms  Last cancel: %@", ledger.maxSampleGapMilliseconds, ledger.lastCancellationReason)
    }
    var report: [String: Any] {
        ["schema": 1, "probeBuild": 1, "deviceModel": UIDevice.current.model,
         "osVersion": UIDevice.current.systemVersion, "held": ledger.held,
         "downs": ledger.downs, "moves": ledger.moves, "ups": ledger.ups,
         "cancellations": ledger.cancellations, "duplicates": ledger.duplicates,
         "rejected": ledger.rejected, "maxContactSampleGapMs": ledger.maxSampleGapMilliseconds,
         "lastCancellationReason": ledger.lastCancellationReason,
         "pencilSamples": pencilSamples, "pointerSamples": pointerSamples,
         "fingerSamples": fingerSamples, "coalescedEntries": coalescedSamples,
         "estimatedUpdates": estimatedUpdates, "pencilHoverEvents": pencilHoverEvents,
         "pointerHoverEvents": pointerHoverEvents, "wheelEvents": wheelEvents,
         "wheelXPoints": wheelX, "wheelYPoints": wheelY,
         "keyDowns": keyDowns, "keyUps": keyUps, "keyCancellations": keyCancellations,
         "keysHeld": keys.count, "pressureHardwareSupport": "unverified",
         "rawWacomCapture": false, "remoteInput": false]
    }
    override func draw(_ rect: CGRect) {
        super.draw(rect)
        UIColor.separator.setStroke()
        let grid = UIBezierPath(); grid.lineWidth = 0.5
        for x in stride(from: CGFloat(0), through: bounds.width, by: 48) {
            grid.move(to: CGPoint(x: x, y: 0)); grid.addLine(to: CGPoint(x: x, y: bounds.height))
        }
        for y in stride(from: CGFloat(0), through: bounds.height, by: 48) {
            grid.move(to: CGPoint(x: 0, y: y)); grid.addLine(to: CGPoint(x: bounds.width, y: y))
        }
        grid.stroke()
        for stroke in strokes {
            stroke.color.setStroke(); stroke.color.setFill()
            if let point = stroke.points.first {
                UIBezierPath(ovalIn: CGRect(x: point.x - 2, y: point.y - 2,
                                            width: 4, height: 4)).fill()
                let line = UIBezierPath(); line.lineWidth = 3; line.lineCapStyle = .round
                line.lineJoinStyle = .round; line.move(to: point)
                for next in stroke.points.dropFirst() { line.addLine(to: next) }
                line.stroke()
            }
        }
        for (position, color) in [(pencilHover, UIColor.systemBlue), (pointerHover, .systemOrange)] {
            if let position {
                color.setStroke(); let ring = UIBezierPath(ovalIn:
                    CGRect(x: position.x - 8, y: position.y - 8, width: 16, height: 16))
                ring.lineWidth = 2; ring.stroke()
            }
        }
        UIColor.systemTeal.setFill()
        UIBezierPath(roundedRect: CGRect(x: bounds.midX - 40, y: markerY * bounds.height - 8,
                                         width: 80, height: 16), cornerRadius: 8).fill()
    }
}
