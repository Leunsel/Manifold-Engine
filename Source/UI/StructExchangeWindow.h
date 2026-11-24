#pragma once

#include "Table/StructExchange.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Manifold::UI
{
    enum class StructFormat
    {
        Cpp,
        CSharp,
        ReClass
    };

    // Structure Dissect > Structures > Export: the structures (and, with children,
    // what they point to or embed) as a file or on the clipboard. The pointer size
    // is the open process's, 64-bit without one.
    void ExportStructuresToFile(Workspace& workspace, const std::vector<std::string>& roots, StructFormat format, bool children);
    void CopyStructures(Workspace& workspace, const std::vector<std::string>& roots, StructFormat format, bool children);

    // Structures > Import: what a C/C++ header, a C# file, a ReClass.NET project or
    // the clipboard holds, to look over (sizes, warnings, names already taken)
    // before it goes into the table.
    class StructImportWindow : public ToolWindowBase
    {
    public:
        explicit StructImportWindow(Workspace& workspace);
        void Draw() override;
        bool FromFile();
        bool FromClipboard();
        bool FromPath(const std::filesystem::path& path);

    private:
        void Load(std::vector<std::uint8_t> data, std::string source);
        void Parse();
        void Summary();
        void List();
        void Footer();
        void Import();

        Workspace& m_workspace;
        std::vector<std::uint8_t> m_data;
        std::string m_source;
        bool m_reclass = false;
        bool m_is64Bit = true;
        std::optional<Table::StructImport> m_parsed;
        std::string m_error;
        std::set<std::string> m_left;  // names not ticked
        Table::StructConflict m_conflict = Table::StructConflict::Rename;
    };
}
