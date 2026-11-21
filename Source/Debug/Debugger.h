#pragma once

#include "Core/Errors.h"
#include "Debug/BreakAction.h"
#include "Disasm/Disassembler.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::Debug
{
    enum class WatchKind
    {
        Accesses,
        Writes,
        InstructionAccesses,
        Calls
    };

    struct WatchEntry
    {
        std::uint64_t Instruction = 0;
        std::uint64_t Accessed = 0;
        std::uint64_t Count = 0;
        std::uint32_t ThreadId = 0;
        Disasm::RegisterValues Registers;
    };

    struct Watch
    {
        int Id = 0;
        WatchKind Kind = WatchKind::Accesses;
        std::uint64_t Address = 0;
        int Size = 4;
        bool Running = false;
        int Slot = -1;
        std::uint64_t Hits = 0;
        std::uint64_t Generation = 0;
        std::vector<WatchEntry> Entries;
    };

    enum class BreakpointType
    {
        Execute,
        Write,
        Access
    };

    struct Breakpoint
    {
        std::uint64_t Address = 0;
        BreakpointType Type = BreakpointType::Execute;
        int Size = 1;
        bool Hardware = false;
        int Slot = -1;
        bool Temporary = false;
        int WatchId = 0;
        std::uint8_t Original = 0;
        bool Inserted = false;
        std::uint32_t OwnerThread = 0;
        std::uint64_t AboveStack = 0;
        BreakAction Action;
        std::uint64_t Hits = 0;
    };

    enum class ResumeMode
    {
        Continue,
        StepInto,
        StepOver,
        RunToReturn
    };

    struct VectorRegisters
    {
        std::array<std::array<std::uint8_t, 16>, 16> Xmm{};
        std::uint32_t Mxcsr = 0;
        int Count = 0;

        bool operator==(const VectorRegisters&) const = default;
    };

    // The first 160 bytes of the FXSAVE area: control, status and tag words,
    // the last opcode and pointers, MXCSR, and the eight x87 stack registers
    // (ST0 first, 16 bytes each, 10 used).
    struct FpuRegisters
    {
        static constexpr int kBytes = 160;
        std::array<std::uint8_t, kBytes> Area{};
        bool Valid = false;

        std::uint16_t Control() const;
        std::uint16_t Status() const;
        std::uint8_t Tags() const;
        std::uint16_t Opcode() const;
        int Top() const { return (Status() >> 11) & 7; }
        bool Used(int st) const { return (Tags() >> ((Top() + st) & 7)) & 1; }
        const std::uint8_t* Stack(int st) const { return Area.data() + 32 + st * 16; }
        std::uint8_t* Stack(int st) { return Area.data() + 32 + st * 16; }
        void SetControl(std::uint16_t value);
        void SetStatus(std::uint16_t value);
        void SetTags(std::uint8_t value);

        bool operator==(const FpuRegisters&) const = default;
    };

    double ExtendedToDouble(const std::uint8_t* tenBytes);
    void DoubleToExtended(double value, std::uint8_t* tenBytes);
    std::string ExtendedText(const std::uint8_t* tenBytes);

    enum class ExceptionBreak
    {
        Never,
        Always,
        InRegion
    };

    struct BreakException
    {
        bool Raised = false;
        std::uint32_t Code = 0;
        bool FirstChance = true;
        int ParamCount = 0;
        std::uint64_t Params[2] = {};
    };

    std::string ExceptionName(std::uint32_t code);
    std::string DescribeException(const BreakException& exception);
    bool UnexpectedException(std::uint32_t code);

    struct BreakInfo
    {
        bool Broken = false;
        std::uint32_t ThreadId = 0;
        std::uint64_t Address = 0;
        std::uint64_t BreakpointAddress = 0;
        Disasm::RegisterValues Registers;
        VectorRegisters Vectors;
        FpuRegisters Fpu;
        BreakException Exception;
        std::string Reason;
        std::uint64_t Serial = 0;
        bool Is64Bit = true;
    };

    const char* WatchKindName(WatchKind kind);
    const char* BreakpointTypeName(BreakpointType type);

    enum class Backend
    {
        Windows,
        Veh
    };

    class Debugger
    {
    public:
        Debugger();
        ~Debugger();

        Debugger(const Debugger&) = delete;
        Debugger& operator=(const Debugger&) = delete;

        Status Attach(std::shared_ptr<Target::Process> process);
        void Detach();
        bool Attached() const { return m_attached.load(); }
        std::uint64_t Generation() const { return m_generation.load(); }
        void SetLogDebugStrings(bool on) { m_logDebugStrings = on; }
        void SetPreferHardware(bool on) { m_preferHardware = on; }
        void PauseHeartbeat(bool paused) { m_beatPaused = paused; }
        void SimulateMissingStepBits(bool on) { m_simulateMissingStepBits = on; }
        void SetBackend(Backend backend) { m_backend = backend; }
        Backend CurrentBackend() const { return m_backend; }
        bool UsingVeh() const { return m_backend == Backend::Veh; }
        int HiddenThreads() const { return m_hiddenThreads.load(); }
        bool MainThreadHidden() const { return m_mainThreadHidden.load(); }

        Status StartWatch(WatchKind kind, std::uint64_t address, int size, int& id);
        void StopWatch(int id);
        void RemoveWatch(int id);
        void ClearWatch(int id);
        bool CopyWatch(int id, Watch& out) const;
        std::vector<int> WatchIds() const;

        Status SetBreakpoint(std::uint64_t address, BreakpointType type, int size, bool hardware, BreakAction action = {});
        Status SetBreakpointAction(std::uint64_t address, BreakAction action);
        Status RemoveBreakpoint(std::uint64_t address);
        Status ToggleBreakpoint(std::uint64_t address);
        bool HasBreakpoint(std::uint64_t address) const;
        std::vector<Breakpoint> Breakpoints() const;
        int FreeHardwareSlots() const;

        bool Broken() const;
        bool Stepping() const { return m_stepping.load(); }
        BreakInfo Break() const;
        void Resume(ResumeMode mode);
        void ResumeHandled();
        void SetRegisters(const Disasm::RegisterValues& registers);
        void SetVectors(const VectorRegisters& vectors);
        void SetFpu(const FpuRegisters& fpu);
        void SetExceptionBreak(ExceptionBreak mode, std::uint64_t start, std::uint64_t stop);
        ExceptionBreak ExceptionBreakMode() const { return m_exceptionMode.load(); }
        Status BreakNow();
        void Unpatch(std::uint64_t address, std::uint8_t* buffer, std::size_t size) const;
        // Writes into the debugged process without disarming the software breakpoints in the range:
        // their int3 stays, and the written byte is the one put back when the breakpoint goes.
        Status WriteAround(const Target::Process& process, std::uint64_t address, std::span<const std::uint8_t> bytes);
        // Reads as the code is without the debugger's int3s, in one step with the breakpoint list, so a
        // breakpoint hit in between cannot leave an int3 in the buffer.
        bool ReadAround(const Target::Process& process, std::uint64_t address, std::uint8_t* buffer, std::size_t size) const;

    private:
        struct ThreadState;
        struct Task
        {
            std::function<Status()> Work;
            std::shared_ptr<std::promise<Status>> Done;
        };

        Status Post(std::function<Status()> work, unsigned timeoutMilliseconds = 3000);
        void Loop(std::shared_ptr<std::promise<Status>> attached);
        void RunTasks();
        unsigned long HandleException(void* event, bool& enterBreak, std::string& reason, std::uint64_t& breakpointAddress, std::uint32_t& breakThread);
        std::uint32_t Wow64BreakThread(std::uint32_t breakIn) const;
        void EnterBreak(std::uint32_t threadId, const std::string& reason, std::uint64_t breakpointAddress);
        bool BreaksOnException(std::uint32_t code, std::uint64_t address) const;
        void VehWriteExceptionBreak();
        Status ApplyHardware(std::uint32_t onlyThread = 0);
        void LeaveOut(ThreadState& thread);
        void CheckHiddenThreads(bool force);
        void CountHidden();
        void AttributeLostHit(std::uint64_t ip, std::uint32_t threadId, const Disasm::RegisterValues& registers);
        void PlanStepAccess(ThreadState& thread, const Disasm::RegisterValues& registers);
        Status InsertSoftware(Breakpoint& breakpoint);
        void RemoveSoftware(Breakpoint& breakpoint);
        int TakeSlot() const;
        Status AddBreakpointInternal(Breakpoint breakpoint);
        void RemoveBreakpointInternal(std::uint64_t address);
        void UnpatchLocked(std::uint64_t address, std::uint8_t* buffer, std::size_t size) const;
        Disasm::Instruction DecodeUnpatched(std::uint64_t address) const;
        void DropTemporaries();
        std::optional<std::uint64_t> PreviousInstruction(std::uint64_t address) const;
        void RecordHit(Watch& watch, std::uint64_t instruction, std::uint64_t accessed, std::uint32_t threadId, const Disasm::RegisterValues& registers);
        struct HitPlace
        {
            std::uint64_t Instruction = 0;
            std::uint64_t Accessed = 0;
        };
        std::optional<HitPlace> ExecuteHitPlace(WatchKind kind, std::uint64_t at, const Disasm::RegisterValues& registers) const;
        bool AnyHardware() const;
        void CleanUp();

        Status VehAttach();
        void VehDetach();
        Status VehStartWatch(WatchKind kind, std::uint64_t address, int size, int& id);
        void VehSetActive(int id, bool active);
        void VehBump();
        void VehPollLoop();
        int VehFreeSlot() const;
        Status VehSetBreakpoint(std::uint64_t address, BreakpointType type, int size, const BreakAction& action);
        void VehWriteAction(int slot, const BreakAction* action, std::uint32_t threadId, bool keepCounts);
        enum class HitOutcome
        {
            Skip,
            Run,
            Stop
        };
        HitOutcome RunBreakAction(Breakpoint& breakpoint, Disasm::RegisterValues& registers, std::uint32_t threadId, bool& changed);
        void DataHit(Breakpoint& breakpoint, std::uint64_t instruction, std::uint32_t threadId, Disasm::RegisterValues& registers, bool& changed, bool& enterBreak,
            std::string& reason, std::uint64_t& breakpointAddress);
        void VehRemoveBreakpoint(std::uint64_t address);
        void VehResume(ResumeMode mode);
        void VehSetRegisters(const Disasm::RegisterValues& registers);
        void VehWriteSlot(int slot, std::uint32_t mode, std::uint64_t address, int size, bool active);
        void VehCheckBreak();
        std::vector<std::uint64_t> VehStepTargets(const Disasm::Instruction& instruction, const Disasm::RegisterValues& registers, ResumeMode mode) const;
        void VehClearTempSlots();
        std::optional<std::uint64_t> ReturnSlot(const Disasm::RegisterValues& registers) const;
        void VehWakeAgent();
        std::wstring AgentPath() const;
        std::wstring AgentName() const;

        std::shared_ptr<Target::Process> m_process;
        std::unique_ptr<Disasm::Disassembler> m_disassembler;
        std::thread m_thread;
        std::atomic<bool> m_attached{ false };
        std::atomic<bool> m_stop{ false };
        std::atomic<std::uint64_t> m_generation{ 1 };
        std::atomic<bool> m_logDebugStrings{ true };
        std::atomic<bool> m_preferHardware{ false };
        std::atomic<bool> m_beatPaused{ false };
        std::atomic<bool> m_simulateMissingStepBits{ false };
        bool m_is64Bit = true;
        bool m_initialBreakSeen = false;

        mutable std::mutex m_lock;
        std::condition_variable m_wake;
        std::deque<Task> m_tasks;
        std::map<std::uint32_t, std::unique_ptr<ThreadState>> m_threads;
        std::map<std::uint64_t, Breakpoint> m_breakpoints;
        std::map<int, Watch> m_watches;
        int m_nextWatchId = 1;

        BreakInfo m_break;
        bool m_resumeRequested = false;
        ResumeMode m_resumeMode = ResumeMode::Continue;
        bool m_registersChanged = false;
        Disasm::RegisterValues m_newRegisters;
        bool m_vectorsChanged = false;
        VectorRegisters m_newVectors;
        bool m_fpuChanged = false;
        FpuRegisters m_newFpu;
        bool m_exceptionHandled = false;
        BreakException m_cause;
        std::atomic<ExceptionBreak> m_exceptionMode{ ExceptionBreak::Never };
        std::atomic<std::uint64_t> m_exceptionStart{ 0 };
        std::atomic<std::uint64_t> m_exceptionStop{ 0 };
        std::atomic<bool> m_stepping{ false };
        std::uint64_t m_breakSerial = 0;
        bool m_breakRequested = false;
        std::uint64_t m_lastHardwareTick = 0;
        std::uint64_t m_lastHiddenCheck = 0;
        std::uint32_t m_mainThread = 0;
        std::atomic<int> m_hiddenThreads{ 0 };
        std::atomic<bool> m_mainThreadHidden{ false };

        Backend m_backend = Backend::Windows;
        std::uint64_t m_vehBlock = 0;
        std::thread m_vehThread;
        std::atomic<bool> m_vehStop{ false };
        std::uint64_t m_vehBreakSerial = 0;
        std::vector<int> m_vehTempSlots;
        bool m_vehStepOut = false;
        void* m_vehBreakEvent = nullptr;
        void* m_vehResumeEvent = nullptr;
    };
}
