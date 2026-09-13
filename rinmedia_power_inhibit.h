/* SPDX-License-Identifier: MIT */
/* Generation-bound power-inhibition request admission. */
#ifndef RIN_MEDIA_POWER_INHIBIT_H
#define RIN_MEDIA_POWER_INHIBIT_H

#include "rinmedia_playback_policy.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RIN_MEDIA_POWER_INHIBIT_VERSION 1u

typedef struct RinMediaPowerInhibitRequestV1 {
    uint32_t struct_size;
    uint32_t version;
    uint64_t session_id;
    uint64_t session_generation;
    uint32_t inhibit;
    uint32_t reserved0;
} RinMediaPowerInhibitRequestV1;

#if defined(__cplusplus)
static_assert(sizeof(RinMediaPowerInhibitRequestV1) == 32u,
              "media power inhibit request ABI drift");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(RinMediaPowerInhibitRequestV1) == 32u,
               "media power inhibit request ABI drift");
#endif

/* Return 1 when the privileged owner may acquire inhibition, 0 for a valid
 * playback snapshot that does not require it, and -1 for malformed input. */
static inline int rin_media_power_inhibit_build_v1(
    const RinMediaPlaybackPolicyInputV1* input, uint64_t session_id,
    RinMediaPowerInhibitRequestV1* output, size_t output_size)
{
    RinMediaPlaybackPolicyDecisionV1 decision;
    if (output == NULL || output_size < sizeof(*output))
        return -1;
#if defined(__cplusplus)
    *output = RinMediaPowerInhibitRequestV1{};
#else
    *output = (RinMediaPowerInhibitRequestV1){0};
#endif
    if (input == NULL || session_id == 0u ||
        rin_media_playback_policy_evaluate_v1(input, &decision,
                                              sizeof(decision)) != 0 ||
        decision.reason == RIN_MEDIA_PLAYBACK_POLICY_INVALID_INPUT)
        return -1;
    if (decision.request_power_inhibit == 0u)
        return 0;
    output->struct_size = sizeof(*output);
    output->version = RIN_MEDIA_POWER_INHIBIT_VERSION;
    output->session_id = session_id;
    output->session_generation = input->session_generation;
    output->inhibit = 1u;
    return 1;
}

static inline int rin_media_power_inhibit_request_valid_v1(
    const RinMediaPowerInhibitRequestV1* request)
{
    return request != NULL && request->struct_size == sizeof(*request) &&
           request->version == RIN_MEDIA_POWER_INHIBIT_VERSION &&
           request->session_id != 0u && request->session_generation != 0u &&
           request->inhibit == 1u && request->reserved0 == 0u;
}

/* Privileged power owners receive only this fixed request.  The adapter
 * keeps lease lifecycle and generation checks in one place; it never stores a
 * descriptor, path, or UI state.  release is idempotent and is attempted
 * after every apply failure because an owner may have acquired the lease
 * before returning an error. */
typedef int (*RinMediaPowerInhibitApplyV1)(
    void* context, const RinMediaPowerInhibitRequestV1* request);
typedef int (*RinMediaPowerInhibitReleaseV1)(void* context);

typedef struct RinMediaPowerInhibitOwnerOpsV1 {
    uint32_t struct_size;
    uint32_t version;
    void* context;
    RinMediaPowerInhibitApplyV1 apply;
    RinMediaPowerInhibitReleaseV1 release;
} RinMediaPowerInhibitOwnerOpsV1;

typedef struct RinMediaPowerInhibitOwnerV1 {
    uint32_t struct_size;
    uint32_t version;
    RinMediaPowerInhibitOwnerOpsV1 ops;
    RinMediaPowerInhibitRequestV1 active_request;
    uint32_t has_lease;
    uint32_t release_pending;
    uint32_t callback_active;
    uint32_t reserved0;
} RinMediaPowerInhibitOwnerV1;

#define RIN_MEDIA_POWER_INHIBIT_OWNER_VERSION 1u

static inline int rin_media_power_inhibit_owner_ops_valid_v1(
    const RinMediaPowerInhibitOwnerOpsV1* operations)
{
    return operations != NULL &&
           operations->struct_size == sizeof(*operations) &&
           operations->version == RIN_MEDIA_POWER_INHIBIT_OWNER_VERSION &&
           operations->context != NULL && operations->apply != NULL &&
           operations->release != NULL;
}

static inline int rin_media_power_inhibit_owner_valid_v1(
    const RinMediaPowerInhibitOwnerV1* owner)
{
    return owner != NULL && owner->struct_size == sizeof(*owner) &&
           owner->version == RIN_MEDIA_POWER_INHIBIT_OWNER_VERSION &&
           rin_media_power_inhibit_owner_ops_valid_v1(&owner->ops) &&
           owner->has_lease <= 1u && owner->release_pending <= 1u &&
           owner->callback_active <= 1u && owner->reserved0 == 0u &&
           (!owner->has_lease ||
            rin_media_power_inhibit_request_valid_v1(&owner->active_request));
}

static inline int rin_media_power_inhibit_owner_init_v1(
    RinMediaPowerInhibitOwnerV1* owner,
    const RinMediaPowerInhibitOwnerOpsV1* operations)
{
    if (owner == NULL) return -1;
    memset(owner, 0, sizeof(*owner));
    if (!rin_media_power_inhibit_owner_ops_valid_v1(operations)) return -1;
    owner->struct_size = sizeof(*owner);
    owner->version = RIN_MEDIA_POWER_INHIBIT_OWNER_VERSION;
    owner->ops = *operations;
    return 0;
}

static inline int rin_media_power_inhibit_owner_release_v1(
    RinMediaPowerInhibitOwnerV1* owner)
{
    int result;
    if (!rin_media_power_inhibit_owner_valid_v1(owner) ||
        owner->callback_active != 0u)
        return -1;
    if (owner->has_lease == 0u && owner->release_pending == 0u) return 0;
    owner->callback_active = 1u;
    result = owner->ops.release(owner->ops.context);
    owner->callback_active = 0u;
    if (result != 0) {
        owner->release_pending = 1u;
        return -1;
    }
    memset(&owner->active_request, 0, sizeof(owner->active_request));
    owner->has_lease = 0u;
    owner->release_pending = 0u;
    return 0;
}

static inline int rin_media_power_inhibit_owner_apply_v1(
    RinMediaPowerInhibitOwnerV1* owner,
    const RinMediaPowerInhibitRequestV1* request)
{
    int result;
    if (!rin_media_power_inhibit_owner_valid_v1(owner) ||
        !rin_media_power_inhibit_request_valid_v1(request) ||
        owner->callback_active != 0u)
        return -1;
    owner->callback_active = 1u;
    result = owner->ops.apply(owner->ops.context, request);
    owner->callback_active = 0u;
    if (result != 0) {
        owner->active_request = *request;
        owner->has_lease = 1u;
        owner->release_pending = 0u;
        (void)rin_media_power_inhibit_owner_release_v1(owner);
        return -1;
    }
    owner->active_request = *request;
    owner->has_lease = 1u;
    owner->release_pending = 0u;
    return 1;
}

/* Evaluate the playback policy and reconcile the privileged lease.  Return
 * 1 when inhibition is active, 0 for a valid playing snapshot that does not
 * require inhibition, and -1 for malformed input, stale generation, callback
 * failure, or pending cleanup. */
static inline int rin_media_power_inhibit_owner_sync_v1(
    RinMediaPowerInhibitOwnerV1* owner,
    const RinMediaPlaybackPolicyInputV1* input, uint64_t session_id)
{
    RinMediaPowerInhibitRequestV1 candidate;
    int build;
    if (!rin_media_power_inhibit_owner_valid_v1(owner) ||
        owner->callback_active != 0u)
        return -1;
    build = rin_media_power_inhibit_build_v1(input, session_id, &candidate,
                                             sizeof(candidate));
    if (build < 0) {
        (void)rin_media_power_inhibit_owner_release_v1(owner);
        return -1;
    }
    if (build == 0) {
        if (rin_media_power_inhibit_owner_release_v1(owner) != 0) return -1;
        return 0;
    }
    if (owner->release_pending != 0u &&
        rin_media_power_inhibit_owner_release_v1(owner) != 0)
        return -1;
    if (owner->has_lease != 0u) {
        if (owner->active_request.session_id == candidate.session_id &&
            candidate.session_generation <
                owner->active_request.session_generation)
            return -1;
        if (owner->active_request.session_id == candidate.session_id &&
            owner->active_request.session_generation ==
                candidate.session_generation)
            return 1;
        if (rin_media_power_inhibit_owner_release_v1(owner) != 0) return -1;
    }
    return rin_media_power_inhibit_owner_apply_v1(owner, &candidate) < 0 ? -1
                                                                         : 1;
}

#endif /* RIN_MEDIA_POWER_INHIBIT_H */
