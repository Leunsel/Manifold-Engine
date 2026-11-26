#include "App/ObscuredTypes.h"

#include "Lua/LuaEngine.h"
#include "Scan/Obscured.h"

#include <random>

namespace Manifold
{
    void RegisterObscuredTypes(Lua::LuaEngine& lua, int pointerSize)
    {
        for (const Scan::ObscuredType& obscured : Scan::ObscuredTypes())
        {
            const Scan::ObscuredType* type = &obscured;
            const Scan::ObscuredLayout layout = Scan::ObscuredLayoutOf(type->Kind, pointerSize);
            Lua::CustomType custom;
            custom.Name = type->Name;
            custom.Float = Scan::IsFloating(type->Plain) && Scan::ObscuredScalar(*type);
            custom.String = !Scan::ObscuredScalar(*type);
            custom.SetByteCount(layout.Size);
            custom.NativeText = [type, layout](std::span<const std::uint8_t> bytes) -> std::optional<std::string>
                {
                    if (static_cast<int>(bytes.size()) < layout.Size)
                        return std::nullopt;
                    std::uint8_t plain[16]{};
                    if (Scan::ObscuredDecode(*type, layout, bytes.data(), plain, false) == Scan::ObscuredState::Invalid)
                        return std::nullopt;
                    return Scan::ObscuredText(*type, std::span<const std::uint8_t>(plain, static_cast<std::size_t>(Scan::ObscuredPlainSize(*type))));
                };
            custom.NativeUpdate = [type, layout](std::string_view text, std::span<const std::uint8_t> current) -> std::optional<std::vector<std::uint8_t>>
                {
                    const auto plain = Scan::ObscuredParse(*type, text);
                    if (!plain)
                        return std::nullopt;
                    std::uint8_t decoded[16]{};
                    if (static_cast<int>(current.size()) >= layout.Size && Scan::ObscuredDecode(*type, layout, current.data(), decoded, false) == Scan::ObscuredState::Invalid)
                        return std::nullopt;
                    static thread_local std::mt19937_64 random{ std::random_device{}() };
                    return Scan::ObscuredEncode(*type, layout, current, *plain, random());
                };
            lua.RegisterCustomType(std::move(custom));
        }
    }
}
