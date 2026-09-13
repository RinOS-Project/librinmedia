/* SPDX-License-Identifier: MIT */
/* Versioned, path-free media-session broker wire contract. */
#ifndef RIN_MEDIA_SESSION_SERVICE_H
#define RIN_MEDIA_SESSION_SERVICE_H

#include <stdint.h>

#define RIN_MEDIA_SESSION_MAGIC UINT32_C(0x3153534d) /* "MSS1" */
#define RIN_MEDIA_SESSION_VERSION 1u
#define RIN_MEDIA_SESSION_SOCKET_PATH "/run/rin/mediasessiond.sock"
#define RIN_MEDIA_SESSION_TEXT_CAPACITY 128u

enum RinMediaSessionOperationV1 {
    RIN_MEDIA_SESSION_OP_PUBLISH = 1u,
    RIN_MEDIA_SESSION_OP_UPDATE = 2u,
    RIN_MEDIA_SESSION_OP_POLL_COMMAND = 3u,
    RIN_MEDIA_SESSION_OP_UNPUBLISH = 4u,
    RIN_MEDIA_SESSION_OP_QUERY_ACTIVE = 5u,
    RIN_MEDIA_SESSION_OP_DISPATCH_COMMAND = 6u,
};

enum RinMediaSessionPlaybackStateV1 {
    RIN_MEDIA_SESSION_PLAYBACK_PAUSED = 1u,
    RIN_MEDIA_SESSION_PLAYBACK_PLAYING = 2u,
};

enum RinMediaSessionCommandV1 {
    RIN_MEDIA_SESSION_COMMAND_NONE = 0u,
    RIN_MEDIA_SESSION_COMMAND_PLAY_PAUSE = 1u,
    RIN_MEDIA_SESSION_COMMAND_NEXT = 2u,
    RIN_MEDIA_SESSION_COMMAND_PREVIOUS = 3u,
    RIN_MEDIA_SESSION_COMMAND_STOP = 4u,
};

#define RIN_MEDIA_SESSION_CONTROL_PLAY_PAUSE UINT32_C(0x00000001)
#define RIN_MEDIA_SESSION_CONTROL_NEXT       UINT32_C(0x00000002)
#define RIN_MEDIA_SESSION_CONTROL_PREVIOUS   UINT32_C(0x00000004)
#define RIN_MEDIA_SESSION_CONTROL_STOP       UINT32_C(0x00000008)
#define RIN_MEDIA_SESSION_CONTROL_ALL \
    (RIN_MEDIA_SESSION_CONTROL_PLAY_PAUSE | RIN_MEDIA_SESSION_CONTROL_NEXT | \
     RIN_MEDIA_SESSION_CONTROL_PREVIOUS | RIN_MEDIA_SESSION_CONTROL_STOP)

typedef struct RinMediaSessionHeaderV1 {
    uint32_t magic;
    uint32_t version;
    uint16_t operation;
    uint16_t reserved0;
    uint32_t request_id;
    uint32_t payload_bytes;
    int32_t status;
    uint64_t session_id;
} RinMediaSessionHeaderV1;

/* Metadata is a bounded display snapshot.  It deliberately has no path,
 * descriptor, artwork payload, or caller-owned pointer.  `revision` is
 * service-owned: publishers send zero and the service fills it in replies. */
typedef struct RinMediaSessionInfoV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t playback_state;
    uint32_t control_flags;
    int64_t position_ms;
    int64_t duration_ms;
    uint64_t revision;
    uint32_t title_bytes;
    uint32_t artist_bytes;
    uint32_t album_bytes;
    uint32_t reserved0;
    char title[RIN_MEDIA_SESSION_TEXT_CAPACITY];
    char artist[RIN_MEDIA_SESSION_TEXT_CAPACITY];
    char album[RIN_MEDIA_SESSION_TEXT_CAPACITY];
} RinMediaSessionInfoV1;

typedef struct RinMediaSessionCommandMessageV1 {
    uint32_t command;
    uint32_t reserved0;
    uint64_t revision;
} RinMediaSessionCommandMessageV1;

typedef struct RinMediaSessionPublishReplyV1 {
    uint64_t session_id;
    uint64_t revision;
} RinMediaSessionPublishReplyV1;

#if defined(__cplusplus)
static_assert(sizeof(RinMediaSessionHeaderV1) == 32u,
              "media session header ABI drift");
static_assert(sizeof(RinMediaSessionInfoV1) == 440u,
              "media session information ABI drift");
static_assert(sizeof(RinMediaSessionCommandMessageV1) == 16u,
              "media session command ABI drift");
static_assert(sizeof(RinMediaSessionPublishReplyV1) == 16u,
              "media session publish reply ABI drift");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(RinMediaSessionHeaderV1) == 32u,
               "media session header ABI drift");
_Static_assert(sizeof(RinMediaSessionInfoV1) == 440u,
               "media session information ABI drift");
_Static_assert(sizeof(RinMediaSessionCommandMessageV1) == 16u,
               "media session command ABI drift");
_Static_assert(sizeof(RinMediaSessionPublishReplyV1) == 16u,
               "media session publish reply ABI drift");
#endif

#endif /* RIN_MEDIA_SESSION_SERVICE_H */
