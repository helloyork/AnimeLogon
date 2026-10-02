// .altheme: a theme packed for sharing. It is a ZIP archive whose entries are all stored
// (method 0, no compression): pictures and videos are compressed already, neither side
// needs a compression library, and any zip tool can still open one.
//
// The settings app reads and writes these as the signed-in user. Nothing here runs in the
// logon screen.
//
// An archive holds these names, exactly (lowercase, '/' between directory and file), and
// nothing else:
//   theme.xml                       exactly one; at most kMaxXmlBytes
//   wallpaper.mp4, wallpaper.png,   at most one of the four (none when theme.xml names a
//   wallpaper.jpg, wallpaper.webp   built-in wallpaper or none); a picture at most
//                                   kMaxPictureBytes, a video up to the archive's limit.
//                                   Whether theme.xml and the entries agree is the
//                                   importer's check, not the reader's.
//   components/<name>.xml           0 to kMaxComponents, <name> matching [a-z0-9_-]{1,32};
//                                   each at most kMaxXmlBytes
//   preview.png                     optional; at most kMaxPreviewBytes
//
// The reader refuses, saying why in one line for the log:
//   - an archive over kMaxArchiveBytes, with more than kMaxEntries entries, or with an
//     archive comment;
//   - zip64 records or fields, more than one disk, encryption, any compression method but
//     0 (stored), and general-purpose flags other than bit 3 (data descriptor) and bit 11
//     (UTF-8 names);
//   - absolute paths, "..", backslashes, directory entries, entries whose external
//     attributes make them a link or anything but a plain file, duplicate names (compared
//     case-insensitively), and any name not in the list above;
//   - a local header that disagrees with the central directory, sizes that disagree, a
//     CRC-32 that does not match the bytes, and any byte not accounted for: the entries
//     (from offset 0, in any order), then the central directory, then the end record must
//     cover the file exactly, with no gap, overlap or trailing data.
//
// Data descriptors (flag bit 3), which zip tools writing to a stream produce, are
// accepted. The central directory is what the reader trusts for sizes and CRCs; the local
// header must then carry either zeros or the same values, and the descriptor after the
// data (12 bytes, or 16 with its optional signature) must repeat them exactly.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace animelogon::package {

constexpr uint64_t kMaxArchiveBytes = 1ull << 30;  // 1 GiB, the whole file
constexpr size_t kMaxEntries = 16;
constexpr size_t kMaxComponents = 8;
constexpr uint32_t kMaxXmlBytes = 64 * 1024;
constexpr uint32_t kMaxPreviewBytes = 16 * 1024 * 1024;
constexpr uint32_t kMaxPictureBytes = 256 * 1024 * 1024;  // the same as image::kMaxSourceBytes

enum class Kind { Theme, Wallpaper, Component, Preview };

// Is `name` one the format allows? `kind` (optional) receives what it is.
bool Classify(std::string_view name, Kind *kind);

// CRC-32 as ZIP (and PNG) use it: polynomial 0xEDB88320, reflected, inverted. Pass the
// previous result as `crc` to continue over more bytes.
uint32_t Crc32(const void *bytes, size_t size, uint32_t crc = 0);

struct Entry {
    std::string name;       // as in the archive, for example "components/clock.xml"
    Kind kind = Kind::Theme;
    std::string extension;  // "xml", "mp4", "png", "jpg" or "webp"
    std::string component;  // Kind::Component: <name> in components/<name>.xml; otherwise empty
    uint32_t size = 0;      // bytes
    uint32_t crc = 0;
    uint64_t offset = 0;    // where the entry's bytes start in the archive
};

// An archive that has passed every check above, CRCs included, and stays open for reading
// its entries. While it is open from a file, no one can write that file.
class Reader {
public:
    Reader() = default;
    ~Reader();
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;

    // Opens the archive at `path` and checks all of it, reading every entry once.
    bool Open(const std::wstring &path, std::wstring *why);
    // The same for an archive already in memory.
    bool OpenBytes(std::vector<uint8_t> bytes, std::wstring *why);
    void Close();

    // In central directory order.
    const std::vector<Entry> &Entries() const { return entries_; }
    // nullptr when the archive has no entry by that exact name.
    const Entry *Find(std::string_view name) const;
    // After a successful Open: the one theme.xml, and the wallpaper or nullptr when the
    // archive carries none.
    const Entry &Theme() const;
    const Entry *Wallpaper() const;

    // Reads an entry into memory, refusing one larger than `limit`, and checks its CRC
    // again.
    bool Read(const Entry &entry, size_t limit, std::vector<uint8_t> *bytes, std::wstring *why) const;
    // Copies an entry into a new file (never over an existing one), checking its CRC on
    // the way; on any failure the new file is deleted.
    bool Extract(const Entry &entry, const std::wstring &file, std::wstring *why) const;

private:
    bool Check(std::wstring *why);
    bool ReadAt(uint64_t offset, void *buffer, size_t size) const;
    bool CrcOf(const Entry &entry, uint32_t *crc) const;

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::vector<uint8_t> memory_;
    uint64_t size_ = 0;
    std::vector<Entry> entries_;
};

// One entry for the writer: its archive name, and its bytes, read from `file` when that is
// not empty and taken from `bytes` otherwise.
struct Item {
    std::string name;
    std::wstring file;
    std::vector<uint8_t> bytes;
};

// Writes a stored zip of `items` to `path`, through a new file beside it that replaces
// `path` once complete. Entries go in a fixed order (theme.xml, the wallpaper if there is
// one, components by name, preview.png), every timestamp is 1980-01-01 00:00, and no field depends on the
// machine or the time, so the same items always give the same bytes. Refuses anything the
// reader would refuse, and a source file that changes size while it is being copied.
bool Write(const std::vector<Item> &items, const std::wstring &path, std::wstring *why);
// The same into memory.
bool WriteToMemory(const std::vector<Item> &items, std::vector<uint8_t> *out, std::wstring *why);

}  // namespace animelogon::package
