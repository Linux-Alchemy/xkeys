#pragma once

#include "types.h"

#include <QJsonObject>

namespace xkeys
{

Profile makeBlankProfile(const ProfileId &id, const QString &name);
Result<void> validateProfile(const Profile &profile);
QJsonObject profileToJson(const Profile &profile);
Result<Profile> profileFromJson(const QJsonObject &object);

class ProfileStore
{
  public:
    explicit ProfileStore(QString root);

    Result<Profile> load(const ProfileId &id) const;
    Result<QList<Profile>> list() const;
    Result<Revision> save(const Profile &profile, Revision expected);
    Result<void> remove(const ProfileId &id, Revision expected);

  private:
    QString profilePath(const ProfileId &id) const;

    QString m_root;
};

} // namespace xkeys
