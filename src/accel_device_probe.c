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

enum {
    CMD_TIME_SYNC_REQUEST = 0x2003,
    CMD_TIME_SYNC_RESPONSE = 0x2004,
    CMD_AUDIO_PACKET_OBSERVED = 0x2005,

    AUDIO_STREAM_ID = 0,
    AUDIO_SAMPLE_RATE_HZ = 8000,
    AUDIO_BYTES_PER_SAMPLE = 2,
    AUDIO_PAYLOAD_BYTES = 640,

    DEFAULT_ITERATIONS = 1,
    DEFAULT_REPEAT = 20,
    DEFAULT_INTERVAL_MS = 100,
    DEFAULT_TIMEOUT_MS = 1000,
    DEFAULT_CONNECT_TIMEOUT_MS = 20000,
    DEFAULT_DURATION_MS = 10000,
    DEFAULT_CONNECTIONS = 1,
    DEFAULT_FRAME_MS = 40,
    DEFAULT_LOG_LEVEL = 3,
    DEFAULT_START_RETRIES = 5,
    SDK_STOP_TIMEOUT_MS = 3000,
    DISCONNECT_TIMEOUT_MS = 3000,
    CALLBACK_EVENT_RING_CAP = 2048,
    CALLBACK_EVENT_DATA_BYTES = 2048,
};

static const uint32_t kSdkMaxSendBufferBytes = 2U * 1024U * 1024U;
static const char kClientIdSuffix[] = "-accel-probe";
static const double kAudioToneHz = 440.0;
static const double kTwoPi = 6.28318530717958647692;
static int16_t g_audio_tone_samples[AUDIO_SAMPLE_RATE_HZ];
static int g_audio_tone_samples_initialized;

typedef enum {
    COMMAND_CONNECT,
    COMMAND_IDLE,
    COMMAND_TIMESYNC,
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
    int duration_ms;
    int frame_ms;
    int audio_iterations;
    int start_retries;
    int log_level;
    int json_output;
    const char *audio_sample_log;
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
    uint32_t frame_ts_ms;
    int64_t client_send_unix_ns;
    int64_t client_send_mono_us;
    int64_t send_late_us;
    int64_t client_echo_recv_mono_us;
    int64_t server_recv_unix_ns;
    int send_ret;
    int observed;
    int echoed;
    int64_t echo_arrival_gap_us;
    int64_t stutter_time_us;
    int stutter;
} audio_sample_t;

typedef struct {
    audio_sample_t *items;
    size_t len;
    size_t cap;
    uint32_t next_frame_index;
    int64_t first_client_send_unix_ns;
    int64_t first_server_recv_unix_ns;
    int64_t first_echo_recv_unix_ns;
    int64_t first_echo_recv_mono_us;
    int64_t last_echo_recv_mono_us;
    int64_t stutter_time_us;
    int64_t call_started_mono_us;
    int64_t call_finished_mono_us;
    int64_t max_send_late_us;
    int expected_frame_ms;
    uint64_t late_send_count;
    uint64_t stutter_count;
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
    int64_t connect_started_us;
    int64_t connect_cost_us;
    uint32_t waiting_timesync_seq;
    int64_t waiting_timesync_client_send_ns;
    int timesync_done;
    int timesync_error;
    time_sync_sample_t timesync_sample;
    audio_metrics_t audio;
} probe_session_t;

typedef enum {
    CALLBACK_EVENT_SYSTEM,
    CALLBACK_EVENT_CONNECT_RESULT,
    CALLBACK_EVENT_CONN_ERROR,
    CALLBACK_EVENT_DISCONNECTED,
    CALLBACK_EVENT_COMMAND,
    CALLBACK_EVENT_AUDIO,
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
    fprintf(stream, "[accel-probe] ");
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

static double ns_to_s(int64_t value_ns)
{
    return (double)value_ns / 1000000000.0;
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

static void print_duration_summary_ms_cn(const char *name, sample_set_t *set)
{
    if (set->len == 0) {
        printf("%s: 无样本\n", name);
        return;
    }
    qsort(set->values, set->len, sizeof(*set->values), compare_i64);
    printf("%s: 样本数=%zu 平均=%.2fms 中位数=%.2fms P90=%.2fms P95=%.2fms P99=%.2fms\n",
           name,
           set->len,
           average_samples(set) / 1000.0,
           us_to_ms(percentile_sorted(set, 0.50)),
           us_to_ms(percentile_sorted(set, 0.90)),
           us_to_ms(percentile_sorted(set, 0.95)),
           us_to_ms(percentile_sorted(set, 0.99)));
}

static void print_value_summary_cn(const char *name, sample_set_t *set)
{
    if (set->len == 0) {
        printf("%s: 无样本\n", name);
        return;
    }
    qsort(set->values, set->len, sizeof(*set->values), compare_i64);
    printf("%s: 样本数=%zu 平均=%.2f 中位数=%" PRId64 " P90=%" PRId64 " P95=%" PRId64 " P99=%" PRId64 "\n",
           name,
           set->len,
           average_samples(set),
           percentile_sorted(set, 0.50),
           percentile_sorted(set, 0.90),
           percentile_sorted(set, 0.95),
           percentile_sorted(set, 0.99));
}

static void print_percent_summary_cn(const char *name, sample_set_t *set)
{
    if (set->len == 0) {
        printf("%s: 无样本\n", name);
        return;
    }
    qsort(set->values, set->len, sizeof(*set->values), compare_i64);
    printf("%s: 样本数=%zu 平均=%.2f%% 中位数=%.2f%% P90=%.2f%% P95=%.2f%% P99=%.2f%%\n",
           name,
           set->len,
           average_samples(set) / 10000.0,
           (double)percentile_sorted(set, 0.50) / 10000.0,
           (double)percentile_sorted(set, 0.90) / 10000.0,
           (double)percentile_sorted(set, 0.95) / 10000.0,
           (double)percentile_sorted(set, 0.99) / 10000.0);
}

static void sample_set_append_all(sample_set_t *dst, const sample_set_t *src)
{
    size_t i;

    for (i = 0; i < src->len; ++i) {
        (void)sample_set_push(dst, src->values[i]);
    }
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

static audio_sample_t *audio_metrics_find(audio_metrics_t *metrics, uint32_t frame_ts_ms)
{
    size_t i;

    for (i = 0; i < metrics->len; ++i) {
        if (metrics->items[i].frame_ts_ms == frame_ts_ms) {
            return &metrics->items[i];
        }
    }
    return NULL;
}

static audio_sample_t *audio_metrics_append(audio_metrics_t *metrics,
                                            uint32_t send_index,
                                            uint32_t frame_ts_ms,
                                            int64_t send_unix_ns,
                                            int64_t send_mono_us,
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
    sample->frame_ts_ms = frame_ts_ms;
    sample->client_send_unix_ns = send_unix_ns;
    sample->client_send_mono_us = send_mono_us;
    sample->send_late_us = send_late_us;
    sample->send_ret = 0;
    return sample;
}

static void print_missing_audio_ranges(const char *label, const audio_metrics_t *metrics, int observed_field)
{
    int in_range = 0;
    uint32_t range_start = 0;
    uint32_t range_end = 0;
    int printed = 0;
    size_t i;

    printf("%s: ", label);
    for (i = 0; i < metrics->len; ++i) {
        const audio_sample_t *sample = &metrics->items[i];
        int present = observed_field ? sample->observed : sample->echoed;
        if (!present) {
            if (!in_range) {
                range_start = sample->send_index;
                in_range = 1;
            }
            range_end = sample->send_index;
            continue;
        }
        if (in_range) {
            printf("%s%u", printed ? "," : "", range_start);
            if (range_end != range_start) {
                printf("-%u", range_end);
            }
            printed = 1;
            in_range = 0;
        }
    }
    if (in_range) {
        printf("%s%u", printed ? "," : "", range_start);
        if (range_end != range_start) {
            printf("-%u", range_end);
        }
        printed = 1;
    }
    if (!printed) {
        printf("无");
    }
    printf("\n");
}

static void print_missing_audio_details(const char *label, const audio_metrics_t *metrics, int observed_field)
{
    size_t i;
    size_t printed = 0;
    size_t missing_count = 0;
    const size_t max_print = 80U;

    for (i = 0; i < metrics->len; ++i) {
        const audio_sample_t *sample = &metrics->items[i];
        int present = observed_field ? sample->observed : sample->echoed;
        if (!present) {
            missing_count++;
        }
    }
    if (missing_count == 0) {
        return;
    }

    printf("%s: 缺失数=%zu", label, missing_count);
    for (i = 0; i < metrics->len && printed < max_print; ++i) {
        const audio_sample_t *sample = &metrics->items[i];
        int present = observed_field ? sample->observed : sample->echoed;
        if (present) {
            continue;
        }
        printf(" #%u(send_unix=%.6f, since_start=%.2fms, late=%.2fms, send_ret=%d)",
               sample->send_index,
               ns_to_s(sample->client_send_unix_ns),
               us_to_ms(sample->client_send_mono_us - metrics->call_started_mono_us),
               us_to_ms(sample->send_late_us),
               sample->send_ret);
        printed++;
    }
    if (missing_count > printed) {
        printf(" ... 省略=%zu", missing_count - printed);
    }
    printf("\n");
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

static int is_latency_probe_payload(const void *data, uint32_t len)
{
    static const uint8_t magic[] = {'T', 'I', 'R', 'T', 'C', 'E', 'C', 'H'};

    return len >= sizeof(magic) && memcmp(data, magic, sizeof(magic)) == 0;
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
    } else if (cmdw == CMD_AUDIO_PACKET_OBSERVED) {
        uint32_t frame_ts_ms = 0;
        int64_t server_recv_unix_ns = 0;
        if (json_get_u32(json, "frame_ts_ms", &frame_ts_ms) == 0 &&
            json_get_i64(json, "received_at_unix_ns", &server_recv_unix_ns) == 0) {
            audio_sample_t *sample = audio_metrics_find(&session->audio, frame_ts_ms);
            if (sample != NULL) {
                sample->server_recv_unix_ns = server_recv_unix_ns;
                sample->observed = 1;
            }
            if (session->audio.first_server_recv_unix_ns == 0) {
                session->audio.first_server_recv_unix_ns = server_recv_unix_ns;
            }
        }
    }
    pthread_mutex_unlock(&session->mutex);
    free(json);
}

static void handle_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
    probe_session_t *session = active_session_for_conn(hconn);
    uint32_t frame_ts_ms;
    int64_t now_mono_us;
    int64_t now_unix_ns;
    audio_sample_t *sample;
    int64_t gap_us;

    if (session == NULL || info == NULL || data == NULL) {
        return;
    }
    if (session->idle_only) {
        return;
    }
    if (is_latency_probe_payload(data, info->length)) {
        (void)TiRtcSendAudioStream(hconn, info, data);
        return;
    }
    frame_ts_ms = info->ts;

    now_mono_us = monotonic_now_us();
    now_unix_ns = realtime_now_ns();

    pthread_mutex_lock(&session->mutex);
    sample = audio_metrics_find(&session->audio, frame_ts_ms);
    if (sample != NULL && !sample->echoed) {
        sample->client_echo_recv_mono_us = now_mono_us;
        sample->echoed = 1;
        session->audio.echo_count++;
        if (session->audio.first_echo_recv_unix_ns == 0) {
            session->audio.first_echo_recv_unix_ns = now_unix_ns;
            session->audio.first_echo_recv_mono_us = now_mono_us;
        }
        if (session->audio.last_echo_recv_mono_us > 0) {
            gap_us = now_mono_us - session->audio.last_echo_recv_mono_us;
            sample->echo_arrival_gap_us = gap_us;
            if (gap_us > 300000LL) {
                int expected_frame_ms = session->audio.expected_frame_ms > 0 ?
                    session->audio.expected_frame_ms :
                    DEFAULT_FRAME_MS;
                sample->stutter = 1;
                sample->stutter_time_us = gap_us - (int64_t)expected_frame_ms * 1000LL;
                session->audio.stutter_count++;
                session->audio.stutter_time_us += sample->stutter_time_us;
            }
        }
        session->audio.last_echo_recv_mono_us = now_mono_us;
    }
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

static const TIRTCCALLBACKS g_callbacks = {
    .on_event = on_event,
    .on_conn_error = on_conn_error,
    .on_disconnected = on_disconnected,
    .on_audio = on_audio,
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
                "idle ramp started: target_connections=%d interval_ms=%d hold_duration_ms=%d",
                config->connections,
                config->interval_ms,
                config->duration_ms);
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
           monotonic_now_us() - hold_started_us < (int64_t)config->duration_ms * 1000LL) {
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
                                   const audio_metrics_t *audio,
                                   int64_t offset_ns)
{
    size_t i;

    if (stream == NULL) {
        return;
    }
    for (i = 0; i < audio->len; ++i) {
        const audio_sample_t *sample = &audio->items[i];
        int64_t echo_us = sample->echoed ?
            sample->client_echo_recv_mono_us - sample->client_send_mono_us : 0;
        int64_t echo_recv_unix_ns = sample->echoed ?
            sample->client_send_unix_ns + echo_us * 1000LL : 0;
        int64_t uplink_us = sample->observed ?
            (sample->server_recv_unix_ns - (sample->client_send_unix_ns + offset_ns)) / 1000LL : 0;
        int64_t downlink_us = sample->observed && sample->echoed ?
            ((echo_recv_unix_ns + offset_ns) - sample->server_recv_unix_ns) / 1000LL : 0;

        fprintf(stream,
                "%d,%" PRIu32 ",%" PRIu32 ",%d,%" PRId64 ",%d,%d,%" PRId64 ",%" PRId64
                ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d,%" PRId64 "\n",
                success_iteration,
                sample->send_index,
                sample->frame_ts_ms,
                sample->send_ret,
                sample->send_late_us,
                sample->observed,
                sample->echoed,
                sample->client_send_unix_ns,
                sample->server_recv_unix_ns,
                echo_recv_unix_ns,
                uplink_us,
                downlink_us,
                echo_us,
                sample->echo_arrival_gap_us,
                sample->stutter,
                sample->stutter_time_us);
    }
    fflush(stream);
}

static int run_audio_iteration(probe_session_t *session,
                               const probe_config_t *config,
                               int64_t offset_ns,
                               int success_iteration,
                               int target_success_iterations,
                               FILE *sample_log,
                               sample_set_t *first_d2s_us,
                               sample_set_t *first_echo_us,
                               sample_set_t *all_d2s_us,
                               sample_set_t *all_s2d_us,
                               sample_set_t *stutter_counts,
                               sample_set_t *stutter_time_us,
                               sample_set_t *stutter_rate_ppm,
                               uint64_t *total_sent,
                               uint64_t *total_send_failed,
                               uint64_t *total_server_observed,
                               uint64_t *total_echo_received)
{
    int64_t start_us;
    int64_t next_send_us;
    uint32_t base_ts_ms;
    sample_set_t d2s_us = {0};
    sample_set_t echo_us = {0};
    sample_set_t s2d_us = {0};
    int first_d2s_valid = 0;
    int first_echo_valid = 0;
    int64_t first_d2s_value_us = 0;
    int64_t first_echo_value_us = 0;
    uint64_t iteration_stutter_count = 0;
    int64_t iteration_stutter_time_us = 0;
    int64_t iteration_stutter_rate_ppm = 0;
    int64_t loop_cpu_start_ns;
    int64_t loop_cpu_ns;
    int64_t prepare_cpu_ns = 0;
    int64_t payload_cpu_ns = 0;
    int64_t sdk_send_cpu_ns = 0;
    int64_t post_send_cpu_ns = 0;
    size_t i;

    if (target_success_iterations > 1) {
        printf("音频测试轮次: 第 %d/%d 次\n", success_iteration, target_success_iterations);
    }

    init_audio_tone_samples();
    start_us = monotonic_now_us();
    next_send_us = start_us;
    base_ts_ms = (uint32_t)(realtime_now_ns() / 1000000LL);
    pthread_mutex_lock(&session->mutex);
    audio_metrics_reset_iteration(&session->audio);
    session->audio.call_started_mono_us = start_us;
    session->audio.expected_frame_ms = config->frame_ms;
    pthread_mutex_unlock(&session->mutex);

    loop_cpu_start_ns = thread_cpu_now_ns();
    while (!g_should_exit &&
           monotonic_now_us() - start_us < (int64_t)config->duration_ms * 1000LL) {
        int64_t now_us = monotonic_now_us();
        if (now_us >= next_send_us) {
            uint8_t payload[AUDIO_PAYLOAD_BYTES];
            TIRTCFRAMEINFO info;
            uint32_t frame_ts_ms;
            uint32_t frame_index;
            size_t sample_index;
            int64_t segment_cpu_start_ns = thread_cpu_now_ns();
            int64_t send_unix_ns = realtime_now_ns();
            int64_t send_mono_us = monotonic_now_us();
            int64_t send_late_us = now_us - next_send_us;
            int send_ret;

            pthread_mutex_lock(&session->mutex);
            frame_index = ++session->audio.next_frame_index;
            frame_ts_ms = base_ts_ms +
                (frame_index - 1U) * (uint32_t)config->frame_ms;
            if (session->audio.first_client_send_unix_ns == 0) {
                session->audio.first_client_send_unix_ns = send_unix_ns;
            }
            if (send_late_us > session->audio.max_send_late_us) {
                session->audio.max_send_late_us = send_late_us;
            }
            if (send_late_us > (int64_t)config->frame_ms * 1000LL) {
                session->audio.late_send_count++;
            }
            if (audio_metrics_append(&session->audio,
                                     frame_index,
                                     frame_ts_ms,
                                     send_unix_ns,
                                     send_mono_us,
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
            make_audio_payload(payload, frame_index);
            memset(&info, 0, sizeof(info));
            info.stream_id = AUDIO_STREAM_ID;
            info.media = TIRTC_AUDIO_PCM;
            info.flags = TIRTC_AUDIOSAMPLE_8K16B1C;
            info.ts = frame_ts_ms;
            info.length = AUDIO_PAYLOAD_BYTES;
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
            next_send_us += (int64_t)config->frame_ms * 1000LL;
            post_send_cpu_ns += thread_cpu_now_ns() - segment_cpu_start_ns;
        } else {
            sleep_until_monotonic_us(next_send_us);
        }
    }
    loop_cpu_ns = thread_cpu_now_ns() - loop_cpu_start_ns;
    sleep_for_us(1000000LL);

    pthread_mutex_lock(&session->mutex);
    session->audio.call_finished_mono_us = monotonic_now_us();
    for (i = 0; i < session->audio.len; ++i) {
        audio_sample_t *sample = &session->audio.items[i];
        if (sample->observed) {
            int64_t client_send_on_server_ns = sample->client_send_unix_ns + offset_ns;
            (void)sample_set_push(&d2s_us, (sample->server_recv_unix_ns - client_send_on_server_ns) / 1000LL);
        }
        if (sample->echoed) {
            (void)sample_set_push(&echo_us, sample->client_echo_recv_mono_us - sample->client_send_mono_us);
        }
        if (sample->observed && sample->echoed) {
            int64_t echo_recv_unix_ns = sample->client_send_unix_ns +
                                        (sample->client_echo_recv_mono_us - sample->client_send_mono_us) * 1000LL;
            int64_t echo_recv_on_server_ns = echo_recv_unix_ns + offset_ns;
            (void)sample_set_push(&s2d_us, (echo_recv_on_server_ns - sample->server_recv_unix_ns) / 1000LL);
        }
    }
    if (session->audio.len > 0) {
        audio_sample_t *first = &session->audio.items[0];
        if (first->observed) {
            int64_t client_send_on_server_ns = first->client_send_unix_ns + offset_ns;
            first_d2s_value_us = (first->server_recv_unix_ns - client_send_on_server_ns) / 1000LL;
            first_d2s_valid = 1;
        }
        if (first->echoed) {
            first_echo_value_us = first->client_echo_recv_mono_us - first->client_send_mono_us;
            first_echo_valid = 1;
        }
    }
    write_audio_sample_log(sample_log, success_iteration, &session->audio, offset_ns);
    printf("音频首包时间: 设备发送=%.6f 服务端收到(已换算到设备时钟)=%.6f 设备收到回声=%.6f\n",
           ns_to_s(session->audio.first_client_send_unix_ns),
           ns_to_s(session->audio.first_server_recv_unix_ns - offset_ns),
           ns_to_s(session->audio.first_echo_recv_unix_ns));
    printf("音频包统计: 设备发送=%" PRIu64 " 发送失败=%" PRIu64 " 服务端收到=%zu 设备收到回声=%" PRIu64 "\n",
           session->audio.send_count,
           session->audio.send_failed,
           d2s_us.len,
           session->audio.echo_count);
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
    if (d2s_us.len != session->audio.len || session->audio.echo_count != session->audio.len) {
        print_missing_audio_ranges("音频服务端未收到包序号", &session->audio, 1);
        print_missing_audio_ranges("音频设备未收到回声包序号", &session->audio, 0);
        print_missing_audio_details("音频服务端未收到包明细", &session->audio, 1);
        print_missing_audio_details("音频设备未收到回声包明细", &session->audio, 0);
    }
    *total_sent += session->audio.send_count;
    *total_send_failed += session->audio.send_failed;
    *total_server_observed += (uint64_t)d2s_us.len;
    *total_echo_received += session->audio.echo_count;
    {
        int64_t call_us = session->audio.call_finished_mono_us - session->audio.call_started_mono_us;
        double stutter_rate = call_us <= 0 ? 0.0 : (double)session->audio.stutter_time_us / (double)call_us;
        iteration_stutter_count = session->audio.stutter_count;
        iteration_stutter_time_us = session->audio.stutter_time_us;
        iteration_stutter_rate_ppm = call_us <= 0 ? 0 : session->audio.stutter_time_us * 1000000LL / call_us;
        printf("音频卡顿统计: 次数=%" PRIu64 " 累计时长=%.2fms 占比=%.2f%% 说明=相邻回声包间隔超过300ms计为卡顿\n",
               session->audio.stutter_count,
               us_to_ms(session->audio.stutter_time_us),
               stutter_rate * 100.0);
    }
    pthread_mutex_unlock(&session->mutex);

    print_duration_summary_ms_cn("音频上行延迟(设备到服务端)", &d2s_us);
    print_duration_summary_ms_cn("音频下行延迟(服务端到设备)", &s2d_us);
    print_duration_summary_ms_cn("音频回声总延迟(设备发出到收到回声)", &echo_us);

    if (first_d2s_valid) {
        (void)sample_set_push(first_d2s_us, first_d2s_value_us);
    }
    if (first_echo_valid) {
        (void)sample_set_push(first_echo_us, first_echo_value_us);
    }
    sample_set_append_all(all_d2s_us, &d2s_us);
    sample_set_append_all(all_s2d_us, &s2d_us);
    (void)sample_set_push(stutter_counts, (int64_t)iteration_stutter_count);
    (void)sample_set_push(stutter_time_us, iteration_stutter_time_us);
    (void)sample_set_push(stutter_rate_ppm, iteration_stutter_rate_ppm);
    sample_set_free(&d2s_us);
    sample_set_free(&echo_us);
    sample_set_free(&s2d_us);
    return 0;
}

static void print_audio_cumulative_summary(int success,
                                           int target_success,
                                           const sample_set_t *first_d2s_us,
                                           const sample_set_t *first_echo_us,
                                           const sample_set_t *all_d2s_us,
                                           const sample_set_t *all_s2d_us,
                                           const sample_set_t *stutter_counts,
                                           const sample_set_t *stutter_time_us,
                                           const sample_set_t *stutter_rate_ppm,
                                           uint64_t total_sent,
                                           uint64_t total_send_failed,
                                           uint64_t total_server_observed,
                                           uint64_t total_echo_received)
{
    sample_set_t first_d2s_copy = {0};
    sample_set_t first_echo_copy = {0};
    sample_set_t all_d2s_copy = {0};
    sample_set_t all_s2d_copy = {0};
    sample_set_t stutter_counts_copy = {0};
    sample_set_t stutter_time_copy = {0};
    sample_set_t stutter_rate_copy = {0};
    double server_observed_rate = total_sent == 0 ? 0.0 :
        (double)total_server_observed * 100.0 / (double)total_sent;
    double echo_received_rate = total_sent == 0 ? 0.0 :
        (double)total_echo_received * 100.0 / (double)total_sent;

    sample_set_append_all(&first_d2s_copy, first_d2s_us);
    sample_set_append_all(&first_echo_copy, first_echo_us);
    sample_set_append_all(&all_d2s_copy, all_d2s_us);
    sample_set_append_all(&all_s2d_copy, all_s2d_us);
    sample_set_append_all(&stutter_counts_copy, stutter_counts);
    sample_set_append_all(&stutter_time_copy, stutter_time_us);
    sample_set_append_all(&stutter_rate_copy, stutter_rate_ppm);

    printf("音频多轮汇总: 成功轮次=%d/%d\n", success, target_success);
    printf("音频包数多轮汇总: 设备发送=%" PRIu64 " 发送失败=%" PRIu64 " 服务端收到=%" PRIu64 " 设备收到回声=%" PRIu64 " 服务端收包率=%.2f%% 回声收包率=%.2f%%\n",
           total_sent,
           total_send_failed,
           total_server_observed,
           total_echo_received,
           server_observed_rate,
           echo_received_rate);
    print_duration_summary_ms_cn("音频首包上行延迟(设备到服务端)", &first_d2s_copy);
    print_duration_summary_ms_cn("音频首包回声总延迟(设备发出到收到回声)", &first_echo_copy);
    print_value_summary_cn("音频卡顿次数多轮汇总", &stutter_counts_copy);
    print_duration_summary_ms_cn("音频卡顿时长多轮汇总", &stutter_time_copy);
    print_percent_summary_cn("音频卡顿占比多轮汇总", &stutter_rate_copy);
    print_duration_summary_ms_cn("音频上行延迟多轮汇总(设备到服务端)", &all_d2s_copy);
    print_duration_summary_ms_cn("音频下行延迟多轮汇总(服务端到设备)", &all_s2d_copy);

    sample_set_free(&first_d2s_copy);
    sample_set_free(&first_echo_copy);
    sample_set_free(&all_d2s_copy);
    sample_set_free(&all_s2d_copy);
    sample_set_free(&stutter_counts_copy);
    sample_set_free(&stutter_time_copy);
    sample_set_free(&stutter_rate_copy);
}

static int run_audio_command(const probe_config_t *config)
{
    probe_session_t sync_session;
    probe_config_t sync_config = *config;
    time_sync_sample_t *sync_samples = NULL;
    size_t sync_count = 0;
    int64_t offset_ns = 0;
    sample_set_t first_d2s_us = {0};
    sample_set_t first_echo_us = {0};
    sample_set_t all_d2s_us = {0};
    sample_set_t all_s2d_us = {0};
    sample_set_t stutter_counts = {0};
    sample_set_t stutter_time_us = {0};
    sample_set_t stutter_rate_ppm = {0};
    uint64_t total_sent = 0;
    uint64_t total_send_failed = 0;
    uint64_t total_server_observed = 0;
    uint64_t total_echo_received = 0;
    int success = 0;
    int attempts = 0;
    int sync_connect_attempt;
    int rc = TIRTC_E_CONN_OTHER_ERROR;
    FILE *sample_log = NULL;

    if (config->audio_sample_log != NULL && config->audio_sample_log[0] != '\0') {
        sample_log = fopen(config->audio_sample_log, "w");
        if (sample_log == NULL) {
            log_message(stderr,
                        "failed to open audio sample log %s: %s",
                        config->audio_sample_log,
                        strerror(errno));
            return 1;
        }
        fprintf(sample_log,
                "iteration,send_index,frame_ts_ms,send_ret,send_late_us,observed,echoed,"
                "client_send_unix_ns,server_recv_unix_ns,client_echo_recv_unix_ns,"
                "uplink_us,downlink_us,echo_us,echo_arrival_gap_us,stutter,stutter_time_us\n");
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
        return 1;
    }
    print_timesync_summary(sync_samples, sync_count, offset_ns);
    free(sync_samples);

    while (success < config->audio_iterations && !g_should_exit) {
        probe_session_t session;

        attempts++;
        session_init(&session);
        rc = connect_session(config, &session);
        if (rc == 0) {
            rc = run_audio_iteration(&session,
                                     config,
                                     offset_ns,
                                     success + 1,
                                     config->audio_iterations,
                                     sample_log,
                                     &first_d2s_us,
                                     &first_echo_us,
                                     &all_d2s_us,
                                     &all_s2d_us,
                                     &stutter_counts,
                                     &stutter_time_us,
                                     &stutter_rate_ppm,
                                     &total_sent,
                                     &total_send_failed,
                                     &total_server_observed,
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
                                           &first_d2s_us,
                                           &first_echo_us,
                                           &all_d2s_us,
                                           &all_s2d_us,
                                           &stutter_counts,
                                           &stutter_time_us,
                                           &stutter_rate_ppm,
                                           total_sent,
                                           total_send_failed,
                                           total_server_observed,
                                           total_echo_received);
        } else {
            log_message(stderr,
                        "audio attempt %d failed, successful_iterations=%d/%d",
                        attempts,
                        success,
                        config->audio_iterations);
        }
    }

    sample_set_free(&first_d2s_us);
    sample_set_free(&first_echo_us);
    sample_set_free(&all_d2s_us);
    sample_set_free(&all_s2d_us);
    sample_set_free(&stutter_counts);
    sample_set_free(&stutter_time_us);
    sample_set_free(&stutter_rate_ppm);
    if (sample_log != NULL) {
        fclose(sample_log);
    }
    return success == config->audio_iterations ? 0 : 1;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s connect  --endpoint <url> --device-id <id> --device-secret-key <key> --peer-id <whips://...> --token <token> [--iterations <n>] [--connect-timeout-ms <ms>] [--start-retries <n>] [--log-level <1-5|11+>]\n"
            "  %s idle     --endpoint <url> --device-id <id> --device-secret-key <key> --peer-id <whips://...> --token <token> --connections <n> [--duration-ms <ms>] [--interval-ms <ms>] [--connect-timeout-ms <ms>] [--log-level <1-5|11+>]\n"
            "  %s timesync --endpoint <url> --device-id <id> --device-secret-key <key> --peer-id <whips://...> --token <token> [--repeat <n>] [--interval-ms <ms>] [--timeout-ms <ms>] [--log-level <1-5|11+>]\n"
            "  %s audio    --endpoint <url> --device-id <id> --device-secret-key <key> --peer-id <whips://...> --token <token> [--audio-iterations <n>] [--duration-ms <ms>] [--frame-ms <ms>] [--repeat <n>] [--start-retries <n>] [--audio-sample-log <csv-path>] [--log-level <1-5|11+>]\n",
            program,
            program,
            program,
            program);
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
    config->duration_ms = DEFAULT_DURATION_MS;
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
            strcmp(arg, "--duration-ms") == 0 ||
            strcmp(arg, "--audio-iterations") == 0 ||
            strcmp(arg, "--audio-sample-log") == 0 ||
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
            } else if (strcmp(arg, "--duration-ms") == 0 &&
                       parse_int_value(arg, argv[i + 1], 1, 3600000, &config->duration_ms) != 0) {
                return -1;
            } else if (strcmp(arg, "--audio-iterations") == 0 &&
                       parse_int_value(arg, argv[i + 1], 1, 10000, &config->audio_iterations) != 0) {
                return -1;
            } else if (strcmp(arg, "--audio-sample-log") == 0) {
                config->audio_sample_log = argv[i + 1];
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
        if (strcmp(arg, "--help") == 0) {
            return 1;
        }
        log_message(stderr, "unknown argument: %s", arg);
        return -1;
    }

    if (config->endpoint == NULL || config->endpoint[0] == '\0' ||
        config->device_id == NULL || config->device_id[0] == '\0' ||
        config->device_secret_key == NULL || config->device_secret_key[0] == '\0' ||
        config->peer_id == NULL || config->peer_id[0] == '\0' ||
        config->token == NULL || config->token[0] == '\0') {
        log_message(stderr, "missing required endpoint/device/peer/token argument");
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

    rc = TiRtcStart(config->device_id, &g_callbacks);
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

    printf("[accel-probe] TiRTC SDK variant: %s\n", TIRTC_SDK_VARIANT);
    printf("[accel-probe] TiRTC SDK source: %s\n", TIRTC_SDK_SOURCE_URL);
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
        config.command == COMMAND_AUDIO) {
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
