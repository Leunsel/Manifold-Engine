#pragma once

#include "Disasm/Signature.h"

namespace Manifold
{
    struct Settings;

    Disasm::SignatureOptions SignatureOptionsFrom(const Settings& settings);
    Disasm::SignatureFormatting SignatureFormattingFrom(const Settings& settings);
    Disasm::SignatureFormat SignatureCopyFormat(const Settings& settings);
}
