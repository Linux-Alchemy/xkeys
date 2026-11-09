#pragma once

#include <QList>
#include <QString>

namespace xkeys
{

using ControlId = QString;

struct Resolution {
    ControlId control;
    int layer = 1;
    QString label;
    QList<int> chord;
    bool blank = false;
    bool valid = false;
    QString error;
};

struct Control {
    ControlId id;
    QString label;
};

} // namespace xkeys
