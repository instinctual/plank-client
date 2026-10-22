import Combine
import Foundation

@MainActor
final class HostStore: ObservableObject {
    @Published private(set) var hosts: [HostBookmark] = []

    private let defaults: UserDefaults
    private let storageKey = "plank.vision.hosts.v1"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        load()
    }

    func add(name: String, address: String, port: UInt16,
             displaySize: SpatialDisplaySize = .standard,
             frameRate: Int = StreamFrameRate.defaultValue,
             videoBitrateKbps: Int = StreamBitrate.defaultKbps,
             secondDisplaySize: SpatialDisplaySize? = nil) {
        let host = HostBookmark(name: name, address: address, port: port,
                                spatialDisplaySize: displaySize,
                                streamFrameRate: frameRate,
                                videoBitrateKbps: videoBitrateKbps, secondDisplaySize: secondDisplaySize)
        hosts.append(host)
        sortAndSave()
    }

    func add(discoveredHost: DiscoveredHost) {
        guard !hosts.contains(where: { $0.name == discoveredHost.serviceName }) else { return }
        hosts.append(
            HostBookmark(
                name: discoveredHost.serviceName,
                address: discoveredHost.endpointDescription
            )
        )
        sortAndSave()
    }

    func update(_ host: HostBookmark) {
        guard let index = hosts.firstIndex(where: { $0.id == host.id }) else { return }
        hosts[index] = host
        sortAndSave()
    }

    func remove(_ host: HostBookmark) {
        hosts.removeAll { $0.id == host.id }
        save()
    }

    func markConnected(_ host: HostBookmark) {
        guard let index = hosts.firstIndex(where: { $0.id == host.id }) else { return }
        hosts[index].lastConnectedAt = .now
        save()
    }

    private func load() {
        guard let data = defaults.data(forKey: storageKey),
              let decoded = try? JSONDecoder().decode([HostBookmark].self, from: data) else {
            return
        }
        var migrated = false
        hosts = decoded.map { host in
            guard host.port == 47989 else { return host }
            var corrected = host
            corrected.port = 28989
            migrated = true
            return corrected
        }
        if migrated {
            save()
        }
    }

    private func sortAndSave() {
        hosts.sort { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
        save()
    }

    private func save() {
        guard let data = try? JSONEncoder().encode(hosts) else { return }
        defaults.set(data, forKey: storageKey)
    }
}
