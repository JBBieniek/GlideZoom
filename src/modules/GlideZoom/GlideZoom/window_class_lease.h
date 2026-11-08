// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#pragma once

#include <cstdint>
#include <limits>

namespace glide::window
{
    // Shared on the renderer's owner UI thread. An instance acquires once before
    // creating windows and releases only after destroying all of its windows.
    // The final release alone may unregister a class registered by this process.
    class ClassLease
    {
    public:
        template<class Backend>
        bool Acquire(std::uintptr_t instance, Backend& backend) noexcept
        {
            constexpr auto maximumReferences = std::numeric_limits<std::uint32_t>::max();
            if (!instance || references_ == maximumReferences ||
                (instance_ && instance_ != instance))
                return false;
            if (!references_)
            {
                bool newlyOwned = false;
                if (!backend.Register(newlyOwned))
                    return false;
                instance_ = instance;
                owned_ = owned_ || newlyOwned;
            }
            ++references_;
            return true;
        }

        template<class Backend>
        void Release(Backend& backend) noexcept
        {
            if (!references_ || --references_)
                return;
            if (owned_ && !backend.Unregister())
                return; // Retain ownership and retry after a later final release.
            instance_ = 0;
            owned_ = false;
        }

    private:
        std::uintptr_t instance_ = 0;
        std::uint32_t references_ = 0;
        bool owned_ = false;
    };
}
