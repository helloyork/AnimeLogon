// A windowless agent ended by sign-out must not be restarted into that same logon, even while
// WTS still hands out its token. Authentication ids change on a new logon, including when the
// same account signs in again with the same terminal-session id.
#pragma once
#include <cstdint>

class AgentLogonGuard {
public:
    bool Ended(uint32_t exitCode, uint64_t logon) {
        constexpr uint32_t controlExit = 0x40010004;
        constexpr uint32_t logoffInitializationFailure = 0xC000026B;
        if (!logon || (exitCode != controlExit && exitCode != logoffInitializationFailure)) return false;
        endedLogon_ = logon;
        return true;
    }
    bool CanStart(uint64_t logon) const { return logon && logon != endedLogon_; }
private:
    uint64_t endedLogon_ = 0;
};
