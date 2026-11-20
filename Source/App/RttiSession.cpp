#include "App/RttiSession.h"

#include "App/Settings.h"

namespace Manifold
{
    RttiSession::RttiSession(TargetSession& target, Settings& settings) : m_target(target), m_settings(settings)
    {
        m_target.AddExtraSymbols([this](std::string_view name) { return ResolveSymbol(name); });
    }

    bool RttiSession::Enabled() const
    {
        return m_settings.StructureRtti;
    }

    void RttiSession::SetEnabled(bool enabled)
    {
        m_settings.StructureRtti = enabled;
    }

    std::shared_ptr<Target::RttiRuntime> RttiSession::Runtime()
    {
        const std::shared_ptr<Target::Process> process = m_target.Process();
        std::lock_guard lock(m_lock);
        if (!process)
        {
            Drop();
            return nullptr;
        }
        if (m_runtime && m_generation == m_target.Generation() && m_runtime->ProcessHandle() == process)
            return m_runtime;
        Drop();
        m_generation = m_target.Generation();
        m_alive = std::make_shared<std::atomic<bool>>(true);
        Target::Symbols* symbols = &m_target.Symbols();
        m_runtime = std::make_shared<Target::RttiRuntime>(process, [symbols, alive = m_alive](const std::string& name, std::uint64_t table)
            {
                if (alive->load())
                    symbols->Register(name, table, kSymbolOwner);
            });
        return m_runtime;
    }

    void RttiSession::Drop()
    {
        if (m_alive)
            m_alive->store(false);
        m_alive.reset();
        m_runtime.reset();
    }

    void RttiSession::ProcessOpened()
    {
        std::lock_guard lock(m_lock);
        Drop();
    }

    void RttiSession::ProcessClosed()
    {
        std::lock_guard lock(m_lock);
        Drop();
    }

    void RttiSession::Reset()
    {
        {
            std::lock_guard lock(m_lock);
            Drop();
        }
        m_target.Symbols().UnregisterOwner(kSymbolOwner);
    }

    std::string RttiSession::StatusText()
    {
        const auto runtime = Runtime();
        return runtime ? runtime->StatusText() : std::string("no process");
    }

    bool RttiSession::Names(std::uint64_t address)
    {
        const auto runtime = Runtime();
        if (!runtime)
            return false;
        return runtime->ObjectAt(address, true).has_value() || runtime->TableClass(address, true) != nullptr;
    }

    std::optional<std::uint64_t> RttiSession::ResolveSymbol(std::string_view name)
    {
        if (name.find("::vftable") == std::string_view::npos)
            return std::nullopt;
        const auto runtime = Runtime();
        if (!runtime)
            return std::nullopt;
        return runtime->ResolveSymbol(name);
    }
}
