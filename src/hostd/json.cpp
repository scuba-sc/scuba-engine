#include "hostd/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace scuba::hostd {

const Json* Json::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (size_t i = keys_.size(); i-- > 0;)
        if (keys_[i] == key) return &values_[i];
    return nullptr;
}

Json& Json::set(std::string_view key, Json value) {
    if (type_ == Type::Null) type_ = Type::Object;
    for (size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key) {
            values_[i] = std::move(value);
            return *this;
        }
    keys_.emplace_back(key);
    values_.push_back(std::move(value));
    return *this;
}

Json& Json::push_back(Json value) {
    if (type_ == Type::Null) type_ = Type::Array;
    values_.push_back(std::move(value));
    return *this;
}

bool operator==(const Json& a, const Json& b) {
    if (a.type_ != b.type_) return false;
    switch (a.type_) {
        case Json::Type::Null:   return true;
        case Json::Type::Bool:   return a.bool_ == b.bool_;
        case Json::Type::Number: return a.number_ == b.number_;
        case Json::Type::String: return a.string_ == b.string_;
        case Json::Type::Array:  return a.values_ == b.values_;
        case Json::Type::Object: return a.keys_ == b.keys_ && a.values_ == b.values_;
    }
    return false;
}

void AppendJsonString(std::string& out, std::string_view s) {
    static const char kHex[] = "0123456789abcdef";
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += kHex[c >> 4];
                    out += kHex[c & 15u];
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

void Json::DumpTo(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (!std::isfinite(number_)) {
                out += "null";
                break;
            }
            char buf[32];
            if (std::floor(number_) == number_ && std::fabs(number_) < 9007199254740992.0)
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(number_));
            else
                std::snprintf(buf, sizeof buf, "%.17g", number_);
            out += buf;
            break;
        }
        case Type::String: AppendJsonString(out, string_); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < values_.size(); ++i) {
                if (i) out += ',';
                values_[i].DumpTo(out);
            }
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < values_.size(); ++i) {
                if (i) out += ',';
                AppendJsonString(out, keys_[i]);
                out += ':';
                values_[i].DumpTo(out);
            }
            out += '}';
            break;
    }
}

std::string Json::dump() const {
    std::string out;
    DumpTo(out);
    return out;
}

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view text, bool rejectDuplicateKeys) : s_(text), rejectDuplicateKeys_(rejectDuplicateKeys) {}

    std::optional<Json> Run(std::string* error) {
        Json value;
        SkipWs();
        if (!Value(value, 0)) return Fail(error);
        SkipWs();
        if (pos_ != s_.size()) {
            Err("trailing characters");
            return Fail(error);
        }
        return value;
    }

private:
    std::optional<Json> Fail(std::string* error) {
        if (error) *error = error_ + " at offset " + std::to_string(pos_);
        return std::nullopt;
    }
    bool Err(const char* what) {
        if (error_.empty()) error_ = what;
        return false;
    }
    void SkipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }
    bool Literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return Err("invalid literal");
        pos_ += word.size();
        return true;
    }

    bool Value(Json& out, int depth) {
        if (depth > kMaxDepth) return Err("nesting too deep");
        if (pos_ >= s_.size()) return Err("unexpected end");
        const char c = s_[pos_];
        if (c == '{') return Object(out, depth);
        if (c == '[') return Array(out, depth);
        if (c == '"') {
            std::string str;
            if (!String(str)) return false;
            out = Json(std::move(str));
            return true;
        }
        if (c == 't') { out = Json(true); return Literal("true"); }
        if (c == 'f') { out = Json(false); return Literal("false"); }
        if (c == 'n') { out = Json(); return Literal("null"); }
        if (c == '-' || (c >= '0' && c <= '9')) return Number(out);
        return Err("unexpected character");
    }

    bool Object(Json& out, int depth) {
        out = Json::MakeObject();
        ++pos_;
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        for (;;) {
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return Err("expected object key");
            std::string key;
            if (!String(key)) return false;
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return Err("expected ':'");
            ++pos_;
            SkipWs();
            Json value;
            if (!Value(value, depth + 1)) return false;
            if (rejectDuplicateKeys_ && out.find(key)) return Err("duplicate object key");
            out.set(key, std::move(value));
            SkipWs();
            if (pos_ >= s_.size()) return Err("unexpected end");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; return true; }
            return Err("expected ',' or '}'");
        }
    }

    bool Array(Json& out, int depth) {
        out = Json::MakeArray();
        ++pos_;
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        for (;;) {
            SkipWs();
            Json value;
            if (!Value(value, depth + 1)) return false;
            out.push_back(std::move(value));
            SkipWs();
            if (pos_ >= s_.size()) return Err("unexpected end");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; return true; }
            return Err("expected ',' or ']'");
        }
    }

    bool Hex4(uint32_t& out) {
        if (pos_ + 4 > s_.size()) return Err("short \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            uint32_t d;
            if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
            else return Err("bad \\u escape");
            out = (out << 4) | d;
        }
        return true;
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0u | (cp >> 6));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0u | (cp >> 12));
            out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        } else {
            out += static_cast<char>(0xF0u | (cp >> 18));
            out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
            out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        }
    }

    bool String(std::string& out) {
        ++pos_;
        for (;;) {
            if (pos_ >= s_.size()) return Err("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return Err("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return Err("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t lo;
                        if (s_.substr(pos_, 2) != "\\u") return Err("unpaired surrogate");
                        pos_ += 2;
                        if (!Hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return Err("unpaired surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return Err("unpaired surrogate");
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Err("bad escape");
            }
        }
    }

    bool Number(Json& out) {
        const size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size()) return Err("bad number");
        if (s_[pos_] == '0') {
            ++pos_;
        } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        } else {
            return Err("bad number");
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return Err("bad number");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return Err("bad number");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        const std::string text(s_.substr(start, pos_ - start));
        out = Json(std::strtod(text.c_str(), nullptr));
        return true;
    }

    std::string_view s_;
    bool rejectDuplicateKeys_;
    size_t pos_ = 0;
    std::string error_;
};

}  // namespace

std::optional<Json> Json::parse(std::string_view text, std::string* error, bool rejectDuplicateKeys) {
    return Parser(text, rejectDuplicateKeys).Run(error);
}

}  // namespace scuba::hostd
