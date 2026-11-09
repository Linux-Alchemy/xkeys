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
    required property var controlLabels

    signal controlPressed(string controlId)

    implicitWidth: 610
    implicitHeight: 345

    component PadButton: Button {
        id: padButton

        required property string controlId
        property string accessibleLabel: text
        readonly property string savedShortcut: root.controlLabels[controlId] || ""
        readonly property bool compact: controlId.startsWith("wheel")
        topPadding: 3
        bottomPadding: 3

        objectName: "pad-" + controlId
        implicitWidth: 60
        enabled: root.ready
        Layout.fillWidth: true
        Layout.fillHeight: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: accessibleLabel + (savedShortcut.length > 0 ? ": " + savedShortcut : "")
        ToolTip.visible: hovered
        ToolTip.text: accessibleLabel + (savedShortcut.length > 0 ? ": " + savedShortcut : "")
        ToolTip.delay: 500

        contentItem: Column {
            spacing: 3
            Text {
                textFormat: Text.PlainText
                width: parent.width
                text: padButton.compact && padButton.savedShortcut.length > 0 ? padButton.text + ": " + padButton.savedShortcut : padButton.text
                color: padButton.enabled ? "#e5e9f0" : "#77808c"
                font.pixelSize: padButton.compact ? 10 : (padButton.savedShortcut.length > 0 ? 12 : 15)
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                visible: !padButton.compact && padButton.savedShortcut.length > 0
                text: padButton.savedShortcut
                color: padButton.savedShortcut === "Blank" ? "#77808c" : "#8dd4e8"
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 2
                elide: Text.ElideRight
            }
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
                textFormat: Text.PlainText
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
                textFormat: Text.PlainText
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
                uniformCellWidths: true
                uniformCellHeights: true
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
                    textFormat: Text.PlainText
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
