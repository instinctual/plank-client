import SwiftUI

struct HostDetailView: View {
    let host: HostBookmark
    @ObservedObject var store: HostStore
    @ObservedObject var client: PlankCoreClient
    let onEdit: (HostBookmark) -> Void
    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismissWindow) private var dismissWindow
    @State private var username = ""
    @State private var password = ""

    var body: some View {
        connectionPanel
        .padding(20)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .navigationTitle(host.name)
    }

    private var connectionPanel: some View {
        VStack(spacing: 28) {
            Image(systemName: "display.2")
                .font(.system(size: 64, weight: .light))
                .symbolRenderingMode(.hierarchical)
                .foregroundStyle(.cyan)

            VStack(spacing: 8) {
                Text(host.name)
                    .font(.largeTitle.weight(.semibold))
                Text("\(host.address):\(host.port)")
                    .font(.title3.monospaced())
                    .foregroundStyle(.secondary)
                VStack(spacing: 4) {
                    if let second = host.secondDisplaySize {
                        Text("Display 1 — Left: \(host.spatialDisplaySize.title)")
                        Text("Display 2 — Right: \(second.title)")
                        Text("One window · \(host.desktopDimensions.width) × \(host.desktopDimensions.height) · \(host.streamFrameRate) fps")
                    } else {
                        Text("Virtual display: \(host.spatialDisplaySize.title) · \(host.streamFrameRate) fps")
                    }
                }
                .font(.footnote)
                .foregroundStyle(.secondary)
            }

            status

            Button {
                onEdit(host)
            } label: {
                Label("Edit Workstation", systemImage: "pencil")
            }
            .buttonStyle(.bordered)
            if isAnySessionActive {
                Text("Bookmark changes take effect on the next connection.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }

            if activeSessionElsewhere {
                Label("Disconnect the current desktop before starting another session.",
                      systemImage: "display.trianglebadge.exclamationmark")
                    .foregroundStyle(.secondary)
            }

            if client.activeHostID == host.id, !client.isClosingSession,
               case .authenticated = client.phase {
                Button {
                    openWindow(id: "plank-desktop")
                    client.startSession(displaySize: host.spatialDisplaySize,
                                        frameRate: host.streamFrameRate,
                                        videoBitrateKbps: host.videoBitrateKbps,
                                        secondDisplaySize: host.secondDisplaySize)
                } label: {
                    Label("Start Session", systemImage: "play.rectangle.fill")
                        .frame(minWidth: 160)
                }
                .buttonStyle(.borderedProminent)
            }

            if isSessionActive {
                HStack(spacing: 16) {
                    Button {
                        openWindow(id: "plank-desktop")
                    } label: {
                        Label("Open Session", systemImage: "macwindow.on.rectangle")
                    }
                    .buttonStyle(.borderedProminent)

                    Button(role: .destructive) {
                        client.disconnectSession()
                        dismissWindow(id: "plank-desktop")
                    } label: {
                        Label("Disconnect", systemImage: "xmark.circle.fill")
                    }
                }
            }

            if client.activeHostID == host.id, case .needsCredentials = client.phase {
                VStack(spacing: 14) {
                    TextField("Username", text: $username)
                        .textContentType(.username)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    SecureField("Password", text: $password)
                        .textContentType(.password)

                    Button {
                        let suppliedPassword = password
                        password = ""
                        Task {
                            await client.authenticate(
                                username: username,
                                password: suppliedPassword
                            )
                            if case .authenticated = client.phase {
                                store.markConnected(host)
                            }
                        }
                    } label: {
                        Label("Sign In", systemImage: "person.badge.key.fill")
                            .frame(minWidth: 140)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(username.isEmpty || password.isEmpty)
                }
                .frame(maxWidth: 420)
            } else {
                HStack(spacing: 16) {
                    if !activeSessionElsewhere {
                        switch client.phase {
                        case .idle:
                            connectButton("Connect", systemImage: "play.fill")
                        case .failed:
                            if client.activeHostID == host.id && client.canRetrySession {
                                Button {
                                    openWindow(id: "plank-desktop")
                                    client.retrySession(displaySize: host.spatialDisplaySize,
                                                        frameRate: host.streamFrameRate,
                                                        videoBitrateKbps: host.videoBitrateKbps,
                                                        secondDisplaySize: host.secondDisplaySize)
                                } label: {
                                    Label("Retry Session", systemImage: "arrow.clockwise")
                                        .frame(minWidth: 120)
                                }
                                .buttonStyle(.borderedProminent)
                            } else {
                                connectButton("Try Again", systemImage: "arrow.clockwise")
                            }
                        case .authenticated, .needsCredentials:
                            if client.activeHostID != host.id {
                                connectButton("Connect", systemImage: "play.fill")
                            }
                        default:
                            EmptyView()
                        }
                    }
                }
            }
        }
    }

    private var isSessionActive: Bool {
        guard client.activeHostID == host.id else { return false }
        switch client.phase {
        case .startingSession, .frameReceived, .streaming:
            return true
        default:
            return false
        }
    }

    private var activeSessionElsewhere: Bool {
        client.activeHostID != host.id && isAnySessionActive
    }

    private var isAnySessionActive: Bool {
        if client.isClosingSession { return true }
        switch client.phase {
        case .startingSession, .frameReceived, .streaming:
            return true
        default:
            return false
        }
    }

    @ViewBuilder
    private var status: some View {
        if client.isClosingSession && client.activeHostID == host.id {
            ProgressView("Closing previous desktop session…")
        } else if activeSessionElsewhere {
            Text("Another workstation is active")
                .foregroundStyle(.secondary)
        } else if client.activeHostID != host.id {
            Text("Ready")
                .foregroundStyle(.secondary)
        } else {
        switch client.phase {
        case .idle:
            Text("Ready")
                .foregroundStyle(.secondary)
        case .probing:
            ProgressView("Contacting workstation…")
        case let .needsCredentials(identity):
            Label("Connected securely to \(identity.name)", systemImage: "lock.shield.fill")
                .foregroundStyle(.green)
        case let .authenticating(identity):
            ProgressView("Signing in to \(identity.name)…")
        case let .authenticated(identity, authentication):
            VStack(spacing: 8) {
                Label("Signed in to \(identity.name)", systemImage: "checkmark.seal.fill")
                    .foregroundStyle(.green)
                Text(authentication.desktopStage == "greeter" ?
                     "The Linux desktop is unlocking. Streaming session startup is next." :
                     "The Host accepted this client. Streaming session startup is next.")
                    .foregroundStyle(.secondary)
            }
        case let .startingSession(identity, _):
            ProgressView("Starting secure stream from \(identity.name)…")
        case let .frameReceived(_, _, probe):
            VStack(spacing: 8) {
                Label("Live video reached Vision Pro", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(.green)
                Text(probe.negotiationSummary)
                    .foregroundStyle(.secondary)
            }
        case let .streaming(identity, _, _):
            Label("Live video from \(identity.name)", systemImage: "dot.radiowaves.left.and.right")
                .foregroundStyle(.green)
        case let .failed(message):
            Label(message, systemImage: "exclamationmark.triangle")
                .foregroundStyle(.orange)
        }
        }
    }

    private func connectButton(_ title: String, systemImage: String) -> some View {
        Button {
            Task { await client.connect(to: host) }
        } label: {
            Label(title, systemImage: systemImage)
                .frame(minWidth: 120)
        }
        .buttonStyle(.borderedProminent)
    }

}
