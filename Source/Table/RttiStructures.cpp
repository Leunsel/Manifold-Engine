#include "Table/RttiStructures.h"

#include "Core/Text.h"

#include <algorithm>
#include <map>

namespace Manifold::Table
{
    namespace
    {
        using Target::RttiClassPtr;
        using Target::RttiObject;
        using Target::RttiRuntime;

        struct Build
        {
            RttiRuntime& Runtime;
            const std::set<std::string>& Existing;
            std::vector<StructDefinition> Children;
            std::set<std::string> Building;

            bool Known(const std::string& name) const
            {
                return Existing.count(name) != 0 || std::any_of(Children.begin(), Children.end(), [&](const StructDefinition& child) { return child.Name == name; });
            }
        };

        struct Member
        {
            int Start = 0;
            int Size = 0;
            RttiClassPtr Class;
        };

        int AnnotateObject(Build& build, std::vector<StructField>& fields, const RttiObject& hit, int depth);

        bool Generic(const std::string& name)
        {
            if (name.empty())
                return true;
            const std::string lowered = Lower(name);
            if (lowered == "pointer")
                return true;
            if (lowered.size() <= 8 || lowered.compare(0, 8, "pointer ") != 0)
                return false;
            return std::all_of(lowered.begin() + 8, lowered.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        }

        std::uint64_t FieldAddress(const RttiObject& hit, const StructField& field)
        {
            return hit.Start + static_cast<std::uint64_t>(static_cast<std::int64_t>(field.Offset));
        }

        std::optional<std::string> PointerLabel(Build& build, const RttiObject& hit, std::optional<std::uint64_t> value)
        {
            if (!value)
                return std::nullopt;
            RttiRuntime& runtime = build.Runtime;
            if (const RttiClassPtr type = runtime.TableClass(*value, true))
            {
                if (type->TypeDescriptor != hit.Class->TypeDescriptor)
                    return type->Name + " vftable";
                if (type->Offset == 0)
                    return std::string("vftable");
                if (const auto base = runtime.BaseAt(*type, type->Offset, hit.Start))
                    return "vftable (" + *base + ")";
                return std::string("vftable");
            }
            if (runtime.HasVirtualBases(*hit.Class) && runtime.VirtualBaseTable(*value))
                return std::string("vbtable");
            const auto target = runtime.ObjectAt(*value, true);
            if (!target)
                return std::nullopt;
            if (target->Start == *value)
                return target->Class->Name + "*";
            return runtime.BaseAt(*target->Class, static_cast<std::int64_t>(*value - target->Start), target->Start).value_or(target->Class->Name) + "*";
        }

        std::optional<std::string> MemberStructure(Build& build, const RttiClassPtr& type, std::uint64_t address, int size, int depth)
        {
            const std::string& name = type->Name;
            if (build.Known(name))
                return name;
            if (build.Building.count(name))
                return std::nullopt;
            build.Building.insert(name);
            StructDefinition child;
            child.Name = name;
            GuessFields(*build.Runtime.ProcessHandle(), child, address, 0, size);
            if (child.Fields.empty())
            {
                build.Building.erase(name);
                return std::nullopt;
            }
            RttiObject object;
            object.Address = address;
            object.Start = address;
            object.Class = type;
            AnnotateObject(build, child.Fields, object, depth + 1);
            build.Children.push_back(std::move(child));
            build.Building.erase(name);
            return name;
        }

        int EmbedMembers(Build& build, std::vector<StructField>& fields, const RttiObject& hit, int depth)
        {
            if (depth >= kRttiMemberDepth || hit.Via || fields.empty())
                return 0;
            RttiRuntime& runtime = build.Runtime;
            const int finish = fields.back().Offset + std::max(fields.back().Size, 1);
            std::map<int, std::size_t> byOffset;
            for (std::size_t i = 0; i < fields.size(); ++i)
                byOffset[fields[i].Offset] = i;
            std::vector<int> own;
            std::vector<Member> members;
            std::set<int> seen;
            for (const StructField& field : fields)
            {
                if (field.Type != Scan::ValueType::Pointer)
                    continue;
                const auto value = runtime.ReadPointer(FieldAddress(hit, field));
                const RttiClassPtr type = value ? runtime.TableClass(*value, true) : nullptr;
                if (!type)
                    continue;
                if (type->TypeDescriptor == hit.Class->TypeDescriptor)
                {
                    own.push_back(field.Offset);
                }
                else if (static_cast<std::int64_t>(type->Offset) <= field.Offset)
                {
                    const int start = field.Offset - static_cast<int>(type->Offset);
                    if (start > 0 && byOffset.count(start) && seen.insert(start).second)
                        members.push_back({ start, 0, type });
                }
            }
            if (members.empty())
                return 0;
            std::sort(members.begin(), members.end(), [](const Member& a, const Member& b) { return a.Start < b.Start; });
            for (std::size_t i = 0; i < members.size(); ++i)
            {
                Member& member = members[i];
                int stop = i + 1 < members.size() ? members[i + 1].Start : std::min(finish, member.Start + kRttiMemberLimit);
                for (const int offset : own)
                {
                    if (offset > member.Start && offset < stop)
                        stop = offset;
                }
                member.Size = stop - member.Start;
            }
            int embedded = 0;
            for (std::size_t i = members.size(); i-- > 0;)
            {
                const Member& member = members[i];
                if (member.Size <= 0)
                    continue;
                const auto child = MemberStructure(build, member.Class, hit.Start + static_cast<std::uint64_t>(member.Start), member.Size, depth);
                if (!child)
                    continue;
                const std::size_t first = byOffset[member.Start];
                const int end = member.Start + member.Size;
                std::size_t last = first + 1;
                while (last < fields.size() && fields[last].Offset < end)
                    ++last;
                fields.erase(fields.begin() + static_cast<std::ptrdiff_t>(first) + 1, fields.begin() + static_cast<std::ptrdiff_t>(last));
                StructField& field = fields[first];
                field.Type = Scan::ValueType::ByteArray;
                field.Size = member.Size;
                field.ChildStructure = *child;
                if (Generic(field.Name) || field.Name == "vftable")
                    field.Name = member.Class->Name;
                ++embedded;
            }
            return embedded;
        }

        int AnnotateObject(Build& build, std::vector<StructField>& fields, const RttiObject& hit, int depth)
        {
            std::stable_sort(fields.begin(), fields.end(), [](const StructField& a, const StructField& b) { return a.Offset < b.Offset; });
            EmbedMembers(build, fields, hit, depth);
            int labelled = 0;
            for (StructField& field : fields)
            {
                if (field.Type != Scan::ValueType::Pointer || !Generic(field.Name))
                    continue;
                if (const auto label = PointerLabel(build, hit, build.Runtime.ReadPointer(FieldAddress(hit, field))))
                {
                    field.Name = *label;
                    ++labelled;
                }
            }
            return labelled;
        }

        int AnnotateTable(Build& build, std::vector<StructField>& fields, std::uint64_t table)
        {
            RttiRuntime& runtime = build.Runtime;
            const int pointer = runtime.PointerSize();
            int labelled = 0;
            for (StructField& field : fields)
            {
                const int offset = field.Offset;
                if (offset < 0 || offset % pointer != 0 || offset / pointer >= RttiRuntime::kMaxVirtuals)
                    break;
                const auto value = runtime.ReadPointer(table + static_cast<std::uint64_t>(offset));
                if (!value || !runtime.InImage(*value) || (offset > 0 && runtime.Locator(*value)))
                    break;
                if (field.Type == Scan::ValueType::Pointer && Generic(field.Name))
                {
                    field.Name = "virtual " + std::to_string(offset / pointer);
                    ++labelled;
                }
            }
            return labelled;
        }
    }

    std::set<std::string> StructureNames(const StructureStore& store)
    {
        std::set<std::string> names;
        for (const auto& item : store.Items)
            names.insert(item->Name);
        return names;
    }

    std::optional<RttiName> RttiStructureName(Target::RttiRuntime& runtime, std::uint64_t address)
    {
        if (const auto hit = runtime.ObjectAt(address))
            return RttiName{ hit->Class->Name, hit->Start, true };
        if (const RttiClassPtr type = runtime.TableClass(address))
            return RttiName{ runtime.SymbolFor(*type), address, false };
        return std::nullopt;
    }

    RttiFill FillRttiObject(Target::RttiRuntime& runtime, const Target::RttiObject& object, int size, const std::set<std::string>& existing)
    {
        RttiFill fill;
        StructDefinition scratch;
        GuessFields(*runtime.ProcessHandle(), scratch, object.Start, 0, std::max(size, kRttiGuessSize));
        if (scratch.Fields.empty())
            return fill;
        Build build{ runtime, existing, {}, {} };
        fill.Object = true;
        fill.Labelled = AnnotateObject(build, scratch.Fields, object, 0);
        fill.Fields = std::move(scratch.Fields);
        fill.Children = std::move(build.Children);
        fill.Handled = true;
        return fill;
    }

    RttiFill FillFromRtti(Target::RttiRuntime& runtime, std::uint64_t address, int size, const std::set<std::string>& existing)
    {
        RttiFill fill;
        const auto hit = runtime.ObjectAt(address);
        if (hit)
            return hit->Start == address ? FillRttiObject(runtime, *hit, size, existing) : fill;
        if (!runtime.TableClass(address))
            return fill;
        StructDefinition scratch;
        GuessFields(*runtime.ProcessHandle(), scratch, address, 0, std::max(size, kRttiGuessSize));
        if (scratch.Fields.empty())
            return fill;
        Build build{ runtime, existing, {}, {} };
        fill.Labelled = AnnotateTable(build, scratch.Fields, address);
        fill.Fields = std::move(scratch.Fields);
        fill.Children = std::move(build.Children);
        fill.Handled = true;
        return fill;
    }

    RttiFill LabelFromRtti(Target::RttiRuntime& runtime, std::vector<StructField> fields, std::uint64_t address, const std::set<std::string>& existing)
    {
        RttiFill fill;
        Build build{ runtime, existing, {}, {} };
        const auto hit = runtime.ObjectAt(address);
        if (hit && hit->Start == address)
        {
            fill.Object = true;
            fill.Labelled = AnnotateObject(build, fields, *hit, 0);
        }
        else if (runtime.TableClass(address))
        {
            fill.Labelled = AnnotateTable(build, fields, address);
        }
        else
        {
            return fill;
        }
        fill.Fields = std::move(fields);
        fill.Children = std::move(build.Children);
        fill.Handled = true;
        return fill;
    }

    void ApplyRttiFill(StructureStore& store, StructDefinition& definition, RttiFill fill)
    {
        definition.Fields = std::move(fill.Fields);
        for (StructDefinition& child : fill.Children)
        {
            if (store.Find(child.Name))
                continue;
            StructDefinition& added = store.Add(child.Name);
            added.Fields = std::move(child.Fields);
        }
        ++store.Generation;
    }
}
