#pragma once

#include <QString>

namespace xkeys::ipc
{

// Returns an empty path with an explanatory error if the endpoint is unsafe.
QString demoEndpoint(const QString &overridePath, QString &error);
QString demoProfilesDir(const QString &overridePath, const QString &socketOverride, QString &error);
QString checkSocketPath(const QString &path);
bool prepareListener(const QString &path, QString &error);

} // namespace xkeys::ipc
