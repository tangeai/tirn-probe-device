#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "device_demo_streamer.h"

typedef struct {
    const char *endpoint;
    const char *device_id;
    const char *device_secret_key;
    int log_level;
} device_demo_config_t;

enum {
    DEFAULT_LOG_LEVEL = 4,
};

typedef struct device_demo_session {
    tirtc_conn_t hconn;
    device_demo_streamer_t *streamer;
    pthread_mutex_t mutex;
    int disconnect_scheduled;
    int finalized;
} device_demo_session_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    device_demo_session_t *active_session;
    size_t live_session_count;
    int sdk_started;
    int sdk_stopped;
} device_demo_app_t;

static device_demo_app_t g_app = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};
static volatile sig_atomic_t g_should_exit = 0;
static const char kClientIdSuffix[] = "-12345";

static void log_message(FILE *stream, const char *fmt, ...)
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

    delay.tv_sec = (time_t)(duration_us / 1000000ULL);
    delay.tv_nsec = (long)(duration_us % 1000000ULL) * 1000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static void signal_handler(int signal_number)
{
    (void)signal_number;
    g_should_exit = 1;
}

static void print_usage(const char *program_name)
{
    fprintf(stderr,
            "Usage: %s [--endpoint <url>] --device-id <id> --device-secret-key <key> "
            "[--log-level <1-5|11+>]\n",
            program_name);
}

static int parse_log_level(const char *value, int *out_log_level)
{
    char *endptr;
    long parsed;

    errno = 0;
    parsed = strtol(value, &endptr, 10);
    if (errno != 0 || endptr == value || *endptr != '\0' ||
        parsed < 1 || parsed > 100 || (parsed > 5 && parsed <= 10)) {
        log_message(stderr, "invalid --log-level: %s (expected 1-5 or 11-100)", value);
        return -1;
    }
    *out_log_level = (int)parsed;
    return 0;
}

static int validate_required_value(const char *field_name, const char *value)
{
    if (value != NULL && value[0] != '\0') {
        return 0;
    }

    log_message(stderr, "missing required argument: %s", field_name);
    return -1;
}

static int validate_required_file(const char *path)
{
    if (access(path, R_OK) == 0) {
        return 0;
    }

    log_message(stderr, "required file is missing or unreadable: %s", path);
    return -1;
}

static int parse_arguments(int argc,
                           char **argv,
                           device_demo_config_t *out_config)
{
    int index = 1;

    memset(out_config, 0, sizeof(*out_config));
    out_config->log_level = DEFAULT_LOG_LEVEL;
    while (index < argc) {
        const char *argument = argv[index];

        if (strcmp(argument, "--endpoint") == 0) {
            if (index + 1 >= argc) {
                log_message(stderr, "--endpoint requires a value");
                return -1;
            }
            out_config->endpoint = argv[index + 1];
            index += 2;
            continue;
        }
        if (strcmp(argument, "--device-id") == 0) {
            if (index + 1 >= argc) {
                log_message(stderr, "--device-id requires a value");
                return -1;
            }
            out_config->device_id = argv[index + 1];
            index += 2;
            continue;
        }
        if (strcmp(argument, "--device-secret-key") == 0) {
            if (index + 1 >= argc) {
                log_message(stderr, "--device-secret-key requires a value");
                return -1;
            }
            out_config->device_secret_key = argv[index + 1];
            index += 2;
            continue;
        }
        if (strcmp(argument, "--log-level") == 0) {
            if (index + 1 >= argc) {
                log_message(stderr, "--log-level requires a value");
                return -1;
            }
            if (parse_log_level(argv[index + 1], &out_config->log_level) != 0) {
                return -1;
            }
            index += 2;
            continue;
        }
        if (strcmp(argument, "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        }

        log_message(stderr, "unknown argument: %s", argument);
        return -1;
    }

    if (validate_required_value("device_id", out_config->device_id) != 0 ||
        validate_required_value("device_secret_key", out_config->device_secret_key) != 0) {
        return -1;
    }
    return 0;
}

static char *build_client_id(const char *device_id)
{
    size_t device_id_len = strlen(device_id);
    size_t suffix_len = strlen(kClientIdSuffix);
    char *client_id;

    if (device_id_len > SIZE_MAX - suffix_len - 1U) {
        return NULL;
    }

    client_id = (char *)malloc(device_id_len + suffix_len + 1U);
    if (client_id == NULL) {
        return NULL;
    }
    memcpy(client_id, device_id, device_id_len);
    memcpy(client_id + device_id_len, kClientIdSuffix, suffix_len + 1U);
    return client_id;
}

static int make_deadline_ms(uint32_t timeout_ms, struct timespec *out_deadline)
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

static void session_finalize(device_demo_session_t *session, const char *reason)
{
    int already_finalized = 0;

    pthread_mutex_lock(&session->mutex);
    already_finalized = session->finalized;
    if (!already_finalized) {
        session->finalized = 1;
    }
    pthread_mutex_unlock(&session->mutex);
    if (already_finalized) {
        return;
    }

    pthread_mutex_lock(&g_app.mutex);
    if (g_app.active_session == session) {
        g_app.active_session = NULL;
    }
    if (g_app.live_session_count > 0) {
        g_app.live_session_count -= 1;
    }
    pthread_cond_broadcast(&g_app.cond);
    pthread_mutex_unlock(&g_app.mutex);

    device_demo_streamer_destroy(session->streamer);
    pthread_mutex_destroy(&session->mutex);
    log_message(stdout,
                "connection resources released: hconn=%p (%s)",
                (void *)session->hconn,
                reason);
    free(session);
}

typedef struct {
    device_demo_session_t *session;
} disconnect_task_t;

static void *disconnect_worker_main(void *opaque)
{
    disconnect_task_t *task = (disconnect_task_t *)opaque;
    device_demo_session_t *session = task->session;
    int disconnect_result;

    free(task);
    device_demo_streamer_stop(session->streamer);
    disconnect_result = TiRtcDisconnect(session->hconn);
    if (disconnect_result != 0) {
        log_message(stderr,
                    "TiRtcDisconnect failed: %s",
                    TiRtcGetErrorStr(disconnect_result));
        session_finalize(session, "disconnect failure fallback");
    }
    return NULL;
}

static void schedule_session_disconnect(device_demo_session_t *session,
                                        const char *reason)
{
    disconnect_task_t *task;
    pthread_t thread;

    pthread_mutex_lock(&session->mutex);
    if (session->disconnect_scheduled || session->finalized) {
        pthread_mutex_unlock(&session->mutex);
        return;
    }
    session->disconnect_scheduled = 1;
    pthread_mutex_unlock(&session->mutex);

    task = (disconnect_task_t *)calloc(1, sizeof(*task));
    if (task == NULL) {
        log_message(stderr, "failed to allocate disconnect task for %s", reason);
        session_finalize(session, "disconnect task allocation failure");
        return;
    }
    task->session = session;
    if (pthread_create(&thread, NULL, disconnect_worker_main, task) != 0) {
        log_message(stderr, "failed to create disconnect worker for %s", reason);
        free(task);
        session_finalize(session, "disconnect worker creation failure");
        return;
    }
    pthread_detach(thread);
    log_message(stdout,
                "connection cleanup scheduled: hconn=%p (%s)",
                (void *)session->hconn,
                reason);
}

typedef struct {
    tirtc_conn_t hconn;
} raw_disconnect_task_t;

static void *raw_disconnect_worker_main(void *opaque)
{
    raw_disconnect_task_t *task = (raw_disconnect_task_t *)opaque;
    int disconnect_result = TiRtcDisconnect(task->hconn);

    if (disconnect_result != 0) {
        log_message(stderr,
                    "TiRtcDisconnect failed for untracked connection: %s",
                    TiRtcGetErrorStr(disconnect_result));
    }
    free(task);
    return NULL;
}

static void schedule_raw_disconnect(tirtc_conn_t hconn)
{
    raw_disconnect_task_t *task = (raw_disconnect_task_t *)calloc(1, sizeof(*task));
    pthread_t thread;

    if (task == NULL) {
        log_message(stderr, "failed to allocate raw disconnect task");
        return;
    }
    task->hconn = hconn;
    if (pthread_create(&thread, NULL, raw_disconnect_worker_main, task) != 0) {
        log_message(stderr, "failed to create raw disconnect worker");
        free(task);
        return;
    }
    pthread_detach(thread);
}

static device_demo_session_t *create_session(tirtc_conn_t hconn)
{
    device_demo_session_t *session = (device_demo_session_t *)calloc(1, sizeof(*session));
    int set_user_data_result;

    if (session == NULL) {
        log_message(stderr, "failed to allocate connection session");
        return NULL;
    }
    session->hconn = hconn;
    pthread_mutex_init(&session->mutex, NULL);
    if (device_demo_streamer_create(&session->streamer, hconn) != 0) {
        pthread_mutex_destroy(&session->mutex);
        free(session);
        log_message(stderr, "failed to create streamer for accepted connection");
        return NULL;
    }

    set_user_data_result = TiRtcConnSetUserData(hconn, session);
    if (set_user_data_result != 0) {
        log_message(stderr,
                    "TiRtcConnSetUserData failed: %s",
                    TiRtcGetErrorStr(set_user_data_result));
        device_demo_streamer_destroy(session->streamer);
        pthread_mutex_destroy(&session->mutex);
        free(session);
        return NULL;
    }
    return session;
}

static void on_event(int event, const void *data, int len)
{
    (void)data;
    (void)len;

    if (event == TIRTC_EVENT_SYS_STARTED) {
        pthread_mutex_lock(&g_app.mutex);
        g_app.sdk_started = 1;
        pthread_cond_broadcast(&g_app.cond);
        pthread_mutex_unlock(&g_app.mutex);
        log_message(stdout, "TiRTC started, waiting for client connections");
        return;
    }
    if (event == TIRTC_EVENT_SYS_STOPPED) {
        pthread_mutex_lock(&g_app.mutex);
        g_app.sdk_stopped = 1;
        pthread_cond_broadcast(&g_app.cond);
        pthread_mutex_unlock(&g_app.mutex);
        log_message(stdout, "TiRTC stopped");
        return;
    }
    if (event == TIRTC_EVENT_ACCESS_HIJACKING) {
        log_message(stderr, "warning: endpoint access may be hijacked");
        return;
    }

    log_message(stdout, "received SDK event=%d", event);
}

static void on_conn_accepted(tirtc_conn_t hconn)
{
    device_demo_session_t *previous_session;
    device_demo_session_t *session;

    log_message(stdout, "client connection accepted: hconn=%p", (void *)hconn);
    session = create_session(hconn);
    if (session == NULL) {
        schedule_raw_disconnect(hconn);
        return;
    }

    pthread_mutex_lock(&g_app.mutex);
    previous_session = g_app.active_session;
    g_app.active_session = session;
    g_app.live_session_count += 1;
    pthread_cond_broadcast(&g_app.cond);
    pthread_mutex_unlock(&g_app.mutex);

    device_demo_streamer_set_streaming_enabled(session->streamer, 1);
    log_message(stdout,
                "streaming enabled for accepted client: hconn=%p",
                (void *)hconn);
    if (previous_session != NULL && previous_session != session) {
        log_message(stdout, "replacing previous active connection");
        schedule_session_disconnect(previous_session, "connection replacement");
    }
}

static void on_conn_error(tirtc_conn_t hconn, int error)
{
    device_demo_session_t *session =
        (device_demo_session_t *)TiRtcConnGetUserData(hconn);

    log_message(stdout,
                "connection ended: hconn=%p reason=%s",
                (void *)hconn,
                TiRtcGetErrorStr(error));
    if (session == NULL) {
        return;
    }
    schedule_session_disconnect(session, "connection error");
}

static void on_disconnected(tirtc_conn_t hconn)
{
    device_demo_session_t *session =
        (device_demo_session_t *)TiRtcConnGetUserData(hconn);

    if (session == NULL) {
        return;
    }
    session_finalize(session, "on_disconnected");
}

static int on_subscribe_video(tirtc_conn_t hconn, uint8_t stream_id)
{
    device_demo_session_t *session =
        (device_demo_session_t *)TiRtcConnGetUserData(hconn);

    if (stream_id != kVideoStreamId) {
        log_message(stderr, "rejecting unexpected video stream_id=%u", stream_id);
        return -1;
    }
    if (session == NULL) {
        return -1;
    }

    log_message(stdout, "video subscribe callback received: stream_id=%u", stream_id);
    return 0;
}

static void on_unsubscribe_video(tirtc_conn_t hconn, uint8_t stream_id)
{
    device_demo_session_t *session =
        (device_demo_session_t *)TiRtcConnGetUserData(hconn);

    if (session == NULL || stream_id != kVideoStreamId) {
        return;
    }
    log_message(stdout, "video unsubscribe callback received: stream_id=%u", stream_id);
}

static int on_subscribe_audio(tirtc_conn_t hconn, uint8_t stream_id)
{
    device_demo_session_t *session =
        (device_demo_session_t *)TiRtcConnGetUserData(hconn);

    if (stream_id != kAudioStreamId) {
        log_message(stderr, "rejecting unexpected audio stream_id=%u", stream_id);
        return -1;
    }
    if (session == NULL) {
        return -1;
    }

    log_message(stdout, "audio subscribe callback received: stream_id=%u", stream_id);
    return 0;
}

static void on_unsubscribe_audio(tirtc_conn_t hconn, uint8_t stream_id)
{
    device_demo_session_t *session =
        (device_demo_session_t *)TiRtcConnGetUserData(hconn);

    if (session == NULL || stream_id != kAudioStreamId) {
        return;
    }
    log_message(stdout, "audio unsubscribe callback received: stream_id=%u", stream_id);
}

static void on_request_key_frame(tirtc_conn_t hconn, uint8_t stream_id)
{
    device_demo_session_t *session =
        (device_demo_session_t *)TiRtcConnGetUserData(hconn);

    if (session == NULL || stream_id != kVideoStreamId) {
        return;
    }
    device_demo_streamer_request_key_frame(session->streamer);
    log_message(stdout, "remote requested I frame: stream_id=%u", stream_id);
}

static int wait_for_sdk_stop(void)
{
    struct timespec deadline;

    if (make_deadline_ms(kSdkStopTimeoutMs, &deadline) != 0) {
        return -1;
    }

    pthread_mutex_lock(&g_app.mutex);
    while (!g_app.sdk_stopped) {
        int wait_result = pthread_cond_timedwait(&g_app.cond, &g_app.mutex, &deadline);
        if (wait_result == ETIMEDOUT) {
            pthread_mutex_unlock(&g_app.mutex);
            return -1;
        }
    }
    pthread_mutex_unlock(&g_app.mutex);
    return 0;
}

static int wait_for_connection_cleanup(void)
{
    struct timespec deadline;

    if (make_deadline_ms(kConnectionCleanupTimeoutMs, &deadline) != 0) {
        return -1;
    }

    pthread_mutex_lock(&g_app.mutex);
    while (g_app.live_session_count > 0) {
        int wait_result = pthread_cond_timedwait(&g_app.cond, &g_app.mutex, &deadline);
        if (wait_result == ETIMEDOUT) {
            pthread_mutex_unlock(&g_app.mutex);
            return -1;
        }
    }
    pthread_mutex_unlock(&g_app.mutex);
    return 0;
}

int main(int argc, char **argv)
{
    device_demo_config_t config;
    device_demo_session_t *session_to_stop = NULL;
    char *client_id = NULL;
    static const TIRTCCALLBACKS callbacks = {
        .on_event = on_event,
        .on_conn_accepted = on_conn_accepted,
        .on_conn_error = on_conn_error,
        .on_disconnected = on_disconnected,
        .on_request_key_frame = on_request_key_frame,
        .on_subscribe_video = on_subscribe_video,
        .on_unsubscribe_video = on_unsubscribe_video,
        .on_subscribe_audio = on_subscribe_audio,
        .on_unsubscribe_audio = on_unsubscribe_audio,
    };

    int parse_result = parse_arguments(argc, argv, &config);
    if (parse_result > 0) {
        return 0;
    }
    if (parse_result < 0) {
        print_usage(argv[0]);
        return 1;
    }

    log_message(stdout, "validating startup inputs");
    if (validate_required_file(kAudioFilePath) != 0 ||
        validate_required_file(kVideoFilePath) != 0) {
        return 1;
    }
    client_id = build_client_id(config.device_id);
    if (client_id == NULL) {
        log_message(stderr, "failed to build client id");
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    log_message(stdout, "TiRTC version: %s", TiRtcGetVersion());
    if (TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER,
                       &kSdkMaxSendBufferBytes,
                       sizeof(kSdkMaxSendBufferBytes)) != 0) {
        log_message(stderr, "failed to set SDK max send buffer");
        free(client_id);
        return 1;
    }
    log_message(stdout, "SDK max send buffer set to %u bytes", kSdkMaxSendBufferBytes);
    if (TiRtcInit() != 0) {
        log_message(stderr, "TiRtcInit failed");
        free(client_id);
        return 1;
    }
    TiRtcLogConfig(1, NULL, 0);
    TiRtcLogSetLevel(config.log_level);
    log_message(stdout, "TiRTC log level: %d", config.log_level);

    if (TiRtcSetOption(TIRTC_OPT_DEVICE_SECRET_KEY,
                       config.device_secret_key,
                       (uint32_t)strlen(config.device_secret_key)) != 0) {
        log_message(stderr, "failed to set device secret key");
        TiRtcUninit();
        free(client_id);
        return 1;
    }
    if (TiRtcSetOption(TIRTC_OPT_CLIENT_ID,
                       client_id,
                       (uint32_t)strlen(client_id)) != 0) {
        log_message(stderr, "failed to set client id");
        TiRtcUninit();
        free(client_id);
        return 1;
    }
    log_message(stdout, "using client_id=%s", client_id);

    if (config.endpoint != NULL && config.endpoint[0] != '\0') {
        if (TiRtcSetOption(TIRTC_OPT_SERVICE_ENDPOINT,
                           config.endpoint,
                           (uint32_t)strlen(config.endpoint)) != 0) {
            log_message(stderr, "failed to set service endpoint");
            TiRtcUninit();
            free(client_id);
            return 1;
        }
        log_message(stdout, "using endpoint override: %s", config.endpoint);
    }

    log_message(stdout,
                "starting device demo for device_id=%s",
                config.device_id);
    if (TiRtcStart(config.device_id, &callbacks) != 0) {
        log_message(stderr, "TiRtcStart failed");
        TiRtcUninit();
        free(client_id);
        return 1;
    }

    while (!g_should_exit) {
        sleep_for_us(100000ULL);
    }

    log_message(stdout, "shutdown requested");
    pthread_mutex_lock(&g_app.mutex);
    session_to_stop = g_app.active_session;
    g_app.active_session = NULL;
    pthread_mutex_unlock(&g_app.mutex);
    if (session_to_stop != NULL) {
        schedule_session_disconnect(session_to_stop, "process shutdown");
    }

    if (TiRtcStop() != 0) {
        log_message(stderr, "TiRtcStop returned failure");
    }
    if (wait_for_sdk_stop() != 0) {
        log_message(stderr, "timed out waiting for TiRTC stop event");
    }
    if (wait_for_connection_cleanup() != 0) {
        log_message(stderr, "timed out waiting for connection cleanup");
    }

    TiRtcUninit();
    free(client_id);
    log_message(stdout, "process exit");
    return 0;
}
