// A strict reader for the small part of XML that skin files use: elements and attributes.
// No DOCTYPE, no CDATA, no processing instructions besides the declaration, and no text
// between elements, so there is nothing to expand and nothing to fetch.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace animelogon::xml {

struct Element {
    std::wstring name;
    std::vector<std::pair<std::wstring, std::wstring>> attributes;
    std::vector<Element> children;
    int line = 0;

    // The attribute's value, or null.
    const std::wstring *Find(std::wstring_view attribute) const;
};

struct Limits {
    size_t bytes = 64 * 1024;
    int depth = 4;
    size_t elements = 256;
    size_t attributes = 32;  // per element
    size_t value = 512;      // characters per attribute value
};

// Parses UTF-8 text, with or without a byte order mark. On failure `error` says what and
// on which line.
bool Parse(std::string_view utf8, Element *root, std::wstring *error, const Limits &limits = {});

// Writes the tree back as UTF-8, indented, with every attribute value escaped.
std::string Write(const Element &root);

}  // namespace animelogon::xml
