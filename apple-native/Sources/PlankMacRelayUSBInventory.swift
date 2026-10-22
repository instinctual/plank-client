import Foundation
import IOKit

// Presence only. Registry reads never open a device, request permission, take
// the capture lease, or register input callbacks. Raw capture remains gated by
// the approved drawing connection's SESSION_READY.
struct MacRelayUSBTablet: Sendable {
    let group: UInt64
    let name: String
    let serial: String?

    static func connected() -> [MacRelayUSBTablet] {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOHIDDevice"), &iterator) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterator) }
        var tablets: [MacRelayUSBTablet] = []
        while true {
            let entry = IOIteratorNext(iterator)
            guard entry != 0 else { break }
            defer { IOObjectRelease(entry) }
            guard (property(entry, "VendorID") as? NSNumber)?.intValue == 0x056a,
                  property(entry, "Transport") as? String == "USB",
                  let group = usbParent(entry) else { continue }
            tablets.append(.init(group: group, name: property(entry, "Product") as? String ?? "Wacom USB tablet",
                                 serial: property(entry, "SerialNumber") as? String))
        }
        return grouped(tablets)
    }
    // Match the raw worker's ordering by physical USB parent, collapsing its
    // multiple HID interfaces into one tablet. Inventory does not prove capture.
    static func grouped(_ tablets: [MacRelayUSBTablet]) -> [MacRelayUSBTablet] {
        var unique: [UInt64: MacRelayUSBTablet] = [:]
        for tablet in tablets where tablet.group != 0 {
            if unique[tablet.group] == nil { unique[tablet.group] = tablet }
        }
        return unique.keys.sorted().prefix(8).compactMap { unique[$0] }
    }
    static func records(_ tablets: [MacRelayUSBTablet]) -> [[String: Any]] {
        grouped(tablets).enumerated().map { index, tablet in
            var record: [String: Any] = ["id": "usb:\(String(tablet.group, radix: 16))", "name": bounded(tablet.name),
                                        "port": "USB \(String(tablet.group, radix: 16))", "active": index == 0]
            if let serial = tablet.serial, !serial.isEmpty { record["serial"] = bounded(serial) }
            return record
        }
    }
    private static func bounded(_ text: String) -> String {
        var value = String(text.prefix(64))
        while value.utf8.count > 64 { value.removeLast() }
        return value
    }
    private static func property(_ entry: io_registry_entry_t, _ name: String) -> CFTypeRef? {
        IORegistryEntryCreateCFProperty(entry, name as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
    }
    private static func usbParent(_ device: io_registry_entry_t) -> UInt64? {
        IOObjectRetain(device)
        var entry = device
        for _ in 0..<16 {
            if IOObjectConformsTo(entry, "IOUSBHostDevice") != 0 {
                var id: UInt64 = 0
                let result = IORegistryEntryGetRegistryEntryID(entry, &id)
                IOObjectRelease(entry)
                return result == KERN_SUCCESS && id != 0 ? id : nil
            }
            var parent: io_registry_entry_t = 0
            let result = IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent)
            IOObjectRelease(entry)
            guard result == KERN_SUCCESS, parent != 0 else { return nil }
            entry = parent
        }
        IOObjectRelease(entry)
        return nil
    }
}
