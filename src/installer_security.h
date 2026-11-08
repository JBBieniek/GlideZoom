#pragma once
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <array>
#include <string>
#include "setup_policy.h"

namespace glide::installer {
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void Reset(HANDLE next = INVALID_HANDLE_VALUE) noexcept { if (*this) CloseHandle(value_); value_ = next; }
private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};
// Locks exact local paths against replacement while checking raw SID permissions.
// This is deliberately restrictive: an unexpected owner, junction or writer
// produces a visible failure instead of executing an elevated external script.
class SecurePaths {
public:
    SecurePaths() noexcept {
        ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &trustedInstaller_);
    }
    ~SecurePaths() { if (trustedInstaller_) LocalFree(trustedInstaller_); }
    SecurePaths(const SecurePaths&) = delete;
    SecurePaths& operator=(const SecurePaths&) = delete;
    bool Lock(const std::wstring& path, bool directory) {
        if (!trustedInstaller_ || path.size() < 3 || path.size() > 240 || path[1] != L':' || path[2] != L'\\' ||
            !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))) return false;
        for (std::size_t i = 2; i < path.size(); ++i)
            if (path[i] == L':' || path[i] == L'/' || path[i] == L'\0') return false;
        if (!One(path.substr(0, 3), true, true)) return false;
        std::size_t start = 3;
        while (start < path.size()) {
            auto end = path.find(L'\\', start);
            if (end == std::wstring::npos) end = path.size();
            const auto part = path.substr(start, end - start);
            if (part.empty() || part == L"." || part == L"..") return false;
            const bool final = end == path.size();
            if (!One(path.substr(0, end), !final || directory, false)) return false;
            start = end + 1;
        }
        return true;
    }
private:
    PSID trustedInstaller_ = nullptr;
    std::array<Handle, 64> locks_{};
    std::size_t count_ = 0;
    bool Privileged(PSID sid) const noexcept {
        return sid && IsValidSid(sid) && (IsWellKnownSid(sid, WinLocalSystemSid) ||
            IsWellKnownSid(sid, WinBuiltinAdministratorsSid) || EqualSid(sid, trustedInstaller_));
    }
    bool One(const std::wstring& path, bool directory, bool ancestor) noexcept {
        if (count_ == locks_.size()) return false;
        auto& lock = locks_[count_++];
        lock.Reset(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
            directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!lock || !GetFileInformationByHandle(lock.Get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) return false;
        PSID owner = nullptr; PACL acl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
        const auto error = GetSecurityInfo(lock.Get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner, nullptr, &acl, nullptr, &descriptor);
        bool allowed = error == ERROR_SUCCESS && Privileged(owner) && acl && IsValidAcl(acl) && acl->AceCount != 0;
        if (allowed) for (DWORD i = 0; i < acl->AceCount; ++i) {
            void* raw = nullptr;
            if (!GetAce(acl, i, &raw)) { allowed = false; break; }
            const auto* header = static_cast<const ACE_HEADER*>(raw);
            if ((header->AceFlags & INHERIT_ONLY_ACE) || header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { allowed = false; break; }
            const auto* rule = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            if (!setup::AllowedAce(setup::AceKind::Allow, rule->Mask, false,
                Privileged(const_cast<DWORD*>(&rule->SidStart)), ancestor)) { allowed = false; break; }
        }
        if (descriptor) LocalFree(descriptor);
        return allowed;
    }
};
} // namespace glide::installer
