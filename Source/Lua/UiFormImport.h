#pragma once

#include "Lua/UiForm.h"
#include "Table/CeForms.h"

#include <string>
#include <vector>

namespace Manifold::Lua
{
    struct FormImport
    {
        FormDocument Form;
        std::vector<std::string> Notes;
        int Converted = 0;
    };

    // Turns a Cheat Engine form (TCEForm and its controls) into a manifold.ui form. Pixel
    // positions become rows, label and field pairs become grids, Align layouts become rows,
    // columns and splits; event names become handler names. What has no counterpart is
    // listed in Notes.
    FormImport FormFromCe(const Table::CeComponent& root);
}
