import QtQuick 2.0
import QtQuick.Controls 2.2
import QtQuick.Window 2.2

import Session 1.0

Item {
    property Session session
    property string appName
    property string stageText : isResume ? qsTr("Resuming %1...").arg(appName) :
                                           qsTr("Starting %1...").arg(appName)
    property bool isResume : false
    property bool quitAfter : false

    function stageStarting(stage)
    {
        // Update the spinner text
        stageText = qsTr("Starting %1...").arg(stage)
    }

    function stageFailed(stage, errorCode, failingPorts)
    {
        // Display the error dialog after Session::exec() returns
        streamSegueErrorDialog.text = qsTr("Starting %1 failed: Error %2").arg(stage).arg(errorCode)

        if (failingPorts) {
            streamSegueErrorDialog.text += "\n\n" + qsTr("Check your firewall and port forwarding rules for port(s): %1").arg(failingPorts)
        }
    }

    function connectionStarted()
    {
        // Hide the UI contents so the user doesn't
        // see them briefly when we pop off the StackView
        stageSpinner.visible = false
        stageLabel.visible = false
        hintText.visible = false

        // Hide the window now that streaming has begun
        window.visible = false
    }

    function sessionCleanupWaitChanged(waiting, text)
    {
        if (waiting) {
            stageText = text
        }
        cancelWaitButton.visible = waiting
    }

    function activeSessionTakeoverRequested(text)
    {
        activeSessionTakeoverDialog.text = text
        activeSessionTakeoverDialog.open()
    }

    function displayLaunchError(text)
    {
        // Display the error dialog after Session::exec() returns
        streamSegueErrorDialog.text = text
        console.error(text)
    }

    function displayLaunchWarning(text)
    {
        // This toast appears for 3 seconds, just shorter than how long
        // Session will wait for it to be displayed. This gives it time
        // to transition to invisible before continuing.
        var toast = Qt.createQmlObject('import QtQuick.Controls 2.2; ToolTip {}', parent, '')
        toast.text = text
        toast.timeout = 3000
        toast.visible = true
        console.warn(text)
    }

    function sessionFinished()
    {
        activeSessionTakeoverDialog.close()
        if (quitAfter) {
            if (streamSegueErrorDialog.text) {
                // Quit when the error dialog is acknowledged
                streamSegueErrorDialog.quitAfter = quitAfter
                streamSegueErrorDialog.open()
            }
            else {
                // Quit immediately
                Qt.quit()
            }
        } else {
            // Exit this view
            stackView.pop()

            // Show the Qt window again after streaming
            window.visible = true

            // Display any launch errors. We do this after
            // the Qt UI is visible again to prevent losing
            // focus on the dialog.
            if (streamSegueErrorDialog.text) {
                streamSegueErrorDialog.quitAfter = quitAfter
                streamSegueErrorDialog.open()
            }
        }
    }

    function sessionReadyForDeletion()
    {
        // Garbage collect the Session object since it's pretty heavyweight
        // and keeps session-owned resources alive until it is deleted.
        session = null
        gc()
    }

    StackView.onDeactivating: {
        // Show the toolbar again when popped off the stack
        toolBar.visible = true
    }

    StackView.onActivated: {
        // Hide the toolbar before we start loading
        toolBar.visible = false

        // Hook up our signals
        session.stageStarting.connect(stageStarting)
        session.stageFailed.connect(stageFailed)
        session.connectionStarted.connect(connectionStarted)
        session.sessionCleanupWaitChanged.connect(sessionCleanupWaitChanged)
        session.activeSessionTakeoverRequested.connect(activeSessionTakeoverRequested)
        session.displayLaunchError.connect(displayLaunchError)
        session.displayLaunchWarning.connect(displayLaunchWarning)
        session.sessionFinished.connect(sessionFinished)
        session.readyForDeletion.connect(sessionReadyForDeletion)

        // Kick off the stream
        spinnerTimer.start()
        streamLoader.active = true
    }

    SessionTakeoverDialog {
        id: activeSessionTakeoverDialog

        onAccepted: session.respondToActiveSessionTakeover(true)
        onRejected: session.respondToActiveSessionTakeover(false)
    }

    Timer {
        id: spinnerTimer

        // Display the spinner appearance a bit to allow us to reach
        // the code in Session.exec() that pumps the event loop.
        // If we display it immediately, it will briefly hang in the
        // middle of the animation on Windows, which looks very
        // obviously broken.
        interval: 100
        onTriggered: stageSpinner.running = true
    }

    Loader {
        id: streamLoader
        active: false
        asynchronous: true

        onLoaded: {
            hintText.text = qsTr("Tip:") + " " + qsTr("Press %1 to disconnect your session").arg(qsTr("Ctrl+Alt+Shift+Q"))

            // Garbage collect QML stuff before we start streaming,
            // since we'll probably be streaming for a while and we
            // won't be able to GC during the stream.
            gc()

            // Run the streaming session to completion
            session.exec(Window.window)
        }

        sourceComponent: Item {}
    }

    Column {
        anchors.centerIn: parent
        spacing: 12

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 5

            BusyIndicator {
                id: stageSpinner
                running: false
            }

            Label {
                id: stageLabel
                height: stageSpinner.height
                text: stageText
                font.pointSize: 20
                verticalAlignment: Text.AlignVCenter

                wrapMode: Text.Wrap
            }
        }

        Button {
            id: cancelWaitButton
            anchors.horizontalCenter: parent.horizontalCenter
            visible: false
            text: qsTr("Cancel")
            onClicked: session.cancelConnectionStart()
        }
    }

    Label {
        id: hintText
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 50
        anchors.horizontalCenter: parent.horizontalCenter
        font.pointSize: 18
        verticalAlignment: Text.AlignVCenter

        wrapMode: Text.Wrap
    }
}
