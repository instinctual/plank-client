import SwiftUI
import AppKit
struct PlankMacDesktop: View {
    @EnvironmentObject private var client: PlankCoreClient
    var outputIndex = 0
    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismissWindow) private var dismissWindow
    @State private var controls = false
    @State private var secondDisplayOpen = false
    @AppStorage(PlankAudioPreferences.volumeKey) private var volume = Double(PlankAudioPreferences.volume())
    @AppStorage(PlankAudioPreferences.mutedKey) private var muted = PlankAudioPreferences.muted()
    @State private var desktopWindow: NSWindow?
    @State private var placedGeneration: String?
    @State private var screens = PlankMacDisplayPriority.screens()
    var body: some View {
        ZStack(alignment: .topLeading) {
            PlankMacSurface(client: client, windowChanged: { if let window = $0 {
                desktopWindow = window
                placeOnAssignedDisplay()
            } }, localControlsPresented: controls || client.showingTabletWaitScreen,
                            topology: client.sessionTopology, outputIndex: outputIndex)
            if client.videoDiagnosticsEnabled { Text(client.videoDiagnosticText + "\n" + client.audioDiagnosticText).font(.system(.caption, design: .monospaced)).padding(10).background(.black.opacity(0.8)).foregroundStyle(.white).padding().allowsHitTesting(false) }
            if client.showingTabletWaitScreen {
                VStack { ProgressView("Waiting for Wacom…"); Button("Continue without tablet") { client.continueWithoutTablet() } }.padding(24).background(.regularMaterial)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }.frame(minWidth: 640, maxWidth: .infinity, minHeight: 360, maxHeight: .infinity)
        .toolbar {
            ToolbarItem { Button {
                NSLog("PLANK Mac fullscreen action: toolbar")
                PlankMacSessionWindows.toggleFullScreen(client: client)
            } label: { Label("Full Screen", systemImage: "arrow.up.left.and.arrow.down.right") }
                .onHover { if $0 { NSCursor.arrow.set() } }
                .background(PlankMacLocalPointerRegion())
                .help("Enter or leave full screen for all desktop windows").keyboardShortcut("f", modifiers: [.control, .command]) }
            ToolbarItem { Button { controls.toggle() } label: { Label("Session Controls", systemImage: "slider.horizontal.3") }
                .onHover { if $0 { NSCursor.arrow.set() } }
                .background(PlankMacLocalPointerRegion())
                .popover(isPresented: $controls, arrowEdge: .bottom) { sessionControls } }
            ToolbarItem { Button("Disconnect", role: .destructive) {
                PlankMacSessionWindows.disconnect(client: client) {
                    dismissWindow(id: "desktop-secondary")
                    dismissWindow(id: "desktop")
                }
            }
                .onHover { if $0 { NSCursor.arrow.set() } }
                .background(PlankMacLocalPointerRegion()) }
        }
        .onDisappear {
            if outputIndex == 0, client.hasActiveDesktopSession {
                client.disconnectSession()
            }
        }
        .onAppear { synchronizeWindows() }
        .onChange(of: client.sessionTopology) { _, _ in synchronizeWindows() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didChangeScreenParametersNotification)) { _ in
            screens = PlankMacDisplayPriority.screens()
            placedGeneration = nil
            placeOnAssignedDisplay()
        }
        .onReceive(NotificationCenter.default.publisher(for: NSWindow.didExitFullScreenNotification)) { event in
            if let window = event.object as? NSWindow, window === desktopWindow { placeOnAssignedDisplay() }
        }
        .onChange(of: client.phase) { _, phase in if case .failed = phase { closeThisWindow() } }
    }
    private var sessionControls: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Session Controls").font(.headline)
            if let topology = client.sessionTopology, topology.splitPresentation,
               topology.macPresentationOutputs.indices.contains(outputIndex) {
                let output = topology.macPresentationOutputs[outputIndex]
                Text("\(output.primary ? "Primary" : "Secondary") display · \(output.width) × \(output.height)").font(.caption)
                if let layout = client.sessionLocalDisplayLayout, let side = layout.primarySpatialIndex,
                   !topology.orderedOutputs[side].primary {
                    Text("This Host has not matched the Mac's primary display.").font(.caption).foregroundStyle(.secondary)
                }
                Menu("Move to Mac display") {
                    ForEach(Array(screens.enumerated()), id: \.offset) { index, screen in
                        Button("\(index == 0 ? "Primary" : "Secondary"): \(screen.localizedName)") {
                            if let window = desktopWindow { place(window, on: screen) }
                        }
                    }
                }.disabled(desktopWindow?.styleMask.contains(.fullScreen) == true)
                if outputIndex == 0 {
                    Button(secondDisplayOpen ? "Show second display" : "Reopen second display") {
                        controls = false
                        DispatchQueue.main.async {
                            PlankMacSessionWindows.showSecondDisplay(client: client, excluding: desktopWindow) {
                                openWindow(id: "desktop-secondary")
                            }
                        }
                    }.buttonStyle(.borderless)
                }
            }
            HStack { Button { muted.toggle(); applyAudio() } label: { Image(systemName: muted ? "speaker.slash" : "speaker.wave.2") }; PlankMacSlider(value: $volume, range: 0...1, label: "Audio volume").frame(height: 24).onChange(of: volume) { _,_ in applyAudio() } }.accessibilityLabel("Audio volume")
            if let status = client.liveBitrate {
                Text("Bitrate · \(StreamBitrate.megabitsLabel(status.currentKbps))")
                PlankMacSlider(value: Binding(get: { Double(client.liveBitrate?.currentKbps ?? status.startupKbps) }, set: { client.chooseLiveBitrate(Int($0), final: false) }), range: 10_000...150_000, step: 500, label: "Video bitrate") { editing in
                    if !editing { client.chooseLiveBitrate(client.liveBitrate?.currentKbps ?? status.startupKbps, final: true) }
                }.frame(height: 24).disabled(!status.supported)
                if !status.supported { Text("This Host does not support live bitrate changes.").font(.caption).foregroundStyle(.secondary) }
            }
            Toggle("Show statistics", isOn: Binding(get: { client.videoDiagnosticsEnabled }, set: { client.setVideoDiagnosticsEnabled($0) }))
            Divider()
            if PlankMacTabletSource.saved != .pencil {
                Text(client.tabletPreflightSummary).font(.caption)
            }
            Text("Tablet: \(PlankMacTabletSource.saved.title)").font(.caption).foregroundStyle(.secondary)
            if PlankMacTabletSource.saved == .pencil {
                PlankPencilRelaySettings(receiver: client.pencilRelay)
            }
        }.padding(20).frame(width: 340)
            .buttonStyle(.borderless)
            .background(PlankMacLocalPointerRegion())
            .onAppear {
                screens = PlankMacDisplayPriority.screens()
                secondDisplayOpen = PlankMacSessionWindows.otherWindow(client: client, excluding: desktopWindow) != nil
                NSCursor.arrow.set()
            }
    }
    private func synchronizeWindows() {
        placeOnAssignedDisplay()
        if outputIndex == 0, client.sessionTopology?.splitPresentation == true {
            openWindow(id: "desktop-secondary")
        } else if outputIndex == 1, client.sessionTopology?.splitPresentation != true,
                  !PlankMacSessionWindows.isClosing(client: client) {
            closeThisWindow()
        }
    }
    private func placeOnAssignedDisplay() {
        guard let topology = client.sessionTopology, topology.splitPresentation else {
            placedGeneration = nil
            return
        }
        guard placedGeneration != topology.generation, let window = desktopWindow,
              !window.styleMask.contains(.fullScreen) else { return }
        guard let screen = PlankMacDisplayPriority.screen(outputIndex: outputIndex, topology: topology,
                  layout: client.sessionLocalDisplayLayout),
              topology.macPresentationOutputs.indices.contains(outputIndex) else { return }
        place(window, on: screen)
        placedGeneration = topology.generation
        NSLog("PLANK Mac placement: role=%@ host=%@ mac=%@ displayID=%u generation=%@",
            outputIndex == 0 ? "primary" : "secondary",
            topology.macPresentationOutputs[outputIndex].id, screen.localizedName,
            PlankMacDisplayPriority.id(screen) ?? 0, topology.generation)
    }
    private func closeThisWindow() {
        let id = outputIndex == 0 ? "desktop" : "desktop-secondary"
        if let presentation = desktopWindow?.delegate as? PlankMacWindowPresentation {
            presentation.closeWhenWindowed { dismissWindow(id: id) }
        } else { dismissWindow(id: id) }
    }
    private func place(_ window: NSWindow, on screen: NSScreen) {
        guard !window.styleMask.contains(.fullScreen) else { return }
        let area = screen.visibleFrame.insetBy(dx: 20, dy: 20)
        var frame = window.frame
        frame.size.width = min(frame.width, area.width)
        frame.size.height = min(frame.height, area.height)
        frame.origin = CGPoint(x: area.midX - frame.width / 2, y: area.midY - frame.height / 2)
        window.setFrame(frame, display: true)
    }
    private func applyAudio() {
        UserDefaults.standard.set(volume, forKey: PlankAudioPreferences.volumeKey)
        UserDefaults.standard.set(muted, forKey: PlankAudioPreferences.mutedKey)
        PlankAudioOutput.shared.setVolume(Float(volume), muted: muted)
    }
}
