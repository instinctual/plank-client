import SwiftUI
import UIKit

private struct PlankIPadControlChromeKey: EnvironmentKey {
    static let defaultValue: PlankControlChromeGeometry? = nil
}
extension EnvironmentValues {
    var plankControlChrome: PlankControlChromeGeometry? {
        get { self[PlankIPadControlChromeKey.self] }
        set { self[PlankIPadControlChromeKey.self] = newValue }
    }
}

/// Desktop and Pencil sharing use the same window-sized artist-control canvas.
/// Local chrome is a foreground layer; only the video/pad reserves its space.
/// This does not change either UIKit surface's remote-coordinate transform.
struct PlankIPadWorkingSurface<Content: View, Controls: View, Toolbar: View, Floating: View, Consent: View>: View {
    let barsVisible: Bool
    let consentVisible: Bool
    let measured: (CGSize) -> Void
    let chromeChanged: (PlankControlChromeGeometry) -> Void
    let content: Content
    let controls: Controls
    let toolbar: Toolbar
    let floating: Floating
    let consent: Consent
    let navigation: AnyView?
    let navigationPreferences: PlankIPadZoomNavigatorPreferences?
    @State private var insets = UIEdgeInsets.zero
    @State private var floatingFrame = CGRect.zero
    @State private var consentFrame = CGRect.zero
    @State private var navigationFrame = CGRect.zero
    init(barsVisible: Bool, consentVisible: Bool = false, measured: @escaping (CGSize) -> Void,
         chromeChanged: @escaping (PlankControlChromeGeometry) -> Void = { _ in },
         navigation: AnyView? = nil,
         navigationPreferences: PlankIPadZoomNavigatorPreferences? = nil,
         @ViewBuilder content: () -> Content, @ViewBuilder controls: () -> Controls,
         @ViewBuilder toolbar: () -> Toolbar, @ViewBuilder floating: () -> Floating,
         @ViewBuilder consent: () -> Consent) {
        self.barsVisible = barsVisible; self.consentVisible = consentVisible
        self.measured = measured; self.chromeChanged = chromeChanged
        self.content = content(); self.controls = controls(); self.toolbar = toolbar()
        self.floating = floating(); self.consent = consent()
        self.navigation = navigation
        self.navigationPreferences = navigationPreferences
    }
    var body: some View {
        GeometryReader { geometry in
            let chrome = chromeGeometry(size:geometry.size)
            ZStack {
                content
                    .frame(maxWidth:.infinity,maxHeight:.infinity)
                    .padding(.top,barsVisible ? insets.top + 44 : 0)
                    .padding(.bottom,barsVisible ? insets.bottom : 0)
                controls.frame(width:geometry.size.width,height:geometry.size.height)
                    .environment(\.plankControlChrome,chrome)
                if let navigation, let navigationPreferences {
                    PlankIPadZoomNavigatorPanel(preferences:navigationPreferences,
                        usableBounds:navigationBounds(size:geometry.size),
                        excludedRects:localReservedRects,
                        panelRectChanged:{ navigationFrame = $0 ?? .zero }) {
                            navigation
                        }
                }
                VStack(spacing:0) {
                    if barsVisible {
                        Color.clear.frame(height:insets.top).allowsHitTesting(false)
                        toolbar.frame(height:44).frame(maxWidth:.infinity)
                            .background(.regularMaterial)
                    }
                    Spacer(minLength:0).allowsHitTesting(false)
                }
                if !barsVisible {
                    floating
                        .onGeometryChange(for:CGRect.self) { $0.frame(in:.named("plank-control-window")) } action: { floatingFrame = $0 }
                        .frame(maxWidth:.infinity,maxHeight:.infinity,alignment:.topTrailing)
                        .padding(.top,max(12,insets.top)).padding(.trailing,12)
                }
                consent
                    .onGeometryChange(for:CGRect.self) { $0.frame(in:.named("plank-control-window")) } action: { consentFrame = $0 }
                    .frame(maxWidth:.infinity,maxHeight:.infinity,alignment:.top)
                    .padding(.top,barsVisible ? insets.top + 52 : max(12,insets.top))
            }
            .frame(width:geometry.size.width,height:geometry.size.height)
            .coordinateSpace(name:"plank-control-window")
            .background {
                PlankIPadWindowMetrics { _,value in if insets != value { insets = value } }
            }
            .onAppear { measured(geometry.size) }
            .onChange(of:geometry.size) { _,size in measured(size) }
            .onAppear { chromeChanged(chrome) }
            .onChange(of:chrome) { _,value in chromeChanged(value) }
            .onChange(of:consentVisible) { _,visible in if !visible { consentFrame = .zero } }
            .onChange(of:navigationPreferences?.navigatorVisible) { _,visible in
                if visible != true { navigationFrame = .zero }
            }
        }
        .ignoresSafeArea(.container)
        .ignoresSafeArea(.keyboard,edges:.bottom)
        .background(Color.black.ignoresSafeArea())
    }
    private func chromeGeometry(size: CGSize) -> PlankControlChromeGeometry {
        let bounds = CGRect(origin:.zero,size:size)
        let top = barsVisible ? insets.top + 44 : insets.top
        let usable = CGRect(x:insets.left,y:top,width:max(0,size.width - insets.left - insets.right),
                            height:max(0,size.height - top - insets.bottom))
        // Wait for the actual hidden-toolbar footprint, rather than guessing
        // its width or placing keys under local navigation for one frame.
        let navigationPlaced = navigation != nil && navigationPreferences.map {
            $0.navigatorVisible && PlankIPadZoomNavigatorPlacement.rect(position:$0.normalizedPosition,
                usableBounds:navigationBounds(size:size),excluding:localReservedRects) != nil
        } == true
        let ready = (barsVisible || PlankControlChromeGeometry.valid(floatingFrame))
            && (!navigationPlaced || PlankControlChromeGeometry.valid(navigationFrame))
        return PlankControlChromeGeometry(bounds:bounds,usableBounds:ready ? usable : .zero,
            reservedRects:localReservedRects + (navigationPlaced ? [navigationFrame] : []))
    }
    private var localReservedRects: [CGRect] {
        (barsVisible ? [] : [floatingFrame]) + (consentVisible ? [consentFrame] : [])
    }
    private func navigationBounds(size: CGSize) -> CGRect {
        let top = barsVisible ? insets.top + 44 : insets.top
        return CGRect(x:insets.left + 12,y:top + 12,
            width:max(0,size.width - insets.left - insets.right - 24),
            height:max(0,size.height - top - insets.bottom - 24))
    }
}

/// A public UIKit window measure also gives the idle editor the same reference
/// as a working surface. It never derives dimensions from a toolbar or sheet.
struct PlankIPadWindowMetrics: UIViewRepresentable {
    var sceneChanged: (CGSize,Bool) -> Void = { _,_ in }
    let changed: (CGSize,UIEdgeInsets) -> Void
    func makeUIView(context: Context) -> PlankIPadWindowMetricsView {
        let view = PlankIPadWindowMetricsView(); view.changed = changed; view.sceneChanged = sceneChanged; return view
    }
    func updateUIView(_ view: PlankIPadWindowMetricsView,context: Context) {
        view.changed = changed; view.sceneChanged = sceneChanged; view.report()
    }
}
final class PlankIPadWindowMetricsView: UIView {
    var changed: (CGSize,UIEdgeInsets) -> Void = { _,_ in }
    var sceneChanged: (CGSize,Bool) -> Void = { _,_ in }
    private var previousSize = CGSize.zero
    private var previousInsets = UIEdgeInsets.zero
    private var previousScreenSize = CGSize.zero
    private var previousLandscape: Bool?
    override func didMoveToWindow() { super.didMoveToWindow(); report() }
    override func layoutSubviews() { super.layoutSubviews(); report() }
    override func safeAreaInsetsDidChange() { super.safeAreaInsetsDidChange(); report() }
    func report() {
        guard let window else { return }
        let size = window.bounds.size, insets = window.safeAreaInsets
        let screenSize = window.windowScene?.screen.bounds.size ?? size
        let orientation = window.windowScene?.interfaceOrientation
        let landscape = orientation?.isLandscape == true ? true
            : orientation?.isPortrait == true ? false : (previousLandscape ?? (screenSize.width >= screenSize.height))
        guard size != previousSize || insets != previousInsets
                || screenSize != previousScreenSize || landscape != previousLandscape else { return }
        previousSize = size; previousInsets = insets
        previousScreenSize = screenSize; previousLandscape = landscape
        // Defer the observable SwiftUI update out of UIKit's layout pass.
        DispatchQueue.main.async { [weak self] in
            guard let self,self.window != nil else { return }
            self.sceneChanged(screenSize,landscape)
            self.changed(size,insets)
        }
    }
}

/// A passive background probe for the two native navigation presentations.
/// It observes public UIKit geometry; it never changes layout or navigation.
struct PlankIPadNavigationDiagnostics: UIViewRepresentable {
    private let role: String
    init(role: String) {
        // Do not put an arbitrary caller-provided label into device logs.
        self.role = role == "idle" || role == "editor" ? role : "unavailable"
    }
    func makeUIView(context: Context) -> PlankIPadNavigationDiagnosticsView {
        PlankIPadNavigationDiagnosticsView(role:role)
    }
    func updateUIView(_ view: PlankIPadNavigationDiagnosticsView,context: Context) {
        view.setRole(role)
        view.requestSample()
    }
    static func dismantleUIView(_ view: PlankIPadNavigationDiagnosticsView,coordinator: ()) {
        view.retire()
    }
}

@MainActor
final class PlankIPadNavigationDiagnosticsView: UIView {
    private static let responderLimit = 64
    private static let parentLimit = 32
    private static let sampleLimit = 128
    private static let emissionLimit = 16
    private static let sampleDelay: TimeInterval = 0.1
    private static let emissionInterval: TimeInterval = 0.25
    private var role: String
    private var generation: UInt64 = 0
    private var pending = false
    private var retired = false
    private var samples = 0
    private var emissions = 0
    private var candidate: String?
    private var previous: String?
    private var lastEmission: TimeInterval = -Double.infinity

    init(role: String) {
        self.role = role == "idle" || role == "editor" ? role : "unavailable"
        super.init(frame:.zero)
        isUserInteractionEnabled = false
        isAccessibilityElement = false
        backgroundColor = .clear
    }
    required init?(coder: NSCoder) {
        role = "unavailable"
        super.init(coder:coder)
        isUserInteractionEnabled = false
        isAccessibilityElement = false
        backgroundColor = .clear
    }
    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil {
            generation &+= 1
            pending = false
            candidate = nil
        } else {
            requestSample()
        }
    }
    override func layoutSubviews() { super.layoutSubviews(); requestSample() }
    override func safeAreaInsetsDidChange() { super.safeAreaInsetsDidChange(); requestSample() }
    func setRole(_ value: String) {
        let validated = value == "idle" || value == "editor" ? value : "unavailable"
        guard role != validated else { return }
        role = validated
        candidate = nil
    }
    func retire() {
        retired = true
        generation &+= 1
        pending = false
        candidate = nil
    }
    func requestSample() {
        guard !retired,!pending,window != nil,
              samples < Self.sampleLimit,emissions < Self.emissionLimit else { return }
        pending = true
        generation &+= 1
        let scheduledGeneration = generation
        // Read after UIKit's layout callback has returned. A second matching
        // deferred sample is required before publishing a changed snapshot.
        DispatchQueue.main.asyncAfter(deadline:.now() + Self.sampleDelay) { [weak self] in
            guard let self,!self.retired,self.generation == scheduledGeneration else { return }
            self.pending = false
            self.sample()
        }
    }
    private func sample() {
        guard let window,!retired,samples < Self.sampleLimit,
              emissions < Self.emissionLimit else { return }
        samples += 1
        let snapshot = snapshot(in:window)
        guard candidate == snapshot else {
            candidate = snapshot
            requestSample()
            return
        }
        guard previous != snapshot else { return }
        let now = ProcessInfo.processInfo.systemUptime
        guard now - lastEmission >= Self.emissionInterval else {
            requestSample()
            return
        }
        previous = snapshot
        lastEmission = now
        emissions += 1
        NSLog("%@","PLANK iPad navigation layout emission=\(emissions) \(snapshot)")
    }
    private func snapshot(in window: UIWindow) -> String {
        let owner = owningController()
        let navigation = owningNavigationController(from:owner)
        let ownerView = owner?.viewIfLoaded
        let navigationView = navigation?.viewIfLoaded
        // Accessing navigationBar is avoided until the controller is loaded.
        let bar = navigation?.isViewLoaded == true ? navigation?.navigationBar : nil
        let status = window.windowScene?.statusBarManager
        let fields = [
            "role=\(role)",
            "window_bounds=\(Self.rect(window.bounds))",
            "window_safe=\(Self.insets(window.safeAreaInsets))",
            "probe_frame_window=\(frameInWindow(self,window:window))",
            "probe_safe=\(Self.insets(safeAreaInsets))",
            "owner_available=\(owner == nil ? "unavailable" : "true")",
            "owner_frame_window=\(frameInWindow(ownerView,window:window))",
            "owner_bounds=\(ownerView.map { Self.rect($0.bounds) } ?? "unavailable")",
            "owner_safe=\(ownerView.map { Self.insets($0.safeAreaInsets) } ?? "unavailable")",
            "navigation_available=\(navigation == nil ? "unavailable" : "true")",
            "navigation_controller_frame_window=\(frameInWindow(navigationView,window:window))",
            "navigation_controller_safe=\(navigationView.map { Self.insets($0.safeAreaInsets) } ?? "unavailable")",
            "navigation_bar_frame_window=\(frameInWindow(bar,window:window))",
            "navigation_bar_policy_hidden=\(navigation.map { Self.available($0.isNavigationBarHidden) } ?? "unavailable")",
            "navigation_bar_hidden=\(bar.map { Self.available($0.isHidden) } ?? "unavailable")",
            "status_visible=\(status.map { Self.available(!$0.isStatusBarHidden) } ?? "unavailable")",
            "status_frame_native=\(status.map { Self.rect($0.statusBarFrame) } ?? "unavailable")",
            "status_frame_space=\(status == nil ? "unavailable" : "unverified")",
            // UIStatusBarManager's public header does not identify the frame's
            // coordinate space. Do not guess a screen-to-window conversion.
            "status_frame_window=\(status == nil ? "unavailable" : "unavailable_coordinate_space")"
        ]
        return fields.joined(separator:" ")
    }
    private func owningController() -> UIViewController? {
        var responder: UIResponder? = self
        var visited = Set<ObjectIdentifier>()
        for _ in 0..<Self.responderLimit {
            guard let current = responder,visited.insert(ObjectIdentifier(current)).inserted else { return nil }
            if let controller = current as? UIViewController { return controller }
            responder = current.next
        }
        return nil
    }
    private func owningNavigationController(from owner: UIViewController?) -> UINavigationController? {
        var controller = owner
        var visited = Set<ObjectIdentifier>()
        for _ in 0..<Self.parentLimit {
            guard let current = controller,visited.insert(ObjectIdentifier(current)).inserted else { return nil }
            if let navigation = current as? UINavigationController { return navigation }
            if let navigation = current.navigationController { return navigation }
            controller = current.parent
        }
        return nil
    }
    private func frameInWindow(_ view: UIView?,window: UIWindow) -> String {
        guard let view else { return "unavailable" }
        guard view.window === window else { return "unavailable_window" }
        return Self.rect(view.convert(view.bounds,to:window))
    }
    private static func available(_ value: Bool) -> String { value ? "true" : "false" }
    private static func rect(_ value: CGRect) -> String {
        String(format:"(%.1f,%.1f,%.1f,%.1f)",Double(value.origin.x),Double(value.origin.y),
               Double(value.size.width),Double(value.size.height))
    }
    private static func insets(_ value: UIEdgeInsets) -> String {
        String(format:"(%.1f,%.1f,%.1f,%.1f)",Double(value.top),Double(value.left),
               Double(value.bottom),Double(value.right))
    }
}
