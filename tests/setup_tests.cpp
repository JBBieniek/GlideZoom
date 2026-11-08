#include "../src/setup_policy.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
std::uint64_t assertions = 0, cases = 0, boundaries = 0;
std::uint64_t cleanupRequested = 0, cleanupReached = 0;
void Check(bool condition, const char* why) {
    ++assertions;
    if (!condition) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}
using namespace glide::setup;
enum Op { Preflight, Resources, Stage, Dir0, Dir1, Dir2, File0, File1, File2, File3, File4, File5, Launch, Wait, None };
struct Fake {
    Op failure = None;
    bool partial = false, unknown = false, foreignStage = false;
    std::uint32_t cleanupFailures = 0, replacedFiles = 0;
    int cancelAt = -1, cancelCalls = 0;
    bool stage = false, started = false, terminated = false;
    std::array<bool, DirectoryCount> dirs{};
    std::array<bool, PayloadCount> files{};
    std::vector<Op> events;
    unsigned removedFiles = 0, removedDirs = 0, removedStage = 0;
    Completion completion{true, true, 0};
    bool Canceled() { return cancelAt >= 0 && cancelCalls++ >= cancelAt; }
    bool Hit(Op op) { events.push_back(op); if (failure == op) { ++boundaries; return true; } return false; }
    bool Preflight() { return !Hit(::Preflight); }
    bool LoadResources() { return !Hit(Resources); }
    Creation CreateStage() {
        Check(!stage, "new stage created once");
        if (foreignStage) { Hit(Stage); return {}; }
        const bool failed = Hit(Stage);
        stage = !failed || partial;
        return {stage, !failed};
    }
    Creation CreateChildDirectory(std::size_t i) {
        Check(stage && !dirs[i], "only create child of owned stage once");
        const bool failed = Hit(static_cast<Op>(Dir0 + i));
        dirs[i] = !failed || partial;
        return {dirs[i], !failed};
    }
    Creation WritePayload(std::size_t i) {
        Check(stage && !files[i], "payload created exactly once in owned stage");
        Check(std::all_of(dirs.begin(), dirs.end(), [](bool x) { return x; }), "directories precede payloads");
        const bool failed = Hit(static_cast<Op>(File0 + i));
        files[i] = !failed || partial;
        return {files[i], !failed};
    }
    bool Launch() {
        Check(std::all_of(files.begin(), files.end(), [](bool x) { return x; }), "no child with incomplete payload");
        started = !Hit(::Launch);
        return started;
    }
    Completion Wait() {
        Check(started, "wait only on started child");
        Hit(::Wait);
        terminated = completion.terminated;
        return completion;
    }
    bool RemovePayload(std::size_t i) {
        Check(stage && files[i], "never delete an unowned payload");
        Check(!started || terminated, "never delete payload before confirmed termination");
        ++removedFiles;
        if (cleanupFailures & (1u << i)) ++cleanupReached;
        if ((cleanupFailures | replacedFiles) & (1u << i)) return false;
        files[i] = false;
        return true;
    }
    bool RemoveDirectory(std::size_t i) {
        Check(stage && dirs[i], "never delete an unowned directory");
        Check(!started || terminated, "never delete directory before confirmed termination");
        ++removedDirs;
        if (cleanupFailures & (1u << (PayloadCount + i))) ++cleanupReached;
        if ((cleanupFailures & (1u << (PayloadCount + i))) ||
            (i == 0 && (files[0] || files[5])) || (i == 1 && (files[1] || files[2])) || (i == 2 && files[4])) return false;
        dirs[i] = false;
        return true;
    }
    bool RemoveStage() {
        Check(stage && !foreignStage, "never remove a preexisting stage collision");
        Check(!started || terminated, "never delete stage before confirmed termination");
        ++removedStage;
        if (cleanupFailures & (1u << (PayloadCount + DirectoryCount))) ++cleanupReached;
        if ((cleanupFailures & (1u << (PayloadCount + DirectoryCount))) || unknown ||
            std::any_of(files.begin(), files.end(), [](bool x) { return x; }) ||
            std::any_of(dirs.begin(), dirs.end(), [](bool x) { return x; })) return false;
        stage = false;
        return true;
    }
};
void Verify(Fake& fake) {
    for (unsigned bit = 0; bit < PayloadCount + DirectoryCount + 1; ++bit)
        if (fake.cleanupFailures & (1u << bit)) ++cleanupRequested;
    const auto result = Run(fake);
    ++cases;
    Check(result.childStarted == fake.started, "reported launch matches observed launch");
    Check(result.child.terminated == fake.terminated, "termination is observed, not inferred from exit code");
    if (!fake.stage) {
        Check(std::none_of(fake.files.begin(), fake.files.end(), [](bool x) { return x; }), "removed root has no owned files");
        Check(std::none_of(fake.dirs.begin(), fake.dirs.end(), [](bool x) { return x; }), "removed root has no owned directories");
    }
    if (fake.started && !fake.terminated) {
        Check(!result.cleanupAttempted && !result.cleanupComplete, "uncertain live child retains recovery payload");
        Check(fake.removedFiles == 0 && fake.removedDirs == 0 && fake.removedStage == 0, "no cleanup attempt on uncertain child");
        Check(result.ExitCode() == 1, "uncertain child cannot report success");
    } else if (fake.started && fake.completion.codeKnown) {
        Check(result.ExitCode() == fake.completion.code, "exact child exit code survives cleanup outcome");
        Check(result.phase == Phase::Finished, "confirmed exit finishes orchestration");
    } else if (result.canceled && !fake.started) Check(result.ExitCode() == 2, "early cancellation remains cancellation");
    else Check(result.ExitCode() == 1, "failed preparation or unreadable child code is failure");
    if (result.cleanupComplete && result.cleanupAttempted) Check(!fake.stage, "complete cleanup leaves no owned stage");
    if (fake.foreignStage) Check(fake.removedStage == 0 && !result.cleanupAttempted, "preexisting collision untouched");
}
void LifecycleMatrix() {
    // Every creation/write failure is exercised both before creation and after
    // owning a partial object. Cross cleanup failures rather than assume rollback.
    for (int op = Preflight; op <= Launch; ++op)
        for (bool partial : {false, true})
            for (unsigned cleanup = 0; cleanup < (1u << (PayloadCount + DirectoryCount + 1)); ++cleanup) {
                Fake fake;
                fake.failure = static_cast<Op>(op); fake.partial = partial; fake.cleanupFailures = cleanup;
                Verify(fake);
            }
    for (const auto code : {0u, 1u, 2u, 7u, 3010u, 0xffffffffu})
        for (unsigned cleanup = 0; cleanup < (1u << (PayloadCount + DirectoryCount + 1)); ++cleanup) {
            Fake fake; fake.completion.code = code; fake.cleanupFailures = cleanup; Verify(fake);
        }
    for (bool terminated : {false, true}) for (bool known : {false, true})
        for (const auto code : {0u, 1u, 2u, 0xffffffffu}) {
            Fake fake; fake.completion = {terminated, known, code}; Verify(fake);
        }
    for (int cancel = 0; cancel < 16; ++cancel)
        for (unsigned cleanup : {0u, 1u, 1u << (PayloadCount + DirectoryCount), (1u << (PayloadCount + DirectoryCount + 1)) - 1u}) {
            Fake fake; fake.cancelAt = cancel; fake.cleanupFailures = cleanup; Verify(fake);
        }
    for (unsigned replaced = 1; replaced < (1u << PayloadCount); ++replaced) {
        Fake fake; fake.replacedFiles = replaced; Verify(fake);
        for (unsigned i = 0; i < PayloadCount; ++i) if (replaced & (1u << i)) Check(fake.files[i], "identity replacement is retained");
    }
    { Fake fake; fake.unknown = true; Verify(fake); Check(fake.stage, "unknown files retain stage without recursion"); }
    { Fake fake; fake.foreignStage = true; Verify(fake); }
    // A malformed adapter response may not turn a preexisting directory into
    // owned staging authority. This defends the simple orchestration contract.
    struct Inconsistent : Fake { Creation CreateStage() { return {false, true}; } } invalid;
    const auto result = Run(invalid);
    Check(!result.childStarted && result.ExitCode() == 1 && invalid.events.size() == 2,
          "ready-without-creation cannot authorize staging");
}
void ResourceMatrix() {
    std::array<unsigned char, 64> data{}; data[0] = 'M'; data[1] = 'Z';
    for (std::size_t i = 0; i <= PayloadCount; ++i)
        for (const auto size : {std::size_t{0}, std::size_t{1}, std::size_t{63}, std::size_t{64},
                               std::size_t{65536}, std::size_t{65537}, std::size_t{1048576},
                               std::size_t{1048577}, std::size_t{33554432}, std::size_t{33554433},
                               std::numeric_limits<std::size_t>::max()})
            for (bool present : {false, true}) {
                ++cases;
                const bool expected = i < PayloadCount && present && size > 0 &&
                    size <= ResourceLimits[std::min(i, PayloadCount - 1)] && ((i != 0 && i != 5) || size >= 64);
                Check(ValidResource(i, present ? data.data() : nullptr, size) == expected, "bounded resource matrix");
            }
    data[0] = 'N'; Check(!ValidResource(0, data.data(), data.size()), "non-executable app payload rejected");
    Check(!ValidResource(5, data.data(), data.size()), "non-executable uninstaller payload rejected");
}
void AclMatrix() {
    // Independent list of rights; every single-bit grant combined with ACE
    // type, inheritance, writer identity and ancestor/target interpretation.
    const std::array<unsigned, 10> writeBits{1, 2, 4, 6, 8, 16, 18, 19, 28, 30};
    for (unsigned bit = 0; bit < 32; ++bit)
        for (auto kind : {AceKind::Allow, AceKind::Deny, AceKind::Other})
            for (bool inheritOnly : {false, true}) for (bool privileged : {false, true})
                for (bool ancestor : {false, true}) {
                    ++cases;
                    bool write = std::find(writeBits.begin(), writeBits.end(), bit) != writeBits.end();
                    if (ancestor && (bit == 1 || bit == 2)) write = false;
                    const bool expected = inheritOnly || kind == AceKind::Deny ||
                        (kind == AceKind::Allow && (privileged || !write));
                    Check(AllowedAce(kind, 1u << bit, inheritOnly, privileged, ancestor) == expected,
                          "raw access-mask policy matrix");
                }
    Check(AllowedAce(AceKind::Allow, 0x10000000, true, false, true), "inherit-only Creator Owner root ACE is harmless");
    Check(AllowedAce(AceKind::Allow, 0x4, false, false, true), "ordinary root create-folder ACE does not reject ancestor");
    Check(!AllowedAce(AceKind::Allow, 0x4, false, false, false), "same write right rejects unprivileged staging parent");
}
std::wstring DecodeWindowsArgument(const std::wstring& quoted) {
    std::wstring result;
    bool inQuotes = false;
    for (std::size_t i = 0; i < quoted.size();) {
        std::size_t slashes = 0;
        while (i < quoted.size() && quoted[i] == L'\\') { ++i; ++slashes; }
        if (i < quoted.size() && quoted[i] == L'"') {
            result.append(slashes / 2, L'\\');
            if (slashes % 2) result.push_back(L'"'); else inQuotes = !inQuotes;
            ++i;
        } else {
            result.append(slashes, L'\\');
            if (i < quoted.size()) {
                Check(inQuotes || (quoted[i] != L' ' && quoted[i] != L'\t'), "one encoded argument cannot split");
                result.push_back(quoted[i++]);
            }
        }
    }
    Check(!inQuotes, "command-line quotes are balanced");
    return result;
}
void TextBoundaries() {
    const std::wstring alphabet{L'a', L' ', L'\\', L'"', L'\'', L'$', L';', L'&'};
    std::wstring candidate;
    const auto visit = [&](const auto& self, unsigned depth) -> void {
        ++cases;
        Check(DecodeWindowsArgument(QuoteArgument(candidate)) == candidate, "quoted argument round-trips tricky characters");
        const auto literal = PowerShellLiteral(candidate);
        Check(literal.front() == L'\'' && literal.back() == L'\'', "PowerShell path is one literal string");
        std::wstring decoded;
        for (std::size_t i = 1; i + 1 < literal.size(); ++i) {
            decoded.push_back(literal[i]);
            if (literal[i] == L'\'') { Check(literal[i + 1] == L'\'', "embedded apostrophe is escaped"); ++i; }
        }
        Check(decoded == candidate, "PowerShell literal preserves exact path");
        if (!depth) return;
        for (auto ch : alphabet) { candidate.push_back(ch); self(self, depth - 1); candidate.pop_back(); }
    };
    visit(visit, 4);
    const auto command = CommandLine(L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
                                    L"C:\\Program Files\\GlideZoom.Setup-x\\scripts\\Install.ps1");
    Check(command.find(L"-NoLogo -NoProfile -ExecutionPolicy Bypass -Command") != std::wstring::npos,
          "trusted invocation disables profile loading");
    Check(command.find(L"$env:PSModulePath = $PSHOME + '\\Modules';") != std::wstring::npos,
          "module path is restricted before invoking production script");
    Check(command.find(L"$ErrorActionPreference = 'Stop';") != std::wstring::npos,
          "invocation failure cannot silently proceed");
    Check(command.find(L"; exit $LASTEXITCODE") != std::wstring::npos,
          "0/1/2 installer exit is propagated explicitly");
    const auto gui = GuiCommandLine(L"C:\\Windows\\powershell.exe", L"C:\\Program Files\\quoted ' & ; $\\Install.ps1");
    Check(gui.find(L"-NoProfile -NonInteractive") != std::wstring::npos, "GUI host cannot prompt or load profiles");
    Check(gui.find(L"-ApprovedLocalTrust -NonInteractive; exit $LASTEXITCODE") != std::wstring::npos, "GUI consent flags and exit code reach script");
    Check(gui.find(L"[Console]::OutputEncoding = [Text.Encoding]::UTF8;") != std::wstring::npos, "GUI log has explicit UTF8 encoding");
    for (std::uint32_t parent : {0u, 1u, 0xffffffffu}) {
        const auto uninstall = UninstallCommandLine(L"C:\\Windows\\powershell.exe", L"C:\\Program Files\\GlideZoom\\Uninstall.ps1", parent);
        Check(uninstall.find(L"-NonInteractive -STA -ExecutionPolicy Bypass") != std::wstring::npos, "uninstaller uses STA without console prompts");
        Check(uninstall.find(L" -Gui -BootstrapProcessId " + std::to_wstring(parent) + L"; exit $LASTEXITCODE") != std::wstring::npos, "uninstaller passes only numeric parent identity and preserves exit code");
    }
    const auto environment = Environment(L"C:\\Windows", L"C:\\Windows\\System32", L"C:\\Program Files", L"C:\\Program Files\\GlideZoom.Setup-x");
    Check(environment.size() > 2 && environment.back() == 0 && environment[environment.size() - 2] == 0,
          "Unicode environment is double-NUL terminated");
    std::vector<std::wstring> entries;
    for (std::size_t i = 0; i + 1 < environment.size();) {
        std::wstring entry(environment.data() + i);
        Check(!entry.empty(), "no early environment terminator");
        i += entry.size() + 1;
        entries.push_back(entry);
    }
    const std::array<std::wstring, 10> names{L"ComSpec",L"PATH",L"PROCESSOR_ARCHITECTURE",L"ProgramFiles",L"ProgramW6432",
                                          L"PSModulePath",L"SystemRoot",L"TEMP",L"TMP",L"WINDIR"};
    Check(entries.size() == names.size(), "explicit environment allowlist only");
    std::wstring previous;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Check(entries[i].substr(0, entries[i].find(L'=')) == names[i], "no inherited injection/module/profile variable");
        auto lowered = names[i];
        for (auto& ch : lowered) if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
        Check(previous.empty() || previous < lowered, "environment keys sorted case-insensitively");
        previous = lowered;
    }
    Check(entries[2] == L"PROCESSOR_ARCHITECTURE=AMD64", "production architecture preflight receives actual supported architecture");
    Check(entries[5] == L"PSModulePath=C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\Modules", "user module directories excluded");
    Check(entries[7] == L"TEMP=C:\\Program Files\\GlideZoom.Setup-x", "temporary child effects use protected stage");
}
} // namespace
int main() {
    LifecycleMatrix(); ResourceMatrix(); AclMatrix(); TextBoundaries();
    std::cout << "PASS: " << assertions << " setup assertions; " << cases
              << " matrix cases; " << boundaries << " reached injected preparation/launch boundaries; "
              << "actual orchestration, resource bounds, ACL policy, quoting and environment\n";
    std::cout << "Cleanup callback faults: " << cleanupRequested << " scheduled; " << cleanupReached
              << " reached; " << cleanupRequested - cleanupReached
              << " gated out because the object was not owned\n";
}
