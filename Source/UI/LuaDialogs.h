#pragma once

namespace Manifold::UI
{
    class DialogHost;

    void InstallLuaDialogs(DialogHost& host);
    void RemoveLuaDialogs();
}
