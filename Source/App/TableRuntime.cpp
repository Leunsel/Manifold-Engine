#include "App/TableRuntime.h"

#include "App/Engine.h"
#include "App/Sounds.h"
#include "Core/Profiler.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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
        if (Table::IsRelativeAddress(written))
        {
            Table::MemoryRecord* from = Table::RelativeBase(record);
            if (!from)
            {
                record.AddressError = ErrorMessage(ErrorCode::ExprUnknownSymbol, std::string(written) + " (no record with an address above it)");
                return;
            }
            ResolveRecord(*from);
            if (!from->AddressOk)
            {
                record.AddressError = from->AddressError.empty() ? ErrorMessage(ErrorCode::ExprUnknownSymbol, from->Description) : from->AddressError;
                return;
            }
            const auto delta = m_engine.Evaluate(written.substr(1), &error);
            if (delta)
                base = written.front() == '+' ? from->ResolvedAddress + *delta : from->ResolvedAddress - *delta;
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

    std::vector<AddressStep> TableRuntime::ExplainAddress(Table::MemoryRecord& record)
    {
        std::vector<AddressStep> steps;
        const auto fail = [&](AddressStep::Kind kind, std::string text, std::string error)
            {
                steps.push_back({ kind, std::move(text), 0, false, std::move(error) });
                return steps;
            };
        const bool addressHeader = record.Kind == Table::RecordKind::Group && record.AddressGroupHeader;
        if (record.Kind != Table::RecordKind::Value && !addressHeader)
            return steps;
        if (!m_engine.Session().Process())
            return fail(AddressStep::Kind::Base, record.Address, ErrorMessage(ErrorCode::NoProcess));
        Status error;
        std::uint64_t address = 0;
        const std::string_view written = TrimView(record.Address);
        if (Table::IsRelativeAddress(written))
        {
            Table::MemoryRecord* from = Table::RelativeBase(record);
            if (!from)
                return fail(AddressStep::Kind::Parent, std::string(written), "No record above this one has an address to count from.");
            Table::MemoryRecord& parent = *from;
            ResolveRecord(parent);
            const std::string name = (parent.Description.empty() ? std::string("Parent") : "\"" + parent.Description + "\"") + " (ID " + std::to_string(parent.Id) + ")";
            if (!parent.AddressOk)
                return fail(AddressStep::Kind::Parent, name, parent.AddressError.empty() ? ErrorMessage(ErrorCode::ExprUnknownSymbol, parent.Description) : parent.AddressError);
            steps.push_back({ AddressStep::Kind::Parent, name, parent.ResolvedAddress });
            const auto delta = m_engine.Evaluate(written.substr(1), &error);
            if (!delta)
                return fail(AddressStep::Kind::Relative, std::string(written), error.Text);
            address = written.front() == '+' ? parent.ResolvedAddress + *delta : parent.ResolvedAddress - *delta;
            steps.push_back({ AddressStep::Kind::Relative, std::string(written), address });
        }
        else
        {
            const auto base = m_engine.Evaluate(record.Address, &error);
            if (!base)
                return fail(AddressStep::Kind::Base, record.Address, error.Text);
            address = *base;
            steps.push_back({ AddressStep::Kind::Base, record.Address, address });
        }
        for (const std::string& offsetText : record.Offsets)
        {
            const auto pointer = m_engine.Session().Process()->ReadPointer(address);
            if (!pointer)
                return fail(AddressStep::Kind::Read, "[" + Hex(address) + "]", ErrorMessage(ErrorCode::ExprPointerUnreadable, Hex(address)));
            steps.push_back({ AddressStep::Kind::Read, "[" + Hex(address) + "]", *pointer });
            Status offsetError;
            const std::string shown = offsetText.empty() ? "0" : offsetText;
            const auto offset = m_engine.Evaluate(shown, &offsetError);
            if (!offset)
                return fail(AddressStep::Kind::Offset, shown, offsetError.Text);
            address = *pointer + *offset;
            steps.push_back({ AddressStep::Kind::Offset, "+ " + shown, address });
        }
        return steps;
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
        if (Table::IsRelativeAddress(written))
        {
            const Table::MemoryRecord* from = Table::RelativeBase(record);
            text.Text = record.AddressOk ? AddressHex(record.ResolvedAddress) : record.Address;
            text.Tooltip = (!from ? std::string("No record with an address above") : from->Description.empty() ? std::string("Parent") : from->Description) + " " + std::string(written);
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
        // a switch runs Lua right away in synchronous mode, which may delete the record: it is not used after the action
        const std::string description = record.Description;
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
            LOG_INFO("Hotkey", Table::KeysText(hotkey.Keys) + ": " + Table::HotkeyActionName(hotkey.Action) + " " + description);
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
        // ∑ By id: a hotkey's switch runs Lua, which may delete the record of the next one or replace the table.
        std::vector<std::pair<int, Table::Hotkey>> fired;
        m_table.ForEach([&](Table::MemoryRecord& record)
            {
                for (Table::Hotkey& hotkey : record.Hotkeys)
                {
                    if (Table::KeysPressed(hotkey.Keys, hotkey.WasDown))
                        fired.emplace_back(record.Id, hotkey);
                }
            });
        const std::uint64_t identity = m_table.Identity();
        for (auto& [id, hotkey] : fired)
        {
            if (m_table.Identity() != identity)
            {
                LOG_INFO("Hotkey", "The table was replaced by a hotkey's switch; the other hotkeys pressed with it are dropped");
                break;
            }
            Table::MemoryRecord* record = m_table.FindById(id);
            if (!record)
                continue;
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

    Status TableRuntime::RemoveRecord(int recordId, bool fromScript)
    {
        const std::uint64_t identity = m_table.Identity();
        Table::MemoryRecord* record = m_table.FindById(recordId);
        if (!record)
            return Status::Fail(ErrorCode::TableRecordGone, std::to_string(recordId));
        const std::string description = record->Description;
        std::vector<int> ids;
        const std::function<void(const Table::MemoryRecord&)> collect = [&](const Table::MemoryRecord& node)
            {
                ids.push_back(node.Id);
                for (const auto& child : node.Children)
                    collect(*child);
            };
        collect(*record);
        // ∑ A switch in the background keeps its result for the record: removed under it, the script's hooks and allocations
        //   would have no record left to take them out again.
        ActivationRunner& activation = m_engine.Activation();
        for (int id : ids)
        {
            if (!m_engine.ActivationPending(id) && !activation.Switching(id))
                continue;
            const Table::MemoryRecord* busy = m_table.FindById(id);
            return Status::Fail(ErrorCode::RecordStays, "\"" + description + "\" cannot be removed while " +
                (id == recordId || !busy ? std::string("it is") : "\"" + busy->Description + "\" below it is") + " being switched; remove it once that is done");
        }
        // deepest first: the reverse of the top-down order has every child before its parent
        for (auto it = ids.rbegin(); it != ids.rend(); ++it)
        {
            Table::MemoryRecord* below = m_table.FindById(*it);
            if (!below || !below->Active)
                continue;
            const bool script = below->Kind == Table::RecordKind::Script;
            const std::string name = below->Description;
            const Status off = m_engine.SetActive(*below, false, fromScript);
            if (m_table.Identity() != identity)
                return Status::Fail(ErrorCode::TableRecordGone, "\"" + description + "\"");
            if (!off && script)
                return off;
            const Table::MemoryRecord* again = m_table.FindById(*it);
            if (script && again && again->Active)
            {
                const Status why = activation.LastOutcome(*it);
                return Status::Fail(ErrorCode::RecordStays, "\"" + name + "\" stayed on" + (why ? std::string() : " (" + why.Text + ")") + ", so \"" + description +
                                                               "\" stays in the table");
            }
        }
        record = m_table.FindById(recordId);
        if (!record)
            return Status::Fail(ErrorCode::TableRecordGone, "\"" + description + "\"");
        // Lua of the switches may have added records below it: what is removed is the tree as it is now
        ids.clear();
        collect(*record);
        for (int id : ids)
        {
            const Table::MemoryRecord* below = m_table.FindById(id);
            if (below && below->Kind == Table::RecordKind::Script && below->Active)
                return Status::Fail(ErrorCode::RecordStays, "\"" + below->Description + "\" below \"" + description + "\" was switched on meanwhile, so it stays in the table");
        }
        m_table.Remove(*record);
        m_table.Touch();
        for (int id : ids)
            m_engine.Lua().RecordDestroyed(id);
        return Status::Success();
    }

    std::vector<Table::TableSymbol> TableRuntime::TableSymbols() const
    {
        return m_tableSymbols;
    }

    void TableRuntime::NewTable()
    {
        FlushPendingSave();
        m_engine.CancelActivations();
        m_engine.Activation().WaitForActivation();
        m_engine.StopTableScript();
        const std::uint64_t identity = m_table.Identity();
        for (int id : m_engine.Activation().BottomToTop())
        {
            if (m_table.Identity() != identity)
                break;
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
        // ∑ Every ID of the file is noted before the first record goes in. Add notes one tree at a time, so a record that had to
        //   be renumbered (an ID given twice) used to take the next free number, which a later record of the file still had; that
        //   one moved on in turn, and one duplicate renumbered the rest of the table.
        const std::function<void(const Table::MemoryRecord&)> noteIds = [&](const Table::MemoryRecord& record)
            {
                m_table.NoteId(record.Id);
                for (const auto& child : record.Children)
                    noteIds(*child);
            };
        for (const auto& record : loaded.Records)
            noteIds(*record);
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
        // ∑ A merge adds to a table that is not saved with it: marking it clean lost
        //   the edits made before the merge (no prompt on quit, no session backup).
        if (merge)
            m_table.Touch();
        else
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

    namespace
    {
        std::wstring SaveKey(const std::filesystem::path& path)
        {
            std::error_code ignored;
            std::filesystem::path full = std::filesystem::absolute(path, ignored);
            if (full.empty())
                full = path;
            std::wstring key = full.lexically_normal().wstring();
            if (!key.empty())
                ::CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
            return key;
        }

        std::optional<std::pair<std::uint64_t, std::uint64_t>> DiskStamp(const std::filesystem::path& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                return std::nullopt;
            const std::uint64_t time = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
            const std::uint64_t size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            return std::pair{ time, size };
        }

        std::string SizeText(std::size_t bytes)
        {
            if (bytes >= 1024u * 1024u)
            {
                char text[32];
                std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
                return text;
            }
            return std::to_string((bytes + 1023) / 1024) + " KB";
        }

        std::string Milliseconds(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
        {
            return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
        }
    }

    SaveKind TableRuntime::SaveNow(const std::filesystem::path& path, double now, bool toast, bool background, Status* failure)
    {
        PROFILE_ZONE("Save table");
        FinishWrite();
        m_pendingSave.reset();
        m_lastSaveAt = now;
        const auto started = std::chrono::steady_clock::now();
        const std::string name = WideToUtf8(path.filename().wstring());
        std::string text = Table::WriteTable(m_table, m_structures, m_tableSymbols, m_engine.TableFileNaming());
        const std::size_t hash = std::hash<std::string>{}(text);
        const std::wstring key = SaveKey(path);
        const auto stamp = DiskStamp(path);
        if (m_savedFile && m_savedFile->Path == key && m_savedFile->Hash == hash && m_savedFile->Size == text.size() && stamp &&
            stamp->first == m_savedFile->DiskTime && stamp->second == m_savedFile->DiskSize)
        {
            m_table.FilePath = WideToUtf8(path.wstring());
            m_table.SetClean();
            m_lastSaveKind = SaveKind::UpToDate;
            const std::string message = name + " is up to date, nothing to save";
            LOG_INFO("Table", message + " (" + Milliseconds(started, std::chrono::steady_clock::now()) + " ms)");
            // ∑ The toast goes to the notification centre directly: Engine::Notify would log the save a second time.
            if (toast)
                m_engine.Notifications().Post(LogLevel::Info, message, now);
            return m_lastSaveKind;
        }
        const auto serialized = std::chrono::steady_clock::now();
        const std::uint64_t identity = m_table.Identity();
        const std::uint64_t changes = m_table.Changes();
        const std::size_t size = text.size();
        // Runs on the main thread: in Drain for a background write, right away for one made here.
        const auto finish = [this, path, key, name, hash, size, identity, changes, toast, started, serialized](bool ok, std::optional<std::pair<std::uint64_t, std::uint64_t>> written,
                                std::chrono::steady_clock::time_point finished, Status* failure)
            {
                m_writeId = 0;
                const double at = m_engine.Now();
                if (!ok)
                {
                    const Status status = Status::Fail(ErrorCode::TableNotSaved, WideToUtf8(path.wstring()));
                    LOG_ERROR("Table", status.Text);
                    if (toast)
                        m_engine.Notifications().Post(LogLevel::Error, status.Text, at);
                    if (failure)
                        *failure = status;
                    m_lastSaveKind = SaveKind::Failed;
                    return;
                }
                ++m_tableWrites;
                m_savedFile = SavedFile{ key, hash, size, written ? written->first : 0, written ? written->second : 0 };
                if (m_table.Identity() == identity)
                {
                    m_table.FilePath = WideToUtf8(path.wstring());
                    // ∑ A change made while the file was being written is not in it, so the table stays dirty for it.
                    if (m_table.Changes() == changes)
                        m_table.SetClean();
                }
                m_engine.Options().AddRecentTable(WideToUtf8(path.wstring()));
                LOG_SUCCESS("Table", "Saved " + name + " (" + SizeText(size) + ", " + Milliseconds(started, finished) + " ms: " + Milliseconds(started, serialized) + " ms text, " +
                    Milliseconds(serialized, finished) + " ms disk)");
                if (toast)
                    m_engine.Notifications().Post(LogLevel::Success, "Saved " + name, at);
                m_lastSaveKind = SaveKind::Written;
            };
        if (!background)
        {
            const bool ok = Table::WriteFileSafely(path, text, true);
            finish(ok, ok ? DiskStamp(path) : std::nullopt, std::chrono::steady_clock::now(), failure);
            return m_lastSaveKind;
        }
        // ∑ The owner's Isaac table lives on an SMR hard disk: building the text takes 15-30 ms, the flushed write-through
        //   write 70-290 ms. Only the write runs on a worker; the table is not touched there.
        auto shared = std::make_shared<const std::string>(std::move(text));
        WorkOptions options;
        options.Title = "Saving " + name;
        options.Group = "Table save";
        options.Cancellable = false;
        options.Quiet = true;
        m_writeId = m_engine.Work().Start(std::move(options), [path, shared, finish](WorkProgress&) -> BackgroundWork::Finish
            {
                // ∑ A job that throws ends without a finish, and the save it was would never clear
                //   m_writeId: every later Ctrl+S would only queue. It reports a failed save instead.
                try
                {
                    const bool ok = Table::WriteFileSafely(path, *shared, true);
                    const auto written = ok ? DiskStamp(path) : std::nullopt;
                    const auto finished = std::chrono::steady_clock::now();
                    return [ok, written, finished, finish]() { finish(ok, written, finished, nullptr); };
                }
                catch (...)
                {
                    const auto finished = std::chrono::steady_clock::now();
                    return [finished, finish]() { finish(false, std::nullopt, finished, nullptr); };
                }
            });
        m_writingKey = key;
        if (!m_writeId)
        {
            const bool ok = Table::WriteFileSafely(path, *shared, true);
            finish(ok, ok ? DiskStamp(path) : std::nullopt, std::chrono::steady_clock::now(), failure);
            return m_lastSaveKind;
        }
        return SaveKind::Writing;
    }

    Status TableRuntime::SaveTable(const std::filesystem::path& path, bool toast)
    {
        FinishWrite();
        if (m_pendingSave && SaveKey(*m_pendingSave) != SaveKey(path))
            FlushPendingSave();
        Status failure = Status::Success();
        SaveNow(path, m_engine.Now(), toast, false, &failure);
        return failure;
    }

    SaveKind TableRuntime::RequestSave(const std::filesystem::path& path, double now)
    {
        const std::wstring key = SaveKey(path);
        if (m_pendingSave)
        {
            if (SaveKey(*m_pendingSave) == key)
                return SaveKind::Queued;
            FlushPendingSave();
        }
        if (m_writeId)
        {
            if (m_writingKey != key)
                FinishWrite();
            else
            {
                m_pendingSave = path;
                m_pendingDue = std::max(now, m_lastSaveAt + kSaveCooldownSeconds);
                return SaveKind::Queued;
            }
        }
        // ∑ The owner's Log showed a save every 200-450 ms (Ctrl+S pressed again and again on the Isaac table). Within the
        //   cooldown a request only marks a save for when it ends, so a burst of presses costs one save at the start and at
        //   most one at the end; the queued save takes the table as it is then, so nothing done in between is left out.
        if (m_savedFile && m_savedFile->Path == key && m_lastSaveAt >= 0.0 && now >= m_lastSaveAt && now - m_lastSaveAt < kSaveCooldownSeconds)
        {
            m_pendingSave = path;
            m_pendingDue = m_lastSaveAt + kSaveCooldownSeconds;
            return SaveKind::Queued;
        }
        return SaveNow(path, now, true, true);
    }

    void TableRuntime::TickSaves(double now)
    {
        if (m_pendingSave && !m_writeId && now >= m_pendingDue)
        {
            const std::filesystem::path path = *m_pendingSave;
            SaveNow(path, now, true, true);
        }
    }

    void TableRuntime::FinishWrite()
    {
        if (m_writeId)
            m_engine.Work().Complete(m_writeId);
        m_writeId = 0;
    }

    void TableRuntime::FlushPendingSave()
    {
        FinishWrite();
        if (!m_pendingSave)
            return;
        const std::filesystem::path path = *m_pendingSave;
        SaveNow(path, m_engine.Now(), true, false);
    }
}
