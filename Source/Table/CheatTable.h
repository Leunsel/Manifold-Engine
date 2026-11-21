#pragma once

#include "Core/Errors.h"
#include "Table/AddressList.h"
#include "Table/Structures.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Manifold::Table
{
    struct TableSymbol
    {
        std::string Name;
        std::string Address;
    };

    struct LoadedTable
    {
        std::vector<std::unique_ptr<MemoryRecord>> Records;
        std::vector<TableSymbol> Symbols;
        std::string LuaScript;
        std::string Comments;
        std::map<std::string, AddressList::DisasmNote> DisasmNotes;
        std::vector<std::unique_ptr<StructDefinition>> Structures;
        std::vector<TableFile> Files;
        std::vector<std::string> ExtraXml;
        std::vector<std::string> Warnings;
        int Version = 0;
        FileNaming Naming = FileNaming::Tag;
    };

    inline constexpr int kTableVersion = 45;
    inline constexpr std::size_t kMaxTableFileBytes = 256u * 1024u * 1024u;
    inline constexpr std::size_t kMaxTableFilesBytes = 512u * 1024u * 1024u;

    Status LoadTable(const std::filesystem::path& path, LoadedTable& table);
    Status ParseTable(std::string_view text, LoadedTable& table);
    Status SaveTable(const std::filesystem::path& path, const AddressList& list, const StructureStore& structures, const std::vector<TableSymbol>& symbols,
        FileNaming naming = FileNaming::AsLoaded);
    std::string WriteTable(const AddressList& list, const StructureStore& structures, const std::vector<TableSymbol>& symbols,
        FileNaming naming = FileNaming::AsLoaded);

    std::string RecordsToXml(const std::vector<const MemoryRecord*>& records);
    std::optional<std::vector<std::uint8_t>> DecodeTableFile(std::string_view text, std::size_t limit = kMaxTableFileBytes);
    std::string EncodeTableFile(std::span<const std::uint8_t> data);
    std::vector<std::unique_ptr<MemoryRecord>> RecordsFromXml(std::string_view text);
}
