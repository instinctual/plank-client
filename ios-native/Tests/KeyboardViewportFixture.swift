// Isolated simulator app: compile with PlankIPadKeyboardViewport.swift.
// Exercises production frame conversion with a real keyboard and synthetic
// float/dock reports. Synthetic transitions do not qualify physical gestures.
import UIKit
import SwiftUI

@main struct KeyboardViewportFixture: App {
    var body: some Scene { WindowGroup {
        NavigationStack {
            VStack(spacing: 0) {
                GeometryReader { geometry in
                    FixtureCanvas().frame(width: geometry.size.width, height: geometry.size.height)
                }
            }.ignoresSafeArea(.keyboard, edges: .bottom)
                .navigationTitle("Keyboard viewport check").navigationBarTitleDisplayMode(.inline)
        }.ignoresSafeArea(.keyboard, edges: .bottom)
    } }
}
private struct FixtureCanvas: UIViewRepresentable {
    func makeUIView(context: Context) -> FixtureView { FixtureView() }
    func updateUIView(_ uiView: FixtureView, context: Context) {}
}
@MainActor private final class FixtureView: UIView {
    let video = UIView()
    let entry = UITextField(frame: CGRect(x: 0, y: 0, width: 1, height: 1))
    lazy var viewport = PlankIPadKeyboardViewport(canvas: self)
    var keyboardFrame = CGRect.zero
    var snapshots: [[String: Any]] = []
    var started = false
    override init(frame: CGRect) {
        super.init(frame: frame)
        video.backgroundColor = .black
        addSubview(video); addSubview(entry)
        entry.autocorrectionType = .no
        NotificationCenter.default.addObserver(self, selector: #selector(changed(_:)),
            name: UIResponder.keyboardDidChangeFrameNotification, object: nil)
    }
    required init?(coder: NSCoder) { fatalError() }
    override func didMoveToWindow() {
        super.didMoveToWindow()
        guard window != nil, !started else { return }
        started = true
        viewport.start()
        let bounds = CGRect(x: 0, y: 0, width: 1180, height: 746)
        let docked = CGRect(x: 0, y: 334, width: 1180, height: 486)
        let floating = CGRect(x: 600, y: 170, width: 320, height: 300)
        precondition(PlankIPadKeyboardViewport.visibleFrame(in: bounds, keyboard: floating) == bounds)
        for _ in 0..<10 {
            precondition(PlankIPadKeyboardViewport.visibleFrame(in: bounds, keyboard: docked).height == 334,
                "Repeated reports must not cumulatively subtract keyboard space")
        }
        let alreadyAbove = CGRect(x: 0, y: 0, width: 1180, height: 334)
        precondition(PlankIPadKeyboardViewport.visibleFrame(in: alreadyAbove, keyboard: docked) == alreadyAbove,
            "An owner already above the keyboard must not subtract it again")
        precondition(PlankIPadKeyboardViewport.visibleFrame(in: bounds, keyboard: nil) == bounds)
        precondition(PlankIPadKeyboardViewport.visibleFrame(in: bounds,
            keyboard: CGRect(x: 0, y: 746, width: 1180, height: 300)) == bounds)
        for (delay, action) in [(1.0, true), (5.0, false), (9.0, true)] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                if action { self.entry.becomeFirstResponder() }
                else { self.entry.resignFirstResponder() }
            }
        }
        for (delay, name) in [(3.0, "docked"), (7.0, "hidden"), (11.0, "reopened"),
                              (14.0, "synthetic-floating"), (17.0, "synthetic-expanded"),
                              (20.0, "synthetic-repeated-expanded"), (23.0, "restored-system")] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { self.record(name) }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 12) {
            guard let window = self.window else { return }
            let local = CGRect(x: 100, y: 100, width: 320, height: 300)
            let screen = self.convert(local, to: window.screen.coordinateSpace)
            self.post(screen)
        }
        for delay in [15.0, 18.0] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                guard let window = self.window else { return }
                var local = self.keyboardFrame; local.origin.y += 40; local.size.height -= 40
                self.post(self.convert(local, to: window.screen.coordinateSpace))
            }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 21) {
            guard let window = self.window else { return }
            self.post(self.convert(self.keyboardFrame, to: window.screen.coordinateSpace))
        }
    }
    func post(_ frame: CGRect) {
        NotificationCenter.default.post(name: UIResponder.keyboardDidChangeFrameNotification,
            object: window?.screen, userInfo: [UIResponder.keyboardFrameEndUserInfoKey: frame,
                                            "PLANKFixtureSynthetic": true])
    }
    @objc func changed(_ notification: Notification) {
        guard notification.userInfo?["PLANKFixtureSynthetic"] as? Bool != true else { return }
        guard let rect = notification.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? CGRect,
              let window else { return }
        keyboardFrame = convert(window.convert(rect, from: window.screen.coordinateSpace), from: window)
    }
    override func layoutSubviews() { super.layoutSubviews(); video.frame = viewport.videoFrame }
    func record(_ name: String) {
        layoutIfNeeded()
        snapshots.append(["name": name, "ownerHeight": bounds.height,
            "videoHeight": video.frame.height, "videoBottom": video.frame.maxY,
            "guideTop": keyboardLayoutGuide.layoutFrame.minY,
            "keyboardTop": keyboardFrame.minY, "keyboardWidth": keyboardFrame.width,
            "reportedTop": viewport.reportedFrame?.minY ?? -1,
            "reportedWidth": viewport.reportedFrame?.width ?? 0])
        let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("geometry.json")
        try? JSONSerialization.data(withJSONObject: snapshots, options: [.prettyPrinted, .sortedKeys]).write(to: url)
    }
}
