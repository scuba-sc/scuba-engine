#pragma once
// A small JSON value for the bridge protocol: parse (RFC 8259, depth-limited) and serialize.
// Objects keep insertion order; a repeated key keeps the last value on lookup.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace scuba::hostd {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    template <class T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Json(T n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Json(const char* s) : type_(Type::String), string_(s) {}
    Json(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Json(std::string_view s) : type_(Type::String), string_(s) {}

    static Json MakeArray() { Json j; j.type_ = Type::Array; return j; }
    static Json MakeObject() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool fallback = false) const { return is_bool() ? bool_ : fallback; }
    double as_number(double fallback = 0.0) const { return is_number() ? number_ : fallback; }
    const std::string& as_string() const { return string_; }

    // Arrays and objects: element count. Others: 0.
    size_t size() const { return values_.size(); }
    // Arrays: the i-th element. Objects: the i-th value (keys()[i] is its key).
    const Json& at(size_t i) const { return values_[i]; }
    const std::vector<std::string>& keys() const { return keys_; }

    // Objects: the value for key, or nullptr.
    const Json* find(std::string_view key) const;
    // Objects (a Null becomes an empty object): sets key, replacing an existing value.
    Json& set(std::string_view key, Json value);
    // Arrays (a Null becomes an empty array): appends.
    Json& push_back(Json value);

    std::string dump() const;
    static std::optional<Json> parse(std::string_view text, std::string* error = nullptr, bool rejectDuplicateKeys = false);

    friend bool operator==(const Json& a, const Json& b);

private:
    void DumpTo(std::string& out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<std::string> keys_;
    std::vector<Json> values_;
};

void AppendJsonString(std::string& out, std::string_view s);

}  // namespace scuba::hostd
