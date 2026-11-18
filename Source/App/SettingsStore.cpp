#include "App/SettingsStore.h"

namespace Manifold
{
    Status SettingsStore::Flush()
    {
        m_changedAt = -1.0;
        return m_settings.Save(m_path);
    }
}
