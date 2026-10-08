import SwiftUI

struct SettingsView: View {
    @ObservedObject var client: PlankCoreClient
    @AppStorage("plank.vision.showStatistics") private var showStatistics = false
    @AppStorage("plank.vision.debugPrefer96Hz") private var debugPrefer96Hz = false
    @AppStorage("plank.vision.preferHEVC") private var preferHEVC = true
    @AppStorage("plank.vision.keyboardFunctionKeyMode") private var keyboardFunctionKeyMode = KeyboardFunctionKeyMode.pc.rawValue
    @AppStorage(PlankAudioPreferences.playOnHostKey) private var playAudioOnHost = true
    @AppStorage("plank.vision.timingCapture") private var timingCapture = false
#if PLANK_TABLET_RELAY
    @ObservedObject private var relayHandoff = PlankRelayHandoffInbox.shared
    @Environment(\.openURL) private var openURL
    @State private var setupOpenFailed = false
#endif

    private var desktopSessionActive: Bool {
        switch client.phase {
        case .startingSession, .frameReceived, .streaming: true
        default: false
        }
    }

    var body: some View {
        Form {
            Section("Streaming") {
                Toggle("Prefer HEVC", isOn: $preferHEVC)
            }

            Section("Audio") {
                Toggle("Also play on workstation speakers", isOn: $playAudioOnHost)
                Text("Applies to the next connection. Workstation audio always plays in the headset; volume and mute are in the session controls at the top of the desktop window.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }

            Section("Debug Overlays") {
                Toggle("Show video decoding statistics", isOn: $showStatistics)
                Text("Shows frame counts, the active decoder, and display-link timing in the desktop window.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                Toggle("Record video timing capture", isOn: $timingCapture)
                Text("Logs one line per second (up to 15 minutes per session) with Host frame sizes, arrival, decode, hand-off and presentation timing. Independent of the statistics overlay.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                if showStatistics {
                    Toggle("Request 96 Hz timing for 24/48 fps (test)", isOn: $debugPrefer96Hz)
                    Text("A timing preference for the display link. visionOS may choose another rate; compare the timing readout with this switch off and on.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }

            Section("Physical Keyboard") {
                Picker("Keyboard type", selection: $keyboardFunctionKeyMode) {
                    ForEach(KeyboardFunctionKeyMode.allCases) { mode in
                        Text(mode.title).tag(mode.rawValue)
                    }
                }

                Text(keyboardFunctionKeyMode == KeyboardFunctionKeyMode.appleExtended.rawValue ?
                     "Apple F13 through F24 are forwarded as literal function keys." :
                     "The top-right Windows keys act as Print Screen, Scroll Lock and Pause.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }

#if PLANK_TABLET_RELAY
            Section("Tablet Relay") {
                Picker("Tablet Relay", selection: relaySelection) {
                    Text("Off").tag(RelayChoice.off.tag)
                    ForEach(relayHandoff.registry.relays, id: \.drawingIdentity) { relay in
                        Text(relayHandoff.registry.displayName(for: relay.drawingIdentity) ?? relay.name)
                            .tag(RelayChoice.relay(relay.drawingIdentity).tag)
                    }
                    if showsEarlierPairing {
                        Text("Earlier paired Relay").tag(RelayChoice.earlierPairing.tag)
                    }
                    Text("Set up a Relay…").tag(RelayChoice.setUp.tag)
                }
                if let relay = relayHandoff.registry.activeRelay {
                    Picker("Drawing connection", selection: Binding(
                        get: { relay.pendingTransport ?? relay.transport },
                        set: { relayHandoff.selectTransport($0, desktopSessionActive: desktopSessionActive) }
                    )) {
                        ForEach(relay.availableTransports, id: \.rawValue) { choice in
                            Text(choice.title).tag(choice)
                        }
                    }
                    Text(relay.pendingTransport != nil ? "The connection choice will change after disconnecting." :
                         "Automatic tries the saved network routes, then Bluetooth. Changes apply on the next connection.")
                        .font(.footnote).foregroundStyle(.secondary)
                    if relay.bluetoothIdentifier == nil {
                        Text("To add Bluetooth drawing, open this Relay over Bluetooth in Relay Setup and choose Use in PLANK.")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                }
                LabeledContent("Status", value: relayStatus.title)
                Text(relayStatus.detail)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                if desktopSessionActive && relayHandoff.registry.selection != .off {
                    Text(client.tabletRelayStatus)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
                if setupOpenFailed {
                    Label("Relay Setup could not be opened. Install PLANK AVP Relay Setup on this headset, then choose Set up a Relay… again.",
                          systemImage: "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(.orange)
                }
                if showsHandoffBanner {
                    handoffBanner
                }
                DisclosureGroup("Connection details") {
                    VStack(alignment: .leading, spacing: 8) {
                        LabeledContent("Saved connection", value: relayHandoff.registry.activeRelay?.transport.title ?? "Network")
                        if desktopSessionActive { LabeledContent("Active connection", value: client.tabletRelayTransport ?? "Not connected") }
                        if desktopSessionActive {
                            Text(client.tabletPreflightSummary)
                        }
                        Text("The workstation connection still uses the headset’s network. Discovery, tablet setup and connection tests are in Relay Setup.")
                    }
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                }
            }
#endif

            Section("About") {
                LabeledContent("Client", value: "PLANK for Apple Vision Pro")
                LabeledContent("Version", value: Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "unknown")
                LabeledContent("Build", value: Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "unknown")
                if let branch = Bundle.main.infoDictionary?["PLANKBuildBranch"] as? String,
                   !branch.isEmpty, branch != "main" {
                    LabeledContent("Branch", value: branch)
                }
            }
        }
        .onChange(of: timingCapture) { _, enabled in
            PlankTimingCapture.shared.setEnabled(enabled)
        }
        .navigationTitle("Settings")
        .frame(minWidth: 560, minHeight: 420)
#if PLANK_TABLET_RELAY
        .onAppear { relayHandoff.reloadRegistry() }
#endif
    }

#if PLANK_TABLET_RELAY
    private enum RelayChoice: Equatable {
        case off, earlierPairing, setUp
        case relay(String)

        var tag: String {
            switch self {
            case .off: "off"
            case .earlierPairing: "earlier"
            case .setUp: "setup"
            case let .relay(identity): "relay:" + identity
            }
        }

        init(_ selection: PlankRelaySelection) {
            switch selection {
            case .off: self = .off
            case .earlierPairing: self = .earlierPairing
            case let .relay(identity): self = .relay(identity)
            }
        }

        init?(tag: String) {
            switch tag {
            case "off": self = .off
            case "earlier": self = .earlierPairing
            case "setup": self = .setUp
            default:
                guard tag.hasPrefix("relay:") else { return nil }
                self = .relay(String(tag.dropFirst("relay:".count)))
            }
        }

        var selection: PlankRelaySelection? {
            switch self {
            case .off: .off
            case .earlierPairing: .earlierPairing
            case let .relay(identity): .relay(identity)
            case .setUp: nil
            }
        }
    }

    /// Shows the choice the user made; a change made during a desktop
    /// session is applied after disconnect and the status says so.
    private var relaySelection: Binding<String> {
        Binding(
            get: {
                RelayChoice(relayHandoff.registry.pendingSelection ??
                            relayHandoff.registry.selection).tag
            },
            set: { tag in
                guard let choice = RelayChoice(tag: tag) else { return }
                guard let selection = choice.selection else {
                    openRelaySetup()
                    return
                }
                relayHandoff.select(selection, desktopSessionActive: desktopSessionActive)
                client.clearTabletRelayObservation()
            }
        )
    }

    /// Transitional only: shown while the earlier address/service pairing is
    /// actually selected or pending, never as a way to add a Relay.
    private var showsEarlierPairing: Bool {
        relayHandoff.registry.offersEarlierPairing
    }

    private func name(for selection: PlankRelaySelection?) -> String? {
        switch selection {
        case let .relay(identity): relayHandoff.registry.displayName(for: identity)
        case .earlierPairing: "Earlier paired Relay"
        case .off, nil: nil
        }
    }

    private var relayStatus: PlankRelayStatus {
        let registry = relayHandoff.registry
        return PlankRelayStatus.make(
            selection: registry.selection,
            relayName: name(for: registry.selection),
            approvalPending: relayHandoff.registration.pending != nil,
            link: client.tabletRelayLink,
            deferredName: registry.pendingSelection.map { name(for: $0) ?? "Off" }
        )
    }

    /// The actual route or interface when known. Never inferred: a headset on
    /// Wi-Fi says nothing about how the Relay is attached.
    private var drawingRouteLabel: String {
        switch relayHandoff.registry.selection {
        case .off: "None"
        case .earlierPairing: "Saved network address"
        case .relay: relayHandoff.registry.activeRouteSelection?.routeLabel ?? "Network"
        }
    }

    private func openRelaySetup() {
        guard let url = URL(string: "plank-relay-setup://open") else { return }
        setupOpenFailed = false
        openURL(url) { accepted in setupOpenFailed = !accepted }
    }

    private var showsHandoffBanner: Bool {
        switch relayHandoff.state {
        case .idle, .needsApproval: false
        default: true
        }
    }

    @ViewBuilder
    private var handoffBanner: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(relayHandoff.state.message)
                .font(.footnote)
            HStack {
                switch relayHandoff.state {
                case .identityChanged, .migrationConflict:
                    Button("Open Relay Setup") { openRelaySetup() }
                        .buttonStyle(.bordered)
                default:
                    EmptyView()
                }
                if case .migrationConflict = relayHandoff.state {
                    EmptyView()
                } else {
                    Button("Dismiss") { relayHandoff.dismiss() }
                        .buttonStyle(.borderless)
                }
            }
        }
    }
#endif
}
