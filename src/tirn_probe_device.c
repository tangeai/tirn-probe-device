#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef TIRTC_SDK_VARIANT
#define TIRTC_SDK_VARIANT "unknown"
#endif

#ifndef TIRTC_SDK_SOURCE_URL
#define TIRTC_SDK_SOURCE_URL "unknown"
#endif

#include "tiRTC.h"
#include "ogg_opus.h"
#include "media_sample.h"

enum {
    CMD_TIME_SYNC_REQUEST = 0x2010,
    CMD_TIME_SYNC_RESPONSE = 0x2011,

    AUDIO_STREAM_ID = 0,
    VIDEO_STREAM_ID = 1,
    AUDIO_SAMPLE_RATE_HZ = 8000,
    AUDIO_BYTES_PER_SAMPLE = 2,
    AUDIO_PAYLOAD_BYTES = 640,

    DEFAULT_ITERATIONS = 1,
    DEFAULT_REPEAT = 20,
    DEFAULT_INTERVAL_MS = 100,
    DEFAULT_TIMEOUT_MS = 1000,
    DEFAULT_CONNECT_TIMEOUT_MS = 20000,
    DEFAULT_DURATION_SEC = 10,
    DEFAULT_CONNECTIONS = 1,
    DEFAULT_FRAME_MS = 40,
    DEFAULT_LOG_LEVEL = 3,
    DEFAULT_START_RETRIES = 5,
    SDK_STOP_TIMEOUT_MS = 3000,
    DISCONNECT_TIMEOUT_MS = 3000,
    CALLBACK_EVENT_RING_CAP = 2048,
    CALLBACK_EVENT_DATA_BYTES = 2048,

    AUDIO_TS_INDEX_BITS = 12,
    AUDIO_TS_INDEX_MASK = 0x0fff,
    AUDIO_TS_TIME_MASK = 0x0fffff,
    AUDIO_ECHO_WAIT_TIMEOUT_SEC = 75,
    AUDIO_ECHO_IDLE_GRACE_MS = 2000,
};

static const int64_t kAudioTimestampUnitUs = 100LL;
static const int64_t kAudioTimestampPeriodUs = 104857600LL;
static const int64_t kAudioTimestampToleranceUs = 2000000LL;

static const uint32_t kSdkMaxSendBufferBytes = 2U * 1024U * 1024U;
static const char kClientIdSuffix[] = "-tirn-probe";
static const double kAudioToneHz = 440.0;
static const double kTwoPi = 6.28318530717958647692;
static int16_t g_audio_tone_samples[AUDIO_SAMPLE_RATE_HZ];
static int g_audio_tone_samples_initialized;

typedef enum {
    COMMAND_CONNECT,
    COMMAND_IDLE,
    COMMAND_TIMESYNC,
    COMMAND_MEDIA,
    COMMAND_AUDIO,
} probe_command_t;

typedef struct {
    probe_command_t command;
    const char *endpoint;
    const char *device_id;
    const char *device_secret_key;
    const char *peer_id;
    const char *token;
    int iterations;
    int connections;
    int repeat;
    int interval_ms;
    int timeout_ms;
    int connect_timeout_ms;
    int64_t duration_sec;
    int frame_ms;
    int audio_iterations;
    int start_retries;
    int log_level;
    int json_output;
    int client_mode;
    const char *audio_sample_log;
    const char *audio_input;
    const char *audio_echo_output;
    const char *audio_output;
    int duration_explicit;
} probe_config_t;

typedef struct {
    int64_t *values;
    size_t len;
    size_t cap;
} sample_set_t;

typedef struct {
    int64_t client_send_unix_ns;
    int64_t server_recv_unix_ns;
    int64_t client_recv_unix_ns;
    int64_t rtt_us;
    int64_t offset_ns;
    int ok;
} time_sync_sample_t;

typedef struct {
    uint32_t send_index;
    uint32_t frame_index;
    uint32_t frame_duration_us;
    uint32_t outbound_packed_ts;
    uint32_t echo_packed_ts;
    int64_t client_send_unix_us;
    int64_t client_send_mono_us;
    int64_t estimated_server_send_us;
    int64_t clock_offset_us;
    int64_t time_sync_rtt_us;
    int64_t send_late_us;
    int64_t client_echo_recv_mono_us;
    int64_t client_echo_recv_unix_us;
    int64_t server_recv_unix_us;
    int send_ret;
    int echoed;
    int duplicate_echo_count;
    const char *timestamp_decode_status;
} audio_sample_t;

typedef struct {
    audio_sample_t *items;
    size_t len;
    size_t cap;
    uint32_t next_frame_index;
    int64_t first_client_send_unix_us;
    int64_t first_echo_recv_mono_us;
    int64_t last_echo_recv_mono_us;
    int64_t call_started_mono_us;
    int64_t call_finished_mono_us;
    int64_t max_send_late_us;
    int expected_frame_ms;
    uint64_t late_send_count;
    uint64_t send_count;
    uint64_t send_failed;
    uint64_t echo_count;
} audio_metrics_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    tirtc_conn_t hconn;
    int connect_done;
    int connect_error;
    int disconnected;
    int conn_error;
    int idle_only;
    int media_mode;
    uint64_t media_audio_received;
    uint64_t media_video_received;
    FILE *media_audio_output;
    uint64_t media_audio_output_bytes;
    int media_audio_output_error;
    int64_t connect_started_us;
    int64_t connect_cost_us;
    uint32_t waiting_timesync_seq;
    int64_t waiting_timesync_client_send_ns;
    int timesync_done;
    int timesync_error;
    time_sync_sample_t timesync_sample;
    audio_metrics_t audio;
    ogg_opus_writer_t echo_writer;
    int echo_writer_open;
    int echo_writer_error;
    int64_t echo_timeline_origin_us;
    uint64_t echo_last_granule;
} probe_session_t;

typedef enum {
    CALLBACK_EVENT_SYSTEM,
    CALLBACK_EVENT_CONNECT_RESULT,
    CALLBACK_EVENT_CONN_ERROR,
    CALLBACK_EVENT_DISCONNECTED,
    CALLBACK_EVENT_COMMAND,
    CALLBACK_EVENT_AUDIO,
    CALLBACK_EVENT_VIDEO,
} callback_event_type_t;

typedef struct callback_event {
    callback_event_type_t type;
    probe_session_t *session;
    tirtc_conn_t hconn;
    int event;
    int error;
    uint32_t cmdw;
    uint32_t len;
    TIRTCFRAMEINFO frame_info;
    uint8_t data[CALLBACK_EVENT_DATA_BYTES];
} callback_event_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t callback_worker;
    callback_event_t event_ring[CALLBACK_EVENT_RING_CAP];
    atomic_size_t event_read_index;
    atomic_size_t event_write_index;
    atomic_uint_fast64_t callback_events_dropped;
    atomic_bool callback_worker_stop;
    int sdk_started;
    int sdk_stopped;
    int callback_worker_running;
    probe_session_t *active_session;
} probe_app_t;

static probe_app_t g_app = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};
static volatile sig_atomic_t g_should_exit = 0;

static void log_message(FILE *stream, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fprintf(stream, "[tirn-probe] ");
    vfprintf(stream, fmt, args);
    fputc('\n', stream);
    fflush(stream);
    va_end(args);
}

static int64_t monotonic_now_us(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000LL + (int64_t)now.tv_nsec / 1000LL;
}

static int64_t realtime_now_ns(void)
{
    struct timespec now;

    clock_gettime(CLOCK_REALTIME, &now);
    return (int64_t)now.tv_sec * 1000000000LL + (int64_t)now.tv_nsec;
}

static int64_t thread_cpu_now_ns(void)
{
    struct timespec now;

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return (int64_t)now.tv_sec * 1000000000LL + (int64_t)now.tv_nsec;
}

static void sleep_for_us(int64_t duration_us)
{
    struct timespec delay;

    if (duration_us <= 0) {
        return;
    }
    delay.tv_sec = (time_t)(duration_us / 1000000LL);
    delay.tv_nsec = (long)(duration_us % 1000000LL) * 1000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static void sleep_until_monotonic_us(int64_t deadline_us)
{
#ifdef __linux__
    struct timespec deadline;

    if (deadline_us <= 0) {
        return;
    }
    deadline.tv_sec = (time_t)(deadline_us / 1000000LL);
    deadline.tv_nsec = (long)(deadline_us % 1000000LL) * 1000L;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR) {
    }
#else
    for (;;) {
        int64_t now_us = monotonic_now_us();
        if (now_us >= deadline_us) {
            return;
        }
        sleep_for_us(deadline_us - now_us);
    }
#endif
}

static int make_deadline_ms(int timeout_ms, struct timespec *out_deadline)
{
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return -1;
    }
    out_deadline->tv_sec = now.tv_sec + (time_t)(timeout_ms / 1000);
    out_deadline->tv_nsec = now.tv_nsec + (long)(timeout_ms % 1000) * 1000000L;
    if (out_deadline->tv_nsec >= 1000000000L) {
        out_deadline->tv_sec += 1;
        out_deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

static void write_le16_i16(uint8_t *p, int16_t value)
{
    uint16_t uvalue = (uint16_t)value;

    p[0] = (uint8_t)uvalue;
    p[1] = (uint8_t)(uvalue >> 8);
}

static int sample_set_push(sample_set_t *set, int64_t value)
{
    int64_t *new_values;
    size_t new_cap;

    if (set->len == set->cap) {
        new_cap = set->cap == 0 ? 16U : set->cap * 2U;
        new_values = (int64_t *)realloc(set->values, new_cap * sizeof(*new_values));
        if (new_values == NULL) {
            return -1;
        }
        set->values = new_values;
        set->cap = new_cap;
    }
    set->values[set->len++] = value;
    return 0;
}

static void sample_set_free(sample_set_t *set)
{
    free(set->values);
    memset(set, 0, sizeof(*set));
}

static int compare_i64(const void *lhs, const void *rhs)
{
    int64_t a = *(const int64_t *)lhs;
    int64_t b = *(const int64_t *)rhs;

    return (a > b) - (a < b);
}

static int compare_timesync_by_rtt(const void *lhs, const void *rhs)
{
    const time_sync_sample_t *a = (const time_sync_sample_t *)lhs;
    const time_sync_sample_t *b = (const time_sync_sample_t *)rhs;

    return (a->rtt_us > b->rtt_us) - (a->rtt_us < b->rtt_us);
}

static int64_t percentile_sorted(const sample_set_t *set, double percentile)
{
    double rank;
    size_t index;

    if (set->len == 0) {
        return 0;
    }
    if (set->len == 1) {
        return set->values[0];
    }
    rank = percentile * (double)(set->len - 1U);
    index = (size_t)llround(rank);
    if (index >= set->len) {
        index = set->len - 1U;
    }
    return set->values[index];
}

static double average_samples(const sample_set_t *set)
{
    long double total = 0.0;
    size_t i;

    if (set->len == 0) {
        return 0.0;
    }
    for (i = 0; i < set->len; ++i) {
        total += (long double)set->values[i];
    }
    return (double)(total / (long double)set->len);
}

static double us_to_ms(int64_t value_us)
{
    return (double)value_us / 1000.0;
}

static void print_duration_summary_ms(const char *name, sample_set_t *set)
{
    if (set->len == 0) {
        printf("%s: no samples\n", name);
        return;
    }
    qsort(set->values, set->len, sizeof(*set->values), compare_i64);
    printf("%s: count=%zu avg=%.2fms p50=%.2fms p90=%.2fms p95=%.2fms p99=%.2fms\n",
           name,
           set->len,
           average_samples(set) / 1000.0,
           us_to_ms(percentile_sorted(set, 0.50)),
           us_to_ms(percentile_sorted(set, 0.90)),
           us_to_ms(percentile_sorted(set, 0.95)),
           us_to_ms(percentile_sorted(set, 0.99)));
}

static char *build_client_id(const char *device_id)
{
    size_t device_len = strlen(device_id);
    size_t suffix_len = strlen(kClientIdSuffix);
    char *client_id = (char *)malloc(device_len + suffix_len + 1U);

    if (client_id == NULL) {
        return NULL;
    }
    memcpy(client_id, device_id, device_len);
    memcpy(client_id + device_len, kClientIdSuffix, suffix_len + 1U);
    return client_id;
}

static int json_get_i64(const char *json, const char *key, int64_t *out)
{
    char pattern[64];
    const char *pos;
    char *endptr;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    pos = strstr(json, pattern);
    if (pos == NULL) {
        return -1;
    }
    pos = strchr(pos + strlen(pattern), ':');
    if (pos == NULL) {
        return -1;
    }
    pos++;
    while (*pos == ' ' || *pos == '\t') {
        pos++;
    }
    errno = 0;
    *out = strtoll(pos, &endptr, 10);
    if (errno != 0 || endptr == pos) {
        return -1;
    }
    return 0;
}

static int json_get_u32(const char *json, const char *key, uint32_t *out)
{
    int64_t value;

    if (json_get_i64(json, key, &value) != 0 || value < 0 || value > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

static char *copy_payload_as_string(const void *data, uint32_t len)
{
    char *text = (char *)malloc((size_t)len + 1U);

    if (text == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(text, data, len);
    }
    text[len] = '\0';
    return text;
}

static uint32_t pack_audio_timestamp(int64_t unix_us, uint32_t frame_index)
{
    uint32_t tick = (uint32_t)(unix_us / kAudioTimestampUnitUs) & AUDIO_TS_TIME_MASK;
    return (tick << AUDIO_TS_INDEX_BITS) | (frame_index & AUDIO_TS_INDEX_MASK);
}

static uint32_t unpack_audio_frame_index(uint32_t packed)
{
    return packed & AUDIO_TS_INDEX_MASK;
}

static int reconstruct_audio_timestamp_us(uint32_t packed,
                                          int64_t lower_us,
                                          int64_t upper_us,
                                          int64_t *out_us)
{
    int64_t encoded_us = (int64_t)(packed >> AUDIO_TS_INDEX_BITS) * kAudioTimestampUnitUs;
    int64_t cycle = (lower_us - encoded_us) / kAudioTimestampPeriodUs;
    int count = 0;
    int64_t candidate;

    if (encoded_us + cycle * kAudioTimestampPeriodUs < lower_us) {
        cycle++;
    }
    for (candidate = encoded_us + cycle * kAudioTimestampPeriodUs;
         candidate <= upper_us;
         candidate += kAudioTimestampPeriodUs) {
        *out_us = candidate;
        count++;
        if (count > 1) {
            break;
        }
    }
    return count;
}

static audio_sample_t *audio_metrics_find(audio_metrics_t *metrics,
                                          uint32_t frame_index,
                                          uint32_t packed,
                                          int64_t echo_recv_unix_us,
                                          int *ambiguous,
                                          int *duplicate)
{
    size_t i;
    audio_sample_t *only_index_match = NULL;
    audio_sample_t *valid_match = NULL;
    audio_sample_t *echoed = NULL;
    int index_matches = 0;
    int valid_matches = 0;

    *ambiguous = 0;
    *duplicate = 0;
    for (i = 0; i < metrics->len; ++i) {
        if ((metrics->items[i].frame_index & AUDIO_TS_INDEX_MASK) != frame_index) {
            continue;
        }
        if (metrics->items[i].echoed) {
            echoed = &metrics->items[i];
        } else {
            int64_t candidate_us = 0;
            int64_t lower_us = metrics->items[i].estimated_server_send_us -
                               kAudioTimestampToleranceUs;
            int64_t upper_us = echo_recv_unix_us + metrics->items[i].clock_offset_us +
                               kAudioTimestampToleranceUs;
            int candidates = reconstruct_audio_timestamp_us(packed,
                                                            lower_us,
                                                            upper_us,
                                                            &candidate_us);
            only_index_match = &metrics->items[i];
            index_matches++;
            if (candidates == 1) {
                valid_match = &metrics->items[i];
                valid_matches++;
            }
        }
    }
    if (valid_matches == 1) {
        return valid_match;
    }
    if (valid_matches > 1 || index_matches > 1) {
        for (i = 0; i < metrics->len; ++i) {
            if (!metrics->items[i].echoed &&
                (metrics->items[i].frame_index & AUDIO_TS_INDEX_MASK) == frame_index) {
                metrics->items[i].timestamp_decode_status = "ambiguous_frame_index";
            }
        }
        *ambiguous = 1;
        return NULL;
    }
    if (index_matches == 1) {
        return only_index_match;
    }
    if (echoed != NULL) {
        echoed->duplicate_echo_count++;
        *duplicate = 1;
    }
    return NULL;
}

static audio_sample_t *audio_metrics_append(audio_metrics_t *metrics,
                                            uint32_t send_index,
                                            uint32_t frame_index,
                                            uint32_t frame_duration_us,
                                            uint32_t outbound_packed_ts,
                                            int64_t send_unix_us,
                                            int64_t send_mono_us,
                                            int64_t estimated_server_send_us,
                                            int64_t clock_offset_us,
                                            int64_t time_sync_rtt_us,
                                            int64_t send_late_us)
{
    audio_sample_t *new_items;
    size_t new_cap;
    audio_sample_t *sample;

    if (metrics->len == metrics->cap) {
        new_cap = metrics->cap == 0 ? 128U : metrics->cap * 2U;
        new_items = (audio_sample_t *)realloc(metrics->items, new_cap * sizeof(*new_items));
        if (new_items == NULL) {
            return NULL;
        }
        metrics->items = new_items;
        metrics->cap = new_cap;
    }
    sample = &metrics->items[metrics->len++];
    memset(sample, 0, sizeof(*sample));
    sample->send_index = send_index;
    sample->frame_index = frame_index;
    sample->frame_duration_us = frame_duration_us;
    sample->outbound_packed_ts = outbound_packed_ts;
    sample->client_send_unix_us = send_unix_us;
    sample->client_send_mono_us = send_mono_us;
    sample->estimated_server_send_us = estimated_server_send_us;
    sample->clock_offset_us = clock_offset_us;
    sample->time_sync_rtt_us = time_sync_rtt_us;
    sample->send_late_us = send_late_us;
    sample->send_ret = 0;
    sample->timestamp_decode_status = "not_received";
    return sample;
}

static void audio_metrics_free(audio_metrics_t *metrics)
{
    free(metrics->items);
    memset(metrics, 0, sizeof(*metrics));
}

static void audio_metrics_reset_iteration(audio_metrics_t *metrics)
{
    audio_sample_t *items = metrics->items;
    size_t cap = metrics->cap;

    memset(metrics, 0, sizeof(*metrics));
    metrics->items = items;
    metrics->cap = cap;
}

static void signal_handler(int signo)
{
    (void)signo;
    g_should_exit = 1;
}

static int enqueue_callback_event(const callback_event_t *event)
{
    size_t write_index;
    size_t read_index;
    callback_event_t *slot;

    if (event == NULL || atomic_load_explicit(&g_app.callback_worker_stop, memory_order_acquire)) {
        return -1;
    }
    write_index = atomic_load_explicit(&g_app.event_write_index, memory_order_relaxed);
    read_index = atomic_load_explicit(&g_app.event_read_index, memory_order_acquire);
    if (write_index - read_index >= CALLBACK_EVENT_RING_CAP) {
        atomic_fetch_add_explicit(&g_app.callback_events_dropped, 1, memory_order_relaxed);
        return -1;
    }
    slot = &g_app.event_ring[write_index % CALLBACK_EVENT_RING_CAP];
    *slot = *event;
    atomic_store_explicit(&g_app.event_write_index, write_index + 1U, memory_order_release);
    return 0;
}

static int dequeue_callback_event(callback_event_t *out_event)
{
    size_t read_index;
    size_t write_index;

    read_index = atomic_load_explicit(&g_app.event_read_index, memory_order_relaxed);
    write_index = atomic_load_explicit(&g_app.event_write_index, memory_order_acquire);
    if (read_index == write_index) {
        return 0;
    }
    *out_event = g_app.event_ring[read_index % CALLBACK_EVENT_RING_CAP];
    atomic_store_explicit(&g_app.event_read_index, read_index + 1U, memory_order_release);
    return 1;
}

static probe_session_t *active_session_for_conn(tirtc_conn_t hconn)
{
    probe_session_t *session = NULL;

    if (hconn != NULL) {
        session = (probe_session_t *)TiRtcConnGetUserData(hconn);
    }
    if (session != NULL) {
        return session;
    }
    pthread_mutex_lock(&g_app.mutex);
    session = g_app.active_session;
    pthread_mutex_unlock(&g_app.mutex);
    return session;
}

static void handle_event(int event)
{
    pthread_mutex_lock(&g_app.mutex);
    if (event == TIRTC_EVENT_SYS_STARTED) {
        g_app.sdk_started = 1;
        pthread_cond_broadcast(&g_app.cond);
    } else if (event == TIRTC_EVENT_SYS_STOPPED) {
        g_app.sdk_stopped = 1;
        pthread_cond_broadcast(&g_app.cond);
    }
    pthread_mutex_unlock(&g_app.mutex);

    if (event == TIRTC_EVENT_ACCESS_HIJACKING) {
        log_message(stderr, "warning: endpoint access may be hijacked");
    }
}

static void handle_connect_result(probe_session_t *session, int error, tirtc_conn_t hconn)
{
    if (session == NULL) {
        return;
    }
    if (error == 0 && hconn != NULL) {
        (void)TiRtcConnSetUserData(hconn, session);
    }
    pthread_mutex_lock(&session->mutex);
    session->connect_done = 1;
    session->connect_error = error;
    session->hconn = hconn;
    session->connect_cost_us = monotonic_now_us() - session->connect_started_us;
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mutex);
}

static void handle_conn_error(tirtc_conn_t hconn, int error)
{
    probe_session_t *session = active_session_for_conn(hconn);

    if (session == NULL) {
        return;
    }
    pthread_mutex_lock(&session->mutex);
    session->conn_error = error == 0 ? TIRTC_E_CONN_OTHER_ERROR : error;
    session->disconnected = 1;
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mutex);
}

static void handle_disconnected(tirtc_conn_t hconn)
{
    probe_session_t *session = active_session_for_conn(hconn);

    if (session == NULL) {
        return;
    }
    pthread_mutex_lock(&session->mutex);
    session->disconnected = 1;
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mutex);
}

static void handle_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len)
{
    probe_session_t *session = active_session_for_conn(hconn);
    char *json;

    if (session == NULL || data == NULL) {
        return;
    }
    json = copy_payload_as_string(data, len);
    if (json == NULL) {
        return;
    }

    pthread_mutex_lock(&session->mutex);
    if (cmdw == CMD_TIME_SYNC_RESPONSE) {
        uint32_t seq = 0;
        int64_t client_send_unix_ns = 0;
        int64_t server_recv_unix_ns = 0;
        if (json_get_u32(json, "seq", &seq) == 0 &&
            json_get_i64(json, "client_send_unix_ns", &client_send_unix_ns) == 0 &&
            json_get_i64(json, "server_recv_unix_ns", &server_recv_unix_ns) == 0 &&
            seq == session->waiting_timesync_seq) {
            int64_t client_recv_unix_ns = realtime_now_ns();
            session->timesync_sample.client_send_unix_ns = client_send_unix_ns;
            session->timesync_sample.server_recv_unix_ns = server_recv_unix_ns;
            session->timesync_sample.client_recv_unix_ns = client_recv_unix_ns;
            session->timesync_sample.rtt_us = (client_recv_unix_ns - client_send_unix_ns) / 1000LL;
            session->timesync_sample.offset_ns =
                server_recv_unix_ns - ((client_send_unix_ns + client_recv_unix_ns) / 2LL);
            session->timesync_sample.ok = 1;
            session->timesync_done = 1;
            pthread_cond_broadcast(&session->cond);
        }
    }
    pthread_mutex_unlock(&session->mutex);
    free(json);
}

static void handle_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
    probe_session_t *session = active_session_for_conn(hconn);
    uint32_t frame_index;
    int64_t now_mono_us;
    int64_t now_unix_us;
    audio_sample_t *sample;
    uint32_t duration_samples;
    int new_echo = 0;

    if (session == NULL || info == NULL || data == NULL) {
        return;
    }
    if (session->idle_only) {
        return;
    }
    if (session->media_mode) {
        pthread_mutex_lock(&session->mutex);
        session->media_audio_received++;
        if (session->media_audio_output != NULL && !session->media_audio_output_error) {
            if (info->media != TIRTC_AUDIO_PCM) {
                log_message(stderr,
                            "cannot save media audio: expected PCM, received media=%u",
                            (unsigned int)info->media);
                session->media_audio_output_error = 1;
            } else if (fwrite(data, 1, info->length, session->media_audio_output) !=
                       info->length) {
                log_message(stderr, "failed to write media audio output");
                session->media_audio_output_error = 1;
            } else {
                session->media_audio_output_bytes += info->length;
            }
        }
        pthread_mutex_unlock(&session->mutex);
        return;
    }
    frame_index = unpack_audio_frame_index(info->ts);

    now_mono_us = monotonic_now_us();
    now_unix_us = realtime_now_ns() / 1000LL;
    duration_samples = info->media == TIRTC_AUDIO_OPUS ?
        ogg_opus_packet_duration_samples_48k(data, info->length) :
        (session->audio.expected_frame_ms > 0 ?
         (uint32_t)session->audio.expected_frame_ms * 48U : 0);

    pthread_mutex_lock(&session->mutex);
    {
        int ambiguous = 0;
        int duplicate = 0;
        sample = audio_metrics_find(&session->audio,
                                    frame_index,
                                    info->ts,
                                    now_unix_us,
                                    &ambiguous,
                                    &duplicate);
        if (ambiguous) {
            log_message(stderr, "ambiguous echoed audio frame_index=%" PRIu32, frame_index);
        } else if (duplicate) {
            log_message(stderr, "duplicate echoed audio frame_index=%" PRIu32, frame_index);
        }
    }
    if (sample != NULL && !sample->echoed) {
        int64_t lower_us = sample->estimated_server_send_us - kAudioTimestampToleranceUs;
        int64_t upper_us = now_unix_us + sample->clock_offset_us + kAudioTimestampToleranceUs;
        int candidates;

        new_echo = 1;
        sample->client_echo_recv_mono_us = now_mono_us;
        sample->client_echo_recv_unix_us = now_unix_us;
        sample->echo_packed_ts = info->ts;
        sample->echoed = 1;
        candidates = reconstruct_audio_timestamp_us(info->ts,
                                                    lower_us,
                                                    upper_us,
                                                    &sample->server_recv_unix_us);
        if (candidates == 0) {
            sample->timestamp_decode_status = "invalid_time_window";
            sample->server_recv_unix_us = 0;
        } else if (candidates > 1) {
            sample->timestamp_decode_status = "ambiguous_time_period";
            sample->server_recv_unix_us = 0;
        } else if (sample->server_recv_unix_us < sample->estimated_server_send_us) {
            sample->timestamp_decode_status = "clock_uncertain";
        } else {
            sample->timestamp_decode_status = "ok";
        }
        session->audio.echo_count++;
        session->audio.last_echo_recv_mono_us = now_mono_us;
    }
    if (new_echo && duration_samples > 0) {
        static const uint8_t opus_silence_20ms[] = {0xf8U, 0xffU, 0xfeU};
        uint64_t arrival_start_granule;
        uint64_t missing_samples;
        uint64_t silence_frames;
        uint64_t silence_index;

        if (session->echo_timeline_origin_us == 0) {
            session->echo_timeline_origin_us = now_mono_us;
        }
        arrival_start_granule =
            (uint64_t)(now_mono_us - session->echo_timeline_origin_us) * 48U / 1000U;
        missing_samples = arrival_start_granule > session->echo_last_granule ?
            arrival_start_granule - session->echo_last_granule : 0;
        silence_frames = (missing_samples + 480U) / 960U;
        for (silence_index = 0; silence_index < silence_frames; ++silence_index) {
            session->echo_last_granule += 960U;
            if (session->echo_writer_open && !session->echo_writer_error &&
                ogg_opus_writer_write(&session->echo_writer,
                                      opus_silence_20ms,
                                      sizeof(opus_silence_20ms),
                                      session->echo_last_granule) != 0) {
                session->echo_writer_error = 1;
                break;
            }
        }
        if (!session->echo_writer_open || !session->echo_writer_error) {
            session->echo_last_granule += duration_samples;
            if (session->echo_writer_open && info->media == TIRTC_AUDIO_OPUS &&
                ogg_opus_writer_write(&session->echo_writer,
                                      data,
                                      info->length,
                                      session->echo_last_granule) != 0) {
                session->echo_writer_error = 1;
            }
        }
    }
    pthread_mutex_unlock(&session->mutex);
}

static void handle_video(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info)
{
    probe_session_t *session = active_session_for_conn(hconn);

    if (session == NULL || info == NULL || !session->media_mode) {
        return;
    }
    pthread_mutex_lock(&session->mutex);
    session->media_video_received++;
    pthread_mutex_unlock(&session->mutex);
}

static void *callback_worker_main(void *arg)
{
    (void)arg;

    for (;;) {
        callback_event_t event;
        if (!dequeue_callback_event(&event)) {
            if (atomic_load_explicit(&g_app.callback_worker_stop, memory_order_acquire) &&
                atomic_load_explicit(&g_app.event_read_index, memory_order_acquire) ==
                    atomic_load_explicit(&g_app.event_write_index, memory_order_acquire)) {
                break;
            }
            sleep_for_us(1000LL);
            continue;
        }

        switch (event.type) {
            case CALLBACK_EVENT_SYSTEM:
                handle_event(event.event);
                break;
            case CALLBACK_EVENT_CONNECT_RESULT:
                handle_connect_result(event.session, event.error, event.hconn);
                break;
            case CALLBACK_EVENT_CONN_ERROR:
                handle_conn_error(event.hconn, event.error);
                break;
            case CALLBACK_EVENT_DISCONNECTED:
                handle_disconnected(event.hconn);
                break;
            case CALLBACK_EVENT_COMMAND:
                handle_command(event.hconn, event.cmdw, event.data, event.len);
                break;
            case CALLBACK_EVENT_AUDIO:
                handle_audio(event.hconn, &event.frame_info, event.data);
                break;
            case CALLBACK_EVENT_VIDEO:
                handle_video(event.hconn, &event.frame_info);
                break;
        }
    }
    return NULL;
}

static int callback_worker_start(void)
{
    int rc;

    atomic_store_explicit(&g_app.event_read_index, 0, memory_order_release);
    atomic_store_explicit(&g_app.event_write_index, 0, memory_order_release);
    atomic_store_explicit(&g_app.callback_events_dropped, 0, memory_order_release);
    atomic_store_explicit(&g_app.callback_worker_stop, false, memory_order_release);

    rc = pthread_create(&g_app.callback_worker, NULL, callback_worker_main, NULL);
    if (rc != 0) {
        return -1;
    }
    pthread_mutex_lock(&g_app.mutex);
    g_app.callback_worker_running = 1;
    pthread_mutex_unlock(&g_app.mutex);
    return 0;
}

static void callback_worker_stop(void)
{
    int should_join;
    uint64_t dropped;

    pthread_mutex_lock(&g_app.mutex);
    should_join = g_app.callback_worker_running;
    atomic_store_explicit(&g_app.callback_worker_stop, true, memory_order_release);
    pthread_cond_broadcast(&g_app.cond);
    pthread_mutex_unlock(&g_app.mutex);

    if (should_join) {
        pthread_join(g_app.callback_worker, NULL);
    }

    pthread_mutex_lock(&g_app.mutex);
    g_app.callback_worker_running = 0;
    pthread_mutex_unlock(&g_app.mutex);

    dropped = atomic_load_explicit(&g_app.callback_events_dropped, memory_order_acquire);
    if (dropped > 0) {
        log_message(stderr, "callback events dropped=%" PRIu64, dropped);
    }
}

static void on_event(int event, const void *data, int len)
{
    callback_event_t queued;

    (void)data;
    (void)len;
    memset(&queued, 0, sizeof(queued));
    queued.type = CALLBACK_EVENT_SYSTEM;
    queued.event = event;
    (void)enqueue_callback_event(&queued);
}

static void on_connect_result(int error, tirtc_conn_t hconn, void *user_data)
{
    callback_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = CALLBACK_EVENT_CONNECT_RESULT;
    event.session = (probe_session_t *)user_data;
    event.error = error;
    event.hconn = hconn;
    (void)enqueue_callback_event(&event);
}

static void on_conn_error(tirtc_conn_t hconn, int error)
{
    callback_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = CALLBACK_EVENT_CONN_ERROR;
    event.hconn = hconn;
    event.error = error;
    (void)enqueue_callback_event(&event);
}

static void on_disconnected(tirtc_conn_t hconn)
{
    callback_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = CALLBACK_EVENT_DISCONNECTED;
    event.hconn = hconn;
    (void)enqueue_callback_event(&event);
}

static void on_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len)
{
    callback_event_t event;

    if ((data == NULL && len > 0) || len > CALLBACK_EVENT_DATA_BYTES) {
        atomic_fetch_add_explicit(&g_app.callback_events_dropped, 1, memory_order_relaxed);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.type = CALLBACK_EVENT_COMMAND;
    event.hconn = hconn;
    event.cmdw = cmdw;
    event.len = len;
    if (len > 0) {
        memcpy(event.data, data, len);
    }
    (void)enqueue_callback_event(&event);
}

static void on_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
    callback_event_t event;

    if (info == NULL || data == NULL || info->length == 0 || info->length > CALLBACK_EVENT_DATA_BYTES) {
        atomic_fetch_add_explicit(&g_app.callback_events_dropped, 1, memory_order_relaxed);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.type = CALLBACK_EVENT_AUDIO;
    event.hconn = hconn;
    event.frame_info = *info;
    event.len = info->length;
    memcpy(event.data, data, info->length);
    (void)enqueue_callback_event(&event);
}

static void on_video(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
    callback_event_t event;

    if (info == NULL || data == NULL || info->length == 0) {
        atomic_fetch_add_explicit(&g_app.callback_events_dropped, 1, memory_order_relaxed);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.type = CALLBACK_EVENT_VIDEO;
    event.hconn = hconn;
    event.frame_info = *info;
    (void)enqueue_callback_event(&event);
}

static const TIRTCCALLBACKS g_callbacks = {
    .on_event = on_event,
    .on_conn_error = on_conn_error,
    .on_disconnected = on_disconnected,
    .on_audio = on_audio,
    .on_video = on_video,
    .on_command = on_command,
};

static void session_init(probe_session_t *session)
{
    memset(session, 0, sizeof(*session));
    pthread_mutex_init(&session->mutex, NULL);
    pthread_cond_init(&session->cond, NULL);
}

static void session_destroy(probe_session_t *session)
{
    audio_metrics_free(&session->audio);
    pthread_cond_destroy(&session->cond);
    pthread_mutex_destroy(&session->mutex);
}

static int wait_for_sdk_started(int timeout_ms)
{
    struct timespec deadline;

    if (make_deadline_ms(timeout_ms, &deadline) != 0) {
        return -1;
    }
    pthread_mutex_lock(&g_app.mutex);
    while (!g_app.sdk_started) {
        int rc = pthread_cond_timedwait(&g_app.cond, &g_app.mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&g_app.mutex);
            return -1;
        }
    }
    pthread_mutex_unlock(&g_app.mutex);
    return 0;
}

static int wait_for_sdk_stopped(int timeout_ms)
{
    struct timespec deadline;

    if (make_deadline_ms(timeout_ms, &deadline) != 0) {
        return -1;
    }
    pthread_mutex_lock(&g_app.mutex);
    while (!g_app.sdk_stopped) {
        int rc = pthread_cond_timedwait(&g_app.cond, &g_app.mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&g_app.mutex);
            return -1;
        }
    }
    pthread_mutex_unlock(&g_app.mutex);
    return 0;
}

static int connect_session(const probe_config_t *config, probe_session_t *session)
{
    struct timespec deadline;
    int rc;

    pthread_mutex_lock(&g_app.mutex);
    g_app.active_session = session;
    pthread_mutex_unlock(&g_app.mutex);

    pthread_mutex_lock(&session->mutex);
    session->connect_started_us = monotonic_now_us();
    rc = TiRtcWhipConnect(config->peer_id, config->token, on_connect_result, session);
    if (rc != 0) {
        pthread_mutex_unlock(&session->mutex);
        return rc;
    }
    if (make_deadline_ms(config->connect_timeout_ms, &deadline) != 0) {
        pthread_mutex_unlock(&session->mutex);
        return -1;
    }
    while (!session->connect_done) {
        rc = pthread_cond_timedwait(&session->cond, &session->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&session->mutex);
            return TIRTC_E_TIMEOUTED;
        }
    }
    rc = session->connect_error;
    pthread_mutex_unlock(&session->mutex);
    return rc;
}

static void disconnect_session(probe_session_t *session)
{
    struct timespec deadline;
    tirtc_conn_t hconn;
    int should_disconnect;

    pthread_mutex_lock(&session->mutex);
    hconn = session->hconn;
    should_disconnect = hconn != NULL && !session->disconnected;
    pthread_mutex_unlock(&session->mutex);

    if (should_disconnect) {
        (void)TiRtcDisconnect(hconn);
    }

    pthread_mutex_lock(&session->mutex);
    if (should_disconnect && !session->disconnected) {
        if (make_deadline_ms(DISCONNECT_TIMEOUT_MS, &deadline) == 0) {
            while (!session->disconnected) {
                int rc = pthread_cond_timedwait(&session->cond, &session->mutex, &deadline);
                if (rc == ETIMEDOUT) {
                    break;
                }
            }
        }
    }
    pthread_mutex_unlock(&session->mutex);

    pthread_mutex_lock(&g_app.mutex);
    if (g_app.active_session == session) {
        g_app.active_session = NULL;
    }
    pthread_mutex_unlock(&g_app.mutex);
}

static int send_timesync_request(probe_session_t *session, uint32_t seq, int timeout_ms, time_sync_sample_t *out)
{
    char payload[160];
    int64_t client_send_unix_ns;
    struct timespec deadline;
    int payload_len;
    int send_ret;

    pthread_mutex_lock(&session->mutex);
    session->timesync_done = 0;
    session->timesync_error = 0;
    memset(&session->timesync_sample, 0, sizeof(session->timesync_sample));
    session->waiting_timesync_seq = seq;
    client_send_unix_ns = realtime_now_ns();
    session->waiting_timesync_client_send_ns = client_send_unix_ns;
    payload_len = snprintf(payload,
                           sizeof(payload),
                           "{\"version\":1,\"seq\":%" PRIu32 ",\"client_send_unix_ns\":%" PRId64 "}",
                           seq,
                           client_send_unix_ns);
    if (payload_len < 0 || (size_t)payload_len >= sizeof(payload)) {
        pthread_mutex_unlock(&session->mutex);
        return -1;
    }
    send_ret = TiRtcSendCommand(session->hconn,
                                CMD_TIME_SYNC_REQUEST,
                                payload,
                                (uint32_t)payload_len);
    if (send_ret < 0) {
        pthread_mutex_unlock(&session->mutex);
        return send_ret;
    }
    if (make_deadline_ms(timeout_ms, &deadline) != 0) {
        pthread_mutex_unlock(&session->mutex);
        return -1;
    }
    while (!session->timesync_done && !session->disconnected) {
        int rc = pthread_cond_timedwait(&session->cond, &session->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&session->mutex);
            return TIRTC_E_TIMEOUTED;
        }
    }
    if (!session->timesync_sample.ok) {
        pthread_mutex_unlock(&session->mutex);
        return session->conn_error != 0 ? session->conn_error : TIRTC_E_CONN_OTHER_ERROR;
    }
    *out = session->timesync_sample;
    pthread_mutex_unlock(&session->mutex);
    return 0;
}

static int choose_timesync_offset(const time_sync_sample_t *samples, size_t count, int64_t *out_offset_ns)
{
    time_sync_sample_t *copy;
    sample_set_t offsets = {0};
    size_t usable;
    size_t i;

    if (count == 0) {
        return -1;
    }
    copy = (time_sync_sample_t *)malloc(count * sizeof(*copy));
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, samples, count * sizeof(*copy));
    qsort(copy, count, sizeof(*copy), compare_timesync_by_rtt);
    usable = count < 5U ? count : (count + 1U) / 2U;
    for (i = 0; i < usable; ++i) {
        if (sample_set_push(&offsets, copy[i].offset_ns) != 0) {
            free(copy);
            sample_set_free(&offsets);
            return -1;
        }
    }
    qsort(offsets.values, offsets.len, sizeof(*offsets.values), compare_i64);
    *out_offset_ns = percentile_sorted(&offsets, 0.50);
    free(copy);
    sample_set_free(&offsets);
    return 0;
}

static int run_timesync_on_session(probe_session_t *session,
                                   const probe_config_t *config,
                                   time_sync_sample_t **out_samples,
                                   size_t *out_count,
                                   int64_t *out_offset_ns)
{
    time_sync_sample_t *samples = NULL;
    size_t count = 0;
    int i;

    samples = (time_sync_sample_t *)calloc((size_t)config->repeat, sizeof(*samples));
    if (samples == NULL) {
        return -1;
    }
    for (i = 0; i < config->repeat && !g_should_exit; ++i) {
        time_sync_sample_t sample;
        int rc = send_timesync_request(session, (uint32_t)(i + 1), config->timeout_ms, &sample);
        if (rc == 0) {
            samples[count++] = sample;
        } else {
            log_message(stderr, "timesync sample %d failed: %s", i + 1, TiRtcGetErrorStr(rc));
        }
        if (i + 1 < config->repeat) {
            sleep_for_us((int64_t)config->interval_ms * 1000LL);
        }
    }
    if (choose_timesync_offset(samples, count, out_offset_ns) != 0) {
        free(samples);
        return -1;
    }
    *out_samples = samples;
    *out_count = count;
    return 0;
}

static void print_timesync_summary(const time_sync_sample_t *samples, size_t count, int64_t offset_ns)
{
    sample_set_t rtt_us = {0};
    sample_set_t offset_us = {0};
    sample_set_t one_way_us = {0};
    size_t i;

    for (i = 0; i < count; ++i) {
        (void)sample_set_push(&rtt_us, samples[i].rtt_us);
        (void)sample_set_push(&offset_us, samples[i].offset_ns / 1000LL);
        (void)sample_set_push(&one_way_us, samples[i].rtt_us / 2LL);
    }
    printf("timesync: samples=%zu selected_offset=%.2fms\n", count, (double)offset_ns / 1000000.0);
    print_duration_summary_ms("rtt", &rtt_us);
    print_duration_summary_ms("offset", &offset_us);
    print_duration_summary_ms("device_to_server_estimated", &one_way_us);
    sample_set_free(&rtt_us);
    sample_set_free(&offset_us);
    sample_set_free(&one_way_us);
}

static int run_connect_command(const probe_config_t *config)
{
    sample_set_t connect_us = {0};
    int success = 0;
    int completed = 0;
    int i;

    for (i = 0; i < config->iterations && !g_should_exit; ++i) {
        probe_session_t session;
        int rc;
        int64_t cost_us;

        session_init(&session);
        rc = connect_session(config, &session);
        pthread_mutex_lock(&session.mutex);
        cost_us = session.connect_cost_us;
        pthread_mutex_unlock(&session.mutex);
        if (rc == 0) {
            success++;
            (void)sample_set_push(&connect_us, cost_us);
            log_message(stdout, "connect iteration %d succeeded in %.2fms", i + 1, us_to_ms(cost_us));
        } else {
            log_message(stderr, "connect iteration %d failed: %s", i + 1, TiRtcGetErrorStr(rc));
        }
        disconnect_session(&session);
        session_destroy(&session);
        completed++;
    }

    printf("connect_success: %d/%d %.2f%%\n",
           success,
           config->iterations,
           config->iterations == 0 ? 0.0 : (double)success * 100.0 / (double)config->iterations);
    print_duration_summary_ms("connect_cost", &connect_us);
    sample_set_free(&connect_us);
    return completed == config->iterations ? 0 : 1;
}

static int idle_session_is_connected(probe_session_t *session)
{
    int connected;

    pthread_mutex_lock(&session->mutex);
    connected = session->connect_done && session->connect_error == 0 &&
                session->hconn != NULL && !session->disconnected;
    pthread_mutex_unlock(&session->mutex);
    return connected;
}

static int count_idle_connections(probe_session_t *sessions, int count)
{
    int connected = 0;
    int i;

    for (i = 0; i < count; ++i) {
        if (idle_session_is_connected(&sessions[i])) {
            connected++;
        }
    }
    return connected;
}

static int run_idle_command(const probe_config_t *config)
{
    probe_session_t *sessions;
    sample_set_t connect_us = {0};
    int initialized = 0;
    int succeeded = 0;
    int peak = 0;
    int active_end;
    int unexpected_disconnected;
    int64_t hold_started_us;
    int64_t next_status_us;
    int i;

    sessions = (probe_session_t *)calloc((size_t)config->connections, sizeof(*sessions));
    if (sessions == NULL) {
        log_message(stderr, "failed to allocate %d idle sessions", config->connections);
        return 1;
    }

    log_message(stdout,
                "idle ramp started: target_connections=%d interval_ms=%d hold_duration_sec=%" PRId64,
                config->connections,
                config->interval_ms,
                config->duration_sec);
    for (i = 0; i < config->connections && !g_should_exit; ++i) {
        int rc;
        int64_t cost_us;

        session_init(&sessions[i]);
        sessions[i].idle_only = 1;
        initialized++;
        rc = connect_session(config, &sessions[i]);
        pthread_mutex_lock(&sessions[i].mutex);
        cost_us = sessions[i].connect_cost_us;
        pthread_mutex_unlock(&sessions[i].mutex);
        if (rc == 0) {
            int active;

            succeeded++;
            (void)sample_set_push(&connect_us, cost_us);
            active = count_idle_connections(sessions, initialized);
            if (active > peak) {
                peak = active;
            }
            log_message(stdout,
                        "idle connection %d/%d established in %.2fms active=%d",
                        i + 1,
                        config->connections,
                        us_to_ms(cost_us),
                        active);
        } else {
            log_message(stderr,
                        "idle connection %d/%d failed: rc=%d error=%s",
                        i + 1,
                        config->connections,
                        rc,
                        TiRtcGetErrorStr(rc));
            disconnect_session(&sessions[i]);
        }
        if (i + 1 < config->connections && !g_should_exit) {
            sleep_for_us((int64_t)config->interval_ms * 1000LL);
        }
    }

    hold_started_us = monotonic_now_us();
    next_status_us = hold_started_us;
    while (!g_should_exit &&
           (monotonic_now_us() - hold_started_us) / 1000000LL < config->duration_sec) {
        int64_t now_us = monotonic_now_us();
        if (now_us >= next_status_us) {
            int active = count_idle_connections(sessions, initialized);
            if (active > peak) {
                peak = active;
            }
            log_message(stdout,
                        "idle hold: elapsed_ms=%lld active=%d/%d",
                        (long long)((now_us - hold_started_us) / 1000LL),
                        active,
                        config->connections);
            next_status_us = now_us + 1000000LL;
        }
        sleep_for_us(100000LL);
    }

    active_end = count_idle_connections(sessions, initialized);
    unexpected_disconnected = succeeded > active_end ? succeeded - active_end : 0;
    printf("idle_connections: established=%d/%d peak=%d active_end=%d unexpected_disconnected=%d\n",
           succeeded,
           config->connections,
           peak,
           active_end,
           unexpected_disconnected);
    print_duration_summary_ms("idle_connect_cost", &connect_us);

    for (i = initialized - 1; i >= 0; --i) {
        disconnect_session(&sessions[i]);
        session_destroy(&sessions[i]);
    }
    sample_set_free(&connect_us);
    free(sessions);

    return !g_should_exit && succeeded == config->connections && active_end == succeeded ? 0 : 1;
}

static int run_timesync_command(const probe_config_t *config)
{
    probe_session_t session;
    time_sync_sample_t *samples = NULL;
    size_t count = 0;
    int64_t offset_ns = 0;
    int rc;

    session_init(&session);
    rc = connect_session(config, &session);
    if (rc != 0) {
        log_message(stderr, "connect failed: %s", TiRtcGetErrorStr(rc));
        session_destroy(&session);
        return 1;
    }
    rc = run_timesync_on_session(&session, config, &samples, &count, &offset_ns);
    if (rc == 0) {
        print_timesync_summary(samples, count, offset_ns);
    } else {
        log_message(stderr, "timesync failed");
    }
    free(samples);
    disconnect_session(&session);
    session_destroy(&session);
    return rc == 0 ? 0 : 1;
}

static void init_audio_tone_samples(void)
{
    size_t i;

    if (g_audio_tone_samples_initialized) {
        return;
    }
    for (i = 0; i < AUDIO_SAMPLE_RATE_HZ; ++i) {
        double phase = kTwoPi * kAudioToneHz * (double)i / (double)AUDIO_SAMPLE_RATE_HZ;
        g_audio_tone_samples[i] = (int16_t)(sin(phase) * 6000.0);
    }
    g_audio_tone_samples_initialized = 1;
}

static void make_audio_payload(uint8_t *payload, uint32_t frame_index)
{
    size_t i;
    size_t sample_count = AUDIO_PAYLOAD_BYTES / AUDIO_BYTES_PER_SAMPLE;
    size_t base_sample = ((uint64_t)(frame_index - 1U) * sample_count) % AUDIO_SAMPLE_RATE_HZ;

    for (i = 0; i < sample_count; ++i) {
        int16_t sample = g_audio_tone_samples[(base_sample + i) % AUDIO_SAMPLE_RATE_HZ];
        write_le16_i16(payload + i * AUDIO_BYTES_PER_SAMPLE, sample);
    }
}

static void write_audio_sample_log(FILE *stream,
                                   int success_iteration,
                                   const audio_metrics_t *audio)
{
    size_t i;

    if (stream == NULL) {
        return;
    }
    for (i = 0; i < audio->len; ++i) {
        const audio_sample_t *sample = &audio->items[i];
        fprintf(stream,
                "%d,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%d,%" PRId64 ",%" PRId64
                ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRIu32 ",%d,%" PRIu32
                ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%s,%d\n",
                success_iteration,
                sample->send_index,
                sample->frame_index,
                sample->frame_duration_us,
                sample->send_ret,
                sample->send_late_us,
                sample->client_send_unix_us,
                sample->client_send_mono_us,
                sample->estimated_server_send_us,
                sample->clock_offset_us,
                sample->time_sync_rtt_us,
                sample->outbound_packed_ts,
                sample->echoed,
                sample->echo_packed_ts,
                sample->server_recv_unix_us,
                sample->client_echo_recv_unix_us,
                sample->client_echo_recv_mono_us,
                audio->call_finished_mono_us - audio->call_started_mono_us,
                sample->timestamp_decode_status,
                sample->duplicate_echo_count);
    }
    fflush(stream);
}

static int run_audio_iteration(probe_session_t *session,
                               const probe_config_t *config,
                               const ogg_opus_file_t *input_opus,
                               int64_t offset_ns,
                               int64_t time_sync_rtt_us,
                               int success_iteration,
                               int target_success_iterations,
                               FILE *sample_log,
                               uint64_t *total_sent,
                               uint64_t *total_send_failed,
                               uint64_t *total_echo_received)
{
    int64_t start_us;
    int64_t next_send_us;
    int64_t loop_cpu_start_ns;
    int64_t loop_cpu_ns;
    int64_t prepare_cpu_ns = 0;
    int64_t payload_cpu_ns = 0;
    int64_t sdk_send_cpu_ns = 0;
    int64_t post_send_cpu_ns = 0;
    size_t input_packet_index = 0;
    char *echo_output_path = NULL;
    uint8_t opus_audio_flags = 0;

    if (target_success_iterations > 1) {
        printf("音频测试轮次: 第 %d/%d 次\n", success_iteration, target_success_iterations);
    }

    if (input_opus == NULL) {
        init_audio_tone_samples();
    } else {
        int sample_rate_flag = input_opus->input_sample_rate == 16000U ? 1 : 0;
        int channel_flag = input_opus->channels == 2U ? 2 : 0;
        opus_audio_flags = (uint8_t)(sample_rate_flag + channel_flag);
    }
    if (config->audio_echo_output != NULL && config->audio_echo_output[0] != '\0') {
        if (target_success_iterations == 1) {
            echo_output_path = strdup(config->audio_echo_output);
        } else {
            const char *dot = strrchr(config->audio_echo_output, '.');
            size_t prefix_len = dot == NULL ? strlen(config->audio_echo_output) :
                (size_t)(dot - config->audio_echo_output);
            const char *suffix = dot == NULL ? ".opus" : dot;
            size_t needed = prefix_len + strlen(suffix) + 32U;
            echo_output_path = (char *)malloc(needed);
            if (echo_output_path != NULL) {
                (void)snprintf(echo_output_path,
                               needed,
                               "%.*s.iteration-%d%s",
                               (int)prefix_len,
                               config->audio_echo_output,
                               success_iteration,
                               suffix);
            }
        }
        if (echo_output_path == NULL ||
            ogg_opus_writer_open(&session->echo_writer,
                                 echo_output_path,
                                 input_opus == NULL ? 1U : input_opus->channels,
                                 input_opus == NULL ? 48000U : input_opus->input_sample_rate) != 0) {
            log_message(stderr,
                        "failed to open echo audio output %s: %s",
                        echo_output_path == NULL ? config->audio_echo_output : echo_output_path,
                        strerror(errno));
            free(echo_output_path);
            return 1;
        }
        session->echo_writer_open = 1;
        log_message(stdout, "echo audio output: %s", echo_output_path);
    }
    start_us = monotonic_now_us();
    next_send_us = start_us;
    pthread_mutex_lock(&session->mutex);
    audio_metrics_reset_iteration(&session->audio);
    session->echo_timeline_origin_us = 0;
    session->echo_last_granule = 0;
    session->audio.call_started_mono_us = start_us;
    session->audio.expected_frame_ms = input_opus == NULL ? config->frame_ms :
        (int)(input_opus->packets[0].duration_samples_48k / 48U);
    pthread_mutex_unlock(&session->mutex);

    loop_cpu_start_ns = thread_cpu_now_ns();
    while (!g_should_exit &&
           (input_opus == NULL || input_packet_index < input_opus->len) &&
           (!config->duration_explicit ||
            (monotonic_now_us() - start_us) / 1000000LL < config->duration_sec) &&
           (input_opus != NULL ||
            (monotonic_now_us() - start_us) / 1000000LL < config->duration_sec)) {
        int64_t now_us = monotonic_now_us();
        if (now_us >= next_send_us) {
            uint8_t tone_payload[AUDIO_PAYLOAD_BYTES];
            const uint8_t *payload;
            uint32_t payload_len;
            uint32_t packet_duration_samples;
            int packet_duration_ms;
            TIRTCFRAMEINFO info;
            uint32_t outbound_packed_ts;
            uint32_t frame_index;
            size_t sample_index;
            int64_t segment_cpu_start_ns = thread_cpu_now_ns();
            int64_t send_unix_us = realtime_now_ns() / 1000LL;
            int64_t send_mono_us = monotonic_now_us();
            int64_t clock_offset_us = offset_ns / 1000LL;
            int64_t estimated_server_send_us = send_unix_us + clock_offset_us;
            int64_t send_late_us = now_us - next_send_us;
            int send_ret;

            pthread_mutex_lock(&session->mutex);
            frame_index = ++session->audio.next_frame_index;
            if (input_opus != NULL) {
                const ogg_opus_packet_t *packet = &input_opus->packets[input_packet_index];
                packet_duration_samples = packet->duration_samples_48k;
                packet_duration_ms = (int)(packet_duration_samples / 48U);
                payload = packet->data;
                payload_len = (uint32_t)packet->len;
            } else {
                packet_duration_ms = config->frame_ms;
                packet_duration_samples = (uint32_t)packet_duration_ms * 48U;
                payload = tone_payload;
                payload_len = AUDIO_PAYLOAD_BYTES;
            }
            outbound_packed_ts = pack_audio_timestamp(estimated_server_send_us, frame_index);
            if (session->audio.first_client_send_unix_us == 0) {
                session->audio.first_client_send_unix_us = send_unix_us;
            }
            if (send_late_us > session->audio.max_send_late_us) {
                session->audio.max_send_late_us = send_late_us;
            }
            if (send_late_us > (int64_t)packet_duration_ms * 1000LL) {
                session->audio.late_send_count++;
            }
            if (audio_metrics_append(&session->audio,
                                     frame_index,
                                     frame_index,
                                     (uint32_t)packet_duration_ms * 1000U,
                                     outbound_packed_ts,
                                     send_unix_us,
                                     send_mono_us,
                                     estimated_server_send_us,
                                     clock_offset_us,
                                     time_sync_rtt_us,
                                     send_late_us) == NULL) {
                pthread_mutex_unlock(&session->mutex);
                log_message(stderr, "failed to record audio sample");
                break;
            }
            sample_index = session->audio.len - 1U;
            session->audio.send_count++;
            pthread_mutex_unlock(&session->mutex);
            prepare_cpu_ns += thread_cpu_now_ns() - segment_cpu_start_ns;

            segment_cpu_start_ns = thread_cpu_now_ns();
            if (input_opus == NULL) {
                make_audio_payload(tone_payload, frame_index);
            }
            memset(&info, 0, sizeof(info));
            info.stream_id = AUDIO_STREAM_ID;
            info.media = input_opus == NULL ? TIRTC_AUDIO_PCM : TIRTC_AUDIO_OPUS;
            info.flags = input_opus == NULL ? TIRTC_AUDIOSAMPLE_8K16B1C : opus_audio_flags;
            info.ts = outbound_packed_ts;
            info.length = payload_len;
            payload_cpu_ns += thread_cpu_now_ns() - segment_cpu_start_ns;

            segment_cpu_start_ns = thread_cpu_now_ns();
            send_ret = TiRtcSendAudioStream(session->hconn, &info, payload);
            sdk_send_cpu_ns += thread_cpu_now_ns() - segment_cpu_start_ns;

            segment_cpu_start_ns = thread_cpu_now_ns();
            pthread_mutex_lock(&session->mutex);
            session->audio.items[sample_index].send_ret = send_ret;
            if (send_ret < 0) {
                session->audio.send_failed++;
            }
            pthread_mutex_unlock(&session->mutex);
            next_send_us += (int64_t)packet_duration_samples * 1000000LL / 48000LL;
            if (input_opus != NULL) {
                input_packet_index++;
            }
            post_send_cpu_ns += thread_cpu_now_ns() - segment_cpu_start_ns;
        } else {
            sleep_until_monotonic_us(next_send_us);
        }
    }
    loop_cpu_ns = thread_cpu_now_ns() - loop_cpu_start_ns;
    {
        int64_t wait_started_us = monotonic_now_us();
        for (;;) {
            uint64_t echo_count;
            int64_t last_echo_us;
            int64_t now_us = monotonic_now_us();

            pthread_mutex_lock(&session->mutex);
            echo_count = session->audio.echo_count;
            last_echo_us = session->audio.last_echo_recv_mono_us;
            pthread_mutex_unlock(&session->mutex);
            if (echo_count > 0 &&
                now_us - last_echo_us >= AUDIO_ECHO_IDLE_GRACE_MS * 1000LL) {
                break;
            }
            if (now_us - wait_started_us >= AUDIO_ECHO_WAIT_TIMEOUT_SEC * 1000000LL) {
                break;
            }
            sleep_for_us(10000LL);
        }
    }

    pthread_mutex_lock(&session->mutex);
    if (session->echo_writer_open) {
        if (ogg_opus_writer_close(&session->echo_writer) != 0 || session->echo_writer_error) {
            log_message(stderr, "failed writing echo audio output: %s", echo_output_path);
        }
        session->echo_writer_open = 0;
    }
    pthread_mutex_unlock(&session->mutex);
    free(echo_output_path);

    pthread_mutex_lock(&session->mutex);
    session->audio.call_finished_mono_us = monotonic_now_us();
    write_audio_sample_log(sample_log, success_iteration, &session->audio);
    printf("音频原始事件: 设备发送=%" PRIu64 " 发送失败=%" PRIu64 " 设备收到回声=%" PRIu64 "\n",
           session->audio.send_count, session->audio.send_failed, session->audio.echo_count);
    printf("音频发送调度: 最大延迟=%.2fms 超过一帧间隔次数=%" PRIu64 "\n",
           us_to_ms(session->audio.max_send_late_us),
           session->audio.late_send_count);
    {
        int64_t classified_cpu_ns = prepare_cpu_ns + payload_cpu_ns + sdk_send_cpu_ns + post_send_cpu_ns;
        int64_t other_cpu_ns = loop_cpu_ns > classified_cpu_ns ? loop_cpu_ns - classified_cpu_ns : 0;
        double total = loop_cpu_ns > 0 ? (double)loop_cpu_ns : 1.0;
        printf("音频主线程CPU: 总计=%.3fms 准备/统计=%.3fms(%.1f%%) payload=%.3fms(%.1f%%) "
               "SDK发送=%.3fms(%.1f%%) 发送后记账=%.3fms(%.1f%%) 其他循环=%.3fms(%.1f%%)\n",
               (double)loop_cpu_ns / 1000000.0,
               (double)prepare_cpu_ns / 1000000.0, (double)prepare_cpu_ns * 100.0 / total,
               (double)payload_cpu_ns / 1000000.0, (double)payload_cpu_ns * 100.0 / total,
               (double)sdk_send_cpu_ns / 1000000.0, (double)sdk_send_cpu_ns * 100.0 / total,
               (double)post_send_cpu_ns / 1000000.0, (double)post_send_cpu_ns * 100.0 / total,
               (double)other_cpu_ns / 1000000.0, (double)other_cpu_ns * 100.0 / total);
    }
    *total_sent += session->audio.send_count;
    *total_send_failed += session->audio.send_failed;
    *total_echo_received += session->audio.echo_count;
    pthread_mutex_unlock(&session->mutex);
    return 0;
}

static void print_audio_cumulative_summary(int success,
                                           int target_success,
                                           uint64_t total_sent,
                                           uint64_t total_send_failed,
                                           uint64_t total_echo_received)
{
    printf("音频多轮汇总: 成功轮次=%d/%d\n", success, target_success);
    printf("音频原始事件多轮汇总: 设备发送=%" PRIu64 " 发送失败=%" PRIu64
           " 设备收到回声=%" PRIu64 "，业务指标请使用CSV生成报告\n",
           total_sent, total_send_failed, total_echo_received);
}

static int run_media_command(const probe_config_t *config)
{
    probe_session_t session;
    FILE *audio_output = NULL;
    uint64_t audio_sent = 0;
    uint64_t video_sent = 0;
    uint64_t audio_received;
    uint64_t video_received;
    uint64_t audio_output_bytes;
    int audio_output_error;
    uint32_t audio_index = 0;
    int64_t started_us;
    int64_t next_audio_us;
    int64_t next_video_us;
    int rc;

    session_init(&session);
    session.media_mode = 1;
    if (config->audio_output != NULL) {
        audio_output = fopen(config->audio_output, "wb");
        if (audio_output == NULL) {
            log_message(stderr,
                        "failed to open audio output %s: %s",
                        config->audio_output,
                        strerror(errno));
            session_destroy(&session);
            return 1;
        }
        session.media_audio_output = audio_output;
    }
    rc = connect_session(config, &session);
    if (rc != 0) {
        log_message(stderr, "media connection failed: rc=%d error=%s",
                    rc, TiRtcGetErrorStr(rc));
        disconnect_session(&session);
        if (audio_output != NULL) {
            fclose(audio_output);
        }
        session_destroy(&session);
        return 1;
    }

    printf("媒体测试源: 音频=内置 440 Hz PCM，视频=内置 JPEG 测试帧\n");
    started_us = monotonic_now_us();
    next_audio_us = started_us;
    next_video_us = started_us;
    while (!g_should_exit &&
           monotonic_now_us() - started_us < config->duration_sec * 1000000LL) {
        int64_t now_us = monotonic_now_us();

        if (now_us >= next_audio_us) {
            uint8_t payload[AUDIO_PAYLOAD_BYTES];
            TIRTCFRAMEINFO info;

            audio_index++;
            make_audio_payload(payload, audio_index);
            memset(&info, 0, sizeof(info));
            info.stream_id = AUDIO_STREAM_ID;
            info.media = TIRTC_AUDIO_PCM;
            info.flags = TIRTC_AUDIOSAMPLE_8K16B1C;
            info.ts = (uint32_t)(now_us / 1000LL);
            info.length = sizeof(payload);
            if (TiRtcSendAudioStream(session.hconn, &info, payload) >= 0) {
                audio_sent++;
            }
            next_audio_us += DEFAULT_FRAME_MS * 1000LL;
        }
        if (now_us >= next_video_us) {
            TIRTCFRAMEINFO info;

            memset(&info, 0, sizeof(info));
            info.stream_id = VIDEO_STREAM_ID;
            info.media = TIRTC_VIDEO_JPEG;
            info.flags = TIRTC_FRAME_FLAG_KEY_FRAME;
            info.ts = (uint32_t)(now_us / 1000LL);
            info.length = sizeof(kMediaSampleJpeg);
            if (TiRtcSendVideoStream(session.hconn, &info, kMediaSampleJpeg) >= 0) {
                video_sent++;
            }
            next_video_us += 1000000LL;
        }
        sleep_for_us(1000LL);
    }

    /* Allow the final media frames to arrive before closing the connection. */
    sleep_for_us(500000LL);
    disconnect_session(&session);
    pthread_mutex_lock(&session.mutex);
    audio_received = session.media_audio_received;
    video_received = session.media_video_received;
    audio_output_bytes = session.media_audio_output_bytes;
    audio_output_error = session.media_audio_output_error;
    pthread_mutex_unlock(&session.mutex);
    if (audio_output != NULL && fclose(audio_output) != 0) {
        log_message(stderr,
                    "failed to close audio output %s: %s",
                    config->audio_output,
                    strerror(errno));
        audio_output_error = 1;
    }
    session_destroy(&session);

    printf("媒体联调结果: 音频发送=%" PRIu64 " 接收=%" PRIu64
           "，视频发送=%" PRIu64 " 接收=%" PRIu64 "\n",
           audio_sent, audio_received, video_sent, video_received);
    if (config->audio_output != NULL) {
        printf("接收音频已保存: %s，格式=PCM 8kHz/16bit/单声道，字节=%" PRIu64 "\n",
               config->audio_output,
               audio_output_bytes);
    }
    if (audio_sent == 0 || video_sent == 0 ||
        audio_received == 0 || video_received == 0 || audio_output_error) {
        log_message(stderr, "media validation failed: expected sent and received audio/video");
        return 1;
    }
    printf("媒体联调通过\n");
    return 0;
}

static int run_audio_command(const probe_config_t *config)
{
    probe_session_t sync_session;
    probe_config_t sync_config = *config;
    time_sync_sample_t *sync_samples = NULL;
    size_t sync_count = 0;
    int64_t offset_ns = 0;
    int64_t time_sync_rtt_us = 0;
    uint64_t total_sent = 0;
    uint64_t total_send_failed = 0;
    uint64_t total_echo_received = 0;
    int success = 0;
    int attempts = 0;
    int sync_connect_attempt;
    int rc = TIRTC_E_CONN_OTHER_ERROR;
    FILE *sample_log = NULL;
    ogg_opus_file_t input_opus = {0};
    const ogg_opus_file_t *input_opus_ptr = NULL;
    char opus_error[256];
    size_t packet_index;

    if (config->audio_input != NULL && config->audio_input[0] != '\0') {
        if (ogg_opus_read_file(config->audio_input,
                               &input_opus,
                               opus_error,
                               sizeof(opus_error)) != 0) {
            log_message(stderr, "failed to read Opus input: %s", opus_error);
            return 1;
        }
        for (packet_index = 0; packet_index < input_opus.len; ++packet_index) {
            if (input_opus.packets[packet_index].len > CALLBACK_EVENT_DATA_BYTES ||
                input_opus.packets[packet_index].len > UINT32_MAX) {
                log_message(stderr,
                            "Opus packet %zu is too large: %zu bytes",
                            packet_index + 1U,
                            input_opus.packets[packet_index].len);
                ogg_opus_file_free(&input_opus);
                return 1;
            }
        }
        input_opus_ptr = &input_opus;
        if (input_opus.input_sample_rate != 8000U && input_opus.input_sample_rate != 16000U) {
            log_message(stderr,
                        "unsupported Opus input sample rate: %u (expected 8000 or 16000)",
                        input_opus.input_sample_rate);
            ogg_opus_file_free(&input_opus);
            return 1;
        }
        log_message(stdout,
                    "Opus input: %s packets=%zu channels=%u input_sample_rate=%u",
                    config->audio_input,
                    input_opus.len,
                    input_opus.channels,
                    input_opus.input_sample_rate);
    }

    if (config->audio_sample_log != NULL && config->audio_sample_log[0] != '\0') {
        sample_log = fopen(config->audio_sample_log, "w");
        if (sample_log == NULL) {
            log_message(stderr,
                        "failed to open audio sample log %s: %s",
                        config->audio_sample_log,
                        strerror(errno));
            ogg_opus_file_free(&input_opus);
            return 1;
        }
        fprintf(sample_log,
                "iteration,send_index,frame_index,frame_duration_us,send_ret,send_late_us,"
                "client_send_unix_us,client_send_monotonic_us,estimated_server_send_unix_us,"
                "clock_offset_us,time_sync_rtt_us,outbound_packed_ts,echo_received,echo_packed_ts,"
                "server_receive_unix_us,client_echo_recv_unix_us,client_echo_recv_monotonic_us,"
                "call_duration_us,timestamp_decode_status,duplicate_echo_count\n");
    }

    for (sync_connect_attempt = 1;
         sync_connect_attempt <= config->start_retries && !g_should_exit;
         ++sync_connect_attempt) {
        session_init(&sync_session);
        rc = connect_session(config, &sync_session);
        if (rc == 0) {
            log_message(stdout,
                        "pre-audio connect succeeded after attempts=%d",
                        sync_connect_attempt);
            break;
        }
        log_message(stderr,
                    "pre-audio connect attempt %d/%d failed: rc=%d error=%s",
                    sync_connect_attempt,
                    config->start_retries,
                    rc,
                    TiRtcGetErrorStr(rc));
        disconnect_session(&sync_session);
        session_destroy(&sync_session);
        if (sync_connect_attempt < config->start_retries && !g_should_exit) {
            sleep_for_us(100000LL);
        }
    }
    if (rc != 0) {
        log_message(stderr,
                    "pre-audio connect failed after attempts=%d: rc=%d error=%s",
                    sync_connect_attempt - 1,
                    rc,
                    TiRtcGetErrorStr(rc));
        if (sample_log != NULL) {
            fclose(sample_log);
        }
        ogg_opus_file_free(&input_opus);
        return 1;
    }

    sync_config.repeat = config->repeat;
    rc = run_timesync_on_session(&sync_session, &sync_config, &sync_samples, &sync_count, &offset_ns);
    disconnect_session(&sync_session);
    session_destroy(&sync_session);
    if (rc != 0) {
        log_message(stderr, "pre-audio timesync failed");
        if (sample_log != NULL) {
            fclose(sample_log);
        }
        ogg_opus_file_free(&input_opus);
        return 1;
    }
    print_timesync_summary(sync_samples, sync_count, offset_ns);
    if (sync_count > 0) {
        size_t sync_index;
        time_sync_rtt_us = sync_samples[0].rtt_us;
        for (sync_index = 1; sync_index < sync_count; ++sync_index) {
            if (sync_samples[sync_index].rtt_us < time_sync_rtt_us) {
                time_sync_rtt_us = sync_samples[sync_index].rtt_us;
            }
        }
    }
    free(sync_samples);

    while (success < config->audio_iterations && !g_should_exit) {
        probe_session_t session;

        attempts++;
        session_init(&session);
        rc = connect_session(config, &session);
        if (rc == 0) {
            rc = run_audio_iteration(&session,
                                     config,
                                     input_opus_ptr,
                                     offset_ns,
                                     time_sync_rtt_us,
                                     success + 1,
                                     config->audio_iterations,
                                     sample_log,
                                     &total_sent,
                                     &total_send_failed,
                                     &total_echo_received);
        } else {
            log_message(stderr, "connect failed: %s", TiRtcGetErrorStr(rc));
        }
        disconnect_session(&session);
        session_destroy(&session);
        if (rc == 0) {
            success++;
            print_audio_cumulative_summary(success,
                                           config->audio_iterations,
                                           total_sent,
                                           total_send_failed,
                                           total_echo_received);
        } else {
            log_message(stderr,
                        "audio attempt %d failed, successful_iterations=%d/%d",
                        attempts,
                        success,
                        config->audio_iterations);
        }
    }

    if (sample_log != NULL) {
        fclose(sample_log);
    }
    ogg_opus_file_free(&input_opus);
    return success == config->audio_iterations ? 0 : 1;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s connect  --endpoint <url> --device-id <id> --device-secret-key <key> --peer-id <whips://...> --token <token> [--client-mode] [--iterations <n>] [--connect-timeout-ms <ms>] [--start-retries <n>] [--log-level <1-5|11+>]\n"
            "  %s idle     --endpoint <url> --device-id <id> --device-secret-key <key> --peer-id <whips://...> --token <token> --connections <n> [--client-mode] [--duration-sec <seconds>] [--interval-ms <ms>] [--connect-timeout-ms <ms>] [--log-level <1-5|11+>]\n"
            "  %s timesync --endpoint <url> --device-id <id> --device-secret-key <key> --peer-id <whips://...> --token <token> [--client-mode] [--repeat <n>] [--interval-ms <ms>] [--timeout-ms <ms>] [--log-level <1-5|11+>]\n"
            "  %s media    --endpoint <url> --device-id <id> --device-secret-key <key> --peer-id <whips://...> --token <token> [--client-mode] [--audio-output <pcm-path>] [--duration-sec <seconds>] [--connect-timeout-ms <ms>] [--log-level <1-5|11+>]\n"
            "  %s audio    --endpoint <url> --device-id <id> --device-secret-key <key> --peer-id <whips://...> --token <token> [--client-mode] [--audio-input <ogg-opus-path> --audio-echo-output <ogg-opus-path>] [--audio-iterations <n>] [--duration-sec <seconds>] [--frame-ms <ms>] [--repeat <n>] [--start-retries <n>] [--audio-sample-log <csv-path>] [--log-level <1-5|11+>]\n",
            program,
            program,
            program,
            program,
            program);
    fprintf(stderr,
            "\nConnection values may also be supplied with TIRTC_ENDPOINT, "
            "TIRTC_DEVICE_ID,\nTIRTC_DEVICE_SECRET_KEY, TIRTC_PEER_ID, and "
            "TIRTC_TOKEN. Command-line values take precedence.\n");
}

static int parse_int_value(const char *name, const char *value, int min_value, int max_value, int *out)
{
    char *endptr;
    long parsed;

    errno = 0;
    parsed = strtol(value, &endptr, 10);
    if (errno != 0 || endptr == value || *endptr != '\0' || parsed < min_value || parsed > max_value) {
        log_message(stderr, "invalid %s: %s", name, value);
        return -1;
    }
    *out = (int)parsed;
    return 0;
}

static int parse_positive_int64_value(const char *name, const char *value, int64_t *out)
{
    char *endptr;
    long long parsed;

    errno = 0;
    parsed = strtoll(value, &endptr, 10);
    if (errno != 0 || endptr == value || *endptr != '\0' || parsed <= 0) {
        log_message(stderr, "invalid %s: %s", name, value);
        return -1;
    }
    *out = (int64_t)parsed;
    return 0;
}

static int parse_arguments(int argc, char **argv, probe_config_t *config)
{
    int i;

    memset(config, 0, sizeof(*config));
    config->iterations = DEFAULT_ITERATIONS;
    config->connections = DEFAULT_CONNECTIONS;
    config->repeat = DEFAULT_REPEAT;
    config->interval_ms = DEFAULT_INTERVAL_MS;
    config->timeout_ms = DEFAULT_TIMEOUT_MS;
    config->connect_timeout_ms = DEFAULT_CONNECT_TIMEOUT_MS;
    config->duration_sec = DEFAULT_DURATION_SEC;
    config->frame_ms = DEFAULT_FRAME_MS;
    config->audio_iterations = DEFAULT_ITERATIONS;
    config->start_retries = DEFAULT_START_RETRIES;
    config->log_level = DEFAULT_LOG_LEVEL;

    if (argc < 2) {
        return -1;
    }
    if (strcmp(argv[1], "connect") == 0) {
        config->command = COMMAND_CONNECT;
    } else if (strcmp(argv[1], "idle") == 0) {
        config->command = COMMAND_IDLE;
    } else if (strcmp(argv[1], "timesync") == 0) {
        config->command = COMMAND_TIMESYNC;
    } else if (strcmp(argv[1], "media") == 0) {
        config->command = COMMAND_MEDIA;
    } else if (strcmp(argv[1], "audio") == 0) {
        config->command = COMMAND_AUDIO;
    } else if (strcmp(argv[1], "--help") == 0) {
        return 1;
    } else {
        log_message(stderr, "unknown command: %s", argv[1]);
        return -1;
    }

    i = 2;
    while (i < argc) {
        const char *arg = argv[i];
        if (strcmp(arg, "--endpoint") == 0 ||
            strcmp(arg, "--device-id") == 0 ||
            strcmp(arg, "--device-secret-key") == 0 ||
            strcmp(arg, "--peer-id") == 0 ||
            strcmp(arg, "--token") == 0 ||
            strcmp(arg, "--iterations") == 0 ||
            strcmp(arg, "--connections") == 0 ||
            strcmp(arg, "--repeat") == 0 ||
            strcmp(arg, "--interval-ms") == 0 ||
            strcmp(arg, "--timeout-ms") == 0 ||
            strcmp(arg, "--connect-timeout-ms") == 0 ||
            strcmp(arg, "--duration-sec") == 0 ||
            strcmp(arg, "--audio-iterations") == 0 ||
            strcmp(arg, "--audio-sample-log") == 0 ||
            strcmp(arg, "--audio-input") == 0 ||
            strcmp(arg, "--audio-echo-output") == 0 ||
            strcmp(arg, "--audio-output") == 0 ||
            strcmp(arg, "--start-retries") == 0 ||
            strcmp(arg, "--log-level") == 0 ||
            strcmp(arg, "--frame-ms") == 0) {
            if (i + 1 >= argc) {
                log_message(stderr, "%s requires a value", arg);
                return -1;
            }
            if (strcmp(arg, "--endpoint") == 0) {
                config->endpoint = argv[i + 1];
            } else if (strcmp(arg, "--device-id") == 0) {
                config->device_id = argv[i + 1];
            } else if (strcmp(arg, "--device-secret-key") == 0) {
                config->device_secret_key = argv[i + 1];
            } else if (strcmp(arg, "--peer-id") == 0) {
                config->peer_id = argv[i + 1];
            } else if (strcmp(arg, "--token") == 0) {
                config->token = argv[i + 1];
            } else if (strcmp(arg, "--iterations") == 0 &&
                       parse_int_value(arg, argv[i + 1], 1, 10000, &config->iterations) != 0) {
                return -1;
            } else if (strcmp(arg, "--connections") == 0 &&
                       parse_int_value(arg, argv[i + 1], 1, 10000, &config->connections) != 0) {
                return -1;
            } else if (strcmp(arg, "--repeat") == 0 &&
                       parse_int_value(arg, argv[i + 1], 1, 10000, &config->repeat) != 0) {
                return -1;
            } else if (strcmp(arg, "--interval-ms") == 0 &&
                       parse_int_value(arg, argv[i + 1], 1, 60000, &config->interval_ms) != 0) {
                return -1;
            } else if (strcmp(arg, "--timeout-ms") == 0 &&
                       parse_int_value(arg, argv[i + 1], 1, 60000, &config->timeout_ms) != 0) {
                return -1;
            } else if (strcmp(arg, "--connect-timeout-ms") == 0 &&
                       parse_int_value(arg, argv[i + 1], 1, 120000, &config->connect_timeout_ms) != 0) {
                return -1;
            } else if (strcmp(arg, "--duration-sec") == 0 &&
                       parse_positive_int64_value(arg, argv[i + 1], &config->duration_sec) != 0) {
                return -1;
            } else if (strcmp(arg, "--duration-sec") == 0) {
                config->duration_explicit = 1;
            } else if (strcmp(arg, "--audio-iterations") == 0 &&
                       parse_int_value(arg, argv[i + 1], 1, 10000, &config->audio_iterations) != 0) {
                return -1;
            } else if (strcmp(arg, "--audio-sample-log") == 0) {
                config->audio_sample_log = argv[i + 1];
            } else if (strcmp(arg, "--audio-input") == 0) {
                config->audio_input = argv[i + 1];
            } else if (strcmp(arg, "--audio-echo-output") == 0) {
                config->audio_echo_output = argv[i + 1];
            } else if (strcmp(arg, "--audio-output") == 0) {
                config->audio_output = argv[i + 1];
            } else if (strcmp(arg, "--start-retries") == 0 &&
                       parse_int_value(arg, argv[i + 1], 1, 100, &config->start_retries) != 0) {
                return -1;
            } else if (strcmp(arg, "--log-level") == 0) {
                if (parse_int_value(arg, argv[i + 1], 1, 100, &config->log_level) != 0 ||
                    (config->log_level > 5 && config->log_level <= 10)) {
                    log_message(stderr, "invalid %s: %s (expected 1-5 or 11-100)",
                                arg, argv[i + 1]);
                    return -1;
                }
            } else if (strcmp(arg, "--frame-ms") == 0 &&
                       parse_int_value(arg, argv[i + 1], 1, 1000, &config->frame_ms) != 0) {
                return -1;
            }
            i += 2;
            continue;
        }
        if (strcmp(arg, "--json") == 0) {
            config->json_output = 1;
            i++;
            continue;
        }
        if (strcmp(arg, "--client-mode") == 0) {
            config->client_mode = 1;
            i++;
            continue;
        }
        if (strcmp(arg, "--help") == 0) {
            return 1;
        }
        log_message(stderr, "unknown argument: %s", arg);
        return -1;
    }

    if (config->endpoint == NULL) {
        config->endpoint = getenv("TIRTC_ENDPOINT");
    }
    if (config->device_id == NULL) {
        config->device_id = getenv("TIRTC_DEVICE_ID");
    }
    if (config->device_secret_key == NULL) {
        config->device_secret_key = getenv("TIRTC_DEVICE_SECRET_KEY");
    }
    if (config->peer_id == NULL) {
        config->peer_id = getenv("TIRTC_PEER_ID");
    }
    if (config->token == NULL) {
        config->token = getenv("TIRTC_TOKEN");
    }

    if (config->endpoint == NULL || config->endpoint[0] == '\0' ||
        config->device_id == NULL || config->device_id[0] == '\0' ||
        config->device_secret_key == NULL || config->device_secret_key[0] == '\0' ||
        config->peer_id == NULL || config->peer_id[0] == '\0' ||
        config->token == NULL || config->token[0] == '\0') {
        log_message(stderr,
                    "missing required endpoint/device-id/device-secret-key/peer-id/token argument");
        return -1;
    }
    if (config->command != COMMAND_AUDIO &&
        (config->audio_input != NULL || config->audio_echo_output != NULL)) {
        log_message(stderr, "--audio-input/--audio-echo-output are valid only for audio command");
        return -1;
    }
    if (config->command != COMMAND_MEDIA && config->audio_output != NULL) {
        log_message(stderr, "--audio-output is valid only for media command");
        return -1;
    }
    if (config->audio_output != NULL && config->audio_output[0] == '\0') {
        log_message(stderr, "--audio-output requires a non-empty path");
        return -1;
    }
    if (config->audio_echo_output != NULL && config->audio_echo_output[0] != '\0' &&
        (config->audio_input == NULL || config->audio_input[0] == '\0')) {
        log_message(stderr, "--audio-echo-output requires --audio-input");
        return -1;
    }
    return 0;
}

static int sdk_start(const probe_config_t *config)
{
    char *client_id;
    int max_connections;
    int rc;

    client_id = build_client_id(config->device_id);
    if (client_id == NULL) {
        return -1;
    }

    if (config->command == COMMAND_IDLE) {
        max_connections = config->connections;
        rc = TiRtcSetOption(TIRTC_OPT_MAX_CONNECTIONS,
                            &max_connections,
                            sizeof(max_connections));
        if (rc != 0) {
            log_message(stderr,
                        "failed to set TiRTC max connections=%d: rc=%d error=%s",
                        max_connections,
                        rc,
                        TiRtcGetErrorStr(rc));
            free(client_id);
            return rc;
        }
        log_message(stdout, "TiRTC max connections: %d", max_connections);
    }

    if (TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER,
                       &kSdkMaxSendBufferBytes,
                       sizeof(kSdkMaxSendBufferBytes)) != 0) {
        free(client_id);
        return -1;
    }
    rc = TiRtcInit();
    if (rc != 0) {
        free(client_id);
        return rc;
    }
    TiRtcLogConfig(1, NULL, 0);
    TiRtcLogSetLevel(config->log_level);
    log_message(stdout, "TiRTC log level: %d", config->log_level);

    if (TiRtcSetOption(TIRTC_OPT_DEVICE_SECRET_KEY,
                       config->device_secret_key,
                       (uint32_t)strlen(config->device_secret_key)) != 0 ||
        TiRtcSetOption(TIRTC_OPT_CLIENT_ID,
                       client_id,
                       (uint32_t)strlen(client_id)) != 0 ||
        TiRtcSetOption(TIRTC_OPT_SERVICE_ENDPOINT,
                       config->endpoint,
                       (uint32_t)strlen(config->endpoint)) != 0) {
        free(client_id);
        TiRtcUninit();
        return -1;
    }
    free(client_id);

    pthread_mutex_lock(&g_app.mutex);
    g_app.sdk_started = 0;
    g_app.sdk_stopped = 0;
    pthread_mutex_unlock(&g_app.mutex);
    if (callback_worker_start() != 0) {
        TiRtcUninit();
        return -1;
    }

    log_message(stdout, "TiRTC start mode: %s",
                config->client_mode ? "client" : "device");
    rc = TiRtcStart(config->client_mode ? NULL : config->device_id, &g_callbacks);
    if (rc != 0) {
        callback_worker_stop();
        TiRtcUninit();
        return rc;
    }
    if (wait_for_sdk_started(config->connect_timeout_ms) != 0) {
        (void)TiRtcStop();
        callback_worker_stop();
        TiRtcUninit();
        return TIRTC_E_TIMEOUTED;
    }
    return 0;
}

static int sdk_start_error_retryable(int rc)
{
    switch (rc) {
        case TIRTC_E_TIMEOUTED:
        case TIRTC_E_BUSY:
        case TIRTC_E_CONN_TIMEOUTCLOSE:
        case TIRTC_E_CONN_REMOTECLOSE:
        case TIRTC_E_CONN_OTHER_ERROR:
        case TIRTC_E_LACK_OF_RESOURCE:
        case TIRTC_E_SERVER_ERROR:
        case TIRTC_E_INTERNAL_ERROR:
        case TIRTC_E_UNEXPECTED_RESPONSE:
            return 1;
        case TIRTC_E_NOT_INITIALIZED:
        case TIRTC_E_INVALID_HANDLE:
        case TIRTC_E_INVALID_PARAMETER:
        case TIRTC_E_INVALID_LICENSE:
        case TIRTC_E_CACHE_EXPIRED:
        case TIRTC_E_NO_SECRET_KEY:
            return 0;
        default:
            /* Preserve retries for transport/internal codes not yet exposed by tiRTC.h. */
            return 1;
    }
}

static int sdk_start_with_retry(const probe_config_t *config)
{
    int64_t started_us = monotonic_now_us();
    int rc = -1;
    int attempt;

    for (attempt = 1; attempt <= config->start_retries && !g_should_exit; ++attempt) {
        rc = sdk_start(config);
        if (rc == 0) {
            log_message(stdout,
                        "TiRtcStart succeeded after attempts=%d elapsed=%.2fms",
                        attempt,
                        us_to_ms(monotonic_now_us() - started_us));
            return 0;
        }

        if (!sdk_start_error_retryable(rc)) {
            log_message(stderr,
                        "TiRtcStart attempt %d/%d failed: rc=%d error=%s retryable=no elapsed=%.2fms",
                        attempt,
                        config->start_retries,
                        rc,
                        TiRtcGetErrorStr(rc),
                        us_to_ms(monotonic_now_us() - started_us));
            return rc;
        }

        log_message(stderr,
                    "TiRtcStart attempt %d/%d failed: rc=%d error=%s retryable=yes elapsed=%.2fms",
                    attempt,
                    config->start_retries,
                    rc,
                    TiRtcGetErrorStr(rc),
                    us_to_ms(monotonic_now_us() - started_us));
    }

    log_message(stderr,
                "TiRtcStart failed after attempts=%d rc=%d error=%s elapsed=%.2fms",
                attempt - 1,
                rc,
                TiRtcGetErrorStr(rc),
                us_to_ms(monotonic_now_us() - started_us));
    return rc;
}

static void sdk_stop(void)
{
    if (TiRtcStop() != 0) {
        log_message(stderr, "TiRtcStop returned failure");
    }
    if (wait_for_sdk_stopped(SDK_STOP_TIMEOUT_MS) != 0) {
        log_message(stderr, "timed out waiting for SDK stop");
    }
    callback_worker_stop();
    TiRtcUninit();
}

int main(int argc, char **argv)
{
    probe_config_t config;
    int parse_result;
    int rc;

    printf("[tirn-probe] TiRTC SDK variant: %s\n", TIRTC_SDK_VARIANT);
    printf("[tirn-probe] TiRTC SDK source: %s\n", TIRTC_SDK_SOURCE_URL);
    parse_result = parse_arguments(argc, argv, &config);
    if (parse_result > 0) {
        print_usage(argv[0]);
        return 0;
    }
    if (parse_result < 0) {
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    log_message(stdout, "TiRTC version: %s", TiRtcGetVersion());
    if (config.command == COMMAND_CONNECT || config.command == COMMAND_IDLE ||
        config.command == COMMAND_MEDIA || config.command == COMMAND_AUDIO) {
        rc = sdk_start_with_retry(&config);
    } else {
        rc = sdk_start(&config);
    }
    if (rc != 0) {
        log_message(stderr, "failed to start SDK: rc=%d error=%s", rc, TiRtcGetErrorStr(rc));
        return 1;
    }

    switch (config.command) {
    case COMMAND_CONNECT:
        rc = run_connect_command(&config);
        break;
    case COMMAND_IDLE:
        rc = run_idle_command(&config);
        break;
    case COMMAND_TIMESYNC:
        rc = run_timesync_command(&config);
        break;
    case COMMAND_MEDIA:
        rc = run_media_command(&config);
        break;
    case COMMAND_AUDIO:
        rc = run_audio_command(&config);
        break;
    default:
        rc = 1;
        break;
    }

    sdk_stop();
    return rc;
}
