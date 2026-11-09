#pragma once

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
    Q_PROPERTY(QStringList sampleShortcuts READ sampleShortcuts CONSTANT)

  public:
    explicit Controller(QObject *parent = nullptr);
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
    QStringList sampleShortcuts() const;

    Q_INVOKABLE void createProfile(const QString &name);
    Q_INVOKABLE void selectProfile(const QString &id);
    Q_INVOKABLE void renameSelected(const QString &name);
    Q_INVOKABLE void deleteSelected();
    Q_INVOKABLE void editSelected();
    Q_INVOKABLE void activateSelected();
    Q_INVOKABLE void deactivate();
    Q_INVOKABLE void setEditLayer(int layer);
    Q_INVOKABLE void setThumbLayer(bool enabled);
    Q_INVOKABLE void assignSample(int preset, const QString &label);
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

  private:
    enum class State { Disconnected, Connecting, Handshake, Ready };

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
