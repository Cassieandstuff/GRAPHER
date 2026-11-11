#include "Json.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace grapher::json {

const Value* Value::Find(std::string_view key) const
{
    const Object* o = Members();
    if (!o) return nullptr;
    for (const Member& m : *o)
        if (m.first == key) return &m.second;
    return nullptr;
}

namespace {

constexpr int kMaxDepth = 256;

class Reader {
public:
    explicit Reader(std::string_view text) : s_(text) {}

    std::optional<Value> Document(std::string& error)
    {
        Skip();
        std::optional<Value> v = ParseValue(0);
        if (v) {
            Skip();
            if (i_ != s_.size()) v = Fail("unexpected text after the document");
        }
        if (!v) error = error_;
        return v;
    }

private:
    std::optional<Value> Fail(std::string what)
    {
        if (error_.empty()) {
            std::size_t line = 1, col = 1;
            for (std::size_t k = 0; k < i_ && k < s_.size(); ++k) {
                if (s_[k] == '\n') ++line, col = 1;
                else ++col;
            }
            error_ = "line " + std::to_string(line) + ", column " + std::to_string(col) + ": " + what;
        }
        return std::nullopt;
    }

    void Skip()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }

    bool Literal(std::string_view word)
    {
        if (s_.substr(i_, word.size()) != word) return false;
        i_ += word.size();
        return true;
    }

    std::optional<Value> ParseValue(int depth)
    {
        if (depth > kMaxDepth) return Fail("nested too deeply");
        if (i_ >= s_.size()) return Fail("unexpected end of text");
        switch (s_[i_]) {
        case '{': return ParseObject(depth);
        case '[': return ParseArray(depth);
        case '"': {
            std::string str;
            if (!ParseString(str)) return std::nullopt;
            return Value(std::move(str));
        }
        case 't':
            if (Literal("true")) return Value(true);
            break;
        case 'f':
            if (Literal("false")) return Value(false);
            break;
        case 'n':
            if (Literal("null")) return Value(nullptr);
            break;
        default:
            if (s_[i_] == '-' || (s_[i_] >= '0' && s_[i_] <= '9')) return ParseNumber();
            break;
        }
        return Fail("expected a value");
    }

    std::optional<Value> ParseNumber()
    {
        const std::size_t start = i_;
        if (s_[i_] == '-') ++i_;
        auto digits = [&] {
            const std::size_t d = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
            return i_ > d;
        };
        if (!digits()) return Fail("expected a digit");
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (!digits()) return Fail("expected a digit after the point");
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (!digits()) return Fail("expected an exponent");
        }
        double d = 0.0;
        const auto r = std::from_chars(s_.data() + start, s_.data() + i_, d);
        if (r.ec != std::errc{}) return Fail("number out of range");
        return Value(d);
    }

    static void PutUtf8(std::string& out, std::uint32_t cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(std::uint32_t& out)
    {
        if (i_ + 4 > s_.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool ParseString(std::string& out)
    {
        ++i_;   // the opening quote
        while (true) {
            if (i_ >= s_.size()) return Fail("unterminated string"), false;
            const char c = s_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in a string"), false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) return Fail("unterminated escape"), false;
            switch (const char e = s_[i_++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!Hex4(cp)) return Fail("bad \\u escape"), false;
                if (cp >= 0xD800 && cp < 0xDC00) {   // a surrogate pair
                    std::uint32_t lo = 0;
                    if (!Literal("\\u") || !Hex4(lo) || lo < 0xDC00 || lo >= 0xE000) return Fail("unpaired surrogate"), false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                PutUtf8(out, cp);
                break;
            }
            default: return Fail(std::string("unknown escape \\") + e), false;
            }
        }
    }

    std::optional<Value> ParseArray(int depth)
    {
        ++i_;
        Array items;
        Skip();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return Value(std::move(items));
        }
        while (true) {
            Skip();
            std::optional<Value> v = ParseValue(depth + 1);
            if (!v) return std::nullopt;
            items.push_back(std::move(*v));
            Skip();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return Value(std::move(items));
            }
            return Fail("expected , or ]");
        }
    }

    std::optional<Value> ParseObject(int depth)
    {
        ++i_;
        Object members;
        Skip();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return Value(std::move(members));
        }
        while (true) {
            Skip();
            if (i_ >= s_.size() || s_[i_] != '"') return Fail("expected a key");
            std::string key;
            if (!ParseString(key)) return std::nullopt;
            Skip();
            if (i_ >= s_.size() || s_[i_] != ':') return Fail("expected :");
            ++i_;
            Skip();
            std::optional<Value> v = ParseValue(depth + 1);
            if (!v) return std::nullopt;
            members.emplace_back(std::move(key), std::move(*v));
            Skip();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return Value(std::move(members));
            }
            return Fail("expected , or }");
        }
    }

    std::string_view s_;
    std::size_t      i_ = 0;
    std::string      error_;
};

void WriteString(std::string& out, const std::string& s)
{
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;   // UTF-8 passes through as it is
            }
        }
    }
    out += '"';
}

void WriteNumber(std::string& out, double d)
{
    if (!std::isfinite(d)) {
        out += "null";
        return;
    }
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, d);   // shortest form that reads back the same
    out.append(buf, r.ptr);
}

bool Scalar(const Value& v) { return !v.Items() && !v.Members(); }

void WriteValue(std::string& out, const Value& v, int indent)
{
    const std::string pad(static_cast<std::size_t>(indent + 1) * 4, ' ');
    const std::string close(static_cast<std::size_t>(indent) * 4, ' ');
    if (v.IsNull()) out += "null";
    else if (const bool* b = v.Bool()) out += *b ? "true" : "false";
    else if (const double* d = v.Number()) WriteNumber(out, *d);
    else if (const std::string* s = v.String()) WriteString(out, *s);
    else if (const Array* a = v.Items()) {
        if (a->empty()) {
            out += "[]";
            return;
        }
        bool flat = a->size() <= 8;
        for (const Value& item : *a) flat = flat && Scalar(item);
        if (flat) {
            out += '[';
            for (std::size_t k = 0; k < a->size(); ++k) {
                if (k) out += ", ";
                WriteValue(out, (*a)[k], indent + 1);
            }
            out += ']';
            return;
        }
        out += "[\n";
        for (std::size_t k = 0; k < a->size(); ++k) {
            out += pad;
            WriteValue(out, (*a)[k], indent + 1);
            out += k + 1 < a->size() ? ",\n" : "\n";
        }
        out += close + "]";
    } else if (const Object* o = v.Members()) {
        if (o->empty()) {
            out += "{}";
            return;
        }
        out += "{\n";
        for (std::size_t k = 0; k < o->size(); ++k) {
            out += pad;
            WriteString(out, (*o)[k].first);
            out += ": ";
            WriteValue(out, (*o)[k].second, indent + 1);
            out += k + 1 < o->size() ? ",\n" : "\n";
        }
        out += close + "}";
    }
}

} // namespace

std::optional<Value> Parse(std::string_view text, std::string& error)
{
    return Reader(text).Document(error);
}

std::string Write(const Value& value)
{
    std::string out;
    WriteValue(out, value, 0);
    out += '\n';
    return out;
}

} // namespace grapher::json
