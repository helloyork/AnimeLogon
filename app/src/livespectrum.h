// The spectra in the settings app's previews, moving with what this computer plays: on the theme
// page and in the lock-screen preview, a panel that holds a spectrum is drawn here again and again
// (about thirty times a second, as the page asks) with the logon screen's own renderer, while the
// still pictures under it leave that panel out. It listens to the default output only while the
// page keeps asking, and stops a few seconds after it stops.
#pragma once

#include <windows.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "animelogon/audiocapture.h"
#include "animelogon/skinpreview.h"
#include "animelogon/spectrum.h"
#include "animelogon/theme.h"

namespace app {

class Library;

class LiveSpectrum {
public:
    explicit LiveSpectrum(const Library &library) : library_(library) {}

    // Panel `panel` (from 1) of instance `instance` of `theme`, as a display of `width` x `height`
    // shows it: the part of the picture it can touch, premultiplied, and where that is. `data` is
    // the battery and what is playing; `stream` keeps its bands apart from other streams of the
    // same panel (the theme page's and the lock-screen preview's).
    bool Frame(const std::wstring &stream, const animelogon::theme::Theme &theme, const std::wstring &instance,
               size_t panel, UINT width, UINT height, const animelogon::data::Snapshot &data,
               animelogon::Picture *out, RECT *where);

    // Stops listening once no frame has been asked for in a while, and lets go of panels no longer
    // drawn. Called every half second or so.
    void Idle();

    // The panels of `theme`'s visible instances that hold a spectrum, as "<instance>.p<n>".
    std::vector<std::wstring> Panels(const animelogon::theme::Theme &theme) const;
    // `theme` with those panels left out of its pictures (made fully transparent), so that the
    // page can lay the moving ones over them. Their places and sizes stay as they were.
    animelogon::theme::Theme Without(const animelogon::theme::Theme &theme) const;

private:
    struct Entry {
        std::unique_ptr<animelogon::LivePreview> view;
        ULONGLONG used = 0;
    };

    const Library &library_;
    std::map<std::wstring, Entry> views_;
    animelogon::AudioCapture capture_;
    animelogon::spectrum::Analyzer analyzer_;
    animelogon::spectrum::Frame heard_;
    std::vector<float> samples_;
    ULONGLONG asked_ = 0, analysed_ = 0;
};

}  // namespace app
