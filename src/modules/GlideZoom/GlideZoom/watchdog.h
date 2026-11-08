#pragma once

#ifndef _WIN32
#error GlideZoom watchdog requires Windows.
#endif

#include <windows.h>
#include <shellapi.h>
#include <objbase.h>

#include <cstddef>
#include <cwchar>
#include <string>
#include <vector>

#include "desktop_context.h"
#include "cursor_visibility.h"
#include "cursor_visibility_win32.h"
#include "magnification_api.h"
#include "zoom_session.h"

// Link with shell32 and ole32, in addition to the usual
// Windows system libraries. Both processes must run the same signed UIAccess
// executable. The watchdog does not change pointer settings or inject input.
namespace glide::watchdog
{

    inline constexpr wchar_t kInstanceMutexName[] = L"Local\\GlideZoom.AccessibilityZoom.SingleInstance";

    namespace detail
    {
        constexpr LONG kMagic = 0x475A5731; // GZW1
        constexpr LONG kVersion = 3;
        constexpr ULONGLONG kHangTime100ns = 5ULL * 10000000ULL;

        // Interlocked operations supply cross-process ordering. In particular, do not
        // replace these fields with ordinary bools or unaligned 64-bit values.
        struct alignas(8) SharedState
        {
            volatile LONG magic;
            LONG version;
            DWORD parentPid;
            DWORD reserved;
            ULONGLONG parentCreationTime;
            alignas(8) volatile LONG64 heartbeat100ns;
            volatile LONG ready;
            volatile LONG clean;
            volatile LONG armed;
            volatile LONG cursorArmed;
            volatile LONG watcherPid;
            volatile LONG activeSlot;
            session::RecoveryRecord slots[2];
        };
        static_assert(offsetof(SharedState, heartbeat100ns) % 8 == 0,
                      "Interlocked heartbeat must be aligned");

        inline LONG Read(volatile LONG* value)
        {
            return InterlockedCompareExchange(value, 0, 0);
        }

        inline ULONGLONG ReadHeartbeat(SharedState* state)
        {
            return static_cast<ULONGLONG>(InterlockedCompareExchange64(
                &state->heartbeat100ns, 0, 0));
        }

        inline bool NeedsRecovery(SharedState* state)
        {
            return Read(&state->armed) != 0 || Read(&state->cursorArmed) != 0;
        }

        inline bool Now(ULONGLONG& value)
        {
            // Excluding sleep avoids killing a healthy app immediately after resume.
            return QueryUnbiasedInterruptTime(&value) != FALSE;
        }

        inline std::wstring ImagePath(HANDLE process)
        {
            std::vector<wchar_t> buffer(32768);
            DWORD size = static_cast<DWORD>(buffer.size());
            if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size))
                return {};
            return std::wstring(buffer.data(), size);
        }

        inline ULONGLONG CreationTime(HANDLE process)
        {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
                return 0;
            ULARGE_INTEGER value{};
            value.LowPart = created.dwLowDateTime;
            value.HighPart = created.dwHighDateTime;
            return value.QuadPart;
        }

        inline std::wstring NewToken()
        {
            GUID id{};
            if (FAILED(CoCreateGuid(&id)))
                return {};
            wchar_t token[33]{};
            const int result = std::swprintf(
                token, sizeof(token) / sizeof(token[0]), L"%08x%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x", static_cast<unsigned>(id.Data1), static_cast<unsigned>(id.Data2), static_cast<unsigned>(id.Data3), static_cast<unsigned>(id.Data4[0]), static_cast<unsigned>(id.Data4[1]), static_cast<unsigned>(id.Data4[2]), static_cast<unsigned>(id.Data4[3]), static_cast<unsigned>(id.Data4[4]), static_cast<unsigned>(id.Data4[5]), static_cast<unsigned>(id.Data4[6]), static_cast<unsigned>(id.Data4[7]));
            return result == 32 ? std::wstring(token) : std::wstring{};
        }

        inline bool ValidToken(const std::wstring& token)
        {
            if (token.size() != 32)
                return false;
            for (wchar_t c : token)
            {
                if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')))
                    return false;
            }
            return true;
        }

        inline std::wstring MappingName(const std::wstring& token)
        {
            return L"Local\\GlideZoom.Guard." + token;
        }

        class Handle
        {
        public:
            explicit Handle(HANDLE value = nullptr) :
                value_(value) {}
            ~Handle()
            {
                if (value_)
                    CloseHandle(value_);
            }
            Handle(const Handle&) = delete;
            Handle& operator=(const Handle&) = delete;
            HANDLE Get() const { return value_; }

        private:
            HANDLE value_;
        };

        class MappedState
        {
        public:
            explicit MappedState(HANDLE mapping) :
                state_(static_cast<SharedState*>(MapViewOfFile(
                    mapping,
                    FILE_MAP_ALL_ACCESS,
                    0,
                    0,
                    sizeof(SharedState)))) {}
            ~MappedState()
            {
                if (state_)
                    UnmapViewOfFile(state_);
            }
            MappedState(const MappedState&) = delete;
            MappedState& operator=(const MappedState&) = delete;
            SharedState* Get() const { return state_; }

        private:
            SharedState* state_;
        };

        // Called only after the parent process handle signals termination. The parent
        // is the only writer of slots, so the selected complete slot is then stable.
        inline int Recover(SharedState* state)
        {
            if (Read(&state->clean) || !NeedsRecovery(state))
                return 0;
            const LONG slot = Read(&state->activeSlot);
            struct Pending
            {
                session::RecoveryRecord record{};
                void Publish(const session::RecoveryRecord& next) { record = next; }
                void Clear() {}
            } pending;
            const bool transformArmed = Read(&state->armed) != 0;
            const bool validSlot = slot >= 0 && slot <= 1;
            if (transformArmed && validSlot)
                pending.record = state->slots[slot];
            const bool validTransform = !transformArmed ||
                                        (validSlot && session::Valid(pending.record));
            // Cursor recovery is independent: an invalid transform record must never
            // skip showing a cursor that this process was about to hide.
            struct CursorJournal
            {
                SharedState* shared;
                void ClearCursorRecovery() noexcept { InterlockedExchange(&shared->cursorArmed, 0); }
            } cursorJournal{ state };

            // Lock/unlock and secure desktop switches do not transfer ownership. Wait
            // for this process's original input desktop before even reading transforms.
            // The retained instance mutex prevents a new instance racing this recovery.
            while (!glide::OnInputDesktop())
                Sleep(100);
            MagnificationApi api;
            if (!api.Load() || !api.Initialize())
                return 12;
            cursor::NativeBackend cursorBackend{ api };

            session::Result result = transformArmed ? session::Result::ReadFailure : session::Result::Unchanged;
            bool transformDone = !transformArmed || !validTransform;
            bool cursorDone = !Read(&state->cursorArmed);
            unsigned failures = 0;
            for (;;)
            {
                while (!glide::OnInputDesktop())
                    Sleep(100);
                if (!cursorDone)
                    cursorDone = cursor::Recover(true, cursorBackend, cursorJournal);
                // The shared API adapter also rechecks the desktop at each native call.
                if (!transformDone)
                {
                    result = session::Recover(pending.record, api, pending);
                    transformDone = result == session::Result::Applied || result == session::Result::Unchanged ||
                                    result == session::Result::Conflict || result == session::Result::Invalid;
                }
                if (transformDone && cursorDone)
                    break;
                // A desktop transition is not an API failure. Otherwise bound retries
                // to one second; failed APIs cannot be made reliable by blind writes.
                if (glide::OnInputDesktop() && ++failures >= 10)
                    break;
                Sleep(100);
            }
            api.Uninitialize();
            if (!validTransform)
                return validSlot ? 11 : 10;
            return cursorDone && (result == session::Result::Applied || result == session::Result::Unchanged ||
                                  result == session::Result::Conflict) ?
                       0 :
                       13;
        }
    } // namespace detail

    class Guardian
    {
    public:
        Guardian() = default;
        Guardian(const Guardian&) = delete;
        Guardian& operator=(const Guardian&) = delete;

        ~Guardian()
        {
            // Do not claim clean shutdown while zoom is still owned. In that case
            // the other process retains its mapping and recovers after we exit.
            if (state_ && !detail::NeedsRecovery(state_))
                MarkClean();
            CloseResources();
        }

        bool Start()
        {
            if (state_)
                return Alive();
            const std::wstring token = detail::NewToken();
            const std::wstring image = detail::ImagePath(GetCurrentProcess());
            const ULONGLONG created = detail::CreationTime(GetCurrentProcess());
            ULONGLONG now = 0;
            if (token.empty() || image.empty() || !created || !detail::Now(now))
                return false;

            const std::wstring name = detail::MappingName(token);
            mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(detail::SharedState), name.c_str());
            if (!mapping_)
                return false;
            if (GetLastError() == ERROR_ALREADY_EXISTS)
            {
                CloseResources();
                return false;
            }
            state_ = static_cast<detail::SharedState*>(MapViewOfFile(
                mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(detail::SharedState)));
            if (!state_)
            {
                CloseResources();
                return false;
            }
            // New page-file-backed mappings are zero-initialized by Windows.
            state_->version = detail::kVersion;
            state_->parentPid = GetCurrentProcessId();
            state_->parentCreationTime = created;
            InterlockedExchange(&state_->activeSlot, -1);
            InterlockedExchange64(&state_->heartbeat100ns, static_cast<LONG64>(now));
            InterlockedExchange(&state_->magic, detail::kMagic);

            const std::wstring arguments = L"--watch " +
                                           std::to_wstring(GetCurrentProcessId()) + L" " + token;
            SHELLEXECUTEINFOW info{};
            info.cbSize = sizeof(info);
            info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
            info.lpVerb = L"open";
            info.lpFile = image.c_str();
            info.lpParameters = arguments.c_str();
            info.nShow = SW_HIDE;
            const BOOL launched = ShellExecuteExW(&info);
            process_ = info.hProcess;
            if (!launched || !process_)
            {
                MarkClean();
                CloseResources();
                return false;
            }
            const DWORD watcherPid = GetProcessId(process_);
            if (!watcherPid)
            {
                MarkClean();
                CloseResources();
                return false;
            }
            InterlockedExchange(&state_->watcherPid, static_cast<LONG>(watcherPid));
            // Startup-only handshake: never block the caller for more than one
            // second while waiting for the watchdog's own initialization.
            const ULONGLONG started = GetTickCount64();
            while (GetTickCount64() - started < 1000)
            {
                if (Alive())
                    return true;
                if (WaitForSingleObject(process_, 10) != WAIT_TIMEOUT)
                    break;
            }
            MarkClean();
            CloseResources();
            return false;
        }

        void Beat()
        {
            if (!state_ || detail::Read(&state_->clean))
                return;
            ULONGLONG now = 0;
            if (detail::Now(now))
            {
                InterlockedExchange64(&state_->heartbeat100ns, static_cast<LONG64>(now));
            }
        }

        bool Alive() const
        {
            return state_ && process_ && detail::Read(&state_->ready) == 1 &&
                   detail::Read(&state_->clean) == 0 && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
        }

        // Only the main thread publishes. A native write must happen after this
        // returns. Death during the inactive-slot copy leaves the previous complete
        // record selected; death after publication leaves before/after recognizable.
        void Publish(const session::RecoveryRecord& record)
        {
            if (!state_ || detail::Read(&state_->clean))
                return;
            Beat();
            const LONG active = detail::Read(&state_->activeSlot);
            const LONG next = active == 0 ? 1 : 0;
            state_->slots[next] = record;
            InterlockedExchange(&state_->activeSlot, next);
            InterlockedExchange(&state_->armed, 1);
        }

        // Clear only after complete restoration, or after observing that another
        // owner has replaced our complete snapshot.
        void Clear()
        {
            if (state_)
                InterlockedExchange(&state_->armed, 0);
        }

        // Publish before MagShowSystemCursor(FALSE), including a native call that
        // could take effect and then report failure. The parent is the only writer
        // until it has stopped; the guardian never races a live cursor writer.
        bool ArmCursorRecovery()
        {
            if (!Alive())
                return false;
            Beat();
            InterlockedExchange(&state_->cursorArmed, 1);
            return true;
        }

        void ClearCursorRecovery()
        {
            if (state_)
                InterlockedExchange(&state_->cursorArmed, 0);
        }

        // Call only once the application's own normal shutdown has reset zoom.
        void MarkClean()
        {
            if (state_ && !detail::NeedsRecovery(state_))
                InterlockedExchange(&state_->clean, 1);
        }

    private:
        void CloseResources()
        {
            if (state_)
            {
                UnmapViewOfFile(state_);
                state_ = nullptr;
            }
            if (mapping_)
            {
                CloseHandle(mapping_);
                mapping_ = nullptr;
            }
            if (process_)
            {
                CloseHandle(process_);
                process_ = nullptr;
            }
        }

        HANDLE mapping_ = nullptr;
        HANDLE process_ = nullptr;
        detail::SharedState* state_ = nullptr;
    };

    // Dispatch --watch before the normal instance mutex, hooks, UI or MagInitialize.
    inline int Run(DWORD parentPid, const std::wstring& token)
    {
        if (!parentPid || parentPid == GetCurrentProcessId() || !detail::ValidToken(token))
            return 2;
        const std::wstring name = detail::MappingName(token);
        detail::Handle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str()));
        if (!mapping.Get())
            return 3;
        detail::MappedState view(mapping.Get());
        detail::SharedState* state = view.Get();
        if (!state || detail::Read(&state->magic) != detail::kMagic ||
            state->version != detail::kVersion || state->parentPid != parentPid ||
            !state->parentCreationTime || detail::Read(&state->clean))
            return 4;

        detail::Handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
                                              PROCESS_TERMINATE,
                                          FALSE,
                                          parentPid));
        if (!parent.Get())
            return 5;
        const std::wstring ownImage = detail::ImagePath(GetCurrentProcess());
        const std::wstring parentImage = detail::ImagePath(parent.Get());
        if (ownImage.empty() || parentImage.empty() ||
            _wcsicmp(ownImage.c_str(), parentImage.c_str()) != 0 ||
            detail::CreationTime(parent.Get()) != state->parentCreationTime)
            return 6;

        // Keep the singleton object alive until all old-instance recovery finishes.
        // The main app rejects an existing object; the guardian never acquires it.
        detail::Handle instance(OpenMutexW(SYNCHRONIZE, FALSE, kInstanceMutexName));
        if (!instance.Get())
            return 7;
        const ULONGLONG handshake = GetTickCount64();
        while (!detail::Read(&state->watcherPid))
        {
            if (detail::Read(&state->clean) || GetTickCount64() - handshake >= 1000 ||
                WaitForSingleObject(parent.Get(), 10) != WAIT_TIMEOUT)
                return 8;
        }
        if (static_cast<DWORD>(detail::Read(&state->watcherPid)) != GetCurrentProcessId())
            return 9;

        InterlockedExchange(&state->ready, 1);
        bool wasInputDesktop = true;
        ULONGLONG desktopGraceUntil = 0;
        for (;;)
        {
            if (detail::Read(&state->clean))
                return 0;
            const DWORD wait = WaitForSingleObject(parent.Get(), 100);
            if (wait == WAIT_OBJECT_0)
            {
                return detail::Recover(state);
            }
            if (wait == WAIT_FAILED)
                return 7;
            if (!detail::NeedsRecovery(state))
                continue;
            if (!glide::OnInputDesktop())
            {
                wasInputDesktop = false;
                continue;
            }

            ULONGLONG now = 0;
            if (!detail::Now(now))
                continue;
            if (!wasInputDesktop)
            {
                wasInputDesktop = true;
                desktopGraceUntil = now + detail::kHangTime100ns;
            }
            if (now < desktopGraceUntil)
                continue;
            const ULONGLONG beat = detail::ReadHeartbeat(state);
            if (now < beat || now - beat < detail::kHangTime100ns)
                continue;
            // Recheck the most recent state before acting, so a beat arriving at
            // the threshold or completed cleanup cancels recovery.
            const ULONGLONG latestBeat = detail::ReadHeartbeat(state);
            if (detail::Read(&state->clean) || !detail::NeedsRecovery(state) ||
                now < latestBeat || now - latestBeat < detail::kHangTime100ns)
                continue;

            // Terminate only the verified same-image, same-creation-time parent.
            // Stop its callbacks before restoring global state, so a resumed main
            // thread cannot immediately overwrite the recovery transform.
            if (!TerminateProcess(parent.Get(), 0xE0475A01u))
            {
                if (WaitForSingleObject(parent.Get(), 0) != WAIT_OBJECT_0)
                    return 8;
            }
            else
            {
                // TerminateProcess is asynchronous. Never restore global state
                // while any parent callback could still run and overwrite it.
                for (;;)
                {
                    const DWORD stopped = WaitForSingleObject(parent.Get(), 100);
                    if (stopped == WAIT_OBJECT_0)
                        break;
                    if (stopped != WAIT_TIMEOUT)
                        return 8;
                }
            }
            return detail::Recover(state);
        }
    }

} // namespace glide::watchdog
