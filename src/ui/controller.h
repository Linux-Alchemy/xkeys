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

  public:
    explicit Controller(QObject *parent = nullptr);
    void startDemo(const QString &socketPath = {});

    bool connected() const;
    bool pending() const;
    QString statusText() const;
    QString errorText() const;
    QString selectedControl() const;
    QString bindingLabel() const;
    int selectedLayer() const;
    bool blank() const;
    QVariantList controls() const;

    Q_INVOKABLE void selectControl(const QString &controlId);
    Q_INVOKABLE void stopDemo();
    Q_INVOKABLE void reconnectDemo();

  signals:
    void connectionChanged();
    void pendingChanged();
    void errorChanged();
    void resolutionChanged();

  private:
    enum class State { Disconnected, Connecting, Handshake, Ready };

    void setState(State state);
    void setError(const QString &error);
    void fail(const QString &error);
    void tryConnect();
    void handleSocketError(QLocalSocket::LocalSocketError error);
    void send(ipc::Operation operation, const QString &control = {});
    void readReplies();
    void clearPending();

    State m_state = State::Disconnected;
    QString m_socketOverride;
    QString m_endpoint;
    QString m_error;
    Resolution m_resolution;
    std::optional<ipc::Request> m_pending;
    bool m_launched = false;
    QElapsedTimer m_startClock;
    QTimer m_retry;
    QTimer m_deadline;
    QLocalSocket m_socket;
};

} // namespace xkeys
