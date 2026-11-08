#pragma once

#include "monitor_geometry.h"
#include "zoom_session.h"

namespace glide::monitor
{

    struct Presentation
    {
        session::Result result = session::Result::Invalid;
        bool localFailure = false;
        bool pending = false;
    };

    // Renderer owns private windows only; Session owns and journals global state.
    // Run every presentation tick, even when input mapping is unchanged, so the
    // native magnifier continues displaying live content. A failure hides the
    // private overlay but leaves conditional restoration to the application.
    template<class Renderer, class Backend, class Journal>
    Presentation Present(const Frame& frame, Renderer& renderer, session::Session& owner, Backend& backend, Journal& journal)
    {
        if (!ValidFrame(frame))
        {
            renderer.Hide();
            return { session::Result::Invalid, true };
        }
        // A mode switch must restore the desktop session first. Never turn an
        // owned wide-desktop visual transform into monitor zoom implicitly.
        if (owner.OwnsState() && owner.Expected().visual.zoom != 1)
        {
            renderer.Hide();
            return { session::Result::Invalid, true };
        }
        const session::Snapshot desired{ { 1, 0, 0 }, { true, frame.source, frame.destination } };
        const auto deferred = [&]() -> Presentation {
            if (renderer.Pending())
            {
                if (!owner.OwnsState())
                    return { session::Result::Unchanged, false, true };
                // During compositor startup, the last submitted image can remain
                // visible while its next readiness signal is pending. Retain only
                // that exact old image/input pair, never the newly requested map.
                // Native reads can reenter cancellation or resolve an uncertain
                // write, so prove the pair again after checking shared ownership.
                if (renderer.CanRetain(owner.Expected()))
                {
                    const auto checked = owner.Check(backend, journal);
                    if (checked != session::Result::Applied && checked != session::Result::Unchanged)
                    {
                        renderer.Hide();
                        return { checked, false };
                    }
                    if (owner.OwnsState() && renderer.CanRetain(owner.Expected()))
                        return { checked, false, true };
                }
            }
            renderer.Hide();
            return { session::Result::WriteFailure, true };
        };
        // Live content needs refreshing even at a stationary zoom, but it does not
        // need another geometry transaction. Only reuse a fully prepared private
        // frame with exactly the same owned input mapping. Check the real shared
        // state on this path; a renderer cache is never evidence of global ownership.
        const bool unchanged = owner.OwnsState() && owner.Expected() == desired &&
                               renderer.Matches(frame);
        if (!unchanged && !renderer.Prepare(frame))
        {
            // A lost/recreated image cannot use the retention exception above.
            return deferred();
        }
        auto result = unchanged ? owner.Check(backend, journal) : owner.Apply(desired, backend, journal);
        // An earlier unverified shared write can leave a recognized before/after
        // pair. Check may resolve it to a different owned mapping; repair that
        // mapping before showing this cached private frame.
        if (unchanged && result == session::Result::Unchanged && owner.Expected() != desired)
            result = owner.Apply(desired, backend, journal);
        if (result != session::Result::Applied && result != session::Result::Unchanged)
        {
            renderer.Hide();
            return { result, false };
        }
        if (!renderer.Show())
        {
            return deferred();
        }
        return { result, false };
    }

} // namespace glide::monitor
