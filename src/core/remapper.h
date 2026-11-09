#pragma once

#include "types.h"

#include <QStringList>

namespace xkeys
{

const QList<Control> &demoControls();
Resolution resolveDemo(ControlId control);
QString chordLabel(const QList<int> &chord);

class Remapper
{
  public:
    Resolution consume(const ControlEvent &event);
    OutputBatch reset();
    Result<void> activate(const Profile &profile);
    bool isActive() const;
    int activeLayer() const;
    QStringList heldControls() const;
    QList<int> ownedKeys() const;

  private:
    void pressChord(const QList<int> &chord, OutputBatch &output);
    void releaseChord(const QList<int> &chord, OutputBatch &output);

    std::optional<Profile> m_profile;
    QMap<ControlId, Resolution> m_held;
    QMap<int, int> m_owners;
    QList<int> m_ownedKeys;
    int m_layer = 1;
};

} // namespace xkeys
