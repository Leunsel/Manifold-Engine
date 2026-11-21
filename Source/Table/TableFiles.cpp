#include "Table/TableFiles.h"

#include "Core/Text.h"
#include "Table/CheatTable.h"
#include "Text/Json.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

namespace Manifold::Table
{
    namespace
    {
        constexpr std::size_t kSniffBytes = 64 * 1024;
        constexpr std::size_t kLongestQuoted = 260;

        bool StartsWith(std::span<const std::uint8_t> data, std::string_view magic, std::size_t at = 0)
        {
            return data.size() >= at + magic.size() && std::memcmp(data.data() + at, magic.data(), magic.size()) == 0;
        }

        bool OneOf(const std::string& extension, std::initializer_list<const char*> list)
        {
            for (const char* entry : list)
            {
                if (extension == entry)
                    return true;
            }
            return false;
        }

        std::optional<FileKind> KindByMagic(std::span<const std::uint8_t> data, const std::string& extension)
        {
            using namespace std::string_view_literals;
            if (StartsWith(data, "\x89PNG"sv) || StartsWith(data, "\xFF\xD8\xFF"sv) || StartsWith(data, "GIF8"sv) || (StartsWith(data, "RIFF"sv) && StartsWith(data, "WEBP"sv, 8)))
                return FileKind::Image;
            if (StartsWith(data, "BM"sv) && data.size() >= 26 && OneOf(extension, { "bmp", "dib", "" }))
                return FileKind::Image;
            if (StartsWith(data, "\0\0\1\0"sv) && OneOf(extension, { "ico", "" }))
                return FileKind::Image;
            if ((StartsWith(data, "RIFF"sv) && StartsWith(data, "WAVE"sv, 8)) || StartsWith(data, "OggS"sv) || StartsWith(data, "ID3"sv) || StartsWith(data, "fLaC"sv))
                return FileKind::Audio;
            if (StartsWith(data, "OTTO"sv) || StartsWith(data, "wOFF"sv) || StartsWith(data, "wOF2"sv) || (StartsWith(data, "\0\1\0\0"sv) && OneOf(extension, { "ttf", "" })))
                return FileKind::Font;
            if (StartsWith(data, "PK\3\4"sv) || StartsWith(data, "7z\xBC\xAF"sv) || StartsWith(data, "Rar!"sv) || StartsWith(data, "\x1F\x8B"sv))
                return FileKind::Archive;
            if (StartsWith(data, "MZ"sv) && data.size() >= 64)
                return FileKind::Program;
            return std::nullopt;
        }

        FileKind KindByExtension(const std::string& extension)
        {
            if (OneOf(extension, { "png", "jpg", "jpeg", "bmp", "gif", "ico", "tga", "webp" }))
                return FileKind::Image;
            if (OneOf(extension, { "wav", "mp3", "ogg", "flac", "xm", "mod" }))
                return FileKind::Audio;
            if (OneOf(extension, { "ttf", "otf", "woff", "woff2" }))
                return FileKind::Font;
            if (OneOf(extension, { "zip", "7z", "rar", "gz" }))
                return FileKind::Archive;
            if (OneOf(extension, { "exe", "dll", "sys" }))
                return FileKind::Program;
            return FileKind::Binary;
        }

        FileKind TextKind(const std::string& extension, std::string_view text)
        {
            if (extension == "lua")
                return FileKind::Lua;
            if (extension == "json")
                return FileKind::Json;
            if (OneOf(extension, { "xml", "ct", "cetrainer", "html", "htm", "svg", "frm" }))
                return FileKind::Xml;
            if (OneOf(extension, { "cea", "asm", "aa" }))
                return FileKind::AutoAssembler;
            if (!extension.empty())
                return FileKind::Text;
            const std::string_view start = TrimView(text.substr(0, 256));
            if (start.starts_with("<?xml") || start.starts_with("<CheatTable"))
                return FileKind::Xml;
            if (Text::LooksLikeJson(text))
                return FileKind::Json;
            return FileKind::Text;
        }

        int CompareNoCase(char a, char b)
        {
            return std::tolower(static_cast<unsigned char>(a)) - std::tolower(static_cast<unsigned char>(b));
        }

        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        int NaturalCompare(std::string_view a, std::string_view b)
        {
            std::size_t i = 0;
            std::size_t k = 0;
            while (i < a.size() && k < b.size())
            {
                if (IsDigit(a[i]) && IsDigit(b[k]))
                {
                    std::size_t endA = i;
                    std::size_t endB = k;
                    while (endA < a.size() && IsDigit(a[endA]))
                        ++endA;
                    while (endB < b.size() && IsDigit(b[endB]))
                        ++endB;
                    std::string_view numberA = a.substr(i, endA - i);
                    std::string_view numberB = b.substr(k, endB - k);
                    while (numberA.size() > 1 && numberA.front() == '0')
                        numberA.remove_prefix(1);
                    while (numberB.size() > 1 && numberB.front() == '0')
                        numberB.remove_prefix(1);
                    if (numberA.size() != numberB.size())
                        return numberA.size() < numberB.size() ? -1 : 1;
                    if (const int order = numberA.compare(numberB))
                        return order;
                    i = endA;
                    k = endB;
                    continue;
                }
                if (const int order = CompareNoCase(a[i], b[k]))
                    return order;
                ++i;
                ++k;
            }
            if (i < a.size())
                return 1;
            if (k < b.size())
                return -1;
            return 0;
        }

        void ScanText(std::string_view text, const std::set<std::string>& names, const FileUse& place, std::string_view self, FileUses& uses)
        {
            for (const auto& [name, line] : QuotedNames(text))
            {
                const std::string key = FileUseKey(name);
                if (!names.count(key) || key == self)
                    continue;
                std::vector<FileUse>& list = uses[key];
                if (!list.empty() && list.back().Where == place.Where && list.back().RecordId == place.RecordId && list.back().Label == place.Label)
                    continue;
                FileUse use = place;
                use.Line = line;
                list.push_back(std::move(use));
            }
        }

        bool ReservedDeviceName(std::string_view name)
        {
            std::string_view base = name.substr(0, name.find('.'));
            while (!base.empty() && base.back() == ' ')
                base.remove_suffix(1);
            static constexpr const char* kDevices[] = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };
            for (const char* device : kDevices)
            {
                if (EqualsNoCase(base, device))
                    return true;
            }
            if (base.size() < 4 || (!EqualsNoCase(base.substr(0, 3), "COM") && !EqualsNoCase(base.substr(0, 3), "LPT")))
                return false;
            const std::string_view number = base.substr(3);
            using namespace std::string_view_literals;
            // ∑ Windows also maps the superscripts 1, 2 and 3 (COM¹, LPT²) to devices.
            return (number.size() == 1 && IsDigit(number[0])) || number == "\xC2\xB9"sv || number == "\xC2\xB2"sv || number == "\xC2\xB3"sv;
        }

        std::string LuaQuoted(std::string_view name)
        {
            std::string text = "'";
            for (const char c : name)
            {
                if (c == '\'' || c == '\\')
                    text += '\\';
                text += c;
            }
            return text + "'";
        }
    }

    const char* FileKindName(FileKind kind)
    {
        switch (kind)
        {
        case FileKind::Lua: return "Lua";
        case FileKind::Json: return "JSON";
        case FileKind::Xml: return "XML";
        case FileKind::AutoAssembler: return "Auto Assembler";
        case FileKind::Text: return "Text";
        case FileKind::Image: return "Image";
        case FileKind::Audio: return "Audio";
        case FileKind::Font: return "Font";
        case FileKind::Archive: return "Archive";
        case FileKind::Program: return "Program";
        case FileKind::Binary: break;
        }
        return "Binary";
    }

    bool IsTextKind(FileKind kind)
    {
        return kind == FileKind::Lua || kind == FileKind::Json || kind == FileKind::Xml || kind == FileKind::AutoAssembler || kind == FileKind::Text;
    }

    bool LooksLikeText(std::span<const std::uint8_t> data)
    {
        const bool cut = data.size() > kSniffBytes;
        if (cut)
            data = data.first(kSniffBytes);
        std::size_t printable = 0;
        std::size_t i = 0;
        while (i < data.size())
        {
            const std::uint8_t c = data[i];
            if (c == 0)
                return false;
            if (c < 0x80)
            {
                if (c >= 0x20 || c == '\n' || c == '\r' || c == '\t')
                    ++printable;
                ++i;
                continue;
            }
            const int follow = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
            if (follow < 0)
                return false;
            if (i + static_cast<std::size_t>(follow) >= data.size())
            {
                if (cut)
                    break;
                return false;
            }
            for (int k = 1; k <= follow; ++k)
            {
                if ((data[i + static_cast<std::size_t>(k)] & 0xC0) != 0x80)
                    return false;
            }
            printable += static_cast<std::size_t>(follow) + 1;
            i += static_cast<std::size_t>(follow) + 1;
        }
        return printable * 10 >= i * 9;
    }

    std::string FileExtension(std::string_view name)
    {
        const std::size_t dot = name.rfind('.');
        if (dot == std::string_view::npos || dot + 1 >= name.size())
            return {};
        return Lower(name.substr(dot + 1));
    }

    FileKind DetectFileKind(std::string_view name, std::span<const std::uint8_t> data)
    {
        const std::string extension = FileExtension(name);
        if (const auto magic = KindByMagic(data, extension))
            return *magic;
        if (LooksLikeText(data))
        {
            if (data.empty() && KindByExtension(extension) != FileKind::Binary)
                return KindByExtension(extension);
            const std::size_t shown = std::min(data.size(), kSniffBytes);
            return TextKind(extension, std::string_view(reinterpret_cast<const char*>(data.data()), shown));
        }
        return KindByExtension(extension);
    }

    bool NaturalLess(std::string_view a, std::string_view b)
    {
        return NaturalCompare(a, b) < 0;
    }

    bool FileMatches(const FileRow& row, std::string_view filter)
    {
        for (const std::string& word : Split(filter, ' '))
        {
            if (!ContainsNoCase(row.Name, word) && !ContainsNoCase(FileKindName(row.Kind), word))
                return false;
        }
        return true;
    }

    std::vector<FileRow> ArrangeFiles(std::vector<FileRow> rows, std::string_view filter, FileColumn sort, bool descending)
    {
        const std::string wanted = Trim(filter);
        if (!wanted.empty())
            rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const FileRow& row) { return !FileMatches(row, wanted); }), rows.end());
        const auto order = [sort](const FileRow& a, const FileRow& b) -> int
        {
            switch (sort)
            {
            case FileColumn::Kind:
                return std::strcmp(FileKindName(a.Kind), FileKindName(b.Kind));
            case FileColumn::Size:
                return a.Size < b.Size ? -1 : a.Size > b.Size ? 1 : 0;
            case FileColumn::Stored:
                return a.Stored < b.Stored ? -1 : a.Stored > b.Stored ? 1 : 0;
            case FileColumn::Uses:
                return a.Uses - b.Uses;
            case FileColumn::Name:
            case FileColumn::Order:
                break;
            }
            return 0;
        };
        if (sort == FileColumn::Order)
        {
            if (descending)
                std::reverse(rows.begin(), rows.end());
            return rows;
        }
        std::stable_sort(rows.begin(), rows.end(), [&](const FileRow& a, const FileRow& b)
            {
                const int first = order(a, b);
                if (first != 0)
                    return descending ? first > 0 : first < 0;
                const int byName = NaturalCompare(a.Name, b.Name);
                return descending && sort == FileColumn::Name ? byName > 0 : byName < 0;
            });
        return rows;
    }

    std::string FileNameProblem(const AddressList& list, std::string_view wanted, std::string_view current)
    {
        const std::string name = Trim(wanted);
        if (name.empty())
            return "A table file needs a name.";
        if (name.size() > kMaxFileNameBytes)
            return "The name is too long.";
        for (const char c : name)
        {
            if (static_cast<unsigned char>(c) < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                return "A name cannot contain \\ / : * ? \" < > | or control characters.";
        }
        if (EqualsNoCase(name, current))
            return {};
        if (!SafeExportName(name))
            return "Windows does not allow this as a file name (CON, NUL, COM1 and the like, \".\", \"..\", or a name that ends with a dot).";
        for (const TableFile& file : list.Files)
        {
            if (EqualsNoCase(file.Name, name))
                return "A table file with this name exists already.";
        }
        return {};
    }

    bool SafeExportName(std::string_view name)
    {
        if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ')
            return false;
        for (const char c : name)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                return false;
        }
        return !ReservedDeviceName(name);
    }

    std::string UniqueFileName(const AddressList& list, std::string_view wanted)
    {
        return UniqueFileName(std::span<const TableFile>(list.Files), wanted);
    }

    std::string UniqueFileName(std::span<const TableFile> files, std::string_view wanted)
    {
        const auto taken = [&](std::string_view name)
        {
            return std::any_of(files.begin(), files.end(), [&](const TableFile& file) { return EqualsNoCase(file.Name, name); });
        };
        if (!taken(wanted))
            return std::string(wanted);
        const std::size_t dot = wanted.rfind('.');
        const bool split = dot != std::string_view::npos && dot > 0;
        const std::string stem(split ? wanted.substr(0, dot) : wanted);
        const std::string tail(split ? wanted.substr(dot) : std::string_view());
        for (int i = 2;; ++i)
        {
            const std::string candidate = stem + "_" + std::to_string(i) + tail;
            if (!taken(candidate))
                return candidate;
        }
    }

    bool RenameFile(AddressList& list, std::string_view from, std::string_view to)
    {
        TableFile* file = list.FindFile(from);
        const std::string name = Trim(to);
        if (!file || !FileNameProblem(list, name, file->Name).empty())
            return false;
        if (file->Name == name)
            return true;
        file->Name = name;
        list.Touch();
        return true;
    }

    TableFile* DuplicateFile(AddressList& list, std::string_view name)
    {
        const TableFile* source = list.FindFile(name);
        if (!source)
            return nullptr;
        const std::vector<std::uint8_t> bytes = *source->Data;
        const std::size_t at = static_cast<std::size_t>(source - list.Files.data());
        TableFile copy;
        copy.Name = UniqueFileName(list, source->Name);
        *copy.Data = bytes;
        const auto placed = list.Files.insert(list.Files.begin() + static_cast<std::ptrdiff_t>(at + 1), std::move(copy));
        list.Touch();
        return &*placed;
    }

    void SetFileData(AddressList& list, TableFile& file, std::vector<std::uint8_t> data)
    {
        *file.Data = std::move(data);
        ++*file.Version;
        list.Touch();
    }

    std::size_t RemoveFiles(AddressList& list, const std::vector<std::string>& names)
    {
        const std::size_t before = list.Files.size();
        list.Files.erase(std::remove_if(list.Files.begin(), list.Files.end(), [&](const TableFile& file)
                             {
                                 return std::any_of(names.begin(), names.end(), [&](const std::string& name) { return EqualsNoCase(file.Name, name); });
                             }),
            list.Files.end());
        if (list.Files.size() != before)
            list.Touch();
        return before - list.Files.size();
    }

    std::size_t StoredFileSize(std::span<const std::uint8_t> data)
    {
        return EncodeTableFile(data).size();
    }

    std::vector<std::pair<std::string, int>> QuotedNames(std::string_view text)
    {
        std::vector<std::pair<std::string, int>> found;
        int line = 1;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == '\n')
            {
                ++line;
                continue;
            }
            if (c != '\'' && c != '"')
                continue;
            const std::size_t limit = std::min(text.size(), i + 1 + kLongestQuoted);
            for (std::size_t end = i + 1; end < limit; ++end)
            {
                if (text[end] == '\n')
                    break;
                if (text[end] == c)
                {
                    if (end > i + 1)
                        found.emplace_back(std::string(text.substr(i + 1, end - i - 1)), line);
                    break;
                }
            }
        }
        return found;
    }

    std::string FileUseKey(std::string_view name)
    {
        return Lower(name);
    }

    FileUses FindFileUses(AddressList& list, std::size_t textLimit)
    {
        FileUses uses;
        if (list.Files.empty())
            return uses;
        std::set<std::string> names;
        for (const TableFile& file : list.Files)
            names.insert(FileUseKey(file.Name));
        FileUse place;
        place.Where = FileUse::Place::TableScript;
        place.Label = "Table Lua Script";
        ScanText(list.TableLuaScript, names, place, {}, uses);
        list.ForEach([&](MemoryRecord& record)
            {
                if (record.Script.empty())
                    return;
                FileUse use;
                use.Where = FileUse::Place::Record;
                use.RecordId = record.Id;
                use.Label = record.Description.empty() ? "Record " + std::to_string(record.Id) : record.Description;
                ScanText(record.Script, names, use, {}, uses);
            });
        std::size_t room = textLimit;
        for (const TableFile& file : list.Files)
        {
            if (file.Data->empty() || file.Data->size() > room || !IsTextKind(DetectFileKind(file.Name, *file.Data)))
                continue;
            room -= file.Data->size();
            FileUse use;
            use.Where = FileUse::Place::File;
            use.Label = file.Name;
            ScanText(std::string_view(reinterpret_cast<const char*>(file.Data->data()), file.Data->size()), names, use, FileUseKey(file.Name), uses);
        }
        return uses;
    }

    std::string LuaFindSnippet(std::string_view name)
    {
        return "findTableFile(" + LuaQuoted(name) + ")";
    }

    std::string LuaReadSnippet(std::string_view name)
    {
        return "local stream = findTableFile(" + LuaQuoted(name) + ").Stream\nlocal text = readStringLocal(stream.Memory, stream.Size)";
    }

    bool IsXmlName(std::string_view name)
    {
        if (name.empty())
            return false;
        const auto start = [](unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; };
        if (!start(static_cast<unsigned char>(name.front())))
            return false;
        return std::all_of(name.begin(), name.end(), [&](char c)
            {
                const unsigned char u = static_cast<unsigned char>(c);
                return start(u) || std::isdigit(u) || c == '.' || c == '-';
            });
    }
}
