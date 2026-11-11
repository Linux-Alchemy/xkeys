import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import XKeys

ApplicationWindow {
    id: root
    required property Controller controller
    readonly property bool bindingTransaction: controller.recordingBusy || controller.recordingState === "Draft" || controller.recordingState === "Saving"
    onActiveChanged: controller.setWindowActive(active && visible)
    onVisibleChanged: controller.setWindowActive(active && visible)
    onClosing: controller.cancelDraft()
    objectName: "mainWindow"
    visible: true
    title: qsTr("Xkeys Demo")
    width: 1180
    height: 880
    minimumWidth: 1000
    minimumHeight: 780
    color: "#141b23"

    function controlName(id: string): string {
        for (let index = 0; index < controller.controls.length; ++index) {
            const control = controller.controls[index];
            if (control.controlId === id)
                return id.startsWith("key") ? qsTr("Button %1").arg(control.label) : control.label;
        }
        return "";
    }
    function profileIndex(): int {
        for (let index = 0; index < controller.profiles.length; ++index) {
            if (controller.profiles[index].id === controller.selectedProfileId)
                return index;
        }
        return -1;
    }

    component AppText: Text {
        textFormat: Text.PlainText
        color: "#c5cdd6"
        font.pixelSize: 13
        wrapMode: Text.Wrap
    }
    component ActionButton: Button {
        palette.button: "#293541"
        palette.buttonText: "#e5e9f0"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            spacing: 16
            AppText {
                text: "xkeys"
                font.pixelSize: 30
                font.bold: true
                color: "#f0f3f6"
            }
            AppText {
                text: root.controller.recordingAvailable ? qsTr("RECORDING EDITOR / PAD OUTPUT SIMULATED") : qsTr("DEMO / SIMULATED CONTROLS")
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
            AppText {
                objectName: "connectionStatus"
                text: root.controller.connected ? qsTr("Connected") : root.controller.statusText
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            ComboBox {
                id: profileSelector
                objectName: "profileSelector"
                Layout.fillWidth: true
                model: root.controller.profiles
                textRole: "name"
                valueRole: "id"
                currentIndex: root.profileIndex()
                displayText: currentIndex < 0 ? qsTr("Choose a profile") : currentText
                enabled: root.controller.connected && !root.controller.pending && !root.bindingTransaction
                onActivated: root.controller.selectProfile(currentValue)
            }
            ActionButton {
                objectName: "newProfileButton"
                text: qsTr("New profile")
                enabled: root.controller.connected && !root.controller.pending && !root.bindingTransaction
                onClicked: newProfileDialog.open()
            }
            ActionButton {
                text: qsTr("Rename")
                enabled: root.controller.hasProfile && root.controller.connected && !root.controller.pending && !root.bindingTransaction
                onClicked: renameDialog.open()
            }
            ActionButton {
                objectName: "deleteProfileButton"
                text: qsTr("Delete")
                enabled: root.controller.hasProfile && root.controller.connected && !root.controller.pending && !root.bindingTransaction
                onClicked: deleteDialog.open()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            ActionButton {
                objectName: "activateButton"
                text: qsTr("Activate")
                enabled: root.controller.hasProfile && root.controller.connected && !root.controller.pending && !root.bindingTransaction
                onClicked: root.controller.activateSelected()
            }
            ActionButton {
                objectName: "editProfileButton"
                text: qsTr("Edit")
                enabled: root.controller.hasProfile && root.controller.connected && !root.controller.pending && !root.bindingTransaction && !root.controller.editing
                onClicked: root.controller.editSelected()
            }
            ActionButton {
                objectName: "deactivateButton"
                text: qsTr("Deactivate")
                enabled: root.controller.mode !== "Inactive" && root.controller.connected && !root.controller.pending && !root.bindingTransaction
                onClicked: root.controller.deactivate()
            }
            AppText {
                objectName: "activeProfileBanner"
                Layout.fillWidth: true
                text: !root.controller.connected ? qsTr("Disconnected") : (root.controller.activeProfileId.length > 0 ? qsTr("Active demo: %1 · Layer %2").arg(root.controller.activeProfileName).arg(root.controller.activeLayer) : (root.controller.editing ? qsTr("Editing %1 · output off").arg(root.controller.selectedProfileName) : qsTr("No active demo profile")))
                color: root.controller.activeProfileId.length > 0 ? "#9ad5b5" : "#aab6c3"
                horizontalAlignment: Text.AlignRight
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.controller.connected && root.controller.mode === "Active"
            spacing: 10
            CheckBox {
                id: holdMode
                objectName: "holdMode"
                text: qsTr("Hold controls")
                enabled: !root.controller.pending
                palette.windowText: "#c5cdd6"
                onClicked: {
                    if (!checked && root.controller.heldControls.length > 0)
                        root.controller.releaseAll();
                }
            }
            AppText {
                Layout.fillWidth: true
                text: holdMode.checked ? qsTr("Click to press and hold; click again to release. Wheel arrows always tap.") : qsTr("Click a control to tap its binding.")
            }
            ActionButton {
                objectName: "releaseAllButton"
                text: qsTr("Release all")
                enabled: root.controller.heldControls.length > 0 && !root.controller.pending
                onClicked: root.controller.releaseAll()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.controller.editing
            spacing: 10
            AppText {
                text: qsTr("Edit layer")
            }
            ActionButton {
                objectName: "editLayerOneButton"
                text: qsTr("Layer 1")
                highlighted: root.controller.editLayer === 1
                enabled: !root.controller.pending && !root.controller.recordingBusy && root.controller.recordingState !== "Saving"
                onClicked: root.controller.setEditLayer(1)
            }
            ActionButton {
                objectName: "editLayerTwoButton"
                text: qsTr("Layer 2")
                highlighted: root.controller.editLayer === 2
                enabled: !root.controller.pending && !root.controller.recordingBusy && root.controller.recordingState !== "Saving"
                onClicked: root.controller.setEditLayer(2)
            }
            CheckBox {
                objectName: "thumbLayerCheckbox"
                text: qsTr("Hold thumb for Layer 2")
                checked: root.controller.thumbLayer
                enabled: !root.controller.pending && !root.bindingTransaction
                palette.windowText: "#c5cdd6"
                onClicked: {
                    root.controller.setThumbLayer(checked);
                    checked = Qt.binding(() => root.controller.thumbLayer);
                }
            }
            AppText {
                Layout.fillWidth: true
                text: root.controller.recordingState === "Draft" ? qsTr("Switching layers discards this unsaved draft.") : (!root.controller.thumbLayer ? qsTr("Enable the thumb control to use Layer 2 during playback.") : qsTr("The thumb switches layers; it does not send a shortcut."))
                color: "#aab6c3"
            }
        }

        AppText {
            Layout.fillWidth: true
            text: !root.controller.connected ? qsTr("Start demo to reconnect.") : root.controller.editing ? qsTr("Layer %1: select a button and recording device, click Start recording, then press the shortcut when ready. Save or Cancel explicitly.").arg(root.controller.editLayer) : (root.controller.mode === "Active" ? qsTr("Filled controls are held. The result and transitions come from the running demo.") : qsTr("Create or choose a profile, then explicitly Activate or Edit. New profiles have two blank layers."))
            color: "#aab6c3"
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
                heldControls: root.controller.heldControls
                controlLabels: root.controller.controlLabels
                ready: root.controller.connected && !root.controller.pending && !root.bindingTransaction && (root.controller.editing || root.controller.mode === "Active")
                onControlPressed: controlId => {
                    if (root.controller.editing)
                        root.controller.selectControl(controlId);
                    else
                        root.controller.simulate(controlId, holdMode.checked);
                }
            }
            ColumnLayout {
                Layout.preferredWidth: root.controller.editing ? 370 : 235
                Layout.fillHeight: root.controller.editing
                Layout.alignment: Qt.AlignVCenter
                spacing: 10
                BindingEditor {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.maximumHeight: 650
                    visible: root.controller.editing
                    controller: root.controller
                    targetName: root.controlName(root.controller.selectedControl)
                }
                TestPopup {
                    visible: !root.controller.editing
                    Layout.fillWidth: true
                    controlId: root.controller.selectedControl
                    controlName: root.controlName(root.controller.selectedControl)
                    bindingLabel: root.controller.bindingLabel
                    activeLayer: root.controller.selectedLayer
                    blank: root.controller.blank
                    connected: root.controller.connected
                    editing: root.controller.editing
                }
                AppText {
                    Layout.fillWidth: true
                    visible: !root.controller.editing
                    text: qsTr("Held output: %1").arg(root.controller.connected ? root.controller.heldOutputLabel : qsTr("None"))
                    color: "#8dd4e8"
                }
                AppText {
                    visible: !root.controller.editing
                    text: qsTr("SIMULATED TRANSITIONS")
                    font.pixelSize: 11
                    font.letterSpacing: 1
                }
                ScrollView {
                    visible: !root.controller.editing
                    Layout.fillWidth: true
                    Layout.preferredHeight: 95
                    clip: true
                    contentWidth: availableWidth
                    TextArea {
                        objectName: "outputEvents"
                        readOnly: true
                        text: root.controller.outputEvents.join("\n")
                        wrapMode: TextEdit.Wrap
                        color: "#c5cdd6"
                        font.pixelSize: 12
                        background: Rectangle {
                            color: "#1c2630"
                        }
                    }
                }
            }
        }

        AppText {
            objectName: "errorMessage"
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.controller.errorText
            color: "#f0bd98"
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            AppText {
                Layout.fillWidth: true
                text: root.controller.recordingAvailable ? qsTr("Pad output is simulated. Recording captures only the chosen keyboard.") : qsTr("Simulation only. Your physical keypad is untouched.")
                color: "#8f9baa"
                font.pixelSize: 12
            }
            ActionButton {
                objectName: "reconnectButton"
                text: qsTr("Start demo")
                visible: !root.controller.connected
                enabled: !root.controller.pending
                onClicked: root.controller.reconnectDemo()
            }
            ActionButton {
                objectName: "stopButton"
                text: qsTr("Stop demo")
                enabled: root.controller.connected && !root.controller.pending && !root.bindingTransaction
                onClicked: root.controller.stopDemo()
            }
        }
    }

    Dialog {
        id: newProfileDialog
        objectName: "newProfileDialog"
        anchors.centerIn: Overlay.overlay
        implicitWidth: 360
        title: qsTr("New blank profile")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: {
            newName.text = "";
            newName.forceActiveFocus();
        }
        onAccepted: root.controller.createProfile(newName.text)
        TextField {
            id: newName
            onAccepted: newProfileDialog.accept()
            objectName: "newProfileName"
            width: 300
            placeholderText: qsTr("Profile name")
            maximumLength: 80
        }
    }
    Dialog {
        id: renameDialog
        anchors.centerIn: Overlay.overlay
        implicitWidth: 360
        title: qsTr("Rename profile")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: {
            renameInput.text = root.controller.selectedProfileName;
            renameInput.forceActiveFocus();
        }
        onAccepted: root.controller.renameSelected(renameInput.text)
        TextField {
            id: renameInput
            onAccepted: renameDialog.accept()
            width: 300
            maximumLength: 80
        }
    }
    Dialog {
        id: deleteDialog
        objectName: "deleteProfileDialog"
        anchors.centerIn: Overlay.overlay
        implicitWidth: 360
        title: qsTr("Delete profile?")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: root.controller.deleteSelected()
        Label {
            textFormat: Text.PlainText
            width: 300
            text: qsTr("Delete “%1” and all its saved bindings?").arg(root.controller.selectedProfileName)
            wrapMode: Text.Wrap
        }
    }
}
