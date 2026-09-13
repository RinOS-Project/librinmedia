/* SPDX-License-Identifier: MIT */
#ifndef RIN_MEDIA_SESSION_CLIENT_HPP
#define RIN_MEDIA_SESSION_CLIENT_HPP

#include "rinmedia_session_service.h"

#include "../libcxx/string.h"

namespace RinMedia {

class MediaSessionClient final {
    int socket_fd_ = -1;
    uint64_t session_id_ = 0u;
    uint32_t next_request_id_ = 1u;
    std::string error_;

    void reset();
    bool call(uint16_t operation, const void* payload, uint32_t payload_bytes,
              RinMediaSessionHeaderV1* reply);

public:
    MediaSessionClient() = default;
    ~MediaSessionClient();
    MediaSessionClient(const MediaSessionClient&) = delete;
    MediaSessionClient& operator=(const MediaSessionClient&) = delete;

    bool publish(const RinMediaSessionInfoV1& info);
    bool update(const RinMediaSessionInfoV1& info);
    bool pollCommand(RinMediaSessionCommandMessageV1* command_out);
    void close();
    bool isOpen() const { return socket_fd_ >= 0 && session_id_ != 0u; }
    uint64_t sessionId() const { return session_id_; }
    const std::string& lastError() const { return error_; }
};

/* The desktop shell holds RIN_CAP_DESKTOP and is the only type of client the
 * broker accepts for observing and dispatching global media actions.  It is
 * intentionally separate from a publisher client so a normal app cannot
 * obtain controller authority through a session handle. */
class MediaSessionDesktopClient final {
    std::string error_;

public:
    bool queryActive(RinMediaSessionPublishReplyV1* session_out,
                     RinMediaSessionInfoV1* info_out);
    bool dispatch(uint64_t session_id,
                  RinMediaSessionCommandMessageV1 command);
    const std::string& lastError() const { return error_; }
};

} // namespace RinMedia

#endif /* RIN_MEDIA_SESSION_CLIENT_HPP */
