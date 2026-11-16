#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace Manifold::Debug::Program
{
    constexpr int kMaxCode = 240;
    constexpr int kMaxChanges = 8;
    constexpr int kMaxStack = 16;
    constexpr int kRegisterCount = 18;
    constexpr std::uint8_t kRip = 16;
    constexpr std::uint8_t kFlags = 17;
    constexpr std::uint8_t kFlagTarget = 0x40;

    enum Op : std::uint8_t
    {
        OpEnd = 0,
        OpInt,
        OpInt32,
        OpFloat,
        OpRegister,
        OpFlag,
        OpThread,
        OpRead,
        OpNeg,
        OpBitNot,
        OpNot,
        OpAdd,
        OpSub,
        OpMul,
        OpDiv,
        OpIntDiv,
        OpMod,
        OpBitAnd,
        OpBitOr,
        OpBitXor,
        OpShl,
        OpShr,
        OpEq,
        OpNe,
        OpLt,
        OpLe,
        OpGt,
        OpGe,
        OpAnd,
        OpOr
    };

    enum Part : std::uint8_t
    {
        PartFull = 0,
        PartLow32,
        PartLow16,
        PartLow8,
        PartHigh8
    };

    enum ReadKind : std::uint8_t
    {
        ReadU8 = 0x01,
        ReadU16 = 0x02,
        ReadU32 = 0x04,
        ReadU64 = 0x08,
        ReadS8 = 0x11,
        ReadS16 = 0x12,
        ReadS32 = 0x14,
        ReadF32 = 0x24,
        ReadF64 = 0x28
    };

    struct Value
    {
        std::int64_t Int = 0;
        double Float = 0.0;
        bool IsFloat = false;
    };

    struct Change
    {
        std::uint8_t Target;
        std::uint8_t Part;
        std::uint8_t Length;
        std::uint8_t Pad;
        std::uint8_t Code[kMaxCode];
    };

    inline double AsFloat(const Value& value)
    {
        return value.IsFloat ? value.Float : static_cast<double>(value.Int);
    }

    inline std::int64_t AsInt(const Value& value)
    {
        if (!value.IsFloat)
            return value.Int;
        if (!(value.Float == value.Float) || value.Float >= 9.2233720368547758e18 || value.Float <= -9.2233720368547758e18)
            return 0;
        return static_cast<std::int64_t>(value.Float);
    }

    inline bool Truthy(const Value& value)
    {
        return value.IsFloat ? value.Float != 0.0 : value.Int != 0;
    }

    inline Value MakeInt(std::int64_t value)
    {
        Value out;
        out.Int = value;
        return out;
    }

    inline Value MakeFloat(double value)
    {
        Value out;
        out.Float = value;
        out.IsFloat = true;
        return out;
    }

    inline Value MakeBool(bool value)
    {
        return MakeInt(value ? 1 : 0);
    }

    inline std::uint64_t ReadPart(std::uint64_t value, std::uint8_t part)
    {
        switch (part)
        {
        case PartLow32: return value & 0xFFFFFFFFull;
        case PartLow16: return value & 0xFFFFull;
        case PartLow8: return value & 0xFFull;
        case PartHigh8: return (value >> 8) & 0xFFull;
        default: return value;
        }
    }

    inline std::uint64_t WritePart(std::uint64_t old, std::uint8_t part, std::uint64_t value)
    {
        switch (part)
        {
        case PartLow32: return value & 0xFFFFFFFFull;
        case PartLow16: return (old & ~0xFFFFull) | (value & 0xFFFFull);
        case PartLow8: return (old & ~0xFFull) | (value & 0xFFull);
        case PartHigh8: return (old & ~0xFF00ull) | ((value & 0xFFull) << 8);
        default: return value;
        }
    }

    inline std::int64_t Wrap(std::uint64_t value)
    {
        std::int64_t out;
        std::memcpy(&out, &value, sizeof(out));
        return out;
    }

    inline std::int64_t FloorDivide(std::int64_t a, std::int64_t b)
    {
        if (b == -1)
            return Wrap(0ull - static_cast<std::uint64_t>(a));
        std::int64_t quotient = a / b;
        if ((a % b != 0) && ((a < 0) != (b < 0)))
            --quotient;
        return quotient;
    }

    inline std::int64_t FloorModulo(std::int64_t a, std::int64_t b)
    {
        if (b == -1)
            return 0;
        std::int64_t rest = a % b;
        if (rest != 0 && ((rest < 0) != (b < 0)))
            rest += b;
        return rest;
    }

    inline std::int64_t ShiftLeft(std::int64_t a, std::int64_t b)
    {
        const std::uint64_t value = static_cast<std::uint64_t>(a);
        if (b <= -64 || b >= 64)
            return 0;
        if (b < 0)
            return Wrap(value >> static_cast<unsigned>(-b));
        return Wrap(value << static_cast<unsigned>(b));
    }

    // Lua 5.4 rules where a condition can tell the difference: integers stay
    // 64-bit and wrap, "/" always gives a float, "//" and "%" floor, shifts
    // are logical. Truth is C-like (zero is false) so "RAX and RBX" means both
    // non-zero; a failed memory read makes the whole program fail, which the
    // callers count as "condition false" and "change skipped".
    template <typename Reader>
    bool Run(const std::uint8_t* code, std::uint32_t length, const std::uint64_t* registers, std::uint32_t threadId, Reader& read, Value& result)
    {
        Value stack[kMaxStack];
        int top = 0;
        std::uint32_t at = 0;
        while (at < length)
        {
            const std::uint8_t op = code[at++];
            if (op == OpEnd)
                break;
            switch (op)
            {
            case OpInt:
            case OpFloat:
            {
                if (at + 8 > length || top >= kMaxStack)
                    return false;
                if (op == OpInt)
                {
                    std::int64_t value;
                    std::memcpy(&value, code + at, 8);
                    stack[top++] = MakeInt(value);
                }
                else
                {
                    double value;
                    std::memcpy(&value, code + at, 8);
                    stack[top++] = MakeFloat(value);
                }
                at += 8;
                break;
            }
            case OpInt32:
            {
                if (at + 4 > length || top >= kMaxStack)
                    return false;
                std::int32_t value;
                std::memcpy(&value, code + at, 4);
                stack[top++] = MakeInt(value);
                at += 4;
                break;
            }
            case OpRegister:
            {
                if (at + 2 > length || top >= kMaxStack || code[at] >= kRegisterCount)
                    return false;
                stack[top++] = MakeInt(Wrap(ReadPart(registers[code[at]], code[at + 1])));
                at += 2;
                break;
            }
            case OpFlag:
            {
                if (at + 1 > length || top >= kMaxStack || code[at] >= 32)
                    return false;
                stack[top++] = MakeInt(static_cast<std::int64_t>((registers[kFlags] >> code[at]) & 1ull));
                at += 1;
                break;
            }
            case OpThread:
            {
                if (top >= kMaxStack)
                    return false;
                stack[top++] = MakeInt(static_cast<std::int64_t>(threadId));
                break;
            }
            case OpRead:
            {
                if (at + 1 > length || top < 1)
                    return false;
                const std::uint8_t kind = code[at++];
                const std::uint32_t size = kind & 0x0Fu;
                if (size != 1 && size != 2 && size != 4 && size != 8)
                    return false;
                std::uint8_t bytes[8] = {};
                if (!read(static_cast<std::uint64_t>(AsInt(stack[top - 1])), bytes, size))
                    return false;
                std::uint64_t raw = 0;
                std::memcpy(&raw, bytes, size);
                Value& slot = stack[top - 1];
                switch (kind)
                {
                case ReadS8: slot = MakeInt(static_cast<std::int8_t>(raw)); break;
                case ReadS16: slot = MakeInt(static_cast<std::int16_t>(raw)); break;
                case ReadS32: slot = MakeInt(static_cast<std::int32_t>(raw)); break;
                case ReadF32:
                {
                    float value;
                    std::memcpy(&value, bytes, 4);
                    slot = MakeFloat(value);
                    break;
                }
                case ReadF64:
                {
                    double value;
                    std::memcpy(&value, bytes, 8);
                    slot = MakeFloat(value);
                    break;
                }
                default: slot = MakeInt(Wrap(raw)); break;
                }
                break;
            }
            case OpNeg:
            case OpBitNot:
            case OpNot:
            {
                if (top < 1)
                    return false;
                Value& slot = stack[top - 1];
                if (op == OpNot)
                    slot = MakeBool(!Truthy(slot));
                else if (op == OpBitNot)
                    slot = MakeInt(Wrap(~static_cast<std::uint64_t>(AsInt(slot))));
                else if (slot.IsFloat)
                    slot = MakeFloat(-slot.Float);
                else
                    slot = MakeInt(Wrap(0ull - static_cast<std::uint64_t>(slot.Int)));
                break;
            }
            default:
            {
                if (op < OpAdd || op > OpOr || top < 2)
                    return false;
                const Value b = stack[--top];
                const Value a = stack[top - 1];
                Value& out = stack[top - 1];
                const bool floats = a.IsFloat || b.IsFloat;
                const std::uint64_t ua = static_cast<std::uint64_t>(AsInt(a));
                const std::uint64_t ub = static_cast<std::uint64_t>(AsInt(b));
                switch (op)
                {
                case OpAdd: out = floats ? MakeFloat(AsFloat(a) + AsFloat(b)) : MakeInt(Wrap(ua + ub)); break;
                case OpSub: out = floats ? MakeFloat(AsFloat(a) - AsFloat(b)) : MakeInt(Wrap(ua - ub)); break;
                case OpMul: out = floats ? MakeFloat(AsFloat(a) * AsFloat(b)) : MakeInt(Wrap(ua * ub)); break;
                case OpDiv: out = MakeFloat(AsFloat(a) / AsFloat(b)); break;
                case OpIntDiv:
                    if (floats)
                        out = MakeFloat(std::floor(AsFloat(a) / AsFloat(b)));
                    else if (b.Int == 0)
                        return false;
                    else
                        out = MakeInt(FloorDivide(a.Int, b.Int));
                    break;
                case OpMod:
                    if (floats)
                    {
                        const double x = AsFloat(a);
                        const double y = AsFloat(b);
                        out = MakeFloat(x - std::floor(x / y) * y);
                    }
                    else if (b.Int == 0)
                        return false;
                    else
                        out = MakeInt(FloorModulo(a.Int, b.Int));
                    break;
                case OpBitAnd: out = MakeInt(Wrap(ua & ub)); break;
                case OpBitOr: out = MakeInt(Wrap(ua | ub)); break;
                case OpBitXor: out = MakeInt(Wrap(ua ^ ub)); break;
                case OpShl: out = MakeInt(ShiftLeft(AsInt(a), AsInt(b))); break;
                case OpShr: out = MakeInt(ShiftLeft(AsInt(a), Wrap(0ull - ub))); break;
                case OpEq: out = MakeBool(floats ? AsFloat(a) == AsFloat(b) : a.Int == b.Int); break;
                case OpNe: out = MakeBool(floats ? AsFloat(a) != AsFloat(b) : a.Int != b.Int); break;
                case OpLt: out = MakeBool(floats ? AsFloat(a) < AsFloat(b) : a.Int < b.Int); break;
                case OpLe: out = MakeBool(floats ? AsFloat(a) <= AsFloat(b) : a.Int <= b.Int); break;
                case OpGt: out = MakeBool(floats ? AsFloat(a) > AsFloat(b) : a.Int > b.Int); break;
                case OpGe: out = MakeBool(floats ? AsFloat(a) >= AsFloat(b) : a.Int >= b.Int); break;
                case OpAnd: out = MakeBool(Truthy(a) && Truthy(b)); break;
                case OpOr: out = MakeBool(Truthy(a) || Truthy(b)); break;
                default: return false;
                }
                break;
            }
            }
        }
        if (top != 1)
            return false;
        result = stack[0];
        return true;
    }

    template <typename Reader>
    bool Holds(const std::uint8_t* code, std::uint32_t length, const std::uint64_t* registers, std::uint32_t threadId, Reader& read)
    {
        if (length == 0)
            return true;
        Value value;
        return Run(code, length, registers, threadId, read, value) && Truthy(value);
    }

    inline void Store(std::uint64_t* registers, std::uint8_t target, std::uint8_t part, const Value& value)
    {
        const std::uint64_t raw = static_cast<std::uint64_t>(AsInt(value));
        if (target >= kFlagTarget)
        {
            const unsigned bit = static_cast<unsigned>(target - kFlagTarget) & 31u;
            if (raw != 0)
                registers[kFlags] |= 1ull << bit;
            else
                registers[kFlags] &= ~(1ull << bit);
            return;
        }
        if (target < kRegisterCount)
            registers[target] = WritePart(registers[target], part, raw);
    }

    // Every right-hand side sees the registers as they were at the hit, so
    // "RAX = RBX; RBX = RAX" swaps them.
    template <typename Reader>
    int Apply(const Change* changes, int count, std::uint64_t* registers, std::uint32_t threadId, Reader& read)
    {
        if (count <= 0)
            return 0;
        if (count > kMaxChanges)
            count = kMaxChanges;
        std::uint64_t before[kRegisterCount];
        std::memcpy(before, registers, sizeof(before));
        Value values[kMaxChanges];
        bool ok[kMaxChanges] = {};
        for (int i = 0; i < count; ++i)
            ok[i] = changes[i].Length <= kMaxCode && Run(changes[i].Code, changes[i].Length, before, threadId, read, values[i]);
        int applied = 0;
        for (int i = 0; i < count; ++i)
        {
            if (!ok[i])
                continue;
            Store(registers, changes[i].Target, changes[i].Part, values[i]);
            ++applied;
        }
        return applied;
    }
}
