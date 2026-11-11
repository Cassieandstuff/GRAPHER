#pragma once
// A small JSON reader and writer for GRAPHER's data files (the schema). Standard library only. Objects keep their keys in
// file order, so what is written reads back the same and diffs stay small.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace grapher::json {

struct Value;
using Array  = std::vector<Value>;
using Member = std::pair<std::string, Value>;
using Object = std::vector<Member>;

struct Value {
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v = nullptr;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : v(b) {}
    Value(double d) : v(d) {}
    Value(int i) : v(static_cast<double>(i)) {}
    Value(std::string s) : v(std::move(s)) {}
    Value(const char* s) : v(std::string(s)) {}
    Value(Array a) : v(std::move(a)) {}
    Value(Object o) : v(std::move(o)) {}

    [[nodiscard]] bool IsNull() const { return std::holds_alternative<std::nullptr_t>(v); }
    [[nodiscard]] const bool*        Bool() const { return std::get_if<bool>(&v); }
    [[nodiscard]] const double*      Number() const { return std::get_if<double>(&v); }
    [[nodiscard]] const std::string* String() const { return std::get_if<std::string>(&v); }
    [[nodiscard]] const Array*       Items() const { return std::get_if<Array>(&v); }
    [[nodiscard]] const Object*      Members() const { return std::get_if<Object>(&v); }
    /// @return the member `key` of an object; nullptr when this is no object or has no such member
    [[nodiscard]] const Value* Find(std::string_view key) const;
};

/// @return the document, or nullopt with `error` saying where and why it is not JSON
[[nodiscard]] std::optional<Value> Parse(std::string_view text, std::string& error);

/// @brief The value as JSON text, indented by four spaces, with a newline at the end; short arrays of scalars stay on
///        one line.
[[nodiscard]] std::string Write(const Value& value);

} // namespace grapher::json
