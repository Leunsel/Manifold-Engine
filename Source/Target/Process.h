#pragma once

#include "Core/Errors.h"
#include "Core/Handle.h"
#include "Core/Types.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Target
{
    inline constexpr std::uint32_t kMaxModuleExports = 0x10000;

    struct ModuleInfo
    {
        std::string Name;
        std::wstring Path;
        std::uint64_t Base = 0;
        std::uint64_t Size = 0;
        bool System = false;

        bool Contains(std::uint64_t address) const { return address >= Base && address < Base + Size; }
    };

    struct RegionInfo
    {
        std::uint64_t Base = 0;
        std::uint64_t Size = 0;
        std::uint64_t AllocationBase = 0;
        std::uint32_t Protect = 0;
        std::uint32_t AllocationProtect = 0;
        std::uint32_t State = 0;
        std::uint32_t Type = 0;

        bool Committed() const;
        bool Readable() const;
        bool Writable() const;
        bool Executable() const;
        bool CopyOnWrite() const;
        bool Guarded() const;
        std::uint64_t End() const { return Base + Size; }
    };

    struct ProcessEntry
    {
        ProcessId Id = ProcessId::None;
        ProcessId ParentId = ProcessId::None;
        std::string Name;
        std::string WindowTitle;
        std::wstring Path;
        bool Is64Bit = true;
        bool Accessible = true;
        std::uint32_t Threads = 0;
    };

    std::vector<ProcessEntry> ListProcesses(bool withWindowTitles);
    std::optional<ProcessId> FindProcessId(std::string_view name);
    bool IsElevated();

    using ModuleList = std::shared_ptr<const std::vector<ModuleInfo>>;

    struct DryRunActivity
    {
        int Writes = 0;
        std::size_t BytesWritten = 0;
        int Allocations = 0;
        int Frees = 0;
        int Protections = 0;
        int Threads = 0;
        int Libraries = 0;
    };

    class Process
    {
    public:
        static Result<std::shared_ptr<Process>> Open(ProcessId id);
        ~Process() = default;

        Process(const Process&) = delete;
        Process& operator=(const Process&) = delete;

        ProcessId Id() const { return m_id; }
        const std::string& Name() const { return m_name; }
        const std::wstring& Path() const { return m_path; }
        bool Is64Bit() const { return m_is64Bit; }
        int PointerSize() const { return m_is64Bit ? 8 : 4; }
        HANDLE Handle() const { return m_handle.Get(); }
        bool Alive() const;
        std::uint64_t LowestAddress() const { return 0x10000; }
        std::uint64_t HighestAddress() const { return m_highest; }

        std::size_t Read(std::uint64_t address, void* buffer, std::size_t size) const;
        bool ReadExact(std::uint64_t address, void* buffer, std::size_t size) const;
        std::optional<std::uint64_t> ReadPointer(std::uint64_t address) const;
        template <class T>
        std::optional<T> ReadValue(std::uint64_t address) const
        {
            T value{};
            if (!ReadExact(address, &value, sizeof(T)))
                return std::nullopt;
            return value;
        }

        Status Write(std::uint64_t address, const void* data, std::size_t size) const;
        Status Write(std::uint64_t address, std::span<const std::uint8_t> bytes) const { return Write(address, bytes.data(), bytes.size()); }

        Result<Address> Allocate(std::size_t size, std::uint32_t protect, Address nearAddress = 0) const;
        Status Free(std::uint64_t address) const;
        std::optional<std::uint32_t> Protect(std::uint64_t address, std::size_t size, std::uint32_t protect) const;

        std::vector<RegionInfo> Regions(std::uint64_t start, std::uint64_t stop) const;
        std::optional<RegionInfo> RegionAt(std::uint64_t address) const;
        bool IsReadable(std::uint64_t address) const;

        ModuleList Modules() const;
        void RefreshModules();
        std::optional<ModuleInfo> ModuleAt(std::uint64_t address) const;
        std::optional<ModuleInfo> FindModule(std::string_view name) const;
        std::optional<ModuleInfo> MainModule() const;

        std::vector<std::uint32_t> Threads() const;
        bool Suspend() const;
        bool Resume() const;

        Status CreateThread(std::uint64_t start, std::uint64_t parameter, bool wait, std::uint32_t timeoutMilliseconds = 10000) const;
        Status InjectLibrary(const std::wstring& path) const;
        std::uint64_t FindExport(std::string_view module, std::string_view name) const;

        static void SetDryRun(bool enabled);
        static bool DryRun();
        DryRunActivity DryActivity() const;

    private:
        Process() = default;
        std::uint64_t ExportAddress(std::string_view module, std::string_view name, int depth) const;

        bool DryReserve(std::uint64_t base, std::size_t size) const;
        void DryOverlay(std::uint64_t address, std::uint8_t* out, std::size_t size) const;
        bool DryInsideAllocation(std::uint64_t address, std::size_t size) const;

        ProcessId m_id = ProcessId::None;
        UniqueHandle m_handle;
        std::string m_name;
        std::wstring m_path;
        bool m_is64Bit = true;
        std::uint64_t m_highest = 0x7FFFFFFEFFFFull;

        mutable std::mutex m_moduleLock;
        struct ModuleSpan
        {
            std::uint64_t Base = 0;
            std::uint64_t End = 0;
            std::size_t Index = 0;
        };
        ModuleList m_modules;
        std::shared_ptr<const std::vector<ModuleSpan>> m_moduleSpans;

        mutable std::mutex m_dryLock;
        mutable std::map<std::uint64_t, std::vector<std::uint8_t>> m_dryAllocations;
        mutable std::map<std::uint64_t, std::uint8_t> m_dryBytes;
        inline static DryRunActivity m_dryActivity;
    };

    std::string ProtectText(std::uint32_t protect);
    std::string RegionTypeText(std::uint32_t type);
}
