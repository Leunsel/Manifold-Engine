#include "UI/StructExchangeWindow.h"

#include "App/Engine.h"
#include "Core/Ini.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Layout/Layout.h"
#include "Scan/ValueType.h"
#include "Table/CheatTable.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace Manifold::UI
{
    namespace
    {
        const std::vector<FileFilter> kImportFiles = { { L"Structures (*.h;*.hpp;*.hxx;*.c;*.cpp;*.cs;*.rcnet;*.txt)", L"*.h;*.hpp;*.hxx;*.c;*.cpp;*.cs;*.rcnet;*.txt" },
            { L"C/C++ headers (*.h;*.hpp)", L"*.h;*.hpp;*.hxx" }, { L"C# files (*.cs)", L"*.cs" }, { L"ReClass.NET projects (*.rcnet)", L"*.rcnet" },
            { L"All files", L"*.*" } };

        const char* FormatName(StructFormat format)
        {
            return format == StructFormat::Cpp ? "C/C++" : format == StructFormat::CSharp ? "C#" : "ReClass.NET";
        }

        bool Is64Bit(Engine& engine)
        {
            return !engine.HasProcess() || engine.Process()->PointerSize() == 8;
        }

        // The text of an export, or the bytes of a .rcnet.
        std::vector<std::uint8_t> Exported(Engine& engine, const std::vector<std::string>& roots, StructFormat format, bool children)
        {
            if (format == StructFormat::ReClass)
                return Table::ExportReClass(engine.Structures(), roots, Is64Bit(engine), children);
            Table::StructExportOptions options;
            options.Language = format == StructFormat::CSharp ? Table::StructLanguage::CSharp : Table::StructLanguage::Cpp;
            options.Is64Bit = Is64Bit(engine);
            options.Children = children;
            const std::string text = Table::ExportStructures(engine.Structures(), roots, options);
            return std::vector<std::uint8_t>(text.begin(), text.end());
        }

        std::string Structures(std::size_t count)
        {
            return Grouped(count) + (count == 1 ? " structure" : " structures");
        }

        // Notepad and PowerShell may save UTF-16; the parser reads UTF-8.
        std::vector<std::uint8_t> FileText(std::string bytes)
        {
            if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE)
            {
                std::wstring wide((bytes.size() - 2) / 2, L'\0');
                std::memcpy(wide.data(), bytes.data() + 2, wide.size() * sizeof(wchar_t));
                bytes = WideToUtf8(wide);
            }
            else if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
                     static_cast<unsigned char>(bytes[2]) == 0xBF)
            {
                bytes.erase(0, 3);
            }
            return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
        }
    }

    namespace
    {
        // What the dissect will show, for the tooltip of a structure's name.
        std::string FieldList(const Table::StructDefinition& definition)
        {
            constexpr std::size_t kShown = 40;
            std::string out;
            for (std::size_t i = 0; i < std::min(kShown, definition.Fields.size()); ++i)
            {
                const Table::StructField& field = definition.Fields[i];
                std::string type = field.Type == Scan::ValueType::Binary ? Format("bits %d..%d", field.BitStart, field.BitStart + field.BitLength - 1)
                                                                        : std::string(Scan::TypeName(field.Type));
                if (field.Type != Scan::ValueType::Binary && field.Size != 4)
                    type += Format(" (%d)", field.Size);
                if (!field.ChildStructure.empty())
                    type += (field.Type == Scan::ValueType::Pointer ? " -> " : " : ") + field.ChildStructure;
                out += Format("+%04X  %-24s %s\n", field.Offset, field.Name.c_str(), type.c_str());
            }
            if (definition.Fields.size() > kShown)
                out += Format("and %zu more", definition.Fields.size() - kShown);
            return out;
        }
    }

    void ExportStructuresToFile(Workspace& workspace, const std::vector<std::string>& roots, StructFormat format, bool children)
    {
        Engine& engine = workspace.Core();
        const std::string stem = roots.size() == 1 ? Table::Identifier(roots.front()) : std::string("structures");
        std::optional<std::filesystem::path> path;
        if (format == StructFormat::Cpp)
            path = workspace.SaveFileDialog({ { L"C/C++ headers (*.h)", L"*.h" }, { L"All files", L"*.*" } }, stem + ".h", L"h");
        else if (format == StructFormat::CSharp)
            path = workspace.SaveFileDialog({ { L"C# files (*.cs)", L"*.cs" }, { L"All files", L"*.*" } }, stem + ".cs", L"cs");
        else
            path = workspace.SaveFileDialog({ { L"ReClass.NET projects (*.rcnet)", L"*.rcnet" } }, stem + ".rcnet", L"rcnet");
        if (!path)
            return;
        const std::size_t count = Table::ExportOrder(engine.Structures(), roots, children).size();
        const std::vector<std::uint8_t> data = Exported(engine, roots, format, children);
        if (!Ini::WriteAtomic(*path, std::string_view(reinterpret_cast<const char*>(data.data()), data.size())))
        {
            engine.Notify(LogLevel::Error, "Could not write " + WideToUtf8(path->wstring()) + ".");
            return;
        }
        engine.Notify(LogLevel::Success, Format("Exported %s as %s to %s (%s pointers).", Structures(count).c_str(), FormatName(format),
                                             WideToUtf8(path->filename().wstring()).c_str(), Is64Bit(engine) ? "64-bit" : "32-bit"));
    }

    void CopyStructures(Workspace& workspace, const std::vector<std::string>& roots, StructFormat format, bool children)
    {
        Engine& engine = workspace.Core();
        const std::vector<std::uint8_t> data = Exported(engine, roots, format, children);
        const std::string text(data.begin(), data.end());
        ImGui::SetClipboardText(text.c_str());
        engine.Notify(LogLevel::Success, Format("Copied %s as %s.", Structures(Table::ExportOrder(engine.Structures(), roots, children).size()).c_str(), FormatName(format)));
    }

    StructImportWindow::StructImportWindow(Workspace& workspace) : m_workspace(workspace)
    {
        m_is64Bit = Is64Bit(workspace.Core());
    }

    bool StructImportWindow::FromFile()
    {
        const auto path = m_workspace.OpenFileDialog(kImportFiles);
        return path && FromPath(*path);
    }

    bool StructImportWindow::FromPath(const std::filesystem::path& path)
    {
        const std::optional<std::string> bytes = Ini::ReadText(path);
        if (!bytes)
        {
            m_workspace.Core().Notify(LogLevel::Error, "Could not read " + WideToUtf8(path.wstring()) + ".");
            return false;
        }
        Load(FileText(*bytes), WideToUtf8(path.filename().wstring()));
        return true;
    }

    bool StructImportWindow::FromClipboard()
    {
        const char* text = ImGui::GetClipboardText();
        if (!text || !*text)
        {
            m_workspace.Core().Notify(LogLevel::Warning, "The clipboard holds no text.");
            return false;
        }
        Load(FileText(text), "the clipboard");
        return true;
    }

    void StructImportWindow::Load(std::vector<std::uint8_t> data, std::string source)
    {
        m_data = std::move(data);
        m_source = std::move(source);
        m_reclass = Table::LooksLikeReClass(m_data);
        m_left.clear();
        Show();
        Parse();
    }

    void StructImportWindow::Parse()
    {
        Table::StructImportOptions options;
        options.Is64Bit = m_is64Bit;
        Result<Table::StructImport> result = m_reclass ? Table::ImportReClass(m_data, options)
                                                       : Table::ImportStructures(std::string_view(reinterpret_cast<const char*>(m_data.data()), m_data.size()), options);
        if (result)
        {
            m_parsed = std::move(*result);
            m_error.clear();
        }
        else
        {
            m_parsed.reset();
            m_error = result.Error().Text;
        }
    }

    void StructImportWindow::Summary()
    {
        const std::size_t found = m_parsed ? m_parsed->Structures.size() : 0;
        Layout::Heading(Format("%s in %s", Structures(found).c_str(), m_source.c_str()));
        Layout::Muted(m_reclass ? "A ReClass.NET project: its classes are laid out node after node, as ReClass.NET does; its own platform decides the pointer size."
                                : "Laid out as MSVC does it (alignment, #pragma pack, bit fields, virtual function tables). Nothing changes until Import.",
            Layout::Overflow::Wrap);
        if (!m_error.empty())
            Layout::Colored(m_error, ManifoldTheme::Error, Layout::Overflow::Wrap);
        if (m_parsed && !m_parsed->Warnings.empty())
        {
            const std::string count = Grouped(m_parsed->Warnings.size());
            Layout::Card warnings("##structwarnings", { .Title = "Warnings", .Badge = count });
            constexpr std::size_t kShown = 4;
            for (std::size_t i = 0; i < std::min(kShown, m_parsed->Warnings.size()); ++i)
                Layout::Colored(ICON_FA_EXCLAMATION_TRIANGLE "  " + m_parsed->Warnings[i], ManifoldTheme::Warning, Layout::Overflow::Ellipsis);
            if (m_parsed->Warnings.size() > kShown)
            {
                Layout::Muted(Format("and %zu more", m_parsed->Warnings.size() - kShown));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    std::string all;
                    for (const std::string& warning : m_parsed->Warnings)
                        all += warning + "\n";
                    Layout::TextTooltip(all);
                }
            }
            warnings.End();
        }

        Layout::Row options("##structoptions", { Layout::Size::Fit(), Layout::Size::Fixed(16.0f), Layout::Size::Fit(), Layout::Size::Fill() }, Layout::Toolbar());
        options.Cell([&] {
            ImGui::BeginDisabled(m_reclass);
            if (ImGui::Checkbox("64-bit Pointers##structbits", &m_is64Bit))
                Parse();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                Layout::TextTooltip("The size of pointers, size_t, uintptr_t, HANDLE and IntPtr. It starts as the open process's.");
        });
        options.Skip();  // a gap between the two options
        options.Cell([&] {
            Layout::FieldOptions field;
            field.Placement = Layout::LabelPlacement::Left;
            field.Width = Px(230.0f);
            Layout::Field("Names Already Taken", field);
            static const char* kModes[] = { "Replace the Existing Ones", "Keep Both (Rename)", "Skip Them" };
            int mode = static_cast<int>(m_conflict);
            if (ImGui::Combo("##structconflict", &mode, kModes, 3))
                m_conflict = static_cast<Table::StructConflict>(mode);
        });
        options.Skip();
        options.End();
    }

    void StructImportWindow::List()
    {
        if (!m_parsed)
            return;
        std::vector<Table::StructDefinition> chosen;
        for (const Table::StructDefinition& definition : m_parsed->Structures)
        {
            if (!m_left.count(definition.Name))
                chosen.push_back(definition);
        }
        const std::vector<std::string> names = Table::PlanMerge(m_workspace.Core().Structures(), chosen, m_conflict);
        if (!BeginDataTable("##structlist", { { "", 0.25f }, { "Structure", 2.4f }, { "Size", 0.7f }, { "Fields", 0.6f }, { "In the Table", 2.4f } },
                ImVec2(0.0f, -Px(46.0f))))
            return;
        std::size_t next = 0;
        for (std::size_t i = 0; i < m_parsed->Structures.size(); ++i)
        {
            const Table::StructDefinition& definition = m_parsed->Structures[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool on = !m_left.count(definition.Name);
            if (ImGui::Checkbox("##structon", &on))
            {
                if (on)
                    m_left.erase(definition.Name);
                else
                    m_left.insert(definition.Name);
            }
            ImGui::TableNextColumn();
            Layout::Text(definition.Name, { .Font = Fonts::Mono() });
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                Layout::TextTooltip(FieldList(definition));
            ImGui::TableNextColumn();
            Layout::Muted(Format("0x%X", definition.Size()));
            ImGui::TableNextColumn();
            Layout::Muted(Grouped(definition.Fields.size()));
            ImGui::TableNextColumn();
            if (!on)
                Layout::Muted("Left out");
            else
            {
                const std::string& name = next < names.size() ? names[next] : definition.Name;
                ++next;
                const bool exists = m_workspace.Core().Structures().Find(definition.Name) != nullptr;
                if (name.empty())
                    Layout::Muted(exists ? "Skipped: the table has one of this name" : "Skipped: the name comes twice, the first one counts");
                else if (name != definition.Name)
                    Layout::Colored("New, as " + name, ManifoldTheme::Warning);
                else if (exists)
                    Layout::Colored("Replaces the existing one", ManifoldTheme::Warning);
                else
                    Layout::Text("New");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void StructImportWindow::Footer()
    {
        const std::size_t chosen = m_parsed ? m_parsed->Structures.size() - std::min(m_parsed->Structures.size(), m_left.size()) : 0;
        Layout::Row bar("##structimportbar", { Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fill(), Layout::Size::Fit() }, Layout::Toolbar());
        bar.Cell([&] {
            if (Layout::Button(ICON_FA_FILE_IMPORT "  Other File...##structother", Layout::ButtonKind::Ghost))
                FromFile();
        });
        bar.Cell([&] {
            if (Layout::Button(ICON_FA_PASTE "  From Clipboard##structpaste", Layout::ButtonKind::Ghost))
                FromClipboard();
        });
        bar.Skip();
        bar.Cell([&] {
            Layout::ButtonStyle style;
            style.Kind = Layout::ButtonKind::Accent;
            ImGui::BeginDisabled(chosen == 0);
            if (Layout::Button(ICON_FA_CHECK "  Import " + Grouped(chosen) + (chosen == 1 ? " Structure" : " Structures") + "##structimport", style))
                Import();
            ImGui::EndDisabled();
        });
        bar.End();
    }

    void StructImportWindow::Import()
    {
        if (!m_parsed)
            return;
        std::vector<Table::StructDefinition> chosen;
        for (const Table::StructDefinition& definition : m_parsed->Structures)
        {
            if (!m_left.count(definition.Name))
                chosen.push_back(definition);
        }
        Engine& engine = m_workspace.Core();
        const Table::StructMerge merge = Table::MergeStructures(engine.Structures(), std::move(chosen), m_conflict);
        if (merge.Added + merge.Replaced > 0)
            engine.Table().Touch();  // the structures are part of the table: unsaved until it is saved
        std::string message = Format("Imported %s from %s", Structures(merge.Added + merge.Replaced).c_str(), m_source.c_str());
        if (merge.Replaced)
            message += Format(", %d replaced", merge.Replaced);
        if (merge.Skipped)
            message += Format(", %d skipped (the name was taken)", merge.Skipped);
        engine.Notify(LogLevel::Success, message + ".");
        m_open = false;
    }

    void StructImportWindow::Draw()
    {
        if (!m_open)
            return;
        if (!BeginWindow(ICON_FA_FILE_IMPORT "  Import Structures###structimport", ImVec2(760.0f, 480.0f), 0, ImVec2(480.0f, 300.0f)))
            return;
        Summary();
        Layout::Spacing(Layout::Space::S);
        List();
        Footer();
        EndToolWindow();
    }
}
