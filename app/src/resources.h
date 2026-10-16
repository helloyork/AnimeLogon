// The page's own files, built into the program (web.rc.in).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace app {

// The file at `path` (index.html, app.js ...; empty for index.html) and its media type.
// False when the page has no such file.
bool WebFile(const std::wstring &path, std::vector<uint8_t> *bytes, std::wstring *type);

}  // namespace app
