import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// Installer handoff: no ComputerManager, bookmark view or stream launcher.
ApplicationWindow {
    id: window
    title: qsTr("PLANK Client setup")
    width: Math.min(720, screen.desktopAvailableWidth)
    height: Math.min(permissions.implicitHeight + 40, screen.desktopAvailableHeight)
    visible: true
    color: theme.canvas

    PlankTheme { id: theme }
    Material.theme: Material.Dark
    Material.background: theme.canvas
    Material.foreground: theme.textPrimary
    Material.accent: theme.accent

    MacPermissionsDialog {
        id: permissions
        objectName: "permissions"
        Overlay.modal: Rectangle { color: "transparent" }
        Component.onCompleted: open()
        onClosed: window.close()
    }
}
