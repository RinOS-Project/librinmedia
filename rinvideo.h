/*
 * RinOS Video Player ✿
 * 統合動画再生ライブラリ
 * AVI (Motion JPEG / 無圧縮) + PCM Audio 対応
 */

#ifndef RINVIDEO_H
#define RINVIDEO_H

#include "../libc/stdint.h"
#include "../libc/stddef.h"
#include "rinavi.h"
#include "rinwav.h"
#include "../rinjpeg/rinjpeg.h"

/* ═══════════════════════════════════════════════════════════════
 * 定数
 * ═══════════════════════════════════════════════════════════════*/

#define RVID_OK              0
#define RVID_ERROR          -1
#define RVID_DATA_ERROR     -2
#define RVID_UNSUPPORTED    -3
#define RVID_END_OF_FILE    -4

/* 再生状態 */
#define RVID_STATE_STOPPED   0
#define RVID_STATE_PLAYING   1
#define RVID_STATE_PAUSED    2

/* ビデオコーデック */
#define RVID_CODEC_UNKNOWN   0
#define RVID_CODEC_MJPEG     1
#define RVID_CODEC_RAW_RGB   2
#define RVID_CODEC_RAW_BGR   3

/* オーディオフォーマット */
#define RVID_AUDIO_NONE      0
#define RVID_AUDIO_PCM       1

/* ═══════════════════════════════════════════════════════════════
 * 構造体
 * ═══════════════════════════════════════════════════════════════*/

/* ビデオ情報 */
typedef struct {
    int width;
    int height;
    float fps;
    uint32_t total_frames;
    uint32_t duration_ms;
    int codec;
} RVidVideoInfo;

/* オーディオ情報 */
typedef struct {
    int channels;
    int sample_rate;
    int bits_per_sample;
    int format;
} RVidAudioInfo;

/* フレームバッファ */
typedef struct {
    uint32_t* pixels;           /* ARGB8888 */
    int width;
    int height;
    int stride;                 /* バイト単位 */
} RVidFrameBuffer;

/* オーディオバッファ */
typedef struct {
    int16_t* samples;           /* 16bit signed PCM */
    int sample_count;
    int channels;
} RVidAudioBuffer;

/* コールバック関数型 */
typedef void (*RVidFrameCallback)(const RVidFrameBuffer* frame, void* user_data);
typedef void (*RVidAudioCallback)(const RVidAudioBuffer* audio, void* user_data);

/* プレイヤーコンテキスト */
typedef struct {
    /* ファイルデータ */
    const uint8_t* data;
    size_t size;

    /* AVIコンテキスト */
    RAviContext avi;

    /* 動画情報 */
    RVidVideoInfo video_info;
    RVidAudioInfo audio_info;

    /* 再生状態 */
    int state;
    uint32_t current_frame;
    uint32_t current_time_ms;

    /* デコードバッファ */
    uint32_t* frame_buffer;
    int frame_buffer_size;

    /* コールバック */
    RVidFrameCallback frame_callback;
    RVidAudioCallback audio_callback;
    void* callback_user_data;

    /* オーディオ同期 */
    uint32_t audio_chunk_index;
    uint32_t samples_played;

    /* ループ再生 */
    int loop;
} RVidPlayer;

/* ═══════════════════════════════════════════════════════════════
 * 内部関数
 * ═══════════════════════════════════════════════════════════════*/

static inline int rvid_detect_codec(uint32_t fourcc) {
    switch (fourcc) {
        case RAVI_MJPG:
        case RAVI_MJPEG:
        case RAVI_JPEG:
            return RVID_CODEC_MJPEG;
        case RAVI_DIB:
        case RAVI_RAW:  /* RAVI_RAW = 0 */
            return RVID_CODEC_RAW_BGR;
        default:
            return RVID_CODEC_UNKNOWN;
    }
}

static inline int rvid_decode_mjpeg_frame(const uint8_t* data, size_t size,
                                           uint32_t* pixels,
                                           size_t pixel_capacity,
                                           int max_width, int max_height) {
    return rjpeg_decode(data, size, pixels, pixel_capacity,
                        max_width, max_height);
}

static inline int rvid_decode_raw_frame(const uint8_t* data, size_t size,
                                         uint32_t* pixels,
                                         size_t pixel_capacity,
                                         int width, int height,
                                         int bits_per_pixel, int is_bgr) {
    if (!data || !pixels || width <= 0 || height <= 0 ||
        (is_bgr != 0 && is_bgr != 1) ||
        (bits_per_pixel != 16 && bits_per_pixel != 24 &&
         bits_per_pixel != 32))
        return RVID_ERROR;

    size_t bytes_per_pixel = (size_t)(unsigned)bits_per_pixel / 8u;
    size_t columns = (size_t)(unsigned)width;
    size_t rows = (size_t)(unsigned)height;
    if (columns > (SIZE_MAX - 3u) / bytes_per_pixel ||
        rows > SIZE_MAX / columns)
        return RVID_ERROR;
    size_t row_bytes = columns * bytes_per_pixel;
    size_t stride = (row_bytes + 3u) & ~(size_t)3u; /* 4バイトアライン */
    size_t required_pixels = columns * rows;
    if (stride != 0u && rows > SIZE_MAX / stride) return RVID_ERROR;
    if (stride * rows > size || required_pixels > pixel_capacity)
        return RVID_ERROR;

    /* ボトムアップBMP形式 */
    for (int y = 0; y < height; y++) {
        const uint8_t* src =
            data + (size_t)(unsigned)(height - 1 - y) * stride;
        uint32_t* dst = pixels + (size_t)(unsigned)y * (size_t)(unsigned)width;

        for (int x = 0; x < width; x++) {
            if (bytes_per_pixel == 3) {
                const uint8_t* sample = src + (size_t)(unsigned)x * 3u;
                uint8_t r = sample[is_bgr ? 2 : 0];
                uint8_t g = sample[1];
                uint8_t b = sample[is_bgr ? 0 : 2];
                dst[x] = 0xff000000u | ((uint32_t)r << 16) |
                         ((uint32_t)g << 8) | (uint32_t)b;
            } else if (bytes_per_pixel == 4) {
                const uint8_t* sample = src + (size_t)(unsigned)x * 4u;
                uint8_t r = sample[is_bgr ? 2 : 0];
                uint8_t g = sample[1];
                uint8_t b = sample[is_bgr ? 0 : 2];
                uint8_t a = sample[3];
                dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                         ((uint32_t)g << 8) | (uint32_t)b;
            } else if (bytes_per_pixel == 2) {
                /* RGB565 */
                const uint8_t* sample = src + (size_t)(unsigned)x * 2u;
                uint16_t c = (uint16_t)sample[0] |
                             ((uint16_t)sample[1] << 8);
                uint8_t r = ((c >> 11) & 0x1F) << 3;
                uint8_t g = ((c >> 5) & 0x3F) << 2;
                uint8_t b = (c & 0x1F) << 3;
                if (is_bgr) {
                    uint8_t swap = r;
                    r = b;
                    b = swap;
                }
                dst[x] = 0xff000000u | ((uint32_t)r << 16) |
                         ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }

    return RVID_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * 公開API
 * ═══════════════════════════════════════════════════════════════*/

/*
 * プレイヤー初期化
 */
static inline int rvid_open(RVidPlayer* player, const uint8_t* data, size_t size) {
    if (!player || !data || size < 12) return RVID_ERROR;

    /* 初期化 */
    for (size_t i = 0; i < sizeof(RVidPlayer); i++) {
        ((uint8_t*)player)[i] = 0;
    }
    player->data = data;
    player->size = size;
    player->state = RVID_STATE_STOPPED;

    /* AVI解析 */
    int ret = ravi_open(&player->avi, data, size);
    if (ret != RAVI_OK) {
        return RVID_DATA_ERROR;
    }

    /* ビデオ情報取得 */
    ravi_get_info(&player->avi, &player->video_info.width, &player->video_info.height,
                  &player->video_info.fps, &player->video_info.total_frames);
    player->video_info.duration_ms = player->avi.duration_ms;

    /* コーデック検出 */
    uint32_t codec_fourcc = ravi_get_video_codec(&player->avi);
    player->video_info.codec = rvid_detect_codec(codec_fourcc);

    if (player->video_info.codec == RVID_CODEC_UNKNOWN) {
        return RVID_UNSUPPORTED;
    }

    /* オーディオ情報取得 */
    if (player->avi.audio_stream >= 0) {
        ravi_get_audio_info(&player->avi,
                            &player->audio_info.channels,
                            &player->audio_info.sample_rate,
                            &player->audio_info.bits_per_sample);

        RAviStream* as = &player->avi.streams[player->avi.audio_stream];
        if (as->audio.format == RWAV_FORMAT_PCM) {
            player->audio_info.format = RVID_AUDIO_PCM;
        }
    }

    return RVID_OK;
}

/*
 * プレイヤー閉じる
 */
static inline void rvid_close(RVidPlayer* player) {
    if (!player) return;
    player->state = RVID_STATE_STOPPED;
    player->frame_buffer = NULL;
}

/*
 * フレームバッファ設定
 */
static inline int rvid_set_frame_buffer(RVidPlayer* player, uint32_t* buffer, int size) {
    if (!player || !buffer) return RVID_ERROR;
    player->frame_buffer = buffer;
    player->frame_buffer_size = size;
    return RVID_OK;
}

/*
 * コールバック設定
 */
static inline void rvid_set_callbacks(RVidPlayer* player,
                                       RVidFrameCallback frame_cb,
                                       RVidAudioCallback audio_cb,
                                       void* user_data) {
    if (!player) return;
    player->frame_callback = frame_cb;
    player->audio_callback = audio_cb;
    player->callback_user_data = user_data;
}

/*
 * ビデオ情報取得
 */
static inline int rvid_get_video_info(RVidPlayer* player, RVidVideoInfo* info) {
    if (!player || !info) return RVID_ERROR;
    *info = player->video_info;
    return RVID_OK;
}

/*
 * オーディオ情報取得
 */
static inline int rvid_get_audio_info(RVidPlayer* player, RVidAudioInfo* info) {
    if (!player || !info) return RVID_ERROR;
    *info = player->audio_info;
    return RVID_OK;
}

/*
 * 1フレームデコード
 */
static inline int rvid_decode_frame(RVidPlayer* player, uint32_t frame_num,
                                     uint32_t* pixels, size_t pixel_capacity,
                                     int max_width, int max_height) {
    if (!player || !pixels || max_width <= 0 || max_height <= 0)
        return RVID_ERROR;

    RAviFrame frame;
    int ret = ravi_get_frame_by_index(&player->avi, frame_num, &frame);
    if (ret != RAVI_OK) {
        return (ret == RAVI_END_OF_FILE) ? RVID_END_OF_FILE : RVID_ERROR;
    }

    /* コーデックに応じてデコード */
    if (player->video_info.codec == RVID_CODEC_MJPEG) {
        ret = rvid_decode_mjpeg_frame(frame.data, frame.size, pixels,
                                       pixel_capacity,
                                       max_width, max_height);
    } else if (player->video_info.codec == RVID_CODEC_RAW_BGR ||
               player->video_info.codec == RVID_CODEC_RAW_RGB) {
        RAviStream* vs = &player->avi.streams[player->avi.video_stream];
        if (vs->video.width <= 0 || vs->video.height <= 0 ||
            vs->video.width > max_width || vs->video.height > max_height)
            return RVID_ERROR;
        ret = rvid_decode_raw_frame(frame.data, frame.size, pixels,
                                     pixel_capacity,
                                     vs->video.width, vs->video.height,
                                     vs->video.bit_depth,
                                     player->video_info.codec == RVID_CODEC_RAW_BGR);
    } else {
        return RVID_UNSUPPORTED;
    }

    return (ret == 0) ? RVID_OK : RVID_ERROR;
}

/*
 * 次のフレームをデコードして進める
 */
static inline int rvid_next_frame(RVidPlayer* player, uint32_t* pixels,
                                   size_t pixel_capacity,
                                   int max_width, int max_height) {
    if (!player) return RVID_ERROR;

    int ret = rvid_decode_frame(player, player->current_frame, pixels,
                                 pixel_capacity,
                                 max_width, max_height);
    if (ret == RVID_OK) {
        player->current_frame++;
        if (player->video_info.fps > 0) {
            player->current_time_ms = (uint32_t)(player->current_frame * 1000.0f /
                                                  player->video_info.fps);
        }

        /* ループ処理 */
        if (player->current_frame >= player->video_info.total_frames) {
            if (player->loop) {
                player->current_frame = 0;
                player->current_time_ms = 0;
            } else {
                player->state = RVID_STATE_STOPPED;
                return RVID_END_OF_FILE;
            }
        }
    }
    return ret;
}

/*
 * オーディオチャンク取得
 */
static inline int rvid_get_audio_chunk(RVidPlayer* player, uint32_t chunk_num,
                                        const uint8_t** data, size_t* size) {
    if (!player || !data || !size) return RVID_ERROR;
    if (player->audio_info.format == RVID_AUDIO_NONE) return RVID_ERROR;

    RAviFrame frame;
    int ret = ravi_get_audio_chunk(&player->avi, chunk_num, &frame);
    if (ret != RAVI_OK) {
        return (ret == RAVI_END_OF_FILE) ? RVID_END_OF_FILE : RVID_ERROR;
    }

    *data = frame.data;
    *size = frame.size;
    return RVID_OK;
}

/*
 * 時間に対応するオーディオを取得 (PCM 16bit signed)
 */
static inline int rvid_get_audio_for_time(RVidPlayer* player, uint32_t time_ms,
                                           int16_t* output, int max_samples,
                                           int* samples_out) {
    if (!player || !output || !samples_out) return RVID_ERROR;
    if (max_samples <= 0) return RVID_ERROR;
    if (player->audio_info.format != RVID_AUDIO_PCM) return RVID_UNSUPPORTED;

    /* 時間からサンプル位置を計算 */
    uint32_t target_sample = (uint32_t)((uint64_t)time_ms *
                             player->audio_info.sample_rate / 1000);

    /* 該当するオーディオチャンクを探す */
    uint32_t chunk = 0;
    uint32_t accumulated_samples = 0;
    int bytes_per_sample = player->audio_info.bits_per_sample / 8 *
                           player->audio_info.channels;

    while (1) {
        const uint8_t* chunk_data;
        size_t chunk_size;

        int ret = rvid_get_audio_chunk(player, chunk, &chunk_data, &chunk_size);
        if (ret != RVID_OK) break;

        uint32_t chunk_samples = chunk_size / bytes_per_sample;

        if (accumulated_samples + chunk_samples > target_sample) {
            /* このチャンクに該当サンプルがある */
            uint32_t offset_samples = target_sample - accumulated_samples;
            uint32_t available = chunk_samples - offset_samples;
            uint32_t requested = (uint32_t)max_samples;
            uint32_t to_copy = (available < requested) ? available : requested;

            const uint8_t* src = chunk_data + offset_samples * bytes_per_sample;

            /* PCMデータを16bitに変換してコピー */
            if (player->audio_info.bits_per_sample == 16) {
                for (uint32_t i = 0; i < to_copy * player->audio_info.channels; i++) {
                    output[i] = ((int16_t*)src)[i];
                }
            } else if (player->audio_info.bits_per_sample == 8) {
                for (uint32_t i = 0; i < to_copy * player->audio_info.channels; i++) {
                    output[i] = ((int16_t)src[i] - 128) << 8;
                }
            }

            *samples_out = to_copy;
            return RVID_OK;
        }

        accumulated_samples += chunk_samples;
        chunk++;
    }

    *samples_out = 0;
    return RVID_END_OF_FILE;
}

/*
 * 再生開始
 */
static inline int rvid_play(RVidPlayer* player) {
    if (!player) return RVID_ERROR;
    player->state = RVID_STATE_PLAYING;
    return RVID_OK;
}

/*
 * 一時停止
 */
static inline int rvid_pause(RVidPlayer* player) {
    if (!player) return RVID_ERROR;
    player->state = RVID_STATE_PAUSED;
    return RVID_OK;
}

/*
 * 停止
 */
static inline int rvid_stop(RVidPlayer* player) {
    if (!player) return RVID_ERROR;
    player->state = RVID_STATE_STOPPED;
    player->current_frame = 0;
    player->current_time_ms = 0;
    return RVID_OK;
}

/*
 * シーク (フレーム番号)
 */
static inline int rvid_seek_frame(RVidPlayer* player, uint32_t frame_num) {
    if (!player) return RVID_ERROR;
    if (frame_num >= player->video_info.total_frames) {
        frame_num = player->video_info.total_frames > 0 ?
                    player->video_info.total_frames - 1 : 0;
    }
    player->current_frame = frame_num;
    if (player->video_info.fps > 0) {
        player->current_time_ms = (uint32_t)(frame_num * 1000.0f / player->video_info.fps);
    }
    return RVID_OK;
}

/*
 * シーク (時間 ミリ秒)
 */
static inline int rvid_seek_ms(RVidPlayer* player, uint32_t time_ms) {
    if (!player || player->video_info.fps <= 0) return RVID_ERROR;
    uint32_t frame = (uint32_t)(time_ms * player->video_info.fps / 1000.0f);
    return rvid_seek_frame(player, frame);
}

/*
 * ループ設定
 */
static inline void rvid_set_loop(RVidPlayer* player, int loop) {
    if (player) player->loop = loop;
}

/*
 * 現在のフレーム番号取得
 */
static inline uint32_t rvid_get_current_frame(RVidPlayer* player) {
    return player ? player->current_frame : 0;
}

/*
 * 現在の再生時間取得 (ミリ秒)
 */
static inline uint32_t rvid_get_current_time_ms(RVidPlayer* player) {
    return player ? player->current_time_ms : 0;
}

/*
 * 再生状態取得
 */
static inline int rvid_get_state(RVidPlayer* player) {
    return player ? player->state : RVID_STATE_STOPPED;
}

/*
 * 再生終了判定
 */
static inline int rvid_is_finished(RVidPlayer* player) {
    if (!player) return 1;
    return !player->loop && player->current_frame >= player->video_info.total_frames;
}

/*
 * タイムベース更新 (メインループから呼び出す)
 * elapsed_ms: 前回呼び出しからの経過時間
 * 戻り値: デコードすべきフレーム数
 */
static inline int rvid_update(RVidPlayer* player, uint32_t elapsed_ms) {
    if (!player || player->state != RVID_STATE_PLAYING) return 0;

    player->current_time_ms += elapsed_ms;

    /* 目標フレームを計算 */
    uint32_t target_frame = (uint32_t)(player->current_time_ms *
                            player->video_info.fps / 1000.0f);

    if (target_frame >= player->video_info.total_frames) {
        if (player->loop) {
            target_frame = target_frame % player->video_info.total_frames;
            player->current_time_ms = (uint32_t)(target_frame * 1000.0f /
                                       player->video_info.fps);
        } else {
            target_frame = player->video_info.total_frames - 1;
        }
    }

    int frames_to_decode = (int)target_frame - (int)player->current_frame;
    if (frames_to_decode < 0) frames_to_decode = 0;

    return frames_to_decode;
}

#endif /* RINVIDEO_H */
