import Foundation

enum PlankMacTabletSource: String, CaseIterable, Identifiable, Sendable {
    case off, usb, relay, pencil
    var id: String { rawValue }
    var title: String {
        switch self { case .off: "Off"; case .usb: "Wacom connected to this Mac (USB)"; case .relay: "Registered Relay"; case .pencil: "Apple Pencil shared from iPad" }
    }
    // A running session keeps the source it started with even if preferences
    // are changed elsewhere. No normalized pen may enter a raw-HID session.
    static func allowsPencil(sessionActive: Bool, sessionSource: Self, savedSource: Self) -> Bool {
        (sessionActive ? sessionSource : savedSource) == .pencil
    }
    var usesUSB: Bool { self == .usb }
    var usesRegisteredRelay: Bool { self == .relay }
    static var saved: Self { Self(rawValue: UserDefaults.standard.string(forKey: "plank.mac.tablet-source") ?? "usb") ?? .usb }
}
