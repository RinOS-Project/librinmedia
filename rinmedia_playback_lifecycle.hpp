/* SPDX-License-Identifier: MIT */
/* Generation-bound media playback lifecycle shared by applications and shell. */
#ifndef RIN_MEDIA_PLAYBACK_LIFECYCLE_HPP
#define RIN_MEDIA_PLAYBACK_LIFECYCLE_HPP

#include "rinmedia_lock_controls.h"

#include <cstdint>

namespace RinMedia {

class MediaPlaybackLifecycle final {
public:
    enum class SurfaceState : std::uint32_t {
        Foreground = RIN_MEDIA_PLAYBACK_SESSION_FOREGROUND,
        Background = RIN_MEDIA_PLAYBACK_SESSION_BACKGROUND,
        Locked = RIN_MEDIA_PLAYBACK_SESSION_LOCKED,
    };

    bool bind(std::uint64_t sessionId, std::uint64_t generation,
              bool backgroundAllowed, bool outputAvailable,
              std::uint32_t controlFlags)
    {
        if (sessionId == 0u || generation == 0u ||
            (controlFlags & ~RIN_MEDIA_SESSION_CONTROL_ALL) != 0u)
            return false;
        sessionId_ = sessionId;
        generation_ = generation;
        backgroundAllowed_ = backgroundAllowed;
        outputAvailable_ = outputAvailable;
        controlFlags_ = controlFlags;
        state_ = SurfaceState::Foreground;
        playing_ = false;
        bound_ = true;
        return true;
    }

    void clear()
    {
        sessionId_ = 0u;
        generation_ = 0u;
        backgroundAllowed_ = false;
        outputAvailable_ = false;
        controlFlags_ = 0u;
        state_ = SurfaceState::Foreground;
        playing_ = false;
        bound_ = false;
    }

    bool setSurfaceState(SurfaceState state, std::uint64_t generation)
    {
        if (!bound_ || generation != generation_ ||
            ((state == SurfaceState::Background || state == SurfaceState::Locked) &&
             !backgroundAllowed_))
            return false;
        state_ = state;
        return true;
    }

    bool setPlayback(bool playing, bool outputAvailable,
                     std::uint64_t generation)
    {
        if (!bound_ || generation != generation_)
            return false;
        playing_ = playing;
        outputAvailable_ = outputAvailable;
        return true;
    }

    bool evaluate(RinMediaPlaybackPolicyDecisionV1& decision) const
    {
        if (!bound_)
            return false;
        RinMediaPlaybackPolicyInputV1 input = {};
        input.struct_size = sizeof(input);
        input.version = RIN_MEDIA_PLAYBACK_POLICY_VERSION;
        input.session_state = static_cast<std::uint32_t>(state_);
        input.playback_state = playing_
            ? RIN_MEDIA_SESSION_PLAYBACK_PLAYING
            : RIN_MEDIA_SESSION_PLAYBACK_PAUSED;
        input.background_allowed = backgroundAllowed_ ? 1u : 0u;
        input.output_available = outputAvailable_ ? 1u : 0u;
        input.control_flags = controlFlags_;
        input.session_generation = generation_;
        return rin_media_playback_policy_evaluate_v1(
                   &input, &decision, sizeof(decision)) == 0;
    }

    int buildLockControl(std::uint32_t command,
                         RinMediaLockControlRequestV1& request) const
    {
        request = RinMediaLockControlRequestV1{};
        RinMediaPlaybackPolicyInputV1 input = {};
        RinMediaPlaybackPolicyDecisionV1 decision = {};
        if (!bound_ || state_ != SurfaceState::Locked)
            return 0;
        input.struct_size = sizeof(input);
        input.version = RIN_MEDIA_PLAYBACK_POLICY_VERSION;
        input.session_state = static_cast<std::uint32_t>(state_);
        input.playback_state = playing_
            ? RIN_MEDIA_SESSION_PLAYBACK_PLAYING
            : RIN_MEDIA_SESSION_PLAYBACK_PAUSED;
        input.background_allowed = backgroundAllowed_ ? 1u : 0u;
        input.output_available = outputAvailable_ ? 1u : 0u;
        input.control_flags = controlFlags_;
        input.session_generation = generation_;
        if (rin_media_playback_policy_evaluate_v1(
                &input, &decision, sizeof(decision)) != 0 ||
            decision.reason == RIN_MEDIA_PLAYBACK_POLICY_INVALID_INPUT)
            return -1;
        return rin_media_lock_control_build_v1(
            &input, sessionId_, command, &request, sizeof(request));
    }

    bool bound() const { return bound_; }
    std::uint64_t sessionId() const { return sessionId_; }
    std::uint64_t generation() const { return generation_; }
    SurfaceState surfaceState() const { return state_; }

private:
    std::uint64_t sessionId_ = 0u;
    std::uint64_t generation_ = 0u;
    bool backgroundAllowed_ = false;
    bool outputAvailable_ = false;
    std::uint32_t controlFlags_ = 0u;
    SurfaceState state_ = SurfaceState::Foreground;
    bool playing_ = false;
    bool bound_ = false;
};

} // namespace RinMedia

#endif /* RIN_MEDIA_PLAYBACK_LIFECYCLE_HPP */
