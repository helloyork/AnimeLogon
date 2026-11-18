// The service's side of the signed-in person's agent (app/src/agent.h): it keeps the agent of the
// person at the console running -- which keeps their lock screen picture, the sign-in background, to
// the theme shown (animelogon/signin.h) -- takes its messages down the pipe while settings.ini's
// `media` allows them, and keeps live.ini (animelogon/live.h) -- written here, readable by SYSTEM and
// administrators only, and deleted as soon as it no longer belongs to the person at the console.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "agentlogon.h"

class LiveData {
public:
    ~LiveData() { Stop(); }

    // Starts taking messages. False if the pipe could not be made: nothing playing is shown.
    bool Start();
    // Ends the agent and the pipe, and deletes live.ini.
    void Stop();

    // From the service's loop, as often as it likes; it looks once a second. Starts the agent
    // in the console session's signed-in user's session when it should run, ends it when it
    // should not.
    void Tick(DWORD consoleSession);
    // Ends the agent and starts none again: AnimeLogon is putting back what it changed, and the
    // agent would set its picture again.
    void Hold();

private:
    void Serve();
    // Whether the pipe's client is the agent in the console session, run by that session's
    // user. `session` is set to its session.
    bool Trusted(HANDLE pipe, DWORD *session);
    void Write(const std::string &message, DWORD session);
    void Forget(const wchar_t *why);
    void EndAgent();

    std::thread thread_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE stop_ = nullptr;  // manual reset: the pipe thread leaves
    std::mutex lock_;         // live.ini, and which session may write it
    DWORD allowed_ = 0xFFFFFFFF;  // the session whose agent runs, 0xFFFFFFFF: none
    std::vector<unsigned char> userSid_;  // that session's user

    HANDLE agent_ = nullptr;
    DWORD agentSession_ = 0xFFFFFFFF;
    std::vector<unsigned char> agentSid_;  // the user the agent runs as
    bool held_ = false;
    uint64_t agentLogon_ = 0;
    AgentLogonGuard logonGuard_;
    ULONGLONG agentAt_ = 0, nextTry_ = 0, settingsAt_ = 0, tickAt_ = 0;
    DWORD backoff_ = 0;
    bool media_ = true;  // settings.ini's `media`
};
