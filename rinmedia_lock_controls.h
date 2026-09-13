/* SPDX-License-Identifier: MIT */
/* Generation-bound lock-screen media command admission. */
#ifndef RIN_MEDIA_LOCK_CONTROLS_H
#define RIN_MEDIA_LOCK_CONTROLS_H

#include "rinmedia_playback_policy.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RIN_MEDIA_LOCK_CONTROLS_VERSION 1u

typedef struct RinMediaLockControlRequestV1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t session_id;
    uint64_t session_generation;
    uint32_t command;
    uint32_t reserved0;
} RinMediaLockControlRequestV1;

#if defined(__cplusplus)
static_assert(sizeof(RinMediaLockControlRequestV1) == 32u,
              "media lock control request ABI drift");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(RinMediaLockControlRequestV1) == 32u,
               "media lock control request ABI drift");
#endif

static inline uint32_t rin_media_lock_control_flag(uint32_t command)
{
    switch (command) {
    case RIN_MEDIA_SESSION_COMMAND_PLAY_PAUSE:
        return RIN_MEDIA_SESSION_CONTROL_PLAY_PAUSE;
    case RIN_MEDIA_SESSION_COMMAND_NEXT:
        return RIN_MEDIA_SESSION_CONTROL_NEXT;
    case RIN_MEDIA_SESSION_COMMAND_PREVIOUS:
        return RIN_MEDIA_SESSION_CONTROL_PREVIOUS;
    case RIN_MEDIA_SESSION_COMMAND_STOP:
        return RIN_MEDIA_SESSION_CONTROL_STOP;
    default:
        return 0u;
    }
}

/* Return 1 for an admitted request, 0 for a valid-but-denied policy input,
 * and -1 for malformed output arguments or policy input. */
static inline int rin_media_lock_control_build_v1(
    const RinMediaPlaybackPolicyInputV1* input, uint64_t session_id,
    uint32_t command, RinMediaLockControlRequestV1* output,
    size_t output_size)
{
    RinMediaPlaybackPolicyDecisionV1 decision;
    const uint32_t flag = rin_media_lock_control_flag(command);
    if (output == NULL || output_size < sizeof(*output))
        return -1;
#if defined(__cplusplus)
    *output = RinMediaLockControlRequestV1{};
#else
    *output = (RinMediaLockControlRequestV1){0};
#endif
    if (input == NULL || session_id == 0u || flag == 0u ||
        rin_media_playback_policy_evaluate_v1(input, &decision,
                                              sizeof(decision)) != 0 ||
        decision.reason == RIN_MEDIA_PLAYBACK_POLICY_INVALID_INPUT)
        return -1;
    if (decision.show_lock_controls == 0u ||
        (input->control_flags & flag) == 0u)
        return 0;
    output->struct_size = sizeof(*output);
    output->version = RIN_MEDIA_LOCK_CONTROLS_VERSION;
    output->session_id = session_id;
    output->session_generation = input->session_generation;
    output->command = command;
    return 1;
}

static inline int rin_media_lock_control_request_valid_v1(
    const RinMediaLockControlRequestV1* request)
{
    const uint32_t flag = request == NULL
        ? 0u : rin_media_lock_control_flag(request->command);
    return request != NULL && request->struct_size == sizeof(*request) &&
           request->version == RIN_MEDIA_LOCK_CONTROLS_VERSION &&
           request->session_id != 0u && request->session_generation != 0u &&
           flag != 0u && request->reserved0 == 0u;
}

/* The compositor-facing owner is deliberately callback based.  It owns no
 * surface, descriptor, or metadata: the authenticated compositor supplies a
 * context and consumes the fixed request only after this state machine has
 * admitted it.  A clear callback is idempotent and is also used to clean up a
 * publish callback that reports failure after side effects. */
typedef int (*RinMediaLockControlPublishV1)(
    void* context, const RinMediaLockControlRequestV1* request);
typedef int (*RinMediaLockControlClearV1)(void* context);

typedef struct RinMediaLockControlOwnerOpsV1 {
    uint32_t struct_size;
    uint32_t version;
    void* context;
    RinMediaLockControlPublishV1 publish;
    RinMediaLockControlClearV1 clear;
} RinMediaLockControlOwnerOpsV1;

typedef struct RinMediaLockControlOwnerV1 {
    uint32_t struct_size;
    uint32_t version;
    RinMediaLockControlOwnerOpsV1 ops;
    RinMediaLockControlRequestV1 visible;
    uint32_t has_visible;
    uint32_t clear_pending;
    uint32_t callback_active;
    uint32_t reserved0;
} RinMediaLockControlOwnerV1;

#define RIN_MEDIA_LOCK_CONTROL_OWNER_VERSION 1u

static inline int rin_media_lock_control_owner_ops_valid_v1(
    const RinMediaLockControlOwnerOpsV1* operations)
{
    return operations != NULL &&
           operations->struct_size == sizeof(*operations) &&
           operations->version == RIN_MEDIA_LOCK_CONTROL_OWNER_VERSION &&
           operations->context != NULL && operations->publish != NULL &&
           operations->clear != NULL;
}

static inline int rin_media_lock_control_owner_valid_v1(
    const RinMediaLockControlOwnerV1* owner)
{
    return owner != NULL && owner->struct_size == sizeof(*owner) &&
           owner->version == RIN_MEDIA_LOCK_CONTROL_OWNER_VERSION &&
           rin_media_lock_control_owner_ops_valid_v1(&owner->ops) &&
           owner->has_visible <= 1u && owner->clear_pending <= 1u &&
           owner->callback_active <= 1u && owner->reserved0 == 0u &&
           (!owner->has_visible ||
            rin_media_lock_control_request_valid_v1(&owner->visible));
}

static inline int rin_media_lock_control_owner_init_v1(
    RinMediaLockControlOwnerV1* owner,
    const RinMediaLockControlOwnerOpsV1* operations)
{
    if (owner == NULL) return -1;
    memset(owner, 0, sizeof(*owner));
    if (!rin_media_lock_control_owner_ops_valid_v1(operations)) return -1;
    owner->struct_size = sizeof(*owner);
    owner->version = RIN_MEDIA_LOCK_CONTROL_OWNER_VERSION;
    owner->ops = *operations;
    return 0;
}

/* Clear is failure-atomic from the owner's perspective.  When the
 * compositor rejects the clear, the previous request remains recorded and
 * no replacement can be published until a later retry succeeds. */
static inline int rin_media_lock_control_owner_clear_v1(
    RinMediaLockControlOwnerV1* owner)
{
    int result;
    if (!rin_media_lock_control_owner_valid_v1(owner) ||
        owner->callback_active != 0u)
        return -1;
    if (owner->has_visible == 0u && owner->clear_pending == 0u) return 0;
    owner->callback_active = 1u;
    result = owner->ops.clear(owner->ops.context);
    owner->callback_active = 0u;
    if (result != 0) {
        owner->clear_pending = 1u;
        return -1;
    }
    memset(&owner->visible, 0, sizeof(owner->visible));
    owner->has_visible = 0u;
    owner->clear_pending = 0u;
    return 0;
}

/* Publish one admitted request.  Any reported failure is treated as
 * potentially having side effects, so the idempotent clear callback is
 * attempted before the failure is exposed to the caller. */
static inline int rin_media_lock_control_owner_publish_v1(
    RinMediaLockControlOwnerV1* owner,
    const RinMediaLockControlRequestV1* request)
{
    int result;
    if (!rin_media_lock_control_owner_valid_v1(owner) ||
        !rin_media_lock_control_request_valid_v1(request) ||
        owner->callback_active != 0u)
        return -1;
    owner->callback_active = 1u;
    result = owner->ops.publish(owner->ops.context, request);
    owner->callback_active = 0u;
    if (result != 0) {
        owner->has_visible = 1u;
        owner->visible = *request;
        owner->clear_pending = 0u;
        if (rin_media_lock_control_owner_clear_v1(owner) != 0) return -1;
        return -1;
    }
    owner->visible = *request;
    owner->has_visible = 1u;
    owner->clear_pending = 0u;
    return 1;
}

/* Evaluate policy and atomically reconcile the compositor surface.  Return
 * 1 when controls are visible, 0 for a valid policy denial, and -1 for input,
 * callback, stale-generation, or cleanup failure. */
static inline int rin_media_lock_control_owner_sync_v1(
    RinMediaLockControlOwnerV1* owner,
    const RinMediaPlaybackPolicyInputV1* input, uint64_t session_id,
    uint32_t command)
{
    RinMediaLockControlRequestV1 candidate;
    int build;
    if (!rin_media_lock_control_owner_valid_v1(owner) ||
        owner->callback_active != 0u)
        return -1;
    build = rin_media_lock_control_build_v1(input, session_id, command,
                                             &candidate, sizeof(candidate));
    if (build < 0) {
        (void)rin_media_lock_control_owner_clear_v1(owner);
        return -1;
    }
    if (build == 0) {
        if (rin_media_lock_control_owner_clear_v1(owner) != 0) return -1;
        return 0;
    }
    if (owner->clear_pending != 0u &&
        rin_media_lock_control_owner_clear_v1(owner) != 0)
        return -1;
    if (owner->has_visible != 0u) {
        if (owner->visible.session_id == candidate.session_id &&
            candidate.session_generation < owner->visible.session_generation)
            return -1;
        if (owner->visible.session_id == candidate.session_id &&
            owner->visible.session_generation == candidate.session_generation &&
            owner->visible.command == candidate.command)
            return 1;
        if (owner->visible.session_id != candidate.session_id ||
            owner->visible.session_generation != candidate.session_generation) {
            if (rin_media_lock_control_owner_clear_v1(owner) != 0) return -1;
        }
    }
    return rin_media_lock_control_owner_publish_v1(owner, &candidate) < 0 ? -1
                                                                           : 1;
}

#endif /* RIN_MEDIA_LOCK_CONTROLS_H */
