/* SPDX-License-Identifier: MIT */
/* Backend-independent media playback admission and power policy. */
#ifndef RIN_MEDIA_PLAYBACK_POLICY_H
#define RIN_MEDIA_PLAYBACK_POLICY_H

#include <stddef.h>
#include <stdint.h>

#include "rinmedia_session_service.h"

#define RIN_MEDIA_PLAYBACK_POLICY_VERSION 1u

enum RinMediaPlaybackSessionStateV1 {
    RIN_MEDIA_PLAYBACK_SESSION_FOREGROUND = 1u,
    RIN_MEDIA_PLAYBACK_SESSION_BACKGROUND = 2u,
    RIN_MEDIA_PLAYBACK_SESSION_LOCKED = 3u,
};

enum RinMediaPlaybackPolicyReasonV1 {
    RIN_MEDIA_PLAYBACK_POLICY_OK = 0u,
    RIN_MEDIA_PLAYBACK_POLICY_INVALID_INPUT = 1u,
    RIN_MEDIA_PLAYBACK_POLICY_PAUSED = 2u,
    RIN_MEDIA_PLAYBACK_POLICY_BACKGROUND_DISABLED = 3u,
    RIN_MEDIA_PLAYBACK_POLICY_OUTPUT_UNAVAILABLE = 4u,
    RIN_MEDIA_PLAYBACK_POLICY_SESSION_UNAVAILABLE = 5u,
};

/* This snapshot is supplied by the session/compositor/audio owners.  It is a
 * policy input only: it contains no path, descriptor, or caller-owned
 * pointer.  A non-zero session_generation binds a decision to one session. */
typedef struct RinMediaPlaybackPolicyInputV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t session_state;
    uint32_t playback_state;
    uint32_t background_allowed;
    uint32_t output_available;
    uint32_t control_flags;
    uint32_t reserved0;
    uint64_t session_generation;
} RinMediaPlaybackPolicyInputV1;

/* The decision is deliberately explicit so callers cannot infer power
 * inhibition from a UI state.  All three actions are false on malformed
 * input or when playback is not admitted. */
typedef struct RinMediaPlaybackPolicyDecisionV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t allow_playback;
    uint32_t show_lock_controls;
    uint32_t request_power_inhibit;
    uint32_t reason;
    uint32_t reserved0;
    uint32_t reserved1;
} RinMediaPlaybackPolicyDecisionV1;

#if defined(__cplusplus)
static_assert(sizeof(RinMediaPlaybackPolicyInputV1) == 40u,
              "media playback policy input ABI drift");
static_assert(sizeof(RinMediaPlaybackPolicyDecisionV1) == 32u,
              "media playback policy decision ABI drift");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(RinMediaPlaybackPolicyInputV1) == 40u,
               "media playback policy input ABI drift");
_Static_assert(sizeof(RinMediaPlaybackPolicyDecisionV1) == 32u,
               "media playback policy decision ABI drift");
#endif

static inline int rin_media_playback_policy_evaluate_v1(
    const RinMediaPlaybackPolicyInputV1* input,
    RinMediaPlaybackPolicyDecisionV1* decision, size_t decision_size)
{
    if (decision == NULL || decision_size < sizeof(*decision)) {
        return -1;
    }

    decision->struct_size = sizeof(*decision);
    decision->version = RIN_MEDIA_PLAYBACK_POLICY_VERSION;
    decision->allow_playback = 0u;
    decision->show_lock_controls = 0u;
    decision->request_power_inhibit = 0u;
    decision->reason = RIN_MEDIA_PLAYBACK_POLICY_INVALID_INPUT;
    decision->reserved0 = 0u;
    decision->reserved1 = 0u;

    if (input == NULL || input->struct_size != sizeof(*input) ||
        input->version != RIN_MEDIA_PLAYBACK_POLICY_VERSION ||
        input->reserved0 != 0u || input->session_generation == 0u ||
        (input->session_state != RIN_MEDIA_PLAYBACK_SESSION_FOREGROUND &&
         input->session_state != RIN_MEDIA_PLAYBACK_SESSION_BACKGROUND &&
         input->session_state != RIN_MEDIA_PLAYBACK_SESSION_LOCKED) ||
        (input->playback_state != RIN_MEDIA_SESSION_PLAYBACK_PAUSED &&
         input->playback_state != RIN_MEDIA_SESSION_PLAYBACK_PLAYING) ||
        (input->background_allowed > 1u) ||
        (input->output_available > 1u) ||
        ((input->control_flags & ~RIN_MEDIA_SESSION_CONTROL_ALL) != 0u)) {
        return 0;
    }

    if (input->playback_state == RIN_MEDIA_SESSION_PLAYBACK_PAUSED) {
        decision->reason = RIN_MEDIA_PLAYBACK_POLICY_PAUSED;
        return 0;
    }

    if (input->output_available == 0u) {
        decision->reason = RIN_MEDIA_PLAYBACK_POLICY_OUTPUT_UNAVAILABLE;
        return 0;
    }

    if (input->session_state != RIN_MEDIA_PLAYBACK_SESSION_FOREGROUND &&
        input->background_allowed == 0u) {
        decision->reason = RIN_MEDIA_PLAYBACK_POLICY_BACKGROUND_DISABLED;
        return 0;
    }

    decision->allow_playback = 1u;
    decision->request_power_inhibit = 1u;
    if (input->session_state == RIN_MEDIA_PLAYBACK_SESSION_LOCKED &&
        input->control_flags != 0u) {
        decision->show_lock_controls = 1u;
    }
    decision->reason = RIN_MEDIA_PLAYBACK_POLICY_OK;
    return 0;
}

#endif /* RIN_MEDIA_PLAYBACK_POLICY_H */
