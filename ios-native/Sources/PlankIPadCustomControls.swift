import SwiftUI
import UIKit

enum PlankIPadControlSurface: Hashable { case desktop, pencilSharing }

/// Fast local visibility control with shared appearance options.
struct PlankIPadControlsVisibilityButton: View {
    @ObservedObject var store: PlankIPadCustomControlsStore
    let visible: Bool
    let toggle: () -> Void
    @State private var showingTransparency = false
    var body: some View {
        Button(action:toggle) {
            Label(visible ? "Hide Controls" : "Show Controls",
                  systemImage:visible ? "rectangle.grid.2x2.fill" : "rectangle.grid.2x2")
                .labelStyle(.iconOnly)
                .frame(minWidth:44,minHeight:44)
        }
        .accessibilityLabel(visible ? "Hide artist controls" : "Show artist controls")
        .accessibilityValue(visible ? "Visible" : "Hidden")
        .contextMenu {
            Button("Transparency…",systemImage:"circle.lefthalf.filled") { showingTransparency = true }
        }
        .accessibilityAction(named:Text("Controls transparency")) { showingTransparency = true }
        .popover(isPresented:$showingTransparency) {
            VStack(alignment:.leading,spacing:16) {
                HStack {
                    Text("Transparency").font(.headline)
                    Spacer()
                    Button("Done") { showingTransparency = false }
                }
                HStack {
                    Text("Artist Controls")
                    Spacer()
                    Text(store.transparency,format:.percent.precision(.fractionLength(0)))
                        .foregroundStyle(.secondary).monospacedDigit()
                }
                Slider(value:Binding(get:{ store.transparency },set:{ store.setTransparency($0) }),in:0...1)
                    .accessibilityLabel("Artist controls transparency")
                    .accessibilityValue(Text(store.transparency,format:.percent.precision(.fractionLength(0))))
                Text("Applies to iPad Desktop and Pencil Sharing. Held keys stay highlighted.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            .padding().frame(width:300)
            .presentationCompactAdaptation(.popover)
        }
    }
}

/// Tap to choose a saved layout; native touch-and-hold opens the editor action.
/// A Button/popover keeps layout selection separate from the context menu.
struct PlankIPadControlLayoutsButton: View {
    @ObservedObject var store: PlankIPadCustomControlsStore
    let select: (UUID) -> Void
    let edit: () -> Void
    @State private var showingLayouts = false
    @State private var pendingEditor = false
    var body: some View {
        Button { showingLayouts = true } label: {
            Label("Layouts",systemImage:"rectangle.stack")
                .labelStyle(.iconOnly).frame(minWidth:44,minHeight:44)
        }
        .accessibilityLabel("Layouts")
        .accessibilityValue(store.selectedLayout.name)
        .accessibilityHint("Choose a saved layout. Touch and hold to edit layouts.")
        .contextMenu { Button("Edit Layouts…",systemImage:"pencil",action:openEditor) }
        .accessibilityAction(named:Text("Edit layouts"),openEditor)
        .popover(isPresented:$showingLayouts) {
            VStack(alignment:.leading,spacing:12) {
                HStack {
                    Text("Layouts").font(.headline)
                    Spacer()
                    Button("Done") { showingLayouts = false }
                }
                ScrollView {
                    VStack(spacing:0) {
                        ForEach(store.library.layouts) { layout in
                            Button {
                                showingLayouts = false
                                select(layout.id)
                            } label: {
                                HStack {
                                    Text(layout.name).lineLimit(2)
                                    Spacer()
                                    Image(systemName:"checkmark")
                                        .opacity(layout.id == store.library.selectedID ? 1 : 0)
                                }.frame(maxWidth:.infinity,minHeight:44,alignment:.leading)
                                    .contentShape(Rectangle())
                            }
                            .accessibilityLabel(layout.name)
                            .accessibilityValue(layout.id == store.library.selectedID ? "Selected" : "")
                            .accessibilityAddTraits(layout.id == store.library.selectedID ? .isSelected : [])
                        }
                    }
                }.frame(maxHeight:min(CGFloat(store.library.layouts.count) * 52,352))
                Divider()
                Button("Edit Layouts…",systemImage:"pencil",action:openEditor)
                    .frame(minHeight:44)
                if let size = store.latestSurfaceSize(for:.desktop),
                   store.unavailableControlCount(referenceSize:size) > 0 {
                    Text("Enlarge the window to use all controls.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            .padding().frame(width:300)
            .presentationCompactAdaptation(.popover)
            .onDisappear(perform:completeEditorRequest)
        }
    }
    private func openEditor() {
        if showingLayouts {
            // The chooser and editor must not compete in one presentation
            // transaction, especially from the idle navigation toolbar.
            pendingEditor = true
            showingLayouts = false
        } else { edit() }
    }
    private func completeEditorRequest() {
        guard pendingEditor else { return }
        pendingEditor = false
        Task { @MainActor in
            await Task.yield()
            edit()
        }
    }
}

/// Local editor typing has its own native responder. A bounded attachment
/// handoff avoids requesting focus while the layout menu is still closing.
private struct PlankIPadLocalNameField: UIViewRepresentable {
    @Binding var text: String
    let submit: () -> Void
    final class Coordinator: NSObject, UITextFieldDelegate {
        var owner: PlankIPadLocalNameField
        init(_ owner: PlankIPadLocalNameField) { self.owner = owner }
        @objc func changed(_ field: UITextField) { owner.text = field.text ?? "" }
        func textFieldShouldReturn(_ textField: UITextField) -> Bool { owner.submit(); return false }
    }
    func makeCoordinator() -> Coordinator { Coordinator(self) }
    func makeUIView(context: Context) -> PlankIPadLocalNameTextField {
        let field = PlankIPadLocalNameTextField(frame:.zero)
        field.borderStyle = .roundedRect; field.placeholder = "Layout name"
        field.accessibilityLabel = "Layout name"
        field.font = .preferredFont(forTextStyle:.body); field.adjustsFontForContentSizeCategory = true
        field.autocapitalizationType = .sentences; field.autocorrectionType = .no
        field.spellCheckingType = .no; field.returnKeyType = .done
        field.delegate = context.coordinator
        field.addTarget(context.coordinator,action:#selector(Coordinator.changed(_:)),for:.editingChanged)
        field.text = text
        return field
    }
    func updateUIView(_ field: PlankIPadLocalNameTextField,context: Context) {
        context.coordinator.owner = self
        if field.text != text { field.text = text }
        field.requestInitialFocus()
    }
    static func dismantleUIView(_ field: PlankIPadLocalNameTextField,coordinator: Coordinator) {
        field.retire()
    }
}

private final class PlankIPadLocalNameTextField: UITextField {
    private var pending = false, retired = false, focusedOnce = false
    private var attempts = 0, emissions = 0
    private var generation: UInt64 = 0
    override init(frame: CGRect) {
        super.init(frame:frame)
        NotificationCenter.default.addObserver(self,selector:#selector(sceneActive(_:)),
            name:UIScene.didActivateNotification,object:nil)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unsupported") }
    override func didMoveToWindow() { super.didMoveToWindow(); requestInitialFocus() }
    override func layoutSubviews() { super.layoutSubviews(); requestInitialFocus() }
    @objc private func sceneActive(_ note: Notification) {
        guard let scene = note.object as? UIWindowScene,scene === window?.windowScene else { return }
        requestInitialFocus()
    }
    func requestInitialFocus() {
        guard !retired,!pending,!focusedOnce,attempts < 3,
              let window,window.isKeyWindow,bounds.width > 0,bounds.height > 0,
              window.windowScene?.activationState == .foregroundActive else { return }
        pending = true; generation &+= 1
        let token = generation
        DispatchQueue.main.async { [weak self,weak window] in
            guard let self,!self.retired,self.generation == token else { return }
            self.pending = false
            guard let window,self.window === window,window.isKeyWindow,
                  window.windowScene?.activationState == .foregroundActive else { return }
            self.attempts += 1
            self.focusedOnce = self.isFirstResponder || self.becomeFirstResponder()
            if self.emissions < 8 {
                self.emissions += 1
                NSLog("PLANK editor name focus attempt=%d focused=%d window=%.0fx%.0f field=%.0fx%.0f",
                    self.attempts,self.isFirstResponder,window.bounds.width,window.bounds.height,self.bounds.width,self.bounds.height)
            }
        }
    }
    func retire() {
        retired = true; generation &+= 1; pending = false
        NotificationCenter.default.removeObserver(self)
        if isFirstResponder { resignFirstResponder() }
    }
}

/// One saved library is shared by the direct desktop and Pencil sharing.
/// Editing stays local until Done; the overlay receives transport closures.
@MainActor final class PlankIPadCustomControlsStore: ObservableObject {
    @Published private(set) var library: PlankCustomControlLibrary
    @Published private(set) var transparency: Double
    @Published private(set) var chromeRevision: UInt64 = 0
    @Published private(set) var sceneLandscape = true
    private var physicalScreenSize = CGSize.zero
    private var latestChrome: [PlankIPadControlSurface: PlankControlChromeGeometry] = [:]
    func reportScene(_ size: CGSize,_ landscape: Bool) {
        guard Self.usable(size), size != physicalScreenSize || landscape != sceneLandscape else { return }
        physicalScreenSize = size; sceneLandscape = landscape
        for (surface,windowSize) in latestSurfaceSizes {
            surfaceSizes[surface,default:[:]][landscape] = windowSize
            if let chrome = latestChrome[surface],chrome.bounds.size == windowSize {
                chromeAreas[surface,default:[:]][landscape] = chrome
            }
        }
        chromeRevision &+= 1
    }
    func legacyReferenceSize(landscape: Bool) -> CGSize {
        guard Self.usable(physicalScreenSize) else { return .zero }
        return (physicalScreenSize.width >= physicalScreenSize.height) == landscape
            ? physicalScreenSize : CGSize(width:physicalScreenSize.height,height:physicalScreenSize.width)
    }
    private let defaults: UserDefaults
    /// Surface geometry is transient. Reporting it must not publish a change
    /// during UIKit layout or alter the saved arrangement.
    private var surfaceSizes: [PlankIPadControlSurface: [Bool: CGSize]] = [:]
    private var latestSurfaceSizes: [PlankIPadControlSurface: CGSize] = [:]
    private var chromeAreas: [PlankIPadControlSurface: [Bool: PlankControlChromeGeometry]] = [:]
    func reportChrome(_ value: PlankControlChromeGeometry,surface: PlankIPadControlSurface = .desktop) {
        guard Self.usable(value.bounds.size) else { return }
        let landscape = sceneLandscape
        latestChrome[surface] = value
        guard chromeAreas[surface]?[landscape] != value else { return }
        chromeAreas[surface,default:[:]][landscape] = value
        chromeRevision &+= 1
    }
    func chromeGeometry(referenceSize: CGSize,surface: PlankIPadControlSurface = .desktop,
                        landscape: Bool? = nil) -> PlankControlChromeGeometry {
        if let value = chromeAreas[surface]?[landscape ?? sceneLandscape],
           value.bounds.size == referenceSize { return value }
        return .unmeasured(size:referenceSize)
    }
    func hasMeasuredChrome(referenceSize: CGSize,surface: PlankIPadControlSurface = .desktop,
                           landscape: Bool? = nil) -> Bool {
        chromeAreas[surface]?[landscape ?? sceneLandscape]?.bounds.size == referenceSize
    }
    func unavailableControlCount(referenceSize: CGSize,surface: PlankIPadControlSurface = .desktop) -> Int {
        PlankControlResponsiveResolution.resolve(layout:selectedLayout,landscape:sceneLandscape,
            chrome:chromeGeometry(referenceSize:referenceSize,surface:surface),
            legacyReferenceSize:legacyReferenceSize(landscape:sceneLandscape)).unavailableIDs.count
    }
    func latestSurfaceSize(for surface: PlankIPadControlSurface) -> CGSize? { latestSurfaceSizes[surface] }
    func reportSurface(size: CGSize, surface: PlankIPadControlSurface = .desktop) {
        guard Self.usable(size) else { return }
        surfaceSizes[surface,default:[:]][sceneLandscape] = size
        latestSurfaceSizes[surface] = size
    }
    func referenceSize(landscape: Bool, fallback: CGSize, surface: PlankIPadControlSurface = .desktop) -> CGSize {
        if Self.usable(fallback), sceneLandscape == landscape { return fallback }
        if let exact = surfaceSizes[surface]?[landscape] { return exact }
        let available = Self.usable(fallback) ? fallback : (latestSurfaceSizes[surface] ?? CGSize(width:1180,height:820))
        return sceneLandscape == landscape
            ? available : CGSize(width:available.height,height:available.width)
    }
    private static func usable(_ size: CGSize) -> Bool {
        size.width.isFinite && size.height.isFinite && size.width > 1 && size.height > 1
    }
    init(library: PlankCustomControlLibrary = .load(),defaults: UserDefaults = .standard) {
        self.library = library
        self.defaults = defaults
        transparency = PlankIPadControlAppearance.load(from:defaults)
    }
    func setTransparency(_ value: Double) {
        guard value.isFinite else { return }
        let next = PlankIPadControlAppearance.normalized(value)
        guard next != transparency else { return }
        PlankIPadControlAppearance.save(next,to:defaults)
        transparency = next
    }
    var selectedLayout: PlankControlLayout { library.selectedLayout }
    func select(id: UUID) {
        var next = library
        guard next.select(id:id) else { return }
        replace(next)
    }
    func replace(_ value: PlankCustomControlLibrary) {
        guard value.isValid else { return }
        value.save(); library = value
    }
}

struct PlankIPadCustomControlsOverlay: UIViewRepresentable {
    @ObservedObject var store: PlankIPadCustomControlsStore
    @Environment(\.plankControlChrome) private var liveChrome
    let enabled: Bool
    let begin: (UUID, PlankControlBinding) -> Bool
    let end: (UUID) -> Void
    let tap: (PlankControlBinding) -> Bool
    let release: () -> Void
    var supports: (PlankControlBinding) -> Bool = { _ in true }
    var inputEpoch: UInt64 = 0
    var pencilSurface: () -> UIView? = { nil }
    var pencilHover: (UIHoverGestureRecognizer) -> Void = { _ in }
    var pencilSqueeze: (UIPencilInteraction.Squeeze, UIView) -> Void = { _,_ in }

    func makeUIView(context: Context) -> PlankIPadCustomControlsOverlayView {
        let view = PlankIPadCustomControlsOverlayView()
        configure(view)
        return view
    }
    func updateUIView(_ view: PlankIPadCustomControlsOverlayView, context: Context) { configure(view) }
    private func configure(_ view: PlankIPadCustomControlsOverlayView) {
        view.configure(layout:store.selectedLayout, appearance:PlankIPadControlAppearance(transparency:store.transparency),
                       landscape:store.sceneLandscape,legacyReferenceSize:store.legacyReferenceSize(landscape:store.sceneLandscape),
                       chrome:liveChrome,
                       enabled:enabled, begin:begin,
                       end:end, tap:tap, release:release, supports:supports,inputEpoch:inputEpoch,
                       pencilSurface:pencilSurface,pencilHover:pencilHover,pencilSqueeze:pencilSqueeze)
    }
    static func dismantleUIView(_ view: PlankIPadCustomControlsOverlayView, coordinator: ()) { view.retire(reason:.teardown) }
}

@MainActor final class PlankIPadCustomControlsOverlayView: UIView, UIPencilInteractionDelegate {
    enum RetirementReason: String { case explicit, layout, geometry, disabled, teardown }
    private var layout: PlankControlLayout?
    private var buttons: [UUID: PlankIPadCustomControlButton] = [:]
    private var previousBounds = CGRect.zero
    private var chrome: PlankControlChromeGeometry?
    private var landscape = true
    private var legacyReferenceSize = CGSize.zero
    private var currentInputEpoch: UInt64?
    private var releaseAction: () -> Void = {}
    private var hoverAction: (UIHoverGestureRecognizer) -> Void = { _ in }
    private var squeezeAction: (UIPencilInteraction.Squeeze, UIView) -> Void = { _,_ in }

    init() {
        super.init(frame:.zero)
        backgroundColor = .clear
        isMultipleTouchEnabled = true
        isAccessibilityElement = false
        let hover = UIHoverGestureRecognizer(target:self,action:#selector(hovered(_:)))
        hover.allowedTouchTypes = [NSNumber(value:UITouch.TouchType.pencil.rawValue)]
        hover.requiresExclusiveTouchType = false
        hover.cancelsTouchesInView = false
        hover.delaysTouchesBegan = false; hover.delaysTouchesEnded = false
        addGestureRecognizer(hover)
        let pencil = UIPencilInteraction(); pencil.delegate = self; addInteraction(pencil)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unsupported") }

    func configure(layout next: PlankControlLayout, appearance: PlankIPadControlAppearance = .init(),
                   landscape nextLandscape: Bool = true,legacyReferenceSize nextReferenceSize: CGSize = .zero,
                   chrome nextChrome: PlankControlChromeGeometry? = nil, enabled: Bool,
                   begin: @escaping (UUID, PlankControlBinding) -> Bool,
                   end: @escaping (UUID) -> Void, tap: @escaping (PlankControlBinding) -> Bool,
                   release: @escaping () -> Void, supports: (PlankControlBinding) -> Bool,
                   inputEpoch: UInt64 = 0,
                   pencilSurface: @escaping () -> UIView? = { nil },
                   pencilHover: @escaping (UIHoverGestureRecognizer) -> Void = { _ in },
                   pencilSqueeze: @escaping (UIPencilInteraction.Squeeze, UIView) -> Void = { _,_ in }) {
        if landscape != nextLandscape || legacyReferenceSize != nextReferenceSize {
            retire(reason:.geometry); landscape = nextLandscape; legacyReferenceSize = nextReferenceSize; setNeedsLayout()
        }
        if chrome != nextChrome {
            retire(reason:.geometry); chrome = nextChrome; setNeedsLayout()
        }
        if currentInputEpoch != inputEpoch {
            let hadEpoch = currentInputEpoch != nil
            currentInputEpoch = inputEpoch
            if hadEpoch { retire(reason:.explicit) }
        }
        if layout != next {
            retire(reason:.layout)
            buttons.values.forEach { $0.removeFromSuperview() }
            buttons.removeAll()
            layout = next
            for control in next.controls {
                let button = PlankIPadCustomControlButton(control:control)
                buttons[control.id] = button
                addSubview(button)
            }
            setNeedsLayout()
        }
        releaseAction = release
        hoverAction = pencilHover; squeezeAction = pencilSqueeze
        for control in next.controls {
            buttons[control.id]?.configure(enabled:enabled && supports(control.binding),
                                           appearance:appearance,begin:begin, end:end, tap:tap,pencilSurface:pencilSurface)
        }
    }
    override func layoutSubviews() {
        super.layoutSubviews()
        if bounds != previousBounds {
            // A touch owns the binding and geometry where it began. Resizing
            // cannot transfer that ownership to a control under its new frame.
            retire(reason:.geometry)
            previousBounds = bounds
        }
        guard let layout else { return }
        let area = chrome ?? .unrestricted(size:bounds.size)
        let resolution = area.bounds == bounds
            ? PlankControlResponsiveResolution.resolve(layout:layout,landscape:landscape,chrome:area,
                legacyReferenceSize:legacyReferenceSize)
            : PlankControlResponsiveResolution.resolve(layout:layout,landscape:landscape,
                chrome:.unmeasured(size:bounds.size),legacyReferenceSize:legacyReferenceSize)
        for control in layout.controls {
            let frame = resolution.frames[control.id]
            buttons[control.id]?.isHidden = frame == nil
            buttons[control.id]?.frame = frame ?? .zero
            buttons[control.id]?.setGeometry(hitFrame:resolution.fingerHitFrames[control.id],scale:resolution.geometry.scale)
        }
    }
    override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
        // Uncovered canvas points pass through. Over a key, route the actual
        // delivered UITouch by type; hit-testing can precede event.allTouches
        // and must not guess a new Pencil contact from nearby touch positions.
        layoutIfNeeded()
        guard let hit = super.hitTest(point, with:event), hit !== self else { return nil }
        return hit
    }
    @objc private func hovered(_ gesture: UIHoverGestureRecognizer) { hoverAction(gesture) }
    func pencilInteraction(_ interaction: UIPencilInteraction,didReceiveSqueeze value: UIPencilInteraction.Squeeze) {
        squeezeAction(value,self)
    }
    func retire(reason: RetirementReason = .explicit) {
        buttons.values.forEach { $0.retire(reason:reason) }
        releaseAction()
    }
}

/// A control can have several finger owners. Every accepted contact snapshots
/// its binding, and an accessibility hold has a separate owner from fingers.
@MainActor private final class PlankIPadCustomControlButton: UIView {
    private final class PencilContact {
        let touch: UITouch
        weak var target: UIView?
        init(touch: UITouch,target: UIView) { self.touch = touch; self.target = target }
    }
    private struct Diagnostics {
        var accepted = 0, rejected = 0, indirect = 0, pencil = 0, ends = 0, cancels = 0
        var disabled = 0, layout = 0, geometry = 0, teardown = 0, explicit = 0
        mutating func increment(_ key: WritableKeyPath<Self,Int>, by count: Int = 1) {
            self[keyPath:key] = min(1_000_000,self[keyPath:key] + count)
        }
    }
    /// Public recognizer state at cancellation is correlation, not UIKit's
    /// cancellation reason. Sets bound inspection and never leave this view.
    private struct CancellationRecognizers {
        static let limit = 64
        private var seen = Set<ObjectIdentifier>()
        var recognizedCanceling = 0
        var possibleCanceling = 0
        var exclusiveRecognized = 0
        var categoryMask = 0
        var stateMask = 0
        var truncated = false
        var count: Int { seen.count }

        mutating func inspect(_ recognizer: UIGestureRecognizer) {
            let id = ObjectIdentifier(recognizer)
            guard !seen.contains(id) else { return }
            guard seen.count < Self.limit else { truncated = true; return }
            seen.insert(id)
            guard recognizer.isEnabled, recognizer.cancelsTouchesInView else { return }
            if recognizer.state == .possible { possibleCanceling += 1; return }
            guard recognizer.state == .began || recognizer.state == .changed || recognizer.state == .ended else { return }
            recognizedCanceling += 1
            if recognizer.requiresExclusiveTouchType { exclusiveRecognized += 1 }
            stateMask |= 1 << recognizer.state.rawValue
            // Public base classes only: no private class or arbitrary name.
            switch recognizer {
            case is UIScreenEdgePanGestureRecognizer: categoryMask |= 1
            case is UIPanGestureRecognizer: categoryMask |= 2
            case is UIPinchGestureRecognizer: categoryMask |= 4
            case is UIRotationGestureRecognizer: categoryMask |= 8
            case is UITapGestureRecognizer: categoryMask |= 16
            case is UILongPressGestureRecognizer: categoryMask |= 32
            case is UISwipeGestureRecognizer: categoryMask |= 64
            case is UIHoverGestureRecognizer: categoryMask |= 128
            default: categoryMask |= 256
            }
        }
    }
    private struct CancellationDiagnostics {
        var callbacks = 0, owners = 0, withPencil = 0, withTouchRecognizer = 0, withAncestorRecognizer = 0
        var lastLog = -Double.infinity
        mutating func increment(_ key: WritableKeyPath<Self,Int>, by count: Int = 1) {
            self[keyPath:key] = min(1_000_000,self[keyPath:key] + count)
        }
    }
    private let control: PlankCustomControl
    private let label = UILabel()
    private var contacts = PlankIPadControlContactPolicy<ObjectIdentifier>()
    private var pencils: [ObjectIdentifier: PencilContact] = [:]
    private var accessibilityOwner: UUID?
    private var enabled = false
    private var requestedEnabled = false
    private var fingerHitBounds: CGRect?
    private var visualScale: CGFloat = 1
    private var labelInsets: [NSLayoutConstraint] = []
    private var appearance = PlankIPadControlAppearance()
    private var beginAction: (UUID, PlankControlBinding) -> Bool = { _,_ in false }
    private var endAction: (UUID) -> Void = { _ in }
    private var tapAction: (PlankControlBinding) -> Bool = { _ in false }
    private var pencilSurface: () -> UIView? = { nil }
    private var diagnostics = Diagnostics()
    private var cancellationDiagnostics = CancellationDiagnostics()
    private var lastDiagnosticLog = -Double.infinity

    init(control: PlankCustomControl) {
        self.control = control
        super.init(frame:.zero)
        isMultipleTouchEnabled = true
        layer.cornerRadius = 12
        layer.borderWidth = 1
        label.text = control.label.isEmpty ? control.binding.title : control.label
        label.textAlignment = .center
        label.font = .preferredFont(forTextStyle:.body)
        label.adjustsFontForContentSizeCategory = true
        label.adjustsFontSizeToFitWidth = true
        label.minimumScaleFactor = 0.8
        label.numberOfLines = 2
        label.isUserInteractionEnabled = false
        label.translatesAutoresizingMaskIntoConstraints = false
        addSubview(label)
        labelInsets = [
            label.leadingAnchor.constraint(equalTo:leadingAnchor, constant:8),
            label.trailingAnchor.constraint(equalTo:trailingAnchor, constant:-8),
            label.topAnchor.constraint(greaterThanOrEqualTo:topAnchor, constant:4),
            label.bottomAnchor.constraint(lessThanOrEqualTo:bottomAnchor, constant:-4),
        ]
        NSLayoutConstraint.activate(labelInsets + [label.centerYAnchor.constraint(equalTo:centerYAnchor)])
        isAccessibilityElement = true
        accessibilityLabel = label.text
        accessibilityHint = control.behavior == .hold
            ? "Double-tap to hold this shortcut. Double-tap again to release."
            : "Sends this shortcut once."
        refresh()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unsupported") }
    func configure(enabled next: Bool, appearance: PlankIPadControlAppearance,
                   begin: @escaping (UUID, PlankControlBinding) -> Bool,
                   end: @escaping (UUID) -> Void, tap: @escaping (PlankControlBinding) -> Bool,
                   pencilSurface: @escaping () -> UIView?) {
        requestedEnabled = next
        let wasEnabled = enabled
        enabled = next && fingerHitBounds != nil
        if wasEnabled && !enabled { retire(reason:.disabled) }
        self.appearance = appearance
        beginAction = begin; endAction = end; tapAction = tap
        self.pencilSurface = pencilSurface
        refresh()
    }
    func setGeometry(hitFrame: CGRect?,scale: CGFloat) {
        fingerHitBounds = hitFrame.map { $0.offsetBy(dx:-frame.minX,dy:-frame.minY) }
        visualScale = scale.isFinite && scale > 0 ? scale : 1
        label.font = UIFont(descriptor:UIFont.preferredFont(forTextStyle:.body).fontDescriptor,
                            size:UIFont.preferredFont(forTextStyle:.body).pointSize * visualScale)
        layer.cornerRadius = 12 * visualScale; layer.borderWidth = visualScale
        for (index,constraint) in labelInsets.enumerated() {
            constraint.constant = [8,-8,4,-4][index] * visualScale
        }
        let wasEnabled = enabled
        enabled = requestedEnabled && fingerHitBounds != nil
        if wasEnabled && !enabled { retire(reason:.geometry) }
        refresh()
    }
    override func point(inside point: CGPoint,with event: UIEvent?) -> Bool {
        if bounds.contains(point) { return true }
        guard event?.type == .touches else { return false }
        if event?.allTouches?.contains(where:{ $0.type == .indirectPointer }) == true { return false }
        return fingerHitBounds?.contains(point) == true
    }
    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil { retire(reason:.teardown) }
    }
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            if touch.type == .pencil {
                forwardPencilBegin(touch,event:event)
                continue
            }
            if touch.type == .indirectPointer,!bounds.contains(touch.location(in:self)) {
                // Initial hit-testing may precede allTouches. An indirect
                // contact in a finger-only expansion still belongs to canvas.
                forwardPencilBegin(touch,event:event)
                continue
            }
            guard touch.type == .direct else { diagnostics.increment(\.indirect); continue }
            guard let contact = contacts.proposal(id:ObjectIdentifier(touch),source:.direct,
                inside:fingerHitBounds?.contains(touch.location(in:self)) == true,enabled:enabled,
                binding:control.binding,behavior:control.behavior) else {
                diagnostics.increment(\.rejected); continue
            }
            let accepted = contact.behavior == .tap || beginAction(contact.owner,contact.binding)
            guard accepted else { diagnostics.increment(\.rejected); continue }
            guard enabled, contacts.accept(contact) else {
                // A synchronous transport refusal can retire the view while its
                // callback is on the stack. Do not resurrect that finger epoch.
                if contact.behavior == .hold { endAction(contact.owner) }
                diagnostics.increment(\.rejected); continue
            }
            diagnostics.increment(\.accepted)
        }
        refresh(); logIfDue()
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches { forwardPencil(touch,phase:.moved,event:event) }
        // Real lift/cancel releases holds. Small finger drift outside a key
        // while painting is not a second synthetic key-up edge.
        refresh()
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            forwardPencil(touch,phase:.ended,event:event)
            finish(touch, activate:fingerHitBounds?.contains(touch.location(in:self)) == true,cancelled:false)
        }
        refresh(); logIfDue()
    }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        let cancellationLog = recordCancellationContext(touches,event:event)
        for touch in touches { forwardPencil(touch,phase:.cancelled,event:event); finish(touch,activate:false,cancelled:true) }
        refresh(); cancellationLog?(); logIfDue()
    }
    private func recordCancellationContext(_ touches: Set<UITouch>,event: UIEvent?) -> (() -> Void)? {
        // Inspect only real callbacks that will cancel an admitted finger owner;
        // forwarded Pencil retirement and explicit view retirement stay separate.
        var owners = 0, held = 0
        var touchRecognizers = CancellationRecognizers()
        let touchLimit = 32, ancestorLimit = 16
        var truncated = touches.count > touchLimit
        for touch in touches.prefix(touchLimit) {
            guard touch.type == .direct, let contact = contacts.movement(id:ObjectIdentifier(touch)) else { continue }
            owners += 1
            if contact.behavior == .hold { held += 1 }
            if let recognizers = touch.gestureRecognizers {
                if recognizers.count > CancellationRecognizers.limit { truncated = true }
                for recognizer in recognizers.prefix(CancellationRecognizers.limit) {
                    touchRecognizers.inspect(recognizer)
                }
            }
        }
        guard owners > 0 else { return nil }
        let eventTouches = event?.allTouches
        var pencilActive = 0, pencilCancelled = 0, pencilTotal = 0
        if let eventTouches {
            if eventTouches.count > touchLimit { truncated = true }
            for touch in eventTouches.prefix(touchLimit) where touch.type == .pencil {
                pencilTotal += 1
                if touch.phase == .began || touch.phase == .moved || touch.phase == .stationary { pencilActive += 1 }
                if touch.phase == .cancelled { pencilCancelled += 1 }
            }
        }
        var ancestorRecognizers = CancellationRecognizers()
        var view: UIView? = self
        var depth = 0
        while let current = view, depth < ancestorLimit {
            if let recognizers = current.gestureRecognizers {
                if recognizers.count > CancellationRecognizers.limit { truncated = true }
                for recognizer in recognizers.prefix(CancellationRecognizers.limit) {
                    ancestorRecognizers.inspect(recognizer)
                }
            }
            depth += 1; view = current.superview
        }
        truncated = truncated || view != nil || touchRecognizers.truncated || ancestorRecognizers.truncated
        cancellationDiagnostics.increment(\.callbacks)
        cancellationDiagnostics.increment(\.owners,by:owners)
        if pencilActive > 0 { cancellationDiagnostics.increment(\.withPencil) }
        if touchRecognizers.recognizedCanceling > 0 { cancellationDiagnostics.increment(\.withTouchRecognizer) }
        if ancestorRecognizers.recognizedCanceling > 0 { cancellationDiagnostics.increment(\.withAncestorRecognizer) }
        let now = ProcessInfo.processInfo.systemUptime
        guard now - cancellationDiagnostics.lastLog >= 1 else { return nil }
        cancellationDiagnostics.lastLog = now
        let sceneActive: Int
        if let scene = window?.windowScene { sceneActive = scene.activationState == .foregroundActive ? 1 : 0 }
        else { sceneActive = -1 }
        // Bounded anonymous totals and a current callback snapshot only. No
        // binding, text, coordinates, identity, recognizer names or touch arrays.
        // Missing event Pencil/recognizer state cannot prove OS palm rejection.
        let summary = cancellationDiagnostics
        let eventKnown = eventTouches == nil ? 0 : 1
        // Snapshot before ownership changes; emit only after key release. The
        // ready log retains no UITouch, UIView or UIGestureRecognizer objects.
        return {
            NSLog("PLANK control cancellation callbacks=%d ownersTotal=%d pencilConcurrent=%d touchGestureConcurrent=%d ancestorGestureConcurrent=%d owners=%d held=%d eventKnown=%d eventPencilTotal=%d eventPencilActive=%d eventPencilCancelled=%d touchRecognizers=%d touchRecognizedCanceling=%d touchPossibleCanceling=%d touchExclusive=%d touchCategoryMask=%d touchStateMask=%d ancestorRecognizers=%d ancestorRecognizedCanceling=%d ancestorPossibleCanceling=%d ancestorExclusive=%d ancestorCategoryMask=%d ancestorStateMask=%d sceneActive=%d truncated=%d",
                summary.callbacks,summary.owners,summary.withPencil,
                summary.withTouchRecognizer,summary.withAncestorRecognizer,
                owners,held,eventKnown,pencilTotal,pencilActive,pencilCancelled,
                touchRecognizers.count,touchRecognizers.recognizedCanceling,touchRecognizers.possibleCanceling,
                touchRecognizers.exclusiveRecognized,touchRecognizers.categoryMask,touchRecognizers.stateMask,
                ancestorRecognizers.count,ancestorRecognizers.recognizedCanceling,ancestorRecognizers.possibleCanceling,
                ancestorRecognizers.exclusiveRecognized,ancestorRecognizers.categoryMask,ancestorRecognizers.stateMask,
                sceneActive,truncated ? 1 : 0)
        }
    }
    private func finish(_ touch: UITouch, activate: Bool, cancelled: Bool) {
        guard let contact = contacts.finish(id:ObjectIdentifier(touch)) else { return }
        diagnostics.increment(cancelled ? \.cancels : \.ends)
        if contact.behavior == .hold { endAction(contact.owner) }
        else if activate && enabled { _ = tapAction(contact.binding) }
    }
    private func forwardPencilBegin(_ touch: UITouch,event: UIEvent?) {
        let id = ObjectIdentifier(touch)
        guard pencils[id] == nil, pencils.count < 8, let target = pencilSurface(), target !== self else { return }
        pencils[id] = PencilContact(touch:touch,target:target)
        diagnostics.increment(\.pencil)
        // Forward immediately so the target can consume the real coalesced
        // samples in this event and resolve coordinates in its own UIView.
        target.touchesBegan([touch],with:event)
    }
    private func forwardPencil(_ touch: UITouch,phase: UITouch.Phase,event: UIEvent?) {
        let id = ObjectIdentifier(touch)
        guard let contact = pencils[id] else { return }
        if phase == .ended || phase == .cancelled { pencils.removeValue(forKey:id) }
        guard let target = contact.target else { return }
        switch phase {
        case .moved: target.touchesMoved([touch],with:event)
        case .ended: target.touchesEnded([touch],with:event)
        case .cancelled: target.touchesCancelled([touch],with:event)
        default: break
        }
    }
    func retire(reason: PlankIPadCustomControlsOverlayView.RetirementReason = .explicit) {
        let retired = contacts.retire()
        let forwarded = Array(pencils.values); pencils.removeAll()
        let accessibility = accessibilityOwner; accessibilityOwner = nil
        if !retired.isEmpty || accessibility != nil {
            switch reason {
            case .disabled: diagnostics.increment(\.disabled)
            case .layout: diagnostics.increment(\.layout)
            case .geometry: diagnostics.increment(\.geometry)
            case .teardown: diagnostics.increment(\.teardown)
            case .explicit: diagnostics.increment(\.explicit)
            }
        }
        for contact in forwarded { contact.target?.touchesCancelled([contact.touch],with:nil) }
        for contact in retired where contact.behavior == .hold { endAction(contact.owner) }
        if let owner = accessibility { endAction(owner) }
        refresh(); logIfDue()
    }
    override func accessibilityActivate() -> Bool {
        guard enabled else { return false }
        if control.behavior == .tap { return tapAction(control.binding) }
        if let owner = accessibilityOwner {
            accessibilityOwner = nil; endAction(owner)
        } else {
            let owner = UUID()
            guard beginAction(owner,control.binding) else { return false }
            guard enabled else { endAction(owner); return false }
            accessibilityOwner = owner
        }
        refresh()
        UIAccessibility.post(notification:.announcement, argument:accessibilityValue)
        return true
    }
    private func refresh() {
        let held = accessibilityOwner != nil || contacts.hasHeldContact
        let touched = held || !contacts.isEmpty
        backgroundColor = touched
            ? UIColor.systemBlue.withAlphaComponent(appearance.pressedFillAlpha)
            : UIColor(white:0.14,alpha:appearance.idleFillAlpha)
        layer.borderColor = (touched
            ? UIColor.systemBlue.withAlphaComponent(appearance.pressedBorderAlpha)
            : UIColor(white:0.32,alpha:appearance.idleBorderAlpha)).cgColor
        label.textColor = UIColor.white.withAlphaComponent(touched ? appearance.pressedLabelAlpha : appearance.idleLabelAlpha)
        alpha = enabled ? 1 : 0.42
        accessibilityTraits = touched ? [.button,.selected] : .button
        if !enabled { accessibilityTraits.insert(.notEnabled) }
        accessibilityValue = fingerHitBounds == nil ? "Enlarge the window to use this control."
            : !enabled ? "Unavailable" : (held ? "Held" : "Released")
    }
    private func logIfDue() {
        let now = ProcessInfo.processInfo.systemUptime
        guard now - lastDiagnosticLog >= 1,
              diagnostics.accepted + diagnostics.rejected + diagnostics.indirect + diagnostics.pencil > 0 else { return }
        lastDiagnosticLog = now
        // Saturating counts only: no positions, key assignments, text, device
        // identities or comparison codes. Cancellation remains a real release.
        NSLog("PLANK control contacts accepted=%d rejected=%d indirect=%d pencil=%d ended=%d cancelled=%d disabled=%d layout=%d geometry=%d teardown=%d explicit=%d active=%d",
            diagnostics.accepted,diagnostics.rejected,diagnostics.indirect,diagnostics.pencil,
            diagnostics.ends,diagnostics.cancels,diagnostics.disabled,diagnostics.layout,
            diagnostics.geometry,diagnostics.teardown,diagnostics.explicit,contacts.activeCount)
    }
}

/// Draft-based native editor. Its control canvas sends no remote input.
struct PlankIPadCustomControlsEditor: View {
    @ObservedObject var store: PlankIPadCustomControlsStore
    private let referenceSurfaceSize: CGSize
    private let surface: PlankIPadControlSurface
    private let closeAction: (() -> Void)?
    @Environment(\.dismiss) private var dismiss
    @State private var draft: PlankCustomControlLibrary
    @State private var history: [PlankCustomControlLibrary] = []
    @State private var selectedControlID: UUID?
    @State private var editingLandscape = true
    @State private var inspectorVisible = true
    @State private var narrowInspector = false
    @State private var dragStart: PlankControlPlacement?
    @State private var resizeStart: PlankControlPlacement?
    @State private var resizeMode = false
    @State private var canvasSize = CGSize.zero
    @State private var snapGuides: [PlankControlSnap.Guide] = []
    // Reference-only counters do not publish view updates or retain controls.
    @State private var gestureDiagnostics = PlankIPadEditorGestureDiagnostics()
    private enum NameTarget { case layout(UUID), control(UUID) }
    @State private var nameTarget: NameTarget?
    @State private var nameDraft = ""
    @State private var namingFromInspector = false
    @State private var showingLayoutMenu = false
    @State private var pendingLayoutRename: UUID?
    @State private var nameReportCount = 0

    init(store: PlankIPadCustomControlsStore, referenceSurfaceSize: CGSize = .zero,
         surface: PlankIPadControlSurface = .desktop,close: (() -> Void)? = nil) {
        self.store = store
        self.referenceSurfaceSize = referenceSurfaceSize
        self.surface = surface
        self.closeAction = close
        _editingLandscape = State(initialValue:store.sceneLandscape)
        _draft = State(initialValue:store.library)
        _selectedControlID = State(initialValue:store.selectedLayout.controls.first(where:{ $0.binding.code == 0x20 })?.id
                                   ?? store.selectedLayout.controls.first?.id)
    }

    private var selectedLayout: PlankControlLayout { draft.selectedLayout }
    private var selectedControl: PlankCustomControl? {
        selectedLayout.controls.first(where:{ $0.id == selectedControlID })
    }
    private var usesSheetInspector: Bool { previewGeometry(in:canvasSize).previewFrame.width < 620 }
    private var inspectorPresented: Bool { usesSheetInspector ? narrowInspector : inspectorVisible }
    private var isRenamingLayout: Bool {
        if case .layout = nameTarget { return true }
        return false
    }
    private var unavailableEditingCount: Int {
        let bounds = previewGeometry(in:canvasSize).referenceBounds
        return controlResolution(in:bounds).unavailableIDs.count
    }
    private var hasMeasuredEditingChrome: Bool {
        store.hasMeasuredChrome(referenceSize:liveWindowBounds.size,surface:surface)
    }
    var body: some View {
        NavigationStack {
            VStack(spacing:0) {
                if !isRenamingLayout { editorToolbar }
                if case let .layout(id) = nameTarget {
                    layoutNameEditor(id:id)
                }
                GeometryReader { geometry in
                    ZStack(alignment:.topLeading) {
                        Color(white:0.035)
                            .contentShape(Rectangle())
                            .onTapGesture(perform:deselectControl)
                        canvas(in:geometry.size).allowsHitTesting(!isRenamingLayout)
                    }
                    .clipped()
                    .onAppear { canvasSize = geometry.size }
                    .onChange(of:geometry.size) { _,value in
                        gestureDiagnostics.count(.canvasChanges)
                        if dragStart != nil || resizeStart != nil { gestureDiagnostics.count(.activeCanvasChanges) }
                        canvasSize = value; dragStart = nil; resizeStart = nil; snapGuides = []
                    }
                }
                Text(!hasMeasuredEditingChrome ? "Menu clearance is not yet measured for this window."
                               : unavailableEditingCount > 0 ? "This window needs more room for \(unavailableEditingCount) controls. Enlarge the window or adjust the red controls."
                               : resizeMode ? "Drag a corner to resize. Sizes are shown at full screen."
                                : "Drag controls to move. Layouts are shared by iPad Desktop and Pencil Sharing.")
                    .font(.footnote).foregroundStyle(.secondary)
                    .padding(.horizontal).padding(.vertical,10)
                    .frame(maxWidth:.infinity).background(.bar)
            }
            .background { PlankIPadNavigationDiagnostics(role:"editor") }
            .navigationTitle("Edit Controls")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement:.cancellationAction) { Button("Cancel") { closeEditor() } }
                ToolbarItem(placement:.confirmationAction) {
                    Button("Done") { finishEditor() }.fontWeight(.semibold)
                        .disabled(!draft.isValid || isRenamingLayout)
                }
            }
            .sheet(isPresented:$narrowInspector,onDismiss:hideControlInspector) {
                NavigationStack {
                    inspector
                    .navigationTitle("Control")
                    .navigationBarTitleDisplayMode(.inline)
                    .toolbar { ToolbarItem(placement:.confirmationAction) { Button("Done",action:hideControlInspector) } }
                }
                .alert(nameTitle,isPresented:namePresentation(fromInspector:true),presenting:nameTarget) { target in
                    nameActions(for:target)
                } message: { _ in Text(nameMessage) }
                .presentationDetents([.medium,.large])
                .presentationDragIndicator(.visible)
                .preferredColorScheme(.dark)
            }
        }
        .alert(nameTitle,isPresented:namePresentation(fromInspector:false),presenting:nameTarget) { target in
            nameActions(for:target)
        } message: { _ in Text(nameMessage) }
        .preferredColorScheme(.dark)
        .statusBarHidden(false)
        .onAppear { gestureDiagnostics.report(reason:0,force:true) }
        .onDisappear { gestureDiagnostics.report(reason:3,force:true) }
    }

    private var editorToolbar: some View {
        HStack(spacing:12) {
            Button { showingLayoutMenu = true } label: {
                Label(selectedLayout.name,systemImage:"square.stack").lineLimit(1)
            }
            .accessibilityLabel("Layout: \(selectedLayout.name)")
            .popover(isPresented:$showingLayoutMenu) {
              ScrollView {
               VStack(alignment:.leading,spacing:8) {
                ForEach(draft.layouts) { layout in
                    Button {
                        showingLayoutMenu = false
                        remember(); _ = draft.select(id:layout.id)
                        selectedControlID = nil
                    } label: {
                        if layout.id == draft.selectedID { Label(layout.name,systemImage:"checkmark") }
                        else { Text(layout.name) }
                    }
                    .accessibilityLabel(layout.name)
                    .frame(minHeight:44)
                }
                Divider()
                Button("New Layout",systemImage:"plus") { showingLayoutMenu = false; addLayout() }
                    .disabled(draft.layouts.count >= PlankCustomControlLibrary.maximumLayouts)
                Button("Rename Layout…",systemImage:"pencil") {
                    pendingLayoutRename = selectedLayout.id
                    showingLayoutMenu = false
                }
                Button("Copy Layout",systemImage:"doc.on.doc") { showingLayoutMenu = false; copyLayout() }
                    .disabled(draft.layouts.count >= PlankCustomControlLibrary.maximumLayouts)
                Button("Reset Controls",systemImage:"arrow.counterclockwise") { showingLayoutMenu = false; resetLayout() }
                Button("Delete Layout",systemImage:"trash",role:.destructive) {
                    showingLayoutMenu = false; remember(); _ = draft.remove(id:selectedLayout.id); selectedControlID = nil
                }.disabled(draft.layouts.count <= 1)
               }.padding().buttonStyle(.borderless)
              }.frame(width:300).frame(maxHeight:500)
                .presentationCompactAdaptation(.popover)
                .onDisappear(perform:completeLayoutRenameRequest)
            }
            Spacer(minLength:0)
            Picker("Arrangement",selection:$editingLandscape) {
                Text("Landscape").tag(true)
                Text("Portrait").tag(false)
            }
            .pickerStyle(.segmented).frame(maxWidth:240)
            .onChange(of:editingLandscape) { _,_ in dragStart = nil; resizeStart = nil; snapGuides = [] }
            Button {
                resizeMode.toggle(); dragStart = nil; resizeStart = nil; snapGuides = []
            } label: { Image(systemName:"arrow.up.left.and.arrow.down.right") }
                .tint(resizeMode ? .blue : .primary)
                .accessibilityLabel("Resize controls")
                .accessibilityValue(resizeMode ? "On" : "Off")
                .accessibilityHint("When on, drag a selected control's corner to resize it. When off, drag controls to move them.")
            Button { addControl() } label: { Image(systemName:"plus") }
                .accessibilityLabel("Add control")
                .disabled(selectedLayout.controls.count >= PlankControlLayout.maximumControls)
            Button {
                guard let previous = history.popLast() else { return }
                draft = previous
                if selectedControl == nil { selectedControlID = nil }
            } label: { Image(systemName:"arrow.uturn.backward") }
                .accessibilityLabel("Undo edit").disabled(history.isEmpty)
        }
        .buttonStyle(.borderless)
        .padding(.horizontal,16).padding(.vertical,10)
        .frame(minHeight:52).background(.bar)
    }

    @ViewBuilder private func canvas(in available: CGSize) -> some View {
        let geometry = previewGeometry(in:available)
        let canvas = geometry.previewFrame
        let bounds = CGRect(origin:.zero,size:canvas.size)
        let chrome = liveSurfaceGeometry().referenceChrome
        let resolution = controlResolution(in:geometry.referenceBounds)
        ZStack(alignment:.topLeading) {
            RoundedRectangle(cornerRadius:16)
                .fill(Color(white:0.045))
                .overlay { RoundedRectangle(cornerRadius:16).stroke(Color.white.opacity(0.10),lineWidth:1) }
                // A sibling background owns empty-space taps. Controls,
                // resize handles and the foreground inspector keep their own
                // gestures instead of competing with an ancestor recognizer.
                .contentShape(Rectangle())
                .onTapGesture(perform:deselectControl)
            // The live canvas is already fitted inside the window's top/bottom
            // menu clearance. Only floating chrome that intersects that canvas
            // belongs inside this preview of the authored tablet.
            ForEach(Array(chrome.reservedRects.enumerated()),id:\.offset) { _, area in
                let reserved = geometry.preview(frame:area)
                Rectangle().fill(Color.gray.opacity(0.24))
                    .overlay { Rectangle().stroke(Color.gray.opacity(0.6),style:StrokeStyle(lineWidth:1,dash:[4,4])) }
                    .frame(width:reserved.width,height:reserved.height)
                    .position(x:reserved.midX,y:reserved.midY)
                    .allowsHitTesting(false).accessibilityHidden(true)
            }
            if selectedLayout.controls.isEmpty {
                VStack(spacing:12) {
                    Image(systemName:"hand.tap").font(.largeTitle).foregroundStyle(.secondary)
                    Text("Add a control, then move and resize it.").font(.body).foregroundStyle(.secondary)
                    Button("Add Control",systemImage:"plus") { addControl() }.buttonStyle(.bordered)
                }
                .frame(maxWidth:.infinity,maxHeight:.infinity)
            }
            ForEach(Array(snapGuides.enumerated()),id:\.offset) { _, guide in
                Path { path in
                    if guide.axis == .vertical {
                        path.move(to:previewPoint(CGPoint(x:guide.position,y:guide.start),geometry:geometry))
                        path.addLine(to:previewPoint(CGPoint(x:guide.position,y:guide.end),geometry:geometry))
                    } else {
                        path.move(to:previewPoint(CGPoint(x:guide.start,y:guide.position),geometry:geometry))
                        path.addLine(to:previewPoint(CGPoint(x:guide.end,y:guide.position),geometry:geometry))
                    }
                }
                .stroke(Color.blue.opacity(0.8),style:StrokeStyle(lineWidth:1,dash:[4,4]))
                .allowsHitTesting(false)
            }
            ForEach(selectedLayout.controls) { control in
                let authored = resolvedFrame(control,resolution:resolution,in:geometry.referenceBounds)
                editorControl(control,frame:geometry.preview(frame:authored),geometry:geometry,
                              unavailable:resolution.unavailableIDs.contains(control.id))
            }
            if let selected = selectedControl, inspectorVisible, canvas.width >= 620 {
                let frame = geometry.preview(frame:resolvedFrame(selected,resolution:resolution,in:geometry.referenceBounds))
                let inspectorSize = CGSize(width:min(304,canvas.width - 32),height:min(430,canvas.height - 24))
                inspector
                    .frame(width:inspectorSize.width,height:inspectorSize.height)
                    .background(.regularMaterial,in:RoundedRectangle(cornerRadius:16))
                    .position(inspectorCenter(beside:frame,size:inspectorSize,bounds:bounds))
            }
            if let selected = selectedControl {
                let frame = geometry.preview(frame:resolvedFrame(selected,resolution:resolution,in:geometry.referenceBounds))
                Button {
                    if usesSheetInspector { narrowInspector = true; inspectorVisible = true }
                    else { inspectorVisible.toggle() }
                } label: { Image(systemName:"slider.horizontal.3") }
                .font(.body).frame(width:44,height:44)
                .background(.regularMaterial,in:Circle())
                .position(inspectorToggleCenter(beside:frame,bounds:bounds))
                .accessibilityLabel(inspectorPresented ? "Hide control inspector" : "Show control inspector")
                .accessibilityValue(inspectorPresented ? "Visible" : "Hidden")
            }
        }
        .frame(width:canvas.width,height:canvas.height)
        .clipped()
        .coordinateSpace(name:"plank-custom-controls-editor-canvas")
        .position(x:canvas.midX,y:canvas.midY)
        .accessibilityElement(children:.contain)
        .accessibilityLabel(editingLandscape ? "Landscape arrangement" : "Portrait arrangement")
        .accessibilityHint("Shaded areas show floating client controls that intersect the tablet.")
    }

    private func editorControl(_ control: PlankCustomControl,frame: CGRect,
                               geometry: PlankControlPreviewGeometry,unavailable: Bool) -> some View {
        let selected = selectedControlID == control.id
        let presentationScale = geometry.scale
        return ZStack {
            RoundedRectangle(cornerRadius:12 * presentationScale)
                .fill(selected ? Color.blue.opacity(0.28) : Color(white:0.14))
                .overlay { RoundedRectangle(cornerRadius:12 * presentationScale).stroke(unavailable ? Color.red : selected ? Color.blue : Color.white.opacity(0.28),lineWidth:selected || unavailable ? 2 : 1) }
            Text(control.label.isEmpty ? control.binding.title : control.label)
                .font(.system(size:UIFont.preferredFont(forTextStyle:.body).pointSize * presentationScale,weight:.medium))
                .lineLimit(2).minimumScaleFactor(0.8)
                .padding(.horizontal,8 * presentationScale).foregroundStyle(.white)
            if selected && resizeMode {
                ForEach(0..<4,id:\.self) { corner in
                    resizeHandle(control,corner:corner,geometry:geometry)
                        .position(x:corner % 2 == 0 ? 0 : frame.width,
                                  y:corner < 2 ? 0 : frame.height)
                }
            }
        }
        .frame(width:frame.width,height:frame.height)
        .contentShape(Rectangle())
        .position(x:frame.midX,y:frame.midY)
        .onGeometryChange(for:CGRect.self) { $0.frame(in:.named("plank-custom-controls-editor-canvas")) } action: { _ in
            gestureDiagnostics.count(.frameChanges)
        }
        .onTapGesture { gestureDiagnostics.count(.taps); select(control.id) }
        .gesture(DragGesture(minimumDistance:4,coordinateSpace:.named("plank-custom-controls-editor-canvas"))
            .onChanged { value in
                gestureDiagnostics.count(.moves)
                guard !resizeMode else {
                    gestureDiagnostics.count(.moveModeBlocks); gestureDiagnostics.report(); return
                }
                if dragStart == nil {
                    remember(); materializeResolvedPlacement(id:control.id,in:geometry.referenceBounds)
                    select(control.id,showInspector:false)
                    dragStart = PlankControlPlacement(frame:resolvedFrame(control,resolution:controlResolution(in:geometry.referenceBounds),
                                                                          in:geometry.referenceBounds),in:geometry.referenceBounds)
                }
                guard var next = dragStart else { return }
                let translation = geometry.referenceTranslation(value.translation)
                let bounds = geometry.referenceBounds
                next.x = min(max(next.x + translation.width / max(bounds.width,1),0),1)
                next.y = min(max(next.y + translation.height / max(bounds.height,1),0),1)
                let result = PlankControlSnap.move(placement:next,in:bounds,peers:peerFrames(excluding:control.id,in:geometry.referenceBounds),
                                                  gutter:6,threshold:8 / max(geometry.scale,0.01),
                                                  chrome:editingSnapChrome)
                snapGuides = result.guides
                let previous = selectedLayout.controls.first(where:{ $0.id == control.id })?.placement(landscape:editingLandscape)
                setPlacement(result.placement,id:control.id)
                let current = selectedLayout.controls.first(where:{ $0.id == control.id })?.placement(landscape:editingLandscape)
                gestureDiagnostics.count(previous == current ? .unchangedProposals : .changedProposals)
                gestureDiagnostics.report()
            }
            .onEnded { _ in
                dragStart = nil; snapGuides = []
                gestureDiagnostics.count(.ends); gestureDiagnostics.report(reason:2)
            })
        .accessibilityElement(children:.ignore)
        .accessibilityLabel("\(control.label), \(control.binding.title)")
        .accessibilityValue(unavailable ? "Needs more room in the current window." : "")
        .accessibilityHint(resizeMode ? "Select, then drag a corner to resize. Exact size is also available in the inspector."
                           : "Drag to move. Select to edit binding, size and position.")
        .accessibilityAddTraits(selected ? [.isButton,.isSelected] : .isButton)
        .accessibilityAction { select(control.id) }
    }

    private func resizeHandle(_ control: PlankCustomControl,corner: Int,
                              geometry: PlankControlPreviewGeometry) -> some View {
        Circle().fill(.white).frame(width:max(6,10 * geometry.scale),height:max(6,10 * geometry.scale))
            .frame(width:44,height:44).contentShape(Rectangle())
            .highPriorityGesture(DragGesture(minimumDistance:0,coordinateSpace:.named("plank-custom-controls-editor-canvas"))
                .onChanged { value in
                    gestureDiagnostics.count(.resizes)
                    if resizeStart == nil {
                        remember(); materializeResolvedPlacement(id:control.id,in:geometry.referenceBounds)
                        hideControlInspector()
                        resizeStart = PlankControlPlacement(frame:resolvedFrame(control,resolution:controlResolution(in:geometry.referenceBounds),
                                                                                in:geometry.referenceBounds),in:geometry.referenceBounds)
                    }
                    guard let original = resizeStart else { return }
                    let bounds = geometry.referenceBounds
                    let result = PlankControlSnap.resize(original:original,corner:corner,
                        translation:geometry.referenceTranslation(value.translation),in:bounds,
                        peers:peerFrames(excluding:control.id,in:geometry.referenceBounds),gutter:6,threshold:8 / max(geometry.scale,0.01),
                        chrome:editingSnapChrome)
                    snapGuides = result.guides
                    let previous = selectedLayout.controls.first(where:{ $0.id == control.id })?.placement(landscape:editingLandscape)
                    setPlacement(result.placement,id:control.id)
                    let current = selectedLayout.controls.first(where:{ $0.id == control.id })?.placement(landscape:editingLandscape)
                    gestureDiagnostics.count(previous == current ? .unchangedProposals : .changedProposals)
                    gestureDiagnostics.report()
                }
                .onEnded { _ in
                    resizeStart = nil; snapGuides = []
                    gestureDiagnostics.count(.ends); gestureDiagnostics.report(reason:2)
                })
            .accessibilityHidden(true)
    }

    private func peerFrames(excluding id: UUID,in bounds: CGRect) -> [CGRect] {
        controlResolution(in:bounds).frames.filter { $0.key != id }.map(\.value)
    }
    private func controlResolution(in bounds: CGRect) -> PlankControlPlacementResolver.Resolution {
        let value = PlankControlResponsiveResolution.resolve(layout:selectedLayout,landscape:editingLandscape,
            chrome:store.chromeGeometry(referenceSize:liveWindowBounds.size,surface:surface),
            legacyReferenceSize:store.legacyReferenceSize(landscape:editingLandscape))
        let frames = Dictionary(uniqueKeysWithValues:selectedLayout.controls.map { control in
            (control.id,PlankControlPlacementResolver.pointSizedFrame(control.placement(landscape:editingLandscape),in:bounds))
        })
        return .init(frames:frames,unavailableIDs:value.unavailableIDs)
    }
    private var authoredReferenceSize: CGSize {
        selectedLayout.referenceExtent(landscape:editingLandscape,
            legacySize:store.legacyReferenceSize(landscape:editingLandscape))?.size ?? .zero
    }
    private var liveWindowBounds: CGRect {
        CGRect(origin:.zero,size:store.latestSurfaceSize(for:surface) ?? referenceSurfaceSize)
    }
    private func liveSurfaceGeometry() -> PlankControlSurfaceGeometry {
        PlankControlSurfaceGeometry(referenceSize:authoredReferenceSize,
            chrome:store.chromeGeometry(referenceSize:liveWindowBounds.size,surface:surface))
    }
    private var editingSnapChrome: PlankControlChromeGeometry? {
        let geometry = liveSurfaceGeometry()
        // Unknown live clearance disables live targets, but the local saved
        // canvas remains editable without inventing window menu measurements.
        return geometry.isUsable ? geometry.referenceChrome : nil
    }
    private func previewPoint(_ point: CGPoint,geometry: PlankControlPreviewGeometry) -> CGPoint {
        CGPoint(x:(point.x - geometry.referenceBounds.minX) * geometry.scale,
                y:(point.y - geometry.referenceBounds.minY) * geometry.scale)
    }
    private func resolvedFrame(_ control: PlankCustomControl,resolution: PlankControlPlacementResolver.Resolution,in bounds: CGRect) -> CGRect {
        resolution.frames[control.id] ?? PlankControlPlacementResolver.pointSizedFrame(control.placement(landscape:editingLandscape),
            in:bounds)
    }
    private func materializeResolvedPlacement(id: UUID,in bounds: CGRect) {
        let resolution = controlResolution(in:bounds)
        var layout = selectedLayout
        guard let index = layout.controls.firstIndex(where:{ $0.id == id }),
              let frame = resolution.frames[id] else { return }
        layout.controls[index].setPlacement(PlankControlPlacement(frame:frame,in:bounds),landscape:editingLandscape)
        // An explicit geometry edit adopts only its own displayed anchor into
        // the draft. Every other control retains its exact saved placement.
        // Undo predates this step; passive rendering never rewrites anchors.
        replaceDraft(layout)
    }

    private var inspector: some View {
        ScrollView {
            VStack(alignment:.leading,spacing:16) {
                HStack {
                    Text("Control").font(.headline)
                    Spacer()
                    if !usesSheetInspector {
                        Button(action:hideControlInspector) {
                            Image(systemName:"xmark.circle.fill").foregroundStyle(.secondary)
                                .frame(width:44,height:44)
                        }
                            .accessibilityLabel("Hide inspector")
                    }
                }
                if let control = selectedControl {
                    VStack(alignment:.leading,spacing:8) {
                        Text("Button label").font(.subheadline).foregroundStyle(.secondary)
                        Button { beginNameEdit(.control(control.id)) } label: {
                            HStack {
                                Text(control.label.isEmpty ? control.automaticLabel : control.label)
                                    .foregroundStyle(.primary).lineLimit(2)
                                Spacer()
                                Image(systemName:"pencil").foregroundStyle(.secondary)
                            }.frame(maxWidth:.infinity,minHeight:44)
                        }
                        .buttonStyle(.bordered)
                        .accessibilityLabel("Edit button label: \(control.label)")
                        if control.label != control.automaticLabel {
                            Button("Use Key Name") { mutateControl { $0.useBindingLabel() } }
                                .font(.subheadline)
                        }
                    }
                    Picker("Key",selection:keyBinding) {
                        ForEach(PlankControlKeyCatalog.entries) { key in Text(key.title).tag(key.code) }
                    }
                    .pickerStyle(.menu)
                    VStack(alignment:.leading,spacing:8) {
                        Text("Modifiers").font(.subheadline).foregroundStyle(.secondary)
                        HStack(spacing:6) {
                            modifierToggle("⇧",name:"Shift",mask:1)
                            modifierToggle("⌃",name:"Control",mask:2)
                            modifierToggle("⌥",name:"Option",mask:4)
                            modifierToggle("⌘",name:"Command",mask:8)
                        }
                    }
                    Picker("Behavior",selection:controlBinding(\.behavior,fallback:.hold)) {
                        Text("Hold").tag(PlankControlBehavior.hold)
                        Text("Tap").tag(PlankControlBehavior.tap)
                    }.pickerStyle(.segmented)
                    Text(selectedControl?.behavior == .hold ? "Held until the finger lifts." : "Sends one complete shortcut.")
                        .font(.footnote).foregroundStyle(.secondary)
                    Divider()
                    dimension("Width",key:\.width,range:44...480)
                    dimension("Height",key:\.height,range:44...240)
                    position("Horizontal",key:\.x)
                    position("Vertical",key:\.y)
                    HStack {
                        Button("Duplicate",systemImage:"plus.square.on.square") { duplicateControl() }
                            .disabled(selectedLayout.controls.count >= PlankControlLayout.maximumControls)
                        Spacer()
                        Button(role:.destructive) { deleteControl() } label: { Image(systemName:"trash") }
                            .accessibilityLabel("Delete control")
                    }
                }
                if !draft.isValid {
                    Text("Give the layout and each control a name to save your changes.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }.padding(16)
        }
        .scrollBounceBehavior(.basedOnSize)
    }

    private func modifierToggle(_ label: String,name: String,mask: UInt8) -> some View {
        let selected = (selectedControl?.binding.modifiers ?? 0) & mask != 0
        return Button {
            mutateControl { control in
                var binding = control.binding; binding.modifiers ^= mask
                control.setBinding(binding)
            }
        } label: { Text(label).font(.title3).frame(maxWidth:.infinity,minHeight:44) }
        .buttonStyle(.bordered)
        .tint(selected ? .blue : .gray)
        .accessibilityLabel(name)
        .accessibilityValue(selected ? "Included" : "Not included")
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
    private func dimension(_ title: String,key: WritableKeyPath<PlankControlPlacement,Double>,range: ClosedRange<Double>) -> some View {
        Stepper(value:placementBinding(key),in:range,step:4) {
            HStack { Text(title); Spacer(); Text(placementValue(key),format:.number.precision(.fractionLength(0))).monospacedDigit(); Text("pt").foregroundStyle(.secondary) }
        }
        .font(.subheadline)
    }
    private func position(_ title: String,key: WritableKeyPath<PlankControlPlacement,Double>) -> some View {
        Stepper(value:placementBinding(key),in:0...1,step:0.01) {
            HStack { Text(title); Spacer(); Text(placementValue(key),format:.percent.precision(.fractionLength(0))).monospacedDigit() }
        }.font(.subheadline)
    }
    private func placementValue(_ key: KeyPath<PlankControlPlacement,Double>) -> Double {
        guard let control = selectedControl else { return 0 }
        let bounds = previewGeometry(in:canvasSize).referenceBounds
        return PlankControlPlacement(frame:resolvedFrame(control,resolution:controlResolution(in:bounds),in:bounds),
            in:bounds)[keyPath:key]
    }
    private func placementBinding(_ key: WritableKeyPath<PlankControlPlacement,Double>) -> Binding<Double> {
        Binding(get:{ placementValue(key) },set:{ value in
            mutateControl(materializeGeometry:true) { control in
                var placement = control.placement(landscape:editingLandscape)
                placement[keyPath:key] = value
                let bounds = previewGeometry(in:canvasSize).referenceBounds
                control.setPlacement(clampedPlacement(placement,in:bounds),landscape:editingLandscape)
            }
        })
    }
    private var nameTitle: String {
        switch nameTarget {
        case .layout: return "Rename Layout"
        case .control: return "Button Label"
        case nil: return "Name"
        }
    }
    private var nameMessage: String {
        if case .layout = nameTarget { return "This name identifies the saved arrangement of controls." }
        return "The label shown on this button. Its key and behavior stay unchanged."
    }
    private func layoutNameEditor(id: UUID) -> some View {
        VStack(alignment:.leading,spacing:8) {
            Text("Rename Layout").font(.headline)
            HStack(spacing:12) {
                PlankIPadLocalNameField(text:$nameDraft,submit:{ saveName(nameDraft,for:.layout(id)) })
                    .frame(height:44)
                    .accessibilityLabel("Layout name")
                Button("Cancel") { nameTarget = nil }
                    .accessibilityLabel("Cancel rename")
                Button("Save") { saveName(nameDraft,for:.layout(id)) }
                    .buttonStyle(.borderedProminent)
                    .accessibilityLabel("Save layout name")
                    .disabled(nameDraft.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty)
            }
        }
        .padding(.horizontal,16).padding(.vertical,10)
        .background(.bar)
        .onAppear { reportName(phase:1,target:.layout(id)) }
    }
    private func namePresentation(fromInspector: Bool) -> Binding<Bool> {
        // Layout naming stays in this editor hierarchy. Only button-label
        // editing uses the existing native alert on its visible inspector.
        Binding(get:{
            if case .control = nameTarget { return namingFromInspector == fromInspector }
            return false
        },set:{ presented in
            if !presented && namingFromInspector == fromInspector,
               case .control = nameTarget { nameTarget = nil }
        })
    }
    @ViewBuilder private func nameActions(for target: NameTarget) -> some View {
        TextField(nameTitle,text:$nameDraft)
            .textInputAutocapitalization(.sentences).autocorrectionDisabled()
            .accessibilityLabel(nameTitle)
        Button("Cancel",role:.cancel) { nameTarget = nil }
        Button("Save") { saveName(nameDraft,for:target) }
            .disabled(nameDraft.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty)
    }
    private func beginNameEdit(_ target: NameTarget) {
        switch target {
        case let .layout(id):
            guard let layout = draft.layouts.first(where:{ $0.id == id }) else { return }
            nameDraft = layout.name
        case let .control(id):
            guard let control = selectedLayout.controls.first(where:{ $0.id == id }) else { return }
            nameDraft = control.label
        }
        if case .layout = target {
            hideControlInspector()
            namingFromInspector = false
        } else { namingFromInspector = narrowInspector }
        nameTarget = target
        reportName(phase:0,target:target)
    }
    private func completeLayoutRenameRequest() {
        guard let id = pendingLayoutRename else { return }
        pendingLayoutRename = nil
        Task { @MainActor in
            await Task.yield()
            beginNameEdit(.layout(id))
        }
    }
    private func saveName(_ value: String,for target: NameTarget) {
        let text = value.trimmingCharacters(in:.whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        reportName(phase:2,target:target)
        switch target {
        case let .layout(id):
            guard let index = draft.layouts.firstIndex(where:{ $0.id == id }) else { return }
            remember(); draft.layouts[index].name = String(text.prefix(48))
        case let .control(id):
            var layout = selectedLayout
            guard let index = layout.controls.firstIndex(where:{ $0.id == id }) else { return }
            remember(); layout.controls[index].label = String(text.prefix(40)); replaceDraft(layout)
        }
        nameTarget = nil
    }
    private func reportName(phase: Int,target: NameTarget) {
        guard nameReportCount < 16 else { return }
        nameReportCount += 1
        let ordinal: Int
        if case let .layout(id) = target { ordinal = draft.layouts.firstIndex(where:{ $0.id == id }) ?? -1 }
        else { ordinal = -1 }
        NSLog("PLANK editor name phase=%d target=%d selected=%d layouts=%d landscape=%d",
            phase,ordinal,draft.layouts.firstIndex(where:{ $0.id == draft.selectedID }) ?? -1,
            draft.layouts.count,editingLandscape)
    }
    private func closeEditor() {
        if let closeAction { closeAction() } else { dismiss() }
    }
    private func finishEditor() {
        var committed = draft
        if let index = committed.layouts.firstIndex(where:{ $0.id == committed.selectedID }) {
            for landscape in [false,true] {
                let size = committed.layouts[index].referenceExtent(landscape:landscape,
                    legacySize:store.legacyReferenceSize(landscape:landscape))?.size ?? .zero
                if size != .zero { _ = committed.layouts[index].materializeReferenceExtent(landscape:landscape,size:size) }
            }
        }
        store.replace(committed); closeEditor()
    }
    private var keyBinding: Binding<UInt16> {
        Binding(get:{ selectedControl?.binding.code ?? 0x20 },set:{ value in
            mutateControl { control in
                var binding = control.binding; binding.code = value
                control.setBinding(binding)
            }
        })
    }
    private func controlBinding<Value>(_ key: WritableKeyPath<PlankCustomControl,Value>,fallback: Value) -> Binding<Value> {
        Binding(get:{ selectedControl?[keyPath:key] ?? fallback },set:{ value in mutateControl { $0[keyPath:key] = value } })
    }
    private func select(_ id: UUID,showInspector: Bool = true) {
        selectedControlID = id
        if showInspector {
            inspectorVisible = true
            if usesSheetInspector { narrowInspector = true }
        } else { hideControlInspector() }
    }
    private func hideControlInspector() {
        inspectorVisible = false
        narrowInspector = false
    }
    private func deselectControl() {
        gestureDiagnostics.count(.deselections)
        selectedControlID = nil
        hideControlInspector()
        dragStart = nil; resizeStart = nil; snapGuides = []
    }
    private func remember() {
        if history.last != draft { history.append(draft) }
        if history.count > 32 { history.removeFirst() }
    }
    private func mutateControl(materializeGeometry: Bool = false,_ action: (inout PlankCustomControl) -> Void) {
        guard let id = selectedControlID else { return }
        remember()
        if materializeGeometry { materializeResolvedPlacement(id:id,in:previewGeometry(in:canvasSize).referenceBounds) }
        var layout = selectedLayout
        guard let index = layout.controls.firstIndex(where:{ $0.id == id }) else { return }
        action(&layout.controls[index]); replaceDraft(layout)
    }
    private func setPlacement(_ placement: PlankControlPlacement,id: UUID) {
        var layout = selectedLayout
        guard let index = layout.controls.firstIndex(where:{ $0.id == id }) else { return }
        layout.controls[index].setPlacement(placement,landscape:editingLandscape)
        replaceDraft(layout)
    }
    private func replaceDraft(_ layout: PlankControlLayout) {
        // Local text edits may briefly be empty. Keep that draft visible and
        // disable Done instead of silently rejecting a text-field keystroke.
        guard let index = draft.layouts.firstIndex(where:{ $0.id == layout.id }) else { return }
        draft.layouts[index] = layout
    }
    private func addControl() {
        guard selectedLayout.controls.count < PlankControlLayout.maximumControls else { return }
        remember()
        var layout = selectedLayout
        let placement = PlankControlPlacement(x:0.5,y:0.5,width:88,height:52)
        let control = PlankCustomControl(id:UUID(),label:"Space",binding:.init(code:0x20),behavior:.hold,
                                         portrait:placement,landscape:placement)
        layout.controls.append(control); replaceDraft(layout); select(control.id)
    }
    private func duplicateControl() {
        guard let original = selectedControl, selectedLayout.controls.count < PlankControlLayout.maximumControls else { return }
        gestureDiagnostics.count(.duplicates)
        remember(); var layout = selectedLayout
        var copied = original; copied.id = UUID()
        copied.portrait.x = min(copied.portrait.x + 0.04,1)
        copied.portrait.y = min(copied.portrait.y + 0.04,1)
        copied.landscape.x = min(copied.landscape.x + 0.04,1)
        copied.landscape.y = min(copied.landscape.y + 0.04,1)
        layout.controls.append(copied); replaceDraft(layout); select(copied.id)
    }
    private func deleteControl() {
        guard let id = selectedControlID else { return }
        remember(); var layout = selectedLayout; layout.controls.removeAll(where:{ $0.id == id })
        replaceDraft(layout); selectedControlID = nil; narrowInspector = false
    }
    private func addLayout() {
        guard draft.layouts.count < PlankCustomControlLibrary.maximumLayouts else { return }
        remember(); _ = draft.upsert(layout:.defaults(name:"New Layout")); selectedControlID = nil
    }
    private func copyLayout() {
        guard draft.layouts.count < PlankCustomControlLibrary.maximumLayouts else { return }
        remember(); _ = draft.duplicate(id:selectedLayout.id); selectedControlID = nil
    }
    private func resetLayout() {
        remember(); var replacement = PlankControlLayout.defaults(name:selectedLayout.name)
        replacement.id = selectedLayout.id
        replaceDraft(replacement); selectedControlID = nil
    }
    private func previewGeometry(in available: CGSize) -> PlankControlPreviewGeometry {
        PlankControlPreviewGeometry(referenceSize:authoredReferenceSize,
                                    availableSize:available,inset:16)
    }
    private func inspectorToggleCenter(beside frame: CGRect,bounds: CGRect) -> CGPoint {
        let radius: CGFloat = 22, gap: CGFloat = 8
        let right = frame.maxX + gap + radius
        let left = frame.minX - gap - radius
        let x = right + radius <= bounds.maxX ? right : (left - radius >= bounds.minX ? left : bounds.midX)
        return CGPoint(x:min(max(x,bounds.minX + radius),max(bounds.minX + radius,bounds.maxX - radius)),
                       y:min(max(frame.midY,bounds.minY + radius),max(bounds.minY + radius,bounds.maxY - radius)))
    }
    private func inspectorCenter(beside frame: CGRect,size: CGSize,bounds: CGRect) -> CGPoint {
        let gap: CGFloat = 16
        let minX = size.width/2 + 12, maxX = max(minX,bounds.width - size.width/2 - 12)
        let minY = size.height/2 + 12, maxY = max(minY,bounds.height - size.height/2 - 12)
        let right = frame.maxX + gap + size.width/2
        let left = frame.minX - gap - size.width/2
        let x = right <= maxX ? right : (left >= minX ? left : maxX)
        let y: CGFloat
        if right > maxX && left < minX {
            let below = frame.maxY + gap + size.height/2
            let above = frame.minY - gap - size.height/2
            y = below <= maxY ? below : (above >= minY ? above : minY)
        } else { y = frame.midY }
        return CGPoint(x:min(max(x,minX),maxX),y:min(max(y,minY),maxY))
    }
}

/// Bounded, anonymous editor diagnostics for a device-only gesture failure.
/// There are no published properties, touch objects, labels or stored positions.
@MainActor private final class PlankIPadEditorGestureDiagnostics {
    enum Counter: Int, CaseIterable {
        case taps, moves, moveModeBlocks, resizes, changedProposals, unchangedProposals
        case frameChanges, canvasChanges, activeCanvasChanges, deselections, duplicates, ends
    }
    private var totals = Array(repeating:0,count:Counter.allCases.count)
    private var emissions = 0
    private var lastReport = -Double.infinity
    func count(_ counter: Counter) {
        totals[counter.rawValue] = min(Int(Int32.max),totals[counter.rawValue] + 1)
    }
    func report(reason: Int = 1,force: Bool = false) {
        let now = ProcessInfo.processInfo.systemUptime
        // Reserve the last slot for the editor's final summary.
        guard emissions < (reason == 3 ? 24 : 23),force || now - lastReport >= 1 else { return }
        emissions += 1; lastReport = now
        NSLog("PLANK editor gestures reason=%d taps=%d moves=%d moveModeBlocks=%d resizes=%d changedProposals=%d unchangedProposals=%d frameChanges=%d canvasChanges=%d activeCanvasChanges=%d deselections=%d duplicates=%d ends=%d",
            reason,totals[0],totals[1],totals[2],totals[3],totals[4],totals[5],
            totals[6],totals[7],totals[8],totals[9],totals[10],totals[11])
    }
}
