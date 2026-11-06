import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import InternetConnectivity 1.0

NavigableToolButton {
    id: control
    objectName: "internetStatus"
    required property var monitor
    readonly property var launcherWindow: control.Window.window
    readonly property bool checksVisible: !!launcherWindow && launcherWindow.visible &&
        launcherWindow.visibility !== Window.Minimized &&
        Qt.application.state !== Qt.ApplicationHidden && Qt.application.state !== Qt.ApplicationSuspended

    // Only the main-window indicator enables Internet probing. Focus in a
    // streaming window does not make this launcher visible again.
    Binding {
        target: control.monitor
        property: "windowVisible"
        value: control.checksVisible
        restoreMode: Binding.RestoreBindingOrValue
    }
    Binding {
        target: control.monitor
        property: "summaryVisible"
        value: pingDialog.visible
        restoreMode: Binding.RestoreBindingOrValue
    }
    function colorForState(state) {
        return state === InternetConnectivity.Reachable ? theme.success :
               state === InternetConnectivity.NoResponse ? theme.danger :
               state === InternetConnectivity.Unavailable ? theme.warning : theme.textSecondary
    }
    readonly property color statusColor: colorForState(monitor.state)
    visible: monitor.state !== InternetConnectivity.Disabled
    hoverEnabled: true
    padding: 6
    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    implicitHeight: implicitContentHeight + topPadding + bottomPadding
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("%1. Show ping results").arg(monitor.statusText)
    Accessible.description: monitor.detail

    PlankTheme { id: theme }
    contentItem: RowLayout {
        spacing: 6
        Rectangle {
            implicitWidth: 9; implicitHeight: 9; radius: 4.5
            color: control.statusColor
        }
        Label {
            objectName: "internetStatusLabel"
            text: control.monitor.statusText
            textFormat: Text.PlainText
            color: control.statusColor
            font.pointSize: 12
            font.weight: Font.DemiBold
        }
    }
    ToolTip.visible: hovered
    ToolTip.delay: 500
    ToolTip.text: monitor.detail + "\n" + qsTr("Click to view live ping results.")
    onClicked: pingDialog.open()

    NavigableDialog {
        id: pingDialog
        objectName: "internetSummaryDialog"
        title: qsTr("Internet Connectivity")
        modal: true
        dim: false
        focus: true
        width: Math.max(0, Math.min(600, parent.width - 32))
        height: Math.max(0, Math.min(implicitHeight, parent.height - 32))
        closePolicy: Popup.CloseOnEscape

        onClosed: {
            control.forceActiveFocus()
        }

        contentItem: ColumnLayout {
            spacing: theme.spaceMedium

            RowLayout {
                Layout.fillWidth: true
                spacing: theme.spaceMedium
                Label {
                    text: qsTr("Address")
                    font.weight: Font.DemiBold
                    color: theme.textPrimary
                    Layout.fillWidth: true
                }
                Label {
                    text: qsTr("Status")
                    font.weight: Font.DemiBold
                    color: theme.textPrimary
                    Layout.preferredWidth: 140
                }
                Label {
                    text: qsTr("Round-trip time")
                    font.weight: Font.DemiBold
                    color: theme.textPrimary
                    horizontalAlignment: Text.AlignRight
                    Layout.preferredWidth: 120
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: theme.border
            }

            ScrollView {
                id: resultsScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: resultsColumn.implicitHeight
                Layout.minimumHeight: 0
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    id: resultsColumn
                    width: resultsScroll.availableWidth
                    spacing: 0

                    Repeater {
                        model: control.monitor.summaryResults
                        RowLayout {
                            required property var modelData
                            required property int index
                            objectName: "internetSummaryRow" + index
                            Layout.fillWidth: true
                            Layout.preferredHeight: 34
                            spacing: theme.spaceMedium
                            Label {
                                objectName: "internetSummaryAddress" + parent.index
                                text: parent.modelData.address
                                textFormat: Text.PlainText
                                color: theme.textPrimary
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                elide: Text.ElideRight
                            }
                            Label {
                                objectName: "internetSummaryStatus" + parent.index
                                text: parent.modelData.status
                                textFormat: Text.PlainText
                                color: control.colorForState(parent.modelData.state)
                                Layout.preferredWidth: 140
                            }
                            Label {
                                objectName: "internetSummaryRtt" + parent.index
                                text: parent.modelData.roundTripMs < 0 ? "—" :
                                      parent.modelData.roundTripMs < 1 ? qsTr("<1 ms") :
                                      qsTr("%1 ms").arg(Math.round(parent.modelData.roundTripMs))
                                textFormat: Text.PlainText
                                color: theme.textPrimary
                                horizontalAlignment: Text.AlignRight
                                Layout.preferredWidth: 120
                            }
                        }
                    }
                }
            }

            Label {
                objectName: "internetSummaryCheckedAt"
                text: control.monitor.summaryError ? control.monitor.summaryError :
                      control.monitor.summaryCheckedAt ? qsTr("Live · Last updated: %1").arg(control.monitor.summaryCheckedAt) :
                      qsTr("Live · Checking…")
                textFormat: Text.PlainText
                color: control.monitor.summaryError ? theme.warning : theme.textSecondary
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

        }

        footer: DialogButtonBox {
            alignment: Qt.AlignRight
            background: Rectangle { color: theme.surfaceRaised }
            Button {
                objectName: "internetSummaryClose"
                text: qsTr("Close")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                onClicked: pingDialog.close()
            }
        }
    }
}
