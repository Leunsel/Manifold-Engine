#include "App/SignatureSettings.h"

#include "App/Settings.h"
#include "Core/Text.h"

#include <algorithm>

namespace Manifold
{
    namespace
    {
        Disasm::MaskMode Mode(int value)
        {
            return static_cast<Disasm::MaskMode>(std::clamp(value, 0, 2));
        }
    }

    Disasm::SignatureOptions SignatureOptionsFrom(const Settings& settings)
    {
        Disasm::SignatureOptions options;
        options.Scope = settings.SignatureScope == 1 ? Disasm::SignatureScope::Process : Disasm::SignatureScope::Module;
        options.CodeOnly = settings.SignatureCodeOnly;
        options.MaskAddresses = settings.SignatureMaskAddresses;
        options.Branches = Mode(settings.SignatureBranches);
        options.Offsets = Mode(settings.SignatureOffsets);
        options.Immediates = Mode(settings.SignatureImmediates);
        options.ImmediateThreshold = ParseHex(Trim(settings.SignatureImmediateThreshold)).value_or(0x10000);
        options.MaxBytes = std::clamp(settings.SignatureMaxBytes, 8, 1024);
        options.MinBytes = std::clamp(settings.SignatureMinBytes, 0, 256);
        options.StopAtFunctionEnd = settings.SignatureStopAtFunctionEnd;
        options.AllowOffset = settings.SignatureOffset;
        options.MaxOffset = std::clamp(settings.SignatureMaxOffset, 1, 512);
        options.MinSaving = std::clamp(settings.SignatureMinSaving, 0, 64);
        return options;
    }

    Disasm::SignatureFormatting SignatureFormattingFrom(const Settings& settings)
    {
        Disasm::SignatureFormatting formatting;
        formatting.Wildcard = static_cast<Disasm::SignatureWildcard>(std::clamp(settings.SignatureWildcard, 0, 3));
        formatting.Spaces = settings.SignatureSpaces;
        formatting.Label = Trim(settings.SignatureLabel).empty() ? std::string("INJECT") : Trim(settings.SignatureLabel);
        return formatting;
    }

    Disasm::SignatureFormat SignatureCopyFormat(const Settings& settings)
    {
        return static_cast<Disasm::SignatureFormat>(std::clamp(settings.SignatureCopyFormat, 0, 3));
    }
}
