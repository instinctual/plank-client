import QtQuick
import QtQuick.Controls.macOS
import QtQuick.Layouts

// Installer-only process uses native macOS controls, with no bookmark or stream.
ApplicationWindow {
    id: window
    title: qsTr("PLANK Client setup")
    width: Math.min(650, screen.desktopAvailableWidth)
    height: Math.min(permissions.implicitHeight + 44 + actions.implicitHeight, screen.desktopAvailableHeight)
    visible: true
    color: palette.window

    Component.onCompleted: macPermissions.refresh()
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state === Qt.ApplicationActive)
                macPermissions.refresh()
        }
    }

    ScrollView {
        id: scroll
        anchors.fill: parent
        leftPadding: 24
        rightPadding: 24
        topPadding: 22
        bottomPadding: 22
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        MacPermissionsPane {
            id: permissions
            objectName: "permissions"
            width: scroll.availableWidth
        }
    }

    footer: Item {
        id: actions
        implicitHeight: buttons.implicitHeight + 22
        RowLayout {
            id: buttons
            anchors.right: parent.right
            anchors.rightMargin: 24
            anchors.top: parent.top
            spacing: 12
            Button {
                objectName: "refreshButton"
                text: qsTr("Refresh")
                onClicked: macPermissions.refresh()
            }
            Button {
                objectName: "closeButton"
                text: qsTr("Close")
                highlighted: true
                onClicked: window.close()
            }
        }
    }
    Shortcut { sequence: "Escape"; onActivated: window.close() }
}
