#pragma once

#include "Table/AddressList.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Table
{
    enum class FileKind
    {
        Lua,
        Json,
        Xml,
        AutoAssembler,
        Text,
        Image,
        Audio,
        Font,
        Archive,
        Program,
        Binary
    };

    enum class FileColumn
    {
        Order,
        Name,
        Kind,
        Size,
        Stored,
        Uses
    };

    struct FileRow
    {
        std::string Name;
        FileKind Kind = FileKind::Binary;
        std::size_t Size = 0;
        std::int64_t Stored = -1;
        int Uses = 0;
    };

    struct FileUse
    {
        enum class Place
        {
            TableScript,
            Record,
            File
        };

        Place Where = Place::TableScript;
        int RecordId = 0;
        std::string Label;
        int Line = 0;
    };

    using FileUses = std::map<std::string, std::vector<FileUse>>;

    inline constexpr std::size_t kMaxFileNameBytes = 200;

    const char* FileKindName(FileKind kind);
    bool IsTextKind(FileKind kind);
    bool LooksLikeText(std::span<const std::uint8_t> data);
    std::string FileExtension(std::string_view name);
    FileKind DetectFileKind(std::string_view name, std::span<const std::uint8_t> data);

    bool NaturalLess(std::string_view a, std::string_view b);
    bool FileMatches(const FileRow& row, std::string_view filter);
    std::vector<FileRow> ArrangeFiles(std::vector<FileRow> rows, std::string_view filter, FileColumn sort, bool descending);

    std::string FileNameProblem(const AddressList& list, std::string_view wanted, std::string_view current = {});
    std::string UniqueFileName(const AddressList& list, std::string_view wanted);
    bool RenameFile(AddressList& list, std::string_view from, std::string_view to);
    TableFile* DuplicateFile(AddressList& list, std::string_view name);
    void SetFileData(AddressList& list, TableFile& file, std::vector<std::uint8_t> data);
    std::size_t RemoveFiles(AddressList& list, const std::vector<std::string>& names);
    std::size_t StoredFileSize(std::span<const std::uint8_t> data);

    std::vector<std::pair<std::string, int>> QuotedNames(std::string_view text);
    FileUses FindFileUses(AddressList& list, std::size_t textLimit = 8u * 1024u * 1024u);
    std::string FileUseKey(std::string_view name);

    std::string LuaFindSnippet(std::string_view name);
    std::string LuaReadSnippet(std::string_view name);

    bool IsXmlName(std::string_view name);
}
