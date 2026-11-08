#include "../src/setup_ui_state.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
using glide::setup::Page;
using glide::setup::WizardState;
namespace {
std::uint64_t checks = 0, sequences = 0;
void Check(bool value, const char* why) {
    ++checks;
    if (!value) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}
void Exercise(WizardState state, unsigned depth, unsigned begun, unsigned completed) {
    ++sequences;
    Check(state.CanOpenApp() == (state.Current() == Page::Success), "app launch is available only after success, failure cannot launch elevated fallback");
    if (!depth) return;
    for (unsigned action = 0; action < 8; ++action) {
        auto next = state;
        const auto before = next.Current();
        bool accepted = false;
        switch (action) {
        case 0: accepted = next.Next(); break;
        case 1: accepted = next.Back(); break;
        case 2: accepted = next.Consent(true); break;
        case 3: accepted = next.Consent(false); break;
        case 4: accepted = next.Begin(); break;
        case 5: accepted = next.Complete(0); break;
        case 6: accepted = next.Complete(1); break;
        case 7: accepted = next.Close(); break;
        }
        const unsigned starts = begun + (action == 4 && accepted ? 1u : 0u);
        const unsigned finishes = completed + ((action == 5 || action == 6) && accepted ? 1u : 0u);
        Check(starts <= 1 && finishes <= starts, "one transaction; no completion without a transaction");
        if (action == 4 && accepted) Check(before == Page::Consent && state.Approved(), "explicit current consent authorizes work");
        if (before == Page::Working && action != 5 && action != 6)
            Check(!accepted && next.Current() == Page::Working, "busy close/back/next/duplicate install cannot abandon child");
        if (before == Page::Closed) Check(!accepted && next.Current() == Page::Closed, "closed wizard cannot reopen or mutate");
        if (before == Page::Existing) Check(!accepted || action == 7, "existing installation cannot be overwritten");
        if (next.Current() == Page::Welcome) Check(!next.Approved(), "returning to welcome revokes consent");
        if (next.Current() == Page::Success) Check(next.ExitCode() == 0, "success requires exact zero child exit");
        if (!accepted) Check(next.Current() == before, "rejected actions do not change page");
        Exercise(next, depth - 1, starts, finishes);
    }
}
}
int main() {
    Exercise(WizardState(false), 7, 0, 0);
    Exercise(WizardState(true), 6, 0, 0);
    for (auto code : {0u, 1u, 2u, 3010u, 0xffffffffu}) {
        WizardState state;
        Check(state.Next() && state.Consent(true) && state.Begin() && state.Complete(code), "complete authorized workflow");
        Check(state.ExitCode() == code, "exact worker exit survives UI");
        Check(state.Close() && state.ExitCode() == code, "closing result preserves outcome");
    }
    std::cout << "PASS: " << sequences << " wizard action prefixes; " << checks << " assertions.\n";
}
