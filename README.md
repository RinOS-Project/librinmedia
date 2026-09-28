# RinMedia

RinMedia provides media-container parsing, audio/video decoding interfaces, and playback/session support for RinOS.

## Public API contract

| Requirement | Contract |
| --- | --- |
| Purpose | RinMedia provides media-container parsing, audio/video decoding interfaces, and playback/session support for RinOS. |
| Supported API | C headers include `rinavi.h`, `rinwav.h`, `rinvideo.h`, `rinmedia_demux.h`, and `rinmedia_flac.h`; C++ media interfaces include the audio decoder and backend-independent media framework headers, whose `MediaContainer::Flac`/`Ogg`/`Adts` values match the C container ABI. The modern pipeline accepts authenticated File Portal descriptors and bounded packets; the demux contract provides failure-atomic MP4/WebM/Matroska/AVI/WAV/FLAC/Ogg/ADTS track metadata inspection plus a fixed packet extent index without exposing packet or filesystem ownership. MP4 sample tables, standard MP4 metadata containers including edit lists, EBML SimpleBlock entries with fixed, Xiph, or EBML lacing, AVI `idx1` entries, bounded PCM/IMA-ADPCM WAV data blocks, CRC-checked FLAC frame extents, CRC-checked Ogg Opus/Vorbis identification headers, a single-page config-0 Opus packet extent subset with 10 ms frames, and a one-audio-packet-per-page Vorbis packet extent subset are supported. `rinmedia_flac.h` additionally decodes one bounded FLAC frame into caller-owned interleaved signed samples, covering constant, verbatim, fixed, and LPC subframes, Rice residuals, channel decorrelation, header/frame CRCs, and failure-atomic output. Malformed lacing, malformed RIFF/index/Ogg page ranges or checksums, page-spanning or other unsupported Opus TOCs, page-spanning or multi-audio-packet Vorbis pages, partial WAV blocks (rejected by `rwav_open()` before a context is published), malformed extensible subtype GUIDs, malformed FLAC metadata/frames, unsupported Ogg codecs, AAC payload decode, and backend publication remain explicit failure boundaries. Legacy AVI/WAV functions expose container and decode operations. |
| Unsupported API | The legacy AVI/WAV APIs are not general media compatibility promises. Supported codecs and containers depend on the documented implementation and linked FFmpeg/build profile. This library does not grant access to arbitrary paths or descriptors. |
| ownership | Legacy parser contexts borrow their source bytes. `RinMedia::AudioDecoder::openDescriptor` takes ownership of its File Portal descriptor; packet/frame buffers and callbacks otherwise follow the declared caller-owned lifetime. Close or destroy objects through their owning API. |
| thread-safety | Decoder, playback, and session instances hold mutable state and must not be used concurrently unless a declaration explicitly says otherwise. Synchronize shared callbacks, descriptors, and output buffers. |
| limits | Container inspection and packet indexing are capped at 4 MiB, 32 tracks, and 256 packet extents; ISO-BMFF/EBML nesting and all variable-length fields are bounded by the caller-owned input. Individual packet extents and decoded FLAC frames are capped at 1 MiB; FLAC blocks are capped at 65,536 samples and 8 channels. AVI parsing is capped at 16 streams. The shared framework caps packets at 1 MiB, tracks at 32, and channels at 32; audio output rate is capped at 384 kHz and channels at 32. Other bounds depend on the selected backend and build. |
| errors | Legacy APIs return status constants such as OK, data error, unsupported, and end-of-file. C++ methods return booleans or frame counts and expose last-error/cancellation state where declared. Callers must check results and stop use after failure. |
| ABI stability | C headers provide source-level C interfaces with packed structures; C++ classes follow compiler and standard-library ABI. No general cross-version binary ABI promise is published. |
| security | Treat media as untrusted. Use authenticated File Portal descriptors for the modern pipeline, enforce service authorization at the boundary, bound decode output, and do not pass arbitrary paths or shared-memory names as authority. |
| build | Build through the owning RinOS media targets and their configured FFmpeg dependencies. No standalone build/install command is documented. |
| test | No standalone test command is documented. Validate through the media, audio, and session targets in the consuming RinOS build. |

`rinmedia_aac.h` provides a public, allocation-free MPEG-4 AudioSpecificConfig
inspect for AAC owners. It accepts at most 32 bytes, resolves the bounded
sampling-frequency and channel-configuration fields, and reports SBR/PS
extension metadata without decoding payloads or taking filesystem/service
ownership. Invalid, truncated, oversized, or unsupported configuration input
is rejected failure-atomically.

`rinmedia_flac.h` provides an allocation-free single-frame decoder. The
caller supplies a bounded `int64_t` scratch arena and an `int32_t` interleaved
output span; the decoder verifies the frame header and frame CRC, supports
constant/verbatim/fixed/LPC subframes with Rice residuals and stereo channel
decorrelation, and clears the output before publishing any failure. Frames,
scratch, and output remain caller-owned.

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
