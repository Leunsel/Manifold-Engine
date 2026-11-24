#pragma once

#include "Scan/TypeCatalog.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class RecordEditor : public ToolWindowBase
    {
    public:
        RecordEditor(Workspace& workspace, int recordId, RecordField field = RecordField::None);
        void Draw() override;
        int RecordId() const { return m_recordId; }
        void Focus(RecordField field = RecordField::None);

    private:
        ImGuiInputTextFlags TakeFocus(RecordField field);
        void FocusDone(RecordField field);
        void Load(const Table::MemoryRecord& record);
        void Apply(Table::MemoryRecord& record);
        void AddressSection(Table::MemoryRecord& record);
        void ValueSection();
        void DropDownSection(Table::MemoryRecord& record);
        void ChildrenSection(Table::MemoryRecord& record);
        void HotkeySection();
        void LookSection();
        void CopyAddress(const char* id, std::uint64_t address);

        Workspace& m_workspace;
        Engine& m_engine;
        int m_recordId = 0;
        std::string m_title;
        std::string m_description;
        std::string m_address;
        std::vector<std::string> m_offsets;
        Scan::TypeChoice m_type;
        int m_length = 10;
        bool m_unicode = false;
        bool m_zeroTerminate = true;
        int m_bitStart = 0;
        int m_bitLength = 1;
        bool m_hex = false;
        bool m_signed = true;
        Table::RecordOptions m_options;
        int m_freeze = 0;
        std::string m_dropDown;
        std::string m_dropDownLink;
        int m_dropDownSource = 0;
        bool m_hasAddress = false;
        std::string m_copied;
        double m_copiedAt = -10.0;
        bool m_async = false;
        bool m_dropDownOnly = false;
        bool m_dropDownDescription = false;
        std::vector<Table::Hotkey> m_hotkeys;
        bool m_capturing = false;
        int m_captureIndex = -1;
        std::vector<int> m_capture;
        float m_color[3] = { 1.0f, 1.0f, 1.0f };
        bool m_hasColor = false;
        std::string m_comment;
        std::string m_error;
        RecordField m_focusField = RecordField::None;
        int m_focusFrames = 0;
        bool m_enterOk = false;
    };
}
