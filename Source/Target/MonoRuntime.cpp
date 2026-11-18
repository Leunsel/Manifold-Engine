#include "Target/MonoRuntime.h"

#include "Core/BackgroundWork.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/Mono.h"
#include "Target/Process.h"

#include <algorithm>
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
            "mono_jit_info_get_code_size", "mono_method_get_class", "mono_class_get_image",
            // Used by the Unity explorer; a runtime without some of them still
            // loads, the explorer then reports what is missing.
            "mono_class_get_method_from_name", "mono_method_signature", "mono_signature_get_param_count",
            "mono_signature_get_params", "mono_signature_get_return_type", "mono_type_get_name", "mono_class_from_mono_type",
            "mono_class_get_type", "mono_type_get_object", "mono_gchandle_new", "mono_gchandle_free",
            "mono_gchandle_new_v2", "mono_gchandle_free_v2", "mono_runtime_invoke",
            "mono_object_new", "mono_string_new", "mono_class_get_parent", "mono_class_get_properties", "mono_property_get_name",
            "mono_property_get_get_method", "mono_property_get_set_method", "mono_method_get_flags", "mono_class_vtable",
            "mono_vtable_get_static_field_data", "mono_field_static_get_value", "mono_field_set_value", "mono_field_static_set_value",
            "mono_class_is_valuetype", "mono_class_is_enum", "mono_class_enum_basetype", "mono_class_value_size",
            "mono_class_get_element_class", "mono_array_element_size", "mono_class_get_rank", "mono_object_to_string", "mono_free",
            "mono_method_get_param_names",
        };
    }

    Status MonoRuntime::Attach(const std::string& module)
    {
        std::map<std::string, std::uint64_t> functions;
        for (const char* name : kFunctions)
        {
            const std::uint64_t address = m_bridge.m_process->FindExport(module, name);
            if (address)
                functions[name] = address;
        }
        if (!functions.count("mono_get_root_domain") || !functions.count("mono_class_get_methods"))
            return Status::Fail(ErrorCode::MonoNotFound, module + " is missing expected functions");
        m_bridge.m_functions = std::move(functions);
        const std::uint64_t root = m_bridge.CallFn("mono_get_root_domain").value_or(0);
        if (!root)
        {
            m_bridge.m_functions.clear();
            return Status::Fail(ErrorCode::MonoNotFound, "the root domain was null");
        }
        m_bridge.m_domain = root;
        m_bridge.m_module = module;
        m_bridge.m_runtime = ManagedRuntime::Mono;
        m_bridge.CallFn("mono_thread_attach", root);
        return Status::Success();
    }

    void MonoRuntime::Detach()
    {
        m_jitRanges.clear();
        m_jitMisses.clear();
    }

    std::optional<std::uint64_t> MonoRuntime::Compile(std::uint64_t method)
    {
        const auto address = m_bridge.CallFn("mono_compile_method", method);
        if (!address || !*address)
            return std::nullopt;
        return *address;
    }

    Status MonoRuntime::Enumerate(WorkProgress* progress, bool starting)
    {
        const auto stopped = [progress] { return progress && progress->Cancelled(); };
        const Status cancelled = Status::Fail(ErrorCode::MonoNotFound, "cancelled");
        if (progress)
            progress->SetText("Reading the assemblies");

        constexpr int kMaxAssemblies = 4096;
        std::vector<std::uint64_t> assemblies;
        {
            std::lock_guard lock(m_bridge.m_lock);
            if (!m_bridge.m_process || !m_bridge.m_domain)
                return Status::Fail(ErrorCode::MonoNotFound);
            m_bridge.m_call.ResetScratch();
            const std::uint64_t array = m_bridge.m_call.Scratch(static_cast<std::size_t>(16 + kMaxAssemblies * 8));
            if (!array)
                return Status::Fail(ErrorCode::MonoNotFound, "no scratch space");
            const std::uint64_t cap = kMaxAssemblies;
            m_bridge.m_process->Write(array + 8, &cap, sizeof(cap));
            m_bridge.CallFn("mono_assembly_foreach", m_bridge.m_call.CollectorAddress(), array);
            std::uint64_t count = 0;
            m_bridge.m_process->ReadExact(array, &count, sizeof(count));
            count = std::min<std::uint64_t>(count, kMaxAssemblies);
            assemblies.resize(static_cast<std::size_t>(count));
            if (count)
                m_bridge.m_process->ReadExact(array + 16, assemblies.data(), static_cast<std::size_t>(count) * 8);
            m_bridge.m_call.ResetScratch();
        }

        std::vector<std::pair<std::uint64_t, std::string>> images;
        for (std::uint64_t assembly : assemblies)
        {
            if (stopped())
                return cancelled;
            std::lock_guard lock(m_bridge.m_lock);
            const auto image = m_bridge.CallFn("mono_assembly_get_image", assembly);
            if (image && *image)
                images.emplace_back(*image, m_bridge.CallString("mono_image_get_name", *image));
        }

        std::vector<int> rows;
        std::int64_t total = 0;
        for (const auto& image : images)
        {
            std::lock_guard lock(m_bridge.m_lock);
            const auto tableInfo = m_bridge.CallFn("mono_image_get_table_info", image.first, kTableTypeDef);
            const auto count = m_bridge.CallFn("mono_table_info_get_rows", tableInfo.value_or(0));
            rows.push_back(std::min(kMaxClasses, static_cast<int>(count.value_or(0) & 0xFFFFFFFFu)));
            total += rows.back();
        }
        if (progress)
        {
            progress->SetSteps(0, total);
            progress->SetText("Reading the classes");
        }

        std::vector<MonoClass> classes;
        classes.reserve(static_cast<std::size_t>(total));
        for (std::size_t i = 0; i < images.size(); ++i)
        {
            for (int row = 1; row <= rows[i]; ++row)
            {
                if ((row & 63) == 0 && stopped())
                    return cancelled;
                MonoClass info;
                {
                    std::lock_guard lock(m_bridge.m_lock);
                    const auto klass = m_bridge.CallFn("mono_class_get", images[i].first, kTokenTypeDef | static_cast<unsigned int>(row));
                    if (klass && *klass)
                    {
                        info.Handle = *klass;
                        info.Name = m_bridge.CallString("mono_class_get_name", *klass);
                        info.Namespace = m_bridge.CallString("mono_class_get_namespace", *klass);
                    }
                }
                if (progress)
                    progress->Step();
                if (!info.Handle || info.Name.empty() || info.Name == "<Module>")
                    continue;
                info.Image = images[i].second;
                classes.push_back(std::move(info));
            }
        }
        if (stopped())
            return cancelled;

        std::map<std::string, int> classIndex;
        std::map<std::uint64_t, int> handleIndex;
        std::uint64_t handleMin = 0;
        std::uint64_t handleMax = 0;
        for (int index = 0; index < static_cast<int>(classes.size()); ++index)
        {
            const MonoClass& info = classes[static_cast<std::size_t>(index)];
            classIndex.emplace(info.Name, index);
            if (!info.Namespace.empty())
                classIndex.emplace(info.FullName(), index);
            handleIndex.emplace(info.Handle, index);
            if (handleMin == 0 || info.Handle < handleMin)
                handleMin = info.Handle;
            if (info.Handle > handleMax)
                handleMax = info.Handle;
        }
        std::unordered_set<std::uint64_t> imageSet;
        for (const auto& image : images)
            imageSet.insert(image.first);
        const std::size_t count = classes.size();
        std::string module;
        {
            std::lock_guard lock(m_bridge.m_lock);
            m_bridge.m_classes = std::move(classes);
            m_bridge.m_classIndex = std::move(classIndex);
            m_bridge.m_handleIndex = std::move(handleIndex);
            m_bridge.m_handleMin = handleMin;
            m_bridge.m_handleMax = handleMax;
            m_bridge.m_images = std::move(imageSet);
            m_jitRanges.clear();
            m_jitMisses.clear();
            module = m_bridge.m_module;
            if (starting)
                m_bridge.m_present = true;
            ++m_bridge.m_generation;
        }
        LOG_INFO("Mono", "Read " + std::to_string(count) + " classes from " + module);
        return Status::Success();
    }

    void MonoRuntime::EnsureMembers(int classIndex)
    {
        if (classIndex < 0 || classIndex >= static_cast<int>(m_bridge.m_classes.size()))
            return;
        MonoClass& info = m_bridge.m_classes[static_cast<std::size_t>(classIndex)];
        if (info.MembersLoaded)
            return;
        info.MembersLoaded = true;
        m_bridge.m_call.ResetScratch();
        const std::uint64_t methodIter = m_bridge.m_call.Scratch(8);
        for (int n = 0; n < kMaxMembers; ++n)
        {
            const auto method = m_bridge.CallFn("mono_class_get_methods", info.Handle, methodIter);
            if (!method || !*method)
                break;
            MonoMethod entry;
            entry.Handle = *method;
            entry.Name = m_bridge.CallString("mono_method_get_name", *method);
            info.Methods.push_back(std::move(entry));
        }
        const std::uint64_t fieldIter = m_bridge.m_call.Scratch(8);
        for (int n = 0; n < kMaxMembers; ++n)
        {
            const auto field = m_bridge.CallFn("mono_class_get_fields", info.Handle, fieldIter);
            if (!field || !*field)
                break;
            const std::uint32_t flags = static_cast<std::uint32_t>(m_bridge.CallFn("mono_field_get_flags", *field).value_or(0) & 0xFFFFFFFFu);
            if (flags & 0x10u)  // FIELD_ATTRIBUTE_STATIC: not part of an instance
                continue;
            MonoField entry;
            entry.Name = m_bridge.CallString("mono_field_get_name", *field);
            entry.Offset = static_cast<int>(m_bridge.CallFn("mono_field_get_offset", *field).value_or(0) & 0xFFFFFFFFu);
            if (const auto type = m_bridge.CallFn("mono_field_get_type", *field); type && *type)
                entry.TypeCode = static_cast<int>(m_bridge.CallFn("mono_type_get_type", *type).value_or(0) & 0xFFFFFFFFu);
            info.Fields.push_back(std::move(entry));
        }
        m_bridge.m_call.ResetScratch();
    }

    std::optional<std::string> MonoRuntime::ResolveHead(std::uint64_t header, bool allowRuntimeFallback)
    {
        // Read the object -> MonoVTable -> MonoClass chain ourselves with safe
        // reads (both pointers are at offset 0) rather than calling
        // mono_object_get_class in the target, which would dereference a wild
        // pointer and could crash the game when the address is not an object.
        const auto vtable = m_bridge.m_process->ReadPointer(header);
        if (!vtable || *vtable < 0x10000)
            return std::nullopt;
        const auto klass = m_bridge.m_process->ReadPointer(*vtable);
        if (!klass || !*klass)
            return std::nullopt;
        const auto found = m_bridge.m_handleIndex.find(*klass);
        if (found != m_bridge.m_handleIndex.end())
            return m_bridge.m_classes[static_cast<std::size_t>(found->second)].FullName();
        // Not in the enumerated set (a generic or inflated class). Trust the
        // klass only when it sits in the same pointer span as the known classes
        // and is readable, then ask the runtime for its name and remember it.
        // Only for the exact address the caller gave, not during a back scan.
        if (!allowRuntimeFallback || !m_bridge.m_handleMin || *klass < m_bridge.m_handleMin || *klass > m_bridge.m_handleMax || !m_bridge.m_process->IsReadable(*klass))
            return std::nullopt;
        const std::string name = m_bridge.CallString("mono_class_get_name", *klass);
        if (name.empty() || name.size() > 256)
            return std::nullopt;
        const char first = name.front();
        if (!(std::isalpha(static_cast<unsigned char>(first)) || first == '_' || first == '<'))
            return std::nullopt;
        MonoClass dynamic;
        dynamic.Handle = *klass;
        dynamic.Name = name;
        dynamic.Namespace = m_bridge.CallString("mono_class_get_namespace", *klass);
        const std::string full = dynamic.FullName();
        const int index = static_cast<int>(m_bridge.m_classes.size());
        m_bridge.m_classIndex.emplace(dynamic.Name, index);
        if (!dynamic.Namespace.empty())
            m_bridge.m_classIndex.emplace(full, index);
        m_bridge.m_handleIndex.emplace(*klass, index);
        m_bridge.m_classes.push_back(std::move(dynamic));
        return full;
    }

    const std::string* MonoRuntime::FindJitName(std::uint64_t address) const
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

    void MonoRuntime::RememberJitRange(std::uint64_t start, std::uint64_t end, std::string name)
    {
        auto it = std::lower_bound(m_jitRanges.begin(), m_jitRanges.end(), start,
            [](const JitRange& range, std::uint64_t value) { return range.Start < value; });
        if (it != m_jitRanges.end() && it->Start == start)
            return;  // already cached
        m_jitRanges.insert(it, JitRange{ start, end, std::move(name) });
    }

    void MonoRuntime::RememberJitMiss(std::uint64_t address)
    {
        // Bound the negative cache: scrolling through non-Mono code must not grow
        // it without limit, and dropping it only re-probes a few addresses later.
        if (m_jitMisses.size() >= kMaxJitMisses)
            m_jitMisses.clear();
        m_jitMisses.insert(address);
    }

    std::optional<std::string> MonoRuntime::MethodNameAt(std::uint64_t address)
    {
        // The disassembler asks per line every frame, so the cache answers first:
        // a resolved range, or a known miss, both without touching the runtime.
        if (const std::string* cached = FindJitName(address))
            return *cached;
        if (m_jitMisses.count(address))
            return std::nullopt;
        // JIT code is never inside the game's native modules, so those (the game
        // executable and its DLLs) are answered here without a remote call. The
        // runtime's own module is still asked, for a runtime embedded in the exe.
        if (const auto module = m_bridge.m_process->ModuleAt(address); module && !EqualsNoCase(module->Name, m_bridge.m_module))
            return std::nullopt;
        // Not remembered as a miss: after the break the address is asked again.
        if (m_bridge.m_paused && m_bridge.m_paused())
            return std::nullopt;
        // A miss costs one remote lookup. mono_jit_info_table_find reports null
        // for any address that is not JITed Mono code, so it is safe on the wild
        // pointers the disassembler hands it.
        const auto info = m_bridge.CallFn("mono_jit_info_table_find", m_bridge.m_domain, address);
        if (!info || !*info)
        {
            RememberJitMiss(address);
            return std::nullopt;
        }
        const auto method = m_bridge.CallFn("mono_jit_info_get_method", *info);
        const auto codeStart = m_bridge.CallFn("mono_jit_info_get_code_start", *info);
        const auto codeSize = m_bridge.CallFn("mono_jit_info_get_code_size", *info);
        if (!method || !*method || !codeStart || !*codeStart || !codeSize)
        {
            RememberJitMiss(address);
            return std::nullopt;
        }
        const std::string methodName = m_bridge.CallString("mono_method_get_name", *method);
        std::string className;
        if (const auto klass = m_bridge.CallFn("mono_method_get_class", *method); klass && *klass)
        {
            const std::string ns = m_bridge.CallString("mono_class_get_namespace", *klass);
            const std::string name = m_bridge.CallString("mono_class_get_name", *klass);
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

    std::optional<std::string> MonoRuntime::MethodStartNameAt(std::uint64_t address)
    {
        MethodNameAt(address);  // resolve and cache the containing method's range
        auto it = std::lower_bound(m_jitRanges.begin(), m_jitRanges.end(), address,
            [](const JitRange& range, std::uint64_t value) { return range.Start < value; });
        if (it != m_jitRanges.end() && it->Start == address)
            return it->Name;
        return std::nullopt;
    }

    std::optional<std::pair<std::uint64_t, std::uint64_t>> MonoRuntime::MethodRange(std::uint64_t address)
    {
        MethodNameAt(address);  // resolve and cache the containing method's range
        auto it = std::upper_bound(m_jitRanges.begin(), m_jitRanges.end(), address,
            [](std::uint64_t value, const JitRange& range) { return value < range.Start; });
        if (it == m_jitRanges.begin())
            return std::nullopt;
        --it;
        if (address >= it->Start && address < it->End)
            return std::make_pair(it->Start, it->End);
        return std::nullopt;
    }
}
