#include "Target/ClrMetadata.h"

#include <algorithm>
#include <cstring>

namespace Manifold::Target
{
    namespace
    {
        enum class Kind : std::uint8_t { U16, U32, Str, Guid, Blob, Tab, Coded };

        enum Coded : std::uint8_t
        {
            TypeDefOrRef, HasConstant, HasCustomAttribute, HasFieldMarshal, HasDeclSecurity, MemberRefParent,
            HasSemantics, MethodDefOrRef, MemberForwarded, Implementation, CustomAttributeType, ResolutionScope,
            TypeOrMethodDef, CodedCount
        };

        struct Column
        {
            Kind What;
            std::uint8_t Param;  // the table of a Tab column, the kind of a Coded one
        };

        constexpr Column U16{ Kind::U16, 0 };
        constexpr Column U32{ Kind::U32, 0 };
        constexpr Column Str{ Kind::Str, 0 };
        constexpr Column Gd{ Kind::Guid, 0 };
        constexpr Column Bl{ Kind::Blob, 0 };
        constexpr Column Tab(int table) { return { Kind::Tab, static_cast<std::uint8_t>(table) }; }
        constexpr Column Cd(Coded coded) { return { Kind::Coded, coded }; }

        using T = ClrMetadata::Table;

        // ECMA-335 II.22: the columns of every table up to GenericParamConstraint.
        struct Schema
        {
            std::uint8_t Count;
            Column Columns[9];
        };
        const Schema kSchemas[T::TableCount] = {
            { 5, { U16, Str, Gd, Gd, Gd } },                                        // Module
            { 3, { Cd(ResolutionScope), Str, Str } },                               // TypeRef
            { 6, { U32, Str, Str, Cd(TypeDefOrRef), Tab(T::Field), Tab(T::MethodDef) } }, // TypeDef
            { 1, { Tab(T::Field) } },                                               // FieldPtr
            { 3, { U16, Str, Bl } },                                                // Field
            { 1, { Tab(T::MethodDef) } },                                           // MethodPtr
            { 6, { U32, U16, U16, Str, Bl, Tab(T::Param) } },                       // MethodDef
            { 1, { Tab(T::Param) } },                                               // ParamPtr
            { 3, { U16, U16, Str } },                                               // Param
            { 2, { Tab(T::TypeDef), Cd(TypeDefOrRef) } },                           // InterfaceImpl
            { 3, { Cd(MemberRefParent), Str, Bl } },                                // MemberRef
            { 3, { U16, Cd(HasConstant), Bl } },                                    // Constant (type byte + pad)
            { 3, { Cd(HasCustomAttribute), Cd(CustomAttributeType), Bl } },         // CustomAttribute
            { 2, { Cd(HasFieldMarshal), Bl } },                                     // FieldMarshal
            { 3, { U16, Cd(HasDeclSecurity), Bl } },                                // DeclSecurity
            { 3, { U16, U32, Tab(T::TypeDef) } },                                   // ClassLayout
            { 2, { U32, Tab(T::Field) } },                                          // FieldLayout
            { 1, { Bl } },                                                          // StandAloneSig
            { 2, { Tab(T::TypeDef), Tab(T::Event) } },                              // EventMap
            { 1, { Tab(T::Event) } },                                               // EventPtr
            { 3, { U16, Str, Cd(TypeDefOrRef) } },                                  // Event
            { 2, { Tab(T::TypeDef), Tab(T::Property) } },                           // PropertyMap
            { 1, { Tab(T::Property) } },                                            // PropertyPtr
            { 3, { U16, Str, Bl } },                                                // Property
            { 3, { U16, Tab(T::MethodDef), Cd(HasSemantics) } },                    // MethodSemantics
            { 3, { Tab(T::TypeDef), Cd(MethodDefOrRef), Cd(MethodDefOrRef) } },     // MethodImpl
            { 1, { Str } },                                                         // ModuleRef
            { 1, { Bl } },                                                          // TypeSpec
            { 4, { U16, Cd(MemberForwarded), Str, Tab(T::ModuleRef) } },            // ImplMap
            { 2, { U32, Tab(T::Field) } },                                          // FieldRVA
            { 2, { U32, U32 } },                                                    // EncLog
            { 1, { U32 } },                                                         // EncMap
            { 9, { U32, U16, U16, U16, U16, U32, Bl, Str, Str } },                  // Assembly
            { 1, { U32 } },                                                         // AssemblyProcessor
            { 3, { U32, U32, U32 } },                                               // AssemblyOS
            { 9, { U16, U16, U16, U16, U32, Bl, Str, Str, Bl } },                   // AssemblyRef
            { 2, { U32, Tab(T::AssemblyRef) } },                                    // AssemblyRefProcessor
            { 4, { U32, U32, U32, Tab(T::AssemblyRef) } },                          // AssemblyRefOS
            { 3, { U32, Str, Bl } },                                                // File
            { 5, { U32, U32, Str, Str, Cd(Implementation) } },                      // ExportedType
            { 4, { U32, U32, Str, Cd(Implementation) } },                           // ManifestResource
            { 2, { Tab(T::TypeDef), Tab(T::TypeDef) } },                            // NestedClass
            { 4, { U16, U16, Cd(TypeOrMethodDef), Str } },                          // GenericParam
            { 2, { Cd(MethodDefOrRef), Bl } },                                      // MethodSpec
            { 2, { Tab(T::GenericParam), Cd(TypeDefOrRef) } },                      // GenericParamConstraint
        };

        // ECMA-335 II.24.2.6: the tables a coded index can point to, in tag
        // order (-1 for a tag that is not used), and its tag width.
        struct CodedInfo
        {
            std::uint8_t Bits;
            std::uint8_t Count;
            int Tables[22];
        };
        const CodedInfo kCoded[CodedCount] = {
            { 2, 3, { T::TypeDef, T::TypeRef, T::TypeSpec } },
            { 2, 3, { T::Field, T::Param, T::Property } },
            { 5, 22, { T::MethodDef, T::Field, T::TypeRef, T::TypeDef, T::Param, T::InterfaceImpl, T::MemberRef, T::Module,
                         T::DeclSecurity, T::Property, T::Event, T::StandAloneSig, T::ModuleRef, T::TypeSpec, T::Assembly,
                         T::AssemblyRef, T::File, T::ExportedType, T::ManifestResource, T::GenericParam,
                         T::GenericParamConstraint, T::MethodSpec } },
            { 1, 2, { T::Field, T::Param } },
            { 2, 3, { T::TypeDef, T::MethodDef, T::Assembly } },
            { 3, 5, { T::TypeDef, T::TypeRef, T::ModuleRef, T::MethodDef, T::TypeSpec } },
            { 1, 2, { T::Event, T::Property } },
            { 1, 2, { T::MethodDef, T::MemberRef } },
            { 1, 2, { T::Field, T::MethodDef } },
            { 2, 3, { T::File, T::AssemblyRef, T::ExportedType } },
            { 3, 5, { -1, -1, T::MethodDef, T::MemberRef, -1 } },
            { 2, 4, { T::Module, T::ModuleRef, T::AssemblyRef, T::TypeRef } },
            { 1, 2, { T::TypeDef, T::MethodDef } },
        };

        constexpr std::uint32_t Token(int table, std::uint32_t row) { return (static_cast<std::uint32_t>(table) << 24) | row; }

        std::uint32_t Read32(const std::uint8_t* at)
        {
            std::uint32_t value = 0;
            std::memcpy(&value, at, sizeof(value));
            return value;
        }

        std::uint16_t Read16(const std::uint8_t* at)
        {
            std::uint16_t value = 0;
            std::memcpy(&value, at, sizeof(value));
            return value;
        }

        // ECMA-335 II.23.2: a compressed unsigned integer.
        std::uint32_t Compressed(std::span<const std::uint8_t> bytes, std::size_t& at, bool& ok)
        {
            if (at >= bytes.size())
            {
                ok = false;
                return 0;
            }
            const std::uint8_t first = bytes[at];
            if ((first & 0x80) == 0)
            {
                at += 1;
                return first;
            }
            if ((first & 0xC0) == 0x80)
            {
                if (at + 2 > bytes.size())
                {
                    ok = false;
                    return 0;
                }
                const std::uint32_t value = (static_cast<std::uint32_t>(first & 0x3F) << 8) | bytes[at + 1];
                at += 2;
                return value;
            }
            if ((first & 0xE0) == 0xC0 && at + 4 <= bytes.size())
            {
                const std::uint32_t value = (static_cast<std::uint32_t>(first & 0x1F) << 24) | (static_cast<std::uint32_t>(bytes[at + 1]) << 16)
                    | (static_cast<std::uint32_t>(bytes[at + 2]) << 8) | bytes[at + 3];
                at += 4;
                return value;
            }
            ok = false;
            return 0;
        }

        std::uint32_t TypeDefOrRefToken(std::uint32_t coded)
        {
            static constexpr int kTables[] = { T::TypeDef, T::TypeRef, T::TypeSpec };
            const std::uint32_t tag = coded & 3;
            return tag < 3 ? Token(kTables[tag], coded >> 2) : 0;
        }

        // "List`1" -> "List", for generic instances spelled with their arguments.
        std::string WithoutArity(const std::string& name)
        {
            const std::size_t tick = name.rfind('`');
            if (tick == std::string::npos || tick + 1 >= name.size())
                return name;
            for (std::size_t i = tick + 1; i < name.size(); ++i)
            {
                if (name[i] < '0' || name[i] > '9')
                    return name;
            }
            return name.substr(0, tick);
        }
    }

    bool ClrMetadata::Parse(std::span<const std::uint8_t> metadata)
    {
        m_valid = false;
        m_data.assign(metadata.begin(), metadata.end());
        const std::size_t size = m_data.size();
        const std::uint8_t* base = m_data.data();
        if (size < 20 || Read32(base) != 0x424A5342)  // "BSJB"
            return false;
        const std::uint32_t versionLength = Read32(base + 12);
        std::size_t at = 16 + static_cast<std::size_t>(versionLength);
        if (versionLength > 255 || at + 4 > size)
            return false;
        const std::uint16_t streams = Read16(base + at + 2);
        at += 4;
        std::size_t tables = 0, tablesSize = 0;
        for (std::uint16_t i = 0; i < streams; ++i)
        {
            if (at + 8 > size)
                return false;
            const std::uint32_t offset = Read32(base + at);
            const std::uint32_t length = Read32(base + at + 4);
            at += 8;
            const std::size_t nameAt = at;
            while (at < size && base[at] != 0 && at - nameAt < 32)
                ++at;
            if (at >= size || base[at] != 0)
                return false;
            const std::string name(reinterpret_cast<const char*>(base + nameAt), at - nameAt);
            at = (at + 4) & ~static_cast<std::size_t>(3);
            if (offset > size || length > size - offset)
                return false;
            if (name == "#~" || name == "#-")
                tables = offset, tablesSize = length;
            else if (name == "#Strings")
                m_strings = offset, m_stringsSize = length;
            else if (name == "#Blob")
                m_blob = offset, m_blobSize = length;
        }
        if (!tablesSize || tablesSize < 24)
            return false;

        const std::uint8_t* header = base + tables;
        const std::uint8_t heapSizes = header[6];
        m_stringIndex = (heapSizes & 1) ? 4 : 2;
        m_guidIndex = (heapSizes & 2) ? 4 : 2;
        m_blobIndex = (heapSizes & 4) ? 4 : 2;
        std::uint64_t valid = 0;
        std::memcpy(&valid, header + 8, sizeof(valid));
        // Tables after GenericParamConstraint belong to portable PDBs; an
        // assembly with any of them has a layout this reader does not know.
        if (valid >> TableCount)
            return false;
        std::size_t cursor = 24;
        m_rows.fill(0);
        for (int table = 0; table < TableCount; ++table)
        {
            if (!(valid & (1ull << table)))
                continue;
            if (cursor + 4 > tablesSize)
                return false;
            m_rows[static_cast<std::size_t>(table)] = Read32(header + cursor);
            cursor += 4;
        }
        if (heapSizes & 0x40)  // extra data after the row counts (#- streams)
            cursor += 4;

        const auto codedSize = [this](Coded coded)
            {
                const CodedInfo& info = kCoded[coded];
                std::uint32_t most = 0;
                for (int i = 0; i < info.Count; ++i)
                {
                    if (info.Tables[i] >= 0)
                        most = std::max(most, m_rows[static_cast<std::size_t>(info.Tables[i])]);
                }
                return most < (1u << (16 - info.Bits)) ? 2 : 4;
            };
        std::size_t at2 = tables + cursor;
        for (int table = 0; table < TableCount; ++table)
        {
            const Schema& schema = kSchemas[table];
            std::uint32_t offset = 0;
            for (int column = 0; column < schema.Count; ++column)
            {
                const Column& info = schema.Columns[column];
                int width = 2;
                switch (info.What)
                {
                    case Kind::U16: width = 2; break;
                    case Kind::U32: width = 4; break;
                    case Kind::Str: width = m_stringIndex; break;
                    case Kind::Guid: width = m_guidIndex; break;
                    case Kind::Blob: width = m_blobIndex; break;
                    case Kind::Tab: width = m_rows[info.Param] < 0x10000 ? 2 : 4; break;
                    case Kind::Coded: width = codedSize(static_cast<Coded>(info.Param)); break;
                }
                m_columnAt[static_cast<std::size_t>(table)][static_cast<std::size_t>(column)] = static_cast<std::uint8_t>(offset);
                m_columnSize[static_cast<std::size_t>(table)][static_cast<std::size_t>(column)] = static_cast<std::uint8_t>(width);
                offset += static_cast<std::uint32_t>(width);
            }
            m_rowSize[static_cast<std::size_t>(table)] = offset;
            m_tableAt[static_cast<std::size_t>(table)] = at2;
            const std::uint64_t bytes = static_cast<std::uint64_t>(offset) * m_rows[static_cast<std::size_t>(table)];
            if (bytes > size - std::min(size, at2))
                return false;
            at2 += static_cast<std::size_t>(bytes);
        }
        if (at2 > tables + tablesSize)
            return false;

        m_valid = true;
        m_nesting.clear();
        for (std::uint32_t row = 1; row <= Rows(NestedClass); ++row)
            m_nesting.emplace_back(Cell(NestedClass, row, 0), Cell(NestedClass, row, 1));
        std::sort(m_nesting.begin(), m_nesting.end());
        return true;
    }

    std::optional<std::vector<std::uint8_t>> ClrMetadata::FromPeFile(std::span<const std::uint8_t> file)
    {
        const std::size_t size = file.size();
        const std::uint8_t* base = file.data();
        if (size < 0x40 || base[0] != 'M' || base[1] != 'Z')
            return std::nullopt;
        const std::uint32_t pe = Read32(base + 0x3C);
        if (pe > size - 24 || Read32(base + pe) != 0x00004550)
            return std::nullopt;
        const std::uint16_t sections = Read16(base + pe + 6);
        const std::uint16_t optionalSize = Read16(base + pe + 20);
        const std::size_t optional = pe + 24;
        if (optional + optionalSize > size || optionalSize < 2)
            return std::nullopt;
        const std::uint16_t magic = Read16(base + optional);
        const std::size_t directories = optional + (magic == 0x20B ? 112 : 96);
        const std::size_t cliEntry = directories + 14 * 8;
        if (cliEntry + 8 > optional + optionalSize)
            return std::nullopt;
        const std::size_t table = optional + optionalSize;
        const auto map = [&](std::uint32_t rva, std::uint32_t length) -> std::optional<std::size_t>
            {
                for (std::uint16_t i = 0; i < sections; ++i)
                {
                    const std::size_t entry = table + static_cast<std::size_t>(i) * 40;
                    if (entry + 40 > size)
                        return std::nullopt;
                    const std::uint32_t virtualSize = Read32(base + entry + 8);
                    const std::uint32_t virtualAddress = Read32(base + entry + 12);
                    const std::uint32_t rawSize = Read32(base + entry + 16);
                    const std::uint32_t rawAt = Read32(base + entry + 20);
                    const std::uint32_t span = std::max(virtualSize, rawSize);
                    if (rva >= virtualAddress && rva - virtualAddress < span)
                    {
                        const std::uint64_t at = static_cast<std::uint64_t>(rawAt) + (rva - virtualAddress);
                        if (at + length > size)
                            return std::nullopt;
                        return static_cast<std::size_t>(at);
                    }
                }
                return std::nullopt;
            };
        const auto cli = map(Read32(base + cliEntry), 72);
        if (!cli)
            return std::nullopt;
        const std::uint32_t metadataRva = Read32(base + *cli + 8);
        const std::uint32_t metadataSize = Read32(base + *cli + 12);
        const auto metadata = map(metadataRva, metadataSize);
        if (!metadata || !metadataSize)
            return std::nullopt;
        return std::vector<std::uint8_t>(base + *metadata, base + *metadata + metadataSize);
    }

    std::uint32_t ClrMetadata::Cell(Table table, std::uint32_t row, int column) const
    {
        if (!m_valid || table < 0 || table >= TableCount || row == 0 || row > m_rows[static_cast<std::size_t>(table)])
            return 0;
        if (column < 0 || column >= kSchemas[table].Count)
            return 0;
        const std::size_t at = m_tableAt[static_cast<std::size_t>(table)] + static_cast<std::size_t>(row - 1) * m_rowSize[static_cast<std::size_t>(table)]
            + m_columnAt[static_cast<std::size_t>(table)][static_cast<std::size_t>(column)];
        return m_columnSize[static_cast<std::size_t>(table)][static_cast<std::size_t>(column)] == 4 ? Read32(m_data.data() + at) : Read16(m_data.data() + at);
    }

    std::string ClrMetadata::String(std::uint32_t index) const
    {
        if (index >= m_stringsSize)
            return {};
        const char* start = reinterpret_cast<const char*>(m_data.data() + m_strings + index);
        const std::size_t room = m_stringsSize - index;
        const void* end = std::memchr(start, 0, room);
        return std::string(start, end ? static_cast<std::size_t>(static_cast<const char*>(end) - start) : room);
    }

    std::span<const std::uint8_t> ClrMetadata::Blob(std::uint32_t index) const
    {
        if (index >= m_blobSize)
            return {};
        const std::span<const std::uint8_t> heap(m_data.data() + m_blob, m_blobSize);
        std::size_t at = index;
        bool ok = true;
        const std::uint32_t length = Compressed(heap, at, ok);
        if (!ok || length > heap.size() - at)
            return {};
        return heap.subspan(at, length);
    }

    std::uint32_t ClrMetadata::ListEnd(Table table, std::uint32_t row, int column, Table target) const
    {
        if (row < Rows(table))
            return Cell(table, row + 1, column);
        return Rows(target) + 1;
    }

    std::optional<ClrMetadata::TypeDefRow> ClrMetadata::TypeDefAt(std::uint32_t row) const
    {
        if (!m_valid || row == 0 || row > Rows(TypeDef))
            return std::nullopt;
        TypeDefRow info;
        info.Flags = Cell(TypeDef, row, 0);
        info.Name = String(Cell(TypeDef, row, 1));
        info.Namespace = String(Cell(TypeDef, row, 2));
        info.Extends = TypeDefOrRefToken(Cell(TypeDef, row, 3));
        if ((info.Extends & 0x00FFFFFF) == 0)
            info.Extends = 0;
        const Table fieldList = Rows(FieldPtr) ? FieldPtr : Field;
        const Table methodList = Rows(MethodPtr) ? MethodPtr : MethodDef;
        info.FieldFirst = Cell(TypeDef, row, 4);
        info.FieldEnd = std::max(info.FieldFirst, ListEnd(TypeDef, row, 4, fieldList));
        info.MethodFirst = Cell(TypeDef, row, 5);
        info.MethodEnd = std::max(info.MethodFirst, ListEnd(TypeDef, row, 5, methodList));
        return info;
    }

    std::optional<ClrMetadata::FieldRow> ClrMetadata::FieldAt(std::uint32_t row) const
    {
        if (!m_valid || row == 0 || row > Rows(Field))
            return std::nullopt;
        return FieldRow{ static_cast<std::uint16_t>(Cell(Field, row, 0)), String(Cell(Field, row, 1)), Cell(Field, row, 2) };
    }

    std::optional<ClrMetadata::MethodRow> ClrMetadata::MethodAt(std::uint32_t row) const
    {
        if (!m_valid || row == 0 || row > Rows(MethodDef))
            return std::nullopt;
        MethodRow info;
        info.Rva = Cell(MethodDef, row, 0);
        info.ImplFlags = static_cast<std::uint16_t>(Cell(MethodDef, row, 1));
        info.Flags = static_cast<std::uint16_t>(Cell(MethodDef, row, 2));
        info.Name = String(Cell(MethodDef, row, 3));
        info.Signature = Cell(MethodDef, row, 4);
        return info;
    }

    std::uint32_t ClrMetadata::EnclosingOf(std::uint32_t row) const
    {
        const auto found = std::lower_bound(m_nesting.begin(), m_nesting.end(), std::make_pair(row, 0u));
        return found != m_nesting.end() && found->first == row ? found->second : 0;
    }

    bool ClrMetadata::HasGenericParams(std::uint32_t row) const
    {
        // GenericParam is sorted by its owner; the coded owner of a TypeDef
        // row is row << 1 (tag 0).
        const std::uint32_t wanted = row << 1;
        std::uint32_t low = 1;
        std::uint32_t high = Rows(GenericParam);
        while (low <= high && high)
        {
            const std::uint32_t middle = low + (high - low) / 2;
            const std::uint32_t owner = Cell(GenericParam, middle, 2);
            if (owner == wanted)
                return true;
            if (owner < wanted)
                low = middle + 1;
            else
                high = middle - 1;
        }
        return false;
    }

    std::string ClrMetadata::TypeDefName(std::uint32_t row) const
    {
        std::string name;
        for (int depth = 0; row && depth < 16; ++depth)
        {
            const std::uint32_t enclosing = EnclosingOf(row);
            const std::string own = String(Cell(TypeDef, row, 1));
            if (enclosing)
                name = name.empty() ? own : own + "+" + name;
            else
            {
                const std::string space = String(Cell(TypeDef, row, 2));
                const std::string full = space.empty() ? own : space + "." + own;
                name = name.empty() ? full : full + "+" + name;
            }
            row = enclosing;
        }
        return name;
    }

    std::string ClrMetadata::TypeName(std::uint32_t token) const
    {
        const std::uint32_t table = token >> 24;
        const std::uint32_t row = token & 0x00FFFFFF;
        if (table == TypeDef)
            return TypeDefName(row);
        if (table == TypeRef)
        {
            if (row == 0 || row > Rows(TypeRef))
                return {};
            const std::string own = String(Cell(TypeRef, row, 1));
            const std::uint32_t scope = Cell(TypeRef, row, 0);
            if ((scope & 3) == 3 && (scope >> 2) != row)  // nested in another TypeRef
            {
                const std::string outer = TypeName(Token(TypeRef, scope >> 2));
                if (!outer.empty())
                    return outer + "+" + own;
            }
            const std::string space = String(Cell(TypeRef, row, 2));
            return space.empty() ? own : space + "." + own;
        }
        if (table == TypeSpec)
        {
            if (row == 0 || row > Rows(TypeSpec))
                return {};
            Cursor cursor{ Blob(Cell(TypeSpec, row, 0)) };
            return SignatureType(cursor, 0);
        }
        return {};
    }

    std::uint32_t ClrMetadata::FieldOwner(std::uint32_t row) const
    {
        std::uint32_t low = 1;
        std::uint32_t high = Rows(TypeDef);
        std::uint32_t found = 0;
        while (low <= high && high)
        {
            const std::uint32_t middle = low + (high - low) / 2;
            if (Cell(TypeDef, middle, 4) <= row)
            {
                found = middle;
                low = middle + 1;
            }
            else
                high = middle - 1;
        }
        // Several types may start at the same row when the ones before it
        // have no fields; the owner is the last of them.
        if (found)
        {
            const auto info = TypeDefAt(found);
            if (!info || row < info->FieldFirst || row >= info->FieldEnd)
                return 0;
        }
        return found;
    }

    std::uint32_t ClrMetadata::MethodOwner(std::uint32_t row) const
    {
        std::uint32_t low = 1;
        std::uint32_t high = Rows(TypeDef);
        std::uint32_t found = 0;
        while (low <= high && high)
        {
            const std::uint32_t middle = low + (high - low) / 2;
            if (Cell(TypeDef, middle, 5) <= row)
            {
                found = middle;
                low = middle + 1;
            }
            else
                high = middle - 1;
        }
        if (found)
        {
            const auto info = TypeDefAt(found);
            if (!info || row < info->MethodFirst || row >= info->MethodEnd)
                return 0;
        }
        return found;
    }

    std::string ClrMetadata::SignatureType(Cursor& cursor, int depth) const
    {
        if (depth > 16 || !cursor.Ok || cursor.At >= cursor.Bytes.size())
        {
            cursor.Ok = false;
            return {};
        }
        const std::uint8_t element = cursor.Bytes[cursor.At++];
        const auto typeToken = [&]() { return TypeDefOrRefToken(Compressed(cursor.Bytes, cursor.At, cursor.Ok)); };
        switch (element)
        {
            case 0x01: return "void";
            case 0x02: return "bool";
            case 0x03: return "char";
            case 0x04: return "sbyte";
            case 0x05: return "byte";
            case 0x06: return "short";
            case 0x07: return "ushort";
            case 0x08: return "int";
            case 0x09: return "uint";
            case 0x0A: return "long";
            case 0x0B: return "ulong";
            case 0x0C: return "float";
            case 0x0D: return "double";
            case 0x0E: return "string";
            case 0x0F: return SignatureType(cursor, depth + 1) + "*";
            case 0x10: return SignatureType(cursor, depth + 1) + "&";
            case 0x11:
            case 0x12: return TypeName(typeToken());
            case 0x13: return "T" + std::to_string(Compressed(cursor.Bytes, cursor.At, cursor.Ok));
            case 0x1E: return "M" + std::to_string(Compressed(cursor.Bytes, cursor.At, cursor.Ok));
            case 0x14:
            {
                std::string inner = SignatureType(cursor, depth + 1);
                const std::uint32_t rank = Compressed(cursor.Bytes, cursor.At, cursor.Ok);
                const std::uint32_t sizes = Compressed(cursor.Bytes, cursor.At, cursor.Ok);
                for (std::uint32_t i = 0; i < sizes && cursor.Ok; ++i)
                    Compressed(cursor.Bytes, cursor.At, cursor.Ok);
                const std::uint32_t bounds = Compressed(cursor.Bytes, cursor.At, cursor.Ok);
                for (std::uint32_t i = 0; i < bounds && cursor.Ok; ++i)
                    Compressed(cursor.Bytes, cursor.At, cursor.Ok);
                return inner + "[" + std::string(rank > 1 && rank < 32 ? rank - 1 : 0, ',') + "]";
            }
            case 0x15:
            {
                if (cursor.At >= cursor.Bytes.size())
                {
                    cursor.Ok = false;
                    return {};
                }
                ++cursor.At;  // CLASS or VALUETYPE
                std::string name = WithoutArity(TypeName(typeToken()));
                const std::uint32_t count = Compressed(cursor.Bytes, cursor.At, cursor.Ok);
                name += "<";
                for (std::uint32_t i = 0; i < count && i < 64 && cursor.Ok; ++i)
                    name += (i ? ", " : "") + SignatureType(cursor, depth + 1);
                return name + ">";
            }
            case 0x16: return "TypedReference";
            case 0x18: return "IntPtr";
            case 0x19: return "UIntPtr";
            case 0x1B: cursor.Ok = false; return "fnptr";
            case 0x1C: return "object";
            case 0x1D: return SignatureType(cursor, depth + 1) + "[]";
            case 0x1F:
            case 0x20:
                typeToken();
                return SignatureType(cursor, depth + 1);
            case 0x45: return SignatureType(cursor, depth + 1);
            default:
                cursor.Ok = false;
                return {};
        }
    }

    std::string ClrMetadata::FieldTypeName(std::uint32_t row) const
    {
        const auto field = FieldAt(row);
        if (!field)
            return {};
        Cursor cursor{ Blob(field->Signature) };
        if (cursor.Bytes.empty() || (cursor.Bytes[0] & 0x0F) != 0x06)
            return {};
        cursor.At = 1;
        std::string name = SignatureType(cursor, 0);
        return cursor.Ok ? name : std::string();
    }

    int ClrMetadata::ParamCount(std::uint32_t row) const
    {
        const auto method = MethodAt(row);
        if (!method)
            return -1;
        const std::span<const std::uint8_t> signature = Blob(method->Signature);
        if (signature.empty())
            return -1;
        std::size_t at = 1;
        bool ok = true;
        if (signature[0] & 0x10)  // generic: the type parameter count comes first
            Compressed(signature, at, ok);
        const std::uint32_t count = Compressed(signature, at, ok);
        return ok ? static_cast<int>(count) : -1;
    }
}
