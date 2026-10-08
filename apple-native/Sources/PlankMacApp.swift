import SwiftUI
import AppKit

@main
struct PlankMacApp: App {
    @NSApplicationDelegateAdaptor(PlankMacDelegate.self) private var delegate
    @StateObject private var store = HostStore()
    @StateObject private var client = PlankCoreClient()
    var body: some Scene {
        Window("PLANK Native Pilot", id: "browser") {
            PlankMacBrowser().environmentObject(store).environmentObject(client)
                .onAppear { delegate.client = client }
        }.defaultSize(width: 860, height: 600)
        Window("PLANK Desktop", id: "desktop") {
            PlankMacDesktop().environmentObject(client)
        }.defaultSize(width: 1280, height: 760).windowResizability(.contentMinSize)
        Settings { PlankMacSettings() }
    }
}
struct PlankMacSettings: View {
    @AppStorage("plank.mac.tablet-source") private var tablet = "usb"
    @State private var speakers = PlankAudioPreferences.playOnHost()
    var body: some View {
        Form {
            Picker("Tablet", selection: $tablet) {
                ForEach([PlankMacTabletSource.off, .usb]) { Text($0.title).tag($0.rawValue) }
            }
            Text("Tablet selection applies on the next connection. USB capture requires macOS Input Monitoring permission.").font(.caption).foregroundStyle(.secondary)
            Toggle("Also play on workstation speakers", isOn: $speakers)
                .onChange(of: speakers) { _, value in UserDefaults.standard.set(value, forKey: PlankAudioPreferences.playOnHostKey) }
            LabeledContent("Build", value: Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "unknown")
            if let branch = Bundle.main.infoDictionary?["PLANKBuildBranch"] as? String,
               !branch.isEmpty, branch != "main" {
                LabeledContent("Branch", value: branch)
            }
        }.padding(24).frame(width: 460)
            .onAppear { NSCursor.arrow.set() }
    }
}

@MainActor final class PlankMacDelegate: NSObject, NSApplicationDelegate {
    weak var client: PlankCoreClient?
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let client else { return .terminateNow }
        Task { await client.reset()?.value; sender.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }
}
