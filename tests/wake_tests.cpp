#include "check.h"

#include "wake.h"

using wake::DecideKey;
using wake::KeyLatch;
using wake::KeyState;
using wake::KeyVerdict;

namespace {

KeyVerdict Down(KeyLatch &latch, const KeyState &s, DWORD vk) { return DecideKey(latch, s, vk, true, false, true); }
KeyVerdict Up(KeyLatch &latch, const KeyState &s, DWORD vk) { return DecideKey(latch, s, vk, false, true, true); }

}  // namespace

TEST(WakeKeyOnlyTheFirstIsWithheld) {
    KeyLatch latch;
    KeyState s;
    s.armed = true;
    CHECK(Down(latch, s, 'A') == KeyVerdict::WithholdWake);
    CHECK(Down(latch, s, 'A') == KeyVerdict::Withhold);  // its auto-repeat
    CHECK(Up(latch, s, 'A') == KeyVerdict::Withhold);    // and its release
    CHECK(Down(latch, s, 'B') == KeyVerdict::Pass);
    CHECK(Down(latch, s, 'A') == KeyVerdict::Pass);
}

TEST(WakeKeyDisarmedPassesEverything) {
    KeyLatch latch;
    KeyState s;  // not armed: woken by a click, or not on the screen
    CHECK(Down(latch, s, 'A') == KeyVerdict::Pass);
    CHECK(Down(latch, s, VK_RETURN) == KeyVerdict::Pass);
}

TEST(WakeKeyChordsAndModifiersPass) {
    KeyLatch latch;
    KeyState s;
    s.armed = true;
    CHECK(!wake::IsWakeKey(VK_SHIFT, false, false, false));
    CHECK(!wake::IsWakeKey('L', false, false, true));
    CHECK(!wake::IsWakeKey(VK_F11, false, false, false));
    CHECK(wake::IsWakeKey(VK_SPACE, false, false, false));
    CHECK(DecideKey(latch, s, VK_LWIN, true, false, false) == KeyVerdict::Pass);
    CHECK(!latch.taken);
}

TEST(WakeKeyDarkPanelLightsFirst) {
    KeyLatch latch;
    KeyState s;
    s.armed = true;
    s.displayOff = true;
    CHECK(Down(latch, s, 'A') == KeyVerdict::Withhold);  // lights the panel, does not wake
    CHECK(Up(latch, s, 'A') == KeyVerdict::Withhold);
    s.displayOff = false;
    CHECK(Down(latch, s, 'B') == KeyVerdict::WithholdWake);
    CHECK(Down(latch, s, 'C') == KeyVerdict::Pass);
}

TEST(WakeKeyLatchIsPerScreen) {
    KeyLatch latch;
    KeyState s;
    s.armed = true;
    CHECK(Down(latch, s, 'A') == KeyVerdict::WithholdWake);
    CHECK(Up(latch, s, 'A') == KeyVerdict::Withhold);
    latch = KeyLatch{};  // the next lock
    CHECK(Down(latch, s, VK_RETURN) == KeyVerdict::WithholdWake);
}

#include "nv12.h"

TEST(Nv12PaddedPlaneRows) {
    CHECK(Nv12Lock::PlaneRows(1080, 1920u * 1088 * 3 / 2, 1920) == 1088);  // software decoder padding
    CHECK(Nv12Lock::PlaneRows(1080, 1920u * 1080 * 3 / 2, 1920) == 1080);
    CHECK(Nv12Lock::PlaneRows(720, 1280u * 720 * 3 / 2, 1280) == 720);
    CHECK(Nv12Lock::PlaneRows(1080, 2048u * 1088 * 3 / 2, 2048) == 1088);   // wider pitch
    CHECK(Nv12Lock::PlaneRows(1080, 1920u * 1080 * 3 / 2 + 4096, 1920) == 1080);
    CHECK(Nv12Lock::PlaneRows(1080, 1000, 1920) == 0);
}
