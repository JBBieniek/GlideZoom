#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shldisp.h>
#include <exdisp.h>
#include <servprov.h>
#include <objbase.h>
#include <commctrl.h>
#include <cstring>
#include <algorithm>
#include "setup_ui_state.h"
#include "installer_security.h"
#ifdef ShellExecute
#undef ShellExecute
#endif

#include <array>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include "setup_policy.h"

namespace {

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) noexcept : value_(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { Reset(); }
    HANDLE Get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void Reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept {
        if (*this) CloseHandle(value_);
        value_ = h;
    }
private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};
class LocalMemory {
public:
    ~LocalMemory() { if (value) LocalFree(value); }
    LocalMemory() = default;
    LocalMemory(const LocalMemory&) = delete;
    LocalMemory& operator=(const LocalMemory&) = delete;
    void* value = nullptr;
};
struct Owned {
    Handle hold{};
    BY_HANDLE_FILE_INFORMATION identity{};
    bool identityKnown = false;
};

bool SameFile(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) noexcept {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
           a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

class NativeHost {
public:
    const wchar_t* Error() const noexcept { return error_; }
    DWORD ErrorCode() const noexcept { return errorCode_; }
    const std::wstring& StagePath() const noexcept { return stage_; }
    bool Canceled() const noexcept { return false; } // The wizard can cancel before Begin, never abandon its worker.
    ~NativeHost() { if (attributesReady_) DeleteProcThreadAttributeList(Attributes()); }
    const char* Log() const noexcept { return log_.data(); }
    std::size_t LogLength() const noexcept { return logSize_; }

    bool Preflight() noexcept {
        // Every C++ allocation is done before the first filesystem mutation.
        try { return Prepare(); }
        catch (...) { return Fail(L"Could not prepare the installer in memory", ERROR_NOT_ENOUGH_MEMORY); }
    }
    bool LoadResources() noexcept {
        const HMODULE self = GetModuleHandleW(nullptr);
        for (std::size_t i = 0; i < payloads_.size(); ++i) {
            const HRSRC resource = FindResourceW(self, MAKEINTRESOURCEW(glide::setup::ResourceIds[i]), RT_RCDATA);
            if (!resource) return Fail(L"An embedded installer payload is missing");
            const DWORD size = SizeofResource(self, resource);
            const HGLOBAL loaded = LoadResource(self, resource);
            const void* data = loaded ? LockResource(loaded) : nullptr;
            if (!glide::setup::ValidResource(i, data, size))
                return Fail(L"An embedded installer payload is invalid", ERROR_INVALID_DATA);
            payloads_[i] = {data, size};
        }
        return true;
    }
    glide::setup::Creation CreateStage() noexcept {
        return Create(stage_, stageObject_);
    }
    glide::setup::Creation CreateChildDirectory(std::size_t i) noexcept {
        if (i >= directories_.size()) return {};
        return Create(directoryPaths_[i], directories_[i]);
    }
    glide::setup::Creation WritePayload(std::size_t i) noexcept {
        if (i >= files_.size()) return {};
        const auto& path = filePaths_[i];
        Handle writer(CreateFileW(path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ, &security_, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (!writer) { Fail(L"Could not create a new embedded payload file"); return {}; }
        auto& owned = files_[i];
        if (!GetFileInformationByHandle(writer.Get(), &owned.identity)) {
            Fail(L"Could not identify a newly created payload file"); return {true, false};
        }
        owned.identityKnown = true;
        DWORD offset = 0;
        while (offset < payloads_[i].size) {
            DWORD written = 0;
            const auto* data = static_cast<const BYTE*>(payloads_[i].data) + offset;
            if (!WriteFile(writer.Get(), data, payloads_[i].size - offset, &written, nullptr) || written == 0) {
                Fail(L"Could not completely write an embedded payload file"); return {true, false};
            }
            offset += written;
        }
        if (!FlushFileBuffers(writer.Get())) { Fail(L"Could not flush an embedded payload file"); return {true, false}; }
        writer.Reset();
        // Keep a read-only, non-delete-shared handle while the script consumes
        // its source. This prevents modifying/replacing the staged payload.
        owned.hold.Reset(CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION observed{};
        if (!owned.hold || !GetFileInformationByHandle(owned.hold.Get(), &observed) ||
            !SameFile(owned.identity, observed) ||
            (observed.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            observed.nFileSizeHigh != 0 || observed.nFileSizeLow != payloads_[i].size ||
            !TrustedAcl(owned.hold.Get(), false, true)) {
            Fail(L"Could not verify and protect a staged payload"); return {true, false};
        }
        return {true, true};
    }
    bool Launch() noexcept {
        SECURITY_ATTRIBUTES inherited{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE reader = nullptr, writer = nullptr;
        if (!CreatePipe(&reader, &writer, &inherited, 0)) return Fail(L"Could not create the installer status channel");
        output_.Reset(reader);
        Handle outputWriter(writer);
        if (!SetHandleInformation(output_.Get(), HANDLE_FLAG_INHERIT, 0))
            return Fail(L"Could not protect the installer status channel");
        Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &inherited, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!input) return Fail(L"Could not prepare noninteractive installer input");
        std::array<HANDLE, 2> allowed{outputWriter.Get(), input.Get()};
        if (!UpdateProcThreadAttribute(Attributes(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            allowed.data(), sizeof(allowed), nullptr, nullptr))
            return Fail(L"Could not restrict child handle inheritance");
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = outputWriter.Get();
        startup.StartupInfo.hStdError = outputWriter.Get();
        startup.StartupInfo.hStdInput = input.Get();
        startup.lpAttributeList = Attributes();
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(powershell_.c_str(), command_.data(), nullptr, nullptr, TRUE,
            CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
            environment_.data(), stage_.c_str(), &startup.StartupInfo, &process))
            return Fail(L"Could not launch the embedded installer with Windows PowerShell");
        process_.Reset(process.hProcess);
        CloseHandle(process.hThread);
        return true;
    }
    glide::setup::Completion Wait() noexcept {
        for (;;) {
            DrainOutput();
            const DWORD waited = WaitForSingleObject(process_.Get(), 50);
            if (waited == WAIT_OBJECT_0) break;
            if (waited != WAIT_TIMEOUT) {
                Fail(L"Could not confirm installer termination; staging files were retained");
                return {};
            }
        }
        DrainOutput();
        DWORD code = 1;
        if (!GetExitCodeProcess(process_.Get(), &code)) {
            Fail(L"The installer exited, but its exit code could not be read");
            return {true, false, 1};
        }
        return {true, true, code};
    }
    bool RemovePayload(std::size_t i) noexcept { return Remove(filePaths_[i], files_[i], false); }
    bool RemoveDirectory(std::size_t i) noexcept { return Remove(directoryPaths_[i], directories_[i], true); }
    bool RemoveStage() noexcept { return Remove(stage_, stageObject_, true); }

private:
    struct Payload { const void* data = nullptr; DWORD size = 0; };
    std::wstring stage_, programFiles_, system_, windows_, powershell_, command_;
    std::array<std::wstring, glide::setup::DirectoryCount> directoryPaths_{};
    std::array<std::wstring, glide::setup::PayloadCount> filePaths_{};
    std::vector<wchar_t> environment_{};
    LocalMemory descriptor_{}, trustedInstaller_{};
    SECURITY_ATTRIBUTES security_{};
    std::array<Payload, glide::setup::PayloadCount> payloads_{};
    Owned stageObject_{};
    std::array<Owned, glide::setup::DirectoryCount> directories_{};
    std::array<Owned, glide::setup::PayloadCount> files_{};
    std::array<Handle, 64> pathLocks_{};
    std::size_t lockCount_ = 0;
    Handle process_{}, output_{};
    std::vector<unsigned char> attributes_{};
    bool attributesReady_ = false;
    std::array<char, 65536> log_{};
    std::size_t logSize_ = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST Attributes() noexcept {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_.data());
    }
    void DrainOutput() noexcept {
        if (!output_) return;
        std::array<char, 4096> chunk{};
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(output_.Get(), nullptr, 0, nullptr, &available, nullptr) || available == 0) return;
            DWORD read = 0;
            if (!ReadFile(output_.Get(), chunk.data(), std::min<DWORD>(available, static_cast<DWORD>(chunk.size())), &read, nullptr) || !read) return;
            const auto incoming = static_cast<std::size_t>(read);
            if (logSize_ + incoming >= log_.size()) {
                const auto discard = logSize_ + incoming - (log_.size() - 1);
                std::memmove(log_.data(), log_.data() + discard, logSize_ - discard);
                logSize_ -= discard;
            }
            std::memcpy(log_.data() + logSize_, chunk.data(), incoming);
            logSize_ += incoming; log_[logSize_] = 0;
        }
    }
    const wchar_t* error_ = L"Installer preparation failed";
    DWORD errorCode_ = ERROR_GEN_FAILURE;

    bool Fail(const wchar_t* message, DWORD code = ERROR_SUCCESS) noexcept {
        error_ = message;
        errorCode_ = code ? code : GetLastError();
        if (!errorCode_) errorCode_ = ERROR_GEN_FAILURE;
        return false;
    }
    bool Privileged(PSID sid) const noexcept {
        return sid && IsValidSid(sid) &&
            (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid) ||
             (trustedInstaller_.value && EqualSid(sid, trustedInstaller_.value)));
    }
    bool TrustedAcl(HANDLE object, bool ancestor, bool requireProtected = false) noexcept {
        PSID owner = nullptr;
        PACL acl = nullptr;
        LocalMemory memory;
        auto** descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR*>(&memory.value);
        const DWORD status = GetSecurityInfo(object, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                             &owner, nullptr, &acl, nullptr, descriptor);
        if (status != ERROR_SUCCESS || !Privileged(owner) || !acl || !IsValidAcl(acl) || acl->AceCount == 0)
            return Fail(L"A required path does not have verifiable privileged ownership and permissions",
                        status == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : status);
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision = 0;
        if (requireProtected && (!GetSecurityDescriptorControl(memory.value, &control, &revision) ||
                                 !(control & SE_DACL_PROTECTED)))
            return Fail(L"The staging permissions did not remain protected", ERROR_ACCESS_DENIED);
        for (DWORD i = 0; i < acl->AceCount; ++i) {
            void* raw = nullptr;
            if (!GetAce(acl, i, &raw)) return Fail(L"Could not inspect required directory permissions");
            const auto* header = static_cast<const ACE_HEADER*>(raw);
            if (header->AceFlags & INHERIT_ONLY_ACE) continue;
            if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
                return Fail(L"An unsupported permission rule blocks secure staging", ERROR_ACCESS_DENIED);
            const auto* rule = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            const auto sid = const_cast<DWORD*>(&rule->SidStart);
            if (!glide::setup::AllowedAce(glide::setup::AceKind::Allow, rule->Mask,
                                         false, Privileged(sid), ancestor))
                return Fail(L"An unprivileged writer has access to an installer path", ERROR_ACCESS_DENIED);
        }
        return true;
    }
    bool LockPath(const std::wstring& path) {
        // Known-folder paths must be local absolute paths. Reject alternate
        // streams, relative components and reparse points rather than resolving
        // a user-controlled junction while elevated.
        if (path.size() < 3 || path.size() > 200 || path[1] != L':' || path[2] != L'\\' ||
            !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')))
            return Fail(L"A required Windows path is not a supported local absolute path", ERROR_BAD_PATHNAME);
        for (std::size_t i = 2; i < path.size(); ++i)
            if (path[i] == L':' || path[i] == L'/' || path[i] == L'\0')
                return Fail(L"A required Windows path contains an unsupported component", ERROR_BAD_PATHNAME);
        std::size_t length = 3;
        while (true) {
            if (lockCount_ == pathLocks_.size()) return Fail(L"Too many directory components", ERROR_BAD_PATHNAME);
            const std::wstring prefix = path.substr(0, length); // Preflight only: no owned files yet.
            Handle& lock = pathLocks_[lockCount_++];
            lock.Reset(CreateFileW(prefix.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION information{};
            if (!lock || !GetFileInformationByHandle(lock.Get(), &information) ||
                !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                return Fail(L"A required Windows directory is unavailable or a reparse point");
            if (!TrustedAcl(lock.Get(), length != path.size())) return false;
            if (length == path.size()) break;
            const std::size_t begin = length == 3 ? 3 : length + 1;
            const auto separator = path.find(L'\\', begin);
            const auto end = separator == std::wstring::npos ? path.size() : separator;
            const auto count = end - begin;
            if (count == 0 || (count == 1 && path[begin] == L'.') ||
                (count == 2 && path[begin] == L'.' && path[begin + 1] == L'.'))
                return Fail(L"A required Windows directory has a relative or empty component", ERROR_BAD_PATHNAME);
            length = end;
        }
        return true;
    }
    bool Prepare() {
        static_assert(sizeof(void*) == 8, "The setup executable must be native x64.");
        SYSTEM_INFO info{};
        GetNativeSystemInfo(&info);
        if (info.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64)
            return Fail(L"GlideZoom Setup requires native x64 Windows", ERROR_NOT_SUPPORTED);
        Handle token;
        HANDLE opened = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &opened)) return Fail(L"Could not inspect installer elevation");
        token.Reset(opened);
        TOKEN_ELEVATION elevation{};
        DWORD bytes = 0;
        if (!GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &bytes) || !elevation.TokenIsElevated)
            return Fail(L"Run GlideZoom-Setup.exe and approve its UAC prompt", ERROR_ELEVATION_REQUIRED);
        if (!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",
                                    reinterpret_cast<PSID*>(&trustedInstaller_.value)))
            return Fail(L"Could not prepare the trusted Windows service identity");
        PWSTR known = nullptr;
        const HRESULT folder = SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &known);
        if (FAILED(folder) || !known) { if (known) CoTaskMemFree(known); return Fail(L"Program Files is unavailable", ERROR_PATH_NOT_FOUND); }
        try { programFiles_ = known; } catch (...) { CoTaskMemFree(known); throw; }
        CoTaskMemFree(known);
        std::array<wchar_t, 32768> buffer{};
        const UINT systemLength = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        if (!systemLength || systemLength >= buffer.size()) return Fail(L"The Windows system directory is unavailable");
        system_.assign(buffer.data(), systemLength);
        const UINT windowsLength = GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        if (!windowsLength || windowsLength >= buffer.size()) return Fail(L"The Windows directory is unavailable");
        windows_.assign(buffer.data(), windowsLength);
        if (!LockPath(programFiles_) || !LockPath(system_ + L"\\WindowsPowerShell\\v1.0")) return false;
        const std::wstring volume = programFiles_.substr(0, 3);
        DWORD volumeFlags = 0;
        if (GetDriveTypeW(volume.c_str()) != DRIVE_FIXED ||
            !GetVolumeInformationW(volume.c_str(), nullptr, 0, nullptr, nullptr, &volumeFlags, nullptr, 0) ||
            !(volumeFlags & FS_PERSISTENT_ACLS))
            return Fail(L"Secure staging requires a fixed local volume with persistent ACLs", ERROR_NOT_SUPPORTED);
        powershell_ = system_ + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        Handle executable(CreateFileW(powershell_.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION executableInfo{};
        if (!executable || !GetFileInformationByHandle(executable.Get(), &executableInfo) ||
            (executableInfo.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !TrustedAcl(executable.Get(), false)) return Fail(L"The trusted Windows PowerShell executable is unavailable");
        // Keep its path protected while launching; it is a verified system file.
        if (lockCount_ == pathLocks_.size()) return Fail(L"Too many protected path handles", ERROR_BAD_PATHNAME);
        HANDLE retained = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), executable.Get(), GetCurrentProcess(), &retained, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return Fail(L"Could not retain the verified PowerShell executable");
        pathLocks_[lockCount_++].Reset(retained);
        GUID id{};
        wchar_t name[40]{};
        if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, name, static_cast<int>(std::size(name))))
            return Fail(L"Could not allocate a unique staging identity", ERROR_GEN_FAILURE);
        stage_ = programFiles_ + L"\\GlideZoom.Setup-" + name;
        for (std::size_t i = 0; i < directoryPaths_.size(); ++i) directoryPaths_[i] = stage_ + L"\\" + glide::setup::DirectoryNames[i];
        for (std::size_t i = 0; i < filePaths_.size(); ++i) filePaths_[i] = stage_ + L"\\" + glide::setup::PayloadNames[i];
        command_ = glide::setup::GuiCommandLine(powershell_, filePaths_[1]);
        environment_ = glide::setup::Environment(windows_, system_, programFiles_, stage_);
        if (command_.size() >= 32767 || stage_.size() > 240)
            return Fail(L"The installation paths exceed the supported length", ERROR_BAD_PATHNAME);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
            reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor_.value), nullptr))
            return Fail(L"Could not construct protected staging permissions");
        security_.nLength = sizeof(security_);
        security_.lpSecurityDescriptor = descriptor_.value;
        security_.bInheritHandle = FALSE;
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        if (!attributeBytes) return Fail(L"Could not size restricted process attributes");
        attributes_.resize(attributeBytes);
        if (!InitializeProcThreadAttributeList(Attributes(), 1, 0, &attributeBytes))
            return Fail(L"Could not prepare restricted process attributes");
        attributesReady_ = true;
        return true;
    }
    glide::setup::Creation Create(const std::wstring& path, Owned& owned) noexcept {
        if (!CreateDirectoryW(path.c_str(), &security_)) { Fail(L"Could not create a new protected staging directory"); return {}; }
        owned.hold.Reset(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!owned.hold || !GetFileInformationByHandle(owned.hold.Get(), &owned.identity)) {
            Fail(L"Could not identify and protect a new staging directory"); return {true, false};
        }
        owned.identityKnown = true;
        if (!(owned.identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (owned.identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !TrustedAcl(owned.hold.Get(), false, true)) {
            Fail(L"The newly created staging directory is not secure"); return {true, false};
        }
        return {true, true};
    }
    bool Remove(const std::wstring& path, Owned& owned, bool directory) noexcept {
        // No recursive deletion and no wildcard/stale-stage cleanup. A name
        // replaced by another object is retained, even if it is empty.
        if (!owned.identityKnown) return false;
        owned.hold.Reset();
        Handle removal(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
        if (!removal) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        BY_HANDLE_FILE_INFORMATION observed{};
        if (!GetFileInformationByHandle(removal.Get(), &observed) || !SameFile(observed, owned.identity) ||
            (observed.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            ((observed.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) return false;
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        return SetFileInformationByHandle(removal.Get(), FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
    }
};
template<class T> class ComPointer {
public:
    ~ComPointer() { if (value) value->Release(); }
    T* operator->() const noexcept { return value; }
    T* value = nullptr;
};
class ComApartment {
public:
    ComApartment() noexcept : result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    HRESULT result;
};
class Variant {
public:
    Variant() noexcept { VariantInit(&value); }
    explicit Variant(const wchar_t* text) noexcept { VariantInit(&value); value.vt = VT_BSTR; value.bstrVal = SysAllocString(text); }
    ~Variant() { VariantClear(&value); }
    VARIANT value{};
};
// Delegate to the existing Explorer desktop rather than create a child with
// the elevated setup token. All COM failures are explicit failures; there is
// no elevated fallback. See Microsoft's desktop-shell automation explanation:
// https://devblogs.microsoft.com/oldnewthing/20131118-00/?p=2643
bool OpenAppFromDesktop(const std::wstring& executable) noexcept {
    glide::installer::SecurePaths paths;
    try { if (!paths.Lock(executable, false)) return false; } catch (...) { return false; }
    ComApartment apartment;
    if (FAILED(apartment.result)) return false;
    ComPointer<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
        IID_IShellWindows, reinterpret_cast<void**>(&windows.value))) || !windows.value) return false;
    Variant desktop, reserved;
    desktop.value.vt = VT_I4; desktop.value.lVal = CSIDL_DESKTOP;
    long desktopWindow = 0;
    ComPointer<IDispatch> dispatch;
    if (windows->FindWindowSW(&desktop.value, &reserved.value, SWC_DESKTOP, &desktopWindow,
        SWFO_NEEDDISPATCH, &dispatch.value) != S_OK || !dispatch.value) return false;
    // A nonstandard elevated Explorer shell is not an unelevated launch broker.
    // Inspect the desktop process rather than assume every shell token is safe.
    const HWND desktopHandle = reinterpret_cast<HWND>(static_cast<INT_PTR>(desktopWindow));
    DWORD desktopPid = 0, shellPid = 0;
    GetWindowThreadProcessId(desktopHandle, &desktopPid);
    GetWindowThreadProcessId(GetShellWindow(), &shellPid);
    if (!desktopPid || desktopPid != shellPid) return false;
    Handle desktopProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, desktopPid));
    HANDLE rawToken = nullptr;
    if (!desktopProcess || !OpenProcessToken(desktopProcess.Get(), TOKEN_QUERY, &rawToken)) return false;
    Handle desktopToken(rawToken);
    TOKEN_ELEVATION elevation{}; DWORD tokenBytes = 0;
    if (!GetTokenInformation(desktopToken.Get(), TokenElevation, &elevation, sizeof(elevation), &tokenBytes) ||
        elevation.TokenIsElevated) return false;
    ComPointer<IServiceProvider> service;
    ComPointer<IShellBrowser> browser;
    ComPointer<IShellView> view;
    ComPointer<IDispatch> viewDispatch;
    ComPointer<IShellFolderViewDual> folder;
    ComPointer<IDispatch> application;
    ComPointer<IShellDispatch2> shell;
    if (FAILED(dispatch->QueryInterface(IID_IServiceProvider, reinterpret_cast<void**>(&service.value))) || !service.value ||
        FAILED(service->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, reinterpret_cast<void**>(&browser.value))) || !browser.value ||
        FAILED(browser->QueryActiveShellView(&view.value)) || !view.value ||
        FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch, reinterpret_cast<void**>(&viewDispatch.value))) || !viewDispatch.value ||
        FAILED(viewDispatch->QueryInterface(IID_IShellFolderViewDual, reinterpret_cast<void**>(&folder.value))) || !folder.value ||
        FAILED(folder->get_Application(&application.value)) || !application.value ||
        FAILED(application->QueryInterface(IID_IShellDispatch2, reinterpret_cast<void**>(&shell.value))) || !shell.value) return false;
    Variant file(executable.c_str()), arguments(L""), directory(L""), operation(L"open"), show;
    if (!file.value.bstrVal || !arguments.value.bstrVal || !directory.value.bstrVal || !operation.value.bstrVal) return false;
    show.value.vt = VT_I4; show.value.lVal = SW_SHOWNORMAL;
    return SUCCEEDED(shell->ShellExecute(file.value.bstrVal, arguments.value, directory.value, operation.value, show.value));
}

constexpr UINT WorkerDone = WM_APP + 31;
constexpr int NextId = 1001, BackId = 1002, CancelId = 1003, ConsentId = 1004, FolderId = 1005, OpenAppId = 1006;
class Wizard {
public:
    explicit Wizard(std::wstring path, bool existing) : state_(existing), installPath_(std::move(path)) {}
    ~Wizard() { if (font_) DeleteObject(font_); if (headingFont_) DeleteObject(headingFont_); }
    int Run(HINSTANCE instance) {
        instance_ = instance;
        WNDCLASSW type{}; type.lpfnWndProc = Dispatch; type.hInstance = instance;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); type.lpszClassName = L"GlideZoomSetupWizard";
        if (!RegisterClassW(&type)) return FailedUi();
        HDC screen = GetDC(nullptr);
        dpi_ = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
        if (screen) ReleaseDC(nullptr, screen);
        RECT frame{0, 0, Scale(680), Scale(520)};
        AdjustWindowRectEx(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
        window_ = CreateWindowExW(0, type.lpszClassName, L"GlideZoom Setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance, this);
        if (!window_) return FailedUi();
        ShowWindow(window_, SW_SHOW); UpdateWindow(window_);
        MSG message{};
        int received = 0;
        while ((received = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
            if (!IsDialogMessageW(window_, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        // Even a message-loop failure must not abandon a running elevated child.
        if (worker_ && WaitForSingleObject(worker_.Get(), INFINITE) != WAIT_OBJECT_0) return 1;
        return received < 0 ? 1 : static_cast<int>(state_.ExitCode());
    }
private:
    glide::setup::WizardState state_;
    std::wstring installPath_, resultDetails_;
    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr, heading_ = nullptr, body_ = nullptr, consent_ = nullptr,
        progress_ = nullptr, details_ = nullptr, next_ = nullptr, back_ = nullptr, cancel_ = nullptr, folder_ = nullptr, openApp_ = nullptr;
    HFONT font_ = nullptr, headingFont_ = nullptr;
    int dpi_ = 96;
    NativeHost host_;
    Handle worker_;
    glide::setup::Outcome outcome_{};
    int Scale(int value) const noexcept { return MulDiv(value, dpi_, 96); }
    int FailedUi() const noexcept {
        MessageBoxW(nullptr, L"The setup window could not be created. No installation was started.", L"GlideZoom Setup", MB_OK | MB_ICONERROR);
        return 1;
    }
    HWND Control(const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id = 0) {
        HWND child = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, Scale(x), Scale(y), Scale(width), Scale(height),
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!child) throw 1;
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return child;
    }
    void CreateControls() {
        font_ = CreateFontW(-MulDiv(10, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        headingFont_ = CreateFontW(-MulDiv(18, dpi_, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font_ || !headingFont_) throw 1;
        heading_ = Control(L"STATIC", L"", 0, 28, 25, 624, 42);
        SendMessageW(heading_, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);
        body_ = Control(L"STATIC", L"", 0, 28, 83, 624, 269);
        consent_ = Control(L"BUTTON", L"I trust this build and approve local signing and certificate trust.",
            BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, 28, 354, 624, 43, ConsentId);
        progress_ = Control(PROGRESS_CLASSW, L"", PBS_MARQUEE, 28, 350, 624, 22);
        details_ = Control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER | WS_TABSTOP,
            28, 220, 624, 190);
        folder_ = Control(L"BUTTON", L"Open installation folder", BS_PUSHBUTTON | WS_TABSTOP, 28, 422, 205, 30, FolderId);
        openApp_ = Control(L"BUTTON", L"Open GlideZoom", BS_PUSHBUTTON | WS_TABSTOP, 246, 422, 165, 30, OpenAppId);
        back_ = Control(L"BUTTON", L"< Back", BS_PUSHBUTTON | WS_TABSTOP, 332, 472, 100, 30, BackId);
        next_ = Control(L"BUTTON", L"Next >", BS_DEFPUSHBUTTON | WS_TABSTOP, 442, 472, 100, 30, NextId);
        cancel_ = Control(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 552, 472, 100, 30, CancelId);
        ShowPage();
    }
    void ShowPage() {
        using glide::setup::Page;
        const auto page = state_.Current();
        ShowWindow(consent_, page == Page::Consent ? SW_SHOW : SW_HIDE);
        ShowWindow(progress_, page == Page::Working ? SW_SHOW : SW_HIDE);
        ShowWindow(details_, page == Page::Failure || page == Page::Success ? SW_SHOW : SW_HIDE);
        ShowWindow(folder_, page == Page::Existing || page == Page::Success ? SW_SHOW : SW_HIDE);
        ShowWindow(openApp_, page == Page::Success ? SW_SHOW : SW_HIDE);
        ShowWindow(back_, page == Page::Consent ? SW_SHOW : SW_HIDE);
        EnableWindow(cancel_, page != Page::Working);
        EnableWindow(next_, page != Page::Working && (page != Page::Consent || state_.Approved()));
        SetWindowTextW(next_, page == Page::Consent ? L"Install" :
            page == Page::Welcome ? L"Next >" : page == Page::Working ? L"Installing..." : L"Finish");
        if (page == Page::Welcome) {
            SetWindowTextW(heading_, L"Welcome to GlideZoom");
            const auto text = L"Setup will install GlideZoom 0.6.2 for this computer.\r\n\r\nDestination:\r\n" + installPath_ +
                L"\r\n\r\nYou can remove the app later in Windows Settings > Apps > Installed apps.\r\n\r\nThe next page explains the local signing and certificate trust required by this development build.\r\n\r\nNo shortcuts or automatic startup are added.";
            SetWindowTextW(body_, text.c_str());
        } else if (page == Page::Consent) {
            SetWindowTextW(heading_, L"Approve local development trust");
            SetWindowTextW(body_, L"This build does not have a verified publisher signature. Windows UIAccess requires a trusted signature and a protected installation location.\r\n\r\nSetup creates one unique, nonexportable code-signing key, trusts its PUBLIC certificate in this computer's Root store, and signs two installed files: GlideZoom.exe and GlideZoom-Uninstall.exe. It then deletes the private signing key.\r\n\r\nThe public trust remains until uninstall and expires in approximately one year. This is local development trust; it is not publisher verification.\r\n\r\nSetup adds the app to Installed apps. It does not change UAC policy, add startup entries or shortcuts, or download anything.");
            SendMessageW(consent_, BM_SETCHECK, state_.Approved() ? BST_CHECKED : BST_UNCHECKED, 0);
        } else if (page == Page::Working) {
            SetWindowTextW(heading_, L"Installing GlideZoom");
            SetWindowTextW(body_, L"Preparing protected files, creating local signing trust, verifying both installed executables, and registering Installed apps removal.\r\n\r\nPlease keep this window open. Setup will report the result when the installation or its recovery cleanup has finished.");
            SendMessageW(progress_, PBM_SETMARQUEE, TRUE, 30);
        } else if (page == Page::Existing) {
            SetWindowTextW(heading_, L"An installation already exists");
            const auto text = L"Setup found:\r\n" + installPath_ +
                L"\r\n\r\nRemove the existing version before installing this version.\r\n\r\nFor recent versions, use Settings > Apps > Installed apps > GlideZoom. For older versions, open the folder below and double-click Uninstall.cmd.\r\n\r\nKeep the installation manifest until removal finishes. Your per-user preferences are kept by the uninstaller. Then run this setup again.\r\n\r\nSetup has not overwritten any installed files.";
            SetWindowTextW(body_, text.c_str());
        } else if (page == Page::Success) {
            SetWindowTextW(heading_, L"GlideZoom is installed");
            SetWindowTextW(body_, L"Both installed executables passed signature verification and the private signing key was deleted.\r\n\r\nClick Open GlideZoom to start the app. You can also open the installation folder and double-click GlideZoom.exe. Uninstall through Windows Settings > Apps > Installed apps.");
            SetWindowTextW(details_, resultDetails_.c_str());
        } else if (page == Page::Failure) {
            SetWindowTextW(heading_, L"Setup did not complete");
            SetWindowTextW(body_, L"The installer reported the details below. If cleanup was incomplete, keep the installation folder and its manifest so the uninstaller can retry the exact recorded cleanup.");
            SetWindowTextW(details_, resultDetails_.c_str());
        }
        if (page != Page::Working) SendMessageW(progress_, PBM_SETMARQUEE, FALSE, 0);
        InvalidateRect(window_, nullptr, TRUE);
    }
    static DWORD WINAPI Work(void* opaque) noexcept {
        auto* self = static_cast<Wizard*>(opaque);
        self->outcome_ = glide::setup::Run(self->host_);
        PostMessageW(self->window_, WorkerDone, 0, 0);
        return 0;
    }
    void Begin() {
        if (!state_.Begin()) return;
        ShowPage();
        worker_.Reset(CreateThread(nullptr, 0, Work, this, 0, nullptr));
        if (!worker_) {
            resultDetails_ = L"The setup worker could not start. No installation was attempted.";
            state_.Complete(1); ShowPage();
        } else SetTimer(window_, 1, 100, nullptr); // Also observes completion if PostMessage fails.
    }
    void Complete() {
        if (!state_.Busy() || !worker_ || WaitForSingleObject(worker_.Get(), 0) != WAIT_OBJECT_0) return;
        KillTimer(window_, 1);
        if (!outcome_.childStarted || !outcome_.child.terminated || !outcome_.child.codeKnown) {
            resultDetails_ = host_.Error(); resultDetails_ += L" (Windows error ";
            resultDetails_ += std::to_wstring(host_.ErrorCode()) + L").\r\n\r\n";
        }
        if (host_.LogLength()) {
            const int needed = MultiByteToWideChar(CP_UTF8, 0, host_.Log(), static_cast<int>(host_.LogLength()), nullptr, 0);
            if (needed > 0) {
                std::wstring log(static_cast<std::size_t>(needed), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, host_.Log(), static_cast<int>(host_.LogLength()), log.data(), needed);
                resultDetails_ += log;
            }
        }
        if (!outcome_.cleanupComplete) {
            resultDetails_ += L"\r\nTemporary setup files were retained at:\r\n" + host_.StagePath() +
                L"\r\nKeep this separate setup folder while any installer may still be running. No unknown files were recursively removed.";
        }
        state_.Complete(outcome_.ExitCode()); ShowPage();
        SetFocus(next_);
    }
    void OpenFolder() const {
        if (state_.Current() != glide::setup::Page::Existing && state_.Current() != glide::setup::Page::Success) return;
        glide::installer::SecurePaths paths;
        if (!paths.Lock(installPath_, true)) {
            MessageBoxW(window_, L"The installation folder or its permissions could not be verified. Keep its files and manifest, and inspect Program Files\\GlideZoom in Explorer before retrying removal.", L"GlideZoom Setup", MB_OK | MB_ICONINFORMATION);
            return;
        }
        SHELLEXECUTEINFOW open{}; open.cbSize = sizeof(open); open.hwnd = window_;
        open.lpVerb = L"open"; open.lpFile = installPath_.c_str(); open.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&open)) MessageBoxW(window_, L"The installation folder could not be opened. Open Program Files\\GlideZoom in Explorer.", L"GlideZoom Setup", MB_OK | MB_ICONINFORMATION);
    }
    LRESULT Message(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: CreateControls(); return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IDOK:
            case NextId:
                if (state_.Current() == glide::setup::Page::Welcome) { state_.Next(); ShowPage(); SetFocus(consent_); }
                else if (state_.Current() == glide::setup::Page::Consent) Begin();
                else if (state_.Close()) DestroyWindow(window_);
                return 0;
            case BackId: if (state_.Back()) { ShowPage(); SetFocus(next_); } return 0;
            case IDCANCEL:
            case CancelId: if (state_.Close()) DestroyWindow(window_); return 0;
            case ConsentId:
                if (state_.Consent(SendMessageW(consent_, BM_GETCHECK, 0, 0) == BST_CHECKED))
                    EnableWindow(next_, state_.Approved());
                return 0;
            case FolderId: OpenFolder(); return 0;
            case OpenAppId:
                if (state_.CanOpenApp() &&
                    !OpenAppFromDesktop(installPath_ + L"\\GlideZoom.exe"))
                    MessageBoxW(window_, L"Explorer could not start GlideZoom. Open the installation folder and double-click GlideZoom.exe after closing setup.", L"GlideZoom Setup", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            break;
        case WorkerDone: Complete(); return 0;
        case WM_TIMER: if (wParam == 1) Complete(); return 0;
        case WM_CLOSE: if (state_.Close()) DestroyWindow(window_); return 0;
        case WM_QUERYENDSESSION: return state_.Busy() ? FALSE : TRUE;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }
    static LRESULT CALLBACK Dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
        auto* self = reinterpret_cast<Wizard*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Wizard*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        try { return self->Message(message, wParam, lParam); }
        catch (...) {
            if (message == WM_CREATE) return -1;
            MessageBoxW(window, L"Setup could not update its window. Any running installer is still being allowed to finish safely.", L"GlideZoom Setup", MB_OK | MB_ICONERROR);
            return 0;
        }
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int) {
    if (arguments && *arguments) {
        MessageBoxW(nullptr, L"Run GlideZoom-Setup.exe without additional command-line arguments.", L"GlideZoom Setup", MB_OK | MB_ICONERROR);
        return 1;
    }
    try {
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_PROGRESS_CLASS};
        if (!InitCommonControlsEx(&common)) {
            MessageBoxW(nullptr, L"Windows could not initialize the setup controls. No installation was started.", L"GlideZoom Setup", MB_OK | MB_ICONERROR);
            return 1;
        }
        PWSTR known = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &known)) || !known) {
            if (known) CoTaskMemFree(known);
            MessageBoxW(nullptr, L"The Windows Program Files folder is unavailable. No installation was started.", L"GlideZoom Setup", MB_OK | MB_ICONERROR);
            return 1;
        }
        std::wstring path;
        try { path = std::wstring(known) + L"\\GlideZoom"; } catch (...) { CoTaskMemFree(known); throw; }
        CoTaskMemFree(known);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        const bool existing = attributes != INVALID_FILE_ATTRIBUTES || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND);
        Wizard wizard(path, existing);
        return wizard.Run(instance);
    } catch (...) {
        MessageBoxW(nullptr, L"Setup could not prepare its window. No installation was started.", L"GlideZoom Setup", MB_OK | MB_ICONERROR);
        return 1;
    }
}
