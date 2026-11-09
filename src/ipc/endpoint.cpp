#include "endpoint.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

#include <cerrno>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace xkeys::ipc
{

namespace
{
bool isPrivateDirectory(const QString &path)
{
    const QFileInfo info(path);
    constexpr auto sharedPermissions = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                                       QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    return info.isDir() && info.canonicalFilePath() == path && info.ownerId() == geteuid() &&
           !(info.permissions() & sharedPermissions);
}
} // namespace

QString demoEndpoint(const QString &overridePath, QString &error)
{
    QString path = overridePath;
    if (path.isEmpty()) {
        const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (!isPrivateDirectory(runtime)) {
            error = QStringLiteral("A private runtime directory is required for the demo.");
            return {};
        }

        const QDir dir(runtime);
        const QString demoDir = dir.filePath(QStringLiteral("xkeys-demo"));
        if (!QFileInfo::exists(demoDir) &&
            !dir.mkdir(QStringLiteral("xkeys-demo"),
                       QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner) &&
            !QFileInfo::exists(demoDir)) {
            error = QStringLiteral("Cannot create the demo runtime directory.");
            return {};
        }
        path = QDir(demoDir).filePath(QStringLiteral("service.sock"));
    }

    if (!QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path ||
        path.contains(QChar::Null) ||
        QFile::encodeName(path).size() >= static_cast<qsizetype>(sizeof(sockaddr_un::sun_path))) {
        error = QStringLiteral("The demo socket needs a short, clean absolute path.");
        return {};
    }

    if (!isPrivateDirectory(QFileInfo(path).absolutePath())) {
        error = QStringLiteral("The demo socket directory must be private and owned by this user.");
        return {};
    }

    error = checkSocketPath(path);
    return error.isEmpty() ? path : QString{};
}

QString checkSocketPath(const QString &path)
{
    const QByteArray name = QFile::encodeName(path);
    struct stat metadata{};
    if (lstat(name.constData(), &metadata) != 0) {
        return errno == ENOENT ? QString{} : QStringLiteral("Cannot inspect the demo socket path.");
    }

    if (!S_ISSOCK(metadata.st_mode) || metadata.st_uid != geteuid() ||
        (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return QStringLiteral("The demo socket path is occupied by an unsafe file.");
    }

    return {};
}

bool prepareListener(const QString &path, QString &error)
{
    error = checkSocketPath(path);
    if (!error.isEmpty()) {
        return false;
    }
    if (!QFileInfo::exists(path)) {
        return true;
    }

    // The caller holds the service lock. Probe an existing socket before removing it.
    QLocalSocket probe;
    probe.connectToServer(path);
    if (probe.waitForConnected(150) || probe.error() != QLocalSocket::ConnectionRefusedError) {
        error = QStringLiteral("The demo socket is already in use.");
        return false;
    }

    if (!QLocalServer::removeServer(path)) {
        error = QStringLiteral("Cannot remove the stale demo socket.");
        return false;
    }
    return true;
}

} // namespace xkeys::ipc
