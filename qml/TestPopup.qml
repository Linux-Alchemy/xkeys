import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    required property string controlId
    required property string controlName
    required property string bindingLabel
    required property int activeLayer
    required property bool blank
    required property bool connected
    property bool editing: false

    objectName: "testPopup"
    color: "#1c2630"
    border.color: "#43505e"
    radius: 3
    implicitWidth: 220
    implicitHeight: 235

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        Text {
            text: root.editing ? qsTr("SAVED BINDING") : qsTr("TEST RESULT")
            color: "#aab6c3"
            font.pixelSize: 12
            font.letterSpacing: 1.2
        }
        Text {
            Layout.fillWidth: true
            text: !root.connected ? qsTr("Disconnected") : (root.controlId.length === 0 ? qsTr("Select a control") : root.controlName)
            color: "#e5e9f0"
            font.pixelSize: 18
            wrapMode: Text.Wrap
        }
        Text {
            objectName: "resolvedBinding"
            Layout.fillWidth: true
            text: root.connected && root.controlId.length > 0 ? root.bindingLabel : "—"
            color: root.blank ? "#aab6c3" : "#8dd4e8"
            font.pixelSize: 23
            font.bold: true
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            text: root.connected && root.controlId.length > 0 ? qsTr("Layer %1 · %2").arg(root.activeLayer).arg(root.blank ? qsTr("Unassigned") : qsTr("Assigned")) : qsTr("Click a key or thumb control to see its demo binding.")
            color: "#aab6c3"
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }
        Item {
            Layout.fillHeight: true
        }
    }
}
