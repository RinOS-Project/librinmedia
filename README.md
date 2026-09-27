# RinMedia

RinMedia provides media-container parsing, audio/video decoding interfaces, and playback/session support for RinOS.

## Public API contract

| Requirement | Contract |
| --- | --- |
| Purpose | RinMedia provides media-container parsing, audio/video decoding interfaces, and playback/session support for RinOS. |
| Supported API | C headers include `rinavi.h`, `rinwav.h`, `rinvideo.h`, and `rinmedia_demux.h`; C++ media interfaces include the audio decoder and backend-independent media framework headers. The modern pipeline accepts authenticated File Portal descriptors and bounded packets; the demux contract provides failure-atomic MP4/WebM/Matroska/AVI track metadata inspection plus a fixed packet extent index without exposing packet or filesystem ownership. MP4 sample tables, EBML SimpleBlock entries with fixed, Xiph, or EBML lacing, and AVI `idx1` entries are supported; malformed lacing, malformed RIFF/index ranges, codec decode, and backend publication remain explicit failure boundaries. Legacy AVI/WAV functions expose container and decode operations. |
| Unsupported API | The legacy AVI/WAV APIs are not general media compatibility promises. Supported codecs and containers depend on the documented implementation and linked FFmpeg/build profile. This library does not grant access to arbitrary paths or descriptors. |
| ownership | Legacy parser contexts borrow their source bytes. `RinMedia::AudioDecoder::openDescriptor` takes ownership of its File Portal descriptor; packet/frame buffers and callbacks otherwise follow the declared caller-owned lifetime. Close or destroy objects through their owning API. |
| thread-safety | Decoder, playback, and session instances hold mutable state and must not be used concurrently unless a declaration explicitly says otherwise. Synchronize shared callbacks, descriptors, and output buffers. |
| limits | Container inspection and packet indexing are capped at 4 MiB, 32 tracks, and 256 packet extents; ISO-BMFF/EBML nesting and all variable-length fields are bounded by the caller-owned input. Individual packet extents are capped at 1 MiB. AVI parsing is capped at 16 streams. The shared framework caps packets at 1 MiB, tracks at 32, and channels at 32; audio output rate is capped at 384 kHz and channels at 32. Other bounds depend on the selected backend and build. |
| errors | Legacy APIs return status constants such as OK, data error, unsupported, and end-of-file. C++ methods return booleans or frame counts and expose last-error/cancellation state where declared. Callers must check results and stop use after failure. |
| ABI stability | C headers provide source-level C interfaces with packed structures; C++ classes follow compiler and standard-library ABI. No general cross-version binary ABI promise is published. |
| security | Treat media as untrusted. Use authenticated File Portal descriptors for the modern pipeline, enforce service authorization at the boundary, bound decode output, and do not pass arbitrary paths or shared-memory names as authority. |
| build | Build through the owning RinOS media targets and their configured FFmpeg dependencies. No standalone build/install command is documented. |
| test | No standalone test command is documented. Validate through the media, audio, and session targets in the consuming RinOS build. |
