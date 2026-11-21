#pragma once

#include <cstdint>
#include <cstring>

#include <windows.h>
#include <unknwn.h>

// The interfaces of the .NET data access library (mscordacwks.dll for the
// .NET Framework, mscordaccore.dll for .NET) that the helper uses. ICLRDataTarget
// and its companions are public (clrdata.h); ISOSDacInterface is the private
// interface SOS and ClrMD read the runtime with (sospriv.idl, dacprivate.h in
// dotnet/runtime). Only the vtable order and the structs matter here: every
// IID below was checked against the GUIDs inside the shipped DAC binaries
// (Framework x86/x64, .NET 8 and .NET 10), and the structs are the
// version-stable ones ClrMD reads on every runtime since .NET Framework 4.
namespace Manifold::ClrHelper
{
    using ClrAddress = std::uint64_t;  // CLRDATA_ADDRESS: 64-bit even in a 32-bit DAC
    using ClrToken = std::uint32_t;

    constexpr GUID kIidClrDataTarget = { 0x3E11CCEE, 0xD08B, 0x43E5, { 0xAF, 0x01, 0x32, 0x71, 0x7A, 0x64, 0xDA, 0x03 } };
    constexpr GUID kIidClrDataTarget2 = { 0x6D05FAE3, 0x189C, 0x4630, { 0xA6, 0xDC, 0x1C, 0x25, 0x1E, 0x1C, 0x01, 0xAB } };
    constexpr GUID kIidClrMetadataLocator = { 0xAA8FA804, 0xBC05, 0x4642, { 0xB2, 0xC5, 0xC3, 0x53, 0xED, 0x22, 0xFC, 0x63 } };
    constexpr GUID kIidClrRuntimeLocator = { 0xB760BF44, 0x9377, 0x4597, { 0x8B, 0xE7, 0x58, 0x08, 0x3B, 0xDC, 0x51, 0x46 } };
    constexpr GUID kIidXClrDataProcess = { 0x5C552AB6, 0xFC09, 0x4CB3, { 0x8E, 0x36, 0x22, 0xFA, 0x03, 0xC7, 0x98, 0xB7 } };
    constexpr GUID kIidSosDac = { 0x436F00F2, 0xB42A, 0x4B9F, { 0x87, 0x0C, 0xE7, 0x3D, 0xB6, 0x6A, 0xE9, 0x30 } };
    // .NET 9 and later keep statics per type; this one answers where they are.
    constexpr GUID kIidSosDac14 = { 0x9AA22ACA, 0x6DC6, 0x4A0C, { 0xB4, 0xE0, 0x70, 0xD2, 0x41, 0x6B, 0x98, 0x37 } };

    struct ClrDataTarget : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetMachineType(ULONG32* machine) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetPointerSize(ULONG32* size) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetImageBase(LPCWSTR imagePath, ClrAddress* base) = 0;
        virtual HRESULT STDMETHODCALLTYPE ReadVirtual(ClrAddress address, BYTE* buffer, ULONG32 size, ULONG32* done) = 0;
        virtual HRESULT STDMETHODCALLTYPE WriteVirtual(ClrAddress address, BYTE* buffer, ULONG32 size, ULONG32* done) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetTLSValue(ULONG32 thread, ULONG32 index, ClrAddress* value) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetTLSValue(ULONG32 thread, ULONG32 index, ClrAddress value) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetCurrentThreadID(ULONG32* thread) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetThreadContext(ULONG32 thread, ULONG32 flags, ULONG32 size, BYTE* context) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetThreadContext(ULONG32 thread, ULONG32 size, BYTE* context) = 0;
        virtual HRESULT STDMETHODCALLTYPE Request(ULONG32 code, ULONG32 inSize, BYTE* in, ULONG32 outSize, BYTE* out) = 0;
    };

    struct ClrMetadataLocator : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetMetadata(LPCWSTR imagePath, ULONG32 timestamp, ULONG32 imageSize, GUID* mvid, ULONG32 mdRva,
            ULONG32 flags, ULONG32 bufferSize, BYTE* buffer, ULONG32* dataSize) = 0;
    };

    struct ClrRuntimeLocator : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetRuntimeBase(ClrAddress* base) = 0;
    };

    // IXCLRDataProcess: only its first method is used, Flush, which drops the
    // DAC's cached copies of the game's memory.
    struct XClrDataProcess : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE Flush() = 0;
    };

    // ∑ Called back by the DAC for every type: with __stdcall, as the 32-bit
    // DAC expects (a cdecl callback left the stack 16 bytes off per call and
    // the 32-bit DAC looped forever). x64 has one convention only.
    using ModuleMapTraverse = void(__stdcall*)(UINT index, ClrAddress methodTable, LPVOID token);

    enum ModuleMapType : int
    {
        kTypeDefToMethodTable = 0,
        kTypeRefToMethodTable = 1,
    };

    struct DacpAppDomainStoreData
    {
        ClrAddress SharedDomain;
        ClrAddress SystemDomain;
        LONG DomainCount;
    };

    struct DacpModuleData
    {
        ClrAddress Address;
        ClrAddress PeFile;
        ClrAddress IlBase;
        ClrAddress MetadataStart;
        std::uint64_t MetadataSize;
        ClrAddress Assembly;
        BOOL IsReflection;
        BOOL IsPeFile;
        std::uint64_t BaseClassIndex;
        std::uint64_t ModuleId;
        DWORD TransientFlags;
        ClrAddress TypeDefToMethodTableMap;
        ClrAddress TypeRefToMethodTableMap;
        ClrAddress MethodDefToDescMap;
        ClrAddress FieldDefToDescMap;
        ClrAddress MemberRefToDescMap;
        ClrAddress FileReferencesMap;
        ClrAddress ManifestModuleReferencesMap;
        ClrAddress LookupTableHeap;
        ClrAddress ThunkHeap;
        std::uint64_t ModuleIndex;
    };

    struct DacpMethodTableData
    {
        BOOL IsFree;
        ClrAddress Module;
        ClrAddress Class;
        ClrAddress ParentMethodTable;
        WORD NumInterfaces;
        WORD NumMethods;
        WORD NumVtableSlots;
        WORD NumVirtuals;
        DWORD BaseSize;
        DWORD ComponentSize;
        ClrToken Token;
        DWORD AttrClass;
        BOOL IsShared;
        BOOL IsDynamic;
        BOOL ContainsPointers;
    };

    struct DacpMethodTableFieldData
    {
        WORD NumInstanceFields;
        WORD NumStaticFields;
        WORD NumThreadStaticFields;
        ClrAddress FirstField;
        WORD ContextStaticOffset;
        WORD ContextStaticsSize;
    };

    struct DacpFieldDescData
    {
        DWORD Type;     // CorElementType as the runtime lays the field out
        DWORD SigType;  // CorElementType from the signature
        ClrAddress MtOfType;
        ClrAddress ModuleOfType;
        ClrToken TokenOfType;
        ClrToken Token;
        ClrAddress MtOfEnclosingClass;
        DWORD Offset;
        BOOL IsThreadLocal;
        BOOL IsContextLocal;
        BOOL IsStatic;
        ClrAddress NextField;
    };

    struct DacpReJitData
    {
        ClrAddress RejitId;
        DWORD Flags;
        ClrAddress NativeCodeAddr;
    };

    struct DacpMethodDescData
    {
        BOOL HasNativeCode;
        BOOL IsDynamic;
        WORD SlotNumber;
        ClrAddress NativeCodeAddr;
        ClrAddress AddressOfNativeCodeSlot;
        ClrAddress MethodDescPtr;
        ClrAddress MethodTablePtr;
        ClrAddress ModulePtr;
        ClrToken Token;
        ClrAddress GcInfo;
        ClrAddress GcStressCodeCopy;
        ClrAddress ManagedDynamicMethodObject;
        ClrAddress RequestedIp;
        DacpReJitData RejitDataCurrent;
        DacpReJitData RejitDataRequested;
        ULONG JittedRejitVersions;
    };

    struct DacpCodeHeaderData
    {
        ClrAddress GcInfo;
        DWORD JitType;
        ClrAddress MethodDescPtr;
        ClrAddress MethodStart;
        DWORD MethodSize;
        ClrAddress ColdRegionStart;
        DWORD ColdRegionSize;
        DWORD HotRegionSize;
    };

    struct DacpDomainLocalModuleData
    {
        ClrAddress AppDomainAddr;
        std::uint64_t ModuleId;
        ClrAddress ClassData;
        ClrAddress DynamicClassTable;
        ClrAddress GcStaticDataStart;
        ClrAddress NonGcStaticDataStart;
    };

    struct DacpUsefulGlobalsData
    {
        ClrAddress ArrayMethodTable;
        ClrAddress StringMethodTable;
        ClrAddress ObjectMethodTable;
        ClrAddress ExceptionMethodTable;
        ClrAddress FreeMethodTable;
    };

    // ∑ The DAC writes these structs; a newer runtime could make one longer
    // than declared here. Every call gets the struct inside a larger zeroed
    // buffer, so a longer answer lands in slack instead of on the stack.
    template <typename T>
    struct Padded
    {
        union
        {
            T Value;
            std::uint8_t Slack[sizeof(T) + 256];
        };
        Padded() { std::memset(Slack, 0, sizeof(Slack)); }
        T* Ptr() { return &Value; }
        T* operator->() { return &Value; }
    };

    // The vtable order of sospriv.idl. Methods the helper does not call keep
    // their slot with a placeholder signature.
    struct SosDac : IUnknown
    {
        using Unused = HRESULT;
        virtual Unused STDMETHODCALLTYPE GetThreadStoreData(void*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetAppDomainStoreData(DacpAppDomainStoreData* data) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetAppDomainList(unsigned count, ClrAddress* values, unsigned* needed) = 0;
        virtual Unused STDMETHODCALLTYPE GetAppDomainData(ClrAddress, void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetAppDomainName(ClrAddress, unsigned, WCHAR*, unsigned*) = 0;
        virtual Unused STDMETHODCALLTYPE GetDomainFromContext(ClrAddress, ClrAddress*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetAssemblyList(ClrAddress domain, int count, ClrAddress* values, int* needed) = 0;
        virtual Unused STDMETHODCALLTYPE GetAssemblyData(ClrAddress, ClrAddress, void*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetAssemblyName(ClrAddress assembly, unsigned count, WCHAR* name, unsigned* needed) = 0;
        virtual Unused STDMETHODCALLTYPE GetModule(ClrAddress, void**) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetModuleData(ClrAddress module, DacpModuleData* data) = 0;
        virtual HRESULT STDMETHODCALLTYPE TraverseModuleMap(ModuleMapType type, ClrAddress module, ModuleMapTraverse callback, LPVOID token) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetAssemblyModuleList(ClrAddress assembly, unsigned count, ClrAddress* modules, unsigned* needed) = 0;
        virtual Unused STDMETHODCALLTYPE GetILForModule(ClrAddress, DWORD, ClrAddress*) = 0;
        virtual Unused STDMETHODCALLTYPE GetThreadData(ClrAddress, void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetThreadFromThinlockID(UINT, ClrAddress*) = 0;
        virtual Unused STDMETHODCALLTYPE GetStackLimits(ClrAddress, ClrAddress*, ClrAddress*, ClrAddress*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetMethodDescData(ClrAddress md, ClrAddress ip, DacpMethodDescData* data, ULONG rejitCount, void* rejitData, ULONG* rejitNeeded) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetMethodDescPtrFromIP(ClrAddress ip, ClrAddress* md) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetMethodDescName(ClrAddress md, unsigned count, WCHAR* name, unsigned* needed) = 0;
        virtual Unused STDMETHODCALLTYPE GetMethodDescPtrFromFrame(ClrAddress, ClrAddress*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetMethodDescFromToken(ClrAddress module, ClrToken token, ClrAddress* md) = 0;
        virtual Unused STDMETHODCALLTYPE GetMethodDescTransparencyData(ClrAddress, void*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetCodeHeaderData(ClrAddress ip, DacpCodeHeaderData* data) = 0;
        virtual Unused STDMETHODCALLTYPE GetJitManagerList(unsigned, void*, unsigned*) = 0;
        virtual Unused STDMETHODCALLTYPE GetJitHelperFunctionName(ClrAddress, unsigned, char*, unsigned*) = 0;
        virtual Unused STDMETHODCALLTYPE GetJumpThunkTarget(void*, ClrAddress*, ClrAddress*) = 0;
        virtual Unused STDMETHODCALLTYPE GetThreadpoolData(void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetWorkRequestData(ClrAddress, void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetHillClimbingLogEntry(ClrAddress, void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetObjectData(ClrAddress, void*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetObjectStringData(ClrAddress object, unsigned count, WCHAR* text, unsigned* needed) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetObjectClassName(ClrAddress object, unsigned count, WCHAR* name, unsigned* needed) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetMethodTableName(ClrAddress mt, unsigned count, WCHAR* name, unsigned* needed) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetMethodTableData(ClrAddress mt, DacpMethodTableData* data) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetMethodTableSlot(ClrAddress mt, unsigned slot, ClrAddress* value) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetMethodTableFieldData(ClrAddress mt, DacpMethodTableFieldData* data) = 0;
        virtual Unused STDMETHODCALLTYPE GetMethodTableTransparencyData(ClrAddress, void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetMethodTableForEEClass(ClrAddress, ClrAddress*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetFieldDescData(ClrAddress field, DacpFieldDescData* data) = 0;
        virtual Unused STDMETHODCALLTYPE GetFrameName(ClrAddress, unsigned, WCHAR*, unsigned*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetPEFileBase(ClrAddress file, ClrAddress* base) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetPEFileName(ClrAddress file, unsigned count, WCHAR* name, unsigned* needed) = 0;
        virtual Unused STDMETHODCALLTYPE GetGCHeapData(void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetGCHeapList(unsigned, ClrAddress*, unsigned*) = 0;
        virtual Unused STDMETHODCALLTYPE GetGCHeapDetails(ClrAddress, void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetGCHeapStaticData(void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetHeapSegmentData(ClrAddress, void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetOOMData(ClrAddress, void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetOOMStaticData(void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetHeapAnalyzeData(ClrAddress, void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetHeapAnalyzeStaticData(void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetDomainLocalModuleData(ClrAddress, void*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetDomainLocalModuleDataFromAppDomain(ClrAddress domain, int moduleId, DacpDomainLocalModuleData* data) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetDomainLocalModuleDataFromModule(ClrAddress module, DacpDomainLocalModuleData* data) = 0;
        virtual Unused STDMETHODCALLTYPE GetThreadLocalModuleData(ClrAddress, unsigned, void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetSyncBlockData(unsigned, void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetSyncBlockCleanupData(ClrAddress, void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetHandleEnum(void**) = 0;
        virtual Unused STDMETHODCALLTYPE GetHandleEnumForTypes(unsigned*, unsigned, void**) = 0;
        virtual Unused STDMETHODCALLTYPE GetHandleEnumForGC(unsigned, void**) = 0;
        virtual Unused STDMETHODCALLTYPE TraverseEHInfo(ClrAddress, void*, LPVOID) = 0;
        virtual Unused STDMETHODCALLTYPE GetNestedExceptionData(ClrAddress, ClrAddress*, ClrAddress*) = 0;
        virtual Unused STDMETHODCALLTYPE GetStressLogAddress(ClrAddress*) = 0;
        virtual Unused STDMETHODCALLTYPE TraverseLoaderHeap(ClrAddress, void*) = 0;
        virtual Unused STDMETHODCALLTYPE GetCodeHeapList(ClrAddress, unsigned, void*, unsigned*) = 0;
        virtual Unused STDMETHODCALLTYPE TraverseVirtCallStubHeap(ClrAddress, int, void*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetUsefulGlobals(DacpUsefulGlobalsData* data) = 0;
    };

    struct SosDac14 : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetStaticBaseAddress(ClrAddress mt, ClrAddress* nonGcStatics, ClrAddress* gcStatics) = 0;
    };

    using ClrDataCreateInstanceFn = HRESULT(STDAPICALLTYPE*)(REFIID iid, ClrDataTarget* target, void** out);

    // CorElementType values the helper looks at.
    enum Element : DWORD
    {
        kElementBoolean = 0x02, kElementChar = 0x03, kElementI1 = 0x04, kElementU1 = 0x05, kElementI2 = 0x06,
        kElementU2 = 0x07, kElementI4 = 0x08, kElementU4 = 0x09, kElementI8 = 0x0A, kElementU8 = 0x0B,
        kElementR4 = 0x0C, kElementR8 = 0x0D, kElementString = 0x0E, kElementPtr = 0x0F, kElementValueType = 0x11,
        kElementClass = 0x12, kElementI = 0x18, kElementU = 0x19, kElementFnPtr = 0x1B, kElementObject = 0x1C,
        kElementSzArray = 0x1D,
    };
}
