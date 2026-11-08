#pragma once

#include "core/profile_store.h"
#include "core/remapper.h"
#include "ipc/protocol.h"

class QLocalSocket;

namespace xkeys
{

class DemoSession
{
  public:
    explicit DemoSession(const QString &profilesDir);
    QJsonObject handle(const QLocalSocket &peer, const ipc::Request &request);
    void disconnect(const QLocalSocket &peer);

  private:
    QJsonObject mutate(const ipc::Request &request);
    QJsonObject stateData() const;
    QJsonObject profilesReply(const QString &id) const;
    QJsonObject saveProfile(const QString &id, Profile profile, Revision expected);
    QJsonObject useProfile(const ipc::Request &request);
    QJsonObject input(const ipc::Request &request);

    QString m_profilesDir;
    ProfileStore m_store;
    Remapper m_remapper;
    ipc::Snapshot m_state;
    const QLocalSocket *m_owner = nullptr;
};

} // namespace xkeys
