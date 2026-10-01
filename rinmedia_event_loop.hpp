/* SPDX-License-Identifier: MIT */
/* Public userspace wait helper for the media service clients. */
#ifndef RIN_MEDIA_EVENT_LOOP_HPP
#define RIN_MEDIA_EVENT_LOOP_HPP

#include <cstdint>

#include <rinruntime/event_loop_poll.hpp>

extern "C" std::uint64_t rin_monotonic_ms(void);

namespace RinMedia {
namespace detail {

enum class MediaWaitStatus : std::uint8_t {
    Ready = 0,
    Timeout = 1,
    Failure = 2,
};

inline std::uint64_t mediaMonotonicNanoseconds(void*) noexcept {
    constexpr std::uint64_t kNanosecondsPerMillisecond = 1000000u;
    const std::uint64_t milliseconds = rin_monotonic_ms();
    return milliseconds > UINT64_MAX / kNanosecondsPerMillisecond
               ? UINT64_MAX
               : milliseconds * kNanosecondsPerMillisecond;
}

inline MediaWaitStatus waitForMediaFd(int fd, std::uint32_t events) noexcept {
    using EventBackend = RinRuntime::PollEventLoopBackend;
    using EventLoop = RinRuntime::EventLoop;
    constexpr std::uint64_t kWaitNanoseconds = UINT64_C(1000000000);

    if (fd <= 0 || events == 0u) return MediaWaitStatus::Failure;
    const std::uint64_t now = mediaMonotonicNanoseconds(nullptr);
    if (now == UINT64_MAX || kWaitNanoseconds >= UINT64_MAX - now)
        return MediaWaitStatus::Failure;

    const EventLoop::WaitRequest request = {
        1u,
        static_cast<std::uint64_t>(fd),
        events | EventLoop::WAIT_ERROR | EventLoop::WAIT_HANGUP,
    };
    EventLoop::WaitResult ready = {};
    EventBackend backend(mediaMonotonicNanoseconds);
    const std::uint64_t deadline = now + kWaitNanoseconds;
    if (backend.wait(&request, 1u, deadline, &ready)) {
        if (ready.id != request.id ||
            (ready.events & (EventLoop::WAIT_ERROR |
                             EventLoop::WAIT_HANGUP)) != 0u ||
            (ready.events & events) == 0u)
            return MediaWaitStatus::Failure;
        return MediaWaitStatus::Ready;
    }

    const std::uint64_t after = mediaMonotonicNanoseconds(nullptr);
    if (after == UINT64_MAX || after < now) return MediaWaitStatus::Failure;
    return after >= deadline ? MediaWaitStatus::Timeout
                             : MediaWaitStatus::Failure;
}

} // namespace detail
} // namespace RinMedia

#endif /* RIN_MEDIA_EVENT_LOOP_HPP */
