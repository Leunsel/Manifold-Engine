#include "Target/Mono.h"

#include "Core/BackgroundWork.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/CodeMap.h"
#include "Target/Process.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>

// The IL2CPP side of MonoBridge. IL2CPP compiles every method ahead of time
// into GameAssembly.dll, so there is no JIT: a method's code address is read
// from its MethodInfo, and an address is named by looking it up in a sorted
// table of every method's code. The worker only makes metadata calls here
// (no il2cpp_runtime_invoke, no main thread, no GC handles).
namespace Manifold::Target
{
    namespace
    {
        constexpr std::uint64_t kMaxAssemblies = 4096;
        constexpr std::uint64_t kMaxClasses = 200000;
        constexpr std::size_t kMaxMembers = 65536;
        constexpr std::uint32_t kFieldStatic = 0x10;  // FIELD_ATTRIBUTE_STATIC
        // ∑ MethodInfo::methodPointer is the first field in every IL2CPP
        // version we know of (Unity 5 to 6; 2021.2 added virtualMethodPointer
        // after it). Used when the build has no il2cpp_method_get_pointer.
        constexpr std::uint32_t kMethodPointerOffset = 0;
        constexpr int kShapeWords = 8;
        constexpr std::size_t kShapeSamples = 6;

        const char* const kIl2CppFunctions[] = {
            "il2cpp_domain_get", "il2cpp_get_corlib", "il2cpp_thread_attach", "il2cpp_thread_detach", "il2cpp_domain_get_assemblies",
            "il2cpp_assembly_get_image", "il2cpp_image_get_name", "il2cpp_image_get_class_count", "il2cpp_image_get_class",
            "il2cpp_class_get_name", "il2cpp_class_get_namespace", "il2cpp_class_get_parent", "il2cpp_class_is_generic",
            "il2cpp_class_get_methods", "il2cpp_method_get_name", "il2cpp_method_get_param_count", "il2cpp_method_get_pointer",
            "il2cpp_class_get_fields", "il2cpp_field_get_name", "il2cpp_field_get_offset", "il2cpp_field_get_type",
            "il2cpp_field_get_flags", "il2cpp_type_get_type",
        };
        const char* const kIl2CppRequired[] = {
            "il2cpp_domain_get", "il2cpp_thread_attach", "il2cpp_domain_get_assemblies", "il2cpp_assembly_get_image",
            "il2cpp_image_get_class_count", "il2cpp_image_get_class", "il2cpp_class_get_name", "il2cpp_class_get_methods",
        };

        std::string StripDll(std::string name)
        {
            if (name.size() > 4 && EqualsNoCase(std::string_view(name).substr(name.size() - 4), ".dll"))
                name.resize(name.size() - 4);
            return name;
        }

        // A class or namespace name as IL2CPP stores it: printable, starting
        // like an identifier (or a compiler-made "<...>" name), UTF-8 allowed
        // for obfuscated games.
        bool PlausibleName(const std::string& name)
        {
            if (name.empty() || name.size() > 256)
                return false;
            const unsigned char first = static_cast<unsigned char>(name.front());
            if (!(std::isalpha(first) || first == '_' || first == '<' || first >= 0x80))
                return false;
            return std::all_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x20 && c != 0x7F; });
        }

        // The offset (8-byte aligned, in the first kShapeWords words) at which
        // every sampled struct holds the value the runtime returned for it,
        // e.g. klass->name for il2cpp_class_get_name. -1 when fewer than two
        // samples agree on one offset.
        int CommonOffset(const Process& process, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& samples)
        {
            unsigned candidates = (1u << kShapeWords) - 1;
            std::size_t used = 0;
            for (const auto& [base, value] : samples)
            {
                if (!base || !value)
                    continue;
                std::uint64_t words[kShapeWords] = {};
                if (!process.ReadExact(base, words, sizeof(words)))
                    return -1;
                unsigned here = 0;
                for (int i = 0; i < kShapeWords; ++i)
                {
                    if (words[i] == value)
                        here |= 1u << i;
                }
                candidates &= here;
                ++used;
            }
            if (used < 2 || candidates == 0)
                return -1;
            return std::countr_zero(candidates) * 8;
        }
    }

    Status MonoBridge::AttachIl2Cpp(const std::string& module)
    {
        std::map<std::string, std::uint64_t> functions;
        for (const char* name : kIl2CppFunctions)
        {
            const std::uint64_t address = m_process->FindExport(module, name);
            if (address)
                functions[name] = address;
        }
        for (const char* required : kIl2CppRequired)
        {
            if (!functions.count(required))
                return Status::Fail(ErrorCode::MonoNotFound, module + " has no " + required + " (an older or stripped IL2CPP build)");
        }
        m_functions = std::move(functions);
        // Before il2cpp_init has run there is no corlib yet, and
        // il2cpp_domain_get would make a domain of its own then; ask first.
        if (Fn("il2cpp_get_corlib") && !CallFn("il2cpp_get_corlib").value_or(0))
        {
            m_functions.clear();
            return Status::Fail(ErrorCode::MonoNotFound, "the IL2CPP runtime is not set up yet (try again once the game has loaded)");
        }
        const std::uint64_t domain = CallFn("il2cpp_domain_get").value_or(0);
        if (!domain)
        {
            m_functions.clear();
            return Status::Fail(ErrorCode::MonoNotFound, "the IL2CPP domain was null");
        }
        m_domain = domain;
        m_attached = CallFn("il2cpp_thread_attach", domain).value_or(0);
        m_module = module;
        m_runtime = ManagedRuntime::Il2Cpp;
        return Status::Success();
    }

    Status MonoBridge::EnumerateIl2Cpp(WorkProgress* progress, bool starting)
    {
        const auto stopped = [progress] { return progress && progress->Cancelled(); };
        const Status cancelled = Status::Fail(ErrorCode::MonoNotFound, "cancelled");
        if (progress)
            progress->SetText("Reading the assemblies");

        std::vector<std::uint64_t> assemblies;
        {
            std::lock_guard lock(m_lock);
            if (!m_process || !m_domain)
                return Status::Fail(ErrorCode::MonoNotFound);
            m_call.ResetScratch();
            const std::uint64_t size = m_call.Scratch(8);
            if (!size)
                return Status::Fail(ErrorCode::MonoNotFound, "no scratch space");
            const std::uint64_t array = CallFn("il2cpp_domain_get_assemblies", m_domain, size).value_or(0);
            std::uint64_t count = 0;
            m_process->ReadExact(size, &count, sizeof(count));
            m_call.ResetScratch();
            count = std::min(count, kMaxAssemblies);
            if (array && count)
            {
                assemblies.resize(static_cast<std::size_t>(count));
                if (!m_process->ReadExact(array, assemblies.data(), assemblies.size() * sizeof(std::uint64_t)))
                    assemblies.clear();
            }
        }
        if (assemblies.empty())
            return Status::Fail(ErrorCode::MonoNotFound, "the IL2CPP domain lists no assemblies");

        struct ImageInfo
        {
            std::uint64_t Handle = 0;
            std::string Name;
            std::uint64_t Classes = 0;
        };
        std::vector<ImageInfo> images;
        std::int64_t total = 0;
        for (const std::uint64_t assembly : assemblies)
        {
            if (stopped())
                return cancelled;
            std::lock_guard lock(m_lock);
            const auto image = CallFn("il2cpp_assembly_get_image", assembly);
            if (!image || !*image)
                continue;
            ImageInfo info;
            info.Handle = *image;
            info.Name = StripDll(CallString("il2cpp_image_get_name", *image));
            info.Classes = std::min(CallFn("il2cpp_image_get_class_count", *image).value_or(0), kMaxClasses);
            total += static_cast<std::int64_t>(info.Classes);
            images.push_back(std::move(info));
        }
        if (progress)
        {
            progress->SetSteps(0, total * 2);
            progress->SetText("Reading the classes");
        }

        // Names come through the runtime here (these Il2CppClass pointers are
        // the runtime's own answers); a few of them also teach where the name,
        // namespace and image sit in the struct, for objects of classes that
        // are not in any image list (generic instances, arrays).
        std::vector<MonoClass> classes;
        classes.reserve(static_cast<std::size_t>(total));
        std::vector<std::pair<std::uint64_t, std::uint64_t>> imageSamples;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> nameSamples;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> namespaceSamples;
        for (const ImageInfo& image : images)
        {
            int sampled = 0;
            for (std::uint64_t index = 0; index < image.Classes; ++index)
            {
                if ((index & 63) == 0 && stopped())
                    return cancelled;
                MonoClass info;
                std::uint64_t namePointer = 0;
                std::uint64_t namespacePointer = 0;
                {
                    std::lock_guard lock(m_lock);
                    const auto klass = CallFn("il2cpp_image_get_class", image.Handle, index);
                    if (klass && *klass)
                    {
                        info.Handle = *klass;
                        namePointer = CallFn("il2cpp_class_get_name", *klass).value_or(0);
                        info.Name = m_call.ReadCString(namePointer);
                        namespacePointer = CallFn("il2cpp_class_get_namespace", *klass).value_or(0);
                        info.Namespace = m_call.ReadCString(namespacePointer);
                    }
                }
                if (progress)
                    progress->Step();
                if (!info.Handle || info.Name.empty() || info.Name == "<Module>")
                    continue;
                if (nameSamples.size() < kShapeSamples * 2 && sampled < 2)
                {
                    imageSamples.emplace_back(info.Handle, image.Handle);
                    nameSamples.emplace_back(info.Handle, namePointer);
                    namespaceSamples.emplace_back(info.Handle, namespacePointer);
                    ++sampled;
                }
                info.Image = image.Name;
                classes.push_back(std::move(info));
            }
        }
        if (stopped())
            return cancelled;
        Il2CppShape shape;
        shape.ClassImage = CommonOffset(*m_process, imageSamples);
        shape.ClassName = CommonOffset(*m_process, nameSamples);
        shape.ClassNamespace = CommonOffset(*m_process, namespaceSamples);

        // Every method's code address, one drain per class: the stub walks
        // il2cpp_class_get_methods in the game and reads methodPointer
        // (or calls il2cpp_method_get_pointer) next to each MethodInfo*.
        if (progress)
            progress->SetText("Reading the method addresses");
        struct Raw
        {
            std::uint64_t Start = 0;
            std::uint64_t Method = 0;
            int Class = -1;
        };
        std::vector<Raw> raw;
        std::uint64_t moduleBase = 0;
        std::uint64_t moduleEnd = 0;
        {
            std::lock_guard lock(m_lock);
            moduleBase = m_moduleBase;
            moduleEnd = m_moduleEnd;
        }
        std::size_t methodCount = 0;
        std::vector<RemoteCall::DrainItem> items;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> methodSamples;
        for (int index = 0; index < static_cast<int>(classes.size()); ++index)
        {
            if ((index & 63) == 0 && stopped())
                return cancelled;
            {
                std::lock_guard lock(m_lock);
                const std::uint64_t pointerFn = Fn("il2cpp_method_get_pointer");
                if (!m_call.Drain(Fn("il2cpp_class_get_methods"), classes[static_cast<std::size_t>(index)].Handle, items,
                        pointerFn ? RemoteCall::kNoPeek : kMethodPointerOffset, pointerFn, kMaxMembers))
                    items.clear();
                if (!items.empty() && methodSamples.size() < kShapeSamples)
                {
                    if (const auto name = CallFn("il2cpp_method_get_name", items.front().Item); name && *name)
                        methodSamples.emplace_back(items.front().Item, *name);
                }
            }
            methodCount += items.size();
            for (const RemoteCall::DrainItem& item : items)
            {
                if (item.Peek >= moduleBase && item.Peek < moduleEnd)
                    raw.push_back({ item.Peek, item.Item, index });
            }
            if (progress)
                progress->Step();
        }
        if (stopped())
            return cancelled;
        shape.MethodName = CommonOffset(*m_process, methodSamples);

        // One entry per distinct code start; identical bodies that the
        // compiler folded share one, named after the first method listed.
        // A method ends at the next method, or earlier where the module's
        // unwind table says its function ends or the next function begins.
        std::stable_sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) { return a.Start < b.Start; });
        std::vector<CodeEntry> code;
        for (std::size_t i = 0; i < raw.size();)
        {
            std::size_t j = i;
            while (j < raw.size() && raw[j].Start == raw[i].Start)
                ++j;
            CodeEntry entry;
            entry.Start = raw[i].Start;
            entry.Method = raw[i].Method;
            entry.Class = raw[i].Class;
            entry.Sharing = static_cast<std::uint32_t>(j - i);
            code.push_back(std::move(entry));
            i = j;
        }
        const auto unwind = CodeMap::UnwindFunctions(*m_process, moduleBase, moduleEnd - moduleBase);
        for (std::size_t k = 0; k < code.size(); ++k)
        {
            std::uint64_t end = k + 1 < code.size() ? code[k + 1].Start : moduleEnd;
            const auto rva = static_cast<std::uint32_t>(code[k].Start - moduleBase);
            const auto next = std::upper_bound(unwind.begin(), unwind.end(), rva,
                [](std::uint32_t value, const std::pair<std::uint32_t, std::uint32_t>& function) { return value < function.first; });
            if (next != unwind.end())
                end = std::min(end, moduleBase + next->first);
            if (next != unwind.begin() && rva < std::prev(next)->second)
                end = std::min(end, moduleBase + std::prev(next)->second);
            code[k].End = std::max(end, code[k].Start + 1);
        }

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
        std::map<std::uint64_t, std::string> imageNames;
        for (const ImageInfo& image : images)
        {
            imageSet.insert(image.Handle);
            imageNames.emplace(image.Handle, image.Name);
        }
        const std::size_t count = classes.size();
        const std::size_t indexed = code.size();
        std::string module;
        {
            std::lock_guard lock(m_lock);
            m_classes = std::move(classes);
            m_classIndex = std::move(classIndex);
            m_handleIndex = std::move(handleIndex);
            m_handleMin = handleMin;
            m_handleMax = handleMax;
            m_images = std::move(imageSet);
            m_imageNames = std::move(imageNames);
            m_code = std::move(code);
            m_shape = shape;
            m_jitRanges.clear();
            m_jitMisses.clear();
            module = m_module;
            if (starting)
                m_present = true;
            ++m_generation;
        }
        LOG_INFO("Mono", Format("Read %zu classes and %zu methods from %s (%zu code addresses)", count, methodCount, module.c_str(), indexed));
        return Status::Success();
    }

    std::string MonoBridge::Il2CppMethodName(std::uint64_t method)
    {
        // MethodInfo::name read from memory where the runtime's answers showed
        // it, so naming code needs no call (and works while the game is held
        // at a breakpoint); the call is the fallback.
        if (m_shape.MethodName >= 0)
        {
            if (const auto pointer = m_process->ReadPointer(method + static_cast<std::uint64_t>(m_shape.MethodName)); pointer && *pointer)
            {
                std::string name = m_call.ReadCString(*pointer, 512);
                if (!name.empty())
                    return name;
            }
        }
        if (m_paused && m_paused())
            return {};
        return CallString("il2cpp_method_get_name", method);
    }

    void MonoBridge::EnsureMembersIl2Cpp(int classIndex)
    {
        MonoClass& info = m_classes[static_cast<std::size_t>(classIndex)];
        if (info.MembersLoaded)
            return;
        info.MembersLoaded = true;
        // A class known only from its shape in memory is never handed to the
        // runtime: a stale or wrong pointer there would crash the game inside it.
        if (info.Discovered)
            return;
        const std::uint64_t klass = info.Handle;
        if (const auto parent = CallFn("il2cpp_class_get_parent", klass); parent && *parent)
        {
            if (const auto known = m_handleIndex.find(*parent); known != m_handleIndex.end())
            {
                info.Parent = m_classes[static_cast<std::size_t>(known->second)].FullName();
            }
            else
            {
                // The runtime's own answer, so asking it about it is safe.
                const std::string name = CallString("il2cpp_class_get_name", *parent);
                const std::string ns = CallString("il2cpp_class_get_namespace", *parent);
                info.Parent = ns.empty() ? name : ns + "." + name;
            }
        }
        if (Fn("il2cpp_class_is_generic"))
            info.Generic = (CallFn("il2cpp_class_is_generic", klass).value_or(0) & 0xFFu) != 0;

        std::vector<RemoteCall::DrainItem> items;
        const std::uint64_t pointerFn = Fn("il2cpp_method_get_pointer");
        if (m_call.Drain(Fn("il2cpp_class_get_methods"), klass, items, pointerFn ? RemoteCall::kNoPeek : kMethodPointerOffset, pointerFn, kMaxMembers))
        {
            const bool counts = Fn("il2cpp_method_get_param_count") != 0;
            for (const RemoteCall::DrainItem& item : items)
            {
                MonoMethod method;
                method.Handle = item.Item;
                method.Address = item.Peek;
                method.Name = Il2CppMethodName(item.Item);
                if (counts)
                    method.ParamCount = static_cast<int>(CallFn("il2cpp_method_get_param_count", item.Item).value_or(0) & 0xFFFFu);
                info.Methods.push_back(std::move(method));
            }
        }
        // An open generic definition (List`1) has no field offsets of its own;
        // they depend on the type arguments of each instance.
        if (info.Generic || !Fn("il2cpp_class_get_fields"))
            return;
        if (!m_call.Drain(Fn("il2cpp_class_get_fields"), klass, items, RemoteCall::kNoPeek, 0, kMaxMembers))
            return;
        for (const RemoteCall::DrainItem& item : items)
        {
            const auto flags = static_cast<std::uint32_t>(CallFn("il2cpp_field_get_flags", item.Item).value_or(0) & 0xFFFFFFFFu);
            if (flags & kFieldStatic)  // not part of an instance (literals are static too)
                continue;
            MonoField field;
            field.Name = CallString("il2cpp_field_get_name", item.Item);
            field.Offset = static_cast<int>(CallFn("il2cpp_field_get_offset", item.Item).value_or(0) & 0xFFFFFFFFu);
            if (const auto type = CallFn("il2cpp_field_get_type", item.Item); type && *type)
                field.TypeCode = static_cast<int>(CallFn("il2cpp_type_get_type", *type).value_or(0) & 0xFFu);
            info.Fields.push_back(std::move(field));
        }
    }

    std::optional<std::string> MonoBridge::ResolveHeadIl2Cpp(std::uint64_t header, bool allowShape)
    {
        // An Il2CppObject starts with its Il2CppClass* (no vtable in between,
        // unlike Mono). Read with safe reads; a class pointer read from game
        // memory is only compared, never handed to the runtime.
        const auto klass = m_process->ReadPointer(header);
        if (!klass || *klass < 0x10000)
            return std::nullopt;
        if (const auto found = m_handleIndex.find(*klass); found != m_handleIndex.end())
            return m_classes[static_cast<std::size_t>(found->second)].FullName();
        // Not in any image list (a generic instance, an array): accepted by its
        // shape only, with the image one of the known ones and a readable,
        // plausible name, and only for the exact address the caller gave.
        if (!allowShape || m_shape.ClassName < 0 || m_shape.ClassImage < 0)
            return std::nullopt;
        const auto image = m_process->ReadPointer(*klass + static_cast<std::uint64_t>(m_shape.ClassImage));
        if (!image)
            return std::nullopt;
        const auto imageName = m_imageNames.find(*image);
        if (imageName == m_imageNames.end())
            return std::nullopt;
        const auto namePointer = m_process->ReadPointer(*klass + static_cast<std::uint64_t>(m_shape.ClassName));
        if (!namePointer || !*namePointer)
            return std::nullopt;
        const std::string name = m_call.ReadCString(*namePointer, 257);
        if (!PlausibleName(name))
            return std::nullopt;
        std::string ns;
        if (m_shape.ClassNamespace >= 0)
        {
            if (const auto pointer = m_process->ReadPointer(*klass + static_cast<std::uint64_t>(m_shape.ClassNamespace)); pointer && *pointer)
            {
                ns = m_call.ReadCString(*pointer, 257);
                if (!ns.empty() && !PlausibleName(ns))
                    return std::nullopt;
            }
        }
        MonoClass discovered;
        discovered.Handle = *klass;
        discovered.Name = name;
        discovered.Namespace = ns;
        discovered.Image = imageName->second;
        discovered.Discovered = true;
        discovered.MembersLoaded = true;
        const std::string full = discovered.FullName();
        const int index = static_cast<int>(m_classes.size());
        m_classIndex.emplace(discovered.Name, index);
        if (!discovered.Namespace.empty())
            m_classIndex.emplace(full, index);
        m_handleIndex.emplace(*klass, index);
        m_classes.push_back(std::move(discovered));
        return full;
    }

    MonoBridge::CodeEntry* MonoBridge::FindCode(std::uint64_t address)
    {
        if (m_code.empty() || address < m_moduleBase || address >= m_moduleEnd)
            return nullptr;
        auto it = std::upper_bound(m_code.begin(), m_code.end(), address, [](std::uint64_t value, const CodeEntry& entry) { return value < entry.Start; });
        if (it == m_code.begin())
            return nullptr;
        --it;
        return address < it->End ? &*it : nullptr;
    }

    const std::string& MonoBridge::CodeName(CodeEntry& entry)
    {
        if (!entry.Name.empty() || entry.Class < 0 || entry.Class >= static_cast<int>(m_classes.size()))
            return entry.Name;
        const std::string method = Il2CppMethodName(entry.Method);
        if (!method.empty())
            entry.Name = m_classes[static_cast<std::size_t>(entry.Class)].FullName() + ":" + method;
        return entry.Name;
    }
}
