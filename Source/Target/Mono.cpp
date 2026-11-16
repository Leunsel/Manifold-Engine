#include "Target/Mono.h"

#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/Process.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace Manifold::Target
{
    namespace
    {
        constexpr unsigned int kTableTypeDef = 2;
        constexpr unsigned int kTokenTypeDef = 0x02000000u;
        constexpr int kMaxClasses = 200000;
        constexpr int kMaxMembers = 65536;
        constexpr std::size_t kMaxJitMisses = 8192;

        const char* const kFunctions[] = {
            "mono_get_root_domain", "mono_thread_attach", "mono_assembly_foreach", "mono_assembly_get_image",
            "mono_image_get_name", "mono_image_get_table_info", "mono_table_info_get_rows", "mono_class_get",
            "mono_class_get_name", "mono_class_get_namespace", "mono_class_get_methods", "mono_method_get_name",
            "mono_compile_method", "mono_class_get_fields", "mono_field_get_name", "mono_field_get_offset",
            "mono_field_get_type", "mono_type_get_type", "mono_field_get_flags", "mono_object_get_class",
            "mono_jit_info_table_find", "mono_jit_info_get_method", "mono_jit_info_get_code_start",
            "mono_jit_info_get_code_size", "mono_method_get_class",
        };

        const char* const kModuleCandidates[] = { "mono-2.0-bdwgc.dll", "mono.dll", "mono-2.0.dll", "monobdwgc-2.0.dll" };
    }

    std::uint64_t MonoBridge::Fn(const char* name) const
    {
        const auto found = m_functions.find(name);
        return found == m_functions.end() ? 0 : found->second;
    }

    std::optional<std::uint64_t> MonoBridge::CallFn(const char* name, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3)
    {
        const std::uint64_t fn = Fn(name);
        if (!fn)
            return std::nullopt;
        std::uint64_t result = 0;
        if (!m_call.Call(fn, a0, a1, a2, a3, result))
            return std::nullopt;
        return result;
    }

    std::string MonoBridge::CallString(const char* name, std::uint64_t handle)
    {
        const auto pointer = CallFn(name, handle);
        if (!pointer || !*pointer)
            return {};
        return m_call.ReadCString(*pointer);
    }

    Status MonoBridge::Start(std::shared_ptr<Process> process)
    {
        Stop();
        std::lock_guard lock(m_lock);
        if (!process || !process->Alive())
            return Status::Fail(ErrorCode::NoProcess);
        if (!process->Is64Bit())
            return Status::Fail(ErrorCode::MonoNotFound, "only 64-bit Mono targets are supported");

        std::string module;
        std::uint64_t root = 0;
        for (const char* candidate : kModuleCandidates)
        {
            if (process->FindModule(candidate) && process->FindExport(candidate, "mono_get_root_domain"))
            {
                module = candidate;
                break;
            }
        }
        if (module.empty())
        {
            // Fall back to scanning every module for the export (covers an
            // embedded runtime or a renamed test target).
            if (const auto modules = process->Modules())
            {
                for (const ModuleInfo& info : *modules)
                {
                    if (process->FindExport(info.Name, "mono_get_root_domain"))
                    {
                        module = info.Name;
                        break;
                    }
                }
            }
        }
        if (module.empty())
            return Status::Fail(ErrorCode::MonoNotFound);

        m_process = process;
        std::map<std::string, std::uint64_t> functions;
        for (const char* name : kFunctions)
        {
            const std::uint64_t address = process->FindExport(module, name);
            if (address)
                functions[name] = address;
        }
        if (!functions.count("mono_get_root_domain") || !functions.count("mono_class_get_methods"))
        {
            m_process.reset();
            return Status::Fail(ErrorCode::MonoNotFound, module + " is missing expected functions");
        }

        if (Status started = m_call.Start(process); !started)
        {
            m_process.reset();
            return started;
        }
        m_functions = std::move(functions);
        m_module = module;

        root = CallFn("mono_get_root_domain").value_or(0);
        if (!root)
        {
            m_call.Stop();
            m_functions.clear();
            m_process.reset();
            return Status::Fail(ErrorCode::MonoNotFound, "the root domain was null");
        }
        m_domain = root;
        CallFn("mono_thread_attach", root);
        m_present = true;
        ++m_generation;
        LOG_SUCCESS("Mono", "Mono runtime found in " + module);
        return Refresh();
    }

    void MonoBridge::Stop()
    {
        std::lock_guard lock(m_lock);
        m_call.Stop();
        m_functions.clear();
        m_classes.clear();
        m_classIndex.clear();
        m_handleIndex.clear();
        m_jitRanges.clear();
        m_jitMisses.clear();
        m_present = false;
        m_domain = 0;
        m_module.clear();
        m_process.reset();
        ++m_generation;
    }

    Status MonoBridge::Refresh()
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_process)
            return Status::Fail(ErrorCode::MonoNotFound);
        m_classes.clear();
        m_classIndex.clear();
        m_handleIndex.clear();
        m_handleMin = m_handleMax = 0;
        m_jitRanges.clear();
        m_jitMisses.clear();
        m_call.ResetScratch();

        // mono_assembly_foreach collects each assembly pointer into an array.
        constexpr int kMaxAssemblies = 4096;
        const std::uint64_t array = m_call.Scratch(static_cast<std::size_t>(16 + kMaxAssemblies * 8));
        if (!array)
            return Status::Fail(ErrorCode::MonoNotFound, "no scratch space");
        const std::uint64_t cap = kMaxAssemblies;
        m_process->Write(array + 8, &cap, sizeof(cap));
        CallFn("mono_assembly_foreach", m_call.CollectorAddress(), array);
        std::uint64_t count = 0;
        m_process->ReadExact(array, &count, sizeof(count));
        count = std::min<std::uint64_t>(count, kMaxAssemblies);
        std::vector<std::uint64_t> assemblies(static_cast<std::size_t>(count));
        if (count)
            m_process->ReadExact(array + 16, assemblies.data(), static_cast<std::size_t>(count) * 8);

        std::vector<std::pair<std::uint64_t, std::string>> images;  // image handle, name
        for (std::uint64_t assembly : assemblies)
        {
            const auto image = CallFn("mono_assembly_get_image", assembly);
            if (image && *image)
                images.emplace_back(*image, CallString("mono_image_get_name", *image));
        }
        m_call.ResetScratch();

        for (const auto& [image, imageName] : images)
        {
            const auto tableInfo = CallFn("mono_image_get_table_info", image, kTableTypeDef);
            const auto rows = CallFn("mono_table_info_get_rows", tableInfo.value_or(0));
            const int rowCount = std::min(kMaxClasses, static_cast<int>(rows.value_or(0) & 0xFFFFFFFFu));
            for (int row = 1; row <= rowCount; ++row)
            {
                const auto klass = CallFn("mono_class_get", image, kTokenTypeDef | static_cast<unsigned int>(row));
                if (!klass || !*klass)
                    continue;
                MonoClass info;
                info.Handle = *klass;
                info.Image = imageName;
                info.Name = CallString("mono_class_get_name", *klass);
                info.Namespace = CallString("mono_class_get_namespace", *klass);
                if (info.Name.empty() || info.Name == "<Module>")
                    continue;
                const int classIndex = static_cast<int>(m_classes.size());
                m_classIndex.emplace(info.Name, classIndex);
                if (!info.Namespace.empty())
                    m_classIndex.emplace(info.FullName(), classIndex);
                m_handleIndex.emplace(info.Handle, classIndex);
                if (m_handleMin == 0 || info.Handle < m_handleMin)
                    m_handleMin = info.Handle;
                if (info.Handle > m_handleMax)
                    m_handleMax = info.Handle;
                m_classes.push_back(std::move(info));
            }
            m_call.ResetScratch();
        }
        ++m_generation;
        LOG_INFO("Mono", "Read " + std::to_string(m_classes.size()) + " classes from " + m_module);
        return Status::Success();
    }

    int MonoBridge::FindClass(std::string_view name) const
    {
        const auto found = m_classIndex.find(std::string(name));
        return found == m_classIndex.end() ? -1 : found->second;
    }

    void MonoBridge::EnsureMembers(int classIndex)
    {
        if (classIndex < 0 || classIndex >= static_cast<int>(m_classes.size()))
            return;
        MonoClass& info = m_classes[static_cast<std::size_t>(classIndex)];
        if (info.MembersLoaded)
            return;
        info.MembersLoaded = true;
        m_call.ResetScratch();
        const std::uint64_t methodIter = m_call.Scratch(8);
        for (int n = 0; n < kMaxMembers; ++n)
        {
            const auto method = CallFn("mono_class_get_methods", info.Handle, methodIter);
            if (!method || !*method)
                break;
            MonoMethod entry;
            entry.Handle = *method;
            entry.Name = CallString("mono_method_get_name", *method);
            info.Methods.push_back(std::move(entry));
        }
        const std::uint64_t fieldIter = m_call.Scratch(8);
        for (int n = 0; n < kMaxMembers; ++n)
        {
            const auto field = CallFn("mono_class_get_fields", info.Handle, fieldIter);
            if (!field || !*field)
                break;
            const std::uint32_t flags = static_cast<std::uint32_t>(CallFn("mono_field_get_flags", *field).value_or(0) & 0xFFFFFFFFu);
            if (flags & 0x10u)  // FIELD_ATTRIBUTE_STATIC: not part of an instance
                continue;
            MonoField entry;
            entry.Name = CallString("mono_field_get_name", *field);
            entry.Offset = static_cast<int>(CallFn("mono_field_get_offset", *field).value_or(0) & 0xFFFFFFFFu);
            if (const auto type = CallFn("mono_field_get_type", *field); type && *type)
                entry.TypeCode = static_cast<int>(CallFn("mono_type_get_type", *type).value_or(0) & 0xFFFFFFFFu);
            info.Fields.push_back(std::move(entry));
        }
        m_call.ResetScratch();
    }

    std::vector<std::string> MonoBridge::MethodsOf(std::string_view className)
    {
        std::lock_guard lock(m_lock);
        const int index = FindClass(className);
        if (index < 0)
            return {};
        EnsureMembers(index);
        std::vector<std::string> names;
        for (const MonoMethod& method : m_classes[static_cast<std::size_t>(index)].Methods)
            names.push_back(method.Name);
        return names;
    }

    std::vector<MonoField> MonoBridge::FieldsOf(std::string_view className)
    {
        std::lock_guard lock(m_lock);
        const int index = FindClass(className);
        if (index < 0)
            return {};
        EnsureMembers(index);
        return m_classes[static_cast<std::size_t>(index)].Fields;
    }

    std::vector<MonoClass> MonoBridge::Classes() const
    {
        std::lock_guard lock(m_lock);
        return m_classes;
    }

    std::size_t MonoBridge::ClassCount() const
    {
        std::lock_guard lock(m_lock);
        return m_classes.size();
    }

    std::optional<std::string> MonoBridge::ResolveHead(std::uint64_t header, bool allowRuntimeFallback)
    {
        // Read the object -> MonoVTable -> MonoClass chain ourselves with safe
        // reads (both pointers are at offset 0) rather than calling
        // mono_object_get_class in the target, which would dereference a wild
        // pointer and could crash the game when the address is not an object.
        const auto vtable = m_process->ReadPointer(header);
        if (!vtable || *vtable < 0x10000)
            return std::nullopt;
        const auto klass = m_process->ReadPointer(*vtable);
        if (!klass || !*klass)
            return std::nullopt;
        const auto found = m_handleIndex.find(*klass);
        if (found != m_handleIndex.end())
            return m_classes[static_cast<std::size_t>(found->second)].FullName();
        // Not in the enumerated set (a generic or inflated class). Trust the
        // klass only when it sits in the same pointer span as the known classes
        // and is readable, then ask the runtime for its name and remember it.
        // Only for the exact address the caller gave, not during a back scan.
        if (!allowRuntimeFallback || !m_handleMin || *klass < m_handleMin || *klass > m_handleMax || !m_process->IsReadable(*klass))
            return std::nullopt;
        const std::string name = CallString("mono_class_get_name", *klass);
        if (name.empty() || name.size() > 256)
            return std::nullopt;
        const char first = name.front();
        if (!(std::isalpha(static_cast<unsigned char>(first)) || first == '_' || first == '<'))
            return std::nullopt;
        MonoClass dynamic;
        dynamic.Handle = *klass;
        dynamic.Name = name;
        dynamic.Namespace = CallString("mono_class_get_namespace", *klass);
        const std::string full = dynamic.FullName();
        const int index = static_cast<int>(m_classes.size());
        m_classIndex.emplace(dynamic.Name, index);
        if (!dynamic.Namespace.empty())
            m_classIndex.emplace(full, index);
        m_handleIndex.emplace(*klass, index);
        m_classes.push_back(std::move(dynamic));
        return full;
    }

    std::optional<std::string> MonoBridge::ClassNameAt(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_process || !address)
            return std::nullopt;
        return ResolveHead(address, true);
    }

    std::optional<MonoInstance> MonoBridge::ClassAt(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_process || !address)
            return std::nullopt;
        // The address is often a field inside an object, not its header. Try the
        // address itself first, then step back a little (8-byte aligned) for the
        // enclosing object's header. The runtime fallback is allowed only for the
        // exact address; the back scan keeps to the enumerated classes so a stray
        // field value cannot be mistaken for an object.
        constexpr std::uint64_t kMaxBack = 0x1000;
        const std::uint64_t aligned = address & ~std::uint64_t(7);
        for (std::uint64_t back = 0; back <= kMaxBack; back += 8)
        {
            if (aligned < back + 0x10000)
                break;
            const std::uint64_t header = aligned - back;
            if (const auto name = ResolveHead(header, back == 0))
                return MonoInstance{ header, *name };
        }
        return std::nullopt;
    }

    const std::string* MonoBridge::FindJitName(std::uint64_t address) const
    {
        // m_jitRanges is sorted by Start and non-overlapping: step back to the
        // last range that begins at or before the address and test its end.
        auto it = std::upper_bound(m_jitRanges.begin(), m_jitRanges.end(), address,
            [](std::uint64_t value, const JitRange& range) { return value < range.Start; });
        if (it == m_jitRanges.begin())
            return nullptr;
        --it;
        return address >= it->Start && address < it->End ? &it->Name : nullptr;
    }

    void MonoBridge::RememberJitRange(std::uint64_t start, std::uint64_t end, std::string name)
    {
        auto it = std::lower_bound(m_jitRanges.begin(), m_jitRanges.end(), start,
            [](const JitRange& range, std::uint64_t value) { return range.Start < value; });
        if (it != m_jitRanges.end() && it->Start == start)
            return;  // already cached
        m_jitRanges.insert(it, JitRange{ start, end, std::move(name) });
    }

    void MonoBridge::RememberJitMiss(std::uint64_t address)
    {
        // Bound the negative cache: scrolling through non-Mono code must not grow
        // it without limit, and dropping it only re-probes a few addresses later.
        if (m_jitMisses.size() >= kMaxJitMisses)
            m_jitMisses.clear();
        m_jitMisses.insert(address);
    }

    std::optional<std::string> MonoBridge::MethodNameAt(std::uint64_t address)
    {
        std::lock_guard lock(m_lock);
        if (!m_present || !m_process || !address)
            return std::nullopt;
        // The disassembler asks per line every frame, so the cache answers first:
        // a resolved range, or a known miss, both without touching the runtime.
        if (const std::string* cached = FindJitName(address))
            return *cached;
        if (m_jitMisses.count(address))
            return std::nullopt;
        // A miss costs one remote lookup. mono_jit_info_table_find reports null
        // for any address that is not JITed Mono code, so it is safe on the wild
        // pointers the disassembler hands it.
        const auto info = CallFn("mono_jit_info_table_find", m_domain, address);
        if (!info || !*info)
        {
            RememberJitMiss(address);
            return std::nullopt;
        }
        const auto method = CallFn("mono_jit_info_get_method", *info);
        const auto codeStart = CallFn("mono_jit_info_get_code_start", *info);
        const auto codeSize = CallFn("mono_jit_info_get_code_size", *info);
        if (!method || !*method || !codeStart || !*codeStart || !codeSize)
        {
            RememberJitMiss(address);
            return std::nullopt;
        }
        const std::string methodName = CallString("mono_method_get_name", *method);
        std::string className;
        if (const auto klass = CallFn("mono_method_get_class", *method); klass && *klass)
        {
            const std::string ns = CallString("mono_class_get_namespace", *klass);
            const std::string name = CallString("mono_class_get_name", *klass);
            className = ns.empty() ? name : ns + "." + name;
        }
        if (methodName.empty() && className.empty())
        {
            RememberJitMiss(address);
            return std::nullopt;
        }
        std::string full = className.empty() ? methodName : className + ":" + methodName;
        RememberJitRange(*codeStart, *codeStart + (*codeSize & 0xFFFFFFFFu), full);
        return full;
    }

    std::optional<std::uint64_t> MonoBridge::AddressOf(std::string_view name)
    {
        std::lock_guard lock(m_lock);
        if (!m_present)
            return std::nullopt;
        const std::size_t colon = name.rfind(':');
        if (colon == std::string_view::npos || colon == 0 || colon + 1 >= name.size())
            return std::nullopt;
        const int index = FindClass(name.substr(0, colon));
        if (index < 0)
            return std::nullopt;
        EnsureMembers(index);
        const std::string_view methodName = name.substr(colon + 1);
        for (MonoMethod& method : m_classes[static_cast<std::size_t>(index)].Methods)
        {
            if (method.Name != methodName)
                continue;
            if (method.Address)
                return method.Address;
            const auto address = CallFn("mono_compile_method", method.Handle);
            if (!address || !*address)
                return std::nullopt;
            method.Address = *address;
            return method.Address;
        }
        return std::nullopt;
    }

    std::optional<int> MonoBridge::FieldOffset(std::string_view className, std::string_view fieldName)
    {
        std::lock_guard lock(m_lock);
        const int index = FindClass(className);
        if (index < 0)
            return std::nullopt;
        EnsureMembers(index);
        for (const MonoField& field : m_classes[static_cast<std::size_t>(index)].Fields)
        {
            if (field.Name == fieldName)
                return field.Offset;
        }
        return std::nullopt;
    }
}
