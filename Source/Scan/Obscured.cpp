#include "Scan/Obscured.h"

#include "Core/Text.h"

#include <algorithm>
#include <cstring>

namespace Manifold::Scan
{
    namespace
    {
        constexpr ObscuredType kTypes[] = {
            { ObscuredKind::Int, "ACTk ObscuredInt", "ObscuredInt", ValueType::FourBytes, 1, true },
            { ObscuredKind::UInt, "ACTk ObscuredUInt", "ObscuredUInt", ValueType::FourBytes, 1, false },
            { ObscuredKind::Long, "ACTk ObscuredLong", "ObscuredLong", ValueType::EightBytes, 1, true },
            { ObscuredKind::ULong, "ACTk ObscuredULong", "ObscuredULong", ValueType::EightBytes, 1, false },
            { ObscuredKind::Short, "ACTk ObscuredShort", "ObscuredShort", ValueType::TwoBytes, 1, true },
            { ObscuredKind::UShort, "ACTk ObscuredUShort", "ObscuredUShort", ValueType::TwoBytes, 1, false },
            { ObscuredKind::Byte, "ACTk ObscuredByte", "ObscuredByte", ValueType::Byte, 1, false },
            { ObscuredKind::SByte, "ACTk ObscuredSByte", "ObscuredSByte", ValueType::Byte, 1, true },
            { ObscuredKind::Float, "ACTk ObscuredFloat", "ObscuredFloat", ValueType::Float, 1, true },
            { ObscuredKind::Double, "ACTk ObscuredDouble", "ObscuredDouble", ValueType::Double, 1, true },
            { ObscuredKind::Bool, "ACTk ObscuredBool", "ObscuredBool", ValueType::Byte, 1, false },
            { ObscuredKind::Vector2, "ACTk ObscuredVector2", "ObscuredVector2", ValueType::Float, 2, true },
            { ObscuredKind::Vector3, "ACTk ObscuredVector3", "ObscuredVector3", ValueType::Float, 3, true },
            { ObscuredKind::Quaternion, "ACTk ObscuredQuaternion", "ObscuredQuaternion", ValueType::Float, 4, true },
        };

        // ∑ ACTk's bool is no 0/1 under the key: true is 213 and false 181 (0xB5 + (value << 5)),
        //   read back as "anything but 181", in ACTk 2.x's Encrypt/Decrypt of ObscuredBool (a 64-bit IL2CPP game).
        constexpr std::uint32_t kBoolFalse = 0xB5;
        constexpr std::uint32_t kBoolTrue = 0xD5;

        int AlignUp(int value, int alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        std::uint64_t LoadBytes(const std::uint8_t* data, int size)
        {
            std::uint64_t value = 0;
            std::memcpy(&value, data, static_cast<std::size_t>(std::clamp(size, 0, 8)));
            return value;
        }

        void StoreBytes(std::uint8_t* data, std::uint64_t value, int size)
        {
            std::memcpy(data, &value, static_cast<std::size_t>(std::clamp(size, 0, 8)));
        }

        std::uint64_t Mask(int bytes)
        {
            return bytes >= 8 ? ~0ull : (1ull << (bytes * 8)) - 1;
        }

        ObscuredLayout Simple(int width)
        {
            ObscuredLayout layout;
            layout.Key = 0;
            layout.KeyBytes = width;
            layout.Hidden = width;
            layout.HiddenBytes = width;
            layout.Inited = 2 * width;
            layout.Fake = AlignUp(layout.Inited + 1, width);
            layout.FakeActive = layout.Fake + width;
            layout.Size = AlignUp(layout.FakeActive + 1, width);
            return layout;
        }

        ObscuredLayout Vector(int components)
        {
            ObscuredLayout layout;
            layout.KeyBytes = 4;
            layout.Hidden = 4;
            layout.HiddenBytes = 4 * components;
            layout.Inited = layout.Hidden + layout.HiddenBytes;
            layout.Fake = AlignUp(layout.Inited + 1, 4);
            layout.FakeActive = layout.Fake + 4 * components;
            layout.Size = AlignUp(layout.FakeActive + 1, 4);
            return layout;
        }

        ObscuredLayout WithOld(int width, int pointerSize)
        {
            const int pointer = pointerSize == 4 ? 4 : 8;
            ObscuredLayout layout;
            layout.KeyBytes = width;
            layout.Hidden = width;
            layout.HiddenBytes = width;
            layout.Old = AlignUp(2 * width, pointer);
            layout.OldBytes = pointer;
            layout.Inited = layout.Old + pointer;
            layout.Fake = AlignUp(layout.Inited + 1, width);
            layout.FakeActive = layout.Fake + width;
            layout.Size = AlignUp(layout.FakeActive + 1, std::max(width, pointer));
            return layout;
        }

        bool Flag(std::uint8_t value)
        {
            return value == 0 || value == 1;
        }

        // ∑ The game zeroes the struct before it fills it (op_Implicit of ObscuredInt clears a stack copy with
        //   movdqu in a 64-bit IL2CPP game), so the gaps between the fields are 0 in real values: D7 00 00 00 | 02 00 00 00 |
        //   01 01 01 00 for a true ObscuredBool. Ordinary memory that happens to decode rarely keeps them 0.
        bool PaddingClear(const ObscuredLayout& layout, int plainSize, const std::uint8_t* block)
        {
            bool used[64]{};
            const auto mark = [&](int at, int count)
                {
                    for (int i = at; i < at + count && i < 64; ++i)
                    {
                        if (i >= 0)
                            used[i] = true;
                    }
                };
            mark(layout.Key, layout.KeyBytes);
            mark(layout.Hidden, layout.HiddenBytes);
            mark(layout.Old, layout.OldBytes);
            mark(layout.Inited, 1);
            mark(layout.Fake, plainSize);
            mark(layout.FakeActive, 1);
            for (int i = 0; i < layout.Size && i < 64; ++i)
            {
                if (!used[i] && block[i] != 0)
                    return false;
            }
            return true;
        }
    }

    std::span<const ObscuredType> ObscuredTypes()
    {
        return kTypes;
    }

    const ObscuredType* FindObscured(std::string_view customName)
    {
        for (const ObscuredType& type : kTypes)
        {
            if (EqualsNoCase(type.Name, customName))
                return &type;
        }
        return nullptr;
    }

    const ObscuredType* FindObscuredClass(std::string_view managedName)
    {
        const std::string_view prefix = kObscuredNamespace;
        if (managedName.size() > prefix.size() + 1 && managedName.substr(0, prefix.size()) == prefix && managedName[prefix.size()] == '.')
            managedName.remove_prefix(prefix.size() + 1);
        else if (managedName.find('.') != std::string_view::npos)
            return nullptr;
        for (const ObscuredType& type : kTypes)
        {
            if (managedName == type.Class)
                return &type;
        }
        return nullptr;
    }

    ObscuredLayout ObscuredLayoutOf(ObscuredKind kind, int pointerSize)
    {
        switch (kind)
        {
        case ObscuredKind::Int:
        case ObscuredKind::UInt: return Simple(4);
        case ObscuredKind::Long:
        case ObscuredKind::ULong: return Simple(8);
        case ObscuredKind::Short:
        case ObscuredKind::UShort: return Simple(2);
        case ObscuredKind::Byte:
        case ObscuredKind::SByte: return Simple(1);
        case ObscuredKind::Float: return WithOld(4, pointerSize);
        case ObscuredKind::Double: return WithOld(8, pointerSize);
        case ObscuredKind::Bool:
        {
            ObscuredLayout layout;
            layout.KeyBytes = 1;
            layout.Hidden = 4;
            layout.HiddenBytes = 4;
            layout.Inited = 8;
            layout.Fake = 9;
            layout.FakeActive = 10;
            layout.Size = 12;
            return layout;
        }
        case ObscuredKind::Vector2: return Vector(2);
        case ObscuredKind::Vector3: return Vector(3);
        case ObscuredKind::Quaternion: return Vector(4);
        }
        return {};
    }

    int ObscuredComponentSize(const ObscuredType& type)
    {
        return ValueSize(type.Plain, ValueFormat{});
    }

    int ObscuredPlainSize(const ObscuredType& type)
    {
        return ObscuredComponentSize(type) * type.Components;
    }

    bool ObscuredScalar(const ObscuredType& type)
    {
        return type.Components == 1;
    }

    ObscuredState ObscuredDecode(const ObscuredType& type, const ObscuredLayout& layout, const std::uint8_t* block, std::uint8_t* plain, bool strict)
    {
        const int plainSize = ObscuredPlainSize(type);
        const std::uint8_t inited = block[layout.Inited];
        const std::uint8_t fakeActive = block[layout.FakeActive];
        if (!Flag(inited) || !Flag(fakeActive))
            return ObscuredState::Invalid;
        const std::uint64_t key = LoadBytes(block + layout.Key, layout.KeyBytes);
        if (!inited)
        {
            if (strict)
                return ObscuredState::Invalid;
            std::memset(plain, 0, static_cast<std::size_t>(plainSize));
            return ObscuredState::Unset;
        }
        // ∑ An assigned value always has a key: ACTk draws a new one until it is not 0, and 0 would mean the
        //   class's static key, which is nowhere in the struct.
        if (key == 0)
            return ObscuredState::Invalid;
        if (strict && !PaddingClear(layout, plainSize, block))
            return ObscuredState::Invalid;
        // ∑ A hidden value of 0 means the value equals its key, which real data never showed but memory like
        //   0E 00.. 00 00.. 01 .. 0E .. 01 (a long "14" in a live game) does all the time. Bytes keep it: 1 in 256.
        if (strict && type.Kind != ObscuredKind::Bool && ObscuredComponentSize(type) >= 2)
        {
            bool zero = true;
            for (int i = 0; i < layout.HiddenBytes && zero; ++i)
                zero = block[layout.Hidden + i] == 0;
            if (zero)
                return ObscuredState::Invalid;
        }
        if (type.Kind == ObscuredKind::Bool)
        {
            const std::uint32_t raw = static_cast<std::uint32_t>(LoadBytes(block + layout.Hidden, 4)) ^ static_cast<std::uint32_t>(key);
            if (strict && raw != kBoolFalse && raw != kBoolTrue)
                return ObscuredState::Invalid;
            plain[0] = raw != kBoolFalse ? 1 : 0;
            if (strict && fakeActive && !Flag(block[layout.Fake]))
                return ObscuredState::Invalid;
        }
        else
        {
            const int width = ObscuredComponentSize(type);
            for (int i = 0; i < type.Components; ++i)
            {
                const std::uint64_t hidden = LoadBytes(block + layout.Hidden + i * width, width);
                StoreBytes(plain + i * width, (hidden ^ key) & Mask(width), width);
            }
        }
        if (strict && fakeActive && std::memcmp(block + layout.Fake, plain, static_cast<std::size_t>(plainSize)) != 0)
            return ObscuredState::Invalid;
        return ObscuredState::Plain;
    }

    std::vector<std::uint8_t> ObscuredEncode(const ObscuredType& type, const ObscuredLayout& layout, std::span<const std::uint8_t> current,
        std::span<const std::uint8_t> plain, std::uint64_t freshKey)
    {
        std::vector<std::uint8_t> out(static_cast<std::size_t>(layout.Size), 0);
        std::memcpy(out.data(), current.data(), std::min(current.size(), out.size()));
        std::uint64_t key = LoadBytes(out.data() + layout.Key, layout.KeyBytes);
        if (out[static_cast<std::size_t>(layout.Inited)] != 1 || key == 0)
        {
            key = freshKey & Mask(layout.KeyBytes);
            if (key == 0)
                key = 0x5A & Mask(layout.KeyBytes);
            StoreBytes(out.data() + layout.Key, key, layout.KeyBytes);
            out[static_cast<std::size_t>(layout.Inited)] = 1;
            out[static_cast<std::size_t>(layout.FakeActive)] = 0;
        }
        const int plainSize = ObscuredPlainSize(type);
        std::vector<std::uint8_t> value(static_cast<std::size_t>(plainSize), 0);
        std::memcpy(value.data(), plain.data(), std::min(plain.size(), value.size()));
        if (type.Kind == ObscuredKind::Bool)
        {
            value[0] = value[0] ? 1 : 0;
            StoreBytes(out.data() + layout.Hidden, (value[0] ? kBoolTrue : kBoolFalse) ^ static_cast<std::uint32_t>(key), 4);
        }
        else
        {
            const int width = ObscuredComponentSize(type);
            for (int i = 0; i < type.Components; ++i)
                StoreBytes(out.data() + layout.Hidden + i * width, (LoadBytes(value.data() + i * width, width) ^ key) & Mask(width), width);
        }
        if (out[static_cast<std::size_t>(layout.FakeActive)] == 1)
            std::memcpy(out.data() + layout.Fake, value.data(), value.size());
        if (layout.Old >= 0)
            std::memset(out.data() + layout.Old, 0, static_cast<std::size_t>(layout.OldBytes));
        return out;
    }

    std::string ObscuredText(const ObscuredType& type, std::span<const std::uint8_t> plain)
    {
        if (static_cast<int>(plain.size()) < ObscuredPlainSize(type))
            return "??";
        if (type.Kind == ObscuredKind::Bool)
            return plain[0] ? "True" : "False";
        ValueFormat format;
        format.Signed = type.Signed;
        const int width = ObscuredComponentSize(type);
        std::string text;
        for (int i = 0; i < type.Components; ++i)
        {
            if (i)
                text += ", ";
            text += FormatValue(type.Plain, plain.subspan(static_cast<std::size_t>(i * width), static_cast<std::size_t>(width)), format);
        }
        return text;
    }

    std::optional<std::vector<std::uint8_t>> ObscuredParse(const ObscuredType& type, std::string_view text)
    {
        text = TrimView(text);
        if (type.Kind == ObscuredKind::Bool)
        {
            if (EqualsNoCase(text, "true") || text == "1")
                return std::vector<std::uint8_t>{ 1 };
            if (EqualsNoCase(text, "false") || text == "0")
                return std::vector<std::uint8_t>{ 0 };
            return std::nullopt;
        }
        ValueFormat format;
        format.Signed = type.Signed;
        if (type.Components == 1)
            return ParseValue(type.Plain, text, format);
        const std::vector<std::string> parts = Split(text, ',');
        if (static_cast<int>(parts.size()) != type.Components)
            return std::nullopt;
        std::vector<std::uint8_t> out;
        for (const std::string& part : parts)
        {
            const auto component = ParseValue(type.Plain, part, format);
            if (!component)
                return std::nullopt;
            out.insert(out.end(), component->begin(), component->end());
        }
        return out;
    }
}
