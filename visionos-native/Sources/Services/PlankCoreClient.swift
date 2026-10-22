import Combine
import Foundation

enum ConnectionPhase: Equatable, Sendable {
    case idle
    case probing
    case needsCredentials(PlankHostIdentity)
    case authenticating(PlankHostIdentity)
    case authenticated(PlankHostIdentity, PlankAuthentication)
    case startingSession(PlankHostIdentity, PlankAuthentication)
    case frameReceived(PlankHostIdentity, PlankAuthentication, PlankFrameProbe)
    case streaming(PlankHostIdentity, PlankAuthentication, UInt64)
    case failed(String)
}

// A busy main actor only needs the newest decoded frame. Keep one pending
// delivery instead of scheduling a separate UI task for every video frame.
private final class PlankFrameMailbox: @unchecked Sendable {
    private let lock = NSLock()
    private var newest: PlankRenderedFrame?
    private var newestOfferedAt: UInt64 = 0
    private var deliveryScheduled = false

    func offer(_ frame: PlankRenderedFrame) -> Bool {
        lock.lock()
        let replacedPending = newest != nil
        newest = frame
        newestOfferedAt = DispatchTime.now().uptimeNanoseconds
        let shouldSchedule = !deliveryScheduled
        deliveryScheduled = true
        lock.unlock()
        PlankTimingCapture.shared.offered(replacedPending: replacedPending)
        return shouldSchedule
    }

    func takeNewest() -> PlankRenderedFrame? {
        lock.lock()
        let frame = newest
        let offeredAt = newestOfferedAt
        newest = nil
        deliveryScheduled = false
        lock.unlock()
        if frame != nil {
            PlankTimingCapture.shared.delivered(
                latencyNanos: DispatchTime.now().uptimeNanoseconds - offeredAt
            )
        }
        return frame
    }
}

// The receive worker reads this flag without consulting preferences or the UI.
private final class PlankVideoDiagnosticsGate: @unchecked Sendable {
    private let lock = NSLock()
    private var enabled = false

    var isEnabled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return enabled
    }

    func setEnabled(_ next: Bool) {
        lock.lock()
        enabled = next
        lock.unlock()
    }
}

// Presentation keeps only the newest completed shape and position. Reliable
// chunks/tablet feedback have already been processed on the control thread.
private final class PlankCursorMailbox: @unchecked Sendable {
    private let lock = NSLock()
    private var position: PlankRemoteCursor?
    private var shape: PlankRemoteCursorShape?
    private var deliveryScheduled = false

    func offer(_ update: PlankCursorUpdate) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        switch update {
        case let .position(next): position = next
        case let .shape(next): shape = next
        }
        let schedule = !deliveryScheduled
        deliveryScheduled = true
        return schedule
    }

    func takeNewest() -> [PlankCursorUpdate] {
        lock.lock()
        defer { lock.unlock() }
        var result: [PlankCursorUpdate] = []
        if let shape { result.append(.shape(shape)) }
        if let position { result.append(.position(position)) }
        self.position = nil
        self.shape = nil
        deliveryScheduled = false
        return result
    }
}

// The Relay will install a receiver here. Host control frames stay on the
// control receiver and are never stored in UI state or copied to logs.
private final class PlankTabletControlReceiver: @unchecked Sendable {
    private let lock = NSLock()
    private var receiver: (@Sendable (Data) -> Void)?

    func set(_ next: (@Sendable (Data) -> Void)?) {
        lock.lock()
        receiver = next
        lock.unlock()
    }

    func deliver(_ frame: Data) {
        lock.lock()
        let current = receiver
        lock.unlock()
        current?(frame)
    }
}

@MainActor
final class PlankCoreClient: ObservableObject {
#if PLANK_PENCIL_RELAY_RECEIVER
    lazy var pencilRelay = PlankPencilRelayReceiver(client: self)
    private var pencilKeyOwnership = PlankPencilKeyOwnership()
    func sendPencilRelayModifier(_ key: PlankPencilModifier, pressed: Bool) -> Bool {
        sendPencilRelayKeys([.init(code:key.rawValue,pressed:pressed)])
    }
    func sendPencilRelayKeys(_ batch: [PlankPencilKeyEvent]) -> Bool {
        guard pencilRelayCanDraw, pencilRelay.ownsPen,
              (try? PlankPencilMessage.keys(batch).encoded()) != nil else { return false }
        // Publish the ledger only after every ordered edge has queue capacity.
        var next = pencilKeyOwnership
        let events = batch.compactMap { edge -> PlankControlKeyEvent? in
            guard let value = next.update(code:edge.code,pressed:edge.pressed,modifiers:edge.modifiers,source:.pad) else { return nil }
            return .init(code:value.code,pressed:value.pressed,modifiers:value.modifiers)
        }
        guard inputQueue.offerControlKeys(events) else { return false }
        pencilKeyOwnership = next; return true
    }
    func retirePencilRelayModifiers() {
        for event in pencilKeyOwnership.retirePad() {
            inputQueue.append(.key(code:event.code,pressed:event.pressed,modifiers:event.modifiers))
        }
    }
    func resetPencilKeyOwnership() { pencilKeyOwnership = .init() }
    var pencilRelaySourceAllowed: Bool {
#if PLANK_NATIVE_MAC_WACOM
        // Use the immutable running-session choice, not preferences changed
        // while a USB worker or registered Relay already owns the backend.
        return PlankMacTabletSource.allowsPencil(sessionActive: hasActiveDesktopSession,
            sessionSource: sessionNativeSource, savedSource: .saved)
#else
        return PlankRelayKeys.relayRegistry().selection == .off
#endif
    }
    var pencilRelaySourceMessage: String {
#if PLANK_NATIVE_MAC_WACOM
        "Choose Apple Pencil shared from iPad as the tablet source before connecting."
#else
        "Turn Tablet Relay off before choosing an iPad Pencil."
#endif
    }
#if PLANK_NATIVE_MAC_WACOM
    private var pencilLocalControls = Set<UUID>()
    func setPencilLocalControls(_ presented: Bool, owner: UUID) {
        if presented { pencilLocalControls.insert(owner) } else { pencilLocalControls.remove(owner) }
        pencilRelay.sync()
    }
#endif
    var pencilRelayCanDraw: Bool {
        guard pencilRelaySourceAllowed, hostSupportsNormalizedPen, tabletSceneActive,
              !isClosingSession else { return false }
#if PLANK_NATIVE_MAC_WACOM
        guard pencilLocalControls.isEmpty else { return false }
#endif
        if case .streaming = phase { return true }; return false
    }
    func sendPencilRelayPen(_ pen: PlankNormalizedPen) -> Bool {
        guard pencilRelayCanDraw, pencilRelay.ownsPen else { return false }
        return inputQueue.offerPencilRelayPen(pen)
    }
    func sendPencilRelayRightClick(x: Float, y: Float, width: Int, height: Int) -> Bool {
        guard pencilRelayCanDraw, pencilRelay.ownsPen, !pencilRelay.strokeActive,
              width > 1, height > 1, x.isFinite, y.isFinite,
              (0...1).contains(x), (0...1).contains(y) else { return false }
        return inputQueue.offerPencilRelayRightClick(
            x:UInt16(clamping:Int((x * Float(width-1)).rounded())),
            y:UInt16(clamping:Int((y * Float(height-1)).rounded())),
            maximumX:UInt16(clamping:width-1),maximumY:UInt16(clamping:height-1))
    }
    func retirePencilRelayPen(_ pen: PlankNormalizedPen) {
        guard pen.phase == .cancel || pen.phase == .leave else { return }
        inputQueue.append(.pen(pen))
    }
#endif
    @Published private(set) var phase: ConnectionPhase = .idle
    @Published private(set) var isClosingSession = false
    @Published private(set) var frameDimensions: PlankFrameDimensions?
    @Published private(set) var sessionTopology: PlankTopology?
    @Published private(set) var sessionLocalDisplayLayout: PlankLocalDisplayLayout?
    @Published private(set) var videoDiagnosticsEnabled = false
    @Published private(set) var videoDiagnosticText = "Video starting…"
    @Published private(set) var audioDiagnosticText = "Audio starting…"
    /// The running session's live encoder target; nil outside a session.
    @Published private(set) var liveBitrate: PlankLiveBitrateStatus?
    @Published private(set) var activeHostID: HostBookmark.ID?
    var canRetrySession: Bool {
        httpClient != nil && lastIdentity != nil && lastAuthentication != nil
    }
#if PLANK_TABLET_RELAY
    @Published private(set) var tabletRelayStatus = "Choose a Tablet Relay in Settings."
    @Published private(set) var tabletPreflightSummary = "No active Wacom preflight."
    @Published private(set) var waitingForTablet = false
    @Published private(set) var showingTabletWaitScreen = false
    /// What the last or current session observed about the drawing link.
    /// Settings derives Connected/Unavailable from this, never from intent.
    @Published private(set) var tabletRelayLink: PlankRelayLinkObservation = .none
    @Published private(set) var tabletRelayTransport: String?
    private var tabletRelayLinkTracker = PlankRelayLinkTracker()
#endif

#if PLANK_NATIVE_MAC_WACOM
    private var nativeWacom: PlankMacWacomSession?
    private var sessionNativeSource = PlankMacTabletSource.off
#endif
    private var latestFrame: PlankRenderedFrame?
    private var remoteCursor: PlankRemoteCursor?
    private var remoteCursorShape: PlankRemoteCursorShape?
    private let videoSurfaces = PlankVideoSurfaces()

    private var httpClient: PlankHTTPClient?
    private var lastIdentity: PlankHostIdentity?
    private var connectedHost: HostBookmark?
    private var transitionCredentials: (username: String, password: String)?
    private var lastAuthentication: PlankAuthentication?
    private var inputQueue = PlankInputQueue()
    private var streamTask: Task<Void, Never>?
    private var stoppingStreamTask: Task<Void, Never>?
    private var streamGeneration = UUID()
    private let tabletControlReceiver = PlankTabletControlReceiver()
    private let videoDiagnostics = PlankVideoDiagnosticsGate()
    private var liveBitrateController: PlankLiveBitrate?
    @Published private(set) var hostSupportsNormalizedPen = false
    private var hostSupportsTabletRelay = false
#if PLANK_TABLET_RELAY
    private var tabletBridge: PlankRelaySessionBridge?
    private var stoppingTabletBridge: PlankRelaySessionBridge?
    private var tabletSceneActive = false
    private var tabletPreflightSequence: UInt64 = 0
    private var tabletInputPolicy = PlankTabletInputPolicy()
    private var tabletWaitTimeoutID = UUID()
#endif

    func setTabletControlReceiver(_ receiver: (@Sendable (Data) -> Void)?) {
        tabletControlReceiver.set(receiver)
    }

    var hasActiveDesktopSession: Bool {
        switch phase {
        case .startingSession, .frameReceived, .streaming: true
        default: false
        }
    }

    /// Changes only the running session's encoder target; the bookmark's
    /// startup target is untouched, as on the desktop Client.
    func chooseLiveBitrate(_ kbps: Int, final: Bool) {
        liveBitrateController?.choose(kbps, final: final)
    }

    func setVideoDiagnosticsEnabled(_ enabled: Bool) {
        videoDiagnosticsEnabled = enabled
        videoDiagnostics.setEnabled(enabled)
    }

    func sendTabletFrame(_ frame: Data) {
        guard case .streaming = phase, hostSupportsTabletRelay else { return }
#if PLANK_TABLET_RELAY
        if rawTabletMessageType(frame) == 3 && !tabletInputPolicy.forwardsTabletReports { return }
#endif
        inputQueue.append(.rawHid(frame))
    }

#if PLANK_TABLET_RELAY
    private func rawTabletMessageType(_ frame: Data) -> UInt16? {
        guard frame.count >= 8 else { return nil }
        return UInt16(frame[6]) | UInt16(frame[7]) << 8
    }

    private func updateTabletPreflight(_ snapshot: PlankWacomPreflightSnapshot,
                                      generation: UUID) {
        // A Relay can disappear while a drag or key is held. Release what the
        // Host has already seen before blocking new user input.
        let wasWaiting = tabletInputPolicy.waitsForTablet
        let wasUnavailable = tabletInputPolicy.shouldContinueWhenUnavailable
        for release in tabletInputPolicy.updatePreflight(
            ready: snapshot.ready,
            relayAttached: snapshot.gates[.deviceOwnership] == .passed
        ) {
            switch release {
            case let .mouse(number):
                inputQueue.append(.button(number: number, pressed: false))
            case let .key(code, modifiers):
                inputQueue.append(.key(code: code, pressed: false, modifiers: modifiers))
            }
        }
        waitingForTablet = tabletInputPolicy.waitsForTablet
        showingTabletWaitScreen = tabletInputPolicy.showsBlockingOverlay
        if !waitingForTablet {
            tabletWaitTimeoutID = UUID()
        } else if (!wasWaiting || !wasUnavailable) &&
                    tabletInputPolicy.shouldContinueWhenUnavailable &&
                    hostSupportsTabletRelay && tabletSceneActive {
            scheduleTabletAvailabilityGrace(generation: generation)
        }
    }

    func continueWithoutTablet() {
        continueWithoutTablet(status: "Continuing this session without Wacom.")
    }

    /// A previous failure describes the previous Relay. Forget it when the
    /// selection changes outside a session; a live session keeps its evidence.
    func clearTabletRelayObservation() {
        switch phase {
        case .startingSession, .frameReceived, .streaming: return
        default:
            tabletRelayLinkTracker = PlankRelayLinkTracker()
            tabletRelayLink = tabletRelayLinkTracker.observation
            tabletRelayTransport = tabletRelayLinkTracker.authenticatedRoute
        }
    }

    private func applyRelayLink(_ event: PlankRelayLinkEvent) {
        tabletRelayLinkTracker.apply(event)
        tabletRelayLink = tabletRelayLinkTracker.observation
        tabletRelayTransport = tabletRelayLinkTracker.authenticatedRoute
    }

    private func continueWithoutTablet(status: String) {
        guard tabletInputPolicy.waitsForTablet else { return }
        tabletInputPolicy.continueWithoutTablet()
        finishContinuingWithoutTablet(status: status)
    }

    private func finishContinuingWithoutTablet(status: String) {
        tabletWaitTimeoutID = UUID()
#if PLANK_NATIVE_MAC_WACOM
        PlankMacWacomSession.retireForSession(&nativeWacom)
#endif
        tabletBridge?.close(reason: .continueWithoutTablet)
        stoppingTabletBridge = tabletBridge
        tabletBridge = nil
        tabletRelayStatus = status
        waitingForTablet = false
        showingTabletWaitScreen = false
    }

    private func scheduleTabletAvailabilityGrace(generation: UUID) {
        guard tabletSceneActive, tabletInputPolicy.shouldContinueWhenUnavailable else { return }
        let timeoutID = UUID()
        tabletWaitTimeoutID = timeoutID
        let bluetooth = PlankRelayKeys.relayRegistry().activeRelay?.connectionRoutes.contains {
            if case .bluetooth = $0 { return true }; return false
        } ?? false
        let grace = PlankRelayConnectionTiming.availabilityGraceSeconds(bluetooth: bluetooth)
        NSLog("PLANK tablet availability grace: transport=%@ seconds=%d", bluetooth ? "Bluetooth" : "network", grace)
        Task { [weak self] in
            try? await Task.sleep(for: .seconds(grace))
            guard let self, !Task.isCancelled,
                  self.streamGeneration == generation,
                  self.tabletWaitTimeoutID == timeoutID,
                  self.waitingForTablet,
                  self.tabletSceneActive,
                  self.tabletInputPolicy.shouldContinueWhenUnavailable else { return }
            switch self.tabletInputPolicy.expireAvailabilityGrace() {
            case .none:
                return
            case .continueWithoutTablet:
                self.finishContinuingWithoutTablet(
                    status: "Wacom Relay unavailable; continuing this session without the tablet."
                )
                if self.tabletRelayLink != .connected { self.applyRelayLink(.failed) }
            case .keepRecovering:
                // A tablet that worked earlier may be powered off for longer
                // than the startup grace. Release the desktop input barrier,
                // but retain the bridge, heartbeats and reconnect attempts.
                self.tabletWaitTimeoutID = UUID()
                self.waitingForTablet = false
                self.showingTabletWaitScreen = false
                self.tabletRelayStatus = "Waiting for Wacom to reconnect; mouse and keyboard remain available."
                NSLog("PLANK tablet recovery: availability grace expired; keeping Relay recovery active")
            }
        }
    }
#endif

    func setTabletActive(_ active: Bool) {
#if PLANK_TABLET_RELAY
        let wasActive = tabletSceneActive
        tabletSceneActive = active
        tabletBridge?.setActive(active)
#if PLANK_PENCIL_RELAY_RECEIVER
        pencilRelay.sync()
#endif
#if PLANK_NATIVE_MAC_WACOM
        if hostSupportsTabletRelay { nativeWacom?.setActive(active) }
#endif
        if active && !wasActive && hostSupportsTabletRelay && waitingForTablet {
            scheduleTabletAvailabilityGrace(generation: streamGeneration)
        }
#endif
    }

    func connect(to host: HostBookmark) async {
        guard !isClosingSession else { return }
        switch phase {
        case .probing, .authenticating, .startingSession, .frameReceived, .streaming:
            return
        default:
            break
        }
        if activeHostID != host.id {
            transitionCredentials = nil
            lastAuthentication = nil
        }
        activeHostID = host.id
        phase = .probing
        do {
            let client = try PlankHTTPClient(host: host)
            let identity = try await client.fetchServerInfo()
            guard identity.supportsAuthentication else {
                throw PlankHTTPError.invalidResponse("This Host does not advertise PLANK authentication.")
            }
            httpClient = client
            lastIdentity = identity
            connectedHost = host
            activeHostID = host.id
            phase = .needsCredentials(identity)
        } catch {
            httpClient = nil
            lastIdentity = nil
            phase = .failed(Self.message(for: error))
        }
    }

    func startSession(displaySize: SpatialDisplaySize, frameRate: Int, videoBitrateKbps: Int,
                      secondDisplaySize: SpatialDisplaySize? = nil,
                      localDisplayLayout: PlankLocalDisplayLayout? = nil) {
        guard !isClosingSession else { return }
        guard let httpClient,
              let identity = lastIdentity,
              var connectedHost,
              case let .authenticated(_, authentication) = phase else {
            phase = .failed("The workstation session is no longer ready.")
            return
        }

        guard PlankTopology.validCanvas(first: displaySize, second: secondDisplaySize) else {
            phase = .failed("The two display widths must total no more than 8192 pixels.")
            return
        }
        connectedHost.secondDisplaySize = secondDisplaySize
        sessionTopology = nil
        sessionLocalDisplayLayout = nil
        latestFrame = nil; frameDimensions = nil
        videoSurfaces.frame(nil); videoSurfaces.cursor(nil); videoSurfaces.shape(nil)
        connectedHost.spatialDisplaySize = displaySize
        connectedHost.streamFrameRate = StreamFrameRate.normalized(frameRate)
        connectedHost.videoBitrateKbps = StreamBitrate.normalized(videoBitrateKbps)
        self.connectedHost = connectedHost

        let previousStreamTask = streamTask ?? stoppingStreamTask
        previousStreamTask?.cancel()
#if PLANK_TABLET_RELAY
        let previousTabletBridge = tabletBridge ?? stoppingTabletBridge
        previousTabletBridge?.close(reason: .sessionReplaced)
        stoppingTabletBridge = previousTabletBridge
        tabletBridge = nil
#endif
#if PLANK_PENCIL_RELAY_RECEIVER
        pencilRelay.endDesktop()
#endif
        inputQueue.stop()
        inputQueue = PlankInputQueue()
        remoteCursor = nil
        remoteCursorShape = nil
        hostSupportsNormalizedPen = false
        hostSupportsTabletRelay = false
#if PLANK_TABLET_RELAY
        tabletRelayStatus = "Waiting for Host tablet support…"
        tabletPreflightSummary = "Waiting for Host and Relay checks…"
        tabletPreflightSequence = 0
        tabletWaitTimeoutID = UUID()
        showingTabletWaitScreen = false
#endif
        let sessionInputQueue = inputQueue
        streamGeneration = UUID()
        let generation = streamGeneration
#if PLANK_TABLET_RELAY
        // GNOME may bind its first pointer interaction before the redirected
        // tablet appears. Present the video immediately, but let a saved Relay
        // attach before forwarding mouse input into that desktop session.
#if PLANK_NATIVE_MAC_WACOM
        sessionNativeSource = PlankMacTabletSource.saved
        let relayConfigured = sessionNativeSource.usesUSB ||
            (sessionNativeSource.usesRegisteredRelay && PlankRelayKeys.sessionRelayConfigured())
#else
        let relayConfigured = PlankRelayKeys.sessionRelayConfigured()
#endif
        tabletInputPolicy.begin(hasPairedTablet: relayConfigured)
        waitingForTablet = tabletInputPolicy.waitsForTablet
        applyRelayLink(.sessionStarted(configured: relayConfigured))
#endif
        phase = .startingSession(identity, authentication)
        streamTask = Task { [weak self] in
            guard let self else { return }
            // Wait for the previous transport to send its disconnect and
            // release the Host reservation and the old Relay link before
            // opening another data plane.
            await previousStreamTask?.value
#if PLANK_TABLET_RELAY
            await previousTabletBridge?.waitForClose()
            if self.stoppingTabletBridge === previousTabletBridge {
                self.stoppingTabletBridge = nil
            }
#endif
            guard !Task.isCancelled, self.streamGeneration == generation else { return }
            await runSession(
                httpClient: httpClient,
                identity: identity,
                authentication: authentication,
                connectedHost: connectedHost,
                localDisplayLayout: localDisplayLayout,
                inputQueue: sessionInputQueue,
                generation: generation
            )
        }
        stoppingStreamTask = nil
    }

    func retrySession(displaySize: SpatialDisplaySize, frameRate: Int, videoBitrateKbps: Int) {
        guard !isClosingSession else { return }
        guard let identity = lastIdentity, let authentication = lastAuthentication else { return }
        phase = .authenticated(identity, authentication)
        startSession(displaySize: displaySize, frameRate: frameRate,
                     videoBitrateKbps: videoBitrateKbps)
    }

    private func runSession(
        httpClient: PlankHTTPClient,
        identity: PlankHostIdentity,
        authentication: PlankAuthentication,
        connectedHost: HostBookmark,
        localDisplayLayout: PlankLocalDisplayLayout?,
        inputQueue: PlankInputQueue,
        generation: UUID
    ) async {
#if PLANK_TABLET_RELAY
        let preflight = PlankWacomPreflight(
            clientVersion: Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "unknown",
            hostVersion: identity.version,
            onChange: { [weak self] snapshot in
                Task { @MainActor [weak self] in
                    guard let self, self.streamGeneration == generation,
                          snapshot.sequence > self.tabletPreflightSequence else { return }
                    self.tabletPreflightSequence = snapshot.sequence
                    self.tabletPreflightSummary = snapshot.summary
                    self.updateTabletPreflight(snapshot, generation: generation)
                }
            }
        )
        let sessionTabletBridge = PlankRelaySessionBridge(
            inputQueue: inputQueue, preflight: preflight,
            reportLink: { [weak self] event in
                Task { @MainActor [weak self] in
                    guard let self, self.streamGeneration == generation else { return }
                    self.applyRelayLink(event)
                }
            }
        ) {
            [weak self] status in
            Task { @MainActor [weak self] in
                guard let self, self.streamGeneration == generation else { return }
                self.tabletRelayStatus = status
            }
        }
#if PLANK_NATIVE_MAC_WACOM
        let localSource = sessionNativeSource
        let localWacom = localSource.usesUSB ? PlankMacWacomSession(input: inputQueue, preflight: preflight) : nil
        nativeWacom = localWacom
        tabletBridge = localSource.usesRegisteredRelay ? sessionTabletBridge : nil
        if localSource.usesRegisteredRelay { sessionTabletBridge.setActive(tabletSceneActive) }
        let sourceDiagnostics: @Sendable () -> String = { localWacom != nil ? "local USB Wacom" : sessionTabletBridge.diagnosticSummary() }
        let closePhysicalCapture: @Sendable () -> Void = { localWacom?.closeSynchronously() }
#else
        tabletBridge = sessionTabletBridge
        sessionTabletBridge.setActive(tabletSceneActive)
        let sourceDiagnostics: @Sendable () -> String = { sessionTabletBridge.diagnosticSummary() }
        let closePhysicalCapture: @Sendable () -> Void = {}
#endif
        defer {
            sessionTabletBridge.close(reason: .sessionEnded)
            stoppingTabletBridge = sessionTabletBridge
            if tabletBridge === sessionTabletBridge { tabletBridge = nil }
#if PLANK_NATIVE_MAC_WACOM
            if nativeWacom === localWacom { nativeWacom = nil }
            // Also cover metadata/launch failures before a transport exists.
            Task { await localWacom?.close() }
#endif
        }
#else
        let closePhysicalCapture: @Sendable () -> Void = {}
#endif
        do {
            let frameMailbox = PlankFrameMailbox()
            let cursorMailbox = PlankCursorMailbox()
            let (readyAuthentication, hostTopology, applications) =
                try await loadSessionMetadata(
                    httpClient: httpClient,
                    authentication: authentication
                )
            try Task.checkCancellation()
            guard let desktop = applications.first(where: {
                $0.title.localizedCaseInsensitiveCompare("Desktop") == .orderedSame
            }) ?? applications.first else {
                throw PlankHTTPError.invalidResponse("The Host has no Desktop application.")
            }
            let requestedTopology = hostTopology.requesting(connectedHost.spatialDisplaySize,
                second: connectedHost.secondDisplaySize, primaryOutput: localDisplayLayout?.primarySpatialIndex)
            if let localDisplayLayout, connectedHost.secondDisplaySize != nil {
                NSLog("PLANK display request: localPrimarySide=%@ negotiatedPrimarySide=%@ modes=%@",
                    localDisplayLayout.primarySpatialIndex.map(String.init) ?? "unmapped",
                    requestedTopology.requestedPrimaryOutput.map(String.init) ?? "not supported",
                    requestedTopology.layout.virtualModes.joined(separator: ","))
            }
            let (activeHTTPClient, activeAuthentication, topology, launch) = try await launchDesktop(
                httpClient: httpClient,
                authentication: readyAuthentication,
                connectedHost: connectedHost,
                applicationID: desktop.id,
                requestedTopology: requestedTopology,
                // Read once per connection: a change applies to the next one.
                playAudioOnHost: PlankAudioPreferences.playOnHost()
            )
            if self.streamGeneration == generation {
                self.httpClient = activeHTTPClient
                self.lastAuthentication = activeAuthentication
                self.sessionLocalDisplayLayout = localDisplayLayout
                self.sessionTopology = topology
            }
            // Once launch has reserved the Host, enter the transport even if
            // cancellation arrived with the HTTP response. Its cancellation
            // handler then closes that fresh reservation cleanly.
#if PLANK_TABLET_RELAY
            let relayDiagnostics: @Sendable () -> String = {
                sourceDiagnostics()
            }
#else
            let relayDiagnostics: @Sendable () -> String = { "not built" }
#endif
            let sessionBitrate = PlankLiveBitrate(
                startupKbps: connectedHost.videoBitrateKbps
            ) { [weak self] status in
                Task { @MainActor [weak self] in
                    guard let self, self.streamGeneration == generation else { return }
                    self.liveBitrate = status
                }
            }
            if self.streamGeneration == generation {
                liveBitrateController = sessionBitrate
                liveBitrate = sessionBitrate.status
            }
            defer {
                if liveBitrateController === sessionBitrate {
                    liveBitrateController = nil
                    liveBitrate = nil
                }
            }
            try await PlankSessionEngine().stream(
                host: connectedHost.address.trimmingCharacters(in: CharacterSet(charactersIn: "[]")),
                topology: topology,
                frameRate: connectedHost.streamFrameRate,
                encoderTargetKbps: connectedHost.videoBitrateKbps,
                launch: launch,
                inputQueue: inputQueue,
                sessionLabel: String(generation.uuidString.prefix(8)),
                relayState: relayDiagnostics,
                onAudioProgress: { [weak self, videoDiagnostics] progress in
                    // Like video statistics, audio statistics reach the main
                    // actor only while the overlay is shown.
                    guard videoDiagnostics.isEnabled else { return }
                    Task { @MainActor [weak self] in
                        guard let self, self.streamGeneration == generation,
                              self.videoDiagnostics.isEnabled else { return }
                        self.audioDiagnosticText = progress.summary
                    }
                },
                shouldReportVideoProgress: { [videoDiagnostics] in videoDiagnostics.isEnabled },
                liveBitrate: sessionBitrate,
                beforeTransportClose: closePhysicalCapture
            ) { [weak self] frame in
                if frameMailbox.offer(frame) {
                    Task { @MainActor [weak self] in
                        guard let self, let newest = frameMailbox.takeNewest(),
                              self.streamGeneration == generation else { return }
                        self.latestFrame = newest
                        let dimensions = PlankFrameDimensions(
                            width: newest.width, height: newest.height
                        )
                        if self.frameDimensions != dimensions {
                            self.frameDimensions = dimensions
                        }
                        self.videoSurfaces.frame(newest)
                        if case .streaming = self.phase {
                            // Frame presentation already updates the surface directly.
                            // Connection phase changes only on a lifecycle transition.
                        } else {
                            self.phase = .streaming(identity, activeAuthentication, newest.frameNumber)
                        }
#if PLANK_PENCIL_RELAY_RECEIVER
                        self.pencilRelay.sync()
#endif
                    }
                }
            } onVideoProgress: { [weak self] progress in
                Task { @MainActor [weak self] in
                    guard let self, self.streamGeneration == generation,
                          self.videoDiagnostics.isEnabled else { return }
                    let shown = self.latestFrame?.frameNumber ?? 0
                    self.videoDiagnosticText =
                        "Received \(progress.received) · decoded \(progress.decoded) · shown \(shown)\n" +
                        "keys \(progress.keyFrames) · drops \(progress.receiveDrops) · " +
                        "gaps \(progress.frameGaps) · FEC \(progress.fecUnrecovered) · " +
                        String(format: "decode %.1f ms", progress.averageDecodeMilliseconds) +
                        " · \(progress.decoder)"
                }
            } onCursor: { [weak self] update in
                if cursorMailbox.offer(update) {
                    Task { @MainActor [weak self] in
                        let updates = cursorMailbox.takeNewest()
                        guard let self, self.streamGeneration == generation else { return }
                        for update in updates {
                            switch update {
                            case let .position(cursor):
                                self.remoteCursor = cursor
                                self.videoSurfaces.cursor(cursor)
                            case let .shape(shape):
                                self.remoteCursorShape = shape
                                self.videoSurfaces.shape(shape)
                            }
                        }
                    }
                }
            } onHostFeatures: { [weak self] flags in
#if PLANK_TABLET_RELAY
                preflight.observeHostFeatures(
                    rawHid: flags & PlankHostFeature.rawHidTablet != 0,
                    focusSuspend: flags & PlankHostFeature.rawHidFocusSuspend != 0
                )
#if PLANK_NATIVE_MAC_WACOM
                if localSource.usesRegisteredRelay { sessionTabletBridge.startIfPaired(hostFeatures: flags) }
#else
                sessionTabletBridge.startIfPaired(hostFeatures: flags)
#endif
#endif
                Task { @MainActor [weak self] in
                    guard let self, self.streamGeneration == generation else { return }
                    self.hostSupportsNormalizedPen = flags & PlankHostFeature.normalizedPen != 0
                    self.hostSupportsTabletRelay =
                        flags & PlankHostFeature.tabletRelayRequired ==
                        PlankHostFeature.tabletRelayRequired
#if PLANK_NATIVE_MAC_WACOM
                    localWacom?.hostFeatures(flags, active: self.tabletSceneActive)
#endif
                    if !self.hostSupportsTabletRelay {
                        let status = "This Host does not support Wacom Relay; continuing without it."
                        if self.waitingForTablet {
                            self.continueWithoutTablet(status: status)
                        } else {
                            self.tabletRelayStatus = status
                        }
                    } else if self.waitingForTablet {
                        self.scheduleTabletAvailabilityGrace(generation: generation)
                    }
                }
            } onRawHid: { [tabletControlReceiver] frame in
#if PLANK_TABLET_RELAY
                preflight.observeHostFrame(frame)
#if PLANK_NATIVE_MAC_WACOM
                if let localWacom { localWacom.control(frame) }
                else if localSource.usesRegisteredRelay { sessionTabletBridge.forwardHostFrame(frame) }
#else
                sessionTabletBridge.forwardHostFrame(frame)
#endif
#else
                tabletControlReceiver.deliver(frame)
#endif
            } onTabletFrameSent: { frame in
#if PLANK_TABLET_RELAY
                preflight.observeSentTabletFrame(frame)
#if PLANK_NATIVE_MAC_WACOM
                localWacom?.observeSent(frame)
#endif
#endif
            }
        } catch {
            guard !Task.isCancelled else { return }
#if PLANK_TABLET_RELAY
            sessionTabletBridge.close(reason: .upstreamSessionFailure)
            tabletRelayStatus = "Tablet link idle; start a desktop session to connect."
            applyRelayLink(.sessionEnded)
            tabletPreflightSummary = "No active Wacom preflight."
            waitingForTablet = false
            showingTabletWaitScreen = false
            tabletWaitTimeoutID = UUID()
#endif
            sessionTopology = nil
            sessionLocalDisplayLayout = nil
            latestFrame = nil; frameDimensions = nil
            videoSurfaces.frame(nil); videoSurfaces.cursor(nil); videoSurfaces.shape(nil)
            phase = .failed(Self.message(for: error))
        }
    }

    private func loadSessionMetadata(
        httpClient: PlankHTTPClient,
        authentication: PlankAuthentication
    ) async throws -> (PlankAuthentication, PlankTopology, [PlankApplication]) {
        func renewAuthentication() async throws -> PlankAuthentication {
            guard let credentials = transitionCredentials else {
                throw PlankHTTPError.invalidResponse(
                    "The Host restarted and needs a new sign-in."
                )
            }
            let renewed = try await httpClient.refreshAuthentication(
                username: credentials.username,
                password: credentials.password
            )
            lastAuthentication = renewed
            return renewed
        }

        func load() async throws -> (PlankTopology, [PlankApplication]) {
            async let topology = httpClient.fetchTopology()
            async let applications = httpClient.fetchApplications()
            return try await (topology, applications)
        }

        let currentAuthentication = httpClient.sessionToken == nil ?
            try await renewAuthentication() : authentication
        do {
            let (topology, applications) = try await load()
            return (currentAuthentication, topology, applications)
        } catch let PlankHTTPError.rejected(code, _) where code == 401 {
            let renewed = try await renewAuthentication()
            let (topology, applications) = try await load()
            return (renewed, topology, applications)
        }
    }

    private func launchDesktop(
        httpClient initialClient: PlankHTTPClient,
        authentication initialAuthentication: PlankAuthentication,
        connectedHost: HostBookmark,
        applicationID: Int,
        requestedTopology: PlankTopology,
        playAudioOnHost: Bool
    ) async throws -> (PlankHTTPClient, PlankAuthentication, PlankTopology, PlankLaunchCredentials) {
        try Task.checkCancellation()
        var needsAuthentication = false
        var waitingForRelease = false
        do {
            let launch = try await initialClient.launchDesktop(
                topology: requestedTopology,
                applicationID: applicationID,
                frameRate: connectedHost.streamFrameRate,
                playAudioOnHost: playAudioOnHost
            )
            return (initialClient, initialAuthentication, requestedTopology, launch)
        } catch let PlankHTTPError.rejected(code, _) where code == 425 {
            // A virtual display change replaces the Host worker. Mirror the
            // desktop Client's bounded wait, reauthentication and verification.
        } catch let PlankHTTPError.rejected(code, _) where code == 401 {
            needsAuthentication = true
        } catch let PlankHTTPError.rejected(code, _)
            where code == 409 || code == 503 {
            // A previous local stream can still be STOPPING at the Host even
            // after its Vision Pro window has closed. Encoder teardown can
            // also make the Host temporarily busy. Give either state a short
            // grace period; a persistent conflict is still reported.
            waitingForRelease = true
        }

        guard let credentials = transitionCredentials else {
            throw PlankHTTPError.invalidResponse(
                "The workstation changed its display layout, but the sign-in could not be resumed."
            )
        }

        var startupDeadline = PlankSessionStartupDeadline(
            now: ContinuousClock.now, waitingForRelease: waitingForRelease
        )
        let client = initialClient
        var authentication = initialAuthentication

        while startupDeadline.canRetry(at: ContinuousClock.now) {
            try Task.checkCancellation()
            try await Task.sleep(for: .milliseconds(500))

            do {
                if needsAuthentication {
                    authentication = try await client.refreshAuthentication(
                        username: credentials.username,
                        password: credentials.password
                    )
                    lastAuthentication = authentication
                    needsAuthentication = false
                }

                let currentTopology = try await client.fetchTopology()
                guard currentTopology.matches(connectedHost.spatialDisplaySize, second: connectedHost.secondDisplaySize,
                    primaryOutput: requestedTopology.requestedPrimaryOutput) else {
                    continue
                }
                let verifiedRequest = currentTopology.requesting(connectedHost.spatialDisplaySize, second: connectedHost.secondDisplaySize,
                    primaryOutput: requestedTopology.requestedPrimaryOutput)
                let launch = try await client.launchDesktop(
                    topology: verifiedRequest,
                    applicationID: applicationID,
                    frameRate: connectedHost.streamFrameRate,
                    playAudioOnHost: playAudioOnHost
                )
                return (client, authentication, verifiedRequest, launch)
            } catch let PlankHTTPError.rejected(code, _) where code == 401 {
                needsAuthentication = true
            } catch let PlankHTTPError.rejected(code, _) where code == 409 {
                waitingForRelease = true
                continue
            } catch let PlankHTTPError.rejected(code, _) where code == 425 {
                waitingForRelease = false
                startupDeadline.displayTransition()
                continue
            } catch let PlankHTTPError.rejected(code, _) where code == 503 {
                waitingForRelease = true
                continue
            } catch is CancellationError {
                throw CancellationError()
            } catch {
                // The replacement worker briefly drops HTTP while X restarts.
                needsAuthentication = true
            }
        }

        throw PlankHTTPError.invalidResponse(waitingForRelease ?
            "The previous workstation session is still closing. Please try again shortly." :
            "The workstation display layout did not become ready within 45 seconds."
        )
    }

    func disconnectSession() {
        guard !isClosingSession else { return }
#if PLANK_NATIVE_MAC_WACOM
        if let nativeWacom {
            self.nativeWacom = nil
            isClosingSession = true
            Task {
                await nativeWacom.close()
                isClosingSession = false
                disconnectAfterCaptureRelease()
            }
            return
        }
#endif
        disconnectAfterCaptureRelease()
    }

    private func disconnectAfterCaptureRelease() {
        let resumable: (PlankHostIdentity, PlankAuthentication)?
        switch phase {
        case let .startingSession(identity, authentication),
             let .frameReceived(identity, authentication, _),
             let .streaming(identity, authentication, _):
            resumable = (identity, authentication)
        case let .authenticated(identity, authentication):
            resumable = (identity, authentication)
        default:
            resumable = nil
        }
        let taskToStop = streamTask ?? stoppingStreamTask
        streamTask?.cancel()
        if let taskToStop { stoppingStreamTask = taskToStop }
        streamTask = nil
#if PLANK_TABLET_RELAY
        let closingTabletBridge = tabletBridge ?? stoppingTabletBridge
        closingTabletBridge?.close(reason: .userDisconnect)
        stoppingTabletBridge = closingTabletBridge
        tabletBridge = nil
        tabletSceneActive = false
        tabletRelayStatus = "Tablet link idle; start a desktop session to connect."
        applyRelayLink(.sessionEnded)
        tabletPreflightSummary = "No active Wacom preflight."
        tabletInputPolicy.begin(hasPairedTablet: false)
        waitingForTablet = false
        showingTabletWaitScreen = false
        tabletWaitTimeoutID = UUID()
#endif
        tabletControlReceiver.set(nil)
        hostSupportsNormalizedPen = false
        hostSupportsTabletRelay = false
        streamGeneration = UUID()
        let closingGeneration = streamGeneration
#if PLANK_PENCIL_RELAY_RECEIVER
        pencilRelay.endDesktop()
#endif
        inputQueue.stop()
        latestFrame = nil
        remoteCursor = nil
        remoteCursorShape = nil
        frameDimensions = nil
        sessionTopology = nil
        sessionLocalDisplayLayout = nil
        videoDiagnosticText = "Video starting…"
        audioDiagnosticText = "Audio starting…"
        liveBitrateController = nil
        liveBitrate = nil
        videoSurfaces.frame(nil)
        videoSurfaces.cursor(nil)
        videoSurfaces.shape(nil)
        if let resumable {
            phase = .authenticated(resumable.0, resumable.1)
        } else {
            phase = .idle
        }
        awaitStreamClosure(taskToStop, generation: closingGeneration)
    }

    @discardableResult
    private func awaitStreamClosure(_ task: Task<Void, Never>?, generation: UUID) -> Task<Void, Never>? {
#if PLANK_TABLET_RELAY
        let closingBridge = stoppingTabletBridge
        isClosingSession = task != nil || closingBridge != nil
        guard task != nil || closingBridge != nil else { return nil }
#else
        isClosingSession = task != nil
        guard task != nil else { return nil }
#endif
        return Task { [weak self] in
            // Closing the window never makes the Host session reusable. Wait
            // until the worker and Relay link have both released their sessions.
            await task?.value
#if PLANK_TABLET_RELAY
            await closingBridge?.waitForClose()
#endif
            guard let self, self.streamGeneration == generation else { return }
            self.stoppingStreamTask = nil
#if PLANK_TABLET_RELAY
            if self.stoppingTabletBridge === closingBridge {
                self.stoppingTabletBridge = nil
            }
#endif
            self.isClosingSession = false
        }
    }

    // Input handoff can anchor the mouse at the pen's latest Host position
    // without publishing cursor updates through SwiftUI.
    func currentPointerPosition() -> (x: Int, y: Int)? {
        guard let remoteCursor else { return nil }
        return (Int(remoteCursor.x), Int(remoteCursor.y))
    }

#if PLANK_NATIVE_MAC_WACOM
    var nativeMouseOwnsPointer: Bool { inputQueue.nativeMouseOwnsPointer }
#endif

    func movePointer(x: Int, y: Int, width: Int, height: Int) {
#if PLANK_PENCIL_RELAY_RECEIVER
        guard !pencilRelay.strokeActive else { return }
#endif
#if PLANK_TABLET_RELAY
        guard !waitingForTablet else { return }
#endif
        guard width > 1, height > 1 else { return }
        let maximumX = min(width - 1, Int(UInt16.max))
        let maximumY = min(height - 1, Int(UInt16.max))
        inputQueue.append(.pointer(
            x: UInt16(clamping: x),
            y: UInt16(clamping: y),
            maximumX: UInt16(maximumX),
            maximumY: UInt16(maximumY)
        ))
    }

    var acceptsNormalizedPen: Bool {
        guard hostSupportsNormalizedPen else { return false }
#if PLANK_PENCIL_RELAY_RECEIVER
        guard !pencilRelay.ownsPen else { return false }
#endif
#if PLANK_TABLET_RELAY
        // A selected raw Relay owns the Host tablet backend. Never alternate
        // that backend with normalized Pencil reports.
#if PLANK_NATIVE_MAC_WACOM
        guard sessionNativeSource == .off else { return false }
#else
        guard PlankRelayKeys.relayRegistry().selection == .off else { return false }
#endif
#endif
        return true
    }

    func sendPen(_ pen: PlankNormalizedPen) {
        guard acceptsNormalizedPen else { return }
        inputQueue.append(.pen(pen))
    }

    func setLeftButton(pressed: Bool) {
        setMouseButton(number: 1, pressed: pressed)
    }

    func setMouseButton(number: UInt8, pressed: Bool) {
#if PLANK_PENCIL_RELAY_RECEIVER
        guard !pressed || !pencilRelay.strokeActive else { return }
#endif
#if PLANK_TABLET_RELAY
        guard tabletInputPolicy.allowsMouseButton(number, pressed: pressed) else { return }
#endif
        inputQueue.append(.button(number: number, pressed: pressed))
    }

    func clickLeftButton() {
        clickMouseButton(number: 1)
    }

    func clickMouseButton(number: UInt8) {
        setMouseButton(number: number, pressed: true)
        setMouseButton(number: number, pressed: false)
    }

    func scroll(vertical: Int16, horizontal: Int16 = 0) {
#if PLANK_PENCIL_RELAY_RECEIVER
        guard !pencilRelay.strokeActive else { return }
#endif
#if PLANK_TABLET_RELAY
        guard !waitingForTablet else { return }
#endif
        guard vertical != 0 || horizontal != 0 else { return }
        inputQueue.append(.scroll(vertical: vertical, horizontal: horizontal))
    }

    func sendText(_ text: String) {
#if PLANK_TABLET_RELAY
        guard !waitingForTablet else { return }
#endif
        guard let data = text.data(using: .utf8), !data.isEmpty else { return }
        inputQueue.append(.text(data))
    }

    // Transport-neutral atomic sink for direct iPad controls. The action
    // ledger is committed by its owner only if this complete batch is admitted.
    func offerControlKeys(_ batch: [PlankControlKeyEvent]) -> Bool {
#if PLANK_TABLET_RELAY
        var policy = tabletInputPolicy
        guard batch.allSatisfy({ policy.allowsKey($0.code,pressed:$0.pressed,modifiers:$0.modifiers) }) else { return false }
#endif
        guard inputQueue.offerControlKeys(batch) else { return false }
#if PLANK_TABLET_RELAY
        tabletInputPolicy = policy
#endif
        return true
    }

    func pressKey(code: UInt16, modifiers: UInt8 = 0) {
        sendKey(code: code, pressed: true, modifiers: modifiers)
        sendKey(code: code, pressed: false, modifiers: modifiers)
    }

    func sendKey(code: UInt16, pressed: Bool, modifiers: UInt8) {
#if PLANK_TABLET_RELAY
        guard tabletInputPolicy.allowsKey(code, pressed: pressed, modifiers: modifiers) else { return }
#endif
#if PLANK_PENCIL_RELAY_RECEIVER
        guard let event = pencilKeyOwnership.update(code:code,pressed:pressed,modifiers:modifiers,source:.local) else { return }
        inputQueue.append(.key(code:event.code,pressed:event.pressed,modifiers:event.modifiers))
#else
        inputQueue.append(.key(code: code, pressed: pressed, modifiers: modifiers))
#endif
    }

    func authenticate(username: String, password: String) async {
        guard let httpClient,
              let identity = lastIdentity else {
            phase = .failed("The workstation connection is no longer ready.")
            return
        }

        phase = .authenticating(identity)
        do {
            transitionCredentials = (username, password)
            let authentication = try await httpClient.authenticate(
                username: username,
                password: password
            )
            lastAuthentication = authentication
            phase = .authenticated(identity, authentication)
        } catch {
            transitionCredentials = nil
            lastAuthentication = nil
            phase = .failed(Self.message(for: error))
        }
    }

    func retryCredentials() {
        guard let identity = lastIdentity else { return }
        phase = .needsCredentials(identity)
    }

    func requiresSessionCloseForBookmark(_ edited: HostBookmark) -> Bool {
        PlankBookmarkSessionPolicy.requiresClosure(
            edited: edited, connected: connectedHost,
            authenticated: httpClient != nil && lastAuthentication != nil
        )
    }

    func closeSessionForBookmarkChange(_ edited: HostBookmark) async {
        guard requiresSessionCloseForBookmark(edited) else { return }
        NSLog("PLANK session: closing authenticated connection for bookmark change")
        // reset clears HTTP/session credentials immediately and invalidates old
        // callbacks. Save cannot complete until both stream and Relay have ended.
        let closure = reset()
        await closure?.value
        NSLog("PLANK session: bookmark teardown complete; fresh sign-in required")
    }

    @discardableResult
    func reset() -> Task<Void, Never>? {
#if PLANK_NATIVE_MAC_WACOM
        if let nativeWacom {
            self.nativeWacom = nil
            isClosingSession = true
            return Task {
                await nativeWacom.close()
                isClosingSession = false
                await resetAfterCaptureRelease()?.value
            }
        }
#endif
        return resetAfterCaptureRelease()
    }

    private func resetAfterCaptureRelease() -> Task<Void, Never>? {
        let taskToStop = streamTask ?? stoppingStreamTask
        taskToStop?.cancel()
        if let taskToStop { stoppingStreamTask = taskToStop }
        streamTask = nil
#if PLANK_TABLET_RELAY
        let closingTabletBridge = tabletBridge ?? stoppingTabletBridge
        closingTabletBridge?.close(reason: .userDisconnect)
        stoppingTabletBridge = closingTabletBridge
        tabletBridge = nil
        tabletSceneActive = false
        tabletRelayStatus = "Tablet link idle; start a desktop session to connect."
        applyRelayLink(.sessionEnded)
        tabletPreflightSummary = "No active Wacom preflight."
        tabletInputPolicy.begin(hasPairedTablet: false)
        waitingForTablet = false
        showingTabletWaitScreen = false
        tabletWaitTimeoutID = UUID()
#endif
        tabletControlReceiver.set(nil)
        hostSupportsNormalizedPen = false
        hostSupportsTabletRelay = false
        streamGeneration = UUID()
        let closingGeneration = streamGeneration
#if PLANK_PENCIL_RELAY_RECEIVER
        pencilRelay.endDesktop()
#endif
        inputQueue.stop()
        httpClient = nil
        lastIdentity = nil
        connectedHost = nil
        activeHostID = nil
        transitionCredentials = nil
        lastAuthentication = nil
        let closure = awaitStreamClosure(taskToStop, generation: closingGeneration)
        latestFrame = nil
        remoteCursor = nil
        remoteCursorShape = nil
        frameDimensions = nil
        sessionTopology = nil
        sessionLocalDisplayLayout = nil
        videoDiagnosticText = "Video starting…"
        audioDiagnosticText = "Audio starting…"
        liveBitrateController = nil
        liveBitrate = nil
        videoSurfaces.frame(nil)
        videoSurfaces.cursor(nil)
        videoSurfaces.shape(nil)
        phase = .idle
        return closure
    }

    func registerVideoSurface(
        id: UUID,
        frame: @escaping (PlankRenderedFrame?) -> Void,
        cursor: @escaping (PlankRemoteCursor?) -> Void,
        cursorShape: @escaping (PlankRemoteCursorShape?) -> Void
    ) {
        videoSurfaces.register(id: id, surface: .init(frame: frame, cursor: cursor, shape: cursorShape))
        frame(latestFrame)
        cursor(remoteCursor)
        cursorShape(remoteCursorShape)
    }

    func unregisterVideoSurface(id: UUID) {
        videoSurfaces.unregister(id: id)
    }

    private static func message(for error: Error) -> String {
        if let error = error as? LocalizedError,
           let description = error.errorDescription {
            return description
        }
        return error.localizedDescription
    }
}
