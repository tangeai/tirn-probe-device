#ifndef OGG_OPUS_H
#define OGG_OPUS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint8_t *data;
    size_t len;
    uint32_t duration_samples_48k;
} ogg_opus_packet_t;

typedef struct {
    ogg_opus_packet_t *packets;
    size_t len;
    size_t cap;
    uint8_t channels;
    uint16_t pre_skip;
    uint32_t input_sample_rate;
} ogg_opus_file_t;

typedef struct {
    FILE *stream;
    uint32_t serial;
    uint32_t sequence;
    uint8_t *pending_data;
    size_t pending_len;
    uint64_t pending_granule;
    int has_pending;
} ogg_opus_writer_t;

int ogg_opus_read_file(const char *path, ogg_opus_file_t *out, char *error, size_t error_size);
void ogg_opus_file_free(ogg_opus_file_t *file);
uint32_t ogg_opus_packet_duration_samples_48k(const void *data, size_t len);

int ogg_opus_writer_open(ogg_opus_writer_t *writer,
                         const char *path,
                         uint8_t channels,
                         uint32_t input_sample_rate);
int ogg_opus_writer_write(ogg_opus_writer_t *writer,
                          const void *packet,
                          size_t packet_len,
                          uint64_t granule);
int ogg_opus_writer_close(ogg_opus_writer_t *writer);

#endif
