#include "App/SettingsStore.h"

namespace Manifold
{
    Status SettingsStore::Flush()
    {
        m_changedAt = -1.0;
        m_last = m_settings.Save(m_path);
        ++m_saves;
        return m_last;
    }
}
