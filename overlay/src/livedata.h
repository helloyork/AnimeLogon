// The service's side of what the signed-in person's session reports for the logon screen
// (animelogon/live.h): it keeps that session's agent running, takes the agent's messages down
// the pipe, and keeps live.ini -- written here, readable by SYSTEM and administrators only, and
// deleted as soon as it no longer belongs to the person at the console.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    ULONGLONG agentAt_ = 0, nextTry_ = 0, settingsAt_ = 0, tickAt_ = 0;
    DWORD backoff_ = 0;
    bool media_ = true;  // settings.ini's `media`
};
