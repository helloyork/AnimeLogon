#pragma once

// The Segoe Fluent Icons code points, named so that a reader does not have to look them up.
// The font holds well over a thousand of them; this file carries the ones a desktop page
// reaches for, gathered loosely by what they are for -- the groups are a reading aid and
// nothing more, and each icon reads for whatever a page wants it to.
//
// Escapes rather than the characters themselves, so this header means the same thing whatever
// code page the compiler reads it in.
//
// A header of its own rather than a corner of the theme, because this is the one part of the
// library that is a list rather than code, and the one part a page is expected to add to: an
// icon that is not here belongs here, beside the others. Draw one with `Fonts::icon` and never
// with a text format -- that is the icon font's own format, and a text format renders the
// substitution box instead.
//
// Every code point below is in Segoe MDL2 Assets at the same value as well, which is what
// makes the fallback in the theme's font setup safe: Windows 10's font draws the same icon.
namespace micula {
namespace glyph {

// What a control draws to report a state.
constexpr const wchar_t *kCheck   = L"\uE73E";  // CheckMark
constexpr const wchar_t *kWarning = L"\uE7BA";  // Warning
constexpr const wchar_t *kError   = L"\uEA39";  // ErrorBadge
constexpr const wchar_t *kInfo    = L"\uE946";  // Info
constexpr const wchar_t *kBusy    = L"\uE895";  // Sync: the ring a page spins while it waits


// The chrome: a pane's toggle, the arrows a combo and a scroll bar draw.
constexpr const wchar_t *kMenu         = L"\uE700";  // GlobalNavButton
constexpr const wchar_t *kChevronDown  = L"\uE70D";  // ChevronDown
constexpr const wchar_t *kChevronUp    = L"\uE70E";  // ChevronUp
constexpr const wchar_t *kChevronLeft  = L"\uE76B";  // ChevronLeft
constexpr const wchar_t *kChevronRight = L"\uE76C";  // ChevronRight
constexpr const wchar_t *kCaretUp      = L"\uEDDB";  // CaretUpSolid8
constexpr const wchar_t *kCaretDown    = L"\uEDDC";  // CaretDownSolid8
constexpr const wchar_t *kRefresh      = L"\uE72C";  // Refresh
constexpr const wchar_t *kSettings     = L"\uE713";  // Settings


// The verbs a menu, a toolbar, or a row of buttons is made of.
constexpr const wchar_t *kAdd        = L"\uE710";  // Add
constexpr const wchar_t *kDelete     = L"\uE74D";  // Delete
constexpr const wchar_t *kEdit       = L"\uE70F";  // Edit
constexpr const wchar_t *kSave       = L"\uE74E";  // Save
constexpr const wchar_t *kCopy       = L"\uE8C8";  // Copy
constexpr const wchar_t *kCut        = L"\uE8C6";  // Cut
constexpr const wchar_t *kPaste      = L"\uE77F";  // Paste
constexpr const wchar_t *kUndo       = L"\uE7A7";  // Undo
constexpr const wchar_t *kRedo       = L"\uE7A6";  // Redo
constexpr const wchar_t *kSearch     = L"\uE721";  // Search
constexpr const wchar_t *kFilter     = L"\uE71C";  // Filter
constexpr const wchar_t *kSort       = L"\uE8CB";  // Sort
constexpr const wchar_t *kMore       = L"\uE712";  // More
constexpr const wchar_t *kClose      = L"\uE8BB";  // ChromeClose
constexpr const wchar_t *kPin        = L"\uE718";  // Pin
constexpr const wchar_t *kLink       = L"\uE71B";  // Link
constexpr const wchar_t *kShare      = L"\uE72D";  // Share
constexpr const wchar_t *kDownload   = L"\uE896";  // Download
constexpr const wchar_t *kUpload     = L"\uE898";  // Upload
constexpr const wchar_t *kPrint      = L"\uE749";  // Print
constexpr const wchar_t *kNewFolder  = L"\uE8F4";  // NewFolder
constexpr const wchar_t *kOpenFile   = L"\uE8E5";  // OpenFile
constexpr const wchar_t *kNewWindow  = L"\uE8A7";  // OpenInNewWindow


// Places a page lists, and the things in them.
constexpr const wchar_t *kHome     = L"\uE80F";  // Home
constexpr const wchar_t *kRecent   = L"\uE823";  // Recent
constexpr const wchar_t *kHistory  = L"\uE81C";  // History
constexpr const wchar_t *kFolder   = L"\uE8B7";  // Folder
constexpr const wchar_t *kDrive    = L"\uEDA2";  // HardDrive
constexpr const wchar_t *kDocument = L"\uE8A5";  // Document
constexpr const wchar_t *kCloud    = L"\uE753";  // Cloud
constexpr const wchar_t *kGlobe    = L"\uE774";  // Globe
constexpr const wchar_t *kCalendar = L"\uE787";  // Calendar
constexpr const wchar_t *kMail     = L"\uE715";  // Mail
constexpr const wchar_t *kContact  = L"\uE77B";  // Contact
constexpr const wchar_t *kPeople   = L"\uE716";  // People


// Devices, and what the user is trusted with.
constexpr const wchar_t *kCamera     = L"\uE722";  // Camera
constexpr const wchar_t *kColor      = L"\uE790";  // Color
constexpr const wchar_t *kBrightness = L"\uE706";  // Brightness
constexpr const wchar_t *kLock       = L"\uE72E";  // Lock
constexpr const wchar_t *kUnlock     = L"\uE785";  // Unlock
constexpr const wchar_t *kShield     = L"\uEA18";  // Shield
constexpr const wchar_t *kBolt       = L"\uE945";  // LightningBolt
constexpr const wchar_t *kBug        = L"\uEBE8";  // Bug


// Media, and the way a view is looked at.
constexpr const wchar_t *kPlay       = L"\uE768";  // Play
constexpr const wchar_t *kPause      = L"\uE769";  // Pause
constexpr const wchar_t *kStop       = L"\uE71A";  // Stop
constexpr const wchar_t *kPrevious   = L"\uE892";  // Previous
constexpr const wchar_t *kNext       = L"\uE893";  // Next
constexpr const wchar_t *kVolume     = L"\uE767";  // Volume
constexpr const wchar_t *kMute       = L"\uE74F";  // Mute
constexpr const wchar_t *kView       = L"\uE890";  // View
constexpr const wchar_t *kFullScreen = L"\uE740";  // FullScreen
constexpr const wchar_t *kZoomIn     = L"\uE8A3";  // ZoomIn
constexpr const wchar_t *kZoomOut    = L"\uE71F";  // ZoomOut

}  // namespace glyph
}  // namespace micula
