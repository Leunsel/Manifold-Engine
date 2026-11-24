#include "Table/StructExchange.h"

#include "Core/Deflate.h"
#include "Core/Text.h"
#include "Core/Xml.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

// ReClass.NET projects (.rcnet). The layout follows ReClass.NET's own reader and
// writer (DataExchange/ReClass/ReClassNetFile*.cs): a ZIP archive with one entry,
// Data.xml, root <reclass version="65537" type="x64|x86">, <classes> with <class
// uuid name comment address> and <node type name comment hidden ...> children.
namespace Manifold::Table
{
    namespace
    {
        // ---------------------------------------------------------------- ZIP

        std::uint32_t Crc32(std::span<const std::uint8_t> data)
        {
            static const std::array<std::uint32_t, 256> kTable = []
                {
                    std::array<std::uint32_t, 256> table{};
                    for (std::uint32_t i = 0; i < 256; ++i)
                    {
                        std::uint32_t value = i;
                        for (int bit = 0; bit < 8; ++bit)
                            value = value & 1 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
                        table[i] = value;
                    }
                    return table;
                }();
            std::uint32_t crc = 0xFFFFFFFFu;
            for (const std::uint8_t byte : data)
                crc = kTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFFu;
        }

        void Put16(std::vector<std::uint8_t>& out, std::uint32_t value)
        {
            out.push_back(static_cast<std::uint8_t>(value));
            out.push_back(static_cast<std::uint8_t>(value >> 8));
        }

        void Put32(std::vector<std::uint8_t>& out, std::uint32_t value)
        {
            Put16(out, value & 0xFFFF);
            Put16(out, value >> 16);
        }

        std::uint32_t Get16(std::span<const std::uint8_t> data, std::size_t at)
        {
            return at + 2 <= data.size() ? data[at] | (data[at + 1] << 8) : 0;
        }

        std::uint32_t Get32(std::span<const std::uint8_t> data, std::size_t at)
        {
            return at + 4 <= data.size() ? Get16(data, at) | (Get16(data, at + 2) << 16) : 0;
        }

        std::vector<std::uint8_t> ZipOne(std::string_view name, std::span<const std::uint8_t> data)
        {
            const std::optional<std::vector<std::uint8_t>> packed = Deflate::Compress(data);
            const bool deflated = packed && packed->size() < data.size();
            const std::span<const std::uint8_t> body = deflated ? std::span<const std::uint8_t>(*packed) : data;
            const std::uint32_t crc = Crc32(data);
            const std::uint32_t method = deflated ? 8 : 0;
            constexpr std::uint32_t kDate = (0 << 9) | (1 << 5) | 1;  // 1980-01-01, the archive carries no time
            std::vector<std::uint8_t> out;
            const auto header = [&](bool central)
                {
                    Put32(out, central ? 0x02014B50u : 0x04034B50u);
                    if (central)
                        Put16(out, 20);  // made by
                    Put16(out, 20);      // needed to extract
                    Put16(out, 0);       // flags
                    Put16(out, method);
                    Put16(out, 0);       // time
                    Put16(out, kDate);
                    Put32(out, crc);
                    Put32(out, static_cast<std::uint32_t>(body.size()));
                    Put32(out, static_cast<std::uint32_t>(data.size()));
                    Put16(out, static_cast<std::uint32_t>(name.size()));
                    Put16(out, 0);       // extra
                    if (central)
                    {
                        Put16(out, 0);   // comment
                        Put16(out, 0);   // disk
                        Put16(out, 0);   // internal attributes
                        Put32(out, 0);   // external attributes
                        Put32(out, 0);   // offset of the local header
                    }
                    out.insert(out.end(), name.begin(), name.end());
                };
            header(false);
            out.insert(out.end(), body.begin(), body.end());
            const std::uint32_t directory = static_cast<std::uint32_t>(out.size());
            header(true);
            const std::uint32_t directorySize = static_cast<std::uint32_t>(out.size()) - directory;
            Put32(out, 0x06054B50u);
            Put16(out, 0);
            Put16(out, 0);
            Put16(out, 1);
            Put16(out, 1);
            Put32(out, directorySize);
            Put32(out, directory);
            Put16(out, 0);
            return out;
        }

        std::optional<std::vector<std::uint8_t>> Unzip(std::span<const std::uint8_t> file, std::string_view wanted, std::string& error)
        {
            if (file.size() < 22)
            {
                error = "the archive is cut short";
                return std::nullopt;
            }
            std::size_t end = std::string::npos;
            const std::size_t lowest = file.size() > 22 + 0xFFFF ? file.size() - 22 - 0xFFFF : 0;
            for (std::size_t at = file.size() - 22 + 1; at-- > lowest;)
            {
                if (Get32(file, at) == 0x06054B50u)
                {
                    end = at;
                    break;
                }
            }
            if (end == std::string::npos)
            {
                error = "the file is no ZIP archive (its directory is missing)";
                return std::nullopt;
            }
            const std::uint32_t entries = Get16(file, end + 10);
            std::size_t at = Get32(file, end + 16);
            for (std::uint32_t i = 0; i < entries && at + 46 <= file.size(); ++i)
            {
                if (Get32(file, at) != 0x02014B50u)
                    break;
                const std::uint32_t flags = Get16(file, at + 8);
                const std::uint32_t method = Get16(file, at + 10);
                const std::uint32_t crc = Get32(file, at + 16);
                const std::uint32_t packedSize = Get32(file, at + 20);
                const std::uint32_t size = Get32(file, at + 24);
                const std::uint32_t nameLength = Get16(file, at + 28);
                const std::uint32_t extraLength = Get16(file, at + 30);
                const std::uint32_t commentLength = Get16(file, at + 32);
                const std::uint32_t local = Get32(file, at + 42);
                if (at + 46 + nameLength > file.size())
                    break;
                const std::string name(reinterpret_cast<const char*>(file.data() + at + 46), nameLength);
                at += 46 + nameLength + extraLength + commentLength;
                if (!EqualsNoCase(name, wanted))
                    continue;
                if (flags & 1)
                {
                    error = name + " is encrypted";
                    return std::nullopt;
                }
                if (Get32(file, local) != 0x04034B50u)
                    break;
                const std::size_t data = local + 30 + Get16(file, local + 26) + Get16(file, local + 28);
                if (data + packedSize > file.size())
                {
                    error = name + " is cut short";
                    return std::nullopt;
                }
                const std::span<const std::uint8_t> packed = file.subspan(data, packedSize);
                std::optional<std::vector<std::uint8_t>> out;
                if (method == 0)
                    out = std::vector<std::uint8_t>(packed.begin(), packed.end());
                else if (method == 8)
                    out = Deflate::Decompress(packed, std::min<std::size_t>(Deflate::kMaxInflated, std::max<std::size_t>(size, 1)));
                else
                {
                    error = Format("%s is packed with method %u, only stored and deflate are read", name.c_str(), method);
                    return std::nullopt;
                }
                if (!out || out->size() != size || Crc32(*out) != crc)
                {
                    error = name + " is damaged (it does not unpack to what the archive says)";
                    return std::nullopt;
                }
                return out;
            }
            error = "the archive holds no " + std::string(wanted);
            return std::nullopt;
        }

        // ---------------------------------------------------------------- ids

        constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string Base64(std::span<const std::uint8_t> data)
        {
            std::string out;
            for (std::size_t i = 0; i < data.size(); i += 3)
            {
                const std::uint32_t chunk = (data[i] << 16) | (i + 1 < data.size() ? data[i + 1] << 8 : 0) | (i + 2 < data.size() ? data[i + 2] : 0);
                out += kBase64[(chunk >> 18) & 63];
                out += kBase64[(chunk >> 12) & 63];
                out += i + 1 < data.size() ? kBase64[(chunk >> 6) & 63] : '=';
                out += i + 2 < data.size() ? kBase64[chunk & 63] : '=';
            }
            return out;
        }

        std::optional<std::vector<std::uint8_t>> FromBase64(std::string_view text)
        {
            std::vector<std::uint8_t> out;
            std::uint32_t chunk = 0;
            int bits = 0;
            for (const char c : text)
            {
                if (c == '=')
                    break;
                const char* found = std::strchr(kBase64, c);
                if (!found || c == 0)
                    return std::nullopt;
                chunk = (chunk << 6) | static_cast<std::uint32_t>(found - kBase64);
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    out.push_back(static_cast<std::uint8_t>(chunk >> bits));
                }
            }
            return out;
        }

        // ∑ Both id forms come down to the 16 bytes of .NET's Guid.ToByteArray: the
        //   Base64 form (ReClass.NET 1.2 and older) is those bytes, the text form
        //   (newer builds) writes the first three groups big-endian.
        std::string IdKey(std::string_view raw)
        {
            const std::string trimmed = Trim(raw);
            const std::string_view text = trimmed;
            if (text.size() == 24)
            {
                if (const auto bytes = FromBase64(text); bytes && bytes->size() == 16)
                    return std::string(bytes->begin(), bytes->end());
            }
            std::string hex;
            for (const char c : text)
            {
                if (std::isxdigit(static_cast<unsigned char>(c)))
                    hex += c;
            }
            if (hex.size() != 32)
                return "\x01" + std::string(text);  // not an id we know; still matches itself
            std::string bytes(16, '\0');
            for (int i = 0; i < 16; ++i)
                bytes[i] = static_cast<char>(std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
            std::reverse(bytes.begin(), bytes.begin() + 4);
            std::reverse(bytes.begin() + 4, bytes.begin() + 6);
            std::reverse(bytes.begin() + 6, bytes.begin() + 8);
            return bytes;
        }

        // A stable id for a structure, so exporting the same table twice gives the same file.
        std::array<std::uint8_t, 16> IdFor(std::string_view name, int salt)
        {
            std::array<std::uint8_t, 16> id{};
            for (int half = 0; half < 2; ++half)
            {
                std::uint64_t hash = 14695981039346656037ull ^ static_cast<std::uint64_t>(half * 0x9E37 + salt);
                for (const char c : name)
                    hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
                for (int i = 0; i < 8; ++i)
                    id[half * 8 + i] = static_cast<std::uint8_t>(hash >> (i * 8));
            }
            id[7] = static_cast<std::uint8_t>((id[7] & 0x0F) | 0x40);  // version 4, as Guid.NewGuid makes them
            id[8] = static_cast<std::uint8_t>((id[8] & 0x3F) | 0x80);
            return id;
        }

        // ---------------------------------------------------------------- export

        Xml::Node& AddNode(Xml::Node& parent, std::string_view type, std::string_view name, std::string_view comment = {})
        {
            Xml::Node& node = parent.Add("node");
            node.SetAttribute("type", std::string(type));
            node.SetAttribute("name", std::string(name));
            node.SetAttribute("comment", std::string(comment));
            node.SetAttribute("hidden", "false");
            return node;
        }

        // Hex nodes for a gap, the largest that fit the alignment first.
        void Gap(Xml::Node& parent, std::int64_t from, std::int64_t to, std::string_view comment = {})
        {
            bool first = true;
            while (from < to)
            {
                int size = 8;
                while (size > 1 && (from % size != 0 || from + size > to))
                    size /= 2;
                AddNode(parent, Format("Hex%dNode", size * 8), "", first ? comment : std::string_view());
                first = false;
                from += size;
            }
        }

        std::string IntNode(const StructField& field, int size)
        {
            const char* kind = field.Display == FieldDisplay::Signed ? "Int" : field.Display == FieldDisplay::Unsigned ? "UInt" : "Hex";
            return Format("%s%dNode", kind, size * 8);
        }

        bool PowerSize(int size)
        {
            return size == 1 || size == 2 || size == 4 || size == 8;
        }

        class ReClassWriter
        {
        public:
            ReClassWriter(const StructureStore& store, const std::vector<const StructDefinition*>& order, bool is64Bit)
                : m_store(store), m_pointer(is64Bit ? 8 : 4)
            {
                std::set<std::string> used;
                for (const StructDefinition* definition : order)
                {
                    std::array<std::uint8_t, 16> id;
                    for (int salt = 0;; ++salt)
                    {
                        id = IdFor(Lower(definition->Name), salt);
                        if (used.insert(std::string(id.begin(), id.end())).second)
                            break;
                    }
                    m_ids[Lower(definition->Name)] = Base64(id);
                }
            }

            void Class(Xml::Node& classes, const StructDefinition& definition)
            {
                Xml::Node& node = classes.Add("class");
                node.SetAttribute("uuid", m_ids.at(Lower(definition.Name)));
                node.SetAttribute("name", definition.Name);
                node.SetAttribute("comment", "");
                node.SetAttribute("address", "0");
                std::vector<StructField> fields = definition.Fields;
                std::stable_sort(fields.begin(), fields.end(), [](const StructField& a, const StructField& b) { return a.Offset < b.Offset; });
                std::vector<std::string> left;
                std::int64_t cursor = 0;
                for (std::size_t i = 0; i < fields.size(); ++i)
                {
                    const StructField& field = fields[i];
                    const std::string shown = field.Name.empty() ? Format("+0x%X", field.Offset) : field.Name;
                    if (field.Offset < cursor || field.Size <= 0)
                    {
                        left.push_back(shown);
                        continue;
                    }
                    Gap(node, cursor, field.Offset);
                    if (field.Type == Scan::ValueType::Binary)
                    {
                        // the bit fields of one unit share a BitFieldNode
                        std::string bits;
                        std::size_t last = i;
                        for (std::size_t j = i; j < fields.size() && fields[j].Offset == field.Offset && fields[j].Type == Scan::ValueType::Binary; ++j)
                        {
                            last = j;
                            bits += Format("%s%s bits %d..%d", bits.empty() ? "" : ", ", fields[j].Name.c_str(), fields[j].BitStart, fields[j].BitStart + fields[j].BitLength - 1);
                        }
                        if (PowerSize(field.Size))
                        {
                            AddNode(node, "BitFieldNode", field.Name, bits).SetAttribute("bits", std::to_string(field.Size * 8));
                        }
                        else
                            Gap(node, field.Offset, field.Offset + field.Size, bits);
                        cursor = field.Offset + field.Size;
                        i = last;
                        continue;
                    }
                    Field(node, field);
                    cursor = static_cast<std::int64_t>(field.Offset) + field.Size;
                }
                Gap(node, cursor, definition.Size());
                if (!left.empty())
                {
                    std::string text = "Left out, they overlap the field before: ";
                    for (std::size_t i = 0; i < left.size(); ++i)
                        text += (i ? ", " : "") + left[i];
                    node.SetAttribute("comment", text);
                }
            }

        private:
            const std::string* IdOf(std::string_view name) const
            {
                const auto it = m_ids.find(Lower(name));
                return it == m_ids.end() ? nullptr : &it->second;
            }

            const StructDefinition* Find(std::string_view name) const
            {
                for (const auto& item : m_store.Items)
                {
                    if (EqualsNoCase(item->Name, name))
                        return item.get();
                }
                return nullptr;
            }

            void Field(Xml::Node& node, const StructField& field)
            {
                const auto fill = [&](std::string_view comment)
                    {
                        if (field.Size <= 8 && PowerSize(field.Size) && field.Offset % field.Size == 0)
                            AddNode(node, Format("Hex%dNode", field.Size * 8), field.Name, comment);
                        else
                        {
                            Xml::Node& array = AddNode(node, "ArrayNode", field.Name, comment);
                            array.SetAttribute("count", std::to_string(field.Size));
                            AddNode(array, "UInt8Node", "");
                        }
                    };
                if (!field.CustomType.empty())
                {
                    fill("custom type " + field.CustomType);
                    return;
                }
                switch (field.Type)
                {
                case Scan::ValueType::Byte:
                case Scan::ValueType::TwoBytes:
                case Scan::ValueType::FourBytes:
                case Scan::ValueType::EightBytes:
                    if (PowerSize(field.Size))
                        AddNode(node, IntNode(field, field.Size), field.Name);
                    else
                        fill({});
                    return;
                case Scan::ValueType::Float:
                    if (field.Size == 4)
                        AddNode(node, "FloatNode", field.Name);
                    else
                        fill("float");
                    return;
                case Scan::ValueType::Double:
                    if (field.Size == 8)
                        AddNode(node, "DoubleNode", field.Name);
                    else
                        fill("double");
                    return;
                case Scan::ValueType::String:
                    if (field.Unicode && field.Size % 2 == 0)
                        AddNode(node, "Utf16TextNode", field.Name).SetAttribute("length", std::to_string(field.Size / 2));
                    else
                        AddNode(node, "Utf8TextNode", field.Name).SetAttribute("length", std::to_string(field.Size));
                    return;
                case Scan::ValueType::Pointer:
                {
                    if (field.Size != m_pointer)
                    {
                        fill("pointer" + (field.ChildStructure.empty() ? std::string() : " to " + field.ChildStructure));
                        return;
                    }
                    const std::string* id = field.ChildStructure.empty() ? nullptr : IdOf(field.ChildStructure);
                    Xml::Node& pointer = AddNode(node, "PointerNode", field.Name, !field.ChildStructure.empty() && !id ? "points to " + field.ChildStructure + " (not exported)" : std::string());
                    if (id)
                        AddNode(pointer, "ClassInstanceNode", "").SetAttribute("reference", *id);
                    return;
                }
                default:
                    if (field.Type != Scan::ValueType::Pointer && !field.ChildStructure.empty())
                    {
                        const std::string* id = IdOf(field.ChildStructure);
                        const StructDefinition* child = Find(field.ChildStructure);
                        if (id && child && child->Size() == field.Size)
                        {
                            AddNode(node, "ClassInstanceNode", field.Name).SetAttribute("reference", *id);
                            return;
                        }
                        fill(field.ChildStructure + (id ? " (its size differs)" : " (not exported)"));
                        return;
                    }
                    fill({});
                    return;
                }
            }

            const StructureStore& m_store;
            int m_pointer = 8;
            std::map<std::string, std::string> m_ids;
        };

        // ---------------------------------------------------------------- import

        bool AutoName(std::string_view name)
        {
            // ∑ ReClass.NET names every new node N + 8 hex digits; such a Hex node is
            //   space nobody looked at yet.
            if (name.empty())
                return true;
            if (name.size() != 9 || name[0] != 'N')
                return false;
            return std::all_of(name.begin() + 1, name.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
        }

        std::string NodeType(const Xml::Node& node)
        {
            std::string type = node.Attribute("type").value_or("");
            static const std::map<std::string, std::string, std::less<>> kLegacy = { { "UTF8TextNode", "Utf8TextNode" }, { "UTF8TextPtrNode", "Utf8TextPtrNode" },
                { "UTF16TextNode", "Utf16TextNode" }, { "UTF16TextPtrNode", "Utf16TextPtrNode" }, { "UTF32TextNode", "Utf32TextNode" },
                { "UTF32TextPtrNode", "Utf32TextPtrNode" }, { "VTableNode", "VirtualMethodTableNode" } };
            if (const auto it = kLegacy.find(type); it != kLegacy.end())
                return it->second;
            return type;
        }

        const Xml::Node* FirstChild(const Xml::Node& node)
        {
            return node.Children.empty() ? nullptr : node.Children.front().get();
        }

        int Number(const Xml::Node& node, std::string_view name, int fallback)
        {
            const std::optional<std::string> text = node.Attribute(name);
            if (!text || text->empty())
                return fallback;
            return static_cast<int>(std::clamp<long long>(std::atoll(text->c_str()), 0, kMaxFieldOffset));
        }

        class ReClassReader
        {
        public:
            ReClassReader(const Xml::Node& root, int pointer) : m_root(root), m_pointer(pointer) {}

            StructImport Run()
            {
                if (const Xml::Node* enums = m_root.Child("enums"))
                {
                    for (const Xml::Node* item : enums->ChildrenNamed("enum"))
                    {
                        const std::string size = item->Attribute("size").value_or("FourBytes");
                        m_enums[item->Attribute("name").value_or("")] = size == "OneByte" ? 1 : size == "TwoBytes" ? 2 : size == "EightBytes" ? 8 : 4;
                    }
                }
                const Xml::Node* classes = m_root.Child("classes");
                std::set<std::string> names;
                for (const Xml::Node* item : classes ? classes->ChildrenNamed("class") : std::vector<const Xml::Node*>())
                {
                    Class entry;
                    entry.Node = item;
                    entry.Name = item->Attribute("name").value_or("");
                    if (entry.Name.empty())
                        entry.Name = Format("Class%zu", m_classes.size() + 1);
                    std::string unique = entry.Name;
                    for (int n = 2; !names.insert(Lower(unique)).second; ++n)
                        unique = entry.Name + "_" + std::to_string(n);
                    entry.Name = unique;
                    const std::string key = IdKey(item->Attribute("uuid").value_or(""));
                    if (!m_ids.count(key))
                        m_ids[key] = m_classes.size();
                    m_classes.push_back(entry);
                }
                for (std::size_t i = 0; i < m_classes.size(); ++i)
                {
                    StructDefinition definition;
                    definition.Name = m_classes[i].Name;
                    m_current = m_classes[i].Name;
                    int offset = 0;
                    for (const auto& child : m_classes[i].Node->Children)
                    {
                        if (child->Name != "node")
                            continue;
                        Emit(definition, *child, offset, nullptr, false, 0);
                        offset += Size(*child, 0);
                    }
                    PadStructure(definition, ClassSize(i));
                    definition.Sort();
                    m_import.Structures.push_back(std::move(definition));
                }
                return std::move(m_import);
            }

        private:
            struct Class
            {
                std::string Name;
                const Xml::Node* Node = nullptr;
                int Size = -1;
                bool Busy = false;
            };

            void Warn(std::string text)
            {
                if (m_warned.insert(text).second && m_import.Warnings.size() < 50)
                    m_import.Warnings.push_back(m_current + ": " + text);
            }

            const Class* Referenced(const Xml::Node& node)
            {
                const auto it = m_ids.find(IdKey(node.Attribute("reference").value_or("")));
                if (it == m_ids.end())
                {
                    Warn("a node points to a class that is not in the file");
                    return nullptr;
                }
                return &m_classes[it->second];
            }

            int ClassSize(std::size_t index)
            {
                Class& entry = m_classes[index];
                if (entry.Size >= 0)
                    return entry.Size;
                if (entry.Busy)
                {
                    Warn("the class " + entry.Name + " holds itself; it is taken as empty there");
                    return 0;
                }
                entry.Busy = true;
                std::int64_t size = 0;
                for (const auto& child : entry.Node->Children)
                {
                    if (child->Name == "node")
                        size += Size(*child, 0);
                }
                entry.Busy = false;
                entry.Size = static_cast<int>(std::min<std::int64_t>(size, kMaxFieldOffset));
                return entry.Size;
            }

            int ClassSizeOf(const Xml::Node& node)
            {
                const Class* entry = Referenced(node);
                return entry ? ClassSize(static_cast<std::size_t>(entry - m_classes.data())) : 0;
            }

            int Size(const Xml::Node& node, int depth)
            {
                if (depth > 32)
                    return 0;
                const std::string type = NodeType(node);
                static const std::map<std::string, int, std::less<>> kFixed = { { "BoolNode", 1 }, { "Int8Node", 1 }, { "UInt8Node", 1 }, { "Hex8Node", 1 },
                    { "Int16Node", 2 }, { "UInt16Node", 2 }, { "Hex16Node", 2 }, { "Int32Node", 4 }, { "UInt32Node", 4 }, { "Hex32Node", 4 }, { "FloatNode", 4 },
                    { "Int64Node", 8 }, { "UInt64Node", 8 }, { "Hex64Node", 8 }, { "DoubleNode", 8 }, { "Vector2Node", 8 }, { "Vector3Node", 12 },
                    { "Vector4Node", 16 }, { "Matrix3x3Node", 36 }, { "Matrix3x4Node", 48 }, { "Matrix4x4Node", 64 } };
                if (const auto it = kFixed.find(type); it != kFixed.end())
                    return it->second;
                static const std::set<std::string, std::less<>> kPointer = { "NIntNode", "NUIntNode", "Utf8TextPtrNode", "Utf16TextPtrNode", "Utf32TextPtrNode",
                    "FunctionPtrNode", "VirtualMethodTableNode", "FunctionNode", "PointerNode", "ClassPtrNode" };
                if (kPointer.count(type))
                    return m_pointer;
                if (type == "Utf8TextNode")
                    return Number(node, "length", 0);
                if (type == "Utf16TextNode")
                    return Number(node, "length", 0) * 2;
                if (type == "Utf32TextNode")
                    return Number(node, "length", 0) * 4;
                if (type == "BitFieldNode")
                {
                    const int bits = Number(node, "bits", m_pointer * 8);
                    return bits <= 8 ? 1 : bits <= 16 ? 2 : bits <= 32 ? 4 : 8;
                }
                if (type == "EnumNode")
                {
                    const auto it = m_enums.find(node.Attribute("reference").value_or(""));
                    return it == m_enums.end() ? 4 : it->second;
                }
                if (type == "ClassInstanceNode")
                    return ClassSizeOf(node);
                if (type == "ClassInstanceArrayNode")
                    return static_cast<int>(std::min<std::int64_t>(static_cast<std::int64_t>(Number(node, "count", 0)) * ClassSizeOf(node), kMaxFieldOffset));
                if (type == "ClassPtrArrayNode")
                    return Number(node, "count", 0) * m_pointer;
                if (type == "ArrayNode")
                {
                    const Xml::Node* element = FirstChild(node);
                    return element ? static_cast<int>(std::min<std::int64_t>(static_cast<std::int64_t>(Number(node, "count", 0)) * Size(*element, depth + 1), kMaxFieldOffset)) : 0;
                }
                if (type == "UnionNode")
                {
                    int size = 0;
                    for (const auto& child : node.Children)
                        size = std::max(size, Size(*child, depth + 1));
                    return size;
                }
                Warn("the node type " + type + " is not known; it takes no room, so the offsets after it may be wrong");
                return 0;
            }

            void Add(StructDefinition& definition, int offset, std::string name, Scan::ValueType type, int size, FieldDisplay display = FieldDisplay::Unsigned)
            {
                if (offset >= kMaxFieldOffset || size <= 0)
                    return;
                StructField field;
                field.Offset = offset;
                field.Name = std::move(name);
                field.Type = type;
                field.Size = std::min(size, kMaxFieldBytes);
                field.Display = display;
                definition.Fields.push_back(std::move(field));
                if (size > kMaxFieldBytes)
                    Warn(Format("%s is longer than a dissect field can be; it is cut to %d bytes", definition.Fields.back().Name.c_str(), kMaxFieldBytes));
            }

            static Scan::ValueType IntType(int size)
            {
                return size == 1 ? Scan::ValueType::Byte : size == 2 ? Scan::ValueType::TwoBytes : size == 4 ? Scan::ValueType::FourBytes : Scan::ValueType::EightBytes;
            }

            // An array: each element as its own field while there are few, else bytes.
            void Elements(StructDefinition& definition, const Xml::Node& element, int offset, const std::string& name, int count, int depth)
            {
                const int size = Size(element, depth + 1);
                if (count <= 0 || size <= 0)
                    return;
                const std::string type = NodeType(element);
                if (type == "Utf8TextNode" || type == "Utf16TextNode" || count > 64 || type == "UInt8Node" || type == "Int8Node" || type == "Hex8Node")
                {
                    Add(definition, offset, name, Scan::ValueType::ByteArray, static_cast<int>(std::min<std::int64_t>(static_cast<std::int64_t>(size) * count, kMaxFieldOffset)));
                    return;
                }
                for (int i = 0; i < count; ++i)
                {
                    const std::string elementName = Format("%s[%d]", name.c_str(), i);
                    Emit(definition, element, offset + i * size, &elementName, true, depth + 1);
                }
            }

            void Emit(StructDefinition& definition, const Xml::Node& node, int offset, const std::string* nameOverride, bool keep, int depth)
            {
                if (depth > 32 || offset >= kMaxFieldOffset)
                    return;
                const std::string type = NodeType(node);
                const std::string own = node.Attribute("name").value_or("");
                const std::string name = nameOverride ? *nameOverride : own;
                const int size = Size(node, depth);
                if (type.starts_with("Hex") && type.ends_with("Node") && type.size() <= 9)
                {
                    if (keep || !AutoName(own))
                        Add(definition, offset, name, IntType(size), size, FieldDisplay::Hex);
                    return;
                }
                if (type.starts_with("Int") || type.starts_with("UInt") || type == "NIntNode" || type == "NUIntNode" || type == "BoolNode")
                {
                    const bool isSigned = type.starts_with("Int") || type == "NIntNode";
                    Add(definition, offset, name, IntType(size), size, isSigned ? FieldDisplay::Signed : FieldDisplay::Unsigned);
                    return;
                }
                if (type == "FloatNode")
                    return Add(definition, offset, name, Scan::ValueType::Float, 4);
                if (type == "DoubleNode")
                    return Add(definition, offset, name, Scan::ValueType::Double, 8);
                if (type.starts_with("Vector"))
                {
                    static const char* kAxes[] = { "x", "y", "z", "w" };
                    for (int i = 0; i < size / 4; ++i)
                        Add(definition, offset + i * 4, name + "." + kAxes[i], Scan::ValueType::Float, 4);
                    return;
                }
                if (type.starts_with("Matrix"))
                {
                    const int columns = type == "Matrix3x3Node" ? 3 : 4;
                    for (int i = 0; i < size / 4; ++i)
                        Add(definition, offset + i * 4, Format("%s[%d][%d]", name.c_str(), i / columns, i % columns), Scan::ValueType::Float, 4);
                    return;
                }
                if (type == "Utf8TextNode" || type == "Utf16TextNode")
                {
                    Add(definition, offset, name, Scan::ValueType::String, size);
                    if (size > 0 && type == "Utf16TextNode")
                        definition.Fields.back().Unicode = true;
                    return;
                }
                if (type == "Utf32TextNode")
                {
                    Add(definition, offset, name, Scan::ValueType::ByteArray, size);
                    return;
                }
                if (type == "VirtualMethodTableNode" || type.ends_with("TextPtrNode") || type == "FunctionPtrNode" || type == "FunctionNode")
                {
                    Add(definition, offset, name.empty() && type == "VirtualMethodTableNode" ? "vftable" : name, Scan::ValueType::Pointer, m_pointer, FieldDisplay::Hex);
                    return;
                }
                if (type == "PointerNode" || type == "ClassPtrNode")
                {
                    Add(definition, offset, name, Scan::ValueType::Pointer, m_pointer, FieldDisplay::Hex);
                    const Xml::Node* target = type == "ClassPtrNode" ? &node : FirstChild(node);
                    if (target && (type == "ClassPtrNode" || NodeType(*target) == "ClassInstanceNode"))
                    {
                        if (const Class* entry = Referenced(*target))
                            definition.Fields.back().ChildStructure = entry->Name;
                    }
                    return;
                }
                if (type == "ClassInstanceNode")
                {
                    const Class* entry = Referenced(node);
                    if (!entry || size <= 0)
                        return;
                    Add(definition, offset, name, Scan::ValueType::ByteArray, size);
                    definition.Fields.back().ChildStructure = entry->Name;
                    return;
                }
                if (type == "ArrayNode")
                {
                    if (const Xml::Node* element = FirstChild(node))
                        Elements(definition, *element, offset, name, Number(node, "count", 0), depth);
                    return;
                }
                if (type == "ClassInstanceArrayNode" || type == "ClassPtrArrayNode")
                {
                    // the old forms: the reference sits on the array itself
                    Xml::Node element;
                    element.Name = "node";
                    element.SetAttribute("type", type == "ClassInstanceArrayNode" ? "ClassInstanceNode" : "ClassPtrNode");
                    element.SetAttribute("reference", node.Attribute("reference").value_or(""));
                    Elements(definition, element, offset, name, Number(node, "count", 0), depth);
                    return;
                }
                if (type == "UnionNode")
                {
                    for (const auto& child : node.Children)
                    {
                        const std::string member = child->Attribute("name").value_or("");
                        const std::string joined = name.empty() || AutoName(name) ? member : name + "." + member;
                        Emit(definition, *child, offset, &joined, keep, depth + 1);
                    }
                    return;
                }
                if (type == "EnumNode")
                {
                    Add(definition, offset, name, IntType(size), size, FieldDisplay::Signed);
                    return;
                }
                if (type == "BitFieldNode")
                {
                    Add(definition, offset, name, IntType(size), size, FieldDisplay::Binary);
                    return;
                }
            }

            const Xml::Node& m_root;
            int m_pointer = 8;
            std::vector<Class> m_classes;
            std::map<std::string, std::size_t> m_ids;
            std::map<std::string, int> m_enums;
            std::string m_current;
            std::set<std::string> m_warned;
            StructImport m_import;
        };
    }

    std::vector<std::uint8_t> ExportReClass(const StructureStore& store, const std::vector<std::string>& roots, bool is64Bit, bool children)
    {
        const std::vector<const StructDefinition*> order = ExportOrder(store, roots, children);
        Xml::Node root;
        root.Name = "reclass";
        root.SetAttribute("version", "65537");
        root.SetAttribute("type", is64Bit ? "x64" : "x86");
        root.Add("custom_data");
        root.Add("type_mapping");
        root.Add("enums");
        Xml::Node& classes = root.Add("classes");
        ReClassWriter writer(store, order, is64Bit);
        for (const StructDefinition* definition : order)
            writer.Class(classes, *definition);
        const std::string xml = "\xEF\xBB\xBF" + Xml::Write(root);
        return ZipOne("Data.xml", std::span(reinterpret_cast<const std::uint8_t*>(xml.data()), xml.size()));
    }

    bool LooksLikeReClass(std::span<const std::uint8_t> file)
    {
        if (file.size() >= 4 && Get32(file, 0) == 0x04034B50u)
            return true;
        const std::string_view text(reinterpret_cast<const char*>(file.data()), std::min<std::size_t>(file.size(), 4096));
        return text.find("<reclass") != std::string_view::npos;
    }

    Result<StructImport> ImportReClass(std::span<const std::uint8_t> file, const StructImportOptions& options)
    {
        std::vector<std::uint8_t> unpacked;
        std::span<const std::uint8_t> xml = file;
        if (file.size() >= 4 && Get32(file, 0) == 0x04034B50u)
        {
            std::string error;
            std::optional<std::vector<std::uint8_t>> data = Unzip(file, "Data.xml", error);
            if (!data)
                return Status::Fail(ErrorCode::StructNotFound, "the ReClass.NET project: " + error);
            unpacked = std::move(*data);
            xml = unpacked;
        }
        const Xml::ParseResult parsed = Xml::Parse(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size()));
        if (!parsed.Root)
            return Status::Fail(ErrorCode::StructNotFound, Format("the ReClass.NET project (%s, line %d)", parsed.Error.c_str(), parsed.Line));
        if (parsed.Root->Name != "reclass" || !parsed.Root->Child("classes"))
            return Status::Fail(ErrorCode::StructNotFound, "the file (it is no ReClass.NET project: <reclass> with <classes> is missing)");
        const std::string platform = parsed.Root->Attribute("type").value_or("");
        const int pointer = platform == "x86" ? 4 : platform == "x64" ? 8 : options.Is64Bit ? 8 : 4;
        ReClassReader reader(*parsed.Root, pointer);
        StructImport imported = reader.Run();
        if (imported.Structures.empty())
            return Status::Fail(ErrorCode::StructNotFound, "the ReClass.NET project (it has no classes)");
        return imported;
    }
}
