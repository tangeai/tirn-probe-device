#ifndef DEVICE_DEMO_STREAMER_H_
#define DEVICE_DEMO_STREAMER_H_

#include <stddef.h>
#include <stdint.h>

#include "tiRTC.h"

static const uint8_t kAudioStreamId = 10;
static const uint8_t kVideoStreamId = 11;
static const uint8_t kAudioSampleSpec = TIRTC_AUDIOSAMPLE_8K16B1C;
static const size_t kAudioPacketBytes = 320;
static const uint32_t kAudioPacketDurationMs = 40;
static const uint32_t kVideoFps = 15;
static const uint32_t kSdkMaxSendBufferBytes = 1024u * 1024u;
static const uint32_t kHeartbeatIntervalMs = 5000;
static const uint32_t kLoopPollIntervalMs = 5;
static const uint32_t kConnectionCleanupTimeoutMs = 5000;
static const uint32_t kSdkStopTimeoutMs = 5000;
static const size_t kMaxPathBytes = 1024;
static const char kAudioFilePath[] = "assets/audio.g711a";
static const char kVideoFilePath[] = "assets/video.h264";

typedef struct device_demo_streamer device_demo_streamer_t;

int device_demo_streamer_create(device_demo_streamer_t **out_streamer,
                                tirtc_conn_t hconn);
void device_demo_streamer_stop(device_demo_streamer_t *streamer);
void device_demo_streamer_destroy(device_demo_streamer_t *streamer);
void device_demo_streamer_set_streaming_enabled(device_demo_streamer_t *streamer,
                                                int enabled);
void device_demo_streamer_request_key_frame(device_demo_streamer_t *streamer);

#endif
