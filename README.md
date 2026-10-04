# RinMedia

RinMedia provides media-container parsing, audio/video decoding interfaces, and playback/session support for RinOS.

The public `AudioDecoderClient` and `MediaSessionClient` keep their local
service authority and descriptor ownership private, while their POSIX socket
waits use the public `RinRuntime::PollEventLoopBackend` userspace adapter.
They do not expose the private RinOS wait-set backend or kernel readiness
producer, and an unavailable media service is still reported as failure.

`rinmedia_pcm.h` also exposes an allocation-free public PCM packet decoder.
It converts bounded interleaved U8/S8/S16LE/S24LE/S32LE/F32LE packets into
caller-owned S16 samples, rejects partial frames and non-finite float values,
and clears the output on every failure. This packet helper is independent of
filesystem, service, device, and hardware ownership.

The public PCM packet decoder is connected to the root common sanitizer target
(`python scripts/run_common_sanitizers.py --target rinmedia --smoke`), which
uses only caller-owned input/output and the public C implementation. This
coverage does not publish or exercise the private media-service or hardware
decoder backends.

The same public sanitizer target exercises `rinvideo.h` through caller-owned
pixel/audio buffers, including malformed AVI rejection, bounded RAW frame
decode, PCM audio chunk/time queries, playback state transitions, and
seek/update arithmetic. Its deterministic AVI seeds cover one-frame RAW video
alone and one-frame RAW video plus PCM audio; JPEG payloads, full codec
coverage, private service ownership, and device/hardware backends remain
outside this target.

The public demux packet table remains a caller-owned source view. When a
consumer needs owned packet bytes, `rin_media_container_copy_packet()` copies
one validated extent into a caller-owned buffer with a 1 MiB bound and a
failure-atomic output/size contract. It does not open paths or descriptors and
does not grant filesystem, File Portal, codec, or service authority.

The public demux inspect and packet-index entry points clear only the caller's
reported output span before validation. An undersized `output_size` therefore
cannot make a failure path write the full private structure past the caller's
buffer; valid full-size outputs remain failure-atomic.

`rin_media_container_read_packet()` provides the same contract for a caller's
opaque source callback. Each callback request is capped at 64 KiB, short reads
are retried, and callback failure, stall, or an oversized report clears the
packet output. After a successful short read it also clears the unreported
tail of that request, so a buggy source cannot leave bytes visible in a
caller-reused buffer. The callback is not retained and is not a filesystem or
caller-reused buffer. The cancellation-enabled form samples an optional
caller-owned cancellation callback before and after each source read and
clears the whole packet on cancellation or owner failure. The callback is not
retained and is not a filesystem or service owner.

The public `rvid_update()` playback clock saturates its millisecond counter at
`UINT32_MAX` and rejects empty or non-positive-fps metadata before computing a
target frame. The public seek and frame-advance paths use the same bounded
conversion, so malformed or extreme frame rates cannot turn a floating-point
infinity into an out-of-range integer or wrap the frame counter. This prevents
long-running pure playback state from wrapping backward; decoder, filesystem,
device, and hardware ownership remain outside the model.

`rvid_close()` clears the complete borrowed player context, including the AVI
metadata, callbacks, playback state, and frame-buffer pointer. A caller can
therefore reuse the context after close without stale media bytes or owner
state remaining visible.

The legacy public PCM time query saturates its time-to-sample conversion,
rejects audio chunk-size and sample-count overflow, and reads 16-bit PCM as
little-endian bytes instead of relying on caller-buffer alignment. Its output
remains caller-owned and the query does not acquire filesystem, device, or
codec authority; malformed or unavailable AVI audio chunks remain errors
instead of being reported as end-of-file.

After valid PCM metadata admission, `rvid_get_audio_for_time()` clears the
caller-provided frame span before scanning AVI chunks. A malformed or missing
chunk therefore cannot leave stale PCM visible on failure; successful reads
publish only the reported frame count and leave the remainder zeroed.

`rvid_update()` also saturates its `int` decode-count result at `INT32_MAX`
instead of narrowing large `uint32_t` frame deltas before subtraction.

The public legacy WAV duration query saturates at `UINT32_MAX` and validates
its metadata before calculating duration. `rwav_read_s16()` rejects a sample
request that cannot be represented by its `int` return value before touching
the caller's output.

The allocation-free container capability probe uses subtraction-based
`size_t` bounds for ISO-BMFF extended-size headers, so a caller-provided size
near the platform maximum cannot wrap the probe's header check. It still only
recognizes the bounded envelope and does not grant packet, filesystem, or
codec authority.

## Public API contract

| Requirement | Contract |
| --- | --- |
| Purpose | RinMedia provides media-container parsing, audio/video decoding interfaces, and playback/session support for RinOS. |
| Supported API | C headers include `rinavi.h`, `rinwav.h`, `rinvideo.h`, `rinmedia_demux.h`, `rinmedia_pcm.h`, and `rinmedia_flac.h`; C++ media interfaces include the audio decoder and backend-independent media framework headers, whose `MediaContainer::Flac`/`Ogg`/`Adts` values match the C container ABI. The modern pipeline accepts authenticated File Portal descriptors and bounded packets; the demux contract provides failure-atomic MP4/WebM/Matroska/AVI/WAV/FLAC/Ogg/ADTS track metadata inspection plus a fixed packet extent index without exposing packet or filesystem ownership. MP4 sample tables, standard MP4 metadata containers including edit lists, EBML SimpleBlock entries with fixed, Xiph, or EBML lacing, one bounded Matroska `Block` per `BlockGroup` with `ReferenceBlock` keyframe semantics and single-frame `BlockDuration` duration, AVI `idx1` entries, bounded PCM/IMA-ADPCM WAV data blocks, CRC-checked FLAC frame extents, CRC-checked Ogg Opus/Vorbis identification headers, a single-page bounded Opus packet extent subset with C=0/1/2/3 frames of known 2.5--60 ms duration (C=2/C=3 VBR lengths and C=3 padding use the RFC 6716 one/two-byte form), and a one-audio-packet-per-page Vorbis packet extent subset are supported. `rinmedia_pcm.h` converts bounded interleaved PCM packets to caller-owned S16 samples; `rinmedia_flac.h` additionally decodes one bounded FLAC frame into caller-owned interleaved signed samples, covering constant, verbatim, fixed, and LPC subframes, Rice residuals, channel decorrelation, header/frame CRCs, and failure-atomic output. Malformed lacing, any `BlockDuration`-bearing laced Block including an explicit zero duration, malformed RIFF/index/Ogg page ranges or checksums, malformed C=3 padding/lengths, other unsupported Opus TOCs, page-spanning or multi-audio-packet Vorbis pages, partial WAV blocks (rejected by `rwav_open()` before a context is published), malformed extensible subtype GUIDs, malformed FLAC metadata/frames, unsupported Ogg codecs, AAC payload decode, and backend publication remain explicit failure boundaries. Legacy AVI/WAV functions expose container and decode operations. |
| Unsupported API | The legacy AVI/WAV APIs are not general media compatibility promises. Supported codecs and containers depend on the documented implementation and linked FFmpeg/build profile. This library does not grant access to arbitrary paths or descriptors. |
| ownership | Legacy parser contexts borrow their source bytes. `RinMedia::AudioDecoder::openDescriptor` takes ownership of its File Portal descriptor; packet/frame buffers and callbacks otherwise follow the declared caller-owned lifetime. Close or destroy objects through their owning API. |
| thread-safety | Decoder, playback, and session instances hold mutable state and must not be used concurrently unless a declaration explicitly says otherwise. Synchronize shared callbacks, descriptors, and output buffers. |
| limits | Container inspection and packet indexing are capped at 4 MiB, 32 tracks, and 256 packet extents; ISO-BMFF/EBML nesting and all variable-length fields are bounded by the caller-owned input. Individual packet extents, `rin_media_container_copy_packet()` output, and decoded FLAC frames are capped at 1 MiB; FLAC blocks are capped at 65,536 samples and 8 channels. AVI parsing is capped at 16 streams. The public PCM packet decoder caps input at 1 MiB, frames at 65,536, sample rate at 384 kHz, and channels at 32. The shared framework caps packets at 1 MiB, tracks at 32, and channels at 32; audio output rate is capped at 384 kHz and channels at 32. Other bounds depend on the selected backend and build. |
| errors | Legacy APIs return status constants such as OK, data error, unsupported, and end-of-file. C++ methods return booleans or frame counts and expose last-error/cancellation state where declared. Callers must check results and stop use after failure. |
| ABI stability | C headers provide source-level C interfaces with packed structures; C++ classes follow compiler and standard-library ABI. No general cross-version binary ABI promise is published. |
| security | Treat media as untrusted. Use authenticated File Portal descriptors for the modern pipeline, enforce service authorization at the boundary, bound decode output, and do not pass arbitrary paths or shared-memory names as authority. |
| build | Build through the owning RinOS media targets and their configured FFmpeg dependencies. No standalone build/install command is documented. |
| test | No standalone test command is documented. Validate through the media, audio, and session targets in the consuming RinOS build. |

`rinmedia_aac.h` provides a public, allocation-free MPEG-4 AudioSpecificConfig
inspect for AAC owners. It accepts at most 32 bytes, resolves the bounded
sampling-frequency and channel-configuration fields, and reports SBR/PS
extension metadata, including bounded LC sync extensions, without decoding
payloads or taking filesystem/service ownership. Conflicting or malformed
extension metadata is rejected failure-atomically. Invalid, truncated,
oversized, or unsupported configuration input is rejected failure-atomically.

`rinmedia_flac.h` provides an allocation-free single-frame decoder. The
caller supplies a bounded `int64_t` scratch arena and an `int32_t` interleaved
output span; the decoder verifies the frame header and frame CRC, supports
constant/verbatim/fixed/LPC subframes with Rice residuals and stereo channel
decorrelation (including arithmetic right-shift semantics for negative odd
side samples), checks every fixed/LPC prediction, decorrelation, and wasted-bit
intermediate for signed `int64_t` overflow, and clears the output before
publishing any failure. Frames, scratch, and output remain caller-owned.

The common public `rinmedia` sanitizer corpus includes a deterministic
one-frame constant FLAC stream, so metadata inspection, packet indexing, and
the successful constant-subframe decode path are exercised without a service,
filesystem, or hardware owner.

The legacy `rinwav.h` S16 reader sign-extends 24-bit PCM before its bounded
8-bit downshift and rejects non-finite IEEE float samples before conversion;
the latter clears the requested output span and leaves the read position
unchanged on failure. IMA-ADPCM blocks are decoded into interleaved stereo
frames and retain bounded in-block state across partial reads instead of
discarding the unread suffix.

The EBML demux accepts unknown-size only for the bounded Segment/Cluster
masters used by the packet path. TrackEntry, Info, Timecode, and SimpleBlock
elements must carry a finite size; an unknown-size leaf is rejected before
metadata or packet output is published.
The top-level WebM/Matroska inspect consumes exactly one Segment and rejects
trailing bytes or a second Segment instead of silently ignoring them.
TrackNumber values above `UINT32_MAX` are also rejected instead of being
truncated into a colliding public track ID.
Duplicate MP4 `track_ID` and WebM/Matroska `TrackNumber` values are rejected
before a colliding track table is published.

WebM/Matroska track metadata inherits the validated Segment `TimecodeScale`
(default 1,000,000) into each public track's `time_scale`; no guessed track
duration is published when the bounded input does not provide one.
The scale is a positive 1- to 4-byte EBML integer and repeated Info scale
elements are rejected rather than silently last-wins.
When an Info `Duration` element is present, its IEEE-754 value must be a
non-negative finite 32-bit or 64-bit value; NaN and infinity are rejected
before metadata is published.
