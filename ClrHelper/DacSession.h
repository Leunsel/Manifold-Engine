#pragma once

#include "ClrHelper/Dac.h"
#include "Target/ClrMetadata.h"
#include "Target/ClrProtocol.h"

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Manifold::ClrHelper
{
    namespace ClrProtocol = Target::ClrProtocol;

    // What the DAC is handed to read the game: memory through
    // ReadProcessMemory with a page cache (the DAC reads a few bytes at a
    // time, thousands of times per question), module bases, and metadata from
    // the files on disk for assemblies whose metadata is not mapped in the game.
    class DataTarget final : public ClrDataTarget, public ClrMetadataLocator, public ClrRuntimeLocator
    {
    public:
        void Attach(HANDLE process);
        void Forget();
        void RefreshModules();
        void SetRuntimeBase(ClrAddress base) { m_runtimeBase = base; }

        struct Module
        {
            std::wstring Path;
            std::wstring Name;
            ClrAddress Base = 0;
            std::uint32_t Size = 0;
        };
        const std::vector<Module>& Modules() const { return m_modules; }
        bool Read(ClrAddress address, void* buffer, std::size_t size);
        // Straight from the game, past the page cache: as many bytes from the
        // start as could be read (whole pages), for walking the heap.
        std::size_t ReadDirect(ClrAddress address, std::uint8_t* buffer, std::size_t size);

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override;
        ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }

        HRESULT STDMETHODCALLTYPE GetMachineType(ULONG32* machine) override;
        HRESULT STDMETHODCALLTYPE GetPointerSize(ULONG32* size) override;
        HRESULT STDMETHODCALLTYPE GetImageBase(LPCWSTR imagePath, ClrAddress* base) override;
        HRESULT STDMETHODCALLTYPE ReadVirtual(ClrAddress address, BYTE* buffer, ULONG32 size, ULONG32* done) override;
        HRESULT STDMETHODCALLTYPE WriteVirtual(ClrAddress, BYTE*, ULONG32, ULONG32*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetTLSValue(ULONG32, ULONG32, ClrAddress*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE SetTLSValue(ULONG32, ULONG32, ClrAddress) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetCurrentThreadID(ULONG32*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetThreadContext(ULONG32, ULONG32, ULONG32, BYTE*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE SetThreadContext(ULONG32, ULONG32, BYTE*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE Request(ULONG32, ULONG32, BYTE*, ULONG32, BYTE*) override { return E_NOTIMPL; }

        HRESULT STDMETHODCALLTYPE GetMetadata(LPCWSTR imagePath, ULONG32 timestamp, ULONG32 imageSize, GUID* mvid, ULONG32 mdRva,
            ULONG32 flags, ULONG32 bufferSize, BYTE* buffer, ULONG32* dataSize) override;
        HRESULT STDMETHODCALLTYPE GetRuntimeBase(ClrAddress* base) override;

    private:
        static constexpr std::size_t kPage = 0x1000;
        static constexpr std::size_t kMaxPages = 32768;  // 128 MB of cached pages at most

        const std::uint8_t* Page(ClrAddress page);

        HANDLE m_process = nullptr;
        ClrAddress m_runtimeBase = 0;
        std::vector<Module> m_modules;
        // A null entry is a page that could not be read.
        std::unordered_map<ClrAddress, std::unique_ptr<std::array<std::uint8_t, kPage>>> m_pages;
        // Metadata read from assembly files, by lower-case path.
        std::unordered_map<std::wstring, std::optional<std::vector<std::uint8_t>>> m_metadataFiles;
    };

    class DacSession
    {
    public:
        ~DacSession();

        // Answers one request; the reply is a whole message.
        std::vector<std::uint8_t> Handle(std::span<const std::uint8_t> request);

    private:
        struct ModuleRecord
        {
            ClrAddress Address = 0;
            ClrAddress Assembly = 0;
            std::uint64_t ModuleId = 0;
            std::string Name;
            std::string Path;
            ClrAddress Base = 0;
            std::uint32_t Size = 0;
            bool Dynamic = false;
            bool MetadataTried = false;
            std::shared_ptr<Target::ClrMetadata> Metadata;
        };

        struct TypeInfo
        {
            bool Valid = false;
            bool Named = false;  // the name is asked for only when needed
            DacpMethodTableData Data{};
            std::string Name;
        };

        std::string Open(std::uint32_t pid, HANDLE inherited, ClrProtocol::Writer& out);
        void Modules(ClrProtocol::Writer& out);
        void Types(ClrAddress module, ClrProtocol::Writer& out);
        void Members(ClrAddress mt, ClrProtocol::Writer& out);
        void DescribeType(ClrAddress mt, ClrProtocol::Writer& out);
        void MethodAt(ClrAddress ip, ClrProtocol::Writer& out);
        void MethodCode(ClrAddress md, ClrProtocol::Writer& out);
        void Flush();

        void LoadModules();
        ModuleRecord* FindModule(ClrAddress module);
        const Target::ClrMetadata* MetadataOf(ClrAddress module);
        const TypeInfo& Type(ClrAddress mt);
        const std::string& NameOf(ClrAddress mt);
        std::uint32_t TypeFlags(ClrAddress mt, bool withGeneric = true);
        std::string MethodName(ClrAddress md, ClrAddress* typeOut);

        struct HeapSegment
        {
            ClrAddress Start = 0;
            ClrAddress End = 0;
            bool Large = false;
        };
        struct HeapTotals
        {
            std::uint64_t Objects = 0;
            std::uint64_t Bytes = 0;
            std::uint64_t Skipped = 0;  // pointer-sized steps over words that start no object
            std::uint32_t Segments = 0;
        };
        struct ObjectSize
        {
            bool Valid = false;
            std::uint32_t Base = 0;
            std::uint32_t Component = 0;
        };
        std::string HeapSegments(std::vector<HeapSegment>& out);
        template <typename Visit>
        std::string WalkHeap(Visit&& visit, HeapTotals& totals);
        const ObjectSize& SizeOf(ClrAddress mt);
        std::string HeapStats(ClrProtocol::Writer& out);
        std::string Instances(ClrAddress mt, std::uint32_t limit, ClrProtocol::Writer& out);
        void ObjectInfo(ClrAddress address, ClrProtocol::Writer& out);
        ClrAddress StaticBase(ClrAddress mt, ClrAddress module, bool primitive);

        DataTarget m_target;
        HANDLE m_process = nullptr;
        HMODULE m_dac = nullptr;
        XClrDataProcess* m_data = nullptr;
        SosDac* m_sos = nullptr;
        SosDac8* m_sos8 = nullptr;
        SosDac14* m_sos14 = nullptr;
        DacpUsefulGlobalsData m_globals{};
        ClrAddress m_defaultDomain = 0;
        ClrAddress m_valueTypeMt = 0;
        ClrAddress m_enumMt = 0;
        std::vector<ModuleRecord> m_modules;
        bool m_modulesLoaded = false;
        std::unordered_map<ClrAddress, TypeInfo> m_types;
        std::unordered_map<ClrAddress, ObjectSize> m_sizes;
        std::chrono::steady_clock::time_point m_lastRequest;
    };
}
