import QtQuick
import QtQuick.Controls

Dialog {
    id: dialog
    objectName: "permissionsDialog"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(680, parent.width - 40)
    title: qsTr("PLANK Client permissions")
    modal: true
    focus: true
    padding: 24
    closePolicy: Popup.CloseOnEscape

    PlankTheme { id: theme }
    background: Rectangle {
        color: theme.surfaceRaised
        radius: theme.radiusLarge
        border.width: 1
        border.color: theme.border
    }
    contentItem: MacPermissionsPane { objectName: "permissions"; showHeading: false }
    footer: DialogButtonBox {
        background: Rectangle { color: theme.surfaceRaised }
        Button {
            objectName: "refreshButton"
            text: qsTr("Refresh")
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: macPermissions.refresh()
        }
        Button {
            objectName: "closeButton"
            text: qsTr("Close")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
        onRejected: dialog.close()
    }

    onOpened: macPermissions.refresh()
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (dialog.opened && Qt.application.state === Qt.ApplicationActive)
                macPermissions.refresh()
        }
    }
}
