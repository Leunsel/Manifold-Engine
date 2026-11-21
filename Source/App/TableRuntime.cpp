#include "App/TableRuntime.h"

#include "App/Engine.h"
#include "App/Sounds.h"
#include "Core/Profiler.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>

#include <windows.h>

namespace Manifold
{
    void TableRuntime::ResolveRecord(Table::MemoryRecord& record)
    {
        record.AddressOk = false;
        const bool addressHeader = record.Kind == Table::RecordKind::Group && record.AddressGroupHeader;
        if ((record.Kind != Table::RecordKind::Value && !addressHeader) || !m_engine.Session().Process())
            return;
        Status error;
        std::optional<std::uint64_t> base;
        const std::string_view written = TrimView(record.Address);
        if (!written.empty() && (written.front() == '+' || written.front() == '-') && record.Parent)
        {
            ResolveRecord(*record.Parent);
            if (!record.Parent->AddressOk)
            {
                record.AddressError = record.Parent->AddressError.empty() ? ErrorMessage(ErrorCode::ExprUnknownSymbol, record.Parent->Description) : record.Parent->AddressError;
                return;
            }
            const auto delta = m_engine.Evaluate(written.substr(1), &error);
            if (delta)
                base = written.front() == '+' ? record.Parent->ResolvedAddress + *delta : record.Parent->ResolvedAddress - *delta;
        }
        else
        {
            base = m_engine.Evaluate(record.Address, &error);
        }
        if (!base)
        {
            record.AddressError = error.Text;
            return;
        }
        std::uint64_t address = *base;
        for (const std::string& offsetText : record.Offsets)
        {
            const auto pointer = m_engine.Session().Process()->ReadPointer(address);
            if (!pointer)
            {
                record.AddressError = ErrorMessage(ErrorCode::ExprPointerUnreadable, Hex(address));
                return;
            }
            Status offsetError;
            const auto offset = m_engine.Evaluate(offsetText.empty() ? "0" : offsetText, &offsetError);
            if (!offset)
            {
                record.AddressError = offsetError.Text;
                return;
            }
            address = *pointer + *offset;
        }
        record.ResolvedAddress = address;
        record.AddressOk = true;
        record.AddressError.clear();
    }

    std::string TableRuntime::AddressHex(std::uint64_t address) const
    {
        const int mode = m_engine.Options().AddressDigits;
        if (mode == 1)
            return Hex(address, 8);
        if (mode == 2)
        {
            const auto& process = m_engine.Session().Process();
            return Hex(address, process && !process->Is64Bit() ? 8 : 16);
        }
        return Hex(address);
    }

    RecordAddressText TableRuntime::AddressTextOf(const Table::MemoryRecord& record) const
    {
        RecordAddressText text;
        if (record.IsPointer())
        {
            text.Text = record.AddressOk ? "P->" + AddressHex(record.ResolvedAddress) : "P->????????";
            text.Tooltip = "[" + record.Address + "] + " + Join(record.Offsets, " + ");
            return text;
        }
        const std::string_view written = TrimView(record.Address);
        if (!written.empty() && (written.front() == '+' || written.front() == '-') && record.Parent)
        {
            text.Text = record.AddressOk ? AddressHex(record.ResolvedAddress) : record.Address;
            text.Tooltip = (record.Parent->Description.empty() ? std::string("Parent") : record.Parent->Description) + " " + std::string(written);
            return text;
        }
        const int mode = m_engine.Options().AddressColumn;
        const bool plain = ParseHex(written) == std::optional<std::uint64_t>(record.ResolvedAddress);
        if (mode == 1 && record.AddressOk)
        {
            text.Text = AddressHex(record.ResolvedAddress);
            text.Tooltip = plain ? std::string() : record.Address;
            return text;
        }
        text.Text = record.Address;
        if (record.AddressOk)
        {
            if (mode == 2 && !plain)
                text.Resolved = AddressHex(record.ResolvedAddress);
            text.Tooltip = AddressHex(record.ResolvedAddress);
        }
        return text;
    }

    std::vector<std::uint8_t> TableRuntime::ReadRecord(Table::MemoryRecord& record)
    {
        ResolveRecord(record);
        if (!record.AddressOk)
            return {};
        const Lua::CustomType* custom = record.CustomTypeName.empty() ? nullptr : m_engine.Lua().FindCustomType(record.CustomTypeName);
        const int size = custom ? custom->ByteCount() : record.Size(m_engine.Session().Process()->PointerSize());
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::max(1, size)));
        if (!m_engine.Session().Process()->ReadExact(record.ResolvedAddress, bytes.data(), bytes.size()))
            return {};
        return bytes;
    }

    std::string TableRuntime::FormatRecord(const Table::MemoryRecord& record, std::span<const std::uint8_t> bytes) const
    {
        if (bytes.empty())
            return "??";
        const int pointerSize = m_engine.Session().Process() ? m_engine.Session().Process()->PointerSize() : 8;
        std::string text;
        const Lua::CustomType* custom = record.CustomTypeName.empty() ? nullptr : m_engine.Lua().FindCustomType(record.CustomTypeName);
        if (custom)
        {
            if (!m_engine.Lua().CustomToText(*custom, bytes, record.ResolvedAddress, text))
                return "??";
            for (const Table::DropDownEntry& entry : DropDownOf(record))
            {
                if (EqualsNoCase(entry.Value, text))
                    return record.DropDownDescriptionOnly ? entry.Text : text + " : " + entry.Text;
            }
            return text;
        }
        text = Scan::FormatValue(record.Type, bytes, record.Format(pointerSize));
        if (record.Type == Scan::ValueType::Pointer && m_engine.Session().Process())
        {
            std::uint64_t pointer = 0;
            std::memcpy(&pointer, bytes.data(), std::min<std::size_t>(bytes.size(), static_cast<std::size_t>(pointerSize)));
            const std::string described = m_engine.Session().Symbols().Describe(pointer);
            if (described != Hex(pointer))
                text = "P->" + described;
        }
        for (const Table::DropDownEntry& entry : DropDownOf(record))
        {
            if (EqualsNoCase(entry.Value, text))
                return record.DropDownDescriptionOnly ? entry.Text : text + " : " + entry.Text;
        }
        return text;
    }

    const std::vector<Table::DropDownEntry>& TableRuntime::DropDownOf(const Table::MemoryRecord& record) const
    {
        if (record.DropDownLink.empty())
            return record.DropDown;
        std::string link = Trim(record.DropDownLink);
        if (link.size() >= 2 && link.front() == '"' && link.back() == '"')
            link = link.substr(1, link.size() - 2);
        if (Table::MemoryRecord* linked = const_cast<Table::AddressList&>(m_table).FindByDescription(link); linked && linked != &record)
            return linked->DropDown;
        return record.DropDown;
    }

    Status TableRuntime::WriteRecord(Table::MemoryRecord& record, std::string_view text, bool recursive)
    {
        if (record.Kind == Table::RecordKind::Value)
        {
            if (!m_engine.Session().Process())
                return Status::Fail(ErrorCode::NoProcess);
            ResolveRecord(record);
            if (!record.AddressOk)
                return Status::Fail(ErrorCode::ExprSyntax, record.AddressError.empty() ? record.Address : record.AddressError);
            std::string valueText(TrimView(text));
            for (const Table::DropDownEntry& entry : DropDownOf(record))
            {
                if (EqualsNoCase(entry.Text, valueText))
                    valueText = entry.Value;
            }
            Scan::ValueFormat format = record.Format(m_engine.Session().Process()->PointerSize());
            const Lua::CustomType* custom = record.CustomTypeName.empty() ? nullptr : m_engine.Lua().FindCustomType(record.CustomTypeName);
            if (custom)
            {
                std::vector<std::uint8_t> converted;
                if (!m_engine.Lua().CustomFromText(*custom, valueText, record.ResolvedAddress, converted))
                    return Status::Fail(ErrorCode::ScanBadValue, valueText);
                const Status written = m_engine.Session().Process()->Write(record.ResolvedAddress, converted);
                if (!written)
                    return written;
                if (record.Active)
                    record.FrozenBytes = converted;
                return Status::Success();
            }
            auto bytes = Scan::ParseValue(record.Type, valueText, format);
            if (!bytes)
                return Status::Fail(ErrorCode::ScanBadValue, valueText);
            if (record.Type == Scan::ValueType::String)
            {
                if (record.ZeroTerminate)
                {
                    bytes->push_back(0);
                    if (record.Unicode)
                        bytes->push_back(0);
                }
            }
            if (record.Type == Scan::ValueType::Binary)
            {
                const int size = record.Size(m_engine.Session().Process()->PointerSize());
                std::vector<std::uint8_t> current(static_cast<std::size_t>(size));
                if (!m_engine.Session().Process()->ReadExact(record.ResolvedAddress, current.data(), current.size()))
                    return Status::Fail(ErrorCode::ReadFailed, Hex(record.ResolvedAddress));
                std::uint64_t whole = 0;
                std::memcpy(&whole, current.data(), std::min<std::size_t>(8, current.size()));
                std::uint64_t value = 0;
                std::memcpy(&value, bytes->data(), 8);
                const std::uint64_t mask = (record.BitLength >= 64 ? ~0ull : ((1ull << record.BitLength) - 1)) << record.BitStart;
                whole = (whole & ~mask) | ((value << record.BitStart) & mask);
                bytes->assign(current.size(), 0);
                std::memcpy(bytes->data(), &whole, std::min<std::size_t>(8, bytes->size()));
            }
            const Status written = m_engine.Session().Process()->Write(record.ResolvedAddress, *bytes);
            if (!written)
                return written;
            if (record.Active)
                record.FrozenBytes = *bytes;
            LOG_DEBUG("Table", "Wrote " + valueText + " to " + record.Description);
        }
        if (recursive && record.Options.RecursiveSetValue)
        {
            for (const auto& child : record.Children)
            {
                const Status status = WriteRecord(*child, text, true);
                if (!status && child->Kind == Table::RecordKind::Value)
                    return status;
            }
        }
        return Status::Success();
    }

    Status TableRuntime::AddToRecord(Table::MemoryRecord& record, double delta)
    {
        if (record.Kind != Table::RecordKind::Value || !m_engine.Session().Process())
            return Status::Fail(ErrorCode::NoProcess);
        const std::vector<std::uint8_t> bytes = ReadRecord(record);
        if (bytes.empty())
            return Status::Fail(ErrorCode::ReadFailed, Hex(record.ResolvedAddress));
        const auto value = Scan::AsDouble(record.Type, bytes, record.ShowSigned);
        if (!value)
            return Status::Fail(ErrorCode::ScanBadValue, record.Description);
        const std::vector<std::uint8_t> next = Scan::FromDouble(record.Type, *value + delta, m_engine.Session().Process()->PointerSize());
        const Status written = m_engine.Session().Process()->Write(record.ResolvedAddress, next);
        if (written && record.Active)
            record.FrozenBytes = next;
        return written;
    }

    void TableRuntime::RefreshValues(bool force)
    {
        const double now = m_engine.Now();
        if (!force && now - m_lastValues < m_engine.Options().UpdateIntervalMs / 1000.0)
            return;
        m_lastValues = now;
        std::vector<int> ids;
        if (m_visibleKnown)
        {
            ids = m_visibleRecords;
            const std::vector<int> watched = m_engine.Lua().RecordsWithEvent("onvaluechanged");
            ids.insert(ids.end(), watched.begin(), watched.end());
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        }
        else
        {
            m_table.ForEach([&](Table::MemoryRecord& record)
                {
                    if (record.Kind == Table::RecordKind::Value)
                        ids.push_back(record.Id);
                });
        }
        for (int id : ids)
            RefreshRecord(id, now);
    }

    void TableRuntime::RefreshRecord(int id, double now)
    {
        Table::MemoryRecord* record = m_table.FindById(id);
        if (!record || record->Kind != Table::RecordKind::Value)
            return;
        record->RefreshedAt = now;
        if (!m_engine.Session().Process())
        {
            record->ValueText = "??";
            return;
        }
        const std::vector<std::uint8_t> bytes = ReadRecord(*record);
        if (bytes.empty())
        {
            record->ValueText = "??";
            record->LastBytes.clear();
            return;
        }
        std::string text = FormatRecord(*record, bytes);
        m_engine.Lua().DisplayValue(*record, text);
        record = m_table.FindById(id);
        if (!record)
            return;
        if (!record->LastBytes.empty() && record->LastBytes != bytes)
        {
            record->ValueChanged = true;
            record->ChangedAt = now;
            if (m_engine.Lua().HasRecordEvent(id, "onvaluechanged"))
            {
                m_engine.Lua().ValueChanged(*record, record->ValueText, text);
                record = m_table.FindById(id);
                if (!record)
                    return;
            }
        }
        record->LastBytes = bytes;
        record->ValueText = std::move(text);
    }

    void TableRuntime::SetVisibleRecords(std::vector<int> ids)
    {
        std::sort(ids.begin(), ids.end());
        m_visibleRecords = std::move(ids);
        m_visibleKnown = true;
    }

    bool TableRuntime::NeedsValue(const Table::MemoryRecord& record) const
    {
        return record.Kind == Table::RecordKind::Value && record.RefreshedAt < m_lastValues;
    }

    void TableRuntime::RefreshRecord(int id)
    {
        RefreshRecord(id, m_engine.Now());
    }

    void TableRuntime::UpdateFreezes()
    {
        if (!m_engine.Session().Process())
            return;
        const double now = m_engine.Now();
        if (now - m_lastFreeze < m_engine.Options().FreezeIntervalMs / 1000.0)
            return;
        m_lastFreeze = now;
        m_table.ForEach([&](Table::MemoryRecord& record)
            {
                if (record.Kind != Table::RecordKind::Value || !record.Active || record.FrozenBytes.empty())
                    return;
                ResolveRecord(record);
                if (!record.AddressOk)
                    return;
                if (record.Freeze != Table::FreezeMode::Normal)
                {
                    std::vector<std::uint8_t> current(record.FrozenBytes.size());
                    if (!m_engine.Session().Process()->ReadExact(record.ResolvedAddress, current.data(), current.size()))
                        return;
                    const auto now = Scan::AsDouble(record.Type, current, record.ShowSigned);
                    const auto frozen = Scan::AsDouble(record.Type, record.FrozenBytes, record.ShowSigned);
                    if (now && frozen)
                    {
                        if (record.Freeze == Table::FreezeMode::AllowIncrease && *now > *frozen)
                        {
                            record.FrozenBytes = current;
                            return;
                        }
                        if (record.Freeze == Table::FreezeMode::AllowDecrease && *now < *frozen)
                        {
                            record.FrozenBytes = current;
                            return;
                        }
                    }
                }
                m_engine.Session().Process()->Write(record.ResolvedAddress, record.FrozenBytes);
            });
    }

    void TableRuntime::ApplyHotkey(Table::MemoryRecord& record, const Table::Hotkey& hotkey)
    {
        Status status;
        switch (hotkey.Action)
        {
        case Table::HotkeyAction::Toggle: m_engine.RequestActive(record, !record.Active); break;
        case Table::HotkeyAction::ToggleAllowIncrease:
        case Table::HotkeyAction::ToggleAllowDecrease:
            if (!record.Active)
                record.Freeze = hotkey.Action == Table::HotkeyAction::ToggleAllowIncrease ? Table::FreezeMode::AllowIncrease : Table::FreezeMode::AllowDecrease;
            m_engine.RequestActive(record, !record.Active);
            break;
        case Table::HotkeyAction::Activate: m_engine.RequestActive(record, true); break;
        case Table::HotkeyAction::Deactivate: m_engine.RequestActive(record, false); break;
        case Table::HotkeyAction::SetValue: status = WriteRecord(record, hotkey.Value); break;
        case Table::HotkeyAction::Increase:
        case Table::HotkeyAction::Decrease:
        {
            const auto delta = ParseDouble(hotkey.Value.empty() ? "1" : hotkey.Value);
            status = delta ? AddToRecord(record, hotkey.Action == Table::HotkeyAction::Increase ? *delta : -*delta) : Status::Fail(ErrorCode::ScanBadValue, hotkey.Value);
            break;
        }
        }
        if (!status)
            m_engine.Notify(status);
        else
            LOG_INFO("Hotkey", Table::KeysText(hotkey.Keys) + ": " + Table::HotkeyActionName(hotkey.Action) + " " + record.Description);
    }

    namespace
    {
        bool RunsScripts(const Table::MemoryRecord& record)
        {
            if (record.Kind == Table::RecordKind::Script)
                return true;
            return std::any_of(record.Children.begin(), record.Children.end(), [](const auto& child) { return RunsScripts(*child); });
        }
    }

    bool TableRuntime::HotkeyHeld(const Table::MemoryRecord& record, const Table::Hotkey& hotkey) const
    {
        if (m_tableScriptWaiting)
            return true;
        if (m_scriptsTrusted)
            return false;
        switch (hotkey.Action)
        {
        case Table::HotkeyAction::SetValue:
        case Table::HotkeyAction::Increase:
        case Table::HotkeyAction::Decrease:
            return false;
        default:
            return RunsScripts(record);
        }
    }

    void TableRuntime::NoteActivationByHand(const Table::MemoryRecord& record)
    {
        if (!m_scriptsTrusted && RunsScripts(record))
            m_scriptsTrusted = true;
    }

    void TableRuntime::UpdateHotkeys()
    {
        std::vector<std::pair<Table::MemoryRecord*, Table::Hotkey>> fired;
        m_table.ForEach([&](Table::MemoryRecord& record)
            {
                for (Table::Hotkey& hotkey : record.Hotkeys)
                {
                    if (Table::KeysPressed(hotkey.Keys, hotkey.WasDown))
                        fired.emplace_back(&record, hotkey);
                }
            });
        for (auto& [record, hotkey] : fired)
        {
            if (!HotkeyHeld(*record, hotkey))
            {
                ApplyHotkey(*record, hotkey);
                continue;
            }
            if (m_tableScriptWaiting || m_hotkeyHoldNoted)
                continue;
            m_hotkeyHoldNoted = true;
            m_engine.Notify(LogLevel::Warning, "Hotkeys of this table cannot turn scripts on yet. Turn a script on yourself first, or run the table's Lua script.");
        }
    }

    std::vector<Table::TableSymbol> TableRuntime::TableSymbols() const
    {
        return m_tableSymbols;
    }

    void TableRuntime::NewTable()
    {
        m_engine.CancelActivations();
        m_engine.Activation().WaitForActivation();
        m_engine.StopTableScript();
        for (int id : m_engine.Activation().BottomToTop())
        {
            Table::MemoryRecord* record = m_table.FindById(id);
            if (record && record->Kind == Table::RecordKind::Script && record->Active)
                m_engine.SetActive(*record, false);
        }
        m_table.Clear();
        m_table.NewIdentity();
        m_engine.Lua().ForgetRecords();
        m_structures.Clear();
        m_tableSymbols.clear();
        m_tableScriptWaiting = false;
        m_pendingTableScript.clear();
        m_scriptsTrusted = true;
        m_hotkeyHoldNoted = false;
        m_engine.Backups().TableReplaced();
    }

    Status TableRuntime::LoadTable(const std::filesystem::path& path, bool merge)
    {
        Table::LoadedTable loaded;
        const Status status = Table::LoadTable(path, loaded);
        if (!status)
        {
            LOG_ERROR("Table", status.Text);
            return status;
        }
        if (!merge)
        {
            NewTable();
            m_table.FilePath = WideToUtf8(path.wstring());
            m_table.TableLuaScript = loaded.LuaScript;
            m_table.TableComment = loaded.Comments;
            m_table.DisasmNotes = std::move(loaded.DisasmNotes);
            m_table.Files = std::move(loaded.Files);
            m_table.ExtraXml = std::move(loaded.ExtraXml);
            m_table.LoadedVersion = loaded.Version;
            m_table.LoadedFileNaming = loaded.Naming;
            m_tableSymbols = loaded.Symbols;
            m_scriptsTrusted = m_engine.Options().TableScripts == TableScriptPolicy::Always;
        }
        else
        {
            m_tableSymbols.insert(m_tableSymbols.end(), loaded.Symbols.begin(), loaded.Symbols.end());
            for (Table::TableFile& file : loaded.Files)
            {
                if (!m_table.FindFile(file.Name))
                    m_table.Files.push_back(std::move(file));
            }
        }
        for (auto& record : loaded.Records)
            m_table.Add(std::move(record));
        const std::function<void(Table::MemoryRecord&)> fix = [&](Table::MemoryRecord& record)
            {
                m_table.NoteId(record.Id);
                for (auto& child : record.Children)
                {
                    child->Parent = &record;
                    fix(*child);
                }
            };
        for (auto& record : m_table.Roots())
            fix(*record);
        for (auto& structure : loaded.Structures)
        {
            if (!m_structures.Find(structure->Name))
                m_structures.Items.push_back(std::move(structure));
        }
        ++m_structures.Generation;
        for (const Table::TableSymbol& symbol : loaded.Symbols)
        {
            if (const auto value = m_engine.Evaluate(symbol.Address))
                m_engine.Session().Symbols().Register(symbol.Name, *value, "table");
        }
        for (const std::string& warning : loaded.Warnings)
            LOG_WARN("Table", warning);
        m_engine.Options().AddRecentTable(WideToUtf8(path.wstring()));
        m_table.SetClean();
        LOG_SUCCESS("Table", "Loaded " + WideToUtf8(path.filename().wstring()) + " with " + std::to_string(m_table.Count()) + " records");

        if (!merge && !loaded.LuaScript.empty())
        {
            if (m_engine.Options().TableScripts == TableScriptPolicy::Always)
                m_engine.Activation().RunTableScript(loaded.LuaScript);
            else if (m_engine.Options().TableScripts == TableScriptPolicy::Ask)
            {
                m_pendingTableScript = loaded.LuaScript;
                m_tableScriptWaiting = true;
            }
        }
        RefreshValues(true);
        return Status::Success();
    }

    void TableRuntime::AnswerTableScript(bool run)
    {
        m_tableScriptWaiting = false;
        if (run)
        {
            m_scriptsTrusted = true;
            m_engine.Activation().RunTableScript(m_pendingTableScript);
        }
        m_pendingTableScript.clear();
    }

    Status TableRuntime::RestoreTable(const std::filesystem::path& path)
    {
        const std::vector<std::string> recent = m_engine.Options().RecentTables;
        const Status status = LoadTable(path, false);
        if (!status)
            return status;
        m_engine.Options().RecentTables = recent;
        m_table.FilePath.clear();
        m_table.Touch();
        m_engine.Backups().Restored(path);
        return status;
    }

    Status TableRuntime::SaveTable(const std::filesystem::path& path)
    {
        std::vector<Table::TableSymbol> symbols = m_tableSymbols;
        const Status status = Table::SaveTable(path, m_table, m_structures, symbols, m_engine.TableFileNaming());
        if (!status)
        {
            LOG_ERROR("Table", status.Text);
            return status;
        }
        m_table.FilePath = WideToUtf8(path.wstring());
        m_table.SetClean();
        m_engine.Options().AddRecentTable(m_table.FilePath);
        LOG_SUCCESS("Table", "Saved " + WideToUtf8(path.filename().wstring()));
        return status;
    }
}
