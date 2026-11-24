#include "Target/Speedhack.h"

#include "Target/AgentLife.h"

#include "Core/Handle.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Disasm/Disassembler.h"
#include "Target/SpeedhackProtocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <thread>

#include <windows.h>

namespace Manifold::Target
{
    namespace
    {
        constexpr std::size_t kPageSize = 0x1000;
        constexpr std::size_t kRelaySize = 16;
        constexpr std::size_t kTrampolineStart = Speed::SourceCount * kRelaySize;
        constexpr std::size_t kTrampolineSize = 48;
        constexpr std::uint64_t kReach = 0x7FF00000ull - 0x20000ull;
        constexpr std::size_t kPatch = Speed::kPatchBytes;
        constexpr int kInstallTries = 200;
        constexpr int kDrainTries = 150;
        constexpr int kSettleMs = 500;

        struct Candidate
        {
            const char* Module;
            const char* Export;
            Speed::Source Source;
        };

        constexpr Candidate kCandidates[] = {
            { "ntdll.dll", "RtlQueryPerformanceCounter", Speed::SourceCounter },
            { "kernel32.dll", "GetTickCount", Speed::SourceTick },
            { "kernelbase.dll", "GetTickCount", Speed::SourceTick },
            { "kernel32.dll", "GetTickCount64", Speed::SourceTick64 },
            { "kernelbase.dll", "GetTickCount64", Speed::SourceTick64 },
            { "winmm.dll", "timeGetTime", Speed::SourceMultimedia },
        };

        struct Range
        {
            std::uint64_t Start = 0;
            std::uint64_t End = 0;
        };

        struct Plan
        {
            SpeedhackHook Hook;
            std::uint64_t Trampoline = 0;
            std::vector<std::uint8_t> Code;
            std::size_t Stolen = 0;
        };

        bool NeedsOriginal(std::uint32_t source)
        {
            return source == Speed::SourceCounter || source == Speed::SourceMultimedia;
        }

        std::wstring AgentFile(bool is64Bit)
        {
            return is64Bit ? L"ManifoldAgent.dll" : L"ManifoldAgent32.dll";
        }

        std::uint64_t Distance(std::uint64_t a, std::uint64_t b)
        {
            return a > b ? a - b : b - a;
        }

        bool FitsRel32(std::int64_t value)
        {
            return value >= INT32_MIN && value <= INT32_MAX;
        }

        void PushRel32(std::vector<std::uint8_t>& out, std::int64_t value)
        {
            const std::int32_t rel = static_cast<std::int32_t>(value);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&rel);
            out.insert(out.end(), bytes, bytes + 4);
        }

        std::vector<std::uint8_t> JumpRel(std::uint64_t from, std::uint64_t to)
        {
            std::vector<std::uint8_t> code{ 0xE9 };
            PushRel32(code, static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from + 5));
            return code;
        }

        std::uint64_t FollowStubs(const Process& process, const Disasm::Disassembler& disassembler, std::uint64_t address)
        {
            for (int depth = 0; depth < 4 && address; ++depth)
            {
                const Disasm::Instruction first = disassembler.Read(process, address);
                if (!first.Valid || first.Kind != Disasm::Flow::Jump || !first.Memory.Present || !first.Memory.Absolute)
                    break;
                const auto pointer = process.ReadPointer(*first.Memory.Absolute);
                if (!pointer || *pointer == 0)
                    break;
                address = *pointer;
            }
            return address;
        }

        bool Relocate(const Disasm::Instruction& instruction, std::uint64_t to, std::vector<std::uint8_t>& out)
        {
            const std::uint8_t* bytes = instruction.Bytes.data();
            const std::size_t length = instruction.Length;
            if (instruction.Memory.Present && instruction.Memory.RipRelative)
            {
                if (!instruction.Memory.Absolute || length < 5)
                    return false;
                const std::int32_t old = static_cast<std::int32_t>(static_cast<std::int64_t>(*instruction.Memory.Absolute) - static_cast<std::int64_t>(instruction.Next()));
                std::size_t at = 0;
                int found = 0;
                for (std::size_t i = 1; i + 4 <= length; ++i)
                {
                    if (std::memcmp(bytes + i, &old, 4) == 0)
                    {
                        at = i;
                        ++found;
                    }
                }
                const std::int64_t moved = static_cast<std::int64_t>(*instruction.Memory.Absolute) - static_cast<std::int64_t>(to + length);
                if (found != 1 || !FitsRel32(moved))
                    return false;
                const std::size_t start = out.size();
                out.insert(out.end(), bytes, bytes + length);
                const std::int32_t rel = static_cast<std::int32_t>(moved);
                std::memcpy(out.data() + start + at, &rel, 4);
                return true;
            }
            if (instruction.Target && !instruction.Memory.Present)
            {
                const std::int64_t target = static_cast<std::int64_t>(*instruction.Target);
                const std::int64_t here = static_cast<std::int64_t>(to);
                std::size_t head = 0;
                if (bytes[0] == 0xE9 || bytes[0] == 0xEB || bytes[0] == 0xE8)
                {
                    out.push_back(bytes[0] == 0xE8 ? 0xE8 : 0xE9);
                    head = 1;
                }
                else if ((bytes[0] >= 0x70 && bytes[0] <= 0x7F) || (bytes[0] == 0x0F && bytes[1] >= 0x80 && bytes[1] <= 0x8F))
                {
                    out.push_back(0x0F);
                    out.push_back(bytes[0] == 0x0F ? bytes[1] : static_cast<std::uint8_t>(0x80 | (bytes[0] & 0x0F)));
                    head = 2;
                }
                else
                {
                    return false;
                }
                const std::int64_t rel = target - (here + static_cast<std::int64_t>(head) + 4);
                if (!FitsRel32(rel))
                    return false;
                PushRel32(out, rel);
                return true;
            }
            out.insert(out.end(), bytes, bytes + length);
            return true;
        }

        using NextThreadCall = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);

        std::vector<UniqueHandle> ThreadsOf(HANDLE process)
        {
            static const NextThreadCall next = []()
                {
                    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
                    return ntdll ? reinterpret_cast<NextThreadCall>(reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtGetNextThread"))) : nullptr;
                }();
            std::vector<UniqueHandle> threads;
            if (!next)
                return threads;
            HANDLE current = nullptr;
            for (int guard = 0; guard < 65536; ++guard)
            {
                HANDLE found = nullptr;
                if (next(process, current, THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, 0, 0, &found) < 0 || !found)
                    break;
                threads.emplace_back(found);
                current = found;
            }
            return threads;
        }

        std::optional<std::uint64_t> InstructionPointer(HANDLE thread, bool is64Bit)
        {
            if (is64Bit)
            {
                alignas(16) CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                if (!::GetThreadContext(thread, &context))
                    return std::nullopt;
                return context.Rip;
            }
            WOW64_CONTEXT context{};
            context.ContextFlags = WOW64_CONTEXT_CONTROL;
            if (!::Wow64GetThreadContext(thread, &context))
                return std::nullopt;
            return context.Eip;
        }

        bool ThreadInside(const Process& process, std::span<const Range> ranges)
        {
            for (const UniqueHandle& thread : ThreadsOf(process.Handle()))
            {
                const auto at = InstructionPointer(thread.Get(), process.Is64Bit());
                if (!at)
                    continue;
                for (const Range& range : ranges)
                {
                    if (*at >= range.Start && *at < range.End)
                        return true;
                }
            }
            return false;
        }

        struct Frozen
        {
            const Process& Target;
            bool Held = false;

            explicit Frozen(const Process& process) : Target(process), Held(process.Suspend()) {}
            ~Frozen() { Thaw(); }
            Frozen(const Frozen&) = delete;
            Frozen& operator=(const Frozen&) = delete;

            void Thaw()
            {
                if (Held)
                    Target.Resume();
                Held = false;
            }

            bool Freeze()
            {
                if (!Held)
                    Held = Target.Suspend();
                return Held;
            }
        };

        Status Failed(std::string_view detail)
        {
            return Status::Fail(ErrorCode::SpeedhackFailed, detail);
        }

        template <class T>
        Status WriteField(const Process& process, std::uint64_t block, std::size_t offset, const T& value)
        {
            return process.Write(block + offset, &value, sizeof(T));
        }

        std::optional<Plan> Prepare(const Process& process, const Disasm::Disassembler& disassembler, std::uint64_t address, std::uint32_t source, std::uint64_t page,
            std::size_t slot, std::string& why)
        {
            std::uint8_t bytes[256] = {};
            const std::size_t got = process.Read(address, bytes, sizeof(bytes));
            if (got < 32)
            {
                why = "its code cannot be read";
                return std::nullopt;
            }
            Plan plan;
            plan.Hook.Address = address;
            plan.Hook.Page = page;
            plan.Hook.Source = source;
            plan.Hook.Original.assign(bytes, bytes + kPatch);
            plan.Hook.Patch = JumpRel(address, page + source * kRelaySize);
            const std::uint64_t trampoline = page + kTrampolineStart + slot * kTrampolineSize;
            std::size_t offset = 0;
            while (offset < kPatch)
            {
                const Disasm::Instruction instruction = disassembler.Decode(address + offset, bytes + offset, got - offset);
                if (!instruction.Valid || instruction.Length == 0)
                {
                    why = "its first instructions cannot be decoded";
                    return std::nullopt;
                }
                const std::size_t end = offset + instruction.Length;
                if ((instruction.Kind == Disasm::Flow::Call && end < kPatch) || ((instruction.Kind == Disasm::Flow::Return || instruction.Kind == Disasm::Flow::Interrupt) && end < kPatch))
                {
                    why = "it is too short to hook";
                    return std::nullopt;
                }
                if (NeedsOriginal(source) && !Relocate(instruction, trampoline + plan.Code.size(), plan.Code))
                {
                    why = "its first instructions cannot be moved (" + instruction.Text() + ")";
                    return std::nullopt;
                }
                offset = end;
            }
            plan.Stolen = offset;
            while (offset + 16 <= got)
            {
                const Disasm::Instruction instruction = disassembler.Decode(address + offset, bytes + offset, got - offset);
                if (!instruction.Valid || instruction.Length == 0)
                    break;
                if (instruction.Target && *instruction.Target > address && *instruction.Target < address + plan.Stolen)
                {
                    why = "a branch leads back into its first instructions";
                    return std::nullopt;
                }
                offset += instruction.Length;
            }
            if (NeedsOriginal(source))
            {
                const std::vector<std::uint8_t> back = JumpRel(trampoline + plan.Code.size(), address + plan.Stolen);
                plan.Code.insert(plan.Code.end(), back.begin(), back.end());
                if (plan.Code.size() > kTrampolineSize)
                {
                    why = "its first instructions do not fit the trampoline";
                    return std::nullopt;
                }
                plan.Trampoline = trampoline;
            }
            return plan;
        }
    }

    Speedhack::~Speedhack() = default;

    std::string Speedhack::Text(double speed)
    {
        return Format("%.4g", speed);
    }

    std::string Speedhack::AgentName() const
    {
        return WideToUtf8(AgentFile(!m_process || m_process->Is64Bit()));
    }

    bool Speedhack::Usable() const
    {
        return m_process && m_process->Alive();
    }

    bool Speedhack::Enabled() const
    {
        std::lock_guard lock(m_lock);
        return m_enabled;
    }

    bool Speedhack::Installed() const
    {
        std::lock_guard lock(m_lock);
        return m_installed;
    }

    bool Speedhack::ClockShifted() const
    {
        std::lock_guard lock(m_lock);
        return m_installed && m_shifted;
    }

    double Speedhack::Speed() const
    {
        std::lock_guard lock(m_lock);
        return m_enabled ? m_speed : 1.0;
    }

    std::vector<SpeedhackHook> Speedhack::Hooks() const
    {
        std::lock_guard lock(m_lock);
        return m_hooks;
    }

    bool Speedhack::Owns(std::uint64_t address, std::uint64_t size) const
    {
        std::lock_guard lock(m_lock);
        for (const SpeedhackHook& hook : m_hooks)
        {
            if (address < hook.Address + kPatch && hook.Address < address + std::max<std::uint64_t>(size, 1))
                return true;
        }
        return false;
    }

    void Speedhack::Forget()
    {
        m_process.reset();
        m_block = 0;
        m_call = 0;
        m_imageBase = 0;
        m_imageSize = 0;
        m_hooks.clear();
        m_pages.clear();
        m_serial = 0;
        m_speed = 1.0;
        m_installed = false;
        m_enabled = false;
        m_shifted = false;
    }

    void Speedhack::Attach(std::shared_ptr<Process> process)
    {
        std::lock_guard lock(m_lock);
        Forget();
        m_process = std::move(process);
        if (!Usable() || Process::DryRun())
            return;
        if (Adopt())
        {
            LOG_INFO("Speedhack", Format("%s still has the speedhack of an earlier session (%d hooks, speed %s)", m_process->Name().c_str(), static_cast<int>(m_hooks.size()),
                Text(m_speed).c_str()));
        }
    }

    void Speedhack::Leave()
    {
        std::lock_guard lock(m_lock);
        if (Usable() && m_installed && !Process::DryRun())
        {
            bool shifted = true;
            if (WriteSpeed(1.0) && Settle(shifted))
            {
                if (!shifted)
                    RemoveLocked();
                else
                    LOG_INFO("Speedhack", "The speedhack hooks stay in " + m_process->Name() + " at normal speed, so its clocks do not jump");
            }
        }
        Forget();
    }

    Status Speedhack::Call(std::uint32_t operation)
    {
        if (!m_call)
            return Failed("the agent is not loaded");
        return m_process->CreateThread(m_call, operation, true, 5000);
    }

    // CallApply does nothing while a thread of the process is in the middle of
    // its own Apply, and that one may still apply the serial before the last.
    // So the agent's Shifted only counts once Applied shows the serial written
    // last; until then (bounded) the clocks count as shifted, so the hooks stay
    // and the time cannot jump back.
    Status Speedhack::Settle(bool& shifted)
    {
        shifted = true;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSettleMs);
        for (int round = 0;; ++round)
        {
            if (round % 10 == 0)
            {
                if (Status called = Call(Speed::CallApply); !called)
                    return called;
            }
            std::uint32_t applied = 0;
            if (m_process->ReadExact(m_block + offsetof(Speed::Block, Applied), &applied, sizeof(applied)) && applied == m_serial)
            {
                std::uint32_t value = 1;
                if (m_process->ReadExact(m_block + offsetof(Speed::Block, Shifted), &value, sizeof(value)))
                    shifted = value != 0;
                return Status::Success();
            }
            if (std::chrono::steady_clock::now() >= until)
            {
                LOG_INFO("Speedhack", m_process->Name() + " did not take the new speed in time; its clocks count as shifted, so the hooks stay");
                return Status::Success();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    Status Speedhack::Watch()
    {
        const UniqueHandle duplicator(::OpenProcess(PROCESS_DUP_HANDLE, FALSE, Raw(m_process->Id())));
        HANDLE remote = nullptr;
        if (!duplicator || !::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(), duplicator.Get(), &remote, SYNCHRONIZE, FALSE, 0))
            return Failed("the process cannot watch Manifold Engine (" + SystemErrorText(::GetLastError()) + ")");
        const std::uint64_t value = reinterpret_cast<std::uintptr_t>(remote);
        if (Status written = WriteField(*m_process, m_block, offsetof(Speed::Block, HostHandle), value); !written)
            return written;
        return Call(Speed::CallWatch);
    }

    bool Speedhack::Adopt()
    {
        const std::string name = AgentName();
        if (!m_process->FindModule(name))
            return false;
        const std::uint64_t block = m_process->FindExport(name, "ManifoldSpeed");
        const std::uint64_t call = m_process->FindExport(name, "ManifoldSpeedCall");
        auto shared = std::make_unique<Speed::Block>();
        if (!block || !call || !m_process->ReadExact(block, shared.get(), sizeof(Speed::Block)))
            return false;
        if (shared->Magic != Speed::kMagic || shared->Version != Speed::kVersion || shared->HookCount == 0 || shared->HookCount > Speed::kMaxHooks)
            return false;
        m_block = block;
        m_call = call;
        m_imageBase = shared->ImageBase;
        m_imageSize = shared->ImageSize;
        std::copy(std::begin(shared->Entries), std::end(shared->Entries), std::begin(m_entries));
        for (std::uint32_t i = 0; i < shared->HookCount; ++i)
        {
            const Speed::Hook& record = shared->Hooks[i];
            if (record.Source >= Speed::SourceCount || !record.Target || !record.Page)
                continue;
            SpeedhackHook hook;
            hook.Address = record.Target;
            hook.Page = record.Page;
            hook.Source = record.Source;
            hook.Name = "time source " + std::to_string(record.Source);
            hook.Original.assign(record.Original, record.Original + kPatch);
            hook.Patch = JumpRel(hook.Address, hook.Page + hook.Source * kRelaySize);
            m_hooks.push_back(std::move(hook));
            if (std::find(m_pages.begin(), m_pages.end(), record.Page) == m_pages.end())
                m_pages.push_back(record.Page);
        }
        m_serial = shared->Serial;
        m_speed = shared->Speed;
        m_shifted = shared->Shifted != 0;
        m_installed = true;
        m_enabled = m_speed != 1.0;
        if (Status watched = Watch(); !watched)
            LOG_WARN("Speedhack", watched.Text);
        return true;
    }

    Status Speedhack::HookMissing()
    {
        const bool is64Bit = m_process->Is64Bit();
        const Disasm::Disassembler disassembler(is64Bit);
        std::vector<Plan> plans;
        std::vector<std::uint64_t> newPages;
        const auto taken = [&](std::uint64_t address)
            {
                return std::any_of(m_hooks.begin(), m_hooks.end(), [&](const SpeedhackHook& hook) { return hook.Address == address; }) ||
                    std::any_of(plans.begin(), plans.end(), [&](const Plan& plan) { return plan.Hook.Address == address; });
            };
        const auto sourceTaken = [&](std::uint32_t source)
            {
                return std::any_of(m_hooks.begin(), m_hooks.end(), [&](const SpeedhackHook& hook) { return hook.Source == source; }) ||
                    std::any_of(plans.begin(), plans.end(), [&](const Plan& plan) { return plan.Hook.Source == source; });
            };
        const auto slotsOn = [&](std::uint64_t page)
            {
                return static_cast<std::size_t>(std::count_if(m_hooks.begin(), m_hooks.end(), [&](const SpeedhackHook& hook) { return hook.Page == page; }) +
                    std::count_if(plans.begin(), plans.end(), [&](const Plan& plan) { return plan.Hook.Page == page; }));
            };
        const auto pageFor = [&](std::uint64_t address) -> std::uint64_t
            {
                for (const std::vector<std::uint64_t>* list : { &m_pages, &newPages })
                {
                    for (std::uint64_t page : *list)
                    {
                        if (!is64Bit || Distance(page, address) < kReach)
                            return page;
                    }
                }
                const Result<Address> allocated = m_process->Allocate(kPageSize, PAGE_EXECUTE_READWRITE, address);
                if (!allocated)
                    return 0;
                const std::uint64_t page = *allocated;
                if (is64Bit && Distance(page, address) >= kReach)
                {
                    m_process->Free(page);
                    return 0;
                }
                std::vector<std::uint8_t> relays(kTrampolineStart, 0xCC);
                for (std::uint32_t source = 0; source < Speed::SourceCount; ++source)
                {
                    std::uint8_t* relay = relays.data() + source * kRelaySize;
                    if (is64Bit)
                    {
                        const std::uint8_t jump[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
                        std::memcpy(relay, jump, sizeof(jump));
                        std::memcpy(relay + sizeof(jump), &m_entries[source], 8);
                    }
                    else
                    {
                        const std::vector<std::uint8_t> jump = JumpRel(page + source * kRelaySize, m_entries[source]);
                        std::memcpy(relay, jump.data(), jump.size());
                    }
                }
                if (!m_process->Write(page, relays.data(), relays.size()))
                {
                    m_process->Free(page);
                    return 0;
                }
                newPages.push_back(page);
                return page;
            };

        for (const Candidate& candidate : kCandidates)
        {
            if (m_hooks.size() + plans.size() >= Speed::kMaxHooks || !m_process->FindModule(candidate.Module))
                continue;
            const std::uint64_t exported = m_process->FindExport(candidate.Module, candidate.Export);
            const std::uint64_t address = exported ? FollowStubs(*m_process, disassembler, exported) : 0;
            if (!address || taken(address) || (NeedsOriginal(candidate.Source) && sourceTaken(candidate.Source)))
                continue;
            const std::string name = std::string(candidate.Module) + "!" + candidate.Export;
            const std::uint64_t page = pageFor(address);
            if (!page)
            {
                LOG_WARN("Speedhack", name + " stays as it is: no memory could be reserved near it");
                continue;
            }
            std::string why;
            std::optional<Plan> plan = Prepare(*m_process, disassembler, address, candidate.Source, page, slotsOn(page), why);
            if (!plan)
            {
                LOG_WARN("Speedhack", name + " stays as it is: " + why);
                continue;
            }
            plan->Hook.Name = name;
            plans.push_back(std::move(*plan));
        }
        const auto dropNewPages = [&]()
            {
                for (std::uint64_t page : newPages)
                    m_process->Free(page);
            };
        if (plans.empty())
        {
            dropNewPages();
            return Status::Success();
        }

        // Originals stays as it is until the process is paused and the hook is
        // written: an entry that pointed into a page freed on a failure below
        // would make the next Apply call into freed memory.
        std::uint32_t count = static_cast<std::uint32_t>(m_hooks.size());
        for (const Plan& plan : plans)
        {
            Status written = Status::Success();
            if (plan.Trampoline)
                written = m_process->Write(plan.Trampoline, plan.Code.data(), plan.Code.size());
            Speed::Hook record{};
            record.Target = plan.Hook.Address;
            record.Page = plan.Hook.Page;
            record.Source = plan.Hook.Source;
            std::memcpy(record.Original, plan.Hook.Original.data(), kPatch);
            if (written)
                written = WriteField(*m_process, m_block, offsetof(Speed::Block, Hooks) + count * sizeof(Speed::Hook), record);
            if (!written)
            {
                dropNewPages();
                return written;
            }
            ++count;
        }

        // Every Apply calls the timeGetTime trampoline, from all hooks, so
        // winmm must never unload under it: the agent pins the module first.
        // The multimedia plan is the last one (winmm is the last candidate),
        // so leaving it out leaves its record past the hook count.
        if (plans.back().Hook.Source == Speed::SourceMultimedia)
        {
            const std::size_t at = offsetof(Speed::Block, Hooks) + (count - 1) * sizeof(Speed::Hook) + offsetof(Speed::Hook, Pinned);
            std::uint32_t pinned = 0;
            const Status called = Call(Speed::CallPin);
            if (!called || !m_process->ReadExact(m_block + at, &pinned, sizeof(pinned)) || !pinned)
            {
                LOG_WARN("Speedhack", plans.back().Hook.Name + " stays as it is: its module could not be kept loaded" + (called ? std::string() : " (" + called.Text + ")"));
                plans.pop_back();
                --count;
                if (plans.empty())
                {
                    dropNewPages();
                    return Status::Success();
                }
            }
        }

        std::vector<Range> busy;
        for (const Plan& plan : plans)
            busy.push_back({ plan.Hook.Address + 1, plan.Hook.Address + plan.Stolen });
        Status result = Status::Success();
        std::vector<std::uint64_t> referenced;
        {
            Frozen frozen(*m_process);
            bool clear = false;
            for (int attempt = 0; attempt < kInstallTries && frozen.Held && !clear; ++attempt)
            {
                clear = !ThreadInside(*m_process, busy);
                if (!clear)
                {
                    frozen.Thaw();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    frozen.Freeze();
                }
            }
            if (!frozen.Held)
                result = Failed("the process could not be paused");
            else if (!clear)
                result = Failed("a thread of the process stays inside a time function");
            else if (Status counted = WriteField(*m_process, m_block, offsetof(Speed::Block, HookCount), count); !counted)
                result = counted;
            for (std::size_t i = 0; i < plans.size() && result; ++i)
            {
                const Plan& plan = plans[i];
                const std::size_t original = offsetof(Speed::Block, Originals) + plan.Hook.Source * sizeof(std::uint64_t);
                if (plan.Trampoline)
                    result = WriteField(*m_process, m_block, original, plan.Trampoline);
                if (result)
                    result = m_process->Write(plan.Hook.Address, plan.Hook.Patch.data(), plan.Hook.Patch.size());
                if (result)
                    m_hooks.push_back(plan.Hook);
                else if (plan.Trampoline && !WriteField(*m_process, m_block, original, std::uint64_t{ 0 }))
                    referenced.push_back(plan.Hook.Page);
            }
        }
        for (std::uint64_t page : newPages)
        {
            if (std::any_of(m_hooks.begin(), m_hooks.end(), [&](const SpeedhackHook& hook) { return hook.Page == page; }) ||
                std::find(referenced.begin(), referenced.end(), page) != referenced.end())
                m_pages.push_back(page);
            else
                m_process->Free(page);
        }
        const std::uint32_t installed = static_cast<std::uint32_t>(m_hooks.size());
        if (installed != count)
            WriteField(*m_process, m_block, offsetof(Speed::Block, HookCount), installed);
        return result;
    }

    Status Speedhack::Install()
    {
        const std::string name = AgentName();
        if (Status loaded = LoadAgent(*m_process); !loaded)
            return loaded;
        m_block = m_process->FindExport(name, "ManifoldSpeed");
        m_call = m_process->FindExport(name, "ManifoldSpeedCall");
        auto shared = std::make_unique<Speed::Block>();
        if (!m_block || !m_call || !m_process->ReadExact(m_block, shared.get(), sizeof(Speed::Block)) || shared->Magic != Speed::kMagic || shared->Version != Speed::kVersion)
        {
            m_block = 0;
            m_call = 0;
            return Failed("the agent library in the process is from another version of the program; restart the game to load the new one");
        }
        m_imageBase = shared->ImageBase;
        m_imageSize = shared->ImageSize;
        std::copy(std::begin(shared->Entries), std::end(shared->Entries), std::begin(m_entries));
        m_serial = shared->Serial;
        m_hooks.clear();
        m_pages.clear();
        m_installed = true;
        Status hooked = HookMissing();
        const bool counter = std::any_of(m_hooks.begin(), m_hooks.end(), [](const SpeedhackHook& hook) { return hook.Source == Speed::SourceCounter; });
        if (!hooked || !counter)
        {
            RemoveLocked();
            return hooked ? Failed("the performance counter of the process could not be hooked") : hooked;
        }
        if (Status watched = Watch(); !watched)
            LOG_WARN("Speedhack", watched.Text + ". If Manifold Engine ends unexpectedly, the process keeps the speed that was set.");
        std::string names;
        for (const SpeedhackHook& hook : m_hooks)
            names += (names.empty() ? "" : ", ") + hook.Name;
        LOG_SUCCESS("Speedhack", "Speedhack ready in " + m_process->Name() + " (" + names + ")");
        return Status::Success();
    }

    Status Speedhack::WriteSpeed(double speed)
    {
        if (Status written = WriteField(*m_process, m_block, offsetof(Speed::Block, Wanted), speed); !written)
            return written;
        std::uint32_t serial = m_serial;
        m_process->ReadExact(m_block + offsetof(Speed::Block, Serial), &serial, sizeof(serial));
        serial += 1;
        if (Status written = WriteField(*m_process, m_block, offsetof(Speed::Block, Serial), serial); !written)
            return written;
        m_serial = serial;
        return Status::Success();
    }

    Status Speedhack::SetSpeed(double speed)
    {
        std::lock_guard lock(m_lock);
        if (!m_process)
            return Status::Fail(ErrorCode::NoProcess);
        if (std::isnan(speed))
            return Failed("the speed is not a number");
        speed = std::clamp(speed, 0.0, kMaxSpeed);
        if (Process::DryRun())
        {
            m_speed = speed;
            m_enabled = true;
            return Status::Success();
        }
        if (!m_process->Alive())
            return Status::Fail(ErrorCode::ProcessExited);
        if (!m_installed)
        {
            if (Status installed = Install(); !installed)
                return installed;
        }
        else if (m_process->FindModule("winmm.dll") && std::none_of(m_hooks.begin(), m_hooks.end(), [](const SpeedhackHook& hook) { return hook.Source == Speed::SourceMultimedia; }))
        {
            if (Status hooked = HookMissing(); !hooked)
                LOG_WARN("Speedhack", hooked.Text);
        }
        if (Status written = WriteSpeed(speed); !written)
            return written;
        if (!m_enabled || m_speed != speed)
            LOG_INFO("Speedhack", "Speed " + Text(speed) + " in " + m_process->Name());
        m_speed = speed;
        m_enabled = true;
        m_shifted = m_shifted || speed != 1.0;
        return Status::Success();
    }

    Status Speedhack::Disable()
    {
        std::lock_guard lock(m_lock);
        const bool was = m_enabled;
        m_enabled = false;
        m_speed = 1.0;
        if (!m_installed || !m_process || Process::DryRun())
            return Status::Success();
        if (!m_process->Alive())
        {
            const std::shared_ptr<Process> process = m_process;
            Forget();
            m_process = process;
            return Status::Success();
        }
        if (Status written = WriteSpeed(1.0); !written)
            return written;
        bool shifted = true;
        if (Status applied = Settle(shifted); !applied)
            return applied;
        m_shifted = shifted;
        if (!m_shifted)
            return RemoveLocked();
        if (was)
            LOG_INFO("Speedhack", "Normal speed in " + m_process->Name() + ". The hooks stay, so its clocks do not jump back; Remove Speedhack Hooks takes them out.");
        return Status::Success();
    }

    Status Speedhack::Remove()
    {
        std::lock_guard lock(m_lock);
        m_enabled = false;
        m_speed = 1.0;
        if (!m_installed || !m_process || Process::DryRun())
            return Status::Success();
        return RemoveLocked();
    }

    Status Speedhack::RemoveLocked()
    {
        const std::shared_ptr<Process> process = m_process;
        const auto reset = [&]()
            {
                Forget();
                m_process = process;
            };
        if (!process->Alive())
        {
            reset();
            return Status::Success();
        }
        std::vector<Range> ranges;
        ranges.push_back({ m_imageBase, m_imageBase + m_imageSize });
        for (std::uint64_t page : m_pages)
            ranges.push_back({ page, page + kPageSize });
        const auto drained = [&]()
            {
                std::int32_t inside[Speed::kStripes * Speed::kStripeStep] = {};
                if (!process->ReadExact(m_block + offsetof(Speed::Block, Inside), inside, sizeof(inside)))
                    return false;
                for (int stripe = 0; stripe < Speed::kStripes; ++stripe)
                {
                    if (inside[stripe * Speed::kStripeStep] != 0)
                        return false;
                }
                return !ThreadInside(*process, ranges);
            };
        bool clean = false;
        int kept = 0;
        Status result = Status::Success();
        {
            Frozen frozen(*process);
            if (!frozen.Held)
                return Failed("the process could not be paused");
            for (const SpeedhackHook& hook : m_hooks)
            {
                std::uint8_t now[kPatch] = {};
                if (!process->ReadExact(hook.Address, now, kPatch) || std::memcmp(now, hook.Patch.data(), kPatch) != 0)
                {
                    ++kept;
                    continue;
                }
                if (Status written = process->Write(hook.Address, hook.Original.data(), kPatch); !written)
                    result = written;
            }
            for (int attempt = 0; attempt < kDrainTries && result && frozen.Held && !clean; ++attempt)
            {
                clean = drained();
                if (!clean)
                {
                    frozen.Thaw();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    frozen.Freeze();
                }
            }
        }
        if (!result)
            return result;
        if (kept)
            LOG_WARN("Speedhack", Format("%d of the speedhack's hooks in %s were changed by something else and were left as they are", kept, process->Name().c_str()));
        // ∑ The hooks that stay (changed by something else) still jump into the agent: their
        //   number tells it to stay loaded; with none left it lets go (Agent/Life.cpp).
        const std::uint32_t left = clean ? static_cast<std::uint32_t>(kept) : static_cast<std::uint32_t>(std::max<int>(kept, 1));
        WriteField(*process, m_block, offsetof(Speed::Block, HookCount), left);
        if (Status released = Call(Speed::CallRelease); !released)
            LOG_WARN("Speedhack", released.Text);
        if (clean && !kept)
        {
            const std::uint64_t originals[Speed::SourceCount] = {};
            process->Write(m_block + offsetof(Speed::Block, Originals), originals, sizeof(originals));
            for (std::uint64_t page : m_pages)
                process->Free(page);
            LOG_INFO("Speedhack", "Speedhack removed from " + process->Name() + "; its time functions are as they were");
        }
        else if (!kept)
        {
            LOG_WARN("Speedhack", "The time functions of " + process->Name() + " are restored, but a thread did not leave the speedhack's code in time, so its library stays loaded");
        }
        reset();
        return Status::Success();
    }
}
