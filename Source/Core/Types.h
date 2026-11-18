#pragma once

#include "Core/Errors.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace Manifold
{
    using Address = std::uint64_t;

    enum class ProcessId : std::uint32_t
    {
        None = 0
    };

    constexpr ProcessId MakeProcessId(std::uint32_t value) { return static_cast<ProcessId>(value); }
    constexpr std::uint32_t Raw(ProcessId id) { return static_cast<std::uint32_t>(id); }
    inline std::string ToText(ProcessId id) { return std::to_string(Raw(id)); }

    template <class T>
    class Result
    {
    public:
        Result(T value) : m_value(std::move(value)) {}
        Result(Status error) : m_error(std::move(error)) {}

        bool Ok() const { return m_value.has_value(); }
        explicit operator bool() const { return Ok(); }

        T& Value() & { return *m_value; }
        const T& Value() const& { return *m_value; }
        T&& Value() && { return std::move(*m_value); }
        T& operator*() & { return *m_value; }
        const T& operator*() const& { return *m_value; }
        T* operator->() { return &*m_value; }
        const T* operator->() const { return &*m_value; }
        T ValueOr(T fallback) const& { return m_value ? *m_value : std::move(fallback); }

        const Status& Error() const { return m_error; }
        Status ToStatus() const { return Ok() ? Status::Success() : m_error; }

    private:
        std::optional<T> m_value;
        Status m_error;
    };
}
