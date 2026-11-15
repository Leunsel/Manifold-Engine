#include "Templates/TemplateFiles.h"

#include "Core/Text.h"
#include "Templates/TemplateEngine.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>

namespace Manifold::Templates
{
    namespace
    {
        std::string PathText(const std::filesystem::path& path)
        {
            return WideToUtf8(path.wstring());
        }

        std::string Number(double value)
        {
            if (value == std::floor(value) && std::abs(value) < 9.0e15)
                return std::to_string(static_cast<long long>(value));
            std::ostringstream stream;
            stream.precision(17);
            stream << value;
            return stream.str();
        }

        std::string ValueLiteral(const Value& value)
        {
            if (const auto* flag = std::get_if<bool>(&value))
                return *flag ? "true" : "false";
            if (const auto* integer = std::get_if<std::int64_t>(&value))
                return std::to_string(*integer);
            if (const auto* number = std::get_if<double>(&value))
                return Number(*number);
            if (const auto* text = std::get_if<std::string>(&value))
                return QuoteLua(*text);
            return "nil";
        }

        std::string List(const std::vector<std::string>& items)
        {
            if (items.empty())
                return "{}";
            std::string text = "{ ";
            for (std::size_t index = 0; index < items.size(); ++index)
                text += (index ? ", " : "") + QuoteLua(items[index]);
            return text + " }";
        }

        bool WriteFile(const std::filesystem::path& path, const std::string& text)
        {
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            const std::filesystem::path temporary = path.wstring() + L".tmp";
            {
                std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                if (!file)
                    return false;
                file.write(text.data(), static_cast<std::streamsize>(text.size()));
                if (!file)
                    return false;
            }
            std::filesystem::rename(temporary, path, error);
            if (error)
            {
                std::filesystem::remove(temporary, error);
                return false;
            }
            return true;
        }

        std::filesystem::path UniqueStem(const std::filesystem::path& folder, const std::string& stem)
        {
            std::error_code error;
            for (int number = 1; number < 1000; ++number)
            {
                const std::string candidate = number == 1 ? stem : stem + " " + std::to_string(number);
                const std::filesystem::path script = folder / Utf8ToWide(candidate + ".CEA");
                const std::filesystem::path settings = folder / Utf8ToWide(candidate + ".Settings.lua");
                if (!std::filesystem::exists(script, error) && !std::filesystem::exists(settings, error))
                    return folder / Utf8ToWide(candidate);
            }
            return folder / Utf8ToWide(stem + " copy");
        }

        Status Failed(const std::string& message)
        {
            return Status::Fail(ErrorCode::TemplateWrite, message);
        }

        bool MoveFile(const std::filesystem::path& from, const std::filesystem::path& to)
        {
            std::error_code error;
            std::filesystem::create_directories(to.parent_path(), error);
            std::filesystem::rename(from, to, error);
            return !error;
        }
    }

    SettingsForm FormOf(const TemplateInfo& info)
    {
        SettingsForm form;
        form.SchemaVersion = info.SchemaVersion;
        form.Id = info.Id;
        form.Caption = info.Caption;
        form.Description = info.Description;
        form.Author = info.Author;
        form.Version = info.Version;
        form.Shortcut = info.Shortcut;
        form.Category = info.Category;
        form.ExplicitCategory = info.ExplicitCategory;
        form.InSubMenu = info.InSubMenu;
        form.CategoryOrder = info.CategoryOrder;
        form.Order = info.Order;
        form.Tags = info.Tags;
        form.Requires = info.Requires;
        form.Optional = info.Optional;
        form.Capabilities = info.Capabilities;
        form.Architectures = info.Architectures;
        form.AllowUnsafeGlobals = info.SchemaVersion >= 2 && info.AllowUnsafeGlobals;
        form.Inputs = info.Inputs;
        form.Memory = info.Memory;
        form.CustomFields = info.CustomFields;
        return form;
    }

    std::string WriteSettings(const SettingsForm& form)
    {
        std::string text = "return {\n";
        auto line = [&](const std::string& key, const std::string& value) { text += "    " + key + " = " + value + ",\n"; };
        if (form.SchemaVersion >= 2)
            line("SchemaVersion", "2");
        if (!form.Id.empty() && (form.SchemaVersion >= 2 || form.Id.rfind("legacy.", 0) != 0))
            line("Id", QuoteLua(form.Id));
        line("Caption", QuoteLua(form.Caption));
        if (!form.Description.empty())
            line("Description", QuoteLua(form.Description));
        if (!form.Author.empty())
            line("Author", QuoteLua(form.Author));
        if (!form.Version.empty())
            line("Version", QuoteLua(form.Version));
        if (!form.Shortcut.empty())
            line("Shortcut", QuoteLua(form.Shortcut));
        if (form.ExplicitCategory && !form.Category.empty())
            line("Category", QuoteLua(form.Category));
        if (form.CategoryOrder)
            line("CategoryOrder", Number(*form.CategoryOrder));
        if (!form.InSubMenu)
            line("InSubMenu", "false");
        if (form.Order)
            line("Order", Number(*form.Order));
        if (!form.Tags.empty())
            line("Tags", List(form.Tags));
        if (!form.Requires.empty())
            line("Requires", List(form.Requires));
        if (!form.Optional.empty())
            line("Optional", List(form.Optional));
        if (!form.Capabilities.empty())
            line("Capabilities", List(form.Capabilities));
        if (!form.Architectures.empty())
            line("Architectures", List(form.Architectures));
        if (form.SchemaVersion >= 2 && form.AllowUnsafeGlobals)
            line("AllowUnsafeGlobals", "true");
        if (!form.Inputs.empty())
        {
            text += "    Inputs = {\n";
            for (const InputDefinition& input : form.Inputs)
            {
                std::string entry = "{ Name = " + QuoteLua(input.Name) + ", Type = " + QuoteLua(input.Type);
                if (!input.Caption.empty() && input.Caption != input.Name)
                    entry += ", Caption = " + QuoteLua(input.Caption);
                if (!std::holds_alternative<std::monostate>(input.Default))
                    entry += ", Default = " + ValueLiteral(input.Default);
                if (input.Type == "enum")
                    entry += ", Values = " + List(input.Values);
                if (input.Min)
                    entry += ", Min = " + Number(*input.Min);
                if (input.Max)
                    entry += ", Max = " + Number(*input.Max);
                text += "        " + entry + " },\n";
            }
            text += "    },\n";
        }
        std::vector<std::string> memory;
        auto addFlag = [&](const char* key, const std::optional<bool>& value)
        {
            if (value)
                memory.push_back(std::string(key) + " = " + (*value ? "true" : "false"));
        };
        auto addText = [&](const char* key, const std::optional<std::string>& value)
        {
            if (value)
                memory.push_back(std::string(key) + " = " + QuoteLua(*value));
        };
        addFlag("AskForInjectionAddress", form.Memory.AskForInjectionAddress);
        addFlag("AskForHookName", form.Memory.AskForHookName);
        addText("DefaultHookName", form.Memory.DefaultHookName);
        addText("AppendToHookName", form.Memory.AppendToHookName);
        addText("AllocationSize", form.Memory.AllocationSize);
        addFlag("AllocationNear", form.Memory.AllocationNear);
        if (!memory.empty())
        {
            text += "    Memory = {\n";
            for (const std::string& entry : memory)
                text += "        " + entry + ",\n";
            text += "    },\n";
        }
        for (const auto& [key, value] : form.CustomFields)
        {
            static const std::regex identifier("^[A-Za-z_][A-Za-z0-9_]*$");
            line(std::regex_match(key, identifier) ? key : "[" + QuoteLua(key) + "]", value);
        }
        text += "}\n";
        return text;
    }

    std::string FileStem(std::string_view caption)
    {
        std::string stem;
        for (const char c : caption)
        {
            const unsigned char byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos)
                stem += '-';
            else
                stem += c;
        }
        while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.'))
            stem.pop_back();
        while (!stem.empty() && stem.front() == ' ')
            stem.erase(stem.begin());
        return stem.empty() ? "Template" : stem;
    }

    std::string UniqueId(const Library& library, std::string_view caption)
    {
        std::string slug;
        bool dash = false;
        for (const char c : caption)
        {
            const unsigned char byte = static_cast<unsigned char>(c);
            if (std::isalnum(byte))
            {
                if (dash && !slug.empty())
                    slug += '-';
                slug += static_cast<char>(std::tolower(byte));
                dash = false;
            }
            else
                dash = true;
        }
        if (slug.empty())
            slug = "template";
        const std::string base = "user." + slug;
        std::string id = base;
        for (int number = 2; library.Find(id); ++number)
            id = base + "-" + std::to_string(number);
        return id;
    }

    std::filesystem::path CategoryFolder(const std::filesystem::path& root, std::string_view category)
    {
        std::filesystem::path folder = root;
        if (category.empty() || category == "Templates")
            return folder;
        std::size_t start = 0;
        while (start <= category.size())
        {
            const std::size_t end = category.find('>', start);
            const std::string segment = Trim(category.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
            if (!segment.empty())
                folder /= Utf8ToWide(FileStem(segment));
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
        return folder;
    }

    std::filesystem::path SettingsPathOf(const TemplateInfo& info)
    {
        if (!info.SettingsPath.empty())
            return info.SettingsPath;
        return info.ScriptPath.parent_path() / Utf8ToWide(info.Name + ".Settings.lua");
    }

    Status CreateTemplate(const std::filesystem::path& root, const SettingsForm& form, const std::string& script, std::filesystem::path* created)
    {
        if (root.empty())
            return Failed("The templates folder is not known");
        const std::filesystem::path folder = form.ExplicitCategory ? root : CategoryFolder(root, form.Category);
        const std::filesystem::path stem = UniqueStem(folder, FileStem(form.Caption));
        const std::filesystem::path scriptPath = stem.wstring() + L".CEA";
        const std::filesystem::path settingsPath = stem.wstring() + L".Settings.lua";
        if (!WriteFile(scriptPath, script))
            return Failed("Could not write " + PathText(scriptPath));
        if (!WriteFile(settingsPath, WriteSettings(form)))
            return Failed("Could not write " + PathText(settingsPath));
        if (created)
            *created = scriptPath;
        return Status::Success();
    }

    Status SaveScript(const TemplateInfo& info, const std::string& script)
    {
        if (info.BuiltIn || info.ScriptPath.empty())
            return Failed("Built-in templates change only through a copy");
        if (!WriteFile(info.ScriptPath, script))
            return Failed("Could not write " + PathText(info.ScriptPath));
        return Status::Success();
    }

    Status SaveSettingsText(const TemplateInfo& info, const std::string& text)
    {
        if (info.BuiltIn || info.ScriptPath.empty())
            return Failed("Built-in templates change only through a copy");
        const std::filesystem::path path = SettingsPathOf(info);
        if (!WriteFile(path, text))
            return Failed("Could not write " + PathText(path));
        return Status::Success();
    }

    Status DuplicateTemplate(const Library& library, const TemplateInfo& info, const std::filesystem::path& root, bool keepId, std::string* newId)
    {
        const auto script = library.Script(info);
        if (!script)
            return Failed("Could not read " + info.SourceName);
        SettingsForm form = FormOf(info);
        if (!keepId)
        {
            form.Caption += " (copy)";
            form.Id = UniqueId(library, form.Caption);
            if (form.SchemaVersion < 2)
            {
                form.SchemaVersion = 2;
                form.AllowUnsafeGlobals = info.AllowUnsafeGlobals;
            }
        }
        const std::filesystem::path folder = info.BuiltIn ? root : info.ScriptPath.parent_path();
        if (folder.empty())
            return Failed("The templates folder is not known");
        const std::filesystem::path stem = UniqueStem(folder, FileStem(info.BuiltIn ? info.Name : form.Caption));
        const std::filesystem::path scriptPath = stem.wstring() + L".CEA";
        const std::filesystem::path settingsPath = stem.wstring() + L".Settings.lua";
        if (!WriteFile(scriptPath, script->Text))
            return Failed("Could not write " + PathText(scriptPath));
        const std::string settings = keepId && info.HasSettings ? info.SettingsText : WriteSettings(form);
        if ((info.HasSettings || !keepId) && !WriteFile(settingsPath, settings))
            return Failed("Could not write " + PathText(settingsPath));
        if (newId)
            *newId = form.Id;
        return Status::Success();
    }

    Status MoveTemplate(const TemplateInfo& info, const std::filesystem::path& root, std::string_view category)
    {
        if (info.BuiltIn)
            return Failed("Built-in templates cannot be moved. Duplicate the template first");
        if (info.ExplicitCategory)
        {
            static const std::regex field(R"(((?:Category|SubMenuName)\s*=\s*)("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'))");
            const std::string& text = info.SettingsText;
            std::smatch match;
            std::string updated;
            const auto count = std::distance(std::sregex_iterator(text.begin(), text.end(), field), std::sregex_iterator());
            if (count == 1 && std::regex_search(text, match, field))
                updated = match.prefix().str() + match[1].str() + QuoteLua(category) + match.suffix().str();
            else
            {
                SettingsForm form = FormOf(info);
                form.Category = std::string(category);
                updated = WriteSettings(form);
            }
            return SaveSettingsText(info, updated);
        }
        const std::filesystem::path folder = CategoryFolder(root, category);
        std::error_code same;
        if (std::filesystem::exists(folder, same) && std::filesystem::equivalent(folder, info.ScriptPath.parent_path(), same))
            return Status::Success();
        const std::filesystem::path stem = UniqueStem(folder, info.Name);
        if (!MoveFile(info.ScriptPath, stem.wstring() + L".CEA"))
            return Failed("Could not move " + PathText(info.ScriptPath));
        std::error_code error;
        const std::filesystem::path settings = SettingsPathOf(info);
        if (std::filesystem::exists(settings, error) && !MoveFile(settings, stem.wstring() + L".Settings.lua"))
            return Failed("Could not move " + PathText(settings));
        return Status::Success();
    }

    Status DeleteTemplate(const TemplateInfo& info, bool recycle)
    {
        if (info.BuiltIn)
            return Failed("Built-in templates cannot be deleted");
        std::vector<std::filesystem::path> files = { info.ScriptPath };
        std::error_code error;
        const std::filesystem::path settings = SettingsPathOf(info);
        if (std::filesystem::exists(settings, error))
            files.push_back(settings);
        if (recycle)
        {
            std::wstring list;
            for (const auto& file : files)
            {
                list += std::filesystem::absolute(file, error).wstring();
                list.push_back(L'\0');
            }
            list.push_back(L'\0');
            SHFILEOPSTRUCTW operation{};
            operation.wFunc = FO_DELETE;
            operation.pFrom = list.c_str();
            operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
            if (::SHFileOperationW(&operation) != 0 || operation.fAnyOperationsAborted)
                return Failed("Could not move " + PathText(info.ScriptPath) + " to the recycle bin");
            return Status::Success();
        }
        for (const auto& file : files)
        {
            std::filesystem::remove(file, error);
            if (error)
                return Failed("Could not remove " + PathText(file));
        }
        return Status::Success();
    }

    Status ImportTemplates(const std::filesystem::path& source, const std::filesystem::path& root, int* imported)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(source, error))
            return Failed("Not a folder: " + PathText(source));
        if (std::filesystem::exists(root, error) && std::filesystem::equivalent(source, root, error))
            return Failed("The folder is the templates folder itself");
        int count = 0;
        for (std::filesystem::recursive_directory_iterator it(source, std::filesystem::directory_options::skip_permission_denied, error), end; it != end; it.increment(error))
        {
            if (error)
                break;
            if (!it->is_regular_file(error))
                continue;
            const std::string name = Lower(WideToUtf8(it->path().filename().wstring()));
            const bool script = name.size() > 4 && name.ends_with(".cea");
            const bool settings = name.ends_with(".settings.lua");
            if (!script && !settings)
                continue;
            const std::filesystem::path target = root / it->path().lexically_relative(source);
            if (std::filesystem::exists(target, error))
                continue;
            std::filesystem::create_directories(target.parent_path(), error);
            std::filesystem::copy_file(it->path(), target, error);
            if (error)
                return Failed("Could not copy " + PathText(it->path()));
            if (script)
                ++count;
        }
        if (imported)
            *imported = count;
        return Status::Success();
    }
}
