#pragma once

#include "Disasm/Signature.h"
#include "Scan/Scanner.h"
#include "UI/BackgroundTask.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::UI
{
    class SignatureWindow : public ToolWindowBase
    {
    public:
        explicit SignatureWindow(Workspace& workspace);
        ~SignatureWindow() override;
        void Draw() override;
        bool Busy() const override { return m_task.Running(); }
        void Hide() { m_open = false; }
        // Makes the signature for an address; Copy puts it on the clipboard when
        // it is done without opening the window.
        void Make(std::uint64_t address);
        void Copy(std::uint64_t address);
        void Prepare(std::uint64_t address);
        void MakeWhenReady(const std::string& expression);

    private:
        struct Outcome
        {
            std::optional<Disasm::Signature> Signature;
            std::string Error;
        };

        void Start(std::uint64_t address, bool copy);
        void Collect();
        void Options();
        void Result();
        void Pattern(const Disasm::Signature& signature);
        void Formats(const Disasm::Signature& signature);
        void Instructions(const Disasm::Signature& signature);
        void Candidates(const Disasm::Signature& signature);
        std::string Summary(const Disasm::Signature& signature) const;

        Workspace& m_workspace;
        std::string m_address;
        std::string m_error;
        bool m_copyWhenDone = false;
        bool m_focus = true;
        bool m_pending = false;
        std::shared_ptr<Target::Process> m_process;
        BackgroundTask<Outcome, Scan::PatternProgress> m_task;
        std::optional<Disasm::Signature> m_result;
    };
}
