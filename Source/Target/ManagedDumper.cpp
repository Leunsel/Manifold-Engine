#include "Target/ManagedDumper.h"

#include <cstring>
#include <string>

namespace Manifold::Target
{
    namespace
    {
        constexpr std::uint32_t kMaxStrings = 1u << 24;
        constexpr std::uint32_t kMaxTypes = 1u << 22;
        constexpr std::uint32_t kMaxMembers = 1u << 16;
        constexpr std::size_t kTypeBytes = 32;
        constexpr std::size_t kFieldBytes = 16;
        constexpr std::size_t kMethodBytes = 20;

        class Reader
        {
        public:
            explicit Reader(std::span<const std::uint8_t> data) : m_data(data) {}

            bool Has(std::size_t bytes) const { return m_data.size() - m_at >= bytes; }
            std::size_t Left() const { return m_data.size() - m_at; }

            template <class T>
            bool Read(T& value)
            {
                if (!Has(sizeof(T)))
                    return false;
                std::memcpy(&value, m_data.data() + m_at, sizeof(T));
                m_at += sizeof(T);
                return true;
            }

            bool Text(std::uint32_t length, std::string& out)
            {
                if (!Has(length))
                    return false;
                out.assign(reinterpret_cast<const char*>(m_data.data() + m_at), length);
                m_at += length;
                return true;
            }

        private:
            std::span<const std::uint8_t> m_data;
            std::size_t m_at = 0;
        };

        Status Broken(const std::string& what)
        {
            return Status::Fail(ErrorCode::MonoCallFailed, "the managed dump is damaged: " + what);
        }
    }

    bool IsDumperImage(std::string_view image)
    {
        return image.substr(0, ManagedDumpFormat::kImagePrefix.size()) == ManagedDumpFormat::kImagePrefix;
    }

    Result<ManagedDump> ParseManagedDump(std::span<const std::uint8_t> blob)
    {
        namespace F = ManagedDumpFormat;
        Reader in(blob);
        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        std::uint32_t header = 0;
        std::uint32_t stringCount = 0;
        if (!in.Read(magic) || !in.Read(version) || !in.Read(header) || !in.Read(stringCount))
            return Broken("the header is cut short");
        if (magic != F::kMagic)
            return Broken("it does not start with the dump signature");
        if (version != F::kVersion)
            return Status::Fail(ErrorCode::MonoCallFailed, "the managed dump has format " + std::to_string(version) + ", this build reads " + std::to_string(F::kVersion));
        if (stringCount > kMaxStrings || stringCount > in.Left() / 4)
            return Broken("it claims " + std::to_string(stringCount) + " strings");

        std::vector<std::string> strings(stringCount);
        for (std::string& text : strings)
        {
            std::uint32_t length = 0;
            if (!in.Read(length) || !in.Text(length, text))
                return Broken("a string runs past the end");
        }
        const auto text = [&](std::uint32_t index, std::string& out)
            {
                if (index == F::kNoString)
                {
                    out.clear();
                    return true;
                }
                if (index >= strings.size())
                    return false;
                out = strings[index];
                return true;
            };

        std::uint32_t typeCount = 0;
        if (!in.Read(typeCount))
            return Broken("the type count is missing");
        if (typeCount > kMaxTypes || typeCount > in.Left() / kTypeBytes)
            return Broken("it claims " + std::to_string(typeCount) + " types");

        ManagedDump dump;
        dump.MonoHandles = (header & F::kHeaderMonoHandles) != 0;
        dump.OffsetsSure = dump.MonoHandles && (header & F::kHeaderOffsetsSure) != 0;
        dump.Classes.reserve(typeCount);
        dump.Tokens.reserve(typeCount);
        dump.Flags.reserve(typeCount);
        for (std::uint32_t t = 0; t < typeCount; ++t)
        {
            std::uint32_t image = 0, space = 0, name = 0, parent = 0, token = 0, flags = 0, fieldCount = 0;
            std::uint64_t klass = 0;
            MonoClass info;
            if (!in.Read(image) || !in.Read(space) || !in.Read(name) || !in.Read(parent) || !in.Read(token) || !in.Read(klass) || !in.Read(flags))
                return Broken("type " + std::to_string(t) + " is cut short");
            if (!text(image, info.Image) || !text(space, info.Namespace) || !text(name, info.Name) || !text(parent, info.Parent))
                return Broken("type " + std::to_string(t) + " names a string that does not exist");
            if (!in.Read(fieldCount) || fieldCount > kMaxMembers || fieldCount > in.Left() / kFieldBytes)
                return Broken("type " + std::to_string(t) + " claims " + std::to_string(fieldCount) + " fields");
            for (std::uint32_t f = 0; f < fieldCount; ++f)
            {
                std::uint32_t fieldName = 0, fieldFlags = 0, code = 0;
                std::int32_t offset = 0;
                MonoField field;
                if (!in.Read(fieldName) || !in.Read(offset) || !in.Read(fieldFlags) || !in.Read(code) || !text(fieldName, field.Name))
                    return Broken("a field of type " + std::to_string(t) + " is damaged");
                if (fieldFlags & (F::kFieldStatic | F::kFieldLiteral))
                    continue;
                field.Offset = offset;
                field.TypeCode = static_cast<int>(code);
                info.Fields.push_back(std::move(field));
            }
            std::uint32_t methodCount = 0;
            if (!in.Read(methodCount) || methodCount > kMaxMembers || methodCount > in.Left() / kMethodBytes)
                return Broken("type " + std::to_string(t) + " claims " + std::to_string(methodCount) + " methods");
            info.Methods.reserve(methodCount);
            for (std::uint32_t m = 0; m < methodCount; ++m)
            {
                std::uint32_t methodName = 0, methodFlags = 0;
                std::uint64_t handle = 0;
                std::int32_t parameters = 0;
                MonoMethod method;
                if (!in.Read(methodName) || !in.Read(handle) || !in.Read(methodFlags) || !in.Read(parameters) || !text(methodName, method.Name))
                    return Broken("a method of type " + std::to_string(t) + " is damaged");
                method.Handle = dump.MonoHandles ? handle : 0;
                method.ParamCount = parameters;
                info.Methods.push_back(std::move(method));
            }
            if (info.Name.empty() || IsDumperImage(info.Image))
                continue;
            info.Handle = dump.MonoHandles ? klass : 0;
            info.Generic = (flags & F::kTypeGenericDefinition) != 0;
            info.MembersLoaded = true;
            dump.Classes.push_back(std::move(info));
            dump.Tokens.push_back(token);
            dump.Flags.push_back(flags);
        }
        if (in.Left() != 0)
            return Broken(std::to_string(in.Left()) + " bytes follow the last type");
        return dump;
    }
}
