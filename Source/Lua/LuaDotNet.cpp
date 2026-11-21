#include "Lua/LuaDotNet.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Lua/LuaArgs.h"
#include "Target/Mono.h"
#include "Target/Process.h"

#include "lua.hpp"

#include <algorithm>
#include <cstring>

namespace Manifold::Lua
{
    namespace
    {
        using Target::MonoBridge;
        using Target::MonoField;

        constexpr int kStructCode = 0x11;
        constexpr std::size_t kMaxText = 1u << 20;

        bool IsReference(int code)
        {
            return code == 0x12 || code == 0x0E || code == 0x1C || code == 0x1D || code == 0x14 || code == 0x15;
        }

        // The bridge when a .NET runtime is loaded in it, else the reason.
        MonoBridge* Bridge(lua_State* state, Status& error)
        {
            Engine& engine = Owner(state);
            if (!engine.HasProcess())
            {
                error = Status::Fail(ErrorCode::NoProcess);
                return nullptr;
            }
            MonoBridge& mono = engine.Mono();
            if (!mono.Present() || !mono.IsClr())
            {
                error = Status::Fail(ErrorCode::ClrNotLoaded, mono.Present() ? std::string(": the loaded runtime is ") + MonoBridge::RuntimeName(mono.Runtime()) : std::string());
                return nullptr;
            }
            return &mono;
        }

        void SetInteger(lua_State* state, const char* key, std::uint64_t value)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(value));
            lua_setfield(state, -2, key);
        }

        void SetString(lua_State* state, const char* key, std::string_view value)
        {
            lua_pushlstring(state, value.data(), value.size());
            lua_setfield(state, -2, key);
        }

        void SetBoolean(lua_State* state, const char* key, bool value)
        {
            lua_pushboolean(state, value ? 1 : 0);
            lua_setfield(state, -2, key);
        }

        void PushField(lua_State* state, const MonoField& field, bool isStatic)
        {
            lua_createtable(state, 0, 6);
            SetString(state, "name", field.Name);
            SetString(state, "type", field.TypeName);
            SetInteger(state, "typeCode", static_cast<std::uint64_t>(field.EnumCode ? field.EnumCode : field.TypeCode));
            if (isStatic)
            {
                if (field.Address)
                    SetInteger(state, "address", field.Address);
            }
            else
                SetInteger(state, "offset", static_cast<std::uint64_t>(field.Offset));
            if (field.ValueSize > 0)
                SetInteger(state, "valueSize", static_cast<std::uint64_t>(field.ValueSize));
            if (field.TypeHandle)
                SetInteger(state, "typeHandle", field.TypeHandle);
        }

        void PushFields(lua_State* state, const std::vector<MonoField>& fields, bool isStatic)
        {
            lua_createtable(state, static_cast<int>(fields.size()), 0);
            for (std::size_t i = 0; i < fields.size(); ++i)
            {
                PushField(state, fields[i], isStatic);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
        }

        // "Ns.Class:Name" -> ("Ns.Class", "Name").
        bool SplitMember(std::string_view text, std::string& owner, std::string& member)
        {
            const std::size_t colon = text.rfind(':');
            if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size())
                return false;
            owner = std::string(text.substr(0, colon));
            member = std::string(text.substr(colon + 1));
            return true;
        }

        const MonoField* FindField(const std::vector<MonoField>& fields, const std::string& name)
        {
            // An auto-property's value sits in a compiler-made backing field.
            const std::string backing = "<" + name + ">k__BackingField";
            for (const std::string* wanted : { &name, &backing })
            {
                const auto found = std::find_if(fields.begin(), fields.end(), [&](const MonoField& field) { return field.Name == *wanted; });
                if (found != fields.end())
                    return &*found;
            }
            return nullptr;
        }

        int Load(lua_State* state)
        {
            Engine& engine = Owner(state);
            if (!engine.HasProcess())
                return PushFailure(state, ErrorCode::NoProcess);
            MonoBridge& mono = engine.Mono();
            if (mono.Present())
            {
                if (!mono.IsClr())
                    return PushFailure(state, ErrorCode::ClrNotLoaded, std::string(": the loaded runtime is ") + MonoBridge::RuntimeName(mono.Runtime()));
                lua_pushboolean(state, 1);
                return 1;
            }
            if (MonoBridge::DetectRuntime(*engine.Process()) != Target::ManagedRuntime::Clr)
                return PushFailure(state, ErrorCode::MonoNotFound, ": the process has no .NET runtime (coreclr.dll or clr.dll)");
            const Status loaded = engine.Managed().Ensure();
            if (!loaded)
                return PushFailure(state, loaded);
            lua_pushboolean(state, 1);
            return 1;
        }

        int Unload(lua_State* state)
        {
            Engine& engine = Owner(state);
            if (engine.Mono().Present() && engine.Mono().IsClr())
                engine.Managed().Stop(true);
            lua_pushboolean(state, 1);
            return 1;
        }

        int IsLoaded(lua_State* state)
        {
            Engine& engine = Owner(state);
            lua_pushboolean(state, engine.HasProcess() && engine.Mono().Present() && engine.Mono().IsClr() ? 1 : 0);
            return 1;
        }

        int StatusOf(lua_State* state)
        {
            Engine& engine = Owner(state);
            MonoBridge& mono = engine.Mono();
            const bool loaded = engine.HasProcess() && mono.Present() && mono.IsClr();
            lua_createtable(state, 0, 7);
            SetBoolean(state, "loaded", loaded);
            SetBoolean(state, "detected", engine.HasProcess() && MonoBridge::DetectRuntime(*engine.Process()) == Target::ManagedRuntime::Clr);
            if (loaded)
            {
                SetString(state, "runtime", mono.RuntimeDescription());
                SetString(state, "module", mono.Module());
                SetInteger(state, "classes", mono.ClassCount());
                SetInteger(state, "pointerSize", static_cast<std::uint64_t>(engine.Process()->PointerSize()));
            }
            return 1;
        }

        int Modules(lua_State* state)
        {
            Status error;
            MonoBridge* mono = Bridge(state, error);
            if (!mono)
                return PushFailure(state, error);
            const std::vector<Target::ManagedModule> modules = mono->Modules();
            lua_createtable(state, static_cast<int>(modules.size()), 0);
            for (std::size_t i = 0; i < modules.size(); ++i)
            {
                const Target::ManagedModule& module = modules[i];
                lua_createtable(state, 0, 6);
                SetInteger(state, "handle", module.Address);
                SetString(state, "name", module.Name);
                SetString(state, "path", module.Path);
                SetInteger(state, "base", module.Base);
                SetInteger(state, "size", module.Size);
                SetBoolean(state, "dynamic", module.Dynamic);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int Classes(lua_State* state)
        {
            Status error;
            MonoBridge* mono = Bridge(state, error);
            if (!mono)
                return PushFailure(state, error);
            const std::string filter = luaL_optstring(state, 1, "");
            const std::vector<Target::MonoClass> classes = mono->Classes();
            lua_createtable(state, 0, 0);
            lua_Integer row = 0;
            for (const Target::MonoClass& info : classes)
            {
                const std::string full = info.FullName();
                if (!filter.empty() && !ContainsNoCase(full, filter))
                    continue;
                lua_createtable(state, 0, 7);
                SetString(state, "name", full);
                SetString(state, "namespace", info.Namespace);
                SetString(state, "image", info.Image);
                SetInteger(state, "handle", info.Handle);
                SetInteger(state, "module", info.Module);
                SetInteger(state, "token", info.Token);
                SetBoolean(state, "generic", info.Generic);
                lua_rawseti(state, -2, ++row);
            }
            return 1;
        }

        int ClassOf(lua_State* state)
        {
            Status error;
            MonoBridge* mono = Bridge(state, error);
            if (!mono)
                return PushFailure(state, error);
            const std::string name = luaL_checkstring(state, 1);
            const auto info = mono->ClassDetails(name);
            if (!info)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_createtable(state, 0, 12);
            SetString(state, "name", info->FullName());
            SetString(state, "namespace", info->Namespace);
            SetString(state, "image", info->Image);
            SetString(state, "parent", info->Parent);
            SetInteger(state, "handle", info->Handle);
            SetInteger(state, "module", info->Module);
            SetInteger(state, "token", info->Token);
            SetBoolean(state, "generic", info->Generic);
            PushFields(state, info->Fields, false);
            lua_setfield(state, -2, "fields");
            PushFields(state, info->Statics, true);
            lua_setfield(state, -2, "statics");
            lua_createtable(state, static_cast<int>(info->Methods.size()), 0);
            for (std::size_t i = 0; i < info->Methods.size(); ++i)
            {
                const Target::MonoMethod& method = info->Methods[i];
                lua_createtable(state, 0, 6);
                SetString(state, "name", method.Name);
                if (method.Address)
                    SetInteger(state, "address", method.Address);
                SetInteger(state, "handle", method.Handle);
                SetInteger(state, "token", method.Token);
                SetInteger(state, "params", static_cast<std::uint64_t>(std::max(method.ParamCount, 0)));
                SetBoolean(state, "static", method.Static);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            lua_setfield(state, -2, "methods");
            return 1;
        }

        template <class Body>
        int AddressQuery(lua_State* state, Body body)
        {
            Status error;
            MonoBridge* mono = Bridge(state, error);
            if (!mono)
                return PushFailure(state, error);
            const auto address = ResolveAddress(state, 1, error);
            if (!address)
                return PushFailure(state, error);
            return body(*mono, *address);
        }

        int ClassAt(lua_State* state)
        {
            return AddressQuery(state, [&](MonoBridge& mono, std::uint64_t address)
                {
                    const auto found = mono.ClassAt(address);
                    if (!found)
                    {
                        lua_pushnil(state);
                        return 1;
                    }
                    lua_createtable(state, 0, 3);
                    SetString(state, "class", found->Name);
                    SetInteger(state, "address", found->Base);
                    SetInteger(state, "offset", address - found->Base);
                    return 1;
                });
        }

        const char* KindName(Target::ManagedObjectKind kind)
        {
            switch (kind)
            {
                case Target::ManagedObjectKind::String: return "string";
                case Target::ManagedObjectKind::Free: return "free";
                case Target::ManagedObjectKind::Array: return "array";
                case Target::ManagedObjectKind::Other:
                case Target::ManagedObjectKind::Object: break;
            }
            return "object";
        }

        int ObjectOf(lua_State* state)
        {
            return AddressQuery(state, [&](MonoBridge& mono, std::uint64_t address)
                {
                    const auto object = mono.ObjectAt(address);
                    if (!object)
                    {
                        lua_pushnil(state);
                        return 1;
                    }
                    lua_createtable(state, 0, 10);
                    SetInteger(state, "address", object->Address);
                    SetString(state, "class", object->Name);
                    SetInteger(state, "handle", object->Handle);
                    SetString(state, "kind", KindName(object->Kind));
                    SetInteger(state, "size", object->Size);
                    if (object->Kind == Target::ManagedObjectKind::Array || object->Kind == Target::ManagedObjectKind::String)
                        SetInteger(state, "length", object->Length);
                    if (object->Kind == Target::ManagedObjectKind::Array)
                    {
                        SetString(state, "elementClass", object->ElementName);
                        SetInteger(state, "elementType", static_cast<std::uint64_t>(object->ElementType));
                        SetInteger(state, "elementSize", object->ElementSize);
                        SetInteger(state, "data", object->Data);
                        SetInteger(state, "rank", static_cast<std::uint64_t>(object->Rank));
                    }
                    return 1;
                });
        }

        int Fields(lua_State* state)
        {
            return AddressQuery(state, [&](MonoBridge& mono, std::uint64_t address)
                {
                    std::vector<MonoField> fields = mono.InstanceFieldsAt(address);
                    if (fields.empty() && !mono.ClassNameAt(address))
                        return PushFailure(state, ErrorCode::ClrNoMember, ": no .NET object at " + Hex(address));
                    std::stable_sort(fields.begin(), fields.end(), [](const MonoField& a, const MonoField& b) { return a.Offset < b.Offset; });
                    PushFields(state, fields, false);
                    return 1;
                });
        }

        int MethodAt(lua_State* state)
        {
            return AddressQuery(state, [&](MonoBridge& mono, std::uint64_t address)
                {
                    const auto name = mono.MethodNameAt(address);
                    const auto range = name ? mono.MethodRange(address) : std::nullopt;
                    if (!name || !range)
                    {
                        lua_pushnil(state);
                        return 1;
                    }
                    lua_createtable(state, 0, 3);
                    SetString(state, "name", *name);
                    SetInteger(state, "start", range->first);
                    SetInteger(state, "size", range->second - range->first);
                    return 1;
                });
        }

        template <class Body>
        int MemberQuery(lua_State* state, Body body)
        {
            Status error;
            MonoBridge* mono = Bridge(state, error);
            if (!mono)
                return PushFailure(state, error);
            return body(*mono, std::string(luaL_checkstring(state, 1)), std::string(luaL_checkstring(state, 2)));
        }

        int MethodAddress(lua_State* state)
        {
            return MemberQuery(state, [&](MonoBridge& mono, const std::string& klass, const std::string& method)
                {
                    if (const auto address = mono.AddressOf(klass + ":" + method); address && *address)
                        lua_pushinteger(state, static_cast<lua_Integer>(*address));
                    else
                        lua_pushnil(state);
                    return 1;
                });
        }

        int StaticAddress(lua_State* state)
        {
            return MemberQuery(state, [&](MonoBridge& mono, const std::string& klass, const std::string& field)
                {
                    const auto info = mono.ClassDetails(klass);
                    const MonoField* found = info ? FindField(info->Statics, field) : nullptr;
                    if (found && found->Address)
                        lua_pushinteger(state, static_cast<lua_Integer>(found->Address));
                    else
                        lua_pushnil(state);
                    return 1;
                });
        }

        int FieldOffset(lua_State* state)
        {
            return MemberQuery(state, [&](MonoBridge& mono, const std::string& klass, const std::string& field)
                {
                    const MonoField* found = nullptr;
                    const std::vector<MonoField> fields = mono.InstanceFieldsOf(klass);
                    found = FindField(fields, field);
                    if (found)
                        lua_pushinteger(state, found->Offset);
                    else
                        lua_pushnil(state);
                    return 1;
                });
        }

        int Heap(lua_State* state)
        {
            Status error;
            MonoBridge* mono = Bridge(state, error);
            if (!mono)
                return PushFailure(state, error);
            const Result<Target::ManagedHeap> heap = mono->ReadHeap();
            if (!heap)
                return PushFailure(state, heap.Error());
            lua_createtable(state, 0, 6);
            SetInteger(state, "objects", heap->Objects);
            SetInteger(state, "bytes", heap->Bytes);
            SetInteger(state, "segments", heap->Segments);
            lua_pushnumber(state, heap->Seconds);
            lua_setfield(state, -2, "seconds");
            lua_createtable(state, static_cast<int>(heap->Types.size()), 0);
            for (std::size_t i = 0; i < heap->Types.size(); ++i)
            {
                const Target::ManagedHeapType& type = heap->Types[i];
                lua_createtable(state, 0, 4);
                SetString(state, "name", type.Name);
                SetInteger(state, "handle", type.Handle);
                SetInteger(state, "count", type.Count);
                SetInteger(state, "bytes", type.Bytes);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            lua_setfield(state, -2, "types");
            return 1;
        }

        int Instances(lua_State* state)
        {
            Status error;
            MonoBridge* mono = Bridge(state, error);
            if (!mono)
                return PushFailure(state, error);
            std::uint64_t handle = 0;
            if (lua_type(state, 1) == LUA_TNUMBER)
                handle = static_cast<std::uint64_t>(lua_tointeger(state, 1));
            else
            {
                const std::string name = luaL_checkstring(state, 1);
                const auto info = mono->ClassDetails(name);
                if (!info)
                    return PushFailure(state, ErrorCode::ClrNoMember, ": no class " + name);
                handle = info->Handle;
            }
            const lua_Integer limit = std::clamp<lua_Integer>(luaL_optinteger(state, 2, 100000), 1, 1000000);
            const Result<Target::ManagedInstances> found = mono->Instances(handle, static_cast<std::uint32_t>(limit));
            if (!found)
                return PushFailure(state, found.Error());
            lua_createtable(state, static_cast<int>(found->Addresses.size()), 0);
            for (std::size_t i = 0; i < found->Addresses.size(); ++i)
            {
                lua_pushinteger(state, static_cast<lua_Integer>(found->Addresses[i]));
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        std::optional<std::string> StringText(const Target::Process& process, std::uint64_t object)
        {
            const std::uint64_t pointer = static_cast<std::uint64_t>(process.PointerSize());
            std::int32_t length = 0;
            if (!process.ReadExact(object + pointer, &length, sizeof(length)) || length < 0)
                return std::nullopt;
            const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(length), kMaxText);
            std::wstring text(count, L'\0');
            if (count && !process.ReadExact(object + pointer + 4, text.data(), count * sizeof(wchar_t)))
                return std::nullopt;
            return WideToUtf8(text);
        }

        int Text(lua_State* state)
        {
            return AddressQuery(state, [&](MonoBridge& mono, std::uint64_t address)
                {
                    if (mono.ClassNameAt(address).value_or("") != "System.String")
                    {
                        lua_pushnil(state);
                        return 1;
                    }
                    const auto text = StringText(*Owner(state).Process(), address);
                    if (!text)
                        return PushFailure(state, ErrorCode::ReadFailed, Hex(address));
                    lua_pushlstring(state, text->data(), text->size());
                    return 1;
                });
        }

        // Where a path ends: the storage of a value, and what kind of value.
        struct Location
        {
            std::uint64_t Address = 0;   // the storage (0 at the start for an object base)
            std::uint64_t Object = 0;    // for an object base without a step yet
            int TypeCode = 0x12;
            std::uint64_t TypeHandle = 0;
            std::string Name;            // for messages
        };

        // Follows a path from a base (an object address, or "Class:StaticField")
        // over fields by name and array elements by index.
        Result<Location> Walk(lua_State* state, MonoBridge& mono)
        {
            const Target::Process& process = *Owner(state).Process();
            const std::uint64_t pointer = static_cast<std::uint64_t>(process.PointerSize());
            Location at;
            std::string owner, member;
            if (lua_type(state, 1) == LUA_TSTRING && SplitMember(lua_tostring(state, 1), owner, member))
            {
                const auto info = mono.ClassDetails(owner);
                if (!info)
                    return Status::Fail(ErrorCode::ClrNoMember, ": no class " + owner);
                const MonoField* field = FindField(info->Statics, member);
                if (!field)
                    return Status::Fail(ErrorCode::ClrNoMember, ": no static field " + member + " in " + info->FullName());
                if (!field->Address)
                    return Status::Fail(ErrorCode::ClrNoMember, ": " + info->FullName() + ":" + member + " has no storage yet");
                at.Address = field->Address;
                at.TypeCode = field->EnumCode ? field->EnumCode : field->TypeCode;
                at.Name = info->Name + "." + member;
                if (at.TypeCode == kStructCode)
                {
                    // A static struct is boxed: the slot points to the box.
                    const std::uint64_t box = process.ReadPointer(field->Address).value_or(0);
                    if (!box)
                        return Status::Fail(ErrorCode::ClrNoMember, ": " + at.Name + " is not set yet");
                    at.Address = box + pointer;
                    at.TypeHandle = field->TypeHandle;
                }
            }
            else
            {
                Status error;
                const auto object = ResolveAddress(state, 1, error);
                if (!object)
                    return error;
                at.Object = *object;
                at.Name = Hex(*object);
            }
            const std::string path = luaL_optstring(state, 2, "");
            std::size_t i = 0;
            while (i < path.size())
            {
                if (path[i] == '.')
                {
                    ++i;
                    continue;
                }
                std::string step;
                bool index = false;
                if (path[i] == '[')
                {
                    const std::size_t close = path.find(']', i);
                    if (close == std::string::npos)
                        return Status::Fail(ErrorCode::ClrNoMember, ": unclosed [ in " + path);
                    step = path.substr(i + 1, close - i - 1);
                    index = true;
                    i = close + 1;
                }
                else
                {
                    const std::size_t end = path.find_first_of(".[", i);
                    step = path.substr(i, end == std::string::npos ? std::string::npos : end - i);
                    i = end == std::string::npos ? path.size() : end;
                }
                if (step.empty())
                    return Status::Fail(ErrorCode::ClrNoMember, ": an empty step in " + path);
                const std::string here = at.Name;
                // A struct stored inline: its fields are inside it, no reference to follow.
                if (at.Address && at.TypeCode == kStructCode && at.TypeHandle && !index)
                {
                    const std::vector<MonoField> inner = mono.TypedFieldsOf(at.TypeHandle);
                    const MonoField* field = FindField(inner, step);
                    if (!field)
                        return Status::Fail(ErrorCode::ClrNoMember, " " + step + " in the struct " + here);
                    at.Address = at.Address + static_cast<std::uint64_t>(field->Offset) - 2 * pointer;
                    at.TypeCode = field->EnumCode ? field->EnumCode : field->TypeCode;
                    at.TypeHandle = field->TypeCode == kStructCode && field->EnumCode == 0 ? field->TypeHandle : 0;
                    at.Name = here + "." + step;
                    continue;
                }
                std::uint64_t object = at.Object;
                if (at.Address)
                {
                    if (!IsReference(at.TypeCode))
                        return Status::Fail(ErrorCode::ClrNoMember, " " + step + ": " + here + " is no object");
                    object = process.ReadPointer(at.Address).value_or(0);
                }
                if (!object)
                    return Status::Fail(ErrorCode::ClrNoMember, " " + step + ": " + here + " is null");
                at.Object = 0;
                if (index)
                {
                    const auto array = mono.ObjectAt(object);
                    if (!array || array->Kind != Target::ManagedObjectKind::Array)
                        return Status::Fail(ErrorCode::ClrNoMember, " [" + step + "]: " + here + " is no array");
                    char* stop = nullptr;
                    const unsigned long long element = std::strtoull(step.c_str(), &stop, 0);
                    if (!stop || *stop)
                        return Status::Fail(ErrorCode::ClrNoMember, " [" + step + "]: not a number");
                    if (element >= array->Length)
                        return Status::Fail(ErrorCode::ClrNoMember, Format(" [%llu]: %s has %llu elements", element, here.c_str(), static_cast<unsigned long long>(array->Length)));
                    at.Address = array->Data + element * array->ElementSize;
                    at.TypeCode = array->ElementType;
                    at.TypeHandle = array->ElementType == kStructCode ? array->ElementHandle : 0;
                    at.Name = here + "[" + step + "]";
                    continue;
                }
                const std::vector<MonoField> fields = mono.InstanceFieldsAt(object);
                const MonoField* field = FindField(fields, step);
                if (!field)
                    return Status::Fail(ErrorCode::ClrNoMember, " " + step + " in " + mono.ClassNameAt(object).value_or(here));
                at.Address = object + static_cast<std::uint64_t>(field->Offset);
                at.TypeCode = field->EnumCode ? field->EnumCode : field->TypeCode;
                at.TypeHandle = field->TypeCode == kStructCode && field->EnumCode == 0 ? field->TypeHandle : 0;
                at.Name = here + "." + step;
            }
            return at;
        }

        int AddressOf(lua_State* state)
        {
            Status error;
            MonoBridge* mono = Bridge(state, error);
            if (!mono)
                return PushFailure(state, error);
            const Result<Location> at = Walk(state, *mono);
            if (!at)
                return PushFailure(state, at.Error());
            lua_pushinteger(state, static_cast<lua_Integer>(at->Address ? at->Address : at->Object));
            return 1;
        }

        int Value(lua_State* state)
        {
            Status error;
            MonoBridge* mono = Bridge(state, error);
            if (!mono)
                return PushFailure(state, error);
            const Result<Location> at = Walk(state, *mono);
            if (!at)
                return PushFailure(state, at.Error());
            const Target::Process& process = *Owner(state).Process();
            std::uint64_t object = at->Object;
            if (at->Address && IsReference(at->TypeCode))
                object = process.ReadPointer(at->Address).value_or(0);
            if (!at->Address || IsReference(at->TypeCode))
            {
                // A reference: a string's text, or the object's address;
                // nil for null.
                if (!object)
                {
                    lua_pushnil(state);
                    return 1;
                }
                if (mono->ClassNameAt(object).value_or("") == "System.String")
                {
                    if (const auto text = StringText(process, object))
                    {
                        lua_pushlstring(state, text->data(), text->size());
                        return 1;
                    }
                }
                lua_pushinteger(state, static_cast<lua_Integer>(object));
                return 1;
            }
            if (at->TypeCode == kStructCode)
            {
                lua_pushinteger(state, static_cast<lua_Integer>(at->Address));
                return 1;
            }
            std::uint8_t bytes[8] = {};
            int size = 0;
            switch (at->TypeCode)
            {
                case 0x02: case 0x04: case 0x05: size = 1; break;
                case 0x03: case 0x06: case 0x07: size = 2; break;
                case 0x08: case 0x09: case 0x0C: size = 4; break;
                case 0x0A: case 0x0B: case 0x0D: size = 8; break;
                case 0x18: case 0x19: case 0x0F: case 0x1B: size = process.PointerSize(); break;
                default: size = 0; break;
            }
            if (!size)
                return PushFailure(state, ErrorCode::ClrNoMember, Format(": %s has an element type 0x%X that is not read", at->Name.c_str(), at->TypeCode));
            if (!process.ReadExact(at->Address, bytes, static_cast<std::size_t>(size)))
                return PushFailure(state, ErrorCode::ReadFailed, Hex(at->Address));
            std::uint64_t raw = 0;
            std::memcpy(&raw, bytes, sizeof(raw));
            switch (at->TypeCode)
            {
                case 0x02: lua_pushboolean(state, bytes[0] != 0); break;
                case 0x04: lua_pushinteger(state, static_cast<std::int8_t>(bytes[0])); break;
                case 0x06: { std::int16_t v = 0; std::memcpy(&v, bytes, 2); lua_pushinteger(state, v); break; }
                case 0x08: { std::int32_t v = 0; std::memcpy(&v, bytes, 4); lua_pushinteger(state, v); break; }
                case 0x0A: { std::int64_t v = 0; std::memcpy(&v, bytes, 8); lua_pushinteger(state, static_cast<lua_Integer>(v)); break; }
                case 0x0C: { float v = 0; std::memcpy(&v, bytes, 4); lua_pushnumber(state, v); break; }
                case 0x0D: { double v = 0; std::memcpy(&v, bytes, 8); lua_pushnumber(state, v); break; }
                default:
                    if (size < 8)
                        raw &= (1ull << (size * 8)) - 1;
                    lua_pushinteger(state, static_cast<lua_Integer>(raw));
                    break;
            }
            return 1;
        }
    }

    void RegisterDotNetNatives(lua_State* state, int natives)
    {
        const int table = lua_absindex(state, natives);
        const luaL_Reg functions[] = {
            { "dotnetLoad", &Load },
            { "dotnetUnload", &Unload },
            { "dotnetIsLoaded", &IsLoaded },
            { "dotnetStatus", &StatusOf },
            { "dotnetModules", &Modules },
            { "dotnetClasses", &Classes },
            { "dotnetClass", &ClassOf },
            { "dotnetClassAt", &ClassAt },
            { "dotnetObject", &ObjectOf },
            { "dotnetFields", &Fields },
            { "dotnetMethodAt", &MethodAt },
            { "dotnetMethodAddress", &MethodAddress },
            { "dotnetStaticAddress", &StaticAddress },
            { "dotnetFieldOffset", &FieldOffset },
            { "dotnetHeap", &Heap },
            { "dotnetInstances", &Instances },
            { "dotnetText", &Text },
            { "dotnetAddress", &AddressOf },
            { "dotnetValue", &Value },
            { nullptr, nullptr },
        };
        lua_pushvalue(state, table);
        luaL_setfuncs(state, functions, 0);
        lua_pop(state, 1);
    }
}
