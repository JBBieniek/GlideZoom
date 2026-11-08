#pragma once

#include "runtime_state.h"

#include <cstdint>

namespace glide::timing
{

    enum class StallAction
    {
        Reset,
        PreserveInitialTargetAndDiscardInput
    };

    struct StallContext
    {
        bool ready = false;
        bool monitorMode = false;
        bool capturePending = false;
        bool ownsInputState = false;
        bool overlayVisible = false;
        bool cursorRecovery = false;
        bool resetRequested = false;
    };

    // Called after a stale UI interval. Only an invisible first-frame capture with
    // normal input/cursor state may retain its already requested target. The caller
    // must still cancel input, advance its epoch and reject stale movement/steps.
    // A real reset request always wins; no applied or uncertain global state earns
    // the startup exception.
    inline StallAction DecideStall(const StallContext& context) noexcept
    {
        return context.ready && context.monitorMode && context.capturePending &&
                       !context.ownsInputState && !context.overlayVisible &&
                       !context.cursorRecovery && !context.resetRequested ?
                   StallAction::PreserveInitialTargetAndDiscardInput :
                   StallAction::Reset;
    }

    // These packets come only from the application's owned input queue. A delayed
    // emergency reset can only cancel zoom; it must survive the epoch change that
    // drops stale zoom-in/motion commands. Session restoration still verifies its
    // own global-state ownership before writing anything.
    inline bool AcceptQueuedInput(bool resetRequest, std::uint64_t eventEpoch, const runtime::State& lifecycle) noexcept
    {
        return resetRequest || lifecycle.Accept(eventEpoch);
    }

} // namespace glide::timing
