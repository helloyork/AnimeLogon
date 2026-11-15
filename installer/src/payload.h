// The files install.exe carries inside itself, so that the download is one file, and which of them
// this machine takes. The list is made once, in installer/CMakeLists.txt: the build puts each file
// in as a resource of type PAYLOAD (payload.rc) and writes the table Files() returns
// (payload_files.inc), so the two cannot disagree.
//
// A release's install.exe is the x64 program carrying the programs of both architectures: x64
// Windows runs it natively and Windows 11 on ARM under emulation, and it installs the programs of
// the processor Windows runs on, so that the logon screen and the service run natively. An ARM64
// build's own install.exe carries only ARM64 programs.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace payload {

// A program or a document goes into the install directory. A wallpaper does not: it is the
// bundled wallpaper's video, which install.exe writes into the wallpaper store (engine.cpp), and
// the uninstaller leaves it to the data directory's own removal.
enum class Kind { Program, Document, Wallpaper };

struct File {
    int id;                // the PAYLOAD resource
    const wchar_t *name;   // relative to the install directory; a wallpaper's is only its name
    Kind kind;
    USHORT machine;        // a program's IMAGE_FILE_MACHINE_*; 0 for the rest, which every machine takes
};

// Every file the build names, programs first, then the licences that go with them, then the
// wallpapers; carried or not (uninstall.exe carries none, and deletes the programs and documents).
const std::vector<File> &Files();
// Each name of that kind once, whatever the machine.
std::vector<const wchar_t *> Names(Kind kind);

// The processor this program was built for.
USHORT OwnMachine();
// The processor Windows runs on, also when Windows runs this program under emulation.
USHORT NativeMachine();
// "x64", "ARM64", or the number.
std::wstring MachineName(USHORT machine);

// The machines whose every program `module` carries (install.exe as built: its own; with
// ANIMELOGON_ARM64_PAYLOAD_DIR, ARM64 too).
std::vector<USHORT> Carried(HMODULE module = nullptr);
// The machine whose programs go onto a machine running `native`: that machine if they are carried.
// Carrying none, the programs are the ones beside this program, built with it, so `own` -- if that
// is what runs here. 0 when there are none for it.
USHORT Choose(USHORT native, USHORT own, const std::vector<USHORT> &carried);
// Choose() for this machine and this program: what install.exe installs here, or 0.
USHORT InstallMachine();

// The bytes of `name` for `machine` (documents for any), or none. `module` null is this program.
bool Bytes(const wchar_t *name, USHORT machine, const void **data, DWORD *size, HMODULE module = nullptr);
bool Has(const wchar_t *name, USHORT machine);
// Writes `name` for `machine` to `path`, replacing what is there. ERROR_FILE_NOT_FOUND when not carried.
DWORD Write(const wchar_t *name, USHORT machine, const std::wstring &path);
// What `machine`'s programs and the documents take on disk, for the space check.
unsigned long long TotalBytes(USHORT machine);

}  // namespace payload
