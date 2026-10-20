import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: surface
    width: 1280
    height: 720

    // Qualification background only. The production presenter will draw under
    // this scene directly; this rectangle is not a CPU video upload path.
    Rectangle {
        visible: !sessionWindow.liveSession
        x: sessionWindow.videoRect.x
        y: sessionWindow.videoRect.y
        width: sessionWindow.videoRect.width
        height: sessionWindow.videoRect.height
        color: "#22272e"
        border.color: "#637083"
        Text {
            anchors.centerIn: parent
            text: "Qt session surface — no remote connection"
            color: "#ccd3dd"
            font.pixelSize: 16
        }
    }

    Text {
        visible: sessionWindow.statusText.length > 0 && !sessionWindow.waiting
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 20
        text: sessionWindow.statusText
        color: "#f1f2f5"
        style: Text.Outline
        styleColor: "#111111"
        font.pixelSize: 20
    }

    Text {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 12
        width: parent.width - 24
        text: sessionWindow.statisticsText
        visible: sessionWindow.statisticsVisible && !sessionWindow.waiting
        color: "#ffff90"
        font.family: "monospace"
        font.pixelSize: 16
        wrapMode: Text.Wrap
        style: Text.Outline
        styleColor: "#000000"
    }

    component ToolButton: Button {
        implicitWidth: 24
        implicitHeight: 24
        focusPolicy: Qt.NoFocus
        padding: 0
        background: Rectangle {
            radius: 4
            color: parent.down ? "#525862" : parent.hovered ? "#444a53" : "#343941"
        }
        contentItem: Text {
            text: parent.text
            font.pixelSize: 15
            color: "#f0f1f4"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        ToolTip.visible: hovered
    }

    // Fixed vector resources avoid font substitutions and Canvas repaint state
    // when the toolbar is hidden, shown or moved between display scales.
    component ToolbarIcon: Item {
        implicitWidth: 24
        implicitHeight: 24
        property alias source: iconImage.source
        readonly property int status: iconImage.status
        Image {
            id: iconImage
            anchors.fill: parent
            fillMode: Image.PreserveAspectFit
            sourceSize.width: Math.round(width * Screen.devicePixelRatio)
            sourceSize.height: Math.round(height * Screen.devicePixelRatio)
        }
    }

    component StatReadout: Item {
        id: stat
        required property string label
        required property string value
        required property string widthSample
        property color valueColor: "#f6f8fa"
        implicitWidth: Math.max(labelText.implicitWidth, valueText.implicitWidth, Math.ceil(valueMetrics.advanceWidth))
        implicitHeight: labelText.implicitHeight + valueText.implicitHeight
        Text {
            id: labelText
            objectName: "statLabel"
            text: stat.label
            color: "#97a1ae"
            font.pixelSize: 11
        }
        Text {
            id: valueText
            objectName: "statValue"
            y: labelText.height
            text: stat.value
            color: stat.valueColor
            font.pixelSize: 13
        }
        TextMetrics {
            id: valueMetrics
            font: valueText.font
            text: stat.widthSample
        }
    }

    Rectangle {
        id: toolbar
        z: 2
        objectName: "sessionToolbar"
        visible: sessionWindow.toolbarVisible && !sessionWindow.waiting
        x: Math.round(Math.max(0, surface.width - width) * sessionWindow.toolbarPosition)
        y: 0
        width: row.implicitWidth + 16
        height: 39
        color: "#2b3038"
        radius: 5

        RowLayout {
            id: row
            anchors.centerIn: parent
            spacing: 8
            Item {
                objectName: "toolbarHandle"
                width: 18
                height: 32
                Text { anchors.centerIn: parent; text: "⠿"; color: "#c3c8d0"; font.pixelSize: 20 }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                    property real grabOffset: 0
                    onPressed: mouse => {
                        grabOffset = mapToItem(surface, mouse.x, mouse.y).x - toolbar.x
                    }
                    onPositionChanged: mouse => {
                        if (pressed)
                            sessionWindow.moveToolbar(mapToItem(surface, mouse.x, mouse.y).x - grabOffset)
                    }
                }
            }
            ToolButton {
                objectName: "pinButton"
                ToolTip.text: sessionWindow.pinned ? "Unpin toolbar" : "Pin toolbar"
                Accessible.name: ToolTip.text
                onClicked: sessionWindow.pinned = !sessionWindow.pinned
                contentItem: ToolbarIcon {
                    objectName: "pinIcon"
                    source: sessionWindow.pinned ? "icons/pin.svg" : "icons/pin-off.svg"
                }
            }
            StatReadout {
                objectName: "fpsStat"
                label: "FPS"
                value: (sessionWindow.telemetry.fps || 0).toFixed(1)
                widthSample: "999.9"
            }
            StatReadout {
                objectName: "videoStat"
                label: "Video Mbps"
                value: (sessionWindow.telemetry.videoMbps || 0).toFixed(1)
                widthSample: "999.9"
            }
            StatReadout {
                objectName: "lossStat"
                property real loss: sessionWindow.telemetry.loss === undefined ? -1 : sessionWindow.telemetry.loss
                label: "Loss"
                value: (loss < 0 ? "—" : loss.toFixed(3)) + "%"
                widthSample: "100.000%"
                valueColor: {
                    if (loss < 0) return "#8aa6c9"
                    const blue = [52, 132, 228], green = [52, 199, 110], red = [239, 88, 88]
                    const a = loss <= 5 ? blue : green, b = loss <= 5 ? green : red
                    const t = Math.min(1, Math.max(0, loss <= 5 ? loss / 5 : (loss - 5) / 5))
                    return Qt.rgba((a[0] + (b[0] - a[0]) * t) / 255, (a[1] + (b[1] - a[1]) * t) / 255,
                                   (a[2] + (b[2] - a[2]) * t) / 255, 1)
                }
            }
            StatReadout {
                objectName: "rttStat"
                label: "RTT"
                value: (sessionWindow.telemetry.rtt || "—") + " ms"
                widthSample: "9999.9 ms"
            }
            ColumnLayout {
                spacing: 0
                TextMetrics {
                    id: targetWidthMetrics
                    font: targetText.font
                    text: "Encoder Target " + (sessionWindow.maximumBitrateKbps / 1000).toFixed(1) + " Mbps"
                }
                Text {
                    id: targetText
                    Layout.preferredWidth: Math.max(implicitWidth, Math.ceil(targetWidthMetrics.advanceWidth))
                    text: "Encoder Target " + (sessionWindow.bitrateKbps / 1000).toFixed(1) + " Mbps"
                    color: sessionWindow.liveSession && !sessionWindow.telemetry.bitrateConfirmed ? "#edc76b" : "#eff1f4"
                    font.pixelSize: 13
                }
                Slider {
                    objectName: "bitrateSlider"
                    Layout.fillWidth: true
                    Layout.preferredWidth: Math.ceil(targetWidthMetrics.advanceWidth)
                    Layout.preferredHeight: 16
                    from: sessionWindow.minimumBitrateKbps
                    to: sessionWindow.maximumBitrateKbps
                    stepSize: sessionWindow.bitrateStepKbps
                    value: sessionWindow.bitrateKbps
                    focusPolicy: Qt.NoFocus
                    enabled: !sessionWindow.liveSession || !!sessionWindow.telemetry.bitrateEnabled
                    onMoved: sessionWindow.bitrateKbps = Math.round(value)
                }
            }
            ToolButton {
                visible: !!sessionWindow.telemetry.microphoneSupported
                text: sessionWindow.telemetry.microphoneState === 2 ? "●" : "○"
                ToolTip.text: ["Microphone off", "Starting microphone", "Microphone on", "Microphone unavailable"][sessionWindow.telemetry.microphoneState || 0]
                onClicked: sessionWindow.toggleMicrophone()
                // Compact microphone glyph; colour indicates actual Host acknowledgement.
                contentItem: Item {
                    Rectangle { x: 9; y: 3; width: 6; height: 11; radius: 3; color: sessionWindow.telemetry.microphoneState === 2 ? "#61d991" : "#c7cbd2" }
                    Rectangle { x: 6; y: 9; width: 12; height: 9; radius: 5; color: "transparent"; border.color: "#c7cbd2" }
                    Rectangle { x: 11; y: 17; width: 2; height: 4; color: "#c7cbd2" }
                }
            }
            ToolButton {
                visible: !!sessionWindow.telemetry.cameraSupported
                text: "▣"
                ToolTip.text: ["Camera off", "Starting camera", "Camera on", "Camera unavailable"][sessionWindow.telemetry.cameraState || 0]
                onClicked: sessionWindow.toggleCamera()
            }
            ToolButton {
                objectName: "fullscreenButton"
                ToolTip.text: sessionWindow.fullscreen ? "Restore window" : "Enter fullscreen"
                Accessible.name: ToolTip.text
                contentItem: ToolbarIcon {
                    objectName: "fullscreenIcon"
                    source: sessionWindow.fullscreen ? "icons/fullscreen-exit.svg" : "icons/fullscreen-enter.svg"
                }
                onClicked: sessionWindow.toggleFullscreen()
            }
            ToolButton {
                text: "−"
                ToolTip.text: "Minimize"
                onClicked: sessionWindow.minimize()
            }
            ToolButton {
                objectName: "disconnectButton"
                text: "×"
                ToolTip.text: "Disconnect"
                onClicked: sessionWindow.disconnectSession()
            }
        }
    }

    Rectangle {
        objectName: "recoveryOverlay"
        z: 3
        visible: sessionWindow.waiting
        anchors.fill: parent
        color: "#55000000"
        MouseArea { anchors.fill: parent }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(440, surface.width - 24)
            height: 160
            radius: 8
            color: "#303640"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                Text { text: "Waiting for workstation"; font.pixelSize: 19; color: "white" }
                Text { text: "Reconnecting automatically…"; font.pixelSize: 14; color: "#e1e4ea" }
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    Button {
                        objectName: "recoveryDisconnectButton"
                        text: "Disconnect"
                        onClicked: sessionWindow.disconnectSession()
                    }
                }
            }
        }
    }
}
