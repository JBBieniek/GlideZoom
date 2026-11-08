#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace glide::setup {

inline constexpr std::size_t DirectoryCount = 3;
inline constexpr std::size_t PayloadCount = 6;
inline constexpr std::array<const wchar_t*, DirectoryCount> DirectoryNames{L"bin", L"scripts", L"resources"};
inline constexpr std::array<const wchar_t*, PayloadCount> PayloadNames{
    L"bin\\GlideZoom.exe", L"scripts\\Install.ps1", L"scripts\\Uninstall.ps1",
    L"Uninstall.cmd", L"resources\\install-security.txt", L"bin\\GlideZoom-Uninstall.exe"};
inline constexpr std::array<unsigned, PayloadCount> ResourceIds{101, 102, 103, 104, 105, 106};
inline constexpr std::array<std::size_t, PayloadCount> ResourceLimits{
    32u * 1024 * 1024, 1024u * 1024, 1024u * 1024, 64u * 1024, 1024u * 1024, 4u * 1024 * 1024};

struct Creation {
    bool created = false; // Ownership begins at successful CREATE_NEW, before writing.
    bool ready = false;
};
struct Completion {
    bool terminated = false;
    bool codeKnown = false;
    std::uint32_t code = 1;
};
enum class Phase { Preflight, Resources, Stage, Directories, Payloads, Launch, Wait, Finished };
struct Outcome {
    Phase phase = Phase::Preflight;
    bool canceled = false;
    bool childStarted = false;
    Completion child{};
    bool cleanupAttempted = false;
    bool cleanupComplete = true;

    std::uint32_t ExitCode() const noexcept {
        if (childStarted && child.terminated && child.codeKnown) return child.code;
        return canceled && !childStarted ? 2u : 1u;
    }
};

// The real bootstrap calls this exact orchestration. Native operations expose
// partial ownership explicitly. An uncertain running child keeps its payload;
// cleanup never guesses that a wait/query failure means process termination.
template<class Host>
Outcome Run(Host& host) noexcept {
    Outcome result{};
    bool stageOwned = false;
    std::array<bool, DirectoryCount> directories{};
    std::array<bool, PayloadCount> files{};
    auto canceled = [&]() noexcept {
        result.canceled = host.Canceled();
        return result.canceled;
    };
    auto clean = [&]() noexcept {
        if (!stageOwned) return;
        result.cleanupAttempted = true;
        for (std::size_t i = PayloadCount; i-- > 0;)
            if (files[i] && !host.RemovePayload(i)) result.cleanupComplete = false;
        for (std::size_t i = DirectoryCount; i-- > 0;)
            if (directories[i] && !host.RemoveDirectory(i)) result.cleanupComplete = false;
        if (!host.RemoveStage()) result.cleanupComplete = false;
    };
    if (canceled() || !host.Preflight()) return result;
    result.phase = Phase::Resources;
    if (canceled() || !host.LoadResources()) return result;
    result.phase = Phase::Stage;
    if (canceled()) return result;
    const auto stage = host.CreateStage();
    stageOwned = stage.created;
    if (!stage.ready || !stage.created) { clean(); return result; }
    result.phase = Phase::Directories;
    for (std::size_t i = 0; i < DirectoryCount; ++i) {
        if (canceled()) { clean(); return result; }
        const auto next = host.CreateChildDirectory(i);
        directories[i] = next.created;
        if (!next.ready || !next.created) { clean(); return result; }
    }
    result.phase = Phase::Payloads;
    for (std::size_t i = 0; i < PayloadCount; ++i) {
        if (canceled()) { clean(); return result; }
        const auto next = host.WritePayload(i);
        files[i] = next.created;
        if (!next.ready || !next.created) { clean(); return result; }
    }
    result.phase = Phase::Launch;
    if (canceled()) { clean(); return result; }
    result.childStarted = host.Launch();
    if (!result.childStarted) { clean(); return result; }
    result.phase = Phase::Wait;
    result.child = host.Wait();
    if (!result.child.terminated) {
        result.cleanupComplete = false;
        return result;
    }
    clean();
    result.phase = Phase::Finished;
    return result;
}

inline bool ValidResource(std::size_t index, const void* data, std::size_t bytes) noexcept {
    if (index >= PayloadCount || !data || bytes == 0 || bytes > ResourceLimits[index]) return false;
    if (index == 0 || index == 5) {
        const auto* raw = static_cast<const unsigned char*>(data);
        return bytes >= 64 && raw[0] == 'M' && raw[1] == 'Z';
    }
    return true;
}

enum class AceKind { Allow, Deny, Other };
inline bool AllowedAce(AceKind kind, std::uint32_t mask, bool inheritOnly,
                       bool privileged, bool ancestor) noexcept {
    if (inheritOnly || kind == AceKind::Deny) return true;
    if (kind != AceKind::Allow) return false;
    // Windows access masks. Inherit-only ACEs do not apply to the locked parent;
    // the new protected stage does not inherit them. C:\ may grant child
    // creation, but must not grant unprivileged deletion/ACL/owner changes.
    constexpr std::uint32_t dangerous = 0x00040000u | 0x00080000u | 0x00010000u |
        0x00000040u | 0x00000100u | 0x00000010u | 0x10000000u | 0x40000000u;
    const auto writes = dangerous | (ancestor ? 0u : 0x00000002u | 0x00000004u);
    return privileged || (mask & writes) == 0;
}

// Quote one Windows command-line argument, including trailing backslashes.
// The caller supplies lpApplicationName separately; no command shell is used.
inline std::wstring QuoteArgument(std::wstring_view value) {
    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (const auto ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') output.append(slashes * 2 + 1, L'\\');
        else output.append(slashes, L'\\');
        slashes = 0;
        output.push_back(ch);
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'\"');
    return output;
}
inline std::wstring PowerShellLiteral(std::wstring_view value) {
    std::wstring output = L"'";
    for (const auto ch : value) { output.push_back(ch); if (ch == L'\'') output.push_back(ch); }
    output.push_back(L'\'');
    return output;
}
inline std::wstring CommandLine(std::wstring_view executable, std::wstring_view script) {
    const std::wstring command = L"$env:PSModulePath = $PSHOME + '\\Modules'; $ErrorActionPreference = 'Stop'; & " +
        PowerShellLiteral(script) + L"; exit $LASTEXITCODE";
    return QuoteArgument(executable) + L" -NoLogo -NoProfile -ExecutionPolicy Bypass -Command " + QuoteArgument(command);
}

inline std::wstring GuiCommandLine(std::wstring_view executable, std::wstring_view script) {
    const std::wstring command = L"$env:PSModulePath = $PSHOME + '\\Modules'; $ErrorActionPreference = 'Stop'; [Console]::OutputEncoding = [Text.Encoding]::UTF8; & " +
        PowerShellLiteral(script) + L" -ApprovedLocalTrust -NonInteractive; exit $LASTEXITCODE";
    return QuoteArgument(executable) + L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command " + QuoteArgument(command);
}
inline std::wstring UninstallCommandLine(std::wstring_view executable, std::wstring_view script, std::uint32_t parent) {
    const std::wstring command = L"$env:PSModulePath = $PSHOME + '\\Modules'; $ErrorActionPreference = 'Stop'; & " +
        PowerShellLiteral(script) + L" -Gui -BootstrapProcessId " + std::to_wstring(parent) + L"; exit $LASTEXITCODE";
    return QuoteArgument(executable) + L" -NoLogo -NoProfile -NonInteractive -STA -ExecutionPolicy Bypass -Command " + QuoteArgument(command);
}

// Build from known-folder/API results only, never inherit PATH, PSModulePath,
// CLR profiler variables, user module directories, or arbitrary parent state.
// Names are in case-insensitive alphabetic order as required by CreateProcess.
inline std::vector<wchar_t> Environment(std::wstring_view windows, std::wstring_view system,
                                        std::wstring_view programFiles, std::wstring_view stage) {
    const std::wstring powershell = std::wstring(system) + L"\\WindowsPowerShell\\v1.0";
    const std::array<std::wstring, 10> items{
        L"ComSpec=" + std::wstring(system) + L"\\cmd.exe",
        L"PATH=" + std::wstring(system) + L";" + std::wstring(windows) + L";" + powershell,
        L"PROCESSOR_ARCHITECTURE=AMD64",
        L"ProgramFiles=" + std::wstring(programFiles),
        L"ProgramW6432=" + std::wstring(programFiles),
        L"PSModulePath=" + powershell + L"\\Modules",
        L"SystemRoot=" + std::wstring(windows),
        L"TEMP=" + std::wstring(stage), L"TMP=" + std::wstring(stage),
        L"WINDIR=" + std::wstring(windows)};
    std::vector<wchar_t> output;
    for (const auto& item : items) {
        output.insert(output.end(), item.begin(), item.end());
        output.push_back(L'\0');
    }
    output.push_back(L'\0');
    return output;
}

} // namespace glide::setup
