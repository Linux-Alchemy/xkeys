pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var controls
    required property string selectedControl
    required property bool ready
    required property var heldControls

    signal controlPressed(string controlId)

    implicitWidth: 610
    implicitHeight: 345

    component PadButton: Button {
        id: padButton

        required property string controlId
        property string accessibleLabel: text

        objectName: "pad-" + controlId
        enabled: root.ready
        Layout.fillWidth: true
        Layout.fillHeight: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: accessibleLabel
        ToolTip.visible: hovered
        ToolTip.text: accessibleLabel
        ToolTip.delay: 500

        contentItem: Text {
            text: padButton.text
            color: padButton.enabled ? "#e5e9f0" : "#77808c"
            font.pixelSize: 15
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: root.heldControls.indexOf(padButton.controlId) >= 0 ? "#496479" : (padButton.down ? "#384755" : (padButton.hovered ? "#303b47" : "#242d37"))
            border.color: padButton.activeFocus ? "#e5e9f0" : (root.selectedControl === padButton.controlId ? "#8dd4e8" : "#43505e")
            border.width: padButton.activeFocus ? 2 : 1
            radius: 3
        }
        onClicked: root.controlPressed(controlId)
    }

    RowLayout {
        anchors.fill: parent
        spacing: 20

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            Text {
                text: qsTr("KEYPAD")
                color: "#aab6c3"
                font.pixelSize: 12
                font.letterSpacing: 1.5
            }
            GridLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                columns: 5
                rowSpacing: 8
                columnSpacing: 8

                Repeater {
                    model: root.controls.slice(0, 19)

                    PadButton {
                        required property var modelData

                        controlId: modelData.controlId
                        text: modelData.label
                        Layout.preferredWidth: 60
                        Layout.preferredHeight: 65
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 60
                    Layout.preferredHeight: 65
                    spacing: 3

                    PadButton {
                        controlId: "wheelUp"
                        text: "↑"
                        accessibleLabel: qsTr("Scroll up")
                    }
                    PadButton {
                        controlId: "wheelClick"
                        text: qsTr("Wheel")
                        accessibleLabel: qsTr("Wheel click")
                    }
                    PadButton {
                        controlId: "wheelDown"
                        text: "↓"
                        accessibleLabel: qsTr("Scroll down")
                    }
                }
            }
        }

        ColumnLayout {
            Layout.preferredWidth: 160
            Layout.fillHeight: true
            spacing: 12

            Text {
                text: qsTr("THUMB CONTROLS")
                color: "#aab6c3"
                font.pixelSize: 12
                font.letterSpacing: 1
            }
            PadButton {
                controlId: "thumb"
                text: qsTr("Thumb button")
                Layout.maximumHeight: 48
            }
            GridLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                columns: 3
                rowSpacing: 5
                columnSpacing: 5

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
                PadButton {
                    controlId: "dpadUp"
                    text: "↑"
                }
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
                PadButton {
                    controlId: "dpadLeft"
                    text: "←"
                }
                Text {
                    text: qsTr("D-pad")
                    color: "#aab6c3"
                    font.pixelSize: 10
                    horizontalAlignment: Text.AlignHCenter
                    Layout.fillWidth: true
                }
                PadButton {
                    controlId: "dpadRight"
                    text: "→"
                }
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
                PadButton {
                    controlId: "dpadDown"
                    text: "↓"
                }
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
            }
            PadButton {
                controlId: "paddle"
                text: qsTr("Thumb paddle")
                Layout.maximumHeight: 55
            }
        }
    }
}
