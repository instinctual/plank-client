import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

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

    Rectangle {
        id: toolbar
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
                onClicked: sessionWindow.pinned = !sessionWindow.pinned
                contentItem: Canvas {
                    id: pinCanvas
                    Connections { target: sessionWindow; function onToolbarChanged() { pinCanvas.requestPaint() } }
                    onPaint: {
                        const c = getContext("2d")
                        c.reset()
                        c.strokeStyle = "#f0f1f4"
                        c.lineWidth = 1.5
                        c.beginPath()
                        c.moveTo(8, 4); c.lineTo(16, 4); c.lineTo(15, 11)
                        c.lineTo(18, 14); c.lineTo(6, 14); c.lineTo(9, 11); c.closePath()
                        c.moveTo(12, 14); c.lineTo(12, 21); c.stroke()
                        if (!sessionWindow.pinned) {
                            c.beginPath(); c.moveTo(4, 3); c.lineTo(21, 21); c.stroke()
                        }
                    }
                }
            }
            Text { text: "FPS —"; color: "#eff1f4"; font.pixelSize: 12 }
            Text { text: "Video — Mbps"; color: "#eff1f4"; font.pixelSize: 12 }
            Text { text: "Loss —%"; color: "#8aa6c9"; font.pixelSize: 12 }
            Text { text: "RTT — ms"; color: "#eff1f4"; font.pixelSize: 12 }
            ColumnLayout {
                spacing: 0
                Text {
                    id: targetText
                    text: "Encoder Target " + (sessionWindow.bitrateKbps / 1000).toFixed(1) + " Mbps"
                    color: "#eff1f4"
                    font.pixelSize: 13
                }
                Slider {
                    objectName: "bitrateSlider"
                    Layout.preferredWidth: targetText.implicitWidth
                    Layout.preferredHeight: 16
                    from: sessionWindow.minimumBitrateKbps
                    to: sessionWindow.maximumBitrateKbps
                    stepSize: sessionWindow.bitrateStepKbps
                    value: sessionWindow.bitrateKbps
                    focusPolicy: Qt.NoFocus
                    onMoved: sessionWindow.bitrateKbps = Math.round(value)
                }
            }
            ToolButton {
                objectName: "fullscreenButton"
                text: "□"
                ToolTip.text: "Fullscreen / windowed"
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
                Text { text: "You can disconnect or keep waiting."; font.pixelSize: 14; color: "#e1e4ea" }
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    Button { text: "Disconnect"; onClicked: sessionWindow.disconnectSession() }
                    Button { text: "Keep Waiting"; onClicked: sessionWindow.keepWaiting() }
                }
            }
        }
    }
}
