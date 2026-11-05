import QtQuick 2.15
import QtQuick.Controls 2.15
import ComputerManager 1.0
import StreamingPreferences 1.0

// Shared by Add/Edit. Discovery is asynchronous and unauthenticated: it only
// filters choices. Failed discovery leaves manual/offline configuration intact.
PlankComboBox {
    id: control
    property string hostAddress: ""
    property bool probingEnabled: false
    property int hostPlatform: 0
    property int hostDisplayPolicy: -1
    property int captureSource: StreamingPreferences.PLANK_CAPTURE_NVFBC_8BIT
    property int requestId: 0
    property bool rebuilding: false
    textRole: "text"
    model: ListModel { id: choices }

    function selectCaptureSource(source) {
        captureSource = source
        rebuild()
    }
    function rebuild() {
        rebuilding = true
        var desired = captureSource
        choices.clear()
        if (hostPlatform !== 2) {
            choices.append({text: qsTr("NvFBC — 8-bit source"), val: StreamingPreferences.PLANK_CAPTURE_NVFBC_8BIT})
            choices.append({text: qsTr("Native X11/XShm — 10-bit (Experimental)"), val: StreamingPreferences.PLANK_CAPTURE_X11_NATIVE10})
        }
        if (hostPlatform !== 1) {
            choices.append({text: qsTr("ScreenCaptureKit — macOS"), val: StreamingPreferences.PLANK_CAPTURE_SCREENCAPTUREKIT})
        }
        var selection = 0
        for (var i = 0; i < choices.count; ++i) if (choices.get(i).val === desired) selection = i
        currentIndex = selection
        rebuilding = false
        captureSource = choices.get(selection).val
    }
    function scheduleProbe() {
        requestId = 0
        hostPlatform = 0
        hostDisplayPolicy = -1
        rebuild()
        probeTimer.stop()
        if (probingEnabled && hostAddress.trim() !== "") probeTimer.restart()
    }
    onCurrentIndexChanged: {
        if (!rebuilding && currentIndex >= 0 && currentIndex < choices.count)
            captureSource = choices.get(currentIndex).val
    }
    onHostAddressChanged: scheduleProbe()
    onProbingEnabledChanged: scheduleProbe()
    Component.onCompleted: rebuild()
    Timer {
        id: probeTimer
        interval: 500
        onTriggered: {
            if (!control.probingEnabled || control.hostAddress.trim() === "") return
            control.requestId = ComputerManager.probeHostPlatform(control.hostAddress.trim())
            if (control.requestId === 0) restart()
        }
    }
    Connections {
        target: ComputerManager
        function onHostPlatformDetected(id, address, platform, displayPolicy) {
            if (!control.probingEnabled || id !== control.requestId || address !== control.hostAddress.trim()) return
            control.requestId = 0
            control.hostPlatform = platform
            control.hostDisplayPolicy = displayPolicy
            control.rebuild()
        }
    }
}
