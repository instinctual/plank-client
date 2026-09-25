import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Installer setup and in-app review share content, not permission logic.
// Controls inherit the surrounding style: native setup or Material preferences.
ColumnLayout {
    id: pane
    spacing: 14
    property bool showHeading: true

    Label {
        visible: pane.showHeading
        text: qsTr("PLANK Client")
        font.pixelSize: 22
        font.weight: Font.DemiBold
    }
    Label {
        visible: pane.showHeading
        text: qsTr("Version %1").arg(Qt.application.version)
        font.pixelSize: 11
    }
    Label {
        text: qsTr("Review access for Client features")
        font.pixelSize: 13
        font.weight: Font.DemiBold
    }
    Label {
        text: qsTr("Client permissions")
        font.pixelSize: 13
        font.weight: Font.DemiBold
    }
    Repeater {
        model: macPermissions.rows
        delegate: RowLayout {
            Layout.fillWidth: true
            spacing: 12
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 245
                Layout.maximumWidth: 245
                Layout.minimumWidth: 0
                spacing: 3
                Label {
                    Layout.fillWidth: true
                    text: modelData.name
                    wrapMode: Text.WordWrap
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    text: modelData.detail
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                }
            }
            Label {
                id: status
                objectName: "permissionStatus" + index
                Layout.preferredWidth: 160
                Layout.minimumWidth: 0
                text: (modelData.verified ? "✓ " : "— ") + modelData.status
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                Accessible.name: modelData.status
                Binding {
                    target: status
                    property: "color"
                    when: modelData.verified
                    value: Qt.styleHints.colorScheme === Qt.Dark ? "#30d158" : "#248a3d"
                    restoreMode: Binding.RestoreBindingOrValue
                }
            }
            Item {
                Layout.fillWidth: true
                Layout.minimumWidth: action.implicitWidth
                implicitHeight: action.implicitHeight
                Button {
                    id: action
                    anchors.right: parent.right
                    text: qsTr("Open Settings")
                    enabled: modelData.actionable
                    Accessible.name: modelData.name + ": " + text
                    onClicked: macPermissions.request(index)
                }
            }
        }
    }
    Label {
        Layout.fillWidth: true
        visible: text.length > 0
        text: macPermissions.error
        wrapMode: Text.WordWrap
        font.pixelSize: 12
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 12
        text: qsTr("Allow permissions for PLANK Client, not the Host.") + "\n" +
              qsTr("Permissions do not turn forwarding on.") + "\n" +
              qsTr("Missing optional permissions do not block desktop video.") + "\n" +
              qsTr("Status refreshes when you return from System Settings.")
    }
}
