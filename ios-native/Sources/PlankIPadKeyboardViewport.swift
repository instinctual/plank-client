import UIKit

@MainActor final class PlankIPadKeyboardViewport: NSObject {
    private weak var canvas: UIView?
    private var screenFrame: CGRect?
    private weak var screen: UIScreen?
    private var observing = false

    init(canvas: UIView) { self.canvas = canvas; super.init() }
    deinit { NotificationCenter.default.removeObserver(self) }

    func start() {
        guard !observing else { return }
        observing = true
        for name in [UIResponder.keyboardWillChangeFrameNotification,
                     UIResponder.keyboardDidChangeFrameNotification,
                     UIResponder.keyboardDidShowNotification,
                     UIResponder.keyboardDidHideNotification] {
            NotificationCenter.default.addObserver(self, selector: #selector(changed(_:)), name: name, object: nil)
        }
    }
    func stop() {
        NotificationCenter.default.removeObserver(self)
        observing = false; screenFrame = nil; screen = nil
        canvas?.setNeedsLayout()
    }
    @objc private func changed(_ notification: Notification) {
        guard let canvas, let window = canvas.window else { return }
        let notifyingScreen = (notification.object as? UIScreen) ?? window.screen
        guard notifyingScreen === window.screen else { return }
        if notification.name == UIResponder.keyboardDidHideNotification {
            screenFrame = nil
        } else {
            guard let frame = notification.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? CGRect else { return }
            screenFrame = frame
        }
        screen = notifyingScreen
        canvas.setNeedsLayout()
    }
    var reportedFrame: CGRect? {
        guard let canvas, let screenFrame, let screen,
              canvas.window?.screen === screen else { return nil }
        return canvas.convert(screenFrame, from: screen.coordinateSpace)
    }
    var videoFrame: CGRect {
        Self.visibleFrame(in: canvas?.bounds ?? .zero, keyboard: reportedFrame)
    }

    static func visibleFrame(in bounds: CGRect, keyboard: CGRect?) -> CGRect {
        // Absolute geometry, never a delta against the previous video frame.
        // The guide can retain an oversized reservation after float -> dock;
        // use only the public screen-frame report converted to canvas space.
        guard let keyboard,
              [keyboard.minX, keyboard.minY, keyboard.width, keyboard.height].allSatisfy({ $0.isFinite }),
              keyboard.width > 0, keyboard.height > 0,
              keyboard.minX <= bounds.minX + 1, keyboard.maxX >= bounds.maxX - 1,
              keyboard.maxY >= bounds.maxY - 1,
              keyboard.minY < bounds.maxY else { return bounds }
        let clipped = bounds.intersection(keyboard)
        guard !clipped.isNull, clipped.height > 0 else { return bounds }
        return CGRect(x: bounds.minX, y: bounds.minY, width: bounds.width,
            height: max(0, clipped.minY - bounds.minY))
    }
}
