import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import XKeys

ApplicationWindow {
    id: root

    required property Controller controller

    objectName: "mainWindow"
    visible: true
    title: qsTr("Xkeys Demo")
    width: 1080
    height: 640
    minimumWidth: 800
    minimumHeight: 550
    color: "#141b23"

    function controlName(id: string): string {
        for (let index = 0; index < controller.controls.length; ++index) {
            const control = controller.controls[index];
            if (control.controlId === id)
                return id.startsWith("key") ? qsTr("Key %1").arg(control.label) : control.label;
        }
        return "";
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 28
        spacing: 20

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Text {
                text: "xkeys"
                color: "#f0f3f6"
                font.pixelSize: 30
                font.bold: true
            }
            Text {
                text: qsTr("DEMO / SIMULATED CONTROLS")
                color: "#8dd4e8"
                font.pixelSize: 12
                font.letterSpacing: 1
            }
            Item {
                Layout.fillWidth: true
            }
            Rectangle {
                implicitWidth: 8
                implicitHeight: 8
                radius: 4
                color: root.controller.connected ? "#9ad5b5" : "#d6b17e"
            }
            Text {
                objectName: "connectionStatus"
                text: root.controller.statusText
                color: "#c5cdd6"
                font.pixelSize: 13
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6

            Text {
                text: qsTr("Try the keypad")
                color: "#e5e9f0"
                font.pixelSize: 23
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("01 and 02 have sample shortcuts. Every other control is blank.")
                color: "#aab6c3"
                font.pixelSize: 14
                wrapMode: Text.Wrap
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 24

            Keypad {
                objectName: "keypad"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.maximumHeight: 420
                Layout.alignment: Qt.AlignVCenter
                controls: root.controller.controls
                selectedControl: root.controller.connected ? root.controller.selectedControl : ""
                ready: root.controller.connected && !root.controller.pending
                onControlPressed: controlId => root.controller.selectControl(controlId)
            }
            TestPopup {
                Layout.preferredWidth: root.width < 940 ? 180 : 235
                Layout.alignment: Qt.AlignVCenter
                controlId: root.controller.selectedControl
                controlName: root.controlName(root.controller.selectedControl)
                bindingLabel: root.controller.bindingLabel
                activeLayer: root.controller.selectedLayer
                blank: root.controller.blank
                connected: root.controller.connected
            }
        }

        Text {
            objectName: "errorMessage"
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.controller.errorText
            color: "#f0bd98"
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Text {
                Layout.fillWidth: true
                text: qsTr("Your physical keypad is untouched. Closing this window keeps the demo running.")
                color: "#8f9baa"
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            Button {
                objectName: "reconnectButton"
                text: qsTr("Start demo")
                visible: !root.controller.connected
                enabled: !root.controller.pending
                onClicked: root.controller.reconnectDemo()
            }
            Button {
                objectName: "stopButton"
                text: qsTr("Stop demo")
                enabled: root.controller.connected && !root.controller.pending
                onClicked: root.controller.stopDemo()
            }
        }
    }
}
