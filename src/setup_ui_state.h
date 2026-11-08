#pragma once
#include <cstdint>
namespace glide::setup {
enum class Page { Welcome, Consent, Working, Success, Failure, Existing, Closed };
class WizardState {
public:
    explicit WizardState(bool existing = false) noexcept : page_(existing ? Page::Existing : Page::Welcome) {}
    Page Current() const noexcept { return page_; }
    bool Approved() const noexcept { return approved_; }
    bool CanOpenApp() const noexcept { return page_ == Page::Success; }
    bool Busy() const noexcept { return page_ == Page::Working; }
    std::uint32_t ExitCode() const noexcept { return exitCode_; }
    bool Next() noexcept {
        if (page_ != Page::Welcome) return false;
        page_ = Page::Consent; approved_ = false; return true;
    }
    bool Back() noexcept {
        if (page_ != Page::Consent) return false;
        page_ = Page::Welcome; approved_ = false; return true;
    }
    bool Consent(bool value) noexcept {
        if (page_ != Page::Consent) return false;
        approved_ = value; return true;
    }
    bool Begin() noexcept {
        if (page_ != Page::Consent || !approved_) return false;
        page_ = Page::Working; exitCode_ = 1; return true;
    }
    bool Complete(std::uint32_t code) noexcept {
        if (page_ != Page::Working) return false;
        exitCode_ = code; page_ = code == 0 ? Page::Success : Page::Failure; return true;
    }
    bool Close() noexcept {
        if (page_ == Page::Working || page_ == Page::Closed) return false;
        page_ = Page::Closed; return true;
    }
private:
    Page page_ = Page::Welcome;
    bool approved_ = false;
    std::uint32_t exitCode_ = 2;
};
} // namespace glide::setup
