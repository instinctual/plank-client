import AppKit
import SwiftUI

@MainActor final class PlankCoreClient {
    var pauses = Set<UUID>()
    func setPencilLocalControls(_ presented: Bool,owner: UUID) {
        if presented { pauses.insert(owner) } else { pauses.remove(owner) }
    }
}

/// Real AppKit visibility/close and retained SwiftUI content; no Host or device.
@main @MainActor struct PlankMacPencilSettingsTests {
    static var checks = 0
    static func check(_ value: Bool,_ message: String) { checks += 1; precondition(value,message) }
    static func settle() { RunLoop.main.run(until:Date().addingTimeInterval(0.15)) }
    static func main() {
        NSApplication.shared.setActivationPolicy(.prohibited)
        let client = PlankCoreClient()
        func make() -> NSWindow {
            let window = NSWindow(contentRect:NSRect(x:-10000,y:-10000,width:300,height:100),
                styleMask:[.titled,.closable],backing:.buffered,defer:false)
            window.isReleasedWhenClosed = false
            window.contentView = NSHostingView(rootView:Color.clear.background(PlankMacPencilSettingsPause(client:client)))
            window.orderFront(nil); settle()
            return window
        }
        let window = make()
        check(client.pauses.count == 1,"Visible Settings pauses Pencil")
        let retainedContent = window.contentView
        window.close(); settle()
        check(!window.isVisible && window.contentView === retainedContent,"Settings content remains retained after actual close")
        check(client.pauses.isEmpty,"Closed Settings cannot keep a stale Pencil pause")
        window.orderFront(nil)
        // The background test cannot become key; model the real reopen signal.
        NotificationCenter.default.post(name:NSWindow.didBecomeKeyNotification,object:window); settle()
        check(client.pauses.count == 1,"Reopening the retained Settings restores its pause")
        let second = make()
        check(client.pauses.count == 2,"Separate Settings owners remain independent")
        window.close(); settle()
        check(client.pauses.count == 1,"Closing one window cannot release another owner's pause")
        second.contentView = nil; settle()
        check(client.pauses.isEmpty,"Removing Settings content releases its owner")
        second.close()
        window.orderFront(nil)
        NotificationCenter.default.post(name:NSWindow.didBecomeKeyNotification,object:window); settle()
        check(client.pauses.count == 1,"Repeated reopen uses one owner")
        window.orderOut(nil); settle()
        check(client.pauses.isEmpty,"A hidden Settings window cannot retain a pause")
        window.close(); settle()
        check(client.pauses.isEmpty,"Repeated close cannot restore a retired pause")
        print("PASS: \(checks) native retained-Settings visibility/close/reopen/detachment/owner checks; reopen key notification modeled, no active-app/Host/device changes")
    }
}
