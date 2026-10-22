import SwiftUI
import UIKit

@main struct PlankIPadApp: App {
    @StateObject private var client: PlankCoreClient
    @StateObject private var store = HostStore()
    @StateObject private var router: PlankIPadInputRouter
    init() {
        let client = PlankCoreClient()
        _client = StateObject(wrappedValue: client)
        _router = StateObject(wrappedValue: PlankIPadInputRouter(client: client))
    }
    var body: some Scene {
        WindowGroup { PlankIPadRoot(client: client, store: store, router: router) }
    }
}

struct PlankIPadRoot: View {
    @ObservedObject var client: PlankCoreClient
    @ObservedObject var store: HostStore
    @ObservedObject var router: PlankIPadInputRouter
    @Environment(\.scenePhase) private var scenePhase
    // Presentation callbacks may retain an earlier Environment snapshot.
    // Admission uses the latest phase delivered by SwiftUI's scene observer.
    @State private var inputScenePhase = ScenePhase.inactive
    @State private var selectedID: UUID?
    @StateObject private var pencilRelay = PlankIPadPencilRelay()
    @StateObject private var customControls = PlankIPadCustomControlsStore()
    @State private var sharePencil = false
    @State private var showingCustomControls = false
    @State private var editingCustomControls = false
    @State private var controlWindowSize = CGSize.zero
    @State private var desktopControlsEnabled = false
    @State private var add = false
    @State private var editing: HostBookmark?
    @State private var controls = false
    @State private var username = ""
    @State private var password = ""
    private enum CredentialField: Hashable { case username, password }
    @FocusState private var credentialFocus: CredentialField?
    @AppStorage("plank.ipad.showSessionToolbar") private var showToolbar = true
    @AppStorage("plank.ipad.keyboardFunctionKeyMode") private var keyboardMode = KeyboardFunctionKeyMode.pc.rawValue
    private var host: HostBookmark? { store.hosts.first { $0.id == selectedID } }
    private var hideBars: Bool { hasWorkingSurface && !showToolbar }
    private var busy: Bool {
        switch client.phase { case .probing, .authenticating, .startingSession: true; default: client.isClosingSession }
    }
    private var hasWorkingSurface: Bool { client.hasActiveDesktopSession || sharePencil }
    var body: some View {
        // Keep one layout owner while the full-window drawing surface and
        // native navigation screens change underneath the presentation host.
        ZStack {
            if hasWorkingSurface { workingSurface }
            else { idleScreen }
        }
        .background {
            PlankIPadWindowMetrics { size,insets in
                // Cache real window changes even while an editor covers idle
                // content, so its next entry cannot reuse a stale orientation.
                reportControlWindow(size)
                if !hasWorkingSurface {
                    reportControlChrome(PlankControlChromeGeometry(bounds:CGRect(origin:.zero,size:size),
                        usableBounds:CGRect(x:insets.left,y:insets.top+44,
                            width:max(0,size.width-insets.left-insets.right),
                            height:max(0,size.height-insets.top-44-insets.bottom))))
                }
            }
        }
        .ignoresSafeArea(.keyboard,edges:hasWorkingSurface ? .bottom : [])
        .preferredColorScheme(sharePencil ? .dark : nil)
        .statusBarHidden(hideBars && !editingCustomControls)
        .fullScreenCover(isPresented:$editingCustomControls,onDismiss:customControlsEditorDismissed) {
            PlankIPadCustomControlsEditor(store:customControls,referenceSurfaceSize:controlWindowSize,
                surface:sharePencil ? .pencilSharing : .desktop)
                .interactiveDismissDisabled()
        }
        .sheet(isPresented:$add) { PlankIPadBookmarkEditor(store:store,client:client,host:.init(name:"",address:"",spatialDisplaySize:PlankIPadDisplayOptions.defaultSize),isNew:true) }
        .sheet(item:$editing) { host in PlankIPadBookmarkEditor(store:store,client:client,host:host,isNew:false) }
        .sheet(isPresented:$controls) {
            if sharePencil { PlankIPadPencilPadOptions(relay:pencilRelay,stopSharing:stopPencilSharing) }
            else { PlankIPadControls(client:client,router:router) }
        }
        .onAppear { selectedID = store.hosts.first?.id; updateAdmission() }
        .onChange(of:store.hosts) { _,hosts in if selectedID == nil { selectedID = hosts.first?.id } }
        .onChange(of:controls) { _,_ in updateAdmission() }
        .onChange(of:editingCustomControls) { _,_ in updateAdmission() }
        .onChange(of:sharePencil) { _,_ in updateAdmission() }
        .onChange(of:client.phase) { _,_ in
            if sharePencil && client.hasActiveDesktopSession { stopPencilSharing() }
            updateAdmission()
        }
        .onChange(of:scenePhase,initial:true) { _,phase in
            inputScenePhase = phase
            // Transient inactivity retires input without discarding the
            // approved peer. Actual backgrounding still ends foreground sharing.
            if phase == .background && sharePencil { stopPencilSharing() }
            updateAdmission()
            if phase == .background { password = ""; disconnect() }
        }
    }
    private var idleScreen: some View {
        NavigationStack {
                    ScrollViewReader { proxy in
                      ScrollView {
                        VStack(spacing: 20) {
                            if let message = router.inputFailure {
                                Text(message).foregroundStyle(.orange).multilineTextAlignment(.center)
                            }
                            Button { editingCustomControls = true } label: {
                                Label("Custom Controls",systemImage:"rectangle.grid.2x2")
                            }.buttonStyle(.bordered).disabled(busy)
                            if store.hosts.isEmpty {
                                ContentUnavailableView("Add a workstation", systemImage: "desktopcomputer",
                                    description: Text("Connect to your PLANK desktop."))
                                Button("Add Workstation") { add = true }.buttonStyle(.borderedProminent)
                            } else {
                                Picker("Workstation", selection: $selectedID) {
                                    Text("Choose…").tag(Optional<UUID>.none)
                                    ForEach(store.hosts) { Text($0.name).tag(Optional($0.id)) }
                                }.pickerStyle(.menu).disabled(busy)
                                if let host {
                                    Image(systemName: "desktopcomputer").font(.system(size: 56)).foregroundStyle(.cyan)
                                    Text(host.name).font(.title)
                                    Text("\(host.spatialDisplaySize.title) · \(host.streamFrameRate) fps").foregroundStyle(.secondary)
                                    connection(host)
                                    Button("Edit Workstation") { editing = host }.disabled(busy)
                                }
                            }
                        }.frame(maxWidth: 480).padding(24).frame(maxWidth: .infinity)
                      }
                      .scrollDismissesKeyboard(.interactively)
                      .onChange(of: credentialFocus) { _, _ in scrollCredentials(using: proxy) }
                      .onChange(of: controlWindowSize) { _, _ in scrollCredentials(using: proxy) }
                      .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardDidChangeFrameNotification)) { _ in
                          // The native scroll view reserves keyboard space first.
                          // Then reveal the focused local credential field.
                          scrollCredentials(using: proxy)
                      }
                    }
            .background { PlankIPadNavigationDiagnostics(role:"idle") }
            .navigationTitle("PLANK iPad Pilot")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItemGroup(placement:.topBarTrailing) {
                    PlankIPadControlLayoutsButton(store:customControls,select:selectCustomControlLayout,edit:editCustomControls)
                        .disabled(busy)
                    Menu {
                        Button { startPencilSharing() } label: { Label("Share Apple Pencil",systemImage:"pencil.tip.crop.circle") }
                            .disabled(busy || scenePhase != .active)
                    } label: { Label("Client Options",systemImage:"slider.horizontal.3") }
                    Button { add = true } label: { Label("Add Workstation",systemImage:"plus") }.disabled(busy)
                }
            }
        }
    }
    private var workingSurface: some View {
        PlankIPadWorkingSurface(barsVisible:!hideBars,consentVisible:sharePencil && pencilRelay.verification != nil,
            measured:reportControlWindow,chromeChanged:reportControlChrome) {
            if sharePencil {
                PlankIPadPencilPad(relay:pencilRelay)
            } else {
                VStack(spacing:0) {
                    if client.frameDimensions == nil { ProgressView("Starting desktop…").padding() }
                    PlankIPadCanvas(client:client,router:router,functionKeyMode:KeyboardFunctionKeyMode(rawValue:keyboardMode) ?? .pc)
                        .overlay(alignment:.topLeading) {
                            if client.videoDiagnosticsEnabled {
                                VStack(alignment:.leading,spacing:2) {
                                    Text(client.videoDiagnosticText); Text(client.audioDiagnosticText)
                                }.font(.caption2.monospacedDigit()).padding(8)
                                    .background(.black.opacity(0.8)).foregroundStyle(.white)
                                    .allowsHitTesting(false)
                            }
                        }
                }
            }
        } controls: {
            if showingCustomControls {
                if sharePencil {
                    PlankIPadCustomControlsOverlay(store:customControls,enabled:pencilRelay.canDraw,
                        begin:pencilRelay.beginControl,end:pencilRelay.endControl,tap:pencilRelay.tapControl,
                        release:pencilRelay.releaseControls,inputEpoch:pencilRelay.inputEpoch,
                        pencilSurface:{ pencilRelay.pad },pencilHover:{ pencilRelay.pad?.controlPencilHover($0) },
                        pencilSqueeze:{ pencilRelay.pad?.controlPencilSqueeze($0,from:$1) })
                        .id("pencil-sharing-controls")
                } else {
                    PlankIPadCustomControlsOverlay(store:customControls,enabled:desktopControlsEnabled && router.enabled,
                        begin:router.beginControl,end:{ _ = router.endControl(owner:$0) },tap:router.tapControl,
                        release:{ _ = router.releaseControls() },inputEpoch:router.inputEpoch,
                        pencilSurface:{ router.surface },pencilHover:{ router.surface?.controlPencilHover($0) },
                        pencilSqueeze:{ router.surface?.controlPencilSqueeze($0,from:$1) })
                        .id("desktop-controls")
                }
            }
        } toolbar: {
            workingToolbar
        } floating: {
            HStack(spacing:8) {
                PlankIPadControlsVisibilityButton(store:customControls,visible:showingCustomControls,
                    toggle:toggleCustomControls)
                    .background(.regularMaterial,in:Circle())
                PlankIPadControlLayoutsButton(store:customControls,select:selectCustomControlLayout,edit:editCustomControls)
                    .background(.regularMaterial,in:Circle())
                Button { setToolbarVisible(true) } label: {
                    Image(systemName:"chevron.down").frame(width:44,height:44)
                        .background(.regularMaterial,in:Circle())
                }.accessibilityLabel("Show session toolbar")
            }
        } consent: {
            if sharePencil,let code = pencilRelay.verification {
                PlankIPadPencilConsent(relay:pencilRelay,code:code,reject:stopPencilSharing)
                    .padding(.horizontal,16)
            }
        }
    }
    private var workingToolbar: some View {
        HStack(spacing:12) {
            Text(sharePencil ? "Apple Pencil" : (host?.name ?? "Desktop"))
                .font(.headline).lineLimit(1)
            Spacer(minLength:8)
            if !sharePencil {
                Button { router.setSoftwareKeyboardPresented(!router.softwareKeyboardPresented) } label: {
                    Label(router.softwareKeyboardPresented ? "Hide Keyboard" : "Show Keyboard",systemImage:"keyboard")
                }.labelStyle(.iconOnly).frame(minWidth:44,minHeight:44)
            }
            Button { controls = true } label: {
                Label(sharePencil ? "Pencil Sharing Settings" : "Session Controls",systemImage:"slider.horizontal.3")
            }
                .labelStyle(.iconOnly).frame(minWidth:44,minHeight:44)
            PlankIPadControlsVisibilityButton(store:customControls,visible:showingCustomControls,
                toggle:toggleCustomControls)
            PlankIPadControlLayoutsButton(store:customControls,select:selectCustomControlLayout,edit:editCustomControls)
            Button { setToolbarVisible(false) } label: { Label("Hide Toolbar",systemImage:"chevron.up") }
                .labelStyle(.iconOnly).frame(minWidth:44,minHeight:44)
            if sharePencil { Button("Stop Sharing",action:stopPencilSharing).frame(minHeight:44) }
            else { Button("Disconnect",action:disconnect).frame(minHeight:44).disabled(client.isClosingSession) }
        }.padding(.horizontal,12)
    }
    private func reportControlWindow(_ size: CGSize) {
        guard size.width.isFinite,size.height.isFinite,size.width > 1,size.height > 1 else { return }
        if controlWindowSize != size { controlWindowSize = size }
        // Both editor entry points resolve the same full-window coordinate
        // system. Reporting geometry never rewrites saved placements/sizes.
        customControls.reportSurface(size:size,surface:.desktop)
        customControls.reportSurface(size:size,surface:.pencilSharing)
    }
    private func reportControlChrome(_ geometry: PlankControlChromeGeometry) {
        // Editor and live keys use the same measured menu boundaries, including
        // the compact controls shown when the main toolbar is hidden.
        customControls.reportChrome(geometry,surface:.desktop)
        customControls.reportChrome(geometry,surface:.pencilSharing)
    }
    @ViewBuilder private func connection(_ host: HostBookmark) -> some View {
        if client.activeHostID == host.id {
            switch client.phase {
            case .needsCredentials, .authenticating:
                TextField("Username", text: $username)
                    .textInputAutocapitalization(.never).autocorrectionDisabled().textContentType(.username)
                    .textFieldStyle(.roundedBorder).submitLabel(.next)
                    .focused($credentialFocus, equals: .username).id(CredentialField.username)
                    .disabled(busy)
                    .onSubmit { if !busy { credentialFocus = .password } }
                SecureField("Password", text: $password).textContentType(.password)
                    .textFieldStyle(.roundedBorder).submitLabel(.go)
                    .focused($credentialFocus, equals: .password).id(CredentialField.password)
                    .disabled(busy).onSubmit { authenticate() }
                Button("Sign In") { authenticate() }.buttonStyle(.borderedProminent).disabled(busy || username.isEmpty || password.isEmpty)
            case .authenticated:
                Button("Open Desktop") { start(host) }.buttonStyle(.borderedProminent).disabled(busy)
            case let .failed(message):
                Text(message).foregroundStyle(.orange).textSelection(.enabled)
                Button("Reconnect") { connect(host) }.buttonStyle(.borderedProminent).disabled(busy)
            case .probing: ProgressView("Connecting…")
            default: Button("Connect") { connect(host) }.buttonStyle(.borderedProminent).disabled(busy)
            }
        } else { Button("Connect") { connect(host) }.buttonStyle(.borderedProminent).disabled(busy) }
    }
    private func scrollCredentials(using proxy: ScrollViewProxy) {
        guard let field = credentialFocus, !client.hasActiveDesktopSession else { return }
        withAnimation(.easeOut(duration: 0.2)) { proxy.scrollTo(field, anchor: .center) }
    }
    private func connect(_ host: HostBookmark) {
        guard !sharePencil else { return }
        credentialFocus = nil
        password = ""
        router.clearInputFailure()
        router.enabled = false
        Task { await client.reset()?.value; await client.connect(to: host) }
    }
    private func authenticate() {
        guard !busy, !username.isEmpty, !password.isEmpty else { return }
        credentialFocus = nil
        let secret = password; password = ""
        Task { await client.authenticate(username: username, password: secret) }
    }
    private func start(_ host: HostBookmark) {
        guard !sharePencil else { return }
        client.setTabletActive(inputScenePhase == .active)
        client.startSession(displaySize: host.spatialDisplaySize, frameRate: host.streamFrameRate,
                            videoBitrateKbps: host.videoBitrateKbps)
        store.markConnected(host)
    }
    private func disconnect() {
        router.enabled = false
        client.setTabletActive(false)
        controls = false
        client.reset()
    }
    private func startPencilSharing() {
        guard !busy,!client.hasActiveDesktopSession,inputScenePhase == .active else { return }
        credentialFocus = nil
        router.release(); router.enabled = false
        client.setTabletActive(false); client.reset()
        sharePencil = true; pencilRelay.start(); updateAdmission()
    }
    private func stopPencilSharing() {
        // Retire the old source before the root changes mode/overlay identity.
        pencilRelay.stop(); sharePencil = false; controls = false
        updateAdmission()
    }
    private func toggleCustomControls() {
        if sharePencil { pencilRelay.releaseControls() } else { router.releaseControls() }
        showingCustomControls.toggle()
    }
    private func editCustomControls() {
        if sharePencil { pencilRelay.setAdjustingPad(true) } else { router.release() }
        editingCustomControls = true
    }
    private func customControlsEditorDismissed() {
        // Done and Cancel both return to the same live source. Restore after
        // the cover has gone as well as when its presentation binding changes.
        updateAdmission()
        Task { @MainActor in
            await Task.yield()
            if router.enabled { router.surface?.resumeKeyboard() }
        }
    }
    private func selectCustomControlLayout(_ id: UUID) {
        guard id != customControls.library.selectedID,
              customControls.library.layouts.contains(where:{ $0.id == id }) else { return }
        // Retire the current source before publishing different bindings. The
        // stream/approved peer stays connected; existing admission is restored.
        if sharePencil { pencilRelay.setAdjustingPad(true) } else { router.release() }
        customControls.select(id:id)
        updateAdmission()
    }
    private func setToolbarVisible(_ visible: Bool) {
        // A local control changes the canvas bounds. Retire held input before
        // the layout moves, while retaining this stream and its remote mode.
        if sharePencil { pencilRelay.setAdjustingPad(true) } else { router.release() }
        showToolbar = visible
        updateAdmission()
        Task { @MainActor in
            await Task.yield()
            if router.enabled { router.surface?.resumeKeyboard() }
        }
    }
    private func updateAdmission() {
        let foreground = inputScenePhase == .active
        let streaming: Bool
        if case .streaming = client.phase { streaming = true } else { streaming = false }
        router.enabled = foreground && streaming && !sharePencil && !controls && !editingCustomControls
        // The router's admission is not published. Reflect its actual value
        // into local presentation after onChange, including editor dismissal.
        desktopControlsEnabled = router.enabled
        client.setTabletActive(foreground && client.hasActiveDesktopSession && !sharePencil)
        if sharePencil { pencilRelay.setAdjustingPad(!foreground || controls || editingCustomControls) }
        PlankAudioOutput.shared.setSceneActive(foreground)
    }
}

struct PlankIPadBookmarkEditor: View {
    @ObservedObject var store: HostStore
    @ObservedObject var client: PlankCoreClient
    @Environment(\.dismiss) private var dismiss
    @State var host: HostBookmark
    let isNew: Bool
    @State private var port = "28989"
    @State private var saving = false
    @State private var confirm = false
    private var valid: Bool {
        !host.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
        !host.address.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
        (UInt16(port) ?? 0) > 0
    }
    private var proposedHost: HostBookmark {
        var edited = host
        edited.name = edited.name.trimmingCharacters(in: .whitespacesAndNewlines)
        edited.address = edited.address.trimmingCharacters(in: .whitespacesAndNewlines)
        edited.port = UInt16(port) ?? host.port
        edited.secondDisplaySize = nil
        return edited
    }
    var body: some View {
        NavigationStack {
            Form {
                Section("Workstation") {
                    TextField("Name", text: $host.name)
                    TextField("Address or hostname", text: $host.address).textInputAutocapitalization(.never).autocorrectionDisabled()
                    TextField("Port", text: $port).keyboardType(.numberPad)
                }
                Section("Display") {
                    Picker("Resolution", selection: $host.spatialDisplaySize) {
                        ForEach(PlankIPadDisplayOptions.choices(current: host.spatialDisplaySize)) {
                            Text(PlankIPadDisplayOptions.title($0)).tag($0)
                        }
                    }
                    Text("Two landscape sizes with the same shape. The desktop keeps its proportions when you rotate the iPad.")
                        .font(.footnote).foregroundStyle(.secondary)
                    Picker("Frame rate", selection: $host.streamFrameRate) {
                        ForEach(StreamFrameRate.presets, id: \.self) { Text("\($0) fps").tag($0) }
                    }
                }
            }.navigationTitle(isNew ? "Add Workstation" : "Edit Workstation")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() }.disabled(saving) }
                ToolbarItem(placement: .confirmationAction) { Button("Save") {
                    if client.requiresSessionCloseForBookmark(proposedHost) { confirm = true } else { save() }
                }.disabled(!valid || saving) }
            }
            .disabled(saving).interactiveDismissDisabled(saving)
            .onAppear { port = String(host.port) }
            .alert("Close the workstation session?", isPresented: $confirm) {
                Button("Cancel", role: .cancel) {}
                Button("Close Session and Save", role: .destructive) { save() }
            } message: { Text("Changing the display requires a fresh sign-in. The current connection will close before saving.") }
        }
    }
    private func save() {
        guard valid, !saving else { return }
        let edited = proposedHost
        saving = true
        Task {
            await client.closeSessionForBookmarkChange(edited)
            if isNew { store.add(name: edited.name, address: edited.address, port: edited.port,
                                displaySize: edited.spatialDisplaySize, frameRate: edited.streamFrameRate) }
            else { store.update(edited) }
            dismiss()
        }
    }
}

struct PlankIPadControls: View {
    @ObservedObject var client: PlankCoreClient
    @ObservedObject var router: PlankIPadInputRouter
    @Environment(\.dismiss) private var dismiss
    @AppStorage(PlankAudioPreferences.volumeKey) private var volume = 1.0
    @AppStorage(PlankAudioPreferences.mutedKey) private var muted = false
    @AppStorage("plank.ipad.keyboardFunctionKeyMode") private var keyboardMode = KeyboardFunctionKeyMode.pc.rawValue
    @State private var bitrate = Double(StreamBitrate.defaultKbps)
    var body: some View {
        NavigationStack {
            Form {
                Section("Audio") {
                    Toggle("Mute", isOn: $muted)
                    Slider(value: $volume, in: 0...1) { Text("Volume") }
                }
                Section("Physical Keyboard") {
                    Picker("Keyboard type", selection: $keyboardMode) {
                        ForEach(KeyboardFunctionKeyMode.allCases) { mode in
                            Text(mode.title).tag(mode.rawValue)
                        }
                    }
                    Text(keyboardMode == KeyboardFunctionKeyMode.appleExtended.rawValue ?
                         "Apple F13 through F24 are sent as function keys." :
                         "The top-right PC keys act as Print Screen, Scroll Lock and Pause.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("Apple Pencil") {
                    Text(client.acceptsNormalizedPen ? "Pencil drawing available" : "Pencil drawing unavailable on this connection")
                    Text("Pressure and tilt come from your Pencil. USB-C Pencil has no pressure sensitivity. Rotate or open controls to end a stroke; lift and start again. Squeeze Pencil Pro while hovering to right-click.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("Video") {
                    Text("Bitrate · \(Int(bitrate / 1000)) Mbps")
                    Slider(value: $bitrate, in: Double(StreamBitrate.minimumKbps)...Double(StreamBitrate.maximumKbps),
                           step: Double(StreamBitrate.stepKbps), onEditingChanged: { editing in
                        client.chooseLiveBitrate(Int(bitrate), final: !editing)
                    }) { Text("Bitrate") }.disabled(client.liveBitrate?.supported != true)
                    Toggle("Show statistics", isOn: Binding(get: { client.videoDiagnosticsEnabled }, set: { client.setVideoDiagnosticsEnabled($0) }))
                }
                if client.videoDiagnosticsEnabled {
                    Section("Scroll diagnostics") {
                        Text(router.wheelDiagnostics).monospacedDigit()
                        Text("Counts since this canvas opened. Sent means submitted by PLANK; it does not confirm workstation receipt.")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                }
            }.navigationTitle("Session Controls").navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .onAppear { bitrate = Double(client.liveBitrate?.currentKbps ?? StreamBitrate.defaultKbps) }
            .onChange(of: bitrate) { _, value in client.chooseLiveBitrate(Int(value), final: false) }
            .onChange(of: volume) { _, _ in applyAudio() }
            .onChange(of: muted) { _, _ in applyAudio() }
        }
    }
    private func applyAudio() { PlankAudioOutput.shared.setVolume(Float(volume), muted: muted) }
}
