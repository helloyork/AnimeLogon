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
