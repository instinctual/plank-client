// SPDX-License-Identifier: GPL-3.0-or-later
import Combine
import Foundation
import Network

#if PLANK_PENCIL_RELAY_RECEIVER
/// The current desktop's optional reference-image lane owns its own video sink.
/// Stopping this lane never stops, approves or changes the Pencil control lane.
@MainActor final class PlankPencilImageSender: ObservableObject {
    enum Lifecycle: Sendable { case ready, ended }
    @Published private(set) var status = "Image is off"
    private weak var client: PlankCoreClient?
    private var surfaceID: UUID?
    private var peer: PlankPencilImagePeer?
    private var worker: PlankPencilImageEncoderWorker?
    private var epoch = UUID()

    func start(client: PlankCoreClient, binding: PlankPencilSessionBinding,
               endpoint: NWEndpoint, privateKey: Data,
               lifecycle: @escaping @MainActor @Sendable (Lifecycle) -> Void) throws {
        stop()
        guard case let .service(name, type, domain, interface) = endpoint,
              type == "_plank-pencil._tcp" || type == "_plank-pencil._tcp." else {
            throw PlankPencilWireError.invalid
        }
        let current = UUID(); epoch = current
        let endpoint = NWEndpoint.service(name: name, type: "_plank-pencil-image._tcp",
                                          domain: domain, interface: interface)
        let connection = NWConnection(to: endpoint, using: .tcp)
        let next = try PlankPencilImagePeer(connection: connection, privateKey: privateKey,
                                           binding: binding, initiator: true) { [weak self] event in
            // Frame events cannot occur on this sending side. Only lane state
            // changes cross to the main actor; video callbacks never create Tasks.
            switch event {
            case .ready:
                Task { @MainActor [weak self] in
                    guard let self, self.epoch == current else { return }
                    self.status = "Sending reference image"; self.worker?.setReady()
                    lifecycle(.ready)
                }
            case .frame: break
            case .ended:
                Task { @MainActor [weak self] in
                    guard let self, self.epoch == current else { return }
                    self.fail(); lifecycle(.ended)
                }
            }
        }
        let encoder = PlankPencilImageEncoderWorker(peer: next) { [weak self] state in
            Task { @MainActor [weak self] in
                guard let self, self.epoch == current else { return }
                switch state {
                case .available: self.status = "Sending reference image"
                case .unavailable: self.status = "Reference image unavailable. Pencil sharing is unchanged."
                }
            }
        }
        peer = next; worker = encoder; self.client = client
        let id = UUID(); surfaceID = id
        status = "Connecting reference image…"
        client.registerVideoSurface(id: id, frame: { [weak encoder] frame in encoder?.offer(frame) },
                                    cursor: { _ in }, cursorShape: { _ in })
        next.start()
    }

    func stop() {
        epoch = UUID()
        worker?.invalidate(); worker = nil
        if let surfaceID { client?.unregisterVideoSurface(id: surfaceID) }
        surfaceID = nil; client = nil
        peer?.close(); peer = nil
        status = "Image is off"
    }

    /// Image startup/transport failure retires only this lane. Its authority
    /// owner decides whether current, independently consented media may retry.
    func fail() {
        stop()
        status = "Reference image unavailable. Pencil sharing is unchanged."
    }
}

private enum PlankPencilImageEncoderState: Sendable { case available, unavailable }

/// One synchronous encoder and one replaceable frame. The lock protects ingress
/// and retirement; the utility executor alone owns cadence and byte accounting.
private final class PlankPencilImageEncoderWorker: @unchecked Sendable {
    private struct Pending {
        let frame: PlankRenderedFrame
        let revision: UInt64
        let geometry: UInt64
    }
    private let lock = NSLock()
    private let queue = DispatchQueue(label: "la.instinctual.plank.pencil.image.encode", qos: .utility)
    private let peer: PlankPencilImagePeer
    private let state: @Sendable (PlankPencilImageEncoderState) -> Void
    private let desktopID = UUID()
    private var pending: Pending?
    private var scheduled = false, closed = false, ready = false
    private var revision: UInt64 = 1, geometry: UInt64 = 0
    private var width = 0, height = 0
    private var frameNumber: UInt64 = 0
    private var lastEncodedAt: UInt64 = 0
    private struct Admission { let at: UInt64; let bytes: Int }
    private var admissions: [Admission] = []
    private var reported: PlankPencilImageEncoderState?
    // Bounded, content-free samples. These are local encode durations, never
    // subtracted from a clock on the iPad or written into image records.
    private var diagnosticSample = 0

    init(peer: PlankPencilImagePeer, state: @escaping @Sendable (PlankPencilImageEncoderState) -> Void) {
        self.peer = peer; self.state = state
    }
    func offer(_ frame: PlankRenderedFrame?) {
        lock.lock()
        guard !closed else { lock.unlock(); return }
        guard let frame else {
            pending = nil
            if revision == UInt64.max { closed = true } else { revision += 1 }
            lock.unlock(); return
        }
        if frame.width != width || frame.height != height {
            guard geometry < UInt64.max, revision < UInt64.max else {
                closed = true; pending = nil; lock.unlock(); peer.close(); return
            }
            width = frame.width; height = frame.height; geometry += 1; revision += 1
        }
        pending = Pending(frame: frame, revision: revision, geometry: geometry)
        let wake = ready && !scheduled
        if wake { scheduled = true }
        lock.unlock()
        if wake { queue.async { [weak self] in self?.pump() } }
    }
    func setReady() {
        lock.lock(); guard !closed else { lock.unlock(); return }
        ready = true; let wake = pending != nil && !scheduled
        if wake { scheduled = true }; lock.unlock()
        if wake { queue.async { [weak self] in self?.pump() } }
    }
    func invalidate() {
        lock.lock(); closed = true; pending = nil; ready = false; lock.unlock()
    }
    private func pump() {
        let now = DispatchTime.now().uptimeNanoseconds
        let interval: UInt64 = 100_000_000 // At most ten encodes/offers per second.
        lock.lock()
        guard !closed, ready, pending != nil else { scheduled = false; lock.unlock(); return }
        if lastEncodedAt != 0, now >= lastEncodedAt, now - lastEncodedAt < interval {
            let wait = interval - (now - lastEncodedAt)
            lock.unlock()
            queue.asyncAfter(deadline: .now() + .nanoseconds(Int(wait))) { [weak self] in self?.pump() }
            return
        }
        let item = pending!; pending = nil; lastEncodedAt = now
        lock.unlock()
        do {
            let encodeStarted = DispatchTime.now().uptimeNanoseconds
            let encoded = try PlankPencilReferenceImageCodec.encode(item.frame)
            let completed = DispatchTime.now().uptimeNanoseconds
            if diagnosticSample == 0, completed >= encodeStarted {
                NSLog("PLANK reference image encode: milliseconds=%.2f bytes=%d source=%dx%d output=%dx%d",
                      Double(completed - encodeStarted) / 1_000_000,
                      encoded.jpegData.count, encoded.sourceWidth, encoded.sourceHeight,
                      encoded.encodedWidth, encoded.encodedHeight)
            }
            diagnosticSample = (diagnosticSample + 1) % 30
            lock.lock()
            let valid = !closed && ready && revision == item.revision
            lock.unlock()
            if valid {
                admissions.removeAll { completed >= $0.at && completed - $0.at >= 1_000_000_000 }
                let used = admissions.reduce(0) { $0 + $1.bytes }
                if encoded.jpegData.count <= 2 * 1024 * 1024 - used {
                    guard frameNumber < UInt64.max else { invalidate(); peer.close(); return }
                    let next = frameNumber + 1
                    let descriptor = PlankPencilImageDescriptor(desktopID: desktopID,
                        geometryGeneration: item.geometry, frameNumber: next,
                        sourceWidth: encoded.sourceWidth, sourceHeight: encoded.sourceHeight,
                        encodedWidth: encoded.encodedWidth, encodedHeight: encoded.encodedHeight)
                    // Serialize admission with retirement. A stopped epoch cannot
                    // offer after its caller has revoked the desktop's authority.
                    lock.lock()
                    let admitted = !closed && ready && revision == item.revision && peer.offer(.init(
                        descriptor: descriptor, jpegData: encoded.jpegData, receivedAtUptimeNanoseconds: completed))
                    lock.unlock()
                    if admitted {
                        frameNumber = next; admissions.append(Admission(at: completed, bytes: encoded.jpegData.count))
                        report(.available)
                    }
                }
            }
        } catch { report(.unavailable) }
        lock.lock()
        let again = !closed && ready && pending != nil
        if !again { scheduled = false }
        lock.unlock()
        if again { queue.async { [weak self] in self?.pump() } }
    }
    private func report(_ value: PlankPencilImageEncoderState) {
        guard value != reported else { return }; reported = value; state(value)
    }
}
#endif
