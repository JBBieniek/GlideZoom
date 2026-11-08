#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace glide::ui
{

    // UI-thread cache for text setters that repaint a window or notify Explorer.
    // A successful native write is the only thing that commits a cache entry.
    // Invalidate on window/icon replacement, even when Windows reuses its handle.
    class TextCache
    {
    public:
        void Invalidate() noexcept
        {
            valid_ = false;
            owner_ = 0;
            value_.clear();
            cancelled_ = true;
        }

        template<class Setter>
        bool Update(std::uintptr_t owner, std::wstring_view text, Setter&& setter)
        {
            if (!owner)
                return false;
            if (busy_)
            {
                Invalidate();
                return false;
            }
            if (valid_ && owner_ == owner && value_ == text)
                return true;
            // Allocate before the native call, so allocation failure cannot leave
            // an unrecorded successful write. Sent-message invalidation still wins.
            std::wstring next{ text };
            struct Busy
            {
                TextCache& cache;
                explicit Busy(TextCache& value) :
                    cache(value)
                {
                    cache.busy_ = true;
                    cache.cancelled_ = false;
                }
                ~Busy() { cache.busy_ = false; }
            } operation{ *this };
            valid_ = false;
            if (!static_cast<bool>(std::forward<Setter>(setter)()) || cancelled_)
                return false;
            owner_ = owner;
            value_.swap(next);
            valid_ = true;
            return true;
        }

    private:
        std::uintptr_t owner_ = 0;
        std::wstring value_;
        bool valid_ = false, busy_ = false, cancelled_ = false;
    };

} // namespace glide::ui
