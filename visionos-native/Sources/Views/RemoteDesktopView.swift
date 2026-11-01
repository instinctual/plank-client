import CoreGraphics
import Combine
import Foundation
import GameController
import QuartzCore
import SwiftUI
import UIKit

@MainActor
private final class PlankDisplayTimingHint: NSObject, ObservableObject {
    @Published private(set) var summary = "Display timing: automatic"
    private var displayLink: CADisplayLink?

    func start(enabled: Bool, prefer96: Bool) {
        // The link exists only for the explicit timing hint. Playback no longer
        // collects and publishes a second set of periodic display statistics.
        let requests96 = enabled && prefer96
        if requests96 && displayLink != nil { return }
        stop()
        guard requests96 else { return }
        summary = "Display timing: 96 Hz requested"
        let link = CADisplayLink(target: self, selector: #selector(tick(_:)))
        link.preferredFrameRateRange = CAFrameRateRange(
            minimum: 96, maximum: 96, preferred: 96
        )
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    func stop() {
        displayLink?.invalidate()
        displayLink = nil
        summary = "Display timing: automatic"
    }

    @objc private func tick(_ link: CADisplayLink) {}
}

struct RemoteDesktopView: View {
    let host: HostBookmark
    @ObservedObject var client: PlankCoreClient
    @Environment(\.dismissWindow) private var dismissWindow
    @Environment(\.scenePhase) private var scenePhase
    @AppStorage("plank.vision.keyboardFunctionKeyMode") private var keyboardFunctionKeyMode = KeyboardFunctionKeyMode.pc.rawValue
    @AppStorage("plank.vision.showStatistics") private var showStatistics = false
    @AppStorage("plank.vision.debugPrefer96Hz") private var debugPrefer96Hz = false
    @AppStorage(PlankAudioPreferences.volumeKey) private var audioVolume = 1.0
    @AppStorage(PlankAudioPreferences.mutedKey) private var audioMuted = false
    @StateObject private var displayTiming = PlankDisplayTimingHint()
    @State private var keyboardFocusGeneration = 0
    @State private var mouseReleaseGeneration = 0
    @State private var mouseTrackingGeneration = 0
    @State private var showingSessionControls = false
    @StateObject private var controlWindows = PlankSessionControlWindows()
    @StateObject private var mouseCapture = PlankMouseCaptureRequest()
    @AppStorage(PlankMouseSensitivity.storageKey) private var mouseSensitivity = PlankMouseSensitivity.defaultValue
    @AppStorage("plank.vision.allowWindowResizing") private var allowWindowResizing = false
    @AppStorage("plank.vision.timingCapture") private var timingCapture = false
    var body: some View {
        ZStack {
            Color.black

            sessionCanvas

            VStack {
                HStack {
                    if showStatistics {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(client.videoDiagnosticText)
                            Text(client.audioDiagnosticText)
                            Text(displayTiming.summary)
                        }
                        .font(.caption2.monospacedDigit())
                        .padding(8)
                        .background(.black.opacity(0.7), in: RoundedRectangle(cornerRadius: 8))
                    }
                    Spacer()
                }
                Spacer()
            }
            .padding(12)
            .allowsHitTesting(false)

            keyboardSurface

            WindowGeometryConfigurator(
                pixelWidth: desktopDimensions.width,
                pixelHeight: desktopDimensions.height,
                allowsUserResize: allowWindowResizing
            )
            .frame(width: 0, height: 0)
            .allowsHitTesting(false)

#if PLANK_TABLET_RELAY
            tabletWaitOverlay
#endif
        }
        .frame(minWidth: 640, maxWidth: 2560,
               minHeight: 640 / desktopAspectRatio,
               maxHeight: 2560 / desktopAspectRatio)
        .ignoresSafeArea()
        .ornament(attachmentAnchor: .scene(.top), contentAlignment:
                    Alignment3D(horizontal: .center, vertical: .sessionControlsAttachment, depth: .back)) {
            sessionControlsOrnament
        }
        .onAppear {
            mouseCapture.beginDesktop()
            applyAudioVolume()
            PlankAudioOutput.shared.setSceneActive(scenePhase != .background)
            client.setVideoDiagnosticsEnabled(showStatistics)
            PlankTimingCapture.shared.setEnabled(timingCapture)
            client.setTabletActive(scenePhase == .active)
            updateDisplayTiming()
            requestKeyboardFocus()
        }
        .onChange(of: showStatistics) { _, enabled in
            client.setVideoDiagnosticsEnabled(enabled)
            updateDisplayTiming()
        }
        .onChange(of: debugPrefer96Hz) { _, _ in updateDisplayTiming() }
        .onChange(of: showingSessionControls) { _, shown in
            NSLog("PLANK session controls: %@", shown ? "opened" : "closed")
            if shown { mouseReleaseGeneration &+= 1 }
            else { requestKeyboardFocus() }
        }
        .onChange(of: audioVolume) { _, _ in applyAudioVolume() }
        .onChange(of: audioMuted) { _, _ in applyAudioVolume() }
        .onChange(of: scenePhase) { _, phase in handleScenePhase(phase) }
#if PLANK_TABLET_RELAY
        .onChange(of: client.waitingForTablet) { _, waiting in
            if waiting { mouseReleaseGeneration &+= 1 }
        }
#endif
        .onChange(of: client.phase) { _, phase in
            if !isSessionActive(phase) {
                dismissWindow(id: "plank-desktop")
            }
        }
        .onDisappear {
            mouseCapture.endDesktop()
            client.setVideoDiagnosticsEnabled(false)
            displayTiming.stop()
            // Closing the spatial window must end its stream too. Otherwise
            // the Host keeps a live reservation behind a vanished window.
            if client.activeHostID == host.id && isSessionActive(client.phase) {
                client.disconnectSession()
            }
        }
    }

    private var desktopDimensions: PlankFrameDimensions {
        // Use the requested composite before startup, then the authenticated
        // Host canvas. Display 1's aspect ratio must not squeeze Display 2.
        if let topology = client.sessionTopology {
            return .init(width: topology.desktopWidth, height: topology.desktopHeight)
        }
        return host.desktopDimensions
    }

    private var desktopAspectRatio: CGFloat {
        CGFloat(max(1, desktopDimensions.width)) / CGFloat(max(1, desktopDimensions.height))
    }

    private var sessionCanvas: some View {
            GeometryReader { proxy in
                ZStack {
                    VideoSurface(client: client)
                    if let frame = client.frameDimensions {
                        RemoteInputSurface(
                            releaseGeneration: mouseReleaseGeneration,
                            trackingGeneration: mouseTrackingGeneration,
                            controlsPresented: showingSessionControls,
                            mouseSensitivity: mouseSensitivity,
                            controlWindows: controlWindows,
                            sceneInBackground: scenePhase == .background,
                            sceneIsActive: scenePhase == .active,
                            mouseCapture: mouseCapture,
                            frameDimensions: frame,
                            pointerPosition: { client.currentPointerPosition() },
                            onPointer: { location in
                                guard !showingSessionControls else { return }
                                sendPointer(location, in: proxy.size, frame: frame)
                            },
                            onButton: { number, pressed in
                                guard !showingSessionControls || !pressed else { return }
                                client.setMouseButton(number: number, pressed: pressed)
                                // Taking keyboard focus during mouse-down can cancel
                                // the gesture before its matching mouse-up arrives.
                                if !pressed && !showingSessionControls { requestKeyboardFocus() }
                            },
                            onScroll: { vertical, horizontal in
                                guard !showingSessionControls else { return }
                                requestKeyboardFocus()
                                client.scroll(vertical: vertical, horizontal: horizontal)
                            },
                            onWindowFocus: { effects in
                                // Mac Virtual Display can change the key
                                // window without a SwiftUI scene-phase change.
                                // PLANK's own session controls keep the tablet.
                                if let active = effects.tabletActive {
                                    client.setTabletActive(active)
                                }
                                if effects.releaseMouse { mouseReleaseGeneration &+= 1 }
                                if effects.reacquireDesktop { requestKeyboardFocus() }
                            }
                        )
                    } else {
                        VStack(spacing: 18) {
                            ProgressView()
                            Text("Starting secure stream from \(host.name)…")
                                .foregroundStyle(.secondary)
                        }
                    }
                }
            }

    }

    private var keyboardSurface: some View {
            KeyboardCapture(
                focusGeneration: keyboardFocusGeneration,
                functionKeyMode: KeyboardFunctionKeyMode(rawValue: keyboardFunctionKeyMode) ?? .pc,
                onCharacters: { characters in
                    guard !showingSessionControls else { return }
                    handleKeyboardCharacters(characters)
                },
                onKeyEvent: { code, pressed, modifiers in
                    guard !showingSessionControls || !pressed else { return }
                    client.sendKey(code: code, pressed: pressed, modifiers: modifiers)
                }
            )
            .allowsHitTesting(false)

    }

#if PLANK_TABLET_RELAY
    @ViewBuilder private var tabletWaitOverlay: some View {
            if client.waitingForTablet {
                if client.showingTabletWaitScreen {
                    Color.black
                        .ignoresSafeArea()
                    VStack(spacing: 18) {
                        ProgressView()
                        Text("Connecting Wacom tablet…")
                            .font(.title2.weight(.semibold))
                        Text(client.tabletRelayStatus)
                            .font(.body)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                        Text("Mouse, keyboard, and pen input will start when the workstation confirms the tablet is ready.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                        HStack(spacing: 14) {
                            Button("Continue without Wacom") {
                                client.continueWithoutTablet()
                            }
                            Button("Disconnect", role: .cancel) {
                                client.disconnectSession()
                                dismissWindow(id: "plank-desktop")
                            }
                        }
                        .buttonStyle(.bordered)
                    }
                    .padding(32)
                    .frame(maxWidth: 580)
                } else {
                    VStack {
                        HStack {
                            Spacer()
                            VStack(alignment: .leading, spacing: 8) {
                                ProgressView("Checking Wacom Relay…")
                                Text(client.tabletRelayStatus)
                                    .font(.footnote)
                                    .foregroundStyle(.secondary)
                                Button("Continue without Wacom") {
                                    client.continueWithoutTablet()
                                }
                                .buttonStyle(.bordered)
                            }
                            .padding(14)
                            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
                            .frame(maxWidth: 380)
                        }
                        Spacer()
                    }
                    .padding(20)
                }
            }
    }
#endif

    private func handleScenePhase(_ phase: ScenePhase) {
            // Audio follows visibility, not focus: looking at another window
            // keeps the stream audible.
            PlankAudioOutput.shared.setSceneActive(phase != .background)
            if phase == .active {
                client.setTabletActive(true)
                mouseTrackingGeneration &+= 1
                requestKeyboardFocus()
            } else {
                mouseReleaseGeneration &+= 1
                // An ornament can move keyboard focus to its own UIWindow
                // without moving away from this session. The input coordinator
                // evaluates the registered window instead of treating every
                // inactive notification as a departure.
                if phase == .background {
                    showingSessionControls = false
                    client.setTabletActive(false)
                }
            }
    }

    private var sessionControlsOrnament: some View {
            // Anchor at the button, independent of the expanded menu height.
            // The whole menu participates in layout and hit testing; an overlay
            // outside a button-sized ornament would not own its full surface.
            VStack(spacing: 8) {
                sessionControlsButton
                if showingSessionControls {
                    PlankSessionControls(
                        client: client, volume: $audioVolume, muted: $audioMuted,
                        allowWindowResizing: $allowWindowResizing,
                        mouseSensitivity: $mouseSensitivity,
                        onDone: { showingSessionControls = false }
                    )
                    .glassBackgroundEffect()
                }
            }
            .alignmentGuide(.sessionControlsAttachment) { _ in 52 }
            .background(SessionControlWindowMarker(owner: controlWindows))
    }

    /// One compact button; the controls themselves stay hidden until opened.
    private var sessionControlsButton: some View {
        Button {
            showingSessionControls.toggle()
        } label: {
            Image(systemName: audioMuted ? "speaker.slash" : "slider.horizontal.3")
                .font(.footnote)
                .frame(width: 20, height: 20)
        }
        .buttonStyle(.borderless)
        .padding(8)
        .glassBackgroundEffect()
        .accessibilityLabel("Session controls")
    }

    private func applyAudioVolume() {
        PlankAudioOutput.shared.setVolume(Float(audioVolume), muted: audioMuted)
    }

    private func updateDisplayTiming() {
        displayTiming.start(
            enabled: showStatistics,
            prefer96: debugPrefer96Hz && (host.streamFrameRate == 24 || host.streamFrameRate == 48)
        )
    }

    private func isSessionActive(_ phase: ConnectionPhase) -> Bool {
        switch phase {
        case .startingSession, .frameReceived, .streaming:
            return true
        default:
            return false
        }
    }

    private func requestKeyboardFocus() {
        keyboardFocusGeneration &+= 1
    }

    private func sendPointer(
        _ location: CGPoint,
        in availableSize: CGSize,
        frame: PlankFrameDimensions
    ) {
        guard let point = PlankDesktopCanvas(dimensions: frame).remotePoint(location, in: availableSize) else { return }
        client.movePointer(x: point.x, y: point.y, width: frame.width, height: frame.height)
    }

    private func handleKeyboardCharacters(_ characters: String) {
        for character in characters {
            switch character {
            case "\r", "\n":
                client.pressKey(code: 0x0D)
            case "\t":
                client.pressKey(code: 0x09)
            case "\u{8}", "\u{7f}":
                client.pressKey(code: 0x08)
            case "\u{1b}":
                client.pressKey(code: 0x1B)
            default:
                if let key = physicalKey(for: character) {
                    client.pressKey(code: key.code, modifiers: key.shifted ? 0x01 : 0)
                } else {
                    client.sendText(String(character))
                }
            }
        }
    }

    private func physicalKey(for character: Character) -> (code: UInt16, shifted: Bool)? {
        if let ascii = character.asciiValue {
            if ascii >= Character("a").asciiValue!, ascii <= Character("z").asciiValue! {
                return (UInt16(ascii - Character("a").asciiValue! + 0x41), false)
            }
            if ascii >= Character("A").asciiValue!, ascii <= Character("Z").asciiValue! {
                return (UInt16(ascii - Character("A").asciiValue! + 0x41), true)
            }
            if ascii >= Character("0").asciiValue!, ascii <= Character("9").asciiValue! {
                return (UInt16(ascii), false)
            }
        }
        switch character {
        case " ": return (0x20, false)
        case "!": return (0x31, true)
        case "@": return (0x32, true)
        case "#": return (0x33, true)
        case "$": return (0x34, true)
        case "%": return (0x35, true)
        case "^": return (0x36, true)
        case "&": return (0x37, true)
        case "*": return (0x38, true)
        case "(": return (0x39, true)
        case ")": return (0x30, true)
        case ";": return (0xBA, false)
        case ":": return (0xBA, true)
        case "=": return (0xBB, false)
        case "+": return (0xBB, true)
        case ",": return (0xBC, false)
        case "<": return (0xBC, true)
        case "-": return (0xBD, false)
        case "_": return (0xBD, true)
        case ".": return (0xBE, false)
        case ">": return (0xBE, true)
        case "/": return (0xBF, false)
        case "?": return (0xBF, true)
        case "`": return (0xC0, false)
        case "~": return (0xC0, true)
        case "[": return (0xDB, false)
        case "{": return (0xDB, true)
        case "\\": return (0xDC, false)
        case "|": return (0xDC, true)
        case "]": return (0xDD, false)
        case "}": return (0xDD, true)
        case "'": return (0xDE, false)
        case "\"": return (0xDE, true)
        default: return nil
        }
    }

}

private struct VideoSurface: UIViewRepresentable {
    let client: PlankCoreClient

    func makeCoordinator() -> Coordinator { Coordinator(client: client) }

    func makeUIView(context: Context) -> VideoSurfaceView {
        let view = VideoSurfaceView(frame: .zero)
        client.registerVideoSurface(
            id: context.coordinator.id,
            frame: { [weak view] in view?.display($0) },
            cursor: { [weak view] in view?.displayCursor($0) },
            cursorShape: { [weak view] in view?.displayCursorShape($0) }
        )
        return view
    }

    func updateUIView(_ view: VideoSurfaceView, context: Context) {}

    static func dismantleUIView(_ view: VideoSurfaceView, coordinator: Coordinator) {
        coordinator.client.unregisterVideoSurface(id: coordinator.id)
    }

    @MainActor
    final class Coordinator {
        let id = UUID()
        let client: PlankCoreClient

        init(client: PlankCoreClient) { self.client = client }
    }
}

private final class VideoSurfaceView: UIView {
    private var dimensions: PlankFrameDimensions?
    private var cursor: PlankRemoteCursor?
    private var cursorShape: PlankRemoteCursorShape?
    private let cursorLayer = CALayer()
    private let cursorFallback = CALayer()
    private let metalVideo = PlankMetalVideoView(frame: .zero)

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        isOpaque = true
        isUserInteractionEnabled = false
        layer.contentsGravity = .resizeAspect
        addSubview(metalVideo)
        metalVideo.isHidden = true
        cursorLayer.contentsGravity = .resize
        cursorLayer.isHidden = true
        layer.addSublayer(cursorLayer)
        cursorFallback.bounds = CGRect(x: 0, y: 0, width: 12, height: 12)
        cursorFallback.cornerRadius = 6
        cursorFallback.borderWidth = 2
        cursorFallback.borderColor = UIColor.cyan.cgColor
        cursorFallback.isHidden = true
        layer.addSublayer(cursorFallback)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    func display(_ frame: PlankRenderedFrame?) {
        if let frame, let pixelBuffer = frame.pixelBuffer, metalVideo.canRender {
            dimensions = PlankFrameDimensions(width: frame.width, height: frame.height)
            layoutVideo()
            layer.contents = nil
            metalVideo.isHidden = false
            metalVideo.display(pixelBuffer)
            layoutCursor()
            return
        }
        metalVideo.isHidden = true
        guard let frame,
              let provider = CGDataProvider(data: frame.pixels as CFData),
              let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else {
            layer.contents = nil
            dimensions = nil
            cursorLayer.isHidden = true
            cursorFallback.isHidden = true
            return
        }
        let bitmapInfo = CGBitmapInfo.byteOrder32Little.union(
            CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue)
        )
        guard let image = CGImage(
            width: frame.width,
            height: frame.height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: frame.bytesPerRow,
            space: colorSpace,
            bitmapInfo: bitmapInfo,
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        ) else { return }
        dimensions = PlankFrameDimensions(width: frame.width, height: frame.height)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layer.contents = image
        layoutCursor()
        CATransaction.commit()
    }

    func displayCursor(_ cursor: PlankRemoteCursor?) {
        self.cursor = cursor
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layoutCursor()
        CATransaction.commit()
    }

    func displayCursorShape(_ shape: PlankRemoteCursorShape?) {
        cursorShape = shape
        cursorLayer.contents = nil
        if let shape,
           let provider = CGDataProvider(data: shape.pixels as CFData),
           let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) {
            let bitmapInfo = CGBitmapInfo.byteOrder32Little.union(
                CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue)
            )
            cursorLayer.contents = CGImage(
                width: shape.width,
                height: shape.height,
                bitsPerComponent: 8,
                bitsPerPixel: 32,
                bytesPerRow: shape.width * 4,
                space: colorSpace,
                bitmapInfo: bitmapInfo,
                provider: provider,
                decode: nil,
                shouldInterpolate: false,
                intent: .defaultIntent
            )
        }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layoutCursor()
        CATransaction.commit()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        layoutVideo()
        layoutCursor()
    }

    private func layoutVideo() {
        guard let dimensions, bounds.width > 0, bounds.height > 0 else {
            metalVideo.frame = bounds
            return
        }
        metalVideo.frame = PlankDesktopCanvas(dimensions: dimensions).rect(in: bounds.size)
    }

    private func layoutCursor() {
        guard let dimensions, let cursor,
              dimensions.width == cursor.frameWidth,
              dimensions.height == cursor.frameHeight,
              bounds.width > 0, bounds.height > 0 else {
            cursorLayer.isHidden = true
            cursorFallback.isHidden = true
            return
        }
        let canvas = PlankDesktopCanvas(dimensions: dimensions).rect(in: bounds.size)
        let scale = canvas.width / CGFloat(dimensions.width)
        let pointer = CGPoint(
            x: canvas.minX + CGFloat(cursor.x) * scale,
            y: canvas.minY + CGFloat(cursor.y) * scale
        )
        guard let cursorShape else {
            cursorLayer.isHidden = true
            cursorFallback.position = pointer
            cursorFallback.isHidden = false
            return
        }
        cursorFallback.isHidden = true
        guard cursorShape.visible, cursorLayer.contents != nil else {
            cursorLayer.isHidden = true
            return
        }
        // Limit the minimum size to half the Host cursor. Full-size Host
        // artwork overwhelms small controls in a spatial desktop window.
        let cursorScale = min(max(scale, 0.5), 1)
        cursorLayer.frame = CGRect(
            x: pointer.x - CGFloat(cursorShape.hotspotX) * cursorScale,
            y: pointer.y - CGFloat(cursorShape.hotspotY) * cursorScale,
            width: CGFloat(cursorShape.width) * cursorScale,
            height: CGFloat(cursorShape.height) * cursorScale
        )
        cursorLayer.isHidden = false
    }
}

private enum SessionControlsAttachment: AlignmentID {
    static func defaultValue(in context: ViewDimensions) -> CGFloat { context[.top] + 52 }
}

private extension VerticalAlignment {
    static let sessionControlsAttachment = VerticalAlignment(SessionControlsAttachment.self)
}

/// Explicit ownership of the ornament's UIKit window. visionOS may host it
/// outside the desktop's UIWindowScene; scene equality is not an ownership test.
@MainActor
private final class PlankSessionControlWindows: ObservableObject {
    private final class WeakWindow {
        weak var window: UIWindow?
        init(_ window: UIWindow) { self.window = window }
    }
    private var windows: [UUID: WeakWindow] = [:]
    @Published private(set) var generation = 0
    private var publicationScheduled = false

    var hasKeyWindow: Bool {
        // Only a registered window can prove controls ownership. A key
        // sibling in its scene can be the desktop itself, not the controls.
        windows.values.contains { $0.window?.isKeyWindow == true }
    }

    func register(_ newWindow: UIWindow?, marker: UUID) {
        guard windows[marker]?.window !== newWindow else { return }
        if let newWindow { windows[marker] = WeakWindow(newWindow) }
        else { windows.removeValue(forKey: marker) }
        // didMoveToWindow may run inside a SwiftUI update; notify observers
        // after that update, coalescing register/deregister in the same pass.
        if !publicationScheduled {
            publicationScheduled = true
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.publicationScheduled = false
                self.generation &+= 1
            }
        }
        NSLog("PLANK session controls window: attached=%d key=%d", newWindow != nil ? 1 : 0,
              newWindow?.isKeyWindow == true ? 1 : 0)
    }
}

private struct SessionControlWindowMarker: UIViewRepresentable {
    let owner: PlankSessionControlWindows

    func makeUIView(context: Context) -> Marker {
        let view = Marker()
        view.owner = owner
        view.isUserInteractionEnabled = false
        return view
    }

    func updateUIView(_ view: Marker, context: Context) {
        view.owner = owner
        owner.register(view.window, marker: view.identifier)
    }

    static func dismantleUIView(_ view: Marker, coordinator: ()) {
        view.owner?.register(nil, marker: view.identifier)
        view.owner = nil
    }

    final class Marker: UIView {
        weak var owner: PlankSessionControlWindows?
        let identifier = UUID()
        override func didMoveToWindow() {
            super.didMoveToWindow()
            owner?.register(window, marker: identifier)
        }
    }
}

@MainActor
private final class RemoteInputController: UIViewController {
    var didAttach: (() -> Void)?
    private var lockRequested = false
    private var requestLogBudget = 4
    override var prefersPointerLocked: Bool { lockRequested }

    override func loadView() {
        let input = AttachedInputView()
        input.didAttach = { [weak self] in self?.didAttach?() }
        view = input
    }
    func updatePointerLockRequest(_ requested: Bool) {
        guard lockRequested != requested else { return }
        lockRequested = requested
        NSLog("PLANK mouse capture preference: %d", requested ? 1 : 0)
        var controller: UIViewController? = self
        var chain: [String] = []
        while let current = controller {
            current.setNeedsUpdateOfPrefersPointerLocked()
            if requested && requestLogBudget > 0 {
                chain.append("\(type(of: current)):prefers=\(current.prefersPointerLocked),child=\(current.childViewControllerForPointerLock != nil)")
            }
            controller = current.parent
        }
        if !chain.isEmpty {
            requestLogBudget -= 1
            NSLog("PLANK mouse capture controller chain: %@", chain.joined(separator: " -> "))
        }
    }
    private final class AttachedInputView: UIView {
        var didAttach: (() -> Void)?
        override func didMoveToWindow() { super.didMoveToWindow(); didAttach?() }
    }
}

private struct RemoteInputSurface: UIViewControllerRepresentable {
    let releaseGeneration: Int
    let trackingGeneration: Int
    let controlsPresented: Bool
    let mouseSensitivity: Double
    @ObservedObject var controlWindows: PlankSessionControlWindows
    let sceneInBackground: Bool
    let sceneIsActive: Bool
    @ObservedObject var mouseCapture: PlankMouseCaptureRequest
    let frameDimensions: PlankFrameDimensions
    let pointerPosition: @MainActor () -> (x: Int, y: Int)?
    let onPointer: @MainActor (CGPoint) -> Void
    let onButton: @MainActor (UInt8, Bool) -> Void
    let onScroll: @MainActor (Int16, Int16) -> Void
    let onWindowFocus: @MainActor (PlankSessionFocus.Effects) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(
            onPointer: onPointer,
            onButton: onButton,
            onScroll: onScroll,
            onWindowFocus: onWindowFocus
        )
    }

    func makeUIViewController(context: Context) -> RemoteInputController {
        let controller = RemoteInputController()
        let view = controller.view!
        context.coordinator.inputController = controller
        controller.didAttach = { [weak coordinator = context.coordinator] in
            coordinator?.observePointerLock()
        }
        context.coordinator.mouseSensitivity = PlankMouseSensitivity.normalized(mouseSensitivity)
        context.coordinator.mouseCapture = mouseCapture
        context.coordinator.frameDimensions = frameDimensions
        context.coordinator.pointerPosition = pointerPosition
        view.backgroundColor = .clear
        view.isOpaque = false

        let hover = UIHoverGestureRecognizer(
            target: context.coordinator,
            action: #selector(Coordinator.handleHover(_:))
        )
        hover.delegate = context.coordinator
        view.addGestureRecognizer(hover)
        view.addInteraction(UIPointerInteraction(delegate: context.coordinator))

        let mousePress = PhysicalMousePressRecognizer()
        mousePress.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]
        mousePress.cancelsTouchesInView = false
        mousePress.delegate = context.coordinator
        mousePress.onPointer = { [weak coordinator = context.coordinator] location in
            coordinator?.absolutePointer(location)
        }
        mousePress.onButton = { [weak coordinator = context.coordinator] button, pressed in
            coordinator?.absoluteButton(button, pressed)
        }
        view.addGestureRecognizer(mousePress)
        context.coordinator.inputView = view
        context.coordinator.controlsPresented = controlsPresented
        context.coordinator.controlWindows = controlWindows
        context.coordinator.sceneInBackground = sceneInBackground
        context.coordinator.sceneIsActive = sceneIsActive
        context.coordinator.beginMouseMotionTracking()

        let wheel = WheelCaptureScrollView(frame: .zero)
        wheel.backgroundColor = .clear
        wheel.isOpaque = false
        wheel.showsVerticalScrollIndicator = false
        wheel.showsHorizontalScrollIndicator = false
        wheel.contentInsetAdjustmentBehavior = .never
        wheel.bounces = false
        wheel.panGestureRecognizer.allowedTouchTypes = []
        wheel.panGestureRecognizer.allowedScrollTypesMask = .discrete
        wheel.delegate = context.coordinator
        wheel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(wheel)
        NSLayoutConstraint.activate([
            wheel.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            wheel.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            wheel.topAnchor.constraint(equalTo: view.topAnchor),
            wheel.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])

        return controller
    }

    func updateUIViewController(_ controller: RemoteInputController, context: Context) {
        let view = controller.view!
        context.coordinator.mouseSensitivity = PlankMouseSensitivity.normalized(mouseSensitivity)
        context.coordinator.mouseCapture = mouseCapture
        context.coordinator.frameDimensions = frameDimensions
        context.coordinator.pointerPosition = pointerPosition
        context.coordinator.onPointer = onPointer
        context.coordinator.onButton = onButton
        context.coordinator.onScroll = onScroll
        context.coordinator.onWindowFocus = onWindowFocus
        context.coordinator.controlWindows = controlWindows
        context.coordinator.sceneInBackground = sceneInBackground
        context.coordinator.sceneIsActive = sceneIsActive
        // Register/deregister of an ornament window is a focus event too.
        context.coordinator.scheduleFocusEvaluation()
        if context.coordinator.controlsPresented != controlsPresented {
            let controlsClosed = !controlsPresented
            context.coordinator.controlsPresented = controlsPresented
            if controlsClosed { context.coordinator.returnFocusFromControls() }
            context.coordinator.scheduleFocusEvaluation()
            for interaction in view.interactions {
                (interaction as? UIPointerInteraction)?.invalidate()
            }
        }
        if context.coordinator.trackingGeneration != trackingGeneration {
            context.coordinator.trackingGeneration = trackingGeneration
            context.coordinator.refreshMouseMotionTracking()
        }
        if context.coordinator.releaseGeneration != releaseGeneration {
            context.coordinator.releaseGeneration = releaseGeneration
            context.coordinator.releaseButtons()
            for recognizer in view.gestureRecognizers ?? [] {
                (recognizer as? PhysicalMousePressRecognizer)?.releaseIfNeeded()
            }
        }
        context.coordinator.updateCaptureRequest()
    }

    static func dismantleUIViewController(_ controller: RemoteInputController, coordinator: Coordinator) {
        controller.updatePointerLockRequest(false)
        let view = controller.view!
        coordinator.stopMouseMotionTracking()
        for recognizer in view.gestureRecognizers ?? [] {
            (recognizer as? PhysicalMousePressRecognizer)?.releaseIfNeeded()
        }
    }

    @MainActor
    final class Coordinator: NSObject, UIGestureRecognizerDelegate, UIScrollViewDelegate, UIPointerInteractionDelegate {
        var onPointer: @MainActor (CGPoint) -> Void
        var onButton: @MainActor (UInt8, Bool) -> Void
        var onScroll: @MainActor (Int16, Int16) -> Void
        var onWindowFocus: @MainActor (PlankSessionFocus.Effects) -> Void
        var releaseGeneration = 0
        var trackingGeneration = 0
        weak var inputView: UIView?
        weak var inputController: RemoteInputController?
        weak var mouseCapture: PlankMouseCaptureRequest?
        var frameDimensions = PlankFrameDimensions(width: 1, height: 1)
        var pointerPosition: (@MainActor () -> (x: Int, y: Int)?)?
        private var capturePolicy = PlankMouseCapturePolicy()
        private var capturedPosition = PlankCapturedMousePosition()
        private var heldButtons = PlankMouseButtons()
        private var pointerLockObserver: NSObjectProtocol?
        private var lastRawMovement = 0.0
        private var lastAbsoluteMovement = 0.0
        private var rawLogAt = 0.0
        private var rawLogBudget = 16
        private var rawCount = 0
        private var absoluteCount = 0
        private var mouseObservers: [NSObjectProtocol] = []
        private var trackedMice: [GCMouse] = []
        private var mouseGeneration = 0
        private var sawPhysicalMouseMotion = false
        private var lastPhysicalMouseMotion = 0.0
        var controlsPresented = false
        var mouseSensitivity = PlankMouseSensitivity.defaultValue
        weak var controlWindows: PlankSessionControlWindows?
        var sceneInBackground = false
        var sceneIsActive = false
        private var pointerRequestLocation: CGPoint?
        private var pointerRequestLogBudget = 8
        private var focus = PlankSessionFocus.desktop
        private var focusEvaluationScheduled = false
        private var departureCheck: DispatchWorkItem?
        private var focusLogBudget = 200

        init(
            onPointer: @escaping @MainActor (CGPoint) -> Void,
            onButton: @escaping @MainActor (UInt8, Bool) -> Void,
            onScroll: @escaping @MainActor (Int16, Int16) -> Void,
            onWindowFocus: @escaping @MainActor (PlankSessionFocus.Effects) -> Void
        ) {
            self.onPointer = onPointer
            self.onButton = onButton
            self.onScroll = onScroll
            self.onWindowFocus = onWindowFocus
        }

        func beginMouseMotionTracking() {
            trackMice()
            let center = NotificationCenter.default
            for name in [NSNotification.Name.GCMouseDidConnect, NSNotification.Name.GCMouseDidDisconnect,
                         NSNotification.Name.GCMouseDidBecomeCurrent] {
                mouseObservers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                    MainActor.assumeIsolated { self?.trackMice() }
                })
            }
            mouseObservers.append(center.addObserver(
                forName: NSNotification.Name.GCMouseDidStopBeingCurrent, object: nil, queue: .main
            ) { [weak self] note in
                guard let mouse = note.object as? GCMouse else { return }
                let identifier = ObjectIdentifier(mouse)
                MainActor.assumeIsolated {
                    guard let self,
                          let source = self.trackedMice.firstIndex(where: { ObjectIdentifier($0) == identifier }),
                          self.heldButtons.isHolding, self.heldButtons.accepts(source: source + 1) else { return }
                    self.releaseButtons()
                    self.capturedPosition.reset()
                }
            })
            for name in [UIScene.willDeactivateNotification, UIScene.didEnterBackgroundNotification] {
                mouseObservers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] note in
                    guard let scene = note.object as? UIWindowScene else { return }
                    MainActor.assumeIsolated {
                        guard let self, scene === self.inputView?.window?.windowScene else { return }
                        self.releaseButtons()
                        self.capturedPosition.reset()
                    }
                })
            }
            // Any key-window change may move focus between the desktop, its
            // own session controls (another window in the same scene) and
            // elsewhere. Evaluate once both halves of a handoff are posted.
            for name in [UIWindow.didBecomeKeyNotification, UIWindow.didResignKeyNotification] {
                mouseObservers.append(center.addObserver(
                    forName: name, object: nil, queue: .main
                ) { [weak self] note in
                    guard let window = note.object as? UIWindow else { return }
                    let resigned = note.name == UIWindow.didResignKeyNotification
                    MainActor.assumeIsolated {
                        guard let self else { return }
                        if resigned && window === self.inputView?.window {
                            self.releaseButtons()
                            self.capturedPosition.reset()
                        }
                        self.scheduleFocusEvaluation()
                    }
                })
            }
        }

        func returnFocusFromControls() {
            // Only an explicit close of this session's controls may reclaim
            // its desktop. Never make the window key on arbitrary raw motion.
            DispatchQueue.main.async { [weak self] in
                guard let self, let window = self.inputView?.window,
                      PlankSessionFocus.shouldReclaimDesktop(
                        controlsPresented: self.controlsPresented,
                        sceneActive: window.windowScene?.activationState == .foregroundActive,
                        background: self.sceneInBackground,
                        desktopIsKey: window.isKeyWindow,
                        ownedControlsAreKey: self.controlWindows?.hasKeyWindow == true) else { return }
                window.makeKey()
                NSLog("PLANK focus: returned desktop key window after closing controls")
                self.scheduleFocusEvaluation()
                self.updateCaptureRequest()
                // The controls used the system pointer style. Refresh only
                // after the desktop is key, so that cached circle is replaced
                // by this canvas's hidden style.
                self.invalidatePointerAppearance()
            }
        }

        private func invalidatePointerAppearance() {
            for interaction in inputView?.interactions ?? [] {
                (interaction as? UIPointerInteraction)?.invalidate()
            }
        }

        func scheduleFocusEvaluation() {
            guard !focusEvaluationScheduled else { return }
            focusEvaluationScheduled = true
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    self.focusEvaluationScheduled = false
                    self.evaluateFocus(confirmed: false)
                }
            }
        }

        private func currentFocus() -> (PlankSessionFocus, String) {
            guard let inputWindow = inputView?.window else { return (.away, "no window") }
            if sceneInBackground { return (.away, "background") }
            let keyWindow = inputWindow.windowScene?.windows.first(where: \.isKeyWindow)
            let ownedControlsAreKey = controlWindows?.hasKeyWindow == true
            let observed = PlankSessionFocus.classify(
                desktopWindowIsKey: inputWindow.isKeyWindow,
                sceneHasKeyWindow: keyWindow != nil,
                controlsPresented: controlsPresented,
                registeredControlsAreKey: ownedControlsAreKey
            )
            return (observed, ownedControlsAreKey ? "registered session controls" :
                    keyWindow.map { String(describing: type(of: $0)) } ?? "none")
        }

        private func evaluateFocus(confirmed: Bool) {
            let (observed, keyWindow) = currentFocus()
            if PlankSessionFocus.needsConfirmation(from: focus, to: observed) && !confirmed {
                guard departureCheck == nil else { return }
                let check = DispatchWorkItem { [weak self] in
                    MainActor.assumeIsolated {
                        self?.departureCheck = nil
                        self?.evaluateFocus(confirmed: true)
                    }
                }
                departureCheck = check
                DispatchQueue.main.asyncAfter(
                    deadline: .now() + PlankSessionFocus.departureConfirmation, execute: check
                )
                return
            }
            departureCheck?.cancel()
            departureCheck = nil
            let previous = focus
            guard observed != previous else { return }
            focus = observed
            if focusLogBudget > 0 {
                focusLogBudget -= 1
                NSLog("PLANK focus: %@ -> %@ (key window %@)",
                      String(describing: previous), String(describing: observed), keyWindow)
            }
            if observed == .desktop { refreshMouseMotionTracking() }
            updateCaptureRequest()
            onWindowFocus(PlankSessionFocus.effects(from: previous, to: observed))
        }

        func refreshMouseMotionTracking() {
            // Do not reinstall an unchanged profile set mid-drag. visionOS
            // exposes several profiles; the one producing input is not always
            // GCMouse.current or the first inventory entry.
            trackMice()
        }

        func stopMouseMotionTracking() {
            releaseButtons()
            if let pointerLockObserver { NotificationCenter.default.removeObserver(pointerLockObserver) }
            pointerLockObserver = nil
            departureCheck?.cancel()
            departureCheck = nil
            mouseGeneration &+= 1
            for mouse in trackedMice { clearRawHandlers(mouse) }
            trackedMice.removeAll()
            for observer in mouseObservers {
                NotificationCenter.default.removeObserver(observer)
            }
            mouseObservers.removeAll()
        }

        private func clearRawHandlers(_ mouse: GCMouse) {
            let input = mouse.mouseInput
            input?.mouseMovedHandler = nil
            input?.leftButton.pressedChangedHandler = nil
            input?.rightButton?.pressedChangedHandler = nil
            input?.middleButton?.pressedChangedHandler = nil
            input?.scroll.valueChangedHandler = nil
        }

        private func trackMice() {
            var discovered = GCMouse.mice()
            if let current = GCMouse.current, !discovered.contains(where: { $0 === current }) {
                discovered.append(current)
            }
            guard Set(discovered.map(ObjectIdentifier.init)) != Set(trackedMice.map(ObjectIdentifier.init)) else {
                updateCaptureRequest()
                return
            }
            releaseButtons()
            capturedPosition.reset()
            for mouse in trackedMice { clearRawHandlers(mouse) }
            trackedMice = discovered
            mouseGeneration &+= 1
            let generation = mouseGeneration
            NSLog("PLANK raw mouse inventory: devices=%d profiles=%d",
                  trackedMice.count, trackedMice.filter { $0.mouseInput != nil }.count)
            for (index, mouse) in trackedMice.enumerated() {
                guard let input = mouse.mouseInput else { continue }
                let source = index + 1
                input.mouseMovedHandler = { [weak self] _, x, y in
                    DispatchQueue.main.async { [weak self] in
                        guard let self, self.mouseGeneration == generation else { return }
                        self.physicalMovement(x: Double(x), y: Double(y), source: source)
                    }
                }
                let buttons: [(GCControllerButtonInput?, UInt8)] = [
                    (input.leftButton, 1), (input.rightButton, 3), (input.middleButton, 2)
                ]
                for (buttonInput, button) in buttons {
                    buttonInput?.pressedChangedHandler = { [weak self] _, _, pressed in
                        DispatchQueue.main.async { [weak self] in
                            guard let self, self.mouseGeneration == generation else { return }
                            guard self.canForwardRawInput else { self.releaseButtons(); return }
                            guard self.heldButtons.accepts(source: source) else { return }
                            if pressed && !self.heldButtons.isHolding &&
                                (self.capturedPosition.point == nil || CACurrentMediaTime() - self.lastRawMovement > 0.15) {
                                self.anchorFromHost()
                                if let point = self.capturedPosition.point { self.onPointer(point) }
                            }
                            self.forwardButton(button, pressed, source: source)
                        }
                    }
                }
                input.scroll.valueChangedHandler = { [weak self] _, x, y in
                    DispatchQueue.main.async { [weak self] in
                        guard let self, self.mouseGeneration == generation,
                              self.canForwardRawInput, self.heldButtons.accepts(source: source),
                              x.isFinite, y.isFinite else { return }
                        let vertical = Int16(max(-12, min(12, Double(y).rounded()))) * 120
                        let horizontal = Int16(max(-12, min(12, Double(x).rounded()))) * 120
                        if vertical != 0 || horizontal != 0 { self.onScroll(vertical, horizontal) }
                    }
                }
            }
            updateCaptureRequest()
        }

        private var canForwardRawInput: Bool {
            capturePolicy.usesRawInput && inputView?.window?.isKeyWindow == true &&
                inputView?.window?.windowScene?.activationState == .foregroundActive &&
                !controlsPresented && currentFocus().0 == .desktop
        }

        private var canvasBounds: CGRect {
            guard let view = inputView else { return .zero }
            return PlankDesktopCanvas(dimensions: frameDimensions).rect(in: view.bounds.size)
        }

        private func anchorFromHost() {
            let bounds = canvasBounds
            if let remote = pointerPosition?() {
                capturedPosition.anchor(CGPoint(
                    x: bounds.minX + Double(remote.x) / Double(max(1, frameDimensions.width - 1)) * bounds.width,
                    y: bounds.minY + Double(remote.y) / Double(max(1, frameDimensions.height - 1)) * bounds.height
                ), in: bounds)
            }
        }

        private func physicalMovement(x: Double, y: Double, source: Int) {
            let now = CACurrentMediaTime()
            sawPhysicalMouseMotion = true
            lastPhysicalMouseMotion = now
            rawCount += 1
            // This bounded trace separates lost hover from lost raw input;
            // no pointer coordinates or report payloads are recorded.
            if rawLogBudget > 0 && now - rawLogAt >= 2 {
                rawLogBudget -= 1; rawLogAt = now
                NSLog("PLANK mouse input: raw=%d absolute=%d hoverAgeMs=%.1f requested=%d locked=%d",
                      rawCount, absoluteCount, lastAbsoluteMovement > 0 ? (now - lastAbsoluteMovement) * 1000 : Double(-1),
                      capturePolicy.shouldRequest ? 1 : 0, capturePolicy.systemLocked ? 1 : 0)
                rawCount = 0; absoluteCount = 0
            }
            if canForwardRawInput && heldButtons.accepts(source: source) {
                // After a pen/mouse handoff, start at the Host's newest cursor.
                // Within a mouse burst keep our own position, avoiding delayed
                // Host echoes undoing accumulated movement.
                if !heldButtons.isHolding && (capturedPosition.point == nil || now - lastRawMovement > 0.15) { anchorFromHost() }
                if let point = capturedPosition.move(x: x, y: y, in: canvasBounds, sensitivity: mouseSensitivity) { onPointer(point) }
                lastRawMovement = now
            } else if !canForwardRawInput {
                releaseButtons()
            }
        }

        func absolutePointer(_ location: CGPoint) {
            guard !controlsPresented, !capturePolicy.usesRawInput else { return }
            lastAbsoluteMovement = CACurrentMediaTime()
            absoluteCount += 1
            capturedPosition.anchor(location, in: canvasBounds)
            onPointer(location)
        }

        func absoluteButton(_ button: UInt8, _ pressed: Bool) {
            // UIKit gesture cancellation cannot release a raw-owned drag.
            // Only one path forwards each physical button edge.
            guard !capturePolicy.usesRawInput else { return }
            forwardButton(button, pressed)
        }

        private func forwardButton(_ button: UInt8, _ pressed: Bool, source: Int = 0) {
            if pressed {
                guard !controlsPresented, currentFocus().0 == .desktop else { return }
            }
            guard heldButtons.transition(button, pressed: pressed, source: source) else { return }
            onButton(button, pressed)
        }

        func releaseButtons() {
            for button in heldButtons.releaseAll() { onButton(button, false) }
        }

        func updateCaptureRequest() {
            let wasRaw = capturePolicy.usesRawInput
            capturePolicy.requested = mouseCapture?.requested == true
            capturePolicy.profilesAvailable = trackedMice.contains { $0.mouseInput != nil }
            capturePolicy.desktopFocused = sceneIsActive && currentFocus().0 == .desktop
            capturePolicy.controlsPresented = controlsPresented
            capturePolicy.background = sceneInBackground
            if wasRaw != capturePolicy.usesRawInput {
                rawLogBudget = 16
                rawLogAt = 0
                rawCount = 0
                absoluteCount = 0
                invalidatePointerAppearance()
            }
            if wasRaw && !capturePolicy.usesRawInput {
                releaseButtons()
                capturedPosition.reset()
            }
            // The probe established that raw profiles work in this window
            // without pointer lock. Do not request a lock that the scene denies.
            inputController?.updatePointerLockRequest(false)
            readPointerLock()
        }

        func observePointerLock() {
            if let pointerLockObserver { NotificationCenter.default.removeObserver(pointerLockObserver) }
            pointerLockObserver = nil
            if let state = inputView?.window?.windowScene?.pointerLockState {
                pointerLockObserver = NotificationCenter.default.addObserver(
                    forName: UIPointerLockState.didChangeNotification, object: state, queue: .main
                ) { [weak self] _ in
                    MainActor.assumeIsolated { self?.readPointerLock() }
                }
            }
            updateCaptureRequest()
        }

        private func readPointerLock() {
            guard let scene = inputView?.window?.windowScene else { return }
            let state = scene.pointerLockState
            let locked = state?.isLocked == true
            if capturePolicy.systemLocked != locked {
                capturePolicy.systemLocked = locked
                NSLog("PLANK mouse capture: requested=%d stateAvailable=%d locked=%d",
                      capturePolicy.shouldRequest ? 1 : 0, state != nil ? 1 : 0, locked ? 1 : 0)
            }
            mouseCapture?.report(stateAvailable: state != nil, locked: locked)
        }

        // Pointer-region requests follow the actual system pointer; they do
        // not depend on GCMouse's raw motion callback remaining installed.
        func pointerInteraction(_ interaction: UIPointerInteraction,
                                regionFor request: UIPointerRegionRequest,
                                defaultRegion: UIPointerRegion) -> UIPointerRegion? {
            guard !controlsPresented else { return nil }
            if request.location != pointerRequestLocation {
                pointerRequestLocation = request.location
                if pointerRequestLogBudget > 0 {
                    pointerRequestLogBudget -= 1
                    NSLog("PLANK mouse pointer: absolute region update")
                }
                absolutePointer(request.location)
            }
            return defaultRegion
        }

        func pointerInteraction(_ interaction: UIPointerInteraction,
                                styleFor region: UIPointerRegion) -> UIPointerStyle? {
            controlsPresented ? nil : UIPointerStyle.hidden()
        }

        private static func wheelStepAmount(_ value: CGFloat) -> Int16 {
            guard value.isFinite else { return 0 }
            guard value != 0 else { return 0 }
            let ticks = Int16(min(12, max(1, (abs(value) / 32).rounded())))
            return value > 0 ? ticks * 120 : -ticks * 120
        }

        func scrollViewDidScroll(_ scrollView: UIScrollView) {
            guard let wheel = scrollView as? WheelCaptureScrollView,
                  !wheel.isRecentering,
                  wheel.hasWheelGeometry else { return }
            let delta = CGPoint(
                x: scrollView.contentOffset.x - wheel.centerOffset.x,
                y: scrollView.contentOffset.y - wheel.centerOffset.y
            )
            guard delta.x != 0 || delta.y != 0 else { return }
            wheel.recenter()
            guard !capturePolicy.usesRawInput else { return }
            let vertical = Self.wheelStepAmount(-delta.y)
            let horizontal = Self.wheelStepAmount(-delta.x)
            if vertical != 0 || horizontal != 0 {
                onScroll(vertical, horizontal)
            }
        }

        @objc func handleHover(_ recognizer: UIHoverGestureRecognizer) {
            guard let view = recognizer.view else { return }
            switch recognizer.state {
            case .began, .changed:
                // On visionOS gaze also drives hover. Once a physical mouse has
                // been observed, only hover paired with recent mouse motion may
                // move the remote pointer; merely looking at the window cannot.
                if sawPhysicalMouseMotion &&
                    CACurrentMediaTime() - lastPhysicalMouseMotion > 0.25 {
                    return
                }
                let location = recognizer.location(in: view)
                absolutePointer(location)
            default:
                break
            }
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer
        ) -> Bool {
            gestureRecognizer is UIHoverGestureRecognizer ||
                otherGestureRecognizer is UIHoverGestureRecognizer
        }
    }
}

// An indirect-pointer touch spans the physical button's down-to-up interval.
// Tap recognizers only fire on release, and pan recognizers wait for movement,
// so neither can faithfully represent a stationary click and hold.
private final class PhysicalMousePressRecognizer: UIGestureRecognizer {
    var onPointer: ((CGPoint) -> Void)?
    var onButton: ((UInt8, Bool) -> Void)?
    private weak var activeTouch: UITouch?
    private var activeButton: UInt8?

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
        super.touchesBegan(touches, with: event)
        guard activeTouch == nil,
              let touch = touches.first(where: { $0.type == .indirectPointer }),
              let view else { return }
        let mask = event.buttonMask
        let button: UInt8
        if mask.contains(.button(3)) {
            button = 2 // Moonlight middle
        } else if mask.contains(.secondary) {
            button = 3 // Moonlight right
        } else {
            button = 1
        }
        activeTouch = touch
        activeButton = button
        onPointer?(touch.location(in: view))
        onButton?(button, true)
        state = .began
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent) {
        super.touchesMoved(touches, with: event)
        guard let activeTouch, touches.contains(activeTouch), let view else { return }
        onPointer?(activeTouch.location(in: view))
        state = .changed
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent) {
        super.touchesEnded(touches, with: event)
        guard let activeTouch, touches.contains(activeTouch) else { return }
        if let view { onPointer?(activeTouch.location(in: view)) }
        releaseIfNeeded()
        state = .ended
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent) {
        super.touchesCancelled(touches, with: event)
        guard let activeTouch, touches.contains(activeTouch) else { return }
        releaseIfNeeded()
        state = .cancelled
    }

    func releaseIfNeeded() {
        if let activeButton { onButton?(activeButton, false) }
        activeTouch = nil
        activeButton = nil
    }

    override func reset() {
        releaseIfNeeded()
        super.reset()
    }
}

private final class WheelCaptureScrollView: UIScrollView {
    private(set) var isRecentering = false
    private(set) var hasWheelGeometry = false

    var centerOffset: CGPoint {
        CGPoint(x: bounds.width, y: bounds.height)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        guard bounds.width > 0, bounds.height > 0 else { return }
        let size = CGSize(width: bounds.width * 3, height: bounds.height * 3)
        guard contentSize != size else { return }
        isRecentering = true
        hasWheelGeometry = false
        contentSize = size
        contentOffset = centerOffset
        hasWheelGeometry = true
        isRecentering = false
    }

    func recenter() {
        isRecentering = true
        contentOffset = centerOffset
        isRecentering = false
    }
}

private struct WindowGeometryConfigurator: UIViewRepresentable {
    let pixelWidth: Int
    let pixelHeight: Int
    let allowsUserResize: Bool

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> UIView {
        let view = UIView(frame: .zero)
        applyGeometry(to: view, coordinator: context.coordinator)
        return view
    }

    func updateUIView(_ view: UIView, context: Context) {
        applyGeometry(to: view, coordinator: context.coordinator)
    }

    private func applyGeometry(to view: UIView, coordinator: Coordinator) {
        guard pixelWidth > 0, pixelHeight > 0 else { return }
        let aspectRatio = CGFloat(pixelWidth) / CGFloat(pixelHeight)
        guard coordinator.appliedAspectRatio != aspectRatio ||
                coordinator.appliedAllowsUserResize != allowsUserResize else { return }

        DispatchQueue.main.async {
            guard let scene = view.window?.windowScene else {
                DispatchQueue.main.async {
                    applyGeometry(to: view, coordinator: coordinator)
                }
                return
            }
            let aspectChanged = coordinator.appliedAspectRatio != aspectRatio
            let preferences = UIWindowScene.GeometryPreferences.Vision()
            if aspectChanged {
                let idealWidth: CGFloat = 1280
                let minimumWidth: CGFloat = 640
                let maximumWidth: CGFloat = 2560
                preferences.size = CGSize(width: idealWidth, height: idealWidth / aspectRatio)
                preferences.minimumSize = CGSize(width: minimumWidth, height: minimumWidth / aspectRatio)
                preferences.maximumSize = CGSize(width: maximumWidth, height: maximumWidth / aspectRatio)
            }
            // A resize preference is optional: unqualified .none would mean
            // nil (unspecified), not the enum that disables user resizing.
            let restrictions: UIWindowScene.ResizingRestrictions = allowsUserResize ? .uniform : .none
            preferences.resizingRestrictions = restrictions
            precondition(preferences.resizingRestrictions == restrictions)
            // Toggling changes only the policy; retain the user's current size.
            scene.requestGeometryUpdate(preferences) { error in
                print("PLANK window geometry update failed: \(error.localizedDescription)")
            }
            coordinator.appliedAspectRatio = aspectRatio
            coordinator.appliedAllowsUserResize = allowsUserResize
            NSLog("PLANK window resizing: requested=%@ preferenceVerified=1", allowsUserResize ? "allowed" : "disabled")
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [weak scene] in
                guard let scene else { return }
                NSLog("PLANK window resizing: effective=%ld interactive=%d min=%@ max=%@",
                      scene.effectiveGeometry.resizingRestrictions.rawValue,
                      scene.effectiveGeometry.isInteractivelyResizing ? 1 : 0,
                      NSCoder.string(for: scene.effectiveGeometry.minimumSize),
                      NSCoder.string(for: scene.effectiveGeometry.maximumSize))
            }
        }
    }

    final class Coordinator {
        var appliedAspectRatio: CGFloat?
        var appliedAllowsUserResize: Bool?
    }
}

private struct KeyboardCapture: UIViewRepresentable {
    let focusGeneration: Int
    let functionKeyMode: KeyboardFunctionKeyMode
    let onCharacters: @MainActor (String) -> Void
    let onKeyEvent: @MainActor (UInt16, Bool, UInt8) -> Void

    func makeUIView(context: Context) -> KeyboardCaptureView {
        let view = KeyboardCaptureView()
        view.onCharacters = onCharacters
        view.onKeyEvent = onKeyEvent
        view.functionKeyMode = functionKeyMode
        view.focusGeneration = focusGeneration
        return view
    }

    func updateUIView(_ view: KeyboardCaptureView, context: Context) {
        view.onCharacters = onCharacters
        view.onKeyEvent = onKeyEvent
        view.functionKeyMode = functionKeyMode
        guard view.focusGeneration != focusGeneration else { return }
        view.focusGeneration = focusGeneration
        view.requestKeyboardFocus()
    }
}

@MainActor
private final class KeyboardCaptureView: UIView {
    var onCharacters: (@MainActor (String) -> Void)?
    var onKeyEvent: (@MainActor (UInt16, Bool, UInt8) -> Void)?
    var focusGeneration = 0
    var functionKeyMode: KeyboardFunctionKeyMode = .pc
    private var pressedKeys: [UInt16: UInt8] = [:]

    override var canBecomeFirstResponder: Bool { true }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil {
            releaseAllKeys()
        } else {
            requestKeyboardFocus()
        }
    }

    func requestKeyboardFocus() {
        guard window != nil else { return }
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            // A mouse press asks the remote window for keyboard focus too.
            // Resigning an already focused capture view for every press can
            // cancel the pointer gesture before its matching release arrives.
            guard !self.isFirstResponder else { return }
            _ = self.becomeFirstResponder()
        }
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var handled = false
        for press in presses {
            guard let key = press.key else { continue }
            if let code = windowsVirtualKey(for: key.keyCode, functionKeyMode: functionKeyMode) {
                let modifiers = plankModifiers(for: key.modifierFlags)
                pressedKeys[code] = modifiers
                onKeyEvent?(code, true, modifiers)
                handled = true
            } else if !key.characters.isEmpty {
                onCharacters?(key.characters)
                handled = true
            }
        }
        if !handled {
            super.pressesBegan(presses, with: event)
        }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var handled = false
        for press in presses {
            guard let key = press.key,
                  let code = windowsVirtualKey(
                    for: key.keyCode,
                    functionKeyMode: functionKeyMode
                  ) else { continue }
            let modifiers = plankModifiers(for: key.modifierFlags)
            onKeyEvent?(code, false, modifiers)
            pressedKeys.removeValue(forKey: code)
            handled = true
        }
        if !handled {
            super.pressesEnded(presses, with: event)
        }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        releaseAllKeys()
        super.pressesCancelled(presses, with: event)
    }

    private func releaseAllKeys() {
        for (code, modifiers) in pressedKeys {
            onKeyEvent?(code, false, modifiers)
        }
        pressedKeys.removeAll(keepingCapacity: true)
    }
}

private func plankModifiers(for flags: UIKeyModifierFlags) -> UInt8 {
    var modifiers: UInt8 = 0
    if flags.contains(.shift) { modifiers |= 0x01 }
    if flags.contains(.control) { modifiers |= 0x02 }
    if flags.contains(.alternate) { modifiers |= 0x04 }
    if flags.contains(.command) { modifiers |= 0x08 }
    return modifiers
}

private func windowsVirtualKey(
    for usage: UIKeyboardHIDUsage,
    functionKeyMode: KeyboardFunctionKeyMode
) -> UInt16? {
    let key = Int(usage.rawValue)
    if functionKeyMode == .pc {
        switch key {
        // visionOS reports these three top-right keys from a Windows keyboard
        // as F13-F15. Restore the meanings printed on the physical keycaps.
        case 0x68: return 0x2C // Print Screen
        case 0x69: return 0x91 // Scroll Lock
        case 0x6A: return 0x13 // Pause
        default: break
        }
    }
    if (0x04...0x1D).contains(key) {
        return UInt16(0x41 + key - 0x04)
    }
    if (0x1E...0x26).contains(key) {
        return UInt16(0x31 + key - 0x1E)
    }
    if key == 0x27 { return 0x30 }
    if (0x3A...0x45).contains(key) {
        return UInt16(0x70 + key - 0x3A)
    }
    if (0x59...0x61).contains(key) {
        return UInt16(0x61 + key - 0x59)
    }
    if key == 0x62 { return 0x60 }
    if (0x68...0x73).contains(key) {
        return UInt16(0x7C + key - 0x68)
    }

    switch key {
    case 0x28, 0x58: return 0x0D // Return and keypad Enter
    case 0x29: return 0x1B
    case 0x2A: return 0x08
    case 0x2B: return 0x09
    case 0x2C: return 0x20
    case 0x2D: return 0xBD
    case 0x2E: return 0xBB
    case 0x2F: return 0xDB
    case 0x30: return 0xDD
    case 0x31, 0x32, 0x64: return 0xDC
    case 0x33: return 0xBA
    case 0x34: return 0xDE
    case 0x35: return 0xC0
    case 0x36: return 0xBC
    case 0x37: return 0xBE
    case 0x38: return 0xBF
    case 0x39: return 0x14
    case 0x46: return 0x2C
    case 0x47: return 0x91
    case 0x48: return 0x13
    case 0x49: return 0x2D
    case 0x4A: return 0x24
    case 0x4B: return 0x21
    case 0x4C: return 0x2E
    case 0x4D: return 0x23
    case 0x4E: return 0x22
    case 0x4F: return 0x27
    case 0x50: return 0x25
    case 0x51: return 0x28
    case 0x52: return 0x26
    case 0x53: return 0x90
    case 0x54: return 0x6F
    case 0x55: return 0x6A
    case 0x56: return 0x6D
    case 0x57: return 0x6B
    case 0x63: return 0x6E
    case 0x65: return 0x5D
    case 0xE0, 0xE4: return 0x11
    case 0xE1, 0xE5: return 0x10
    case 0xE2, 0xE6: return 0x12
    case 0xE3: return 0x5B
    case 0xE7: return 0x5C
    default: return nil
    }
}
