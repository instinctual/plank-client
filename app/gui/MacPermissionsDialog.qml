import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(680, parent.width - 40)
    title: qsTr("PLANK Client permissions")
    modal: true
    focus: true
    padding: 20
    standardButtons: Dialog.Close
    closePolicy: Popup.CloseOnEscape

    PlankTheme { id: theme }

    background: Rectangle {
        color: theme.surfaceRaised
        radius: theme.radiusLarge
        border.width: 1
        border.color: theme.border
    }
    footer: DialogButtonBox {
        background: Rectangle { color: theme.surfaceRaised }
    }

    onOpened: macPermissions.refresh()
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (dialog.opened && Qt.application.state === Qt.ApplicationActive)
                macPermissions.refresh()
        }
    }

    contentItem: ColumnLayout {
        spacing: 18

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Permissions enable features; they do not turn forwarding on.") + "\n" +
                  qsTr("Missing optional permissions do not block desktop video.")
        }

        Repeater {
            model: macPermissions.rows
            delegate: RowLayout {
                Layout.fillWidth: true
                spacing: 16

                Label {
                    text: modelData.verified ? "✓" : "—"
                    color: modelData.verified ? theme.success : theme.textSecondary
                    font.pixelSize: 22
                    Layout.preferredWidth: 24
                    Accessible.ignored: true
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 4
                    Label {
                        Layout.fillWidth: true
                        text: modelData.name
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData.detail
                        wrapMode: Text.WordWrap
                        color: theme.textSecondary
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData.status
                        wrapMode: Text.WordWrap
                        color: modelData.verified ? theme.success : theme.textSecondary
                    }
                }
                Button {
                    text: qsTr("Open Settings")
                    enabled: modelData.actionable
                    Accessible.name: modelData.name + ": " + text
                    onClicked: macPermissions.request(index)
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: macPermissions.error
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Status refreshes when you return from System Settings.")
                color: theme.textSecondary
            }
            Button {
                text: qsTr("Refresh")
                onClicked: macPermissions.refresh()
            }
        }
    }
}
