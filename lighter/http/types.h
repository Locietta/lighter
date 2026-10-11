#pragma once

#include <string>
#include <string_view>

#include <lighter/types.hpp>

namespace lighter::http {

namespace method {

constexpr inline std::string_view get = "GET";
constexpr inline std::string_view post = "POST";
constexpr inline std::string_view put = "PUT";
constexpr inline std::string_view patch = "PATCH";
constexpr inline std::string_view del = "DELETE";
constexpr inline std::string_view head = "HEAD";
constexpr inline std::string_view options = "OPTIONS";
constexpr inline std::string_view trace = "TRACE";
constexpr inline std::string_view connect = "CONNECT";

} // namespace method

struct Header {
    std::string name;
    std::string value;
};

/// The standard reason phrase of an HTTP status code, or "Unknown".
std::string_view status_reason(i32 status) noexcept;

} // namespace lighter::http
