pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import XKeys

Rectangle {
    id: root
    required property Controller controller
    required property string targetName
    readonly property bool conflictDecision: controller.recordingState === "Draft" && (controller.bindingConflicts.length > 0 || !controller.conflictsChecked)
    objectName: "bindingEditor"
    color: "#1c2630"
    border.color: "#43505e"
    radius: 3
    implicitWidth: 370
    implicitHeight: 570

    function keyboardIndex(): int {
        for (let index = 0; index < controller.recordingKeyboards.length; ++index) {
            if (controller.recordingKeyboards[index].id === controller.recordingKeyboardId)
                return index;
        }
        return -1;
    }

    function keyboardDisplay(): string {
        const index = keyboardIndex();
        return index >= 0 ? controller.recordingKeyboards[index].name : qsTr("Choose recording device");
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12
        Text {
            textFormat: Text.PlainText
            objectName: "recordingTarget"
            Layout.fillWidth: true
            text: root.targetName.length > 0 ? qsTr("%1 · Layer 1").arg(root.targetName) : qsTr("Select a button · Layer 1")
            color: "#e5e9f0"
            font.pixelSize: 21
            font.bold: true
            wrapMode: Text.Wrap
        }
        Text {
            textFormat: Text.PlainText
            objectName: "savedChord"
            Layout.fillWidth: true
            text: root.targetName.length > 0 ? qsTr("Saved: %1").arg(root.controller.bindingLabel) : qsTr("Your saved binding stays unchanged until Save.")
            color: "#aab6c3"
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                width: parent.width
                spacing: 12
                Text {
                    textFormat: Text.PlainText
                    text: qsTr("Recording device")
                    color: "#c5cdd6"
                    font.pixelSize: 13
                }
                ComboBox {
                    id: keyboardCombo
                    objectName: "recordingKeyboardSelector"
                    Layout.fillWidth: true
                    model: root.controller.recordingKeyboards
                    textRole: "name"
                    valueRole: "id"
                    currentIndex: root.keyboardIndex()
                    displayText: root.keyboardDisplay()
                    contentItem: Text {
                        textFormat: Text.PlainText
                        text: keyboardCombo.displayText
                        color: keyboardCombo.palette.buttonText
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                    delegate: ItemDelegate {
                        id: keyboardEntry
                        required property var modelData
                        required property int index
                        width: keyboardCombo.width
                        enabled: modelData.accessible
                        highlighted: keyboardCombo.highlightedIndex === index
                        contentItem: Text {
                            textFormat: Text.PlainText
                            text: keyboardEntry.modelData.name
                            color: keyboardEntry.palette.buttonText
                            opacity: keyboardEntry.enabled ? 1 : 0.5
                            elide: Text.ElideRight
                        }
                        ToolTip.visible: hovered && !enabled
                        ToolTip.text: modelData.reason
                    }
                    enabled: root.controller.recordingAvailable && !root.controller.recordingBusy && root.controller.recordingState !== "Draft" && root.controller.recordingState !== "Saving"
                    onActivated: root.controller.setRecordingKeyboard(currentValue)
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: qsTr("Keyboard shortcuts only for now. Native mouse clicks and scrolling are not supported yet.")
                    color: "#aab6c3"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                Button {
                    objectName: "refreshKeyboardsButton"
                    text: qsTr("Refresh devices")
                    enabled: root.controller.recordingAvailable && !root.controller.recordingBusy && root.controller.recordingState !== "Draft" && root.controller.recordingState !== "Saving"
                    onClicked: root.controller.refreshRecordingKeyboards()
                }
                Text {
                    textFormat: Text.PlainText
                    objectName: "recordingStatus"
                    Layout.fillWidth: true
                    text: root.controller.recordingUnavailableReason.length > 0 && (root.controller.recordingState === "Idle" || root.controller.recordingState === "Error") ? root.controller.recordingUnavailableReason : root.controller.recordingMessage
                    color: root.controller.recordingState === "Error" ? "#f0bd98" : "#c5cdd6"
                    font.pixelSize: 14
                    wrapMode: Text.Wrap
                }
                Text {
                    textFormat: Text.PlainText
                    objectName: "recordedChord"
                    Layout.fillWidth: true
                    text: root.controller.draftChord.length > 0 ? root.controller.draftChord : "—"
                    color: "#8dd4e8"
                    font.pixelSize: 27
                    font.bold: true
                    wrapMode: Text.Wrap
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    visible: root.controller.recordingState === "Recording"
                    text: qsTr("%1 s remaining · Release all keys to finish. Use mouse Cancel; Esc can be recorded.").arg(root.controller.recordingRemainingSeconds)
                    color: "#aab6c3"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                TextField {
                    objectName: "draftActionLabel"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Optional action label")
                    text: root.controller.draftActionLabel
                    enabled: root.controller.recordingState === "Draft"
                    maximumLength: 80
                    onTextEdited: root.controller.draftActionLabel = text
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    visible: root.controller.bindingConflicts.length > 0
                    text: root.controller.bindingConflicts.join("\n")
                    color: "#f0bd98"
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: root.controller.conflictCheckNote
                    visible: text.length > 0
                    color: "#aab6c3"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: root.conflictDecision
            text: root.controller.bindingConflicts.length > 0 ? qsTr("Desktop overlap: use this binding anyway, or record a different shortcut?") : qsTr("Desktop check incomplete: use this binding anyway, or record a different shortcut?")
            textFormat: Text.PlainText
            color: "#f0bd98"
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "saveBindingButton"
                visible: !root.conflictDecision
                text: qsTr("Save")
                Layout.fillWidth: true
                enabled: root.controller.canSaveDraft
                onClicked: root.controller.saveDraft()
            }
            Button {
                objectName: "useConflictBindingButton"
                Layout.fillWidth: true
                visible: root.conflictDecision
                text: qsTr("Use binding anyway")
                onClicked: {
                    root.controller.setConflictAcknowledged(true);
                    root.controller.saveDraft();
                }
            }
            Button {
                objectName: "cancelRecordingButton"
                text: root.controller.recordingBusy ? qsTr("Cancel recording") : qsTr("Cancel draft")
                Layout.fillWidth: true
                enabled: root.controller.recordingState !== "Idle" && root.controller.recordingState !== "Saving"
                onClicked: root.controller.cancelDraft()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "recordAgainButton"
                visible: !root.conflictDecision
                text: qsTr("Record again")
                Layout.fillWidth: true
                enabled: root.targetName.length > 0 && root.controller.recordingAvailable && root.controller.recordingKeyboardId.length > 0 && !root.controller.recordingBusy && !root.controller.pending && root.controller.recordingState !== "Saving"
                onClicked: root.controller.recordAgain()
            }
            Button {
                objectName: "recordDifferentShortcutButton"
                Layout.fillWidth: true
                visible: root.conflictDecision
                text: qsTr("Record different")
                onClicked: root.controller.recordAgain()
            }
            Button {
                objectName: "clearBindingButton"
                text: qsTr("Clear binding")
                Layout.fillWidth: true
                enabled: root.targetName.length > 0 && !root.controller.recordingBusy && !root.controller.pending && root.controller.recordingState !== "Saving"
                onClicked: root.controller.clearBinding()
            }
        }
    }
}
