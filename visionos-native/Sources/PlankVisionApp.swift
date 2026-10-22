import SwiftUI

@main
struct PlankVisionApp: App {
    @StateObject private var store = HostStore()
    @StateObject private var client = PlankCoreClient()
#if PLANK_TABLET_RELAY
    // The inbox lives at App scope so a Relay handoff link is parsed and
    // decided independently of which window happens to be showing.
    @StateObject private var relayHandoff = PlankRelayHandoffInbox.shared
    @StateObject private var relayApproval = PlankRelayDrawingApproval()
#endif

    init() {
        // Builds before the keyboard-type selector always applied the Apple
        // extended-keyboard convention. Migrate existing installs once so a
        // paired Windows keyboard starts with literal PC key meanings.
        let defaults = UserDefaults.standard
        let migrationKey = "plank.vision.keyboardTypeDefaultVersion"
        if defaults.integer(forKey: migrationKey) < 1 {
            defaults.set(
                KeyboardFunctionKeyMode.pc.rawValue,
                forKey: "plank.vision.keyboardFunctionKeyMode"
            )
            defaults.set(1, forKey: migrationKey)
        }

#if PLANK_TABLET_RELAY
        // Identity-indexed Relay approvals (contract 10.9). Additive and
        // rollback-safe: it only re-indexes 32-byte values already in the
        // Keychain, never deletes or overwrites an approval, and writes nothing
        // when two legacy accounts disagree.
        let identityMigrationKey = PlankDrawingDefaultsKeys.migrationVersion
        if defaults.integer(forKey: identityMigrationKey) < 1 {
            PlankRelayKeys.migrateDrawingIdentityIndex()
        }
#endif
    }

#if PLANK_TABLET_RELAY
    private var desktopSessionActive: Bool {
        switch client.phase {
        case .startingSession, .frameReceived, .streaming: true
        default: false
        }
    }
#endif

    var body: some Scene {
        // There is one workstation browser. Reopening it must reveal that
        // browser instead of creating another owner of its local UI state.
        Window("PLANK", id: "plank-browser") {
            HostBrowserView(store: store, client: client)
#if PLANK_TABLET_RELAY
                .onOpenURL { relayHandoff.receive($0, desktopSessionActive: desktopSessionActive) }
                .onChange(of: desktopSessionActive) { _, active in
                    // Honour the offer made while a session was running. The
                    // deferred link is re-evaluated, never replayed as input,
                    // and a deferred picker change is applied.
                    relayHandoff.desktopSessionChanged(active)
                }
                .sheet(isPresented: Binding(
                    get: { relayHandoff.registration.pending != nil },
                    set: { if !$0 { relayHandoff.cancelRegistration(relayApproval) } }
                )) {
                    PlankRelayRegistrationSheet(inbox: relayHandoff, approval: relayApproval) {
                        desktopSessionActive
                    }
                }
#endif
        }
        .defaultSize(width: 980, height: 700)
        .defaultLaunchBehavior(.presented)

        WindowGroup("PLANK Desktop", id: "plank-desktop") {
            // Group is layout-transparent, so the desktop window's presentation
            // is unchanged; it only carries the link handler.
            Group {
                if let hostID = client.activeHostID,
                   let host = store.hosts.first(where: { $0.id == hostID }) {
                    RemoteDesktopView(host: host, client: client)
                } else {
                    RestoredDesktopWindow()
                }
            }
#if PLANK_TABLET_RELAY
            // A link delivered while the desktop window is front is declined
            // with an offer for after disconnect; a stroke is never interrupted.
            .onOpenURL { relayHandoff.receive($0, desktopSessionActive: desktopSessionActive) }
#endif
        }
        .defaultSize(width: 1280, height: 720)
        .windowStyle(.plain)
        .restorationBehavior(.disabled)
        .defaultLaunchBehavior(.suppressed)
    }
}

private struct RestoredDesktopWindow: View {
    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismissWindow) private var dismissWindow

    var body: some View {
        ContentUnavailableView {
            Label("Session Closed", systemImage: "display.trianglebadge.exclamationmark")
        } description: {
            Text("Return to PLANK to start a new workstation session.")
        } actions: {
            Button("Return to PLANK") { returnToBrowser() }
                .buttonStyle(.borderedProminent)
        }
        .onAppear { returnToBrowser() }
    }

    private func returnToBrowser() {
        openWindow(id: "plank-browser")
        Task { @MainActor in
            // visionOS cannot dismiss the app's last window, so give the
            // browser scene time to open before closing this restored scene.
            try? await Task.sleep(for: .milliseconds(500))
            dismissWindow(id: "plank-desktop")
        }
    }
}
