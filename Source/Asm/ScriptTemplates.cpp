#include "Asm/AutoAssembler.h"

namespace Manifold::Asm
{
    std::string CheatTableTemplate()
    {
        return "[ENABLE]\n"
               "// code from here to [DISABLE] runs when the script is switched on\n"
               "\n"
               "[DISABLE]\n"
               "// code from here to the end runs when the script is switched off\n";
    }
}
