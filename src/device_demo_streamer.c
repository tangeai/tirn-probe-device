#define _POSIX_C_SOURCE 200809L

#include "device_demo_streamer.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const uint64_t kMicrosecondsPerMillisecond = 1000ULL;
static const uint64_t kMicrosecondsPerSecond = 1000000ULL;
static const size_t kVideoReadChunkBytes = 64 * 1024;
static const size_t kInitialFrameCapacity = 128 * 1024;

typedef struct {
    FILE *file;
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    int eof;
} annexb_reader_t;

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    uint64_t pts_us;
    int is_key_frame;
} video_frame_t;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t size;
    uint64_t pts_us;
} audio_packet_t;

struct device_demo_streamer {
    tirtc_conn_t hconn;
    pthread_t worker;
    pthread_mutex_t mutex;
    int worker_started;
    int stop_requested;
    int streaming_enabled;
    int force_key_frame;

    FILE *audio_file;
    annexb_reader_t video_reader;
    video_frame_t current_video_frame;
    int current_video_ready;

    uint64_t audio_next_pts_us;
    uint64_t video_next_pts_us;
    uint64_t output_origin_us;
    uint64_t last_heartbeat_us;
    uint64_t audio_packets_sent;
    uint64_t video_frames_sent;
    uint64_t loop_count;
    uint64_t stream_started_at_us;
    int first_audio_logged;
    int first_video_logged;
    int first_key_frame_logged;
    int first_send_wait_logged;
    int startup_video_pending;
    int output_active;
    int audio_finished;
    int video_finished;
};

static void streamer_log(FILE *stream, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fprintf(stream, "[demo] ");
    vfprintf(stream, fmt, args);
    fputc('\n', stream);
    fflush(stream);
    va_end(args);
}

static void sleep_for_us(uint64_t duration_us)
{
    struct timespec delay;

    delay.tv_sec = (time_t)(duration_us / kMicrosecondsPerSecond);
    delay.tv_nsec = (long)(duration_us % kMicrosecondsPerSecond) * 1000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static void sleep_loop_interval(void)
{
    sleep_for_us((uint64_t)kLoopPollIntervalMs * kMicrosecondsPerMillisecond);
}

static uint64_t monotonic_now_us(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * kMicrosecondsPerSecond +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static void frame_reset(video_frame_t *frame)
{
    frame->size = 0;
    frame->is_key_frame = 0;
}

static void frame_release(video_frame_t *frame)
{
    free(frame->data);
    memset(frame, 0, sizeof(*frame));
}

static int frame_append(video_frame_t *frame, const uint8_t *data, size_t size)
{
    size_t required_capacity = frame->size + size;
    uint8_t *new_data;
    size_t new_capacity;

    if (required_capacity <= frame->capacity) {
        memcpy(frame->data + frame->size, data, size);
        frame->size += size;
        return 0;
    }

    new_capacity = frame->capacity == 0 ? kInitialFrameCapacity : frame->capacity;
    while (new_capacity < required_capacity) {
        new_capacity *= 2;
    }

    new_data = (uint8_t *)realloc(frame->data, new_capacity);
    if (new_data == NULL) {
        return -1;
    }
    frame->data = new_data;
    frame->capacity = new_capacity;
    memcpy(frame->data + frame->size, data, size);
    frame->size += size;
    return 0;
}

static void annexb_reader_destroy(annexb_reader_t *reader)
{
    free(reader->buffer);
    memset(reader, 0, sizeof(*reader));
}

static int annexb_reader_init(annexb_reader_t *reader, FILE *file)
{
    reader->buffer = (uint8_t *)malloc(kVideoReadChunkBytes);
    if (reader->buffer == NULL) {
        return -1;
    }
    reader->file = file;
    reader->capacity = kVideoReadChunkBytes;
    reader->length = 0;
    reader->eof = 0;
    return 0;
}

static int annexb_reader_reset(annexb_reader_t *reader)
{
    if (fseek(reader->file, 0L, SEEK_SET) != 0) {
        return -1;
    }
    clearerr(reader->file);
    reader->length = 0;
    reader->eof = 0;
    return 0;
}

static int annexb_reader_fill(annexb_reader_t *reader)
{
    size_t bytes_read;
    uint8_t *new_buffer;

    if (reader->eof) {
        return 0;
    }
    if (reader->length == reader->capacity) {
        new_buffer = (uint8_t *)realloc(reader->buffer, reader->capacity * 2);
        if (new_buffer == NULL) {
            return -1;
        }
        reader->buffer = new_buffer;
        reader->capacity *= 2;
    }

    bytes_read = fread(reader->buffer + reader->length,
                       1,
                       reader->capacity - reader->length,
                       reader->file);
    reader->length += bytes_read;
    if (bytes_read == 0) {
        if (ferror(reader->file)) {
            return -1;
        }
        reader->eof = 1;
    }
    return 0;
}

static size_t find_start_code(const uint8_t *data,
                              size_t start,
                              size_t length,
                              size_t *out_prefix_length)
{
    size_t index;

    if (length < 3 || start >= length) {
        return SIZE_MAX;
    }
    for (index = start; index + 3 <= length; ++index) {
        if (data[index] == 0x00 && data[index + 1] == 0x00) {
            if (index + 3 < length && data[index + 2] == 0x00 && data[index + 3] == 0x01) {
                *out_prefix_length = 4;
                return index;
            }
            if (data[index + 2] == 0x01) {
                *out_prefix_length = 3;
                return index;
            }
        }
    }
    return SIZE_MAX;
}

static int nal_type_is_vcl(size_t nal_type)
{
    return nal_type >= 1 && nal_type <= 5;
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_offset;
    uint8_t current_byte;
    int bits_remaining;
    int zeros_seen;
} rbsp_bit_reader_t;

static void rbsp_bit_reader_init(rbsp_bit_reader_t *reader,
                                 const uint8_t *data,
                                 size_t size)
{
    memset(reader, 0, sizeof(*reader));
    reader->data = data;
    reader->size = size;
}

static int rbsp_read_bit(rbsp_bit_reader_t *reader, unsigned *out_bit)
{
    while (reader->bits_remaining == 0) {
        uint8_t next_byte;

        if (reader->byte_offset >= reader->size) {
            return -1;
        }
        next_byte = reader->data[reader->byte_offset++];
        if (reader->zeros_seen >= 2 && next_byte == 0x03) {
            reader->zeros_seen = 0;
            continue;
        }
        reader->zeros_seen = next_byte == 0x00 ? reader->zeros_seen + 1 : 0;
        reader->current_byte = next_byte;
        reader->bits_remaining = 8;
    }

    *out_bit = (unsigned)((reader->current_byte >> (reader->bits_remaining - 1)) & 0x01U);
    reader->bits_remaining -= 1;
    return 0;
}

static int rbsp_read_ue(rbsp_bit_reader_t *reader, unsigned *out_value)
{
    unsigned leading_zero_bits = 0;
    unsigned bit;
    unsigned suffix = 0;
    unsigned index;

    while (1) {
        if (rbsp_read_bit(reader, &bit) != 0) {
            return -1;
        }
        if (bit == 1U) {
            break;
        }
        leading_zero_bits += 1;
        if (leading_zero_bits > 31U) {
            return -1;
        }
    }

    for (index = 0; index < leading_zero_bits; ++index) {
        if (rbsp_read_bit(reader, &bit) != 0) {
            return -1;
        }
        suffix = (suffix << 1) | bit;
    }

    *out_value = ((1U << leading_zero_bits) - 1U) + suffix;
    return 0;
}

static int nal_first_mb_is_zero(const uint8_t *nal_data,
                                size_t nal_size,
                                size_t prefix_length)
{
    rbsp_bit_reader_t reader;
    unsigned first_mb_in_slice = 0;

    if (nal_size <= prefix_length + 1) {
        return 0;
    }
    rbsp_bit_reader_init(&reader,
                         nal_data + prefix_length + 1,
                         nal_size - prefix_length - 1);
    if (rbsp_read_ue(&reader, &first_mb_in_slice) != 0) {
        return 0;
    }
    return first_mb_in_slice == 0U;
}

static int nal_starts_new_access_unit(int frame_has_vcl,
                                      size_t nal_type,
                                      const uint8_t *nal_data,
                                      size_t nal_size,
                                      size_t prefix_length)
{
    if (!frame_has_vcl) {
        return 0;
    }
    if (nal_type == 9 || nal_type == 7 || nal_type == 8 || nal_type == 6) {
        return 1;
    }
    if (nal_type_is_vcl(nal_type) &&
        nal_first_mb_is_zero(nal_data, nal_size, prefix_length)) {
        return 1;
    }
    return 0;
}

static void annexb_reader_consume(annexb_reader_t *reader, size_t bytes)
{
    if (bytes >= reader->length) {
        reader->length = 0;
        return;
    }
    memmove(reader->buffer, reader->buffer + bytes, reader->length - bytes);
    reader->length -= bytes;
}

static int annexb_reader_read_frame(annexb_reader_t *reader, video_frame_t *frame)
{
    int frame_has_vcl = 0;

    frame_reset(frame);
    while (1) {
        size_t prefix_length = 0;
        size_t current_prefix_length = 0;
        size_t start_code_offset = find_start_code(reader->buffer,
                                                   0,
                                                   reader->length,
                                                   &prefix_length);
        size_t nal_type;
        size_t nal_size;
        size_t next_start_offset;

        if (start_code_offset == SIZE_MAX) {
            if (reader->eof) {
                return frame->size > 0 ? 1 : 0;
            }
            if (annexb_reader_fill(reader) != 0) {
                return -1;
            }
            continue;
        }
        if (start_code_offset > 0) {
            annexb_reader_consume(reader, start_code_offset);
            continue;
        }
        if (reader->length <= prefix_length) {
            if (reader->eof) {
                return frame->size > 0 ? 1 : 0;
            }
            if (annexb_reader_fill(reader) != 0) {
                return -1;
            }
            continue;
        }

        current_prefix_length = prefix_length;
        nal_type = reader->buffer[prefix_length] & 0x1F;
        next_start_offset = find_start_code(reader->buffer,
                                            prefix_length,
                                            reader->length,
                                            &prefix_length);
        if (next_start_offset == SIZE_MAX && !reader->eof) {
            if (annexb_reader_fill(reader) != 0) {
                return -1;
            }
            continue;
        }

        nal_size = next_start_offset == SIZE_MAX ? reader->length : next_start_offset;
        if (nal_starts_new_access_unit(frame_has_vcl,
                                       nal_type,
                                       reader->buffer,
                                       nal_size,
                                       current_prefix_length)) {
            return 1;
        }

        if (next_start_offset == SIZE_MAX) {
            if (frame_append(frame, reader->buffer, reader->length) != 0) {
                return -1;
            }
            if (nal_type == 5) {
                frame->is_key_frame = 1;
            }
            if (nal_type_is_vcl(nal_type)) {
                frame_has_vcl = 1;
            }
            reader->length = 0;
            return frame_has_vcl ? 1 : 0;
        }

        if (frame_append(frame, reader->buffer, next_start_offset) != 0) {
            return -1;
        }
        if (nal_type == 5) {
            frame->is_key_frame = 1;
        }
        if (nal_type_is_vcl(nal_type)) {
            frame_has_vcl = 1;
        }
        annexb_reader_consume(reader, next_start_offset);
    }
}

static int load_next_video_frame(device_demo_streamer_t *streamer)
{
    int read_result = annexb_reader_read_frame(&streamer->video_reader,
                                               &streamer->current_video_frame);

    if (read_result < 0) {
        return -1;
    }
    if (read_result == 0) {
        streamer->current_video_ready = 0;
        return 0;
    }

    streamer->current_video_frame.pts_us = streamer->video_next_pts_us;
    streamer->video_next_pts_us += kMicrosecondsPerSecond / kVideoFps;
    streamer->current_video_ready = 1;
    return 1;
}

static void clear_stream_start_logs(device_demo_streamer_t *streamer)
{
    streamer->stream_started_at_us = 0;
    streamer->first_audio_logged = 0;
    streamer->first_video_logged = 0;
    streamer->first_key_frame_logged = 0;
    streamer->first_send_wait_logged = 0;
}

static void reset_stream_start_logs(device_demo_streamer_t *streamer)
{
    clear_stream_start_logs(streamer);
    streamer->stream_started_at_us = monotonic_now_us();
}

static void maybe_log_stream_start_event(device_demo_streamer_t *streamer,
                                         int is_audio,
                                         int is_key_frame,
                                         uint64_t pts_us)
{
    uint64_t elapsed_ms;

    if (streamer->stream_started_at_us == 0) {
        return;
    }

    elapsed_ms = (monotonic_now_us() - streamer->stream_started_at_us) /
                 kMicrosecondsPerMillisecond;
    if (is_audio && !streamer->first_audio_logged) {
        streamer->first_audio_logged = 1;
        streamer_log(stdout,
                     "first audio packet sent at +%llums pts=%llums",
                     (unsigned long long)elapsed_ms,
                     (unsigned long long)(pts_us / kMicrosecondsPerMillisecond));
        return;
    }
    if (!is_audio && !streamer->first_video_logged) {
        streamer->first_video_logged = 1;
        streamer_log(stdout,
                     "first video frame sent at +%llums pts=%llums key=%s",
                     (unsigned long long)elapsed_ms,
                     (unsigned long long)(pts_us / kMicrosecondsPerMillisecond),
                     is_key_frame ? "yes" : "no");
    }
    if (!is_audio && is_key_frame && !streamer->first_key_frame_logged) {
        streamer->first_key_frame_logged = 1;
        streamer_log(stdout,
                     "first I frame sent at +%llums pts=%llums",
                     (unsigned long long)elapsed_ms,
                     (unsigned long long)(pts_us / kMicrosecondsPerMillisecond));
    }
}

static int restart_media_from_beginning(device_demo_streamer_t *streamer,
                                        const char *reason,
                                        int reset_pts)
{
    if (fseek(streamer->audio_file, 0L, SEEK_SET) != 0) {
        return -1;
    }
    clearerr(streamer->audio_file);
    if (annexb_reader_reset(&streamer->video_reader) != 0) {
        return -1;
    }
    if (reset_pts) {
        streamer->audio_next_pts_us = 0;
        streamer->video_next_pts_us = 0;
    }
    if (load_next_video_frame(streamer) <= 0) {
        return -1;
    }
    if (!streamer->current_video_frame.is_key_frame) {
        streamer_log(stderr, "video.h264 does not start with an IDR access unit");
        return -1;
    }

    streamer->audio_finished = 0;
    streamer->video_finished = 0;
    streamer_log(stdout, "%s", reason);
    return 0;
}

static int reset_loop_sources(device_demo_streamer_t *streamer)
{
    if (restart_media_from_beginning(streamer,
                                     "rewound media to the first access unit",
                                     0) != 0) {
        return -1;
    }

    streamer->loop_count += 1;
    streamer_log(stdout, "loop rewind #%llu", (unsigned long long)streamer->loop_count);
    return 0;
}

static int read_audio_packet(device_demo_streamer_t *streamer, audio_packet_t *out_packet)
{
    size_t bytes_read;

    bytes_read = fread(out_packet->data, 1, out_packet->capacity, streamer->audio_file);
    if (bytes_read == 0) {
        if (ferror(streamer->audio_file)) {
            return -1;
        }
        streamer->audio_finished = 1;
        return 0;
    }
    if (bytes_read != out_packet->capacity) {
        streamer_log(stderr, "audio.g711a size is not aligned to 320-byte packets");
        return -1;
    }

    out_packet->size = bytes_read;
    out_packet->pts_us = streamer->audio_next_pts_us;
    streamer->audio_next_pts_us +=
        (uint64_t)kAudioPacketDurationMs * kMicrosecondsPerMillisecond;
    return 1;
}

static int send_audio_packet(device_demo_streamer_t *streamer)
{
    uint8_t packet_data[kAudioPacketBytes];
    audio_packet_t packet;
    TIRTCFRAMEINFO frame_info;
    int read_result;
    int send_result;

    memset(&packet, 0, sizeof(packet));
    packet.data = packet_data;
    packet.capacity = sizeof(packet_data);
    read_result = read_audio_packet(streamer, &packet);
    if (read_result <= 0) {
        return read_result;
    }

    memset(&frame_info, 0, sizeof(frame_info));
    frame_info.stream_id = kAudioStreamId;
    frame_info.media = TIRTC_AUDIO_ALAW;
    frame_info.flags = kAudioSampleSpec;
    frame_info.ts = (uint32_t)(packet.pts_us / kMicrosecondsPerMillisecond);
    frame_info.length = (uint32_t)packet.size;

    send_result = TiRtcSendAudioStream(streamer->hconn, &frame_info, packet.data);
    if (send_result < 0) {
        streamer_log(stderr,
                     "audio send failed: %s",
                     TiRtcGetErrorStr(send_result));
    } else {
        streamer->audio_packets_sent += 1;
        maybe_log_stream_start_event(streamer, 1, 0, packet.pts_us);
    }
    return 1;
}

static int advance_to_next_key_frame(device_demo_streamer_t *streamer)
{
    while (streamer->current_video_ready && !streamer->current_video_frame.is_key_frame) {
        if (load_next_video_frame(streamer) < 0) {
            return -1;
        }
        if (!streamer->current_video_ready) {
            streamer->video_finished = 1;
            return 0;
        }
    }
    return 0;
}

static int send_video_frame_now(device_demo_streamer_t *streamer)
{
    TIRTCFRAMEINFO frame_info;
    uint64_t frame_pts_us;
    int send_result;

    if (!streamer->current_video_ready) {
        streamer->video_finished = 1;
        return 0;
    }
    memset(&frame_info, 0, sizeof(frame_info));
    frame_info.stream_id = kVideoStreamId;
    frame_info.media = TIRTC_VIDEO_H264;
    frame_info.flags = streamer->current_video_frame.is_key_frame ? TIRTC_FRAME_FLAG_KEY_FRAME : 0;
    frame_pts_us = streamer->current_video_frame.pts_us;
    frame_info.ts = (uint32_t)(frame_pts_us / kMicrosecondsPerMillisecond);
    frame_info.length = (uint32_t)streamer->current_video_frame.size;

    send_result = TiRtcSendVideoStream(streamer->hconn,
                                       &frame_info,
                                       streamer->current_video_frame.data);
    if (send_result == TIRTC_E_INVALID_HANDLE) {
        return 0;
    }
    if (send_result < 0) {
        streamer_log(stderr,
                     "video send failed: %s",
                     TiRtcGetErrorStr(send_result));
        return -1;
    }

    streamer->video_frames_sent += 1;
    maybe_log_stream_start_event(streamer,
                                 0,
                                 streamer->current_video_frame.is_key_frame,
                                 frame_pts_us);
    if (load_next_video_frame(streamer) < 0) {
        return -1;
    }
    if (!streamer->current_video_ready) {
        streamer->video_finished = 1;
    }
    return 1;
}

static uint64_t earliest_enabled_pts_us(device_demo_streamer_t *streamer,
                                        int audio_enabled,
                                        int video_enabled)
{
    uint64_t audio_pts = (audio_enabled && !streamer->audio_finished)
                             ? streamer->audio_next_pts_us
                             : UINT64_MAX;
    uint64_t video_pts = (video_enabled && !streamer->video_finished &&
                          streamer->current_video_ready)
                             ? streamer->current_video_frame.pts_us
                             : UINT64_MAX;

    return audio_pts < video_pts ? audio_pts : video_pts;
}

static void maybe_log_heartbeat(device_demo_streamer_t *streamer, uint64_t now_us)
{
    if (now_us - streamer->last_heartbeat_us <
        (uint64_t)kHeartbeatIntervalMs * kMicrosecondsPerMillisecond) {
        return;
    }

    streamer->last_heartbeat_us = now_us;
    streamer_log(stdout,
                 "streaming heartbeat audio_packets=%llu video_frames=%llu loops=%llu",
                 (unsigned long long)streamer->audio_packets_sent,
                 (unsigned long long)streamer->video_frames_sent,
                 (unsigned long long)streamer->loop_count);
}

static int handle_startup_video_pending(device_demo_streamer_t *streamer,
                                        int *out_handled)
{
    int send_result;

    *out_handled = 0;
    if (!streamer->startup_video_pending) {
        return 0;
    }

    *out_handled = 1;
    if (!streamer->current_video_ready || !streamer->current_video_frame.is_key_frame) {
        if (restart_media_from_beginning(streamer,
                                        "rewound media to cache startup I frame",
                                        1) != 0) {
            streamer_log(stderr, "failed to rewind media for startup I frame");
            return -1;
        }
    }

    send_result = send_video_frame_now(streamer);
    if (send_result < 0) {
        streamer_log(stderr, "startup I frame send failed");
        return -1;
    }
    if (send_result == 0) {
        if (!streamer->first_send_wait_logged) {
            streamer->first_send_wait_logged = 1;
            streamer_log(stdout,
                         "connection accepted but media path is not writable yet; caching startup I frame");
        }
        sleep_loop_interval();
        return 0;
    }

    streamer->startup_video_pending = 0;
    streamer->first_send_wait_logged = 0;
    streamer->output_active = 0;
    streamer_log(stdout, "startup I frame flushed; entering steady streaming");
    return 0;
}

static int handle_requested_key_frame(device_demo_streamer_t *streamer,
                                      int force_key_frame,
                                      int *out_handled)
{
    int send_result;

    *out_handled = 0;
    if (!force_key_frame) {
        return 0;
    }

    *out_handled = 1;
    if (advance_to_next_key_frame(streamer) != 0) {
        streamer_log(stderr, "failed to advance to requested key frame");
        return -1;
    }
    if (streamer->video_finished || !streamer->current_video_ready) {
        return 0;
    }

    streamer_log(stdout, "sending requested I frame immediately");
    send_result = send_video_frame_now(streamer);
    if (send_result < 0) {
        streamer_log(stderr, "immediate key frame send failed");
        return -1;
    }
    if (send_result == 0) {
        pthread_mutex_lock(&streamer->mutex);
        streamer->force_key_frame = 1;
        pthread_mutex_unlock(&streamer->mutex);
        if (!streamer->first_send_wait_logged) {
            streamer->first_send_wait_logged = 1;
            streamer_log(stdout,
                         "video path not writable yet; retrying requested I frame");
        }
        sleep_loop_interval();
        return 0;
    }

    streamer->first_send_wait_logged = 0;
    streamer->output_active = 0;
    return 0;
}

static void *streamer_worker_main(void *opaque)
{
    device_demo_streamer_t *streamer = (device_demo_streamer_t *)opaque;

    streamer_log(stdout, "media files opened, streamer worker started");
    while (1) {
        int stop_requested;
        int streaming_enabled;
        int force_key_frame;
        uint64_t now_us;
        uint64_t target_pts_us;
        uint64_t target_time_us;

        pthread_mutex_lock(&streamer->mutex);
        stop_requested = streamer->stop_requested;
        streaming_enabled = streamer->streaming_enabled;
        force_key_frame = streamer->force_key_frame;
        if (force_key_frame) {
            streamer->force_key_frame = 0;
        }
        pthread_mutex_unlock(&streamer->mutex);

        if (stop_requested) {
            break;
        }
        if (!streaming_enabled) {
            streamer->output_active = 0;
            sleep_loop_interval();
            continue;
        }

        {
            int handled = 0;

            if (handle_startup_video_pending(streamer, &handled) != 0) {
                break;
            }
            if (handled) {
                continue;
            }
            if (handle_requested_key_frame(streamer, force_key_frame, &handled) != 0) {
                break;
            }
            if (handled) {
                continue;
            }
        }

        if (streamer->audio_finished && streamer->video_finished) {
            if (reset_loop_sources(streamer) != 0) {
                streamer_log(stderr, "failed to rewind media loop");
                break;
            }
            streamer->output_active = 0;
            continue;
        }

        target_pts_us = earliest_enabled_pts_us(streamer, streaming_enabled, streaming_enabled);
        if (!streamer->output_active) {
            now_us = monotonic_now_us();
            streamer->output_origin_us = now_us - target_pts_us;
            streamer->last_heartbeat_us = now_us;
            streamer->output_active = 1;
        }

        now_us = monotonic_now_us();
        maybe_log_heartbeat(streamer, now_us);
        target_time_us = streamer->output_origin_us + target_pts_us;
        if (target_time_us > now_us) {
            uint64_t sleep_us = target_time_us - now_us;
            if (sleep_us > (uint64_t)kLoopPollIntervalMs * 1000ULL) {
                sleep_us = (uint64_t)kLoopPollIntervalMs * 1000ULL;
            }
            sleep_for_us(sleep_us);
            continue;
        }

        if (!streamer->audio_finished &&
            (!streamer->current_video_ready ||
             streamer->audio_next_pts_us <= streamer->current_video_frame.pts_us)) {
            if (send_audio_packet(streamer) < 0) {
                streamer_log(stderr, "audio loop failed");
                break;
            }
            continue;
        }

        if (!streamer->video_finished) {
            int send_result = send_video_frame_now(streamer);

            if (send_result < 0) {
                streamer_log(stderr, "video loop failed");
                break;
            }
            if (send_result == 0) {
                sleep_loop_interval();
                continue;
            }
            continue;
        }

        if (!streamer->audio_finished) {
            if (send_audio_packet(streamer) < 0) {
                streamer_log(stderr, "audio loop failed");
                break;
            }
            continue;
        }

        sleep_loop_interval();
    }

    streamer_log(stdout,
                 "streamer worker exit audio_packets=%llu video_frames=%llu loops=%llu",
                 (unsigned long long)streamer->audio_packets_sent,
                 (unsigned long long)streamer->video_frames_sent,
                 (unsigned long long)streamer->loop_count);
    return NULL;
}

int device_demo_streamer_create(device_demo_streamer_t **out_streamer,
                                tirtc_conn_t hconn)
{
    char audio_path[kMaxPathBytes];
    char video_path[kMaxPathBytes];
    device_demo_streamer_t *streamer;

    *out_streamer = NULL;
    streamer = (device_demo_streamer_t *)calloc(1, sizeof(*streamer));
    if (streamer == NULL) {
        return -1;
    }
    streamer->hconn = hconn;
    pthread_mutex_init(&streamer->mutex, NULL);

    if (snprintf(audio_path, sizeof(audio_path), "%s", kAudioFilePath) <= 0 ||
        snprintf(video_path, sizeof(video_path), "%s", kVideoFilePath) <= 0) {
        device_demo_streamer_destroy(streamer);
        return -1;
    }

    streamer->audio_file = fopen(audio_path, "rb");
    if (streamer->audio_file == NULL) {
        streamer_log(stderr, "failed to open %s", audio_path);
        device_demo_streamer_destroy(streamer);
        return -1;
    }

    streamer->video_reader.file = fopen(video_path, "rb");
    if (streamer->video_reader.file == NULL) {
        streamer_log(stderr, "failed to open %s", video_path);
        device_demo_streamer_destroy(streamer);
        return -1;
    }
    if (annexb_reader_init(&streamer->video_reader, streamer->video_reader.file) != 0) {
        streamer_log(stderr, "failed to initialize Annex B reader");
        device_demo_streamer_destroy(streamer);
        return -1;
    }
    if (load_next_video_frame(streamer) <= 0) {
        streamer_log(stderr, "failed to read the first H264 access unit");
        device_demo_streamer_destroy(streamer);
        return -1;
    }
    if (!streamer->current_video_frame.is_key_frame) {
        streamer_log(stderr, "first H264 access unit is not an IDR frame");
        device_demo_streamer_destroy(streamer);
        return -1;
    }

    if (pthread_create(&streamer->worker, NULL, streamer_worker_main, streamer) != 0) {
        streamer_log(stderr, "failed to create streamer worker");
        device_demo_streamer_destroy(streamer);
        return -1;
    }
    streamer->worker_started = 1;
    *out_streamer = streamer;
    return 0;
}

void device_demo_streamer_stop(device_demo_streamer_t *streamer)
{
    if (streamer == NULL) {
        return;
    }

    pthread_mutex_lock(&streamer->mutex);
    streamer->stop_requested = 1;
    pthread_mutex_unlock(&streamer->mutex);
    if (streamer->worker_started) {
        pthread_join(streamer->worker, NULL);
        streamer->worker_started = 0;
    }
}

void device_demo_streamer_destroy(device_demo_streamer_t *streamer)
{
    if (streamer == NULL) {
        return;
    }

    device_demo_streamer_stop(streamer);
    frame_release(&streamer->current_video_frame);
    if (streamer->video_reader.file != NULL) {
        fclose(streamer->video_reader.file);
    }
    annexb_reader_destroy(&streamer->video_reader);
    if (streamer->audio_file != NULL) {
        fclose(streamer->audio_file);
    }
    pthread_mutex_destroy(&streamer->mutex);
    free(streamer);
}

void device_demo_streamer_set_streaming_enabled(device_demo_streamer_t *streamer,
                                                int enabled)
{
    pthread_mutex_lock(&streamer->mutex);
    if (!!streamer->streaming_enabled != !!enabled) {
        streamer->output_active = 0;
        if (enabled) {
            streamer->startup_video_pending = 1;
            streamer->force_key_frame = 0;
            reset_stream_start_logs(streamer);
        } else {
            streamer->startup_video_pending = 0;
            streamer->force_key_frame = 0;
            clear_stream_start_logs(streamer);
        }
    }
    streamer->streaming_enabled = enabled != 0;
    pthread_mutex_unlock(&streamer->mutex);
}

void device_demo_streamer_request_key_frame(device_demo_streamer_t *streamer)
{
    pthread_mutex_lock(&streamer->mutex);
    streamer->force_key_frame = 1;
    pthread_mutex_unlock(&streamer->mutex);
}
