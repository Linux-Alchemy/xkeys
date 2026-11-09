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
    width: 1180
    height: 880
    minimumWidth: 1000
    minimumHeight: 780
    color: "#141b23"

    function controlName(id: string): string {
        for (let index = 0; index < controller.controls.length; ++index) {
            const control = controller.controls[index];
            if (control.controlId === id)
                return id.startsWith("key") ? qsTr("Key %1").arg(control.label) : control.label;
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
            AppText {
                objectName: "connectionStatus"
                text: root.controller.statusText
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
                enabled: root.controller.connected && !root.controller.pending
                onActivated: root.controller.selectProfile(currentValue)
            }
            ActionButton {
                objectName: "newProfileButton"
                text: qsTr("New profile")
                enabled: root.controller.connected && !root.controller.pending
                onClicked: newProfileDialog.open()
            }
            ActionButton {
                text: qsTr("Rename")
                enabled: root.controller.hasProfile && root.controller.connected && !root.controller.pending
                onClicked: renameDialog.open()
            }
            ActionButton {
                objectName: "deleteProfileButton"
                text: qsTr("Delete")
                enabled: root.controller.hasProfile && root.controller.connected && !root.controller.pending
                onClicked: deleteDialog.open()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            ActionButton {
                objectName: "activateButton"
                text: qsTr("Activate")
                enabled: root.controller.hasProfile && root.controller.connected && !root.controller.pending
                onClicked: root.controller.activateSelected()
            }
            ActionButton {
                objectName: "editProfileButton"
                text: qsTr("Edit")
                enabled: root.controller.hasProfile && root.controller.connected && !root.controller.pending && !root.controller.editing
                onClicked: root.controller.editSelected()
            }
            ActionButton {
                objectName: "deactivateButton"
                text: qsTr("Deactivate")
                enabled: root.controller.mode !== "Inactive" && root.controller.connected && !root.controller.pending
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
            visible: root.controller.connected && root.controller.editing
            spacing: 10
            AppText {
                text: qsTr("Editing layer")
            }
            ComboBox {
                objectName: "editLayerSelector"
                model: [qsTr("Layer 1"), qsTr("Layer 2")]
                currentIndex: root.controller.editLayer - 1
                enabled: !root.controller.pending
                onActivated: root.controller.setEditLayer(currentIndex + 1)
            }
            CheckBox {
                id: thumbLayerCheck
                objectName: "thumbLayerSwitch"
                nextCheckState: function () {
                    return thumbLayerCheck.checkState;
                }
                text: qsTr("Thumb button holds layer 2")
                checked: root.controller.thumbLayer
                enabled: !root.controller.pending
                onClicked: root.controller.setThumbLayer(!root.controller.thumbLayer)
                palette.windowText: "#c5cdd6"
            }
            Item {
                Layout.fillWidth: true
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.controller.connected && root.controller.editing
            spacing: 8
            ComboBox {
                id: sampleSelector
                objectName: "sampleSelector"
                model: root.controller.sampleShortcuts
                enabled: !root.controller.pending
                Layout.preferredWidth: 150
            }
            TextField {
                id: actionLabel
                objectName: "actionLabel"
                Layout.fillWidth: true
                placeholderText: qsTr("Optional action label")
                text: root.controller.bindingActionLabel
                enabled: !root.controller.pending
            }
            ActionButton {
                objectName: "assignButton"
                text: qsTr("Assign sample")
                enabled: root.controller.selectedControl.length > 0 && !root.controller.pending && !(root.controller.thumbLayer && root.controller.selectedControl === "thumb")
                onClicked: root.controller.assignSample(sampleSelector.currentIndex, actionLabel.text)
            }
            ActionButton {
                objectName: "clearBindingButton"
                text: qsTr("Clear binding")
                enabled: root.controller.selectedControl.length > 0 && !root.controller.pending
                onClicked: root.controller.clearBinding()
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

        AppText {
            Layout.fillWidth: true
            text: !root.controller.connected ? qsTr("Start demo to reconnect.") : root.controller.editing ? (root.controller.thumbLayer && root.controller.selectedControl === "thumb" ? qsTr("This thumb button switches layers. Disable its layer role to assign a shortcut.") : qsTr("Choose a control, then assign a sample shortcut or clear its binding. Changes save without activation.")) : (root.controller.mode === "Active" ? qsTr("Filled controls are held. The result and transitions come from the running demo.") : qsTr("Create or choose a profile, then explicitly Activate or Edit. New profiles have two blank layers."))
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
                ready: root.controller.connected && !root.controller.pending && (root.controller.editing || root.controller.mode === "Active")
                onControlPressed: controlId => {
                    if (root.controller.editing)
                        root.controller.selectControl(controlId);
                    else
                        root.controller.simulate(controlId, holdMode.checked);
                }
            }
            ColumnLayout {
                Layout.preferredWidth: 235
                Layout.alignment: Qt.AlignVCenter
                spacing: 10
                TestPopup {
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
                    text: qsTr("Held output: %1").arg(root.controller.connected ? root.controller.heldOutputLabel : qsTr("None"))
                    color: "#8dd4e8"
                }
                AppText {
                    text: qsTr("SIMULATED TRANSITIONS")
                    font.pixelSize: 11
                    font.letterSpacing: 1
                }
                ScrollView {
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
                text: qsTr("Simulation only. Your physical keypad is untouched.")
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
                enabled: root.controller.connected && !root.controller.pending
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
            width: 300
            text: qsTr("Delete “%1” and all its saved bindings?").arg(root.controller.selectedProfileName)
            wrapMode: Text.Wrap
        }
    }
}
