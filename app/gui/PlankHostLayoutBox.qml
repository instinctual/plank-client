import QtQuick 2.15
import QtQuick.Controls 2.15

// Add/Edit share stable bookmark values, independent of filtered row indices.
PlankComboBox {
    id: control
    property int captureSource: 0
    property int hostDisplayPolicy: -1
    readonly property int layoutChoice: currentIndex >= 0 && currentIndex < choices.count ?
                                           choices.get(currentIndex).val : 0
    property bool rebuilding: false
    property int selectedChoice: 0
    textRole: "text"
    model: ListModel { id: choices }

    function selectLayout(choice) {
        selectedChoice = choice
        rebuild()
    }

    function rebuild() {
        rebuilding = true
        var desired = selectedChoice
        choices.clear()
        if (captureSource === 2) {
            choices.append({text: qsTr("Match client display(s)"), val: 0})
            choices.append({text: qsTr("One Mac virtual display"), val: 1})
        } else {
            choices.append({text: qsTr("Match client displays"), val: 0})
            if (hostDisplayPolicy !== 0) {
                choices.append({text: qsTr("Match Host"), val: 1})
            }
            choices.append({text: qsTr("One virtual display"), val: 2})
            choices.append({text: qsTr("Two virtual displays (horizontal)"), val: 3})
        }
        var selection = 0
        for (var i = 0; i < choices.count; ++i) {
            if (choices.get(i).val === desired) selection = i
        }
        currentIndex = selection
        selectedChoice = choices.get(selection).val
        rebuilding = false
    }

    onCurrentIndexChanged: {
        if (!rebuilding && currentIndex >= 0 && currentIndex < choices.count)
            selectedChoice = choices.get(currentIndex).val
    }
    onCaptureSourceChanged: selectLayout(0)
    onHostDisplayPolicyChanged: rebuild()
    Component.onCompleted: rebuild()
}
