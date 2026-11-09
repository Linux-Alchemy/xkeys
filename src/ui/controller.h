#pragma once

#include "capture_backend.h"
#include "hyprland_context.h"
#include "ipc/protocol.h"

#include <QElapsedTimer>
#include <QLocalSocket>
#include <QObject>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantList>

#include <optional>

namespace xkeys
{

class Controller : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application")

    Q_PROPERTY(bool connected READ connected NOTIFY connectionChanged)
    Q_PROPERTY(bool pending READ pending NOTIFY pendingChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY connectionChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorChanged)
    Q_PROPERTY(QString selectedControl READ selectedControl NOTIFY resolutionChanged)
    Q_PROPERTY(QString bindingLabel READ bindingLabel NOTIFY resolutionChanged)
    Q_PROPERTY(int selectedLayer READ selectedLayer NOTIFY resolutionChanged)
    Q_PROPERTY(bool blank READ blank NOTIFY resolutionChanged)
    Q_PROPERTY(QVariantList controls READ controls CONSTANT)
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(QString selectedProfileId READ selectedProfileId NOTIFY profileChanged)
    Q_PROPERTY(QString selectedProfileName READ selectedProfileName NOTIFY profileChanged)
    Q_PROPERTY(bool hasProfile READ hasProfile NOTIFY profileChanged)
    Q_PROPERTY(bool thumbLayer READ thumbLayer NOTIFY profileChanged)
    Q_PROPERTY(int editLayer READ editLayer NOTIFY profileChanged)
    Q_PROPERTY(QString bindingActionLabel READ bindingActionLabel NOTIFY profileChanged)
    Q_PROPERTY(QString activeProfileId READ activeProfileId NOTIFY simulationChanged)
    Q_PROPERTY(QString activeProfileName READ activeProfileName NOTIFY simulationChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY simulationChanged)
    Q_PROPERTY(bool editing READ editing NOTIFY simulationChanged)
    Q_PROPERTY(int activeLayer READ activeLayer NOTIFY simulationChanged)
    Q_PROPERTY(QStringList heldControls READ heldControls NOTIFY simulationChanged)
    Q_PROPERTY(QString heldOutputLabel READ heldOutputLabel NOTIFY simulationChanged)
    Q_PROPERTY(QStringList outputEvents READ outputEvents NOTIFY simulationChanged)
    Q_PROPERTY(bool recordingAvailable READ recordingAvailable NOTIFY recordingChanged)
    Q_PROPERTY(
        QString recordingUnavailableReason READ recordingUnavailableReason NOTIFY recordingChanged)
    Q_PROPERTY(QVariantList recordingKeyboards READ recordingKeyboards NOTIFY recordingChanged)
    Q_PROPERTY(QString recordingKeyboardId READ recordingKeyboardId NOTIFY recordingChanged)
    Q_PROPERTY(QString recordingState READ recordingState NOTIFY recordingChanged)
    Q_PROPERTY(QString recordingMessage READ recordingMessage NOTIFY recordingChanged)
    Q_PROPERTY(QString draftChord READ draftChord NOTIFY recordingChanged)
    Q_PROPERTY(bool recordingBusy READ recordingBusy NOTIFY recordingChanged)
    Q_PROPERTY(bool canSaveDraft READ canSaveDraft NOTIFY recordingChanged)
    Q_PROPERTY(QStringList bindingConflicts READ bindingConflicts NOTIFY recordingChanged)
    Q_PROPERTY(bool conflictsChecked READ conflictsChecked NOTIFY recordingChanged)
    Q_PROPERTY(QString conflictCheckNote READ conflictCheckNote NOTIFY recordingChanged)
    Q_PROPERTY(bool conflictAcknowledged READ conflictAcknowledged NOTIFY recordingChanged)
    Q_PROPERTY(QVariantMap controlLabels READ controlLabels NOTIFY recordingChanged)
    Q_PROPERTY(QString draftActionLabel READ draftActionLabel WRITE setDraftActionLabel NOTIFY
                   recordingChanged)
    Q_PROPERTY(int recordingRemainingSeconds READ recordingRemainingSeconds NOTIFY recordingChanged)

  public:
    explicit Controller(QObject *parent = nullptr);
    Controller(std::unique_ptr<CaptureBackend> capture, std::unique_ptr<ShortcutContext> context,
               QObject *parent = nullptr);
    void startDemo(const QString &socketPath = {}, const QString &profilesDir = {});

    bool connected() const;
    bool pending() const;
    QString statusText() const;
    QString errorText() const;
    QString selectedControl() const;
    QString bindingLabel() const;
    int selectedLayer() const;
    bool blank() const;
    QVariantList controls() const;
    QVariantList profiles() const;
    QString selectedProfileId() const;
    QString selectedProfileName() const;
    bool hasProfile() const;
    bool thumbLayer() const;
    int editLayer() const;
    QString bindingActionLabel() const;
    QString activeProfileId() const;
    QString activeProfileName() const;
    QString mode() const;
    bool editing() const;
    int activeLayer() const;
    QStringList heldControls() const;
    QString heldOutputLabel() const;
    QStringList outputEvents() const;
    bool recordingAvailable() const;
    QString recordingUnavailableReason() const;
    QVariantList recordingKeyboards() const;
    QString recordingKeyboardId() const;
    QString recordingState() const;
    QString recordingMessage() const;
    QString draftChord() const;
    bool recordingBusy() const;
    bool canSaveDraft() const;
    QStringList bindingConflicts() const;
    bool conflictsChecked() const;
    QString conflictCheckNote() const;
    bool conflictAcknowledged() const;
    QVariantMap controlLabels() const;
    QString draftActionLabel() const;
    void setDraftActionLabel(const QString &label);
    int recordingRemainingSeconds() const;

    Q_INVOKABLE void refreshRecordingKeyboards();
    Q_INVOKABLE void setRecordingKeyboard(const QString &id);
    Q_INVOKABLE void setWindowActive(bool active);
    Q_INVOKABLE void beginEdit(const QString &controlId, int layer);
    Q_INVOKABLE void recordAgain();
    Q_INVOKABLE void saveDraft();
    Q_INVOKABLE void cancelDraft();
    Q_INVOKABLE void setConflictAcknowledged(bool acknowledged);

    Q_INVOKABLE void createProfile(const QString &name);
    Q_INVOKABLE void selectProfile(const QString &id);
    Q_INVOKABLE void renameSelected(const QString &name);
    Q_INVOKABLE void deleteSelected();
    Q_INVOKABLE void editSelected();
    Q_INVOKABLE void activateSelected();
    Q_INVOKABLE void deactivate();
    Q_INVOKABLE void setEditLayer(int layer);
    Q_INVOKABLE void setThumbLayer(bool enabled);
    // Legacy demo test fixture.
    void assignSample(int preset, const QString &label);
    Q_INVOKABLE void clearBinding();
    Q_INVOKABLE void simulate(const QString &controlId, bool hold);
    Q_INVOKABLE void releaseAll();
    Q_INVOKABLE void selectControl(const QString &controlId);
    Q_INVOKABLE void stopDemo();
    Q_INVOKABLE void reconnectDemo();

  signals:
    void connectionChanged();
    void pendingChanged();
    void errorChanged();
    void resolutionChanged();
    void profilesChanged();
    void profileChanged();
    void simulationChanged();
    void recordingChanged();

  private:
    enum class State { Disconnected, Connecting, Handshake, Ready };
    enum class RecordState { Idle, Preparing, Recording, Stopping, Draft, Saving, Error };

    struct RecordTarget {
        ProfileId profileId;
        Revision revision;
        ControlId control;
    };

    void setState(State state);
    void setError(const QString &error);
    void fail(const QString &error);
    void tryConnect();
    void handleSocketError(QLocalSocket::LocalSocketError error);
    void send(ipc::Operation operation, const QJsonObject &args = {});
    void readReplies();
    bool applyReply(const ipc::Request &request, const QJsonObject &data);
    bool applyProfiles(const QJsonObject &data);
    void clearPending();
    void clearRuntime();
    void updateBinding(const Binding &binding);
    void saveProfile(const Profile &profile);
    Binding editBinding() const;
    QJsonObject selectedVersion() const;
    bool canChange() const;
    bool validRecordTarget() const;
    const KeyboardSelection *selectedKeyboard() const;
    QString savedChordLabel(const QList<int> &codes) const;
    void finishCapture(const QList<int> &codes);
    void captureEnded(const QString &reason, bool failed);
    void discardDraft(RecordState next, const QString &message = {});
    void setRecordState(RecordState state, const QString &message);
    void cancelRecording();
    void clearEditLease();

    std::unique_ptr<CaptureBackend> m_capture;
    std::unique_ptr<ShortcutContext> m_context;
    QList<KeyboardSelection> m_keyboards;
    QString m_recordingKeyboardId;
    QString m_layoutKeyboardId;
    QString m_inventoryError;
    std::optional<RecordTarget> m_recordTarget;
    RecordState m_recordState = RecordState::Idle;
    QString m_recordMessage;
    QString m_draftChord;
    QString m_draftActionLabel;
    QList<int> m_draftCodes;
    QStringList m_conflicts;
    bool m_conflictsChecked = false;
    bool m_conflictAcknowledged = false;
    bool m_captureStarted = false;
    bool m_windowActive = false;
    bool m_editLease = false;
    QElapsedTimer m_captureClock;
    QTimer m_recordingTick;

    State m_state = State::Disconnected;
    QString m_socketOverride;
    QString m_profilesOverride;
    QString m_endpoint;
    QString m_profilesDir;
    QString m_error;
    Resolution m_resolution;
    ipc::Snapshot m_snapshot;
    QVariantList m_profiles;
    std::optional<Profile> m_selected;
    QString m_editControl;
    int m_editLayer = 1;
    std::optional<ipc::Request> m_pending;
    bool m_launched = false;
    QElapsedTimer m_startClock;
    QTimer m_retry;
    QTimer m_deadline;
    QLocalSocket m_socket;
};

} // namespace xkeys
