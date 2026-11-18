#include "Target/MonoRuntime.h"

#include "Core/BackgroundWork.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/Mono.h"
#include "Target/Process.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

#include <windows.h>

namespace Manifold::Target
{
    namespace
    {
        constexpr unsigned int kTableTypeDef = 2;
        constexpr unsigned int kTokenTypeDef = 0x02000000u;
        constexpr int kMaxClasses = 200000;
        constexpr int kMaxMembers = 65536;
        constexpr std::size_t kMaxJitMisses = 8192;
        constexpr auto kDumperBudget = std::chrono::seconds(180);
        constexpr auto kDumperPoll = std::chrono::milliseconds(20);
        constexpr std::int64_t kMaxDumpBytes = std::int64_t(1) << 30;
        constexpr std::size_t kClassSamples = 8;
        constexpr std::size_t kMemberSamples = 32;

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
            "mono_image_open_from_data", "mono_assembly_load_from_full", "mono_class_from_name", "mono_class_get_field_from_name",
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
        if (!(m_bridge.m_paused && m_bridge.m_paused()))
            StopDumper();
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
            if (IsDumperImage(image.second))
            {
                rows.push_back(0);
                continue;
            }
            std::lock_guard lock(m_bridge.m_lock);
            const auto tableInfo = m_bridge.CallFn("mono_image_get_table_info", image.first, kTableTypeDef);
            const auto count = m_bridge.CallFn("mono_table_info_get_rows", tableInfo.value_or(0));
            rows.push_back(std::min(kMaxClasses, static_cast<int>(count.value_or(0) & 0xFFFFFFFFu)));
            total += rows.back();
        }
        std::vector<MonoClass> classes;
        bool dumped = false;
        if (auto viaDumper = DumpWithAgent(images, total, progress))
        {
            classes = std::move(*viaDumper);
            dumped = true;
        }
        if (stopped())
            return cancelled;
        if (!dumped && progress)
        {
            progress->SetSteps(0, total);
            progress->SetText("Reading the classes");
        }

        if (!dumped)
            classes.reserve(static_cast<std::size_t>(total));
        for (std::size_t i = 0; i < images.size() && !dumped; ++i)
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
            m_bridge.m_readByDumper = dumped;
            m_jitRanges.clear();
            m_jitMisses.clear();
            module = m_bridge.m_module;
            if (starting)
                m_bridge.m_present = true;
            ++m_bridge.m_generation;
        }
        LOG_INFO("Mono", "Read " + std::to_string(count) + " classes from " + module + (dumped ? " through the managed dumper" : ""));
        return Status::Success();
    }

    std::optional<std::vector<MonoClass>> MonoRuntime::DumpWithAgent(const ImageList& images, std::int64_t total, WorkProgress* progress)
    {
        if (!m_bridge.m_useDumper.load() || m_dumperBroken || ManagedDumperImage().empty() || (m_bridge.m_paused && m_bridge.m_paused()))
            return std::nullopt;
        if (progress)
            progress->SetText("Loading the managed dumper");
        if (const Status loaded = LoadDumper(images); !loaded)
        {
            m_dumperBroken = true;
            LOG_INFO("Mono", "The managed dumper is not used (" + loaded.Text + "), reading the classes through the runtime");
            return std::nullopt;
        }
        Result<ManagedDump> dump = RunDumper(total, progress);
        if (!dump)
        {
            if (progress && progress->Cancelled())
                return std::nullopt;
            m_dumperBroken = true;
            LOG_WARN("Mono", "The managed dumper failed (" + dump.Error().Text + "), reading the classes through the runtime");
            return std::nullopt;
        }
        if (!CheckDump(images, *dump))
        {
            m_dumperBroken = true;
            return std::nullopt;
        }
        NameNestedTypes(*dump, progress);
        if (progress && progress->Cancelled())
            return std::nullopt;
        return std::move(dump->Classes);
    }

    Status MonoRuntime::LoadDumper(const ImageList& images)
    {
        if (m_dumper.Class)
            return Status::Success();
        for (const char* name : { "mono_class_from_name", "mono_class_get_method_from_name", "mono_runtime_invoke", "mono_class_vtable",
                 "mono_class_get_field_from_name", "mono_field_static_get_value", "mono_field_static_set_value" })
        {
            if (!m_bridge.Fn(name))
                return Status::Fail(ErrorCode::MonoCallFailed, std::string("the runtime has no ") + name);
        }
        const std::span<const std::uint8_t> bytes = ManagedDumperImage();
        std::lock_guard lock(m_bridge.m_lock);
        if (!m_bridge.m_process)
            return Status::Fail(ErrorCode::NoProcess);
        Process& process = *m_bridge.m_process;
        RemoteCall& call = m_bridge.m_call;
        const auto text = [&](std::string_view value) -> std::uint64_t
            {
                const std::uint64_t at = call.Scratch(value.size() + 1);
                if (at && !value.empty())
                    process.Write(at, value.data(), value.size());
                return at;
            };
        call.ResetScratch();
        std::uint64_t image = 0;
        for (const auto& [handle, name] : images)
        {
            if (name == ManagedDumpFormat::kImageName)
                image = handle;
        }
        if (!image)
        {
            if (!m_bridge.Fn("mono_image_open_from_data") || !m_bridge.Fn("mono_assembly_load_from_full"))
                return Status::Fail(ErrorCode::MonoCallFailed, "the runtime cannot load an assembly from memory");
            const Result<Address> buffer = process.Allocate(bytes.size(), PAGE_READWRITE);
            if (!buffer)
                return buffer.Error();
            const Status written = process.Write(*buffer, bytes);
            const std::uint64_t status = call.Scratch(8);
            const std::uint64_t opened = written && status
                ? m_bridge.CallFn("mono_image_open_from_data", *buffer, bytes.size(), 1, status).value_or(0)
                : 0;
            process.Free(*buffer);
            if (!opened)
            {
                std::int32_t code = -1;
                if (status)
                    process.ReadExact(status, &code, sizeof(code));
                call.ResetScratch();
                return Status::Fail(ErrorCode::MonoCallFailed, "the runtime did not open the dumper image (status " + std::to_string(code) + ")");
            }
            const std::uint64_t path = text(std::string(ManagedDumpFormat::kImageName) + ".dll");
            const std::uint64_t assembly = m_bridge.CallFn("mono_assembly_load_from_full", opened, path, status, 0).value_or(0);
            image = assembly ? m_bridge.CallFn("mono_assembly_get_image", assembly).value_or(0) : 0;
            if (!image)
            {
                call.ResetScratch();
                return Status::Fail(ErrorCode::MonoCallFailed, "the runtime did not load the dumper assembly");
            }
            LOG_INFO("Mono", "Loaded the managed dumper (" + std::to_string(bytes.size()) + " bytes) into the game");
        }
        DumperHandles found;
        found.Class = m_bridge.CallFn("mono_class_from_name", image, text(ManagedDumpFormat::kNamespace), text(ManagedDumpFormat::kClass)).value_or(0);
        if (found.Class)
        {
            found.VTable = m_bridge.CallFn("mono_class_vtable", m_bridge.m_domain, found.Class).value_or(0);
            found.Begin = m_bridge.CallFn("mono_class_get_method_from_name", found.Class, text("Begin"), 0).value_or(0);
            found.Release = m_bridge.CallFn("mono_class_get_method_from_name", found.Class, text("Release"), 0).value_or(0);
            const auto field = [&](std::string_view name) { return m_bridge.CallFn("mono_class_get_field_from_name", found.Class, text(name)).value_or(0); };
            found.State = field("State");
            found.Types = field("Types");
            found.Cancel = field("Cancel");
            found.Data = field("Data");
            found.Size = field("Size");
        }
        call.ResetScratch();
        if (!found.Class || !found.VTable || !found.Begin || !found.Release || !found.State || !found.Types || !found.Cancel || !found.Data || !found.Size)
            return Status::Fail(ErrorCode::MonoCallFailed, "the dumper in the game does not match this build");
        m_dumper = found;
        return Status::Success();
    }

    Result<ManagedDump> MonoRuntime::RunDumper(std::int64_t total, WorkProgress* progress)
    {
        {
            std::lock_guard lock(m_bridge.m_lock);
            if (!m_bridge.m_process)
                return Status::Fail(ErrorCode::NoProcess);
            m_bridge.m_call.ResetScratch();
            const auto begun = InvokeStatic(m_dumper.Begin);
            m_bridge.m_call.ResetScratch();
            if (!begun)
                return Status::Fail(ErrorCode::MonoCallFailed, "Dumper.Begin did not run");
            m_dumperPending = true;
        }
        if (progress)
        {
            progress->SetText("Reading the classes");
            progress->SetSteps(0, total);
        }
        const auto deadline = std::chrono::steady_clock::now() + kDumperBudget;
        std::int64_t state = ManagedDumpFormat::Running;
        while (state == ManagedDumpFormat::Running)
        {
            std::this_thread::sleep_for(kDumperPoll);
            if (progress && progress->Cancelled())
            {
                StopDumper();
                return Status::Fail(ErrorCode::MonoNotFound, "cancelled");
            }
            if (std::chrono::steady_clock::now() > deadline)
            {
                StopDumper();
                return Status::Fail(ErrorCode::MonoCallFailed, "it did not finish in " + std::to_string(kDumperBudget.count()) + " s");
            }
            std::lock_guard lock(m_bridge.m_lock);
            if (!m_bridge.m_process)
                return Status::Fail(ErrorCode::NoProcess);
            m_bridge.m_call.ResetScratch();
            const auto read = ReadStatic(m_dumper.State, 4);
            const auto types = ReadStatic(m_dumper.Types, 4);
            m_bridge.m_call.ResetScratch();
            if (!read)
            {
                StopDumper();
                return Status::Fail(ErrorCode::MonoCallFailed, "its state could not be read");
            }
            state = *read;
            if (progress && types)
                progress->SetSteps(*types, std::max(total, *types));
        }
        if (state != ManagedDumpFormat::Done)
        {
            StopDumper();
            return Status::Fail(ErrorCode::MonoCallFailed, "it failed inside the game");
        }
        std::vector<std::uint8_t> blob;
        {
            std::lock_guard lock(m_bridge.m_lock);
            if (!m_bridge.m_process)
                return Status::Fail(ErrorCode::NoProcess);
            m_bridge.m_call.ResetScratch();
            const auto data = ReadStatic(m_dumper.Data, 8);
            const auto size = ReadStatic(m_dumper.Size, 4);
            m_bridge.m_call.ResetScratch();
            const bool usable = data && size && *data && *size > 0 && *size <= kMaxDumpBytes;
            if (usable)
            {
                blob.resize(static_cast<std::size_t>(*size));
                if (!m_bridge.m_process->ReadExact(static_cast<std::uint64_t>(*data), blob.data(), blob.size()))
                    blob.clear();
            }
            StopDumper();
            if (!usable)
                return Status::Fail(ErrorCode::MonoCallFailed, "it published no data");
            if (blob.empty())
                return Status::Fail(ErrorCode::ReadFailed, "its data could not be read");
        }
        if (progress)
            progress->SetText("Reading the dump");
        return ParseManagedDump(blob);
    }

    bool MonoRuntime::CheckDump(const ImageList& images, ManagedDump& dump)
    {
        if (!dump.MonoHandles || dump.Classes.empty())
        {
            LOG_WARN("Mono", "The managed dump carries no runtime handles, reading the classes through the runtime");
            return false;
        }
        std::map<std::string, std::uint64_t> imageByName;
        for (const auto& [handle, name] : images)
            imageByName.emplace(name, handle);
        const std::size_t count = dump.Classes.size();
        const std::size_t stride = std::max<std::size_t>(1, count / kClassSamples);

        std::lock_guard lock(m_bridge.m_lock);
        RemoteCall& call = m_bridge.m_call;
        call.ResetScratch();
        std::size_t classesChecked = 0;
        for (std::size_t i = 0; i < count && classesChecked < kClassSamples; i += stride)
        {
            const MonoClass& info = dump.Classes[i];
            const auto image = imageByName.find(info.Image);
            const std::uint32_t token = dump.Tokens[i];
            if (image == imageByName.end() || (token & 0xFF000000u) != kTokenTypeDef || !info.Handle)
                continue;
            const std::uint64_t klass = m_bridge.CallFn("mono_class_get", image->second, token).value_or(0);
            if (klass != info.Handle)
            {
                LOG_WARN("Mono", "The managed dump names " + info.FullName() + " with a class the runtime does not confirm, reading the classes through the runtime");
                return false;
            }
            ++classesChecked;
        }
        if (!classesChecked)
        {
            LOG_WARN("Mono", "No class of the managed dump could be checked against the runtime, reading the classes through the runtime");
            return false;
        }

        bool membersOk = dump.OffsetsSure;
        if (!membersOk)
            LOG_WARN("Mono", "The managed dumper could not make the runtime lay out every class, the members are read through the runtime when needed");
        const std::size_t memberStride = std::max<std::size_t>(1, count / kMemberSamples);
        std::size_t fieldsChecked = 0;
        std::size_t methodsChecked = 0;
        for (std::size_t i = 0; i < count && membersOk && (fieldsChecked < kMemberSamples || methodsChecked < kMemberSamples); ++i)
        {
            const MonoClass& info = dump.Classes[(i * memberStride) % count];
            if (!info.Handle || info.Generic)
                continue;
            if (!info.Fields.empty() && fieldsChecked < kMemberSamples)
            {
                const MonoField& field = info.Fields.front();
                const std::uint64_t name = call.Scratch(field.Name.size() + 1);
                if (name && !field.Name.empty())
                    m_bridge.m_process->Write(name, field.Name.data(), field.Name.size());
                const std::uint64_t handle = name ? m_bridge.CallFn("mono_class_get_field_from_name", info.Handle, name).value_or(0) : 0;
                const auto offset = handle ? m_bridge.CallFn("mono_field_get_offset", handle) : std::nullopt;
                call.ResetScratch();
                membersOk = offset && static_cast<std::int32_t>(*offset & 0xFFFFFFFFu) == field.Offset;
                ++fieldsChecked;
            }
            if (membersOk && !info.Methods.empty() && methodsChecked < kMemberSamples)
            {
                const MonoMethod& method = info.Methods.front();
                membersOk = method.Handle && m_bridge.CallString("mono_method_get_name", method.Handle) == method.Name;
                ++methodsChecked;
            }
        }
        call.ResetScratch();
        if (!membersOk)
        {
            if (dump.OffsetsSure)
                LOG_WARN("Mono", "The managed dump's members do not match the runtime, they are read through the runtime when needed");
            for (MonoClass& info : dump.Classes)
            {
                info.MembersLoaded = false;
                info.Fields.clear();
                info.Methods.clear();
            }
        }
        return true;
    }

    void MonoRuntime::NameNestedTypes(ManagedDump& dump, WorkProgress* progress)
    {
        std::size_t asked = 0;
        std::size_t named = 0;
        for (std::size_t i = 0; i < dump.Classes.size(); ++i)
        {
            MonoClass& info = dump.Classes[i];
            if (!(dump.Flags[i] & ManagedDumpFormat::kTypeNested) || !info.Handle)
                continue;
            if ((asked & 255) == 0 && progress && progress->Cancelled())
                return;
            std::lock_guard lock(m_bridge.m_lock);
            std::string space = m_bridge.CallString("mono_class_get_namespace", info.Handle);
            ++asked;
            if (!space.empty())
            {
                info.Namespace = std::move(space);
                ++named;
            }
        }
        if (named)
            LOG_INFO("Mono", std::to_string(named) + " of " + std::to_string(asked) + " nested classes carry a namespace of their own");
    }

    std::optional<std::uint64_t> MonoRuntime::InvokeStatic(std::uint64_t method)
    {
        const std::uint64_t exception = m_bridge.m_call.Scratch(8);
        if (!exception || !method)
            return std::nullopt;
        const auto result = m_bridge.CallFn("mono_runtime_invoke", method, 0, 0, exception);
        if (!result)
            return std::nullopt;
        std::uint64_t thrown = 0;
        if (!m_bridge.m_process->ReadExact(exception, &thrown, sizeof(thrown)) || thrown)
            return std::nullopt;
        return *result;
    }

    std::optional<std::int64_t> MonoRuntime::ReadStatic(std::uint64_t field, std::size_t bytes)
    {
        const std::uint64_t out = m_bridge.m_call.Scratch(8);
        if (!out || !m_bridge.CallFn("mono_field_static_get_value", m_dumper.VTable, field, out))
            return std::nullopt;
        if (bytes == 4)
        {
            std::int32_t value = 0;
            if (!m_bridge.m_process->ReadExact(out, &value, sizeof(value)))
                return std::nullopt;
            return value;
        }
        std::int64_t value = 0;
        if (!m_bridge.m_process->ReadExact(out, &value, sizeof(value)))
            return std::nullopt;
        return value;
    }

    bool MonoRuntime::WriteStatic(std::uint64_t field, std::int32_t value)
    {
        const std::uint64_t in = m_bridge.m_call.Scratch(8);
        if (!in || !m_bridge.m_process->Write(in, &value, sizeof(value)))
            return false;
        return m_bridge.CallFn("mono_field_static_set_value", m_dumper.VTable, field, in).has_value();
    }

    void MonoRuntime::StopDumper()
    {
        if (!m_dumperPending)
            return;
        std::lock_guard lock(m_bridge.m_lock);
        m_dumperPending = false;
        if (!m_bridge.m_process)
            return;
        m_bridge.m_call.ResetScratch();
        WriteStatic(m_dumper.Cancel, 1);
        InvokeStatic(m_dumper.Release);
        m_bridge.m_call.ResetScratch();
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
