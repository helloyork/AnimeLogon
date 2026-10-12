#include "json.h"

#include <cmath>
#include <cstdio>
#include <cwchar>

namespace json {

namespace {
const Value kNull;
constexpr int kMaxDepth = 32;
}  // namespace

const Value &Value::operator[](std::wstring_view key) const {
    if (type_ != Type::Object) return kNull;
    const auto it = members_.find(key);
    return it == members_.end() ? kNull : it->second;
}

const Value &Value::operator[](size_t index) const {
    return type_ == Type::Array && index < items_.size() ? items_[index] : kNull;
}

class Parser {
public:
    explicit Parser(std::wstring_view text) : s_(text) {}

    bool Document(Value *out) {
        if (!Parse(out, 0)) return false;
        Space();
        return i_ == s_.size();
    }

private:
    void Space() {
        while (i_ < s_.size() && (s_[i_] == L' ' || s_[i_] == L'\t' || s_[i_] == L'\n' || s_[i_] == L'\r')) ++i_;
    }

    bool Literal(const wchar_t *word) {
        const size_t n = wcslen(word);
        if (s_.substr(i_, n) != word) return false;
        i_ += n;
        return true;
    }

    bool Parse(Value *v, int depth) {
        if (depth > kMaxDepth) return false;
        Space();
        if (i_ >= s_.size()) return false;
        const wchar_t c = s_[i_];
        if (c == L'{') return Object(v, depth);
        if (c == L'[') return Array(v, depth);
        if (c == L'"') {
            v->type_ = Value::Type::String;
            return String(&v->text_);
        }
        if (c == L't' || c == L'f') {
            v->type_ = Value::Type::Bool;
            v->flag_ = c == L't';
            return Literal(c == L't' ? L"true" : L"false");
        }
        if (c == L'n') return Literal(L"null");
        return Number(v);
    }

    bool Object(Value *v, int depth) {
        v->type_ = Value::Type::Object;
        ++i_;
        Space();
        if (i_ < s_.size() && s_[i_] == L'}') return ++i_, true;
        for (;;) {
            Space();
            std::wstring key;
            if (i_ >= s_.size() || s_[i_] != L'"' || !String(&key)) return false;
            Space();
            if (i_ >= s_.size() || s_[i_] != L':') return false;
            ++i_;
            Value member;
            if (!Parse(&member, depth + 1)) return false;
            v->members_[key] = std::move(member);
            Space();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == L'}') return ++i_, true;
            if (s_[i_] != L',') return false;
            ++i_;
        }
    }

    bool Array(Value *v, int depth) {
        v->type_ = Value::Type::Array;
        ++i_;
        Space();
        if (i_ < s_.size() && s_[i_] == L']') return ++i_, true;
        for (;;) {
            Value item;
            if (!Parse(&item, depth + 1)) return false;
            v->items_.push_back(std::move(item));
            Space();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == L']') return ++i_, true;
            if (s_[i_] != L',') return false;
            ++i_;
        }
    }

    static int Hex(wchar_t c) {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    }

    bool String(std::wstring *out) {
        ++i_;  // the opening quote
        while (i_ < s_.size()) {
            const wchar_t c = s_[i_++];
            if (c == L'"') return true;
            if (c < 0x20) return false;
            if (c != L'\\') {
                out->push_back(c);
                continue;
            }
            if (i_ >= s_.size()) return false;
            switch (s_[i_++]) {
            case L'"': out->push_back(L'"'); break;
            case L'\\': out->push_back(L'\\'); break;
            case L'/': out->push_back(L'/'); break;
            case L'b': out->push_back(L'\b'); break;
            case L'f': out->push_back(L'\f'); break;
            case L'n': out->push_back(L'\n'); break;
            case L'r': out->push_back(L'\r'); break;
            case L't': out->push_back(L'\t'); break;
            case L'u': {
                if (i_ + 4 > s_.size()) return false;
                int code = 0;
                for (int k = 0; k < 4; ++k) {
                    const int h = Hex(s_[i_ + k]);
                    if (h < 0) return false;
                    code = code * 16 + h;
                }
                i_ += 4;
                out->push_back((wchar_t)code);  // surrogate pairs arrive as two escapes
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool Number(Value *v) {
        const size_t start = i_;
        auto digits = [&] {
            const size_t from = i_;
            while (i_ < s_.size() && s_[i_] >= L'0' && s_[i_] <= L'9') ++i_;
            return i_ > from;
        };
        if (i_ < s_.size() && s_[i_] == L'-') ++i_;
        if (i_ < s_.size() && s_[i_] == L'0') ++i_;
        else if (!digits()) return false;
        if (i_ < s_.size() && s_[i_] == L'.') {
            ++i_;
            if (!digits()) return false;
        }
        if (i_ < s_.size() && (s_[i_] == L'e' || s_[i_] == L'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == L'+' || s_[i_] == L'-')) ++i_;
            if (!digits()) return false;
        }
        const std::wstring number(s_.substr(start, i_ - start));
        wchar_t *end = nullptr;
        v->type_ = Value::Type::Number;
        v->number_ = wcstod(number.c_str(), &end);
        return end && *end == L'\0' && std::isfinite(v->number_);
    }

    std::wstring_view s_;
    size_t i_ = 0;
};

bool Parse(std::wstring_view text, Value *out) {
    Value v;
    if (!Parser(text).Document(&v)) return false;
    *out = std::move(v);
    return true;
}

void Writer::Separate() {
    if (!first_) out_ += L',';
    first_ = false;
}

Writer &Writer::BeginObject() {
    Separate();
    out_ += L'{';
    first_ = true;
    return *this;
}

Writer &Writer::EndObject() {
    out_ += L'}';
    first_ = false;
    return *this;
}

Writer &Writer::BeginArray() {
    Separate();
    out_ += L'[';
    first_ = true;
    return *this;
}

Writer &Writer::EndArray() {
    out_ += L']';
    first_ = false;
    return *this;
}

Writer &Writer::Key(std::wstring_view key) {
    String(key);
    out_ += L':';
    first_ = true;  // the value that follows is not separated from its key
    return *this;
}

Writer &Writer::String(std::wstring_view value) {
    Separate();
    out_ += L'"';
    for (wchar_t c : value) {
        switch (c) {
        case L'"': out_ += L"\\\""; break;
        case L'\\': out_ += L"\\\\"; break;
        case L'\n': out_ += L"\\n"; break;
        case L'\r': out_ += L"\\r"; break;
        case L'\t': out_ += L"\\t"; break;
        default:
            // Control characters, and the two line separators JavaScript once rejected.
            if (c < 0x20 || c == 0x2028 || c == 0x2029) {
                wchar_t buf[8];
                swprintf(buf, 8, L"\\u%04x", (unsigned)c);
                out_ += buf;
            } else {
                out_ += c;
            }
        }
    }
    out_ += L'"';
    return *this;
}

Writer &Writer::Number(double value) {
    Separate();
    if (!std::isfinite(value)) {
        out_ += L"null";
        return *this;
    }
    wchar_t buf[40];
    swprintf(buf, 40, L"%.17g", value);
    // The shortest text that reads back as the same double.
    for (int precision = 1; precision < 17; ++precision) {
        wchar_t shorter[40];
        swprintf(shorter, 40, L"%.*g", precision, value);
        if (wcstod(shorter, nullptr) == value) {
            wcscpy_s(buf, shorter);
            break;
        }
    }
    out_ += buf;
    return *this;
}

Writer &Writer::Int(long long value) {
    Separate();
    out_ += std::to_wstring(value);
    return *this;
}

Writer &Writer::Bool(bool value) {
    Separate();
    out_ += value ? L"true" : L"false";
    return *this;
}

Writer &Writer::Null() {
    Separate();
    out_ += L"null";
    return *this;
}

}  // namespace json
