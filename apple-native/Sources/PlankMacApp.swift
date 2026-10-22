import SwiftUI
import AppKit

@main
struct PlankMacApp: App {
    @NSApplicationDelegateAdaptor(PlankMacDelegate.self) private var delegate
    @StateObject private var store = HostStore()
    @StateObject private var client = PlankCoreClient()
    @StateObject private var relay = PlankMacRelayHost()
    var body: some Scene {
        Window("PLANK Native Pilot", id: "browser") {
            PlankMacBrowser().environmentObject(store).environmentObject(client)
                .environmentObject(relay).onAppear { delegate.client = client; delegate.relay = relay }
        }.defaultSize(width: 860, height: 600)
        Window("PLANK Desktop", id: "desktop") {
            PlankMacDesktop().environmentObject(client)
        }.defaultSize(width: 1280, height: 760).windowResizability(.contentMinSize)
        Window("PLANK Desktop · Display 2", id: "desktop-secondary") {
            PlankMacDesktop(outputIndex: 1).environmentObject(client)
        }.defaultSize(width: 1100, height: 760).windowResizability(.contentMinSize)
        Settings { PlankMacSettings().environmentObject(relay).environmentObject(client) }
    }
}
struct PlankMacSettings: View {
    @EnvironmentObject private var relay: PlankMacRelayHost
    @EnvironmentObject private var client: PlankCoreClient
    @AppStorage("plank.mac.tablet-source") private var tablet = "usb"
    @State private var speakers = PlankAudioPreferences.playOnHost()
    var body: some View {
        Form {
            Picker("Tablet", selection: $tablet) {
                ForEach([PlankMacTabletSource.off, .usb, .pencil]) { Text($0.title).tag($0.rawValue) }
            }.disabled(client.hasActiveDesktopSession || client.isClosingSession || relay.sharing)
                .onChange(of: tablet) { old, new in
                    if old == PlankMacTabletSource.pencil.rawValue, old != new { client.pencilRelay.disconnect() }
                }
            Text("Tablet selection applies on the next connection. USB capture requires macOS Input Monitoring permission.").font(.caption).foregroundStyle(.secondary)
            if tablet == PlankMacTabletSource.pencil.rawValue {
                PlankPencilRelaySettings(receiver: client.pencilRelay)
            }
            Section("Tablet Relay") {
                Toggle("Share tablet with PLANK", isOn: Binding(get: { relay.sharing }, set: { if $0 { relay.start() } else { relay.stop() } }))
                    .disabled(client.phase.macHasVideoSession)
                Text(relay.message).font(.caption).foregroundStyle(.secondary)
                if relay.approvalPending {
                    Button("Approve Relay Setup") { relay.approveSetup() }.buttonStyle(.borderedProminent)
                    Button("Decline and Stop Sharing") { relay.stop() }
                }
                Text("Share a USB Wacom with an approved headset over the local network. Register this Mac in Relay Setup. The tablet cannot be used by a Mac desktop session while shared.").font(.caption).foregroundStyle(.secondary)
            }
            Toggle("Also play on workstation speakers", isOn: $speakers)
                .onChange(of: speakers) { _, value in UserDefaults.standard.set(value, forKey: PlankAudioPreferences.playOnHostKey) }
            LabeledContent("Build", value: Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "unknown")
            if let branch = Bundle.main.infoDictionary?["PLANKBuildBranch"] as? String,
               !branch.isEmpty, branch != "main" {
                LabeledContent("Branch", value: branch)
            }
        }.padding(24).frame(width: 460)
            .onAppear { NSCursor.arrow.set() }
            .background(PlankMacPencilSettingsPause(client:client))
    }
}

@MainActor final class PlankMacDelegate: NSObject, NSApplicationDelegate {
    weak var client: PlankCoreClient?
    weak var relay: PlankMacRelayHost?
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        relay?.stop()
        guard let client else { return .terminateNow }
        Task { await client.reset()?.value; sender.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }
}

private extension ConnectionPhase {
    var macHasVideoSession: Bool {
        switch self { case .streaming, .frameReceived, .startingSession: true; default: false }
    }
}
