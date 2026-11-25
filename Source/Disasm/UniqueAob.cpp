#include "Disasm/UniqueAob.h"

#include "Disasm/Signature.h"
#include "Target/Process.h"

#include <algorithm>

namespace Manifold::Disasm
{
    // ∑ getUniqueAOB is the signature maker's measurement with its default masking. The old search
    //   scanned seeds of 8, 16, 32 and all bytes and gave up when a seed had 64 hits or more, so it
    //   found nothing for the SEH prologue (55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00) that
    //   thousands of x86 MSVC functions share: Isaac gave nil for 10 of 18 functions that the signature
    //   maker makes unique in 11 to 99 bytes. The measurement has no hit limit.
    //   Unique means unique in all readable memory, because that is what AOBScan without flags searches;
    //   the old search counted only executable memory, so a copy of the bytes in data could come first.
    std::optional<UniqueAob> FindUniqueAob(const Target::Process& process, const Disassembler& disassembler, std::uint64_t address, const UniqueAobOptions& options)
    {
        const auto region = process.RegionAt(address);
        if (!region || !region->Committed() || !region->Readable())
            return std::nullopt;
        SignatureOptions signatureOptions;
        signatureOptions.Scope = options.WholeProcess ? SignatureScope::Process : SignatureScope::Module;
        signatureOptions.CodeOnly = options.CodeOnly;
        signatureOptions.MaxBytes = static_cast<int>(std::clamp<std::size_t>(options.MaxLength, 8, 256));
        signatureOptions.MinBytes = 0;
        signatureOptions.AllowOffset = options.BackSteps > 0;
        signatureOptions.MaxOffset = options.MaxOffset;
        std::string error;
        const std::optional<Signature> signature = MakeSignature(process, disassembler, address, signatureOptions, error);
        if (!signature || !signature->Verified || signature->Bytes.empty())
            return std::nullopt;
        UniqueAob found;
        found.Start = signature->Start;
        found.Offset = static_cast<int>(signature->Offset);
        for (const SignatureByte& byte : signature->Bytes)
        {
            found.Bytes.push_back(byte.Wild ? 0 : byte.Value);
            found.Mask.push_back(byte.Wild ? 0x00 : 0xFF);
        }
        found.Text = SignatureText(signature->Bytes, SignatureFormatting{});
        return found;
    }
}
