#pragma once

#include "Table/AddressList.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Table
{
    // The comments and headers the user put on disassembler lines (AddressList::DisasmNotes), for the Comment List window.

    struct NoteKey
    {
        std::string Module;        // empty for a plain address
        std::uint64_t Offset = 0;  // offset into the module, or the address
        bool Parsed = false;       // false for keys in neither form; they sort last, by text
    };

    // Splits a key as Symbols::ModuleOffset writes it: "game.exe"+1A2B, or a plain hex address.
    NoteKey ParseNoteKey(std::string_view key);

    struct RemovedNote
    {
        std::string Key;
        AddressList::DisasmNote Note;
    };

    // Keys of the notes that have text, by module (plain addresses first), then offset. A filter matches the key, the
    // comment or the header, ignoring case.
    std::vector<std::string> SortedNoteKeys(const AddressList& list, std::string_view filter = {});

    // Removes the notes and returns them for RestoreNotes; marks the table changed when any was there.
    std::vector<RemovedNote> RemoveNotes(AddressList& list, const std::vector<std::string>& keys);

    // Puts removed notes back where the key is free again (a note added there since wins) and returns how many came back.
    std::size_t RestoreNotes(AddressList& list, const std::vector<RemovedNote>& removed);
}
