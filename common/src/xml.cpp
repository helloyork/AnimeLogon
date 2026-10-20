#include "animelogon/xml.h"

#include <windows.h>

#include "animelogon/text.h"

namespace animelogon::xml {
namespace {

bool IsSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }
bool IsNameStart(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'_'; }
bool IsNameChar(wchar_t c) { return IsNameStart(c) || (c >= L'0' && c <= L'9') || c == L'-' || c == L'.'; }

class Reader {
public:
    Reader(const std::wstring &text, const Limits &limits) : s_(text), limits_(limits) {}

    bool Document(Element *root) {
        if (Starts(L"<?xml")) {
            const size_t end = s_.find(L"?>", at_);
            if (end == std::wstring::npos) return Fail(L"the XML declaration is not closed");
            Advance(end + 2 - at_);
        }
        if (!Misc()) return false;
        if (!Starts(L"<")) return Fail(L"expected the root element");
        if (!ElementAt(root, 1)) return false;
        if (!Misc()) return false;
        return at_ == s_.size() || Fail(L"unexpected content after the root element");
    }

    const std::wstring &error() const { return error_; }

private:
    bool Fail(const std::wstring &what) {
        if (error_.empty()) error_ = Format(L"line %d: %s", line_, what.c_str());
        return false;
    }
    bool Starts(const wchar_t *token) const { return s_.compare(at_, wcslen(token), token) == 0; }
    void Advance(size_t n) {
        for (size_t i = 0; i < n && at_ < s_.size(); ++i, ++at_)
            if (s_[at_] == L'\n') ++line_;
    }
    void SkipSpace() {
        while (at_ < s_.size() && IsSpace(s_[at_])) Advance(1);
    }

    // Whitespace and comments.
    bool Misc() {
        for (;;) {
            SkipSpace();
            if (Starts(L"<!--")) {
                const size_t end = s_.find(L"-->", at_ + 4);
                if (end == std::wstring::npos) return Fail(L"a comment is not closed");
                Advance(end + 3 - at_);
            } else if (Starts(L"<!")) {
                return Fail(L"DOCTYPE and CDATA are not allowed");
            } else if (Starts(L"<?")) {
                return Fail(L"processing instructions are not allowed");
            } else {
                return true;
            }
        }
    }

    bool Name(std::wstring *out) {
        if (at_ >= s_.size() || !IsNameStart(s_[at_])) return Fail(L"expected a name");
        const size_t start = at_;
        while (at_ < s_.size() && IsNameChar(s_[at_]) && at_ - start < 64) Advance(1);
        if (at_ < s_.size() && IsNameChar(s_[at_])) return Fail(L"a name is too long");
        out->assign(s_, start, at_ - start);
        return true;
    }

    bool Entity(std::wstring *out) {
        const size_t end = s_.find(L';', at_);
        if (end == std::wstring::npos || end - at_ > 10) return Fail(L"a bad character reference");
        const std::wstring ref = s_.substr(at_ + 1, end - at_ - 1);
        Advance(end + 1 - at_);
        if (ref == L"lt") return *out += L'<', true;
        if (ref == L"gt") return *out += L'>', true;
        if (ref == L"amp") return *out += L'&', true;
        if (ref == L"quot") return *out += L'"', true;
        if (ref == L"apos") return *out += L'\'', true;
        if (ref.size() < 2 || ref[0] != L'#') return Fail(L"unknown entity &" + ref + L";");
        const bool hex = ref[1] == L'x';
        unsigned long code = 0;
        size_t digits = 0;
        for (size_t i = hex ? 2 : 1; i < ref.size(); ++i, ++digits) {
            const wchar_t c = ref[i];
            const int d = c >= L'0' && c <= L'9'          ? c - L'0'
                          : hex && c >= L'a' && c <= L'f' ? c - L'a' + 10
                          : hex && c >= L'A' && c <= L'F' ? c - L'A' + 10
                                                          : -1;
            if (d < 0 || code > 0x10FFFF) return Fail(L"a bad character reference");
            code = code * (hex ? 16 : 10) + (unsigned long)d;
        }
        if (!digits || code > 0x10FFFF || (code < 0x20 && code != L'\t') || (code >= 0xD800 && code <= 0xDFFF))
            return Fail(L"a bad character reference");
        if (code >= 0x10000) {
            code -= 0x10000;
            *out += (wchar_t)(0xD800 + (code >> 10));
            *out += (wchar_t)(0xDC00 + (code & 0x3FF));
        } else {
            *out += (wchar_t)code;
        }
        return true;
    }

    bool Attribute(Element *e) {
        std::wstring name, value;
        if (!Name(&name)) return false;
        SkipSpace();
        if (!Starts(L"=")) return Fail(L"expected '=' after " + name);
        Advance(1);
        SkipSpace();
        if (at_ >= s_.size() || (s_[at_] != L'"' && s_[at_] != L'\'')) return Fail(L"expected a quoted value");
        const wchar_t quote = s_[at_];
        Advance(1);
        while (at_ < s_.size() && s_[at_] != quote) {
            const wchar_t c = s_[at_];
            if (c == L'<') return Fail(L"'<' in the value of " + name);
            if (c == L'&') {
                if (!Entity(&value)) return false;
            } else {
                value += IsSpace(c) ? L' ' : c;  // as XML normalises attribute values
                Advance(1);
            }
            if (value.size() > limits_.value) return Fail(L"the value of " + name + L" is too long");
        }
        if (at_ >= s_.size()) return Fail(L"a value is not closed");
        Advance(1);
        for (const auto &a : e->attributes)
            if (a.first == name) return Fail(L"attribute " + name + L" is repeated");
        if (e->attributes.size() >= limits_.attributes) return Fail(L"too many attributes");
        e->attributes.emplace_back(std::move(name), std::move(value));
        return true;
    }

    bool ElementAt(Element *e, int depth) {
        if (depth > limits_.depth) return Fail(L"elements are nested too deeply");
        if (++elements_ > limits_.elements) return Fail(L"too many elements");
        e->line = line_;
        Advance(1);  // '<'
        if (!Name(&e->name)) return false;
        for (;;) {
            const size_t before = at_;
            SkipSpace();
            if (Starts(L"/>")) {
                Advance(2);
                return true;
            }
            if (Starts(L">")) {
                Advance(1);
                break;
            }
            if (at_ == before) return Fail(L"expected an attribute, '>' or '/>'");
            if (!Attribute(e)) return false;
        }
        for (;;) {
            if (!Misc()) return false;
            if (Starts(L"</")) {
                Advance(2);
                std::wstring name;
                if (!Name(&name)) return false;
                if (name != e->name) return Fail(L"</" + name + L"> closes <" + e->name + L">");
                SkipSpace();
                if (!Starts(L">")) return Fail(L"expected '>'");
                Advance(1);
                return true;
            }
            if (at_ >= s_.size()) return Fail(L"<" + e->name + L"> is not closed");
            if (!Starts(L"<")) return Fail(L"text is not allowed between elements");
            e->children.emplace_back();
            if (!ElementAt(&e->children.back(), depth + 1)) return false;
        }
    }

    const std::wstring &s_;
    const Limits &limits_;
    size_t at_ = 0;
    int line_ = 1;
    size_t elements_ = 0;
    std::wstring error_;
};

void Escape(const std::wstring &value, std::wstring *out) {
    for (wchar_t c : value) {
        switch (c) {
        case L'<': *out += L"&lt;"; break;
        case L'>': *out += L"&gt;"; break;
        case L'&': *out += L"&amp;"; break;
        case L'"': *out += L"&quot;"; break;
        default: *out += c;
        }
    }
}

void WriteElement(const Element &e, int indent, std::wstring *out) {
    out->append((size_t)indent * 2, L' ');
    *out += L'<' + e.name;
    for (const auto &[name, value] : e.attributes) {
        *out += L' ' + name + L"=\"";
        Escape(value, out);
        *out += L'"';
    }
    if (e.children.empty()) {
        *out += L"/>\n";
        return;
    }
    *out += L">\n";
    for (const Element &child : e.children) WriteElement(child, indent + 1, out);
    out->append((size_t)indent * 2, L' ');
    *out += L"</" + e.name + L">\n";
}

}  // namespace

const std::wstring *Element::Find(std::wstring_view attribute) const {
    for (const auto &a : attributes)
        if (a.first == attribute) return &a.second;
    return nullptr;
}

bool Parse(std::string_view utf8, Element *root, std::wstring *error, const Limits &limits) {
    auto fail = [&](const wchar_t *what) {
        if (error) *error = what;
        return false;
    };
    if (utf8.size() > limits.bytes) return fail(L"the file is too large");
    if (utf8.size() >= 3 && (uint8_t)utf8[0] == 0xEF && (uint8_t)utf8[1] == 0xBB && (uint8_t)utf8[2] == 0xBF)
        utf8.remove_prefix(3);
    std::wstring text;
    if (!utf8.empty()) {
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(), nullptr, 0);
        if (n <= 0) return fail(L"the file is not UTF-8");
        text.resize((size_t)n);
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(), text.data(), n);
    }
    for (wchar_t c : text)
        if ((c < 0x20 && !IsSpace(c)) || c == 0x7F || c == 0xFFFE || c == 0xFFFF)
            return fail(L"the file contains control characters");
    Reader reader(text, limits);
    Element parsed;
    if (!reader.Document(&parsed)) {
        if (error) *error = reader.error();
        return false;
    }
    *root = std::move(parsed);
    return true;
}

std::string Write(const Element &root) {
    std::wstring out = L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    WriteElement(root, 0, &out);
    return ToUtf8(out);
}

}  // namespace animelogon::xml
