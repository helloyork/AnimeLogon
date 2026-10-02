#include "animelogon/package.h"

#include <algorithm>
#include <cstring>

#include "animelogon/image.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"

namespace animelogon::package {

static_assert(kMaxPictureBytes == image::kMaxSourceBytes, "a packed picture must be one the importer accepts");
static_assert(kMaxXmlBytes == theme::kMaxBytes, "a packed theme.xml must be one the parser accepts");

namespace {

constexpr uint32_t kLocalSig = 0x04034b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kEndSig = 0x06054b50;
constexpr uint32_t kZip64LocatorSig = 0x07064b50;
constexpr uint32_t kDescriptorSig = 0x08074b50;
constexpr size_t kLocalBytes = 30;
constexpr size_t kCentralBytes = 46;
constexpr size_t kEndBytes = 22;
constexpr size_t kZip64LocatorBytes = 20;
// Sixteen entries with whitelisted names need a few kilobytes; this leaves room for the
// extra fields zip tools add, and no more.
constexpr uint32_t kMaxCentralDirectory = 64 * 1024;

constexpr uint16_t kFlagEncrypted = 0x0001;
constexpr uint16_t kFlagDescriptor = 0x0008;
constexpr uint16_t kFlagStrongEncryption = 0x0040;
constexpr uint16_t kFlagUtf8 = 0x0800;
constexpr uint16_t kFlagMaskedHeaders = 0x2000;
constexpr uint16_t kZip64ExtraId = 0x0001;

// What the writer puts in every entry: nothing that depends on the machine or the time.
constexpr uint16_t kVersionMadeBy = 20;  // 2.0, MS-DOS attributes (all zero)
constexpr uint16_t kVersionNeeded = 10;  // 1.0 is enough for stored entries
constexpr uint16_t kDosTime = 0;         // 00:00:00
constexpr uint16_t kDosDate = (1 << 5) | 1;  // 1980-01-01, the first day DOS dates can say

constexpr size_t kChunk = 1 << 20;

uint16_t U16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t *p) { return (uint32_t)U16(p) | ((uint32_t)U16(p + 2) << 16); }

void Put16(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back((uint8_t)v);
    out.push_back((uint8_t)(v >> 8));
}

void Put32(std::vector<uint8_t> &out, uint32_t v) {
    Put16(out, v & 0xFFFF);
    Put16(out, v >> 16);
}

// Slicing by eight: eight bytes per step through eight tables, several times faster than a
// byte at a time, which matters when a whole archive of up to 1 GiB is checked.
struct CrcTables {
    uint32_t t[8][256];
    CrcTables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};

const CrcTables &Tables() {
    static const CrcTables tables;
    return tables;
}

// For the log: the name as the archive spells it, printable ASCII only, not too long.
std::wstring Shown(std::string_view name) {
    std::wstring out = L"entry \"";
    for (char c : name.substr(0, 80)) {
        const unsigned char u = (unsigned char)c;
        out += (u >= 32 && u < 127) ? (wchar_t)u : L'?';
    }
    return out + (name.size() > 80 ? L"...\"" : L"\"");
}

bool SameNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; };
    for (size_t i = 0; i < a.size(); ++i)
        if (fold(a[i]) != fold(b[i])) return false;
    return true;
}

// What is wrong with a name in itself, before the list of allowed names is consulted.
// The list alone would refuse all of these; saying which makes the log useful.
const wchar_t *NameProblem(std::string_view name) {
    if (name.empty()) return L" has no name";
    for (char c : name)
        if ((unsigned char)c < 32 || c == 127) return L" has a control character in its name";
    if (name.find('\\') != std::string_view::npos) return L" uses a backslash";
    if (name.front() == '/' || (name.size() >= 2 && name[1] == ':')) return L" is an absolute path";
    for (size_t start = 0;;) {
        const size_t end = name.find('/', start);
        if (name.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start) == "..")
            return L" climbs out of the archive with \"..\"";
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (name.back() == '/') return L" is a directory";
    return nullptr;
}

// Extra fields are a run of (id, size, data). Refuses a malformed run and zip64's field.
const wchar_t *ExtraProblem(const uint8_t *p, size_t n) {
    for (size_t at = 0; at < n;) {
        if (n - at < 4 || n - at - 4 < U16(p + at + 2)) return L" has a malformed extra field";
        if (U16(p + at) == kZip64ExtraId) return L" uses zip64";
        at += 4 + U16(p + at + 2);
    }
    return nullptr;
}

// External attributes: MS-DOS attributes in the low half, and a Unix mode in the high half
// when the writer filled one in.
const wchar_t *AttributeProblem(uint32_t external) {
    if (external & FILE_ATTRIBUTE_DIRECTORY) return L" is marked as a directory";
    if (external & FILE_ATTRIBUTE_REPARSE_POINT) return L" is marked as a link";
    const uint32_t type = (external >> 16) & 0xF000;
    if (type == 0xA000) return L" is marked as a link";
    if (type == 0x4000) return L" is marked as a directory";
    if (type != 0 && type != 0x8000) return L" is not marked as a plain file";
    return nullptr;
}

std::string_view ExtensionOf(std::string_view name) {
    const size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string_view() : name.substr(dot + 1);
}

uint64_t CapFor(Kind kind, std::string_view extension) {
    switch (kind) {
    case Kind::Theme:
    case Kind::Component: return kMaxXmlBytes;
    case Kind::Preview: return kMaxPreviewBytes;
    case Kind::Wallpaper: return extension == "mp4" ? kMaxArchiveBytes : kMaxPictureBytes;
    }
    return 0;
}

int Rank(Kind kind) {
    switch (kind) {
    case Kind::Theme: return 0;
    case Kind::Wallpaper: return 1;
    case Kind::Component: return 2;
    case Kind::Preview: return 3;
    }
    return 4;
}

// Checks one name against everything before it, then against the list; both the reader
// and the writer go through here.
bool Admit(std::string_view name, const std::vector<std::string> &earlier, Kind *kind, std::wstring *why) {
    if (const wchar_t *p = NameProblem(name)) {
        *why = Shown(name) + p;
        return false;
    }
    for (const std::string &e : earlier) {
        if (SameNoCase(e, name)) {
            *why = Shown(name) + L" is a duplicate of " + Shown(e);
            return false;
        }
    }
    if (!Classify(name, kind)) {
        *why = Shown(name) + L" is not a file a theme package may hold";
        return false;
    }
    return true;
}

bool CountsFit(const std::vector<Kind> &kinds, std::wstring *why) {
    size_t themes = 0, wallpapers = 0, components = 0;
    for (Kind k : kinds) {
        themes += k == Kind::Theme;
        wallpapers += k == Kind::Wallpaper;
        components += k == Kind::Component;
    }
    if (themes != 1) *why = L"has no theme.xml";
    else if (wallpapers > 1) *why = L"has more than one wallpaper";
    else if (components > kMaxComponents) *why = Format(L"has %zu components, more than %zu", components, kMaxComponents);
    else return true;
    return false;
}

Entry Describe(std::string_view name, Kind kind) {
    Entry e;
    e.name = name;
    e.kind = kind;
    e.extension = ExtensionOf(name);
    if (kind == Kind::Component) e.component = name.substr(11, name.size() - 11 - 4);  // components/<name>.xml
    return e;
}

std::vector<uint8_t> LocalHeader(const std::string &name, uint32_t crc, uint32_t size) {
    std::vector<uint8_t> h;
    Put32(h, kLocalSig);
    Put16(h, kVersionNeeded);
    Put16(h, 0);  // flags
    Put16(h, 0);  // method: stored
    Put16(h, kDosTime);
    Put16(h, kDosDate);
    Put32(h, crc);
    Put32(h, size);  // compressed
    Put32(h, size);  // uncompressed
    Put16(h, (uint32_t)name.size());
    Put16(h, 0);  // extra field
    h.insert(h.end(), name.begin(), name.end());
    return h;
}

void CentralHeader(std::vector<uint8_t> &h, const std::string &name, uint32_t crc, uint32_t size, uint32_t offset) {
    Put32(h, kCentralSig);
    Put16(h, kVersionMadeBy);
    Put16(h, kVersionNeeded);
    Put16(h, 0);  // flags
    Put16(h, 0);  // method: stored
    Put16(h, kDosTime);
    Put16(h, kDosDate);
    Put32(h, crc);
    Put32(h, size);
    Put32(h, size);
    Put16(h, (uint32_t)name.size());
    Put16(h, 0);  // extra field
    Put16(h, 0);  // comment
    Put16(h, 0);  // disk
    Put16(h, 0);  // internal attributes
    Put32(h, 0);  // external attributes
    Put32(h, offset);
    h.insert(h.end(), name.begin(), name.end());
}

// Where the writer's bytes go: appended in order, and a CRC patched in afterwards.
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool Append(const void *bytes, size_t size) = 0;
    virtual bool Patch(uint64_t at, const void *bytes, size_t size) = 0;
    uint64_t Size() const { return size_; }

protected:
    uint64_t size_ = 0;
};

class MemorySink : public Sink {
public:
    explicit MemorySink(std::vector<uint8_t> *out) : out_(out) { out_->clear(); }
    bool Append(const void *bytes, size_t size) override {
        const auto *p = static_cast<const uint8_t *>(bytes);
        out_->insert(out_->end(), p, p + size);
        size_ += size;
        return true;
    }
    bool Patch(uint64_t at, const void *bytes, size_t size) override {
        if (at + size > out_->size()) return false;
        std::memcpy(out_->data() + at, bytes, size);
        return true;
    }

private:
    std::vector<uint8_t> *out_;
};

class FileSink : public Sink {
public:
    explicit FileSink(HANDLE file) : file_(file) {}
    bool Append(const void *bytes, size_t size) override {
        const auto *p = static_cast<const uint8_t *>(bytes);
        while (size) {
            const DWORD want = (DWORD)std::min(size, kChunk);
            DWORD wrote = 0;
            if (!WriteFile(file_, p, want, &wrote, nullptr) || wrote != want) return false;
            p += want;
            size -= want;
            size_ += want;
        }
        return true;
    }
    bool Patch(uint64_t at, const void *bytes, size_t size) override {
        LARGE_INTEGER there{}, back{};
        there.QuadPart = (LONGLONG)at;
        back.QuadPart = (LONGLONG)size_;
        DWORD wrote = 0;
        return SetFilePointerEx(file_, there, nullptr, FILE_BEGIN) &&
               WriteFile(file_, bytes, (DWORD)size, &wrote, nullptr) && wrote == size &&
               SetFilePointerEx(file_, back, nullptr, FILE_BEGIN);
    }

private:
    HANDLE file_;
};

struct Planned {
    const Item *item;
    Kind kind;
};

bool Plan(const std::vector<Item> &items, std::vector<Planned> *plan, std::wstring *why) {
    if (items.size() > kMaxEntries) {
        *why = Format(L"has %zu entries, more than %zu", items.size(), kMaxEntries);
        return false;
    }
    std::vector<std::string> names;
    std::vector<Kind> kinds;
    for (const Item &item : items) {
        Kind kind = Kind::Theme;
        if (!Admit(item.name, names, &kind, why)) return false;
        names.push_back(item.name);
        kinds.push_back(kind);
        plan->push_back({&item, kind});
    }
    if (!CountsFit(kinds, why)) return false;
    std::sort(plan->begin(), plan->end(), [](const Planned &a, const Planned &b) {
        return Rank(a.kind) != Rank(b.kind) ? Rank(a.kind) < Rank(b.kind) : a.item->name < b.item->name;
    });
    return true;
}

// One entry's header and bytes. A file is read once: its CRC is patched into the header
// after the copy, and its size must not change while it is copied.
bool WriteEntry(const Planned &p, Sink &sink, uint32_t *crcOut, uint32_t *sizeOut, std::wstring *why) {
    const Item &item = *p.item;
    const std::wstring who = Shown(item.name);
    const uint64_t cap = CapFor(p.kind, ExtensionOf(item.name));
    HANDLE source = INVALID_HANDLE_VALUE;
    uint64_t size = item.bytes.size();
    if (!item.file.empty()) {
        source = CreateFileW(item.file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER length{};
        if (source == INVALID_HANDLE_VALUE || !GetFileSizeEx(source, &length)) {
            *why = who + Format(L": %s cannot be read (%lu)", item.file.c_str(), GetLastError());
            if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
            return false;
        }
        size = (uint64_t)length.QuadPart;
    }
    const uint64_t offset = sink.Size();
    bool ok = true;
    if (size > cap) {
        *why = who + Format(L" is %llu bytes, more than allowed", size);
        ok = false;
    } else if (offset + kLocalBytes + item.name.size() + size > kMaxArchiveBytes) {
        *why = who + L" would make the archive larger than allowed";
        ok = false;
    }
    uint32_t crc = source == INVALID_HANDLE_VALUE ? Crc32(item.bytes.data(), item.bytes.size()) : 0;
    if (ok) {
        const std::vector<uint8_t> header = LocalHeader(item.name, crc, (uint32_t)size);
        ok = sink.Append(header.data(), header.size());
        if (!ok) *why = Format(L"could not be written (%lu)", GetLastError());
    }
    if (ok && source == INVALID_HANDLE_VALUE) {
        ok = sink.Append(item.bytes.data(), item.bytes.size());
        if (!ok) *why = Format(L"could not be written (%lu)", GetLastError());
    } else if (ok) {
        std::vector<uint8_t> buffer(kChunk);
        uint64_t done = 0;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(source, buffer.data(), (DWORD)buffer.size(), &got, nullptr)) {
                *why = who + Format(L": %s could not be read (%lu)", item.file.c_str(), GetLastError());
                ok = false;
                break;
            }
            if (!got) break;
            done += got;
            if (done > size) break;
            crc = Crc32(buffer.data(), got, crc);
            if (!sink.Append(buffer.data(), got)) {
                *why = Format(L"could not be written (%lu)", GetLastError());
                ok = false;
                break;
            }
        }
        if (ok && done != size) {
            *why = who + L": " + item.file + L" changed while it was being copied";
            ok = false;
        }
        uint8_t le[4] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)};
        if (ok && !sink.Patch(offset + 14, le, sizeof(le))) {
            *why = Format(L"could not be written (%lu)", GetLastError());
            ok = false;
        }
    }
    if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
    *crcOut = crc;
    *sizeOut = (uint32_t)size;
    return ok;
}

bool WriteArchive(const std::vector<Item> &items, Sink &sink, std::wstring *why) {
    std::vector<Planned> plan;
    if (!Plan(items, &plan, why)) return false;
    std::vector<uint8_t> directory;
    for (const Planned &p : plan) {
        const uint32_t offset = (uint32_t)sink.Size();
        uint32_t crc = 0, size = 0;
        if (!WriteEntry(p, sink, &crc, &size, why)) return false;
        CentralHeader(directory, p.item->name, crc, size, offset);
    }
    const uint64_t directoryAt = sink.Size();
    if (directoryAt + directory.size() + kEndBytes > kMaxArchiveBytes) {
        *why = L"would be larger than allowed";
        return false;
    }
    std::vector<uint8_t> end;
    Put32(end, kEndSig);
    Put16(end, 0);  // this disk
    Put16(end, 0);  // the directory's disk
    Put16(end, (uint32_t)plan.size());
    Put16(end, (uint32_t)plan.size());
    Put32(end, (uint32_t)directory.size());
    Put32(end, (uint32_t)directoryAt);
    Put16(end, 0);  // comment
    if (!sink.Append(directory.data(), directory.size()) || !sink.Append(end.data(), end.size())) {
        *why = Format(L"could not be written (%lu)", GetLastError());
        return false;
    }
    return true;
}

}  // namespace

bool Classify(std::string_view name, Kind *kind) {
    // Every allowed name is printable ASCII, so widening byte by byte is exact, and the
    // wallpaper and component names are checked by the same functions that check theme.xml's
    // refs: an entry is allowed exactly when a package theme could name it.
    if (name.empty() || name.size() > 64) return false;
    for (char c : name)
        if ((unsigned char)c < 32 || (unsigned char)c > 126) return false;
    const std::wstring wide(name.begin(), name.end());
    Kind k = Kind::Theme;
    if (wide == L"theme.xml") k = Kind::Theme;
    else if (theme::IsPackageWallpaperRef(wide)) k = Kind::Wallpaper;
    else if (wide == L"preview.png") k = Kind::Preview;
    else if (theme::IsPackageComponentRef(wide)) k = Kind::Component;
    else return false;
    if (kind) *kind = k;
    return true;
}

uint32_t Crc32(const void *bytes, size_t size, uint32_t crc) {
    const auto &t = Tables().t;
    const auto *p = static_cast<const uint8_t *>(bytes);
    crc = ~crc;
    for (; size >= 8; p += 8, size -= 8) {
        const uint32_t a = crc ^ U32(p);
        const uint32_t b = U32(p + 4);
        crc = t[7][a & 0xFF] ^ t[6][(a >> 8) & 0xFF] ^ t[5][(a >> 16) & 0xFF] ^ t[4][a >> 24] ^ t[3][b & 0xFF] ^
              t[2][(b >> 8) & 0xFF] ^ t[1][(b >> 16) & 0xFF] ^ t[0][b >> 24];
    }
    for (; size; ++p, --size) crc = (crc >> 8) ^ t[0][(crc ^ *p) & 0xFF];
    return ~crc;
}

Reader::~Reader() { Close(); }

void Reader::Close() {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
    std::vector<uint8_t>().swap(memory_);
    size_ = 0;
    entries_.clear();
}

bool Reader::Open(const std::wstring &path, std::wstring *why) {
    Close();
    std::wstring local;
    if (!why) why = &local;
    // Others may read it too, but no one may change it while it is open.
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                        nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        *why = Format(L"cannot be opened (%lu)", GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_, &size)) {
        *why = Format(L"size unreadable (%lu)", GetLastError());
        Close();
        return false;
    }
    size_ = (uint64_t)size.QuadPart;
    if (!Check(why)) {
        Close();
        return false;
    }
    return true;
}

bool Reader::OpenBytes(std::vector<uint8_t> bytes, std::wstring *why) {
    Close();
    std::wstring local;
    if (!why) why = &local;
    memory_ = std::move(bytes);
    size_ = memory_.size();
    if (!Check(why)) {
        Close();
        return false;
    }
    return true;
}

bool Reader::ReadAt(uint64_t offset, void *buffer, size_t size) const {
    if (offset > size_ || size > size_ - offset) return false;
    if (file_ == INVALID_HANDLE_VALUE) {
        if (size) std::memcpy(buffer, memory_.data() + offset, size);
        return true;
    }
    auto *p = static_cast<uint8_t *>(buffer);
    while (size) {
        OVERLAPPED at{};
        at.Offset = (DWORD)offset;
        at.OffsetHigh = (DWORD)(offset >> 32);
        const DWORD want = (DWORD)std::min(size, kChunk);
        DWORD got = 0;
        if (!ReadFile(file_, p, want, &got, &at) || got != want) return false;
        p += got;
        offset += got;
        size -= got;
    }
    return true;
}

bool Reader::CrcOf(const Entry &entry, uint32_t *crc) const {
    std::vector<uint8_t> buffer(std::min<size_t>(entry.size, kChunk));
    uint32_t c = 0;
    for (uint64_t done = 0; done < entry.size;) {
        const size_t n = (size_t)std::min<uint64_t>(entry.size - done, kChunk);
        if (!ReadAt(entry.offset + done, buffer.data(), n)) return false;
        c = Crc32(buffer.data(), n, c);
        done += n;
    }
    *crc = c;
    return true;
}

bool Reader::Check(std::wstring *why) {
    entries_.clear();
    if (size_ > kMaxArchiveBytes) {
        *why = Format(L"is %llu bytes, more than allowed", size_);
        return false;
    }
    if (size_ < kEndBytes) {
        *why = L"is too short to be a zip archive";
        return false;
    }

    // The end record, which must be the last 22 bytes: no archive comment, nothing after.
    const uint64_t endAt = size_ - kEndBytes;
    uint8_t end[kEndBytes];
    if (!ReadAt(endAt, end, sizeof(end))) {
        *why = L"could not be read";
        return false;
    }
    if (U32(end) != kEndSig || U16(end + 20) != 0) {
        *why = L"does not end with a zip end record (not a zip archive, or one with a comment or trailing data)";
        return false;
    }
    const uint16_t disk = U16(end + 4), directoryDisk = U16(end + 6), here = U16(end + 8), total = U16(end + 10);
    const uint32_t directorySize = U32(end + 12), directoryAt = U32(end + 16);
    if (here == 0xFFFF || total == 0xFFFF || directorySize == 0xFFFFFFFF || directoryAt == 0xFFFFFFFF) {
        *why = L"uses zip64";
        return false;
    }
    if (disk || directoryDisk || here != total) {
        *why = L"spans more than one disk";
        return false;
    }
    if (!total) {
        *why = L"has no entries";
        return false;
    }
    if (total > kMaxEntries) {
        *why = Format(L"has %u entries, more than %zu", (unsigned)total, kMaxEntries);
        return false;
    }
    if ((uint64_t)directoryAt + directorySize != endAt) {
        uint8_t sig[4];
        if (endAt >= kZip64LocatorBytes && ReadAt(endAt - kZip64LocatorBytes, sig, 4) && U32(sig) == kZip64LocatorSig)
            *why = L"uses zip64";
        else
            *why = L"has a central directory that does not end where the end record begins";
        return false;
    }
    if (directorySize > kMaxCentralDirectory) {
        *why = L"has a central directory larger than allowed";
        return false;
    }

    // The central directory: what the archive says it holds.
    std::vector<uint8_t> dir(directorySize);
    if (!ReadAt(directoryAt, dir.data(), dir.size())) {
        *why = L"could not be read";
        return false;
    }
    std::vector<std::string> names;
    std::vector<Kind> kinds;
    std::vector<uint16_t> flagsOf;
    std::vector<uint32_t> localAt;
    size_t at = 0;
    for (unsigned i = 0; i < total; ++i) {
        if (dir.size() - at < kCentralBytes || U32(&dir[at]) != kCentralSig) {
            *why = L"has a damaged central directory";
            return false;
        }
        const uint8_t *c = &dir[at];
        const uint16_t flags = U16(c + 8), method = U16(c + 10), nameBytes = U16(c + 28), extraBytes = U16(c + 30),
                       commentBytes = U16(c + 32), diskStart = U16(c + 34);
        const uint32_t crc = U32(c + 16), packed = U32(c + 20), unpacked = U32(c + 24), external = U32(c + 38),
                       local = U32(c + 42);
        if (dir.size() - at - kCentralBytes < (size_t)nameBytes + extraBytes + commentBytes) {
            *why = L"has a damaged central directory";
            return false;
        }
        const std::string name((const char *)c + kCentralBytes, nameBytes);
        const std::wstring who = Shown(name);
        at += kCentralBytes + nameBytes + extraBytes + commentBytes;

        if (flags & (kFlagEncrypted | kFlagStrongEncryption | kFlagMaskedHeaders)) {
            *why = who + L" is encrypted";
            return false;
        }
        if (flags & ~(kFlagDescriptor | kFlagUtf8)) {
            *why = who + Format(L" uses flags 0x%04x", (unsigned)flags);
            return false;
        }
        if (method != 0) {
            *why = who + Format(L" is compressed (method %u); entries must be stored", (unsigned)method);
            return false;
        }
        if (packed == 0xFFFFFFFF || unpacked == 0xFFFFFFFF || local == 0xFFFFFFFF || diskStart == 0xFFFF) {
            *why = who + L" uses zip64";
            return false;
        }
        if (diskStart != 0) {
            *why = who + L" is on another disk";
            return false;
        }
        if (packed != unpacked) {
            *why = who + L" has sizes that disagree";
            return false;
        }
        if (const wchar_t *p = ExtraProblem(c + kCentralBytes + nameBytes, extraBytes)) {
            *why = who + p;
            return false;
        }
        Kind kind = Kind::Theme;
        if (!Admit(name, names, &kind, why)) return false;
        if (const wchar_t *p = AttributeProblem(external)) {
            *why = who + p;
            return false;
        }
        Entry e = Describe(name, kind);
        e.size = unpacked;
        e.crc = crc;
        if (e.size > CapFor(kind, e.extension)) {
            *why = who + Format(L" is %u bytes, more than allowed", e.size);
            return false;
        }
        names.push_back(name);
        kinds.push_back(kind);
        flagsOf.push_back(flags);
        localAt.push_back(local);
        entries_.push_back(std::move(e));
    }
    if (at != dir.size()) {
        *why = L"has a damaged central directory";
        return false;
    }
    if (!CountsFit(kinds, why)) return false;

    // Each local header must agree with the directory, and the entries, directory and end
    // record must cover the file exactly.
    struct Span {
        uint64_t start, end;
    };
    std::vector<Span> spans;
    for (size_t i = 0; i < entries_.size(); ++i) {
        Entry &e = entries_[i];
        const std::wstring who = Shown(e.name);
        const uint64_t start = localAt[i];
        uint8_t h[kLocalBytes];
        if (start + kLocalBytes > directoryAt || !ReadAt(start, h, sizeof(h)) || U32(h) != kLocalSig) {
            *why = who + L" has no local header where the directory says";
            return false;
        }
        const uint16_t flags = U16(h + 6), method = U16(h + 8), nameBytes = U16(h + 26), extraBytes = U16(h + 28);
        const uint32_t crc = U32(h + 14), packed = U32(h + 18), unpacked = U32(h + 22);
        const bool descriptor = (flagsOf[i] & kFlagDescriptor) != 0;
        const bool same = crc == e.crc && packed == e.size && unpacked == e.size;
        const bool deferred = descriptor && !crc && !packed && !unpacked;
        if (flags != flagsOf[i] || method != 0 || !(same || deferred) || nameBytes != e.name.size()) {
            *why = who + L" has a local header that disagrees with the directory";
            return false;
        }
        const uint64_t nameAt = start + kLocalBytes;
        std::vector<uint8_t> tail((size_t)nameBytes + extraBytes);
        if (nameAt + tail.size() > directoryAt || !ReadAt(nameAt, tail.data(), tail.size()) ||
            std::memcmp(tail.data(), e.name.data(), nameBytes) != 0) {
            *why = who + L" has a local header that disagrees with the directory";
            return false;
        }
        if (const wchar_t *p = ExtraProblem(tail.data() + nameBytes, extraBytes)) {
            *why = who + p;
            return false;
        }
        e.offset = nameAt + tail.size();
        uint64_t stop = e.offset + e.size;
        if (stop > directoryAt) {
            *why = who + L" runs into the central directory";
            return false;
        }
        if (descriptor) {
            // 12 bytes (CRC, sizes), or 16 with the optional signature first; both must
            // repeat the directory's values.
            uint8_t d[16] = {};
            const size_t room = (size_t)std::min<uint64_t>(sizeof(d), directoryAt - stop);
            if (room >= 12 && ReadAt(stop, d, room)) {
                if (room == 16 && U32(d) == kDescriptorSig && U32(d + 4) == e.crc && U32(d + 8) == e.size &&
                    U32(d + 12) == e.size)
                    stop += 16;
                else if (U32(d) == e.crc && U32(d + 4) == e.size && U32(d + 8) == e.size)
                    stop += 12;
            }
            if (stop == e.offset + e.size) {
                *why = who + L" has a data descriptor that disagrees with the directory";
                return false;
            }
        }
        spans.push_back({start, stop});
    }
    std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) { return a.start < b.start; });
    uint64_t expect = 0;
    for (const Span &s : spans) {
        if (s.start != expect) {
            *why = s.start < expect ? L"has entries that overlap" : L"has bytes that belong to no entry";
            return false;
        }
        expect = s.end;
    }
    if (expect != directoryAt) {
        *why = L"has bytes that belong to no entry";
        return false;
    }

    // Last, every byte of every entry against its CRC.
    for (const Entry &e : entries_) {
        uint32_t crc = 0;
        if (!CrcOf(e, &crc)) {
            *why = Shown(e.name) + L" could not be read";
            return false;
        }
        if (crc != e.crc) {
            *why = Shown(e.name) + L" fails its CRC check";
            return false;
        }
    }
    return true;
}

const Entry *Reader::Find(std::string_view name) const {
    for (const Entry &e : entries_)
        if (e.name == name) return &e;
    return nullptr;
}

const Entry &Reader::Theme() const {
    static const Entry none;
    for (const Entry &e : entries_)
        if (e.kind == Kind::Theme) return e;
    return none;
}

const Entry *Reader::Wallpaper() const {
    for (const Entry &e : entries_)
        if (e.kind == Kind::Wallpaper) return &e;
    return nullptr;
}

bool Reader::Read(const Entry &entry, size_t limit, std::vector<uint8_t> *bytes, std::wstring *why) const {
    std::wstring local;
    if (!why) why = &local;
    bytes->clear();
    if (entry.size > limit) {
        *why = Shown(entry.name) + Format(L" is %u bytes, more than %zu", entry.size, limit);
        return false;
    }
    std::vector<uint8_t> out(entry.size);
    if (!ReadAt(entry.offset, out.data(), out.size())) {
        *why = Shown(entry.name) + L" could not be read";
        return false;
    }
    if (Crc32(out.data(), out.size()) != entry.crc) {
        *why = Shown(entry.name) + L" fails its CRC check";
        return false;
    }
    *bytes = std::move(out);
    return true;
}

bool Reader::Extract(const Entry &entry, const std::wstring &file, std::wstring *why) const {
    std::wstring local;
    if (!why) why = &local;
    HANDLE out = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        *why = Format(L"%s cannot be created (%lu)", file.c_str(), GetLastError());
        return false;
    }
    std::vector<uint8_t> buffer(std::min<size_t>(entry.size, kChunk));
    uint32_t crc = 0;
    bool ok = true;
    for (uint64_t done = 0; ok && done < entry.size;) {
        const size_t n = (size_t)std::min<uint64_t>(entry.size - done, kChunk);
        DWORD wrote = 0;
        if (!ReadAt(entry.offset + done, buffer.data(), n)) {
            *why = Shown(entry.name) + L" could not be read";
            ok = false;
        } else if (!WriteFile(out, buffer.data(), (DWORD)n, &wrote, nullptr) || wrote != n) {
            *why = Format(L"%s could not be written (%lu)", file.c_str(), GetLastError());
            ok = false;
        }
        crc = Crc32(buffer.data(), n, crc);
        done += n;
    }
    if (ok && crc != entry.crc) {
        *why = Shown(entry.name) + L" fails its CRC check";
        ok = false;
    }
    CloseHandle(out);
    if (!ok) DeleteFileW(file.c_str());
    return ok;
}

bool Write(const std::vector<Item> &items, const std::wstring &path, std::wstring *why) {
    std::wstring local;
    if (!why) why = &local;
    std::wstring staging;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 8 && h == INVALID_HANDLE_VALUE; ++attempt) {
        staging = path + L"." + RandomHex(6) + L".tmp";
        h = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (h == INVALID_HANDLE_VALUE) {
        *why = Format(L"%s cannot be created (%lu)", staging.c_str(), GetLastError());
        return false;
    }
    FileSink sink(h);
    bool ok = WriteArchive(items, sink, why);
    if (ok && !FlushFileBuffers(h)) {
        *why = Format(L"%s could not be written (%lu)", staging.c_str(), GetLastError());
        ok = false;
    }
    CloseHandle(h);
    if (ok && !MoveFileExW(staging.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        *why = Format(L"%s cannot be replaced (%lu)", path.c_str(), GetLastError());
        ok = false;
    }
    if (!ok) DeleteFileW(staging.c_str());
    return ok;
}

bool WriteToMemory(const std::vector<Item> &items, std::vector<uint8_t> *out, std::wstring *why) {
    std::wstring local;
    if (!why) why = &local;
    MemorySink sink(out);
    if (WriteArchive(items, sink, why)) return true;
    out->clear();
    return false;
}

}  // namespace animelogon::package
