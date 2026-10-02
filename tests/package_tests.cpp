#include "check.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "animelogon/package.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"

using namespace animelogon;

namespace {

// A fresh directory under %TEMP%.
std::wstring ScratchDir() {
    wchar_t temp[MAX_PATH + 1], longer[32768];
    GetTempPathW(ARRAYSIZE(temp), temp);
    const DWORD n = GetLongPathNameW(temp, longer, ARRAYSIZE(longer));
    std::wstring dir = n && n < ARRAYSIZE(longer) ? longer : temp;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\animelogon-test-" + RandomHex(6);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool WriteAll(const std::wstring &path, const std::vector<uint8_t> &bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) && wrote == bytes.size();
    CloseHandle(h);
    return ok;
}

std::vector<uint8_t> ReadAll(const std::wstring &path) {
    std::vector<uint8_t> bytes;
    secure::ReadFileBytes(path, &bytes, 64 * 1024 * 1024);
    return bytes;
}

std::vector<uint8_t> Bytes(const std::string &s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// Deterministic filler, so a large entry has every byte value in it.
std::vector<uint8_t> Noise(size_t n, uint32_t seed) {
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        out[i] = (uint8_t)(seed >> 24);
    }
    return out;
}

void Put16(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back((uint8_t)v);
    out.push_back((uint8_t)(v >> 8));
}

void Put32(std::vector<uint8_t> &out, uint32_t v) {
    Put16(out, v & 0xFFFF);
    Put16(out, v >> 16);
}

void Append(std::vector<uint8_t> &out, const std::string &s) { out.insert(out.end(), s.begin(), s.end()); }

// One entry as the test wants it in an archive, wrong in whatever way the test says.
struct Raw {
    std::string name;
    std::string data;
    uint16_t method = 0;
    uint16_t flags = 0;
    uint32_t crcFlip = 0;           // XORed into the CRC in both headers
    int64_t localSize = -1;         // the local header's sizes, when not -1
    int64_t centralSize = -1;       // the directory's sizes, when not -1
    int64_t centralPacked = -1;     // the directory's compressed size alone, when not -1
    uint32_t external = 0;
    uint16_t madeBy = 20;
    std::string localExtra, centralExtra;
    enum class Descriptor { None, Plain, Signed, Wrong } descriptor = Descriptor::None;
};

struct Shape {
    std::string before;       // bytes ahead of the first entry
    std::string after;        // bytes after the end record
    std::string comment;      // the archive comment
    bool zip64Locator = false;
};

std::vector<uint8_t> Zip(const std::vector<Raw> &entries, const Shape &shape = {}) {
    std::vector<uint8_t> z;
    Append(z, shape.before);
    std::vector<uint32_t> offsets;
    for (const Raw &e : entries) {
        offsets.push_back((uint32_t)z.size());
        const uint32_t crc = package::Crc32(e.data.data(), e.data.size()) ^ e.crcFlip;
        const uint32_t size = (uint32_t)e.data.size();
        const bool descriptor = e.descriptor != Raw::Descriptor::None;
        const uint32_t localSize = e.localSize >= 0 ? (uint32_t)e.localSize : descriptor ? 0 : size;
        Put32(z, 0x04034b50);
        Put16(z, 10);
        Put16(z, e.flags | (descriptor ? 8 : 0));
        Put16(z, e.method);
        Put16(z, 0);
        Put16(z, 0x21);
        Put32(z, descriptor ? 0 : crc);
        Put32(z, localSize);
        Put32(z, localSize);
        Put16(z, (uint32_t)e.name.size());
        Put16(z, (uint32_t)e.localExtra.size());
        Append(z, e.name);
        Append(z, e.localExtra);
        Append(z, e.data);
        if (e.descriptor == Raw::Descriptor::Signed) Put32(z, 0x08074b50);
        if (descriptor) {
            Put32(z, e.descriptor == Raw::Descriptor::Wrong ? crc ^ 1 : crc);
            Put32(z, size);
            Put32(z, size);
        }
    }
    const uint32_t directoryAt = (uint32_t)z.size();
    for (size_t i = 0; i < entries.size(); ++i) {
        const Raw &e = entries[i];
        const uint32_t crc = package::Crc32(e.data.data(), e.data.size()) ^ e.crcFlip;
        const uint32_t size = e.centralSize >= 0 ? (uint32_t)e.centralSize : (uint32_t)e.data.size();
        Put32(z, 0x02014b50);
        Put16(z, e.madeBy);
        Put16(z, 10);
        Put16(z, e.flags | (e.descriptor != Raw::Descriptor::None ? 8 : 0));
        Put16(z, e.method);
        Put16(z, 0);
        Put16(z, 0x21);
        Put32(z, crc);
        Put32(z, e.centralPacked >= 0 ? (uint32_t)e.centralPacked : size);
        Put32(z, size);
        Put16(z, (uint32_t)e.name.size());
        Put16(z, (uint32_t)e.centralExtra.size());
        Put16(z, 0);
        Put16(z, 0);
        Put16(z, 0);
        Put32(z, e.external);
        Put32(z, offsets[i]);
        Append(z, e.name);
        Append(z, e.centralExtra);
    }
    const uint32_t directorySize = (uint32_t)z.size() - directoryAt;
    if (shape.zip64Locator) {
        Put32(z, 0x07064b50);
        Put32(z, 0);
        Put32(z, (uint32_t)z.size());  // where a zip64 end record would be
        Put32(z, 0);
        Put32(z, 1);
    }
    Put32(z, 0x06054b50);
    Put16(z, 0);
    Put16(z, 0);
    Put16(z, (uint32_t)entries.size());
    Put16(z, (uint32_t)entries.size());
    Put32(z, directorySize);
    Put32(z, directoryAt);
    Put16(z, (uint32_t)shape.comment.size());
    Append(z, shape.comment);
    Append(z, shape.after);
    return z;
}

Raw Entry(const std::string &name, const std::string &data) {
    Raw r;
    r.name = name;
    r.data = data;
    return r;
}

std::vector<Raw> Minimal() {
    return {Entry("theme.xml", "<theme format=\"1\"/>"), Entry("wallpaper.png", "\x89PNG pretend")};
}

bool Opens(const std::vector<uint8_t> &zip, std::wstring *why) {
    package::Reader r;
    return r.OpenBytes(zip, why);
}

// Refused, and for the reason the test expects: `reason` appears in the log line.
bool Refused(const std::vector<uint8_t> &zip, const wchar_t *reason) {
    std::wstring why;
    if (Opens(zip, &why)) {
        std::printf("  accepted, expected a refusal mentioning \"%ls\"\n", reason);
        return false;
    }
    if (why.find(reason) == std::wstring::npos) {
        std::printf("  refused with \"%ls\", expected \"%ls\"\n", why.c_str(), reason);
        return false;
    }
    return true;
}

bool RefusedWith(std::vector<Raw> entries, const wchar_t *reason) { return Refused(Zip(entries), reason); }

std::string Zip64Extra() {
    std::vector<uint8_t> x;
    Put16(x, 0x0001);
    Put16(x, 16);
    for (int i = 0; i < 16; ++i) x.push_back(0);
    return std::string(x.begin(), x.end());
}

}  // namespace

TEST(PackageCrc32) {
    CHECK(package::Crc32("123456789", 9) == 0xCBF43926u);
    CHECK(package::Crc32("", 0) == 0);
    // In pieces the same as at once, across the eight-byte steps.
    const std::vector<uint8_t> n = Noise(1000, 7);
    const uint32_t whole = package::Crc32(n.data(), n.size());
    CHECK(package::Crc32(n.data() + 13, n.size() - 13, package::Crc32(n.data(), 13)) == whole);
    // Against the bit-at-a-time definition.
    uint32_t c = ~0u;
    for (uint8_t b : n) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    CHECK(~c == whole);
}

TEST(PackageNames) {
    package::Kind k = package::Kind::Preview;
    CHECK(package::Classify("theme.xml", &k) && k == package::Kind::Theme);
    CHECK(package::Classify("wallpaper.webp", &k) && k == package::Kind::Wallpaper);
    CHECK(package::Classify("preview.png", &k) && k == package::Kind::Preview);
    CHECK(package::Classify("components/clock.xml", &k) && k == package::Kind::Component);
    CHECK(package::Classify("components/a_b-9.xml", nullptr));
    CHECK(package::Classify("components/" + std::string(32, 'a') + ".xml", nullptr));
    CHECK(!package::Classify("components/" + std::string(33, 'a') + ".xml", nullptr));
    CHECK(!package::Classify("components/.xml", nullptr));
    CHECK(!package::Classify("components/Clock.xml", nullptr));
    CHECK(!package::Classify("components/a.b.xml", nullptr));
    CHECK(!package::Classify("components/sub/a.xml", nullptr));
    CHECK(!package::Classify("Theme.xml", nullptr));
    CHECK(!package::Classify("wallpaper.gif", nullptr));
    CHECK(!package::Classify("wallpaper.jpeg", nullptr));
    CHECK(!package::Classify("preview.jpg", nullptr));
    CHECK(!package::Classify("theme.xml/", nullptr));
    CHECK(!package::Classify("", nullptr));
}

TEST(PackageMinimalArchiveOpens) {
    std::wstring why;
    package::Reader r;
    CHECK(r.OpenBytes(Zip(Minimal()), &why));
    CHECK(r.Entries().size() == 2);
    CHECK(r.Theme().name == "theme.xml" && r.Wallpaper() && r.Wallpaper()->extension == "png");
    std::vector<uint8_t> bytes;
    CHECK(r.Read(r.Theme(), package::kMaxXmlBytes, &bytes, &why) && bytes == Bytes("<theme format=\"1\"/>"));
    // Extra fields zip tools add (here an extended timestamp) are fine.
    std::vector<Raw> e = Minimal();
    e[0].localExtra = std::string("UT\x05\x00\x01\x00\x00\x00\x00", 9);
    e[0].centralExtra = e[0].localExtra;
    CHECK(Opens(Zip(e), &why));
    // So is a Unix mode saying "regular file".
    e = Minimal();
    e[1].madeBy = 0x0314;
    e[1].external = 0x81A40000u;  // -rw-r--r--
    CHECK(Opens(Zip(e), &why));
}

TEST(PackageRoundTrip) {
    const std::wstring dir = ScratchDir();
    const std::vector<uint8_t> video = Noise(3 * 1024 * 1024 + 17, 1);  // spans several copy chunks
    CHECK(WriteAll(dir + L"\\video.mp4", video));
    const std::vector<uint8_t> theme = Bytes("<theme format=\"1\" name=\"x\"><wallpaper ref=\"wallpaper.mp4\"/></theme>");
    std::vector<package::Item> items(5);
    items[0] = {"preview.png", L"", Noise(5000, 2)};
    items[1] = {"components/clock.xml", L"", Bytes("<component format=\"1\"/>")};
    items[2] = {"wallpaper.mp4", dir + L"\\video.mp4", {}};
    items[3] = {"theme.xml", L"", theme};
    items[4] = {"components/battery-2.xml", L"", Bytes("<component/>")};

    std::wstring why;
    const std::wstring out = dir + L"\\theme.altheme";
    CHECK(package::Write(items, out, &why));
    const std::vector<uint8_t> file = ReadAll(out);
    CHECK(file.size() > video.size());
    CHECK(file.size() >= 4 && !std::memcmp(file.data(), "PK\x03\x04", 4));
    CHECK(file.size() >= 14 && file[12] == 0x21 && file[13] == 0);  // 1980-01-01

    // The same bytes again, from memory and with the items in another order.
    std::vector<package::Item> reversed(items.rbegin(), items.rend());
    std::vector<uint8_t> memory;
    CHECK(package::WriteToMemory(reversed, &memory, &why));
    CHECK(memory == file);
    // Writing over an existing archive replaces it whole.
    CHECK(package::Write(items, out, &why));
    CHECK(ReadAll(out) == file);

    package::Reader r;
    CHECK(r.Open(out, &why));
    const std::vector<package::Entry> &e = r.Entries();
    CHECK(e.size() == 5);
    if (e.size() != 5) return;
    CHECK(e[0].name == "theme.xml" && e[1].name == "wallpaper.mp4" && e[2].name == "components/battery-2.xml" &&
          e[3].name == "components/clock.xml" && e[4].name == "preview.png");
    CHECK(e[2].component == "battery-2" && e[3].component == "clock" && e[0].component.empty());
    CHECK(e[1].kind == package::Kind::Wallpaper && e[1].extension == "mp4" && e[1].size == video.size());
    CHECK(r.Wallpaper() == &e[1] && &r.Theme() == &e[0]);
    CHECK(r.Find("components/clock.xml") == &e[3] && r.Find("COMPONENTS/clock.xml") == nullptr);

    std::vector<uint8_t> bytes;
    CHECK(r.Read(r.Theme(), package::kMaxXmlBytes, &bytes, &why) && bytes == theme);
    CHECK(!r.Read(e[1], 1024, &bytes, &why) && bytes.empty());
    CHECK(r.Read(e[4], package::kMaxPreviewBytes, &bytes, &why) && bytes == Noise(5000, 2));

    const std::wstring copied = dir + L"\\wallpaper.mp4";
    CHECK(r.Extract(e[1], copied, &why));
    CHECK(ReadAll(copied) == video);
    // Never over an existing file.
    CHECK(!r.Extract(e[0], copied, &why));
    CHECK(ReadAll(copied) == video);

    // While it is open, no one can change the archive.
    HANDLE w = CreateFileW(out.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    CHECK(w == INVALID_HANDLE_VALUE);
    if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
    r.Close();

    package::Reader m;
    CHECK(m.OpenBytes(memory, &why) && m.Entries().size() == 5);
    CHECK(secure::RemoveTree(dir) == ERROR_SUCCESS);
}

TEST(PackageWriterRefuses) {
    std::wstring why;
    std::vector<uint8_t> out;
    const auto item = [](const char *name, const std::string &data) { return package::Item{name, L"", Bytes(data)}; };
    CHECK(!package::WriteToMemory({item("theme.xml", "t"), item("wallpaper.png", "w"), item("notes.txt", "n")}, &out,
                                  &why));
    CHECK(out.empty());
    CHECK(!package::WriteToMemory({item("theme.xml", "t"), item("wallpaper.png", "w"), item("wallpaper.mp4", "v")},
                                  &out, &why));
    CHECK(!package::WriteToMemory({item("wallpaper.png", "w")}, &out, &why));
    CHECK(!package::WriteToMemory({item("theme.xml", "t"), item("THEME.XML", "t"), item("wallpaper.png", "w")}, &out,
                                  &why));
    CHECK(!package::WriteToMemory(
        {item("theme.xml", std::string(package::kMaxXmlBytes + 1, 'x')), item("wallpaper.png", "w")}, &out, &why));
    CHECK(!package::WriteToMemory({item("theme.xml", "t"), package::Item{"wallpaper.png", L"C:\\no\\such\\file.png", {}}},
                                  &out, &why));
    // A failed write leaves no file behind, and nothing in place of the target.
    const std::wstring dir = ScratchDir();
    CHECK(!package::Write({item("wallpaper.png", "w")}, dir + L"\\x.altheme", &why));
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*.*").c_str(), &fd);
    int names = 0;
    if (f != INVALID_HANDLE_VALUE) {
        do ++names;
        while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    CHECK(names == 2);  // "." and ".."
    CHECK(secure::RemoveTree(dir) == ERROR_SUCCESS);

    // A theme with no wallpaper of its own is a whole package, and reads back.
    CHECK(package::WriteToMemory({item("preview.png", "p"), item("theme.xml", "t")}, &out, &why));
    package::Reader r;
    CHECK(r.OpenBytes(out, &why) && r.Entries().size() == 2 && !r.Wallpaper());
    CHECK(r.Entries().size() == 2 && r.Entries()[0].name == "theme.xml" && r.Entries()[1].name == "preview.png");
}

TEST(PackageRefusesTraversalAndOddNames) {
    std::vector<Raw> e = Minimal();
    e.push_back(Entry("../theme.xml", "x"));
    CHECK(RefusedWith(e, L"\"..\""));
    e = Minimal();
    e.push_back(Entry("components/../../evil.xml", "x"));
    CHECK(RefusedWith(e, L"\"..\""));
    e = Minimal();
    e[0].name = "/theme.xml";
    CHECK(RefusedWith(e, L"absolute"));
    e = Minimal();
    e[0].name = "C:/theme.xml";
    CHECK(RefusedWith(e, L"absolute"));
    e = Minimal();
    e.push_back(Entry("components\\clock.xml", "x"));
    CHECK(RefusedWith(e, L"backslash"));
    e = Minimal();
    e.push_back(Entry("components/", ""));
    CHECK(RefusedWith(e, L"directory"));
    e = Minimal();
    e[0].name = std::string("theme.xml\0.png", 14);
    CHECK(RefusedWith(e, L"control character"));
}

TEST(PackageRefusesDuplicates) {
    std::vector<Raw> e = Minimal();
    e.push_back(Entry("theme.xml", "<theme/>"));
    CHECK(RefusedWith(e, L"duplicate"));
    e = Minimal();
    e.push_back(Entry("THEME.xml", "<theme/>"));
    CHECK(RefusedWith(e, L"duplicate"));
    e = Minimal();
    e.push_back(Entry("components/clock.xml", "a"));
    e.push_back(Entry("components/CLOCK.XML", "b"));
    CHECK(RefusedWith(e, L"duplicate"));
}

TEST(PackageRefusesCompressionAndEncryption) {
    std::vector<Raw> e = Minimal();
    e[0].method = 8;
    CHECK(RefusedWith(e, L"compressed"));
    e = Minimal();
    e[1].flags = 1;
    CHECK(RefusedWith(e, L"encrypted"));
    e = Minimal();
    e[1].flags = 0x40;
    CHECK(RefusedWith(e, L"encrypted"));
    e = Minimal();
    e[1].flags = 0x0002;
    CHECK(RefusedWith(e, L"flags"));
}

TEST(PackageRefusesBadCrcAndSizeLies) {
    std::vector<Raw> e = Minimal();
    e[1].crcFlip = 0x10;
    CHECK(RefusedWith(e, L"CRC"));
    // The local header claims more than the directory.
    e = Minimal();
    e[0].localSize = (int64_t)e[0].data.size() + 1;
    CHECK(RefusedWith(e, L"local header"));
    // The directory claims less than the local header.
    e = Minimal();
    e[0].centralSize = (int64_t)e[0].data.size() - 1;
    CHECK(RefusedWith(e, L"local header"));
    // Both claim more than is there: the entry runs over the next one.
    e = Minimal();
    e[0].localSize = e[0].centralSize = (int64_t)e[0].data.size() + 4;
    CHECK(RefusedWith(e, L"overlap"));
    // ...or into the central directory.
    e = Minimal();
    e[1].localSize = e[1].centralSize = (int64_t)e[1].data.size() + 4;
    CHECK(RefusedWith(e, L"central directory"));
    // Stored, yet compressed and uncompressed sizes differ.
    e = Minimal();
    e[0].centralPacked = (int64_t)e[0].data.size() + 1;
    CHECK(RefusedWith(e, L"sizes that disagree"));
    // Too large for its kind, before anything is read.
    e = Minimal();
    e[0].data = std::string(package::kMaxXmlBytes + 1, ' ');
    CHECK(RefusedWith(e, L"more than allowed"));
}

TEST(PackageRefusesTooManyEntries) {
    std::vector<Raw> e = Minimal();
    for (int i = 0; i < 15; ++i) e.push_back(Entry("components/c" + std::to_string(i) + ".xml", "x"));
    CHECK(e.size() == 17);
    CHECK(RefusedWith(e, L"17 entries"));
    e = Minimal();
    for (int i = 0; i < 9; ++i) e.push_back(Entry("components/c" + std::to_string(i) + ".xml", "x"));
    CHECK(RefusedWith(e, L"9 components"));
    e = Minimal();
    for (int i = 0; i < 8; ++i) e.push_back(Entry("components/c" + std::to_string(i) + ".xml", "x"));
    e.push_back(Entry("preview.png", "p"));
    std::wstring why;
    CHECK(Opens(Zip(e), &why));
}

TEST(PackageRefusesUnknownFiles) {
    std::vector<Raw> e = Minimal();
    e.push_back(Entry("readme.txt", "hello"));
    CHECK(RefusedWith(e, L"may hold"));
    e = Minimal();
    e[0].name = "Theme.xml";
    CHECK(RefusedWith(e, L"may hold"));
    e = Minimal();
    e.push_back(Entry("components/clock.xml.exe", "x"));
    CHECK(RefusedWith(e, L"may hold"));
}

TEST(PackageRefusesZip64) {
    std::vector<Raw> e = Minimal();
    e[0].centralExtra = Zip64Extra();
    CHECK(RefusedWith(e, L"zip64"));
    e = Minimal();
    e[0].localExtra = Zip64Extra();
    CHECK(RefusedWith(e, L"zip64"));
    e = Minimal();
    e[0].centralSize = 0xFFFFFFFF;
    CHECK(RefusedWith(e, L"zip64"));
    Shape s;
    s.zip64Locator = true;
    CHECK(Refused(Zip(Minimal(), s), L"zip64"));
    // A malformed extra field is refused too.
    e = Minimal();
    e[0].centralExtra = std::string("\x55\x54\x09\x00\x01", 5);
    CHECK(RefusedWith(e, L"malformed"));
}

TEST(PackageWallpaperAndThemeCounts) {
    std::vector<Raw> e = Minimal();
    e.push_back(Entry("wallpaper.mp4", "video"));
    CHECK(RefusedWith(e, L"more than one wallpaper"));
    e = Minimal();
    e.push_back(Entry("wallpaper.webp", "picture"));
    CHECK(RefusedWith(e, L"more than one wallpaper"));
    e = {Entry("wallpaper.png", "w")};
    CHECK(RefusedWith(e, L"no theme.xml"));

    // No wallpaper at all: theme.xml names a built-in one, or none.
    std::wstring why;
    package::Reader r;
    CHECK(r.OpenBytes(Zip({Entry("theme.xml", "<theme format=\"1\"/>")}), &why));
    CHECK(r.Entries().size() == 1 && r.Wallpaper() == nullptr && r.Theme().name == "theme.xml");
    std::vector<uint8_t> bytes;
    CHECK(r.Read(r.Theme(), package::kMaxXmlBytes, &bytes, &why) && bytes == Bytes("<theme format=\"1\"/>"));
    CHECK(Opens(Zip({Entry("theme.xml", "t"), Entry("components/clock.xml", "c"), Entry("preview.png", "p")}), &why));
}

TEST(PackageRefusesLinksAndDirectoriesByAttribute) {
    std::vector<Raw> e = Minimal();
    e[1].madeBy = 0x0314;
    e[1].external = 0xA1FF0000u;  // lrwxrwxrwx
    CHECK(RefusedWith(e, L"link"));
    e = Minimal();
    e[1].external = FILE_ATTRIBUTE_REPARSE_POINT;
    CHECK(RefusedWith(e, L"link"));
    e = Minimal();
    e[0].external = FILE_ATTRIBUTE_DIRECTORY;
    CHECK(RefusedWith(e, L"directory"));
    e = Minimal();
    e[0].madeBy = 0x0314;
    e[0].external = 0x41ED0000u;  // drwxr-xr-x
    CHECK(RefusedWith(e, L"directory"));
    e = Minimal();
    e[0].madeBy = 0x0314;
    e[0].external = 0x61B00000u;  // a block device
    CHECK(RefusedWith(e, L"plain file"));
}

TEST(PackageDataDescriptors) {
    std::wstring why;
    std::vector<Raw> e = Minimal();
    e[1].descriptor = Raw::Descriptor::Signed;
    CHECK(Opens(Zip(e), &why));
    e[0].descriptor = Raw::Descriptor::Plain;
    CHECK(Opens(Zip(e), &why));
    package::Reader r;
    CHECK(r.OpenBytes(Zip(e), &why));
    std::vector<uint8_t> bytes;
    CHECK(r.Wallpaper() && r.Read(*r.Wallpaper(), 1024, &bytes, &why) && bytes == Bytes("\x89PNG pretend"));
    e = Minimal();
    e[0].descriptor = Raw::Descriptor::Wrong;
    CHECK(RefusedWith(e, L"data descriptor"));
    // Without the flag, sixteen bytes after an entry's data belong to no entry.
    std::vector<Raw> unflagged = Minimal();
    unflagged[1].data += std::string(16, '\0');
    unflagged[1].centralSize = unflagged[1].localSize = (int64_t)Minimal()[1].data.size();
    CHECK(RefusedWith(unflagged, L"belong to no entry"));
}

TEST(PackageRefusesStrayBytes) {
    Shape s;
    s.before = "MZ self-extractor stub";
    CHECK(Refused(Zip(Minimal(), s), L"belong to no entry"));
    s = Shape();
    s.after = "trailing";
    CHECK(Refused(Zip(Minimal(), s), L"end record"));
    s = Shape();
    s.comment = "a comment";
    CHECK(Refused(Zip(Minimal(), s), L"end record"));
    CHECK(Refused({}, L"too short"));
    CHECK(Refused(Bytes("not a zip archive at all, but long enough"), L"end record"));
    // An end record that counts no entries.
    std::vector<uint8_t> empty;
    Put32(empty, 0x06054b50);
    for (int i = 0; i < 9; ++i) Put16(empty, 0);
    CHECK(Refused(empty, L"no entries"));
}

TEST(PackageOpensFromDisk) {
    const std::wstring dir = ScratchDir();
    std::vector<Raw> e = Minimal();
    e[0].crcFlip = 4;
    CHECK(WriteAll(dir + L"\\bad.altheme", Zip(e)));
    CHECK(WriteAll(dir + L"\\good.altheme", Zip(Minimal())));
    package::Reader r;
    std::wstring why;
    CHECK(!r.Open(dir + L"\\bad.altheme", &why) && why.find(L"CRC") != std::wstring::npos);
    CHECK(r.Entries().empty());
    CHECK(r.Open(dir + L"\\good.altheme", &why) && r.Entries().size() == 2);
    CHECK(!r.Open(dir + L"\\missing.altheme", &why));
    r.Close();
    CHECK(secure::RemoveTree(dir) == ERROR_SUCCESS);
}
