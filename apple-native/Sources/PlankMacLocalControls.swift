import AppKit
import SwiftUI

#if PLANK_PENCIL_RELAY_RECEIVER
/// SwiftUI can retain Settings content after its window closes. Tie the local
/// input pause to the actual window instead of the content's appearance alone.
struct PlankMacPencilSettingsPause: NSViewRepresentable {
    let client: PlankCoreClient
    func makeNSView(context: Context) -> PlankMacSettingsPauseView {
        let owner = UUID()
        return PlankMacSettingsPauseView { client.setPencilLocalControls($0,owner:owner) }
    }
    func updateNSView(_ view: PlankMacSettingsPauseView,context: Context) { view.scheduleRefresh() }
    static func dismantleNSView(_ view: PlankMacSettingsPauseView,coordinator: ()) { view.invalidate() }
}

final class PlankMacSettingsPauseView: NSView {
    private let changed: @MainActor (Bool) -> Void
    private weak var observedWindow: NSWindow?
    private var visibilityObservation: NSKeyValueObservation?
    private var observations: [NSObjectProtocol] = []
    private var generation = UUID(), closing = false, refreshQueued = false
    private var published = false
    init(changed: @escaping @MainActor (Bool) -> Void) { self.changed = changed; super.init(frame:.zero) }
    required init?(coder: NSCoder) { fatalError("init(coder:)") }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow(); invalidate()
        guard let window else { return }
        observedWindow = window; closing = false
        let current = generation
        visibilityObservation = window.observe(\.isVisible,options:[.new]) { [weak self] _,_ in
            Task { @MainActor [weak self] in
                guard let self, self.generation == current else { return }
                self.scheduleRefresh()
            }
        }
        for name in [NSWindow.willCloseNotification, NSWindow.didBecomeKeyNotification,
                     NSWindow.didResignKeyNotification, NSWindow.didMiniaturizeNotification,
                     NSWindow.didDeminiaturizeNotification, NSWindow.didChangeOcclusionStateNotification] {
            observations.append(NotificationCenter.default.addObserver(forName:name,object:window,queue:.main) { [weak self] _ in
                Task { @MainActor [weak self] in
                    guard let self, self.generation == current else { return }
                    if name == NSWindow.willCloseNotification { self.closing = true; self.publish(false) }
                    if name == NSWindow.didBecomeKeyNotification { self.closing = false }
                    self.scheduleRefresh()
                }
            })
        }
        scheduleRefresh()
    }
    func scheduleRefresh() {
        guard !refreshQueued else { return }
        refreshQueued = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.refreshQueued = false
            self.publish(!self.closing && self.observedWindow?.isVisible == true &&
                         self.observedWindow?.isMiniaturized == false)
        }
    }
    func invalidate() {
        generation = UUID()
        visibilityObservation?.invalidate(); visibilityObservation = nil
        observations.forEach(NotificationCenter.default.removeObserver); observations = []
        observedWindow = nil; closing = false; publish(false)
    }
    private func publish(_ value: Bool) {
        guard value != published else { return }
        published = value; changed(value)
    }
    isolated deinit {
        visibilityObservation?.invalidate()
        observations.forEach(NotificationCenter.default.removeObserver)
        if published { changed(false) }
    }
}
#endif

// Native controls own their cursor region, including the separate titlebar
// and popover windows AppKit uses in full screen. No global cursor hide count
// is changed, and the region does not intercept clicks.
struct PlankMacLocalPointerRegion: NSViewRepresentable {
    func makeNSView(context: Context) -> PlankMacLocalPointerView { PlankMacLocalPointerView() }
    func updateNSView(_ view: PlankMacLocalPointerView, context: Context) {}
}
final class PlankMacLocalPointerView: NSView {
    private var tracking: NSTrackingArea?
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func resetCursorRects() { addCursorRect(bounds, cursor: .arrow) }
    override func updateTrackingAreas() {
        if let tracking { removeTrackingArea(tracking) }
        let next = NSTrackingArea(rect: .zero,
            options: [.inVisibleRect, .activeAlways, .cursorUpdate, .mouseEnteredAndExited], owner: self)
        addTrackingArea(next); tracking = next
        super.updateTrackingAreas()
    }
    static func showArrow() {
        NSCursor.setHiddenUntilMouseMoves(false)
        NSCursor.arrow.set()
    }
    static func hideForDesktop() {
        // A transparent cursor image survives crossing into the system menu
        // bar, where this app cannot receive a cursor-update event. Keep a
        // real arrow selected and let AppKit reveal it on the next movement.
        NSCursor.arrow.set()
        NSCursor.setHiddenUntilMouseMoves(true)
    }
    override func cursorUpdate(with event: NSEvent) { Self.showArrow() }
    override func mouseEntered(with event: NSEvent) { Self.showArrow() }
}

// Keep AppKit's mouse/keyboard/accessibility behavior while drawing an opaque
// handle directly, independent of SwiftUI's material rendering.
final class PlankMacSliderCell: NSSliderCell {
    override func drawKnob(_ knobRect: NSRect) {
        let diameter = min(14, min(knobRect.width, knobRect.height))
        let rect = NSRect(x: knobRect.midX - diameter / 2, y: knobRect.midY - diameter / 2,
            width: diameter, height: diameter)
        let knob = NSBezierPath(ovalIn: rect)
        NSColor.white.withAlphaComponent(isEnabled ? 1 : 0.55).setFill(); knob.fill()
        NSColor.black.withAlphaComponent(0.3).setStroke(); knob.lineWidth = 0.75; knob.stroke()
    }
}
final class PlankMacNativeSlider: NSSlider {
    var editingChanged: (Bool) -> Void = { _ in }
    private(set) var mouseTracking = false
    override func mouseDown(with event: NSEvent) {
        PlankMacLocalPointerView.showArrow()
        mouseTracking = true; editingChanged(true)
        defer { mouseTracking = false; editingChanged(false); PlankMacLocalPointerView.showArrow() }
        super.mouseDown(with: event)
    }
    override func resetCursorRects() { addCursorRect(bounds, cursor: .arrow) }
}
struct PlankMacSlider: NSViewRepresentable {
    @Binding var value: Double
    let range: ClosedRange<Double>
    var step: Double = 0
    let label: String
    var editingChanged: (Bool) -> Void = { _ in }

    func makeCoordinator() -> Coordinator { Coordinator(parent: self) }
    func makeNSView(context: Context) -> PlankMacNativeSlider {
        let slider = PlankMacNativeSlider(frame: .zero)
        slider.cell = PlankMacSliderCell()
        slider.sliderType = .linear; slider.isContinuous = true
        slider.target = context.coordinator; slider.action = #selector(Coordinator.changed(_:))
        configure(slider, coordinator: context.coordinator, enabled: context.environment.isEnabled)
        return slider
    }
    func updateNSView(_ slider: PlankMacNativeSlider, context: Context) {
        context.coordinator.parent = self
        configure(slider, coordinator: context.coordinator, enabled: context.environment.isEnabled)
    }
    private func configure(_ slider: PlankMacNativeSlider, coordinator: Coordinator, enabled: Bool) {
        slider.minValue = range.lowerBound; slider.maxValue = range.upperBound
        if !slider.mouseTracking { slider.doubleValue = value }
        slider.isEnabled = enabled; slider.setAccessibilityLabel(label)
        slider.editingChanged = { [weak coordinator] editing in coordinator?.parent.editingChanged(editing) }
    }
    @MainActor final class Coordinator: NSObject {
        var parent: PlankMacSlider
        init(parent: PlankMacSlider) { self.parent = parent }
        @objc func changed(_ slider: PlankMacNativeSlider) {
            var next = min(max(slider.doubleValue, parent.range.lowerBound), parent.range.upperBound)
            if parent.step > 0 { next = parent.range.lowerBound + ((next - parent.range.lowerBound) / parent.step).rounded() * parent.step }
            next = min(max(next, parent.range.lowerBound), parent.range.upperBound)
            slider.doubleValue = next; parent.value = next
            if !slider.mouseTracking { parent.editingChanged(false) }
        }
    }
}
