# RinMedia

RinMedia provides media-container parsing, audio/video decoding interfaces, and playback/session support for RinOS.

## Public API contract

| Requirement | Contract |
| --- | --- |
| Purpose | RinMedia provides media-container parsing, audio/video decoding interfaces, and playback/session support for RinOS. |
| Supported API | C headers include `rinavi.h`, `rinwav.h`, and `rinvideo.h`; C++ media interfaces include the audio decoder and backend-independent media framework headers. The modern pipeline accepts authenticated File Portal descriptors and bounded packets; legacy AVI/WAV functions expose container and decode operations. |
| Unsupported API | The legacy AVI/WAV APIs are not general media compatibility promises. Supported codecs and containers depend on the documented implementation and linked FFmpeg/build profile. This library does not grant access to arbitrary paths or descriptors. |
| ownership | Legacy parser contexts borrow their source bytes. `RinMedia::AudioDecoder::openDescriptor` takes ownership of its File Portal descriptor; packet/frame buffers and callbacks otherwise follow the declared caller-owned lifetime. Close or destroy objects through their owning API. |
| thread-safety | Decoder, playback, and session instances hold mutable state and must not be used concurrently unless a declaration explicitly says otherwise. Synchronize shared callbacks, descriptors, and output buffers. |
| limits | AVI parsing is capped at 16 streams. The shared framework caps packets at 1 MiB, tracks at 32, and channels at 32; audio output rate is capped at 384 kHz and channels at 32. Other bounds depend on the selected backend and build. |
| errors | Legacy APIs return status constants such as OK, data error, unsupported, and end-of-file. C++ methods return booleans or frame counts and expose last-error/cancellation state where declared. Callers must check results and stop use after failure. |
| ABI stability | C headers provide source-level C interfaces with packed structures; C++ classes follow compiler and standard-library ABI. No general cross-version binary ABI promise is published. |
| security | Treat media as untrusted. Use authenticated File Portal descriptors for the modern pipeline, enforce service authorization at the boundary, bound decode output, and do not pass arbitrary paths or shared-memory names as authority. |
| build | Build through the owning RinOS media targets and their configured FFmpeg dependencies. No standalone build/install command is documented. |
| test | No standalone test command is documented. Validate through the media, audio, and session targets in the consuming RinOS build. |
