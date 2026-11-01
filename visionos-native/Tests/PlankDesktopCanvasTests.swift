import CoreGraphics
import Foundation

@main @MainActor
struct PlankDesktopCanvasTests {
    static var checks = 0
    static func check(_ condition: Bool, _ message: String) {
        precondition(condition, message)
        checks += 1
    }

    static func main() throws {
        let suite = "plank.vision.canvas-tests.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = HostStore(defaults: defaults)
        store.add(name: "Workstation", address: "workstation.example", port: 28989,
                  displaySize: .ultraHD, secondDisplaySize: .portrait1280)
        var bookmark = HostStore(defaults: defaults).hosts[0]
        check(bookmark.spatialDisplaySize == .ultraHD && bookmark.secondDisplaySize == .portrait1280,
              "new bookmark retains independent left and right resolutions")
        check(bookmark.desktopDimensions == .init(width: 5120, height: 2160), "3840 + 1280 composite")
        let connected = bookmark
        bookmark.secondDisplaySize = .portrait2560
        check(PlankBookmarkSessionPolicy.requiresClosure(edited: bookmark, connected: connected,
              authenticated: true), "right display edit closes the retained layout")
        store.update(bookmark)
        bookmark = HostStore(defaults: defaults).hosts[0]
        check(bookmark.desktopDimensions == .init(width: 6400, height: 2160), "edited right mode persists")
        bookmark.secondDisplaySize = nil
        check(PlankBookmarkSessionPolicy.requiresClosure(edited: bookmark, connected: connected,
              authenticated: true), "returning to one display closes the dual layout")
        store.update(bookmark)
        check(HostStore(defaults: defaults).hosts[0].secondDisplaySize == nil, "one display clears saved second mode")
        check(bookmark.desktopDimensions == .init(width: 3840, height: 2160), "single display canvas unchanged")
        var old = try JSONSerialization.jsonObject(with: JSONEncoder().encode(connected)) as! [String: Any]
        old.removeValue(forKey: "secondDisplaySize")
        let oldBookmark = try JSONDecoder().decode(HostBookmark.self, from: JSONSerialization.data(withJSONObject: old))
        check(oldBookmark.secondDisplaySize == nil && oldBookmark.desktopDimensions == bookmark.desktopDimensions,
              "existing single-display bookmark remains single")

        check(PlankTopology.canvas(first: .portrait2560, second: .portrait2560) == .init(width: 5120, height: 2160), "two equal portrait displays")
        check(PlankTopology.canvas(first: .ultraHD, second: .ultraHD) == .init(width: 7680, height: 2160), "two UHD displays")
        check(PlankTopology.canvas(first: .fullHD, second: .ultraHD) == .init(width: 5760, height: 2160), "short left output uses maximum height")
        check(PlankTopology.canvas(first: .ultraHD, second: .fullHD) == .init(width: 5760, height: 2160), "short right output uses maximum height")
        check(PlankTopology.validCanvas(first: .dci4K, second: .dci4K), "8192 boundary supported")
        check(!PlankTopology.validCanvas(first: .ultrawide, second: .ultrawide), "10240 width cannot be saved/launched")
        check(!PlankTopology.validCanvas(first: .ultrawide, second: .wide), "first invalid available width rejected")
        check(PlankTopology.validCanvas(first: .ultrawide, second: .portrait2560), "5120 + 2560 remains valid")
        for mode in SpatialDisplaySize.allCases {
            check(PlankTopology.validCanvas(first: mode, second: nil), "every existing single mode remains valid")
        }

        let fixture = try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
        let topology = try PlankTopologyDecoder.decode(fixture)
        let requested = topology.requesting(.ultraHD, second: .portrait1280)
        check(requested.matches(.ultraHD, second: .portrait1280), "unchanged authenticated dual layout verified")
        check(requested.outputs == topology.outputs, "Host source rectangles retained without local reordering")
        check(requested.launchLayoutQuery == [
            .init(name: "plankVirtualMode1", value: "3840x2160"),
            .init(name: "plankVirtualMode2", value: "1280x2160")], "launch sends Display 1 left and Display 2 right")
        check(PlankDesktopPresentation.combinedWindow.launchMode(for: requested) == "scaled-span", "Vision requests one composite presentation")
        check(PlankDesktopPresentation.separateWindows.launchMode(for: requested) == "separate-displays", "native Mac presentation remains separate")
        let single = topology.requesting(.ultraHD)
        check(PlankDesktopPresentation.combinedWindow.launchMode(for: single) == "scaled-span", "single presentation unchanged")
        let negotiation = PlankStreamRequest.negotiation(topology: requested, frameRate: 60, encoderTargetKbps: 50_000)
        let video = negotiation["video"] as! [String: Any]
        check(video["width"] as? Int == 5120 && video["height"] as? Int == 2160, "one stream negotiates full canvas")
        check(video["ten_bit"] as? Bool == true && video["negotiated_format"] as? Int == 0x0800, "dual displays retain selected codec precision")

        let canvas = PlankDesktopCanvas(dimensions: .init(width: 5120, height: 2160))
        let window = CGSize(width: 1280, height: 720)
        check(canvas.rect(in: window) == CGRect(x: 0, y: 90, width: 1280, height: 540), "composite aspect fit does not stretch or crop right output")
        check(canvas.remotePoint(CGPoint(x: 0, y: 90), in: window)! == (0, 0), "left top pixel")
        check(canvas.remotePoint(CGPoint(x: 1280, y: 630), in: window)! == (5119, 2159), "rightmost pixel remains reachable")
        check(canvas.remotePoint(CGPoint(x: 1280, y: 89), in: window) == nil, "top letterbox rejects input")
        check(canvas.remotePoint(CGPoint(x: 1281, y: 90), in: window) == nil, "outside desktop rejects input")
        check(canvas.remotePoint(CGPoint(x: CGFloat.nan, y: 100), in: window) == nil, "invalid coordinate cannot convert to integer")
        check(canvas.rect(in: .zero) == .zero && canvas.remotePoint(.zero, in: .zero) == nil, "unlaid-out view cannot send coordinates")

        // A Host pixel on either side of the seam maps back to itself at every
        // local window size; no per-display scaling or primary-output offset.
        for width in [CGFloat(640), 1280, 1920, 2560] {
            let size = CGSize(width: width, height: 900)
            let rect = canvas.rect(in: size)
            for (x, y) in [(0, 0), (3839, 200), (3840, 200), (5119, 2159)] {
                let point = CGPoint(x: rect.minX + CGFloat(x) / 5119 * rect.width,
                                    y: rect.minY + CGFloat(y) / 2159 * rect.height)
                check(canvas.remotePoint(point, in: size)! == (x, y), "seam and endpoint mapping survives window resizing")
            }
        }
        let unequal = PlankDesktopCanvas(dimensions: .init(width: 5760, height: 2160))
        check(unequal.rect(in: CGSize(width: 1440, height: 540)) == CGRect(x: 0, y: 0, width: 1440, height: 540), "unequal display heights preserve the complete top-aligned canvas")
        let singleCanvas = PlankDesktopCanvas(dimensions: bookmark.desktopDimensions)
        check(singleCanvas.rect(in: window) == CGRect(x: 0, y: 0, width: 1280, height: 720), "single UHD rendering unchanged")
        check(singleCanvas.remotePoint(CGPoint(x: 1280, y: 720), in: window)! == (3839, 2159), "single UHD input unchanged")
        print("Vision desktop canvas: \(checks) checks passed")
    }
}
