#include "ogg_opus.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void write_le64(uint8_t *p, uint64_t value)
{
    size_t i;
    for (i = 0; i < 8; ++i) {
        p[i] = (uint8_t)(value >> (i * 8U));
    }
}

static void set_error(char *error, size_t error_size, const char *fmt, ...)
{
    va_list args;
    if (error == NULL || error_size == 0) {
        return;
    }
    va_start(args, fmt);
    (void)vsnprintf(error, error_size, fmt, args);
    va_end(args);
}

uint32_t ogg_opus_packet_duration_samples_48k(const void *packet, size_t len)
{
    const uint8_t *data = (const uint8_t *)packet;
    uint32_t samples_per_frame;
    uint32_t frames;
    uint8_t toc;

    if (len == 0) {
        return 0;
    }
    toc = data[0];
    if (toc & 0x80U) {
        samples_per_frame = 48000U << ((toc >> 3U) & 0x3U);
        samples_per_frame /= 400U;
    } else if ((toc & 0x60U) == 0x60U) {
        samples_per_frame = (toc & 0x08U) ? 480U : 960U;
    } else {
        samples_per_frame = 48000U << ((toc >> 3U) & 0x3U);
        samples_per_frame /= 100U;
    }

    switch (toc & 0x3U) {
    case 0:
        frames = 1;
        break;
    case 1:
    case 2:
        frames = 2;
        break;
    default:
        if (len < 2) {
            return 0;
        }
        frames = data[1] & 0x3fU;
        break;
    }
    if (frames == 0 || samples_per_frame * frames > 5760U) {
        return 0;
    }
    return samples_per_frame * frames;
}

static int append_packet(ogg_opus_file_t *file, const uint8_t *data, size_t len)
{
    ogg_opus_packet_t *packet;
    uint32_t duration;

    duration = ogg_opus_packet_duration_samples_48k(data, len);
    if (duration == 0) {
        return -1;
    }
    if (file->len == file->cap) {
        size_t new_cap = file->cap == 0 ? 256U : file->cap * 2U;
        ogg_opus_packet_t *new_packets =
            (ogg_opus_packet_t *)realloc(file->packets, new_cap * sizeof(*new_packets));
        if (new_packets == NULL) {
            return -1;
        }
        file->packets = new_packets;
        file->cap = new_cap;
    }
    packet = &file->packets[file->len];
    memset(packet, 0, sizeof(*packet));
    packet->data = (uint8_t *)malloc(len);
    if (packet->data == NULL) {
        return -1;
    }
    memcpy(packet->data, data, len);
    packet->len = len;
    packet->duration_samples_48k = duration;
    file->len++;
    return 0;
}

int ogg_opus_read_file(const char *path, ogg_opus_file_t *out, char *error, size_t error_size)
{
    FILE *stream;
    uint8_t header[27];
    uint8_t *packet = NULL;
    size_t packet_len = 0;
    size_t packet_cap = 0;
    unsigned completed_packets = 0;
    int result = -1;

    memset(out, 0, sizeof(*out));
    stream = fopen(path, "rb");
    if (stream == NULL) {
        set_error(error, error_size, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    while (fread(header, 1, sizeof(header), stream) == sizeof(header)) {
        uint8_t lacing[255];
        size_t segment_count;
        size_t i;

        if (memcmp(header, "OggS", 4) != 0 || header[4] != 0) {
            set_error(error, error_size, "invalid Ogg page");
            goto done;
        }
        segment_count = header[26];
        if (fread(lacing, 1, segment_count, stream) != segment_count) {
            set_error(error, error_size, "truncated Ogg lacing table");
            goto done;
        }
        for (i = 0; i < segment_count; ++i) {
            size_t bytes = lacing[i];
            if (packet_len + bytes > packet_cap) {
                size_t new_cap = packet_cap == 0 ? 1024U : packet_cap;
                uint8_t *new_packet;
                while (new_cap < packet_len + bytes) {
                    new_cap *= 2U;
                }
                new_packet = (uint8_t *)realloc(packet, new_cap);
                if (new_packet == NULL) {
                    set_error(error, error_size, "out of memory reading Ogg packet");
                    goto done;
                }
                packet = new_packet;
                packet_cap = new_cap;
            }
            if (bytes > 0 && fread(packet + packet_len, 1, bytes, stream) != bytes) {
                set_error(error, error_size, "truncated Ogg packet");
                goto done;
            }
            packet_len += bytes;
            if (bytes < 255U) {
                if (completed_packets == 0) {
                    if (packet_len < 19 || memcmp(packet, "OpusHead", 8) != 0) {
                        set_error(error, error_size, "missing OpusHead");
                        goto done;
                    }
                    out->channels = packet[9];
                    out->pre_skip = read_le16(packet + 10);
                    out->input_sample_rate = read_le32(packet + 12);
                    if (out->channels == 0 || out->channels > 2) {
                        set_error(error, error_size, "unsupported Opus channels: %u", out->channels);
                        goto done;
                    }
                } else if (completed_packets == 1) {
                    if (packet_len < 8 || memcmp(packet, "OpusTags", 8) != 0) {
                        set_error(error, error_size, "missing OpusTags");
                        goto done;
                    }
                } else if (append_packet(out, packet, packet_len) != 0) {
                    set_error(error, error_size, "invalid or unsupported Opus packet");
                    goto done;
                }
                completed_packets++;
                packet_len = 0;
            }
        }
    }
    if (ferror(stream)) {
        set_error(error, error_size, "failed reading %s", path);
        goto done;
    }
    if (packet_len != 0 || completed_packets < 3 || out->len == 0) {
        set_error(error, error_size, "incomplete or empty Ogg Opus file");
        goto done;
    }
    result = 0;

done:
    free(packet);
    fclose(stream);
    if (result != 0) {
        ogg_opus_file_free(out);
    }
    return result;
}

void ogg_opus_file_free(ogg_opus_file_t *file)
{
    size_t i;
    for (i = 0; i < file->len; ++i) {
        free(file->packets[i].data);
    }
    free(file->packets);
    memset(file, 0, sizeof(*file));
}

static uint32_t ogg_crc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    size_t i;
    for (i = 0; i < len; ++i) {
        unsigned bit;
        crc ^= (uint32_t)data[i] << 24;
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04c11db7U : crc << 1;
        }
    }
    return crc;
}

static int write_page(ogg_opus_writer_t *writer,
                      const uint8_t *packet,
                      size_t packet_len,
                      uint8_t header_type,
                      uint64_t granule)
{
    size_t segments = packet_len / 255U + 1U;
    size_t page_len;
    uint8_t *page;
    size_t remaining;
    size_t i;
    uint32_t crc;

    if (segments > 255U) {
        return -1;
    }
    page_len = 27U + segments + packet_len;
    page = (uint8_t *)calloc(1, page_len);
    if (page == NULL) {
        return -1;
    }
    memcpy(page, "OggS", 4);
    page[4] = 0;
    page[5] = header_type;
    write_le64(page + 6, granule);
    write_le32(page + 14, writer->serial);
    write_le32(page + 18, writer->sequence++);
    page[26] = (uint8_t)segments;
    remaining = packet_len;
    for (i = 0; i < segments; ++i) {
        size_t segment_len = remaining > 255U ? 255U : remaining;
        page[27U + i] = (uint8_t)segment_len;
        remaining -= segment_len;
    }
    memcpy(page + 27U + segments, packet, packet_len);
    crc = ogg_crc(page, page_len);
    write_le32(page + 22, crc);
    if (fwrite(page, 1, page_len, writer->stream) != page_len) {
        free(page);
        return -1;
    }
    free(page);
    return 0;
}

int ogg_opus_writer_open(ogg_opus_writer_t *writer,
                         const char *path,
                         uint8_t channels,
                         uint32_t input_sample_rate)
{
    uint8_t head[19] = {'O','p','u','s','H','e','a','d',1,0,0,0,0,0,0,0,0,0,0};
    static const uint8_t tags[] = {
        'O','p','u','s','T','a','g','s',
        11,0,0,0,'a','c','c','e','l','-','p','r','o','b','e',
        0,0,0,0
    };

    memset(writer, 0, sizeof(*writer));
    writer->stream = fopen(path, "wb");
    if (writer->stream == NULL) {
        return -1;
    }
    writer->serial = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)writer;
    head[9] = channels;
    write_le16(head + 10, 0);
    write_le32(head + 12, input_sample_rate == 0 ? 48000U : input_sample_rate);
    if (write_page(writer, head, sizeof(head), 0x02U, 0) != 0 ||
        write_page(writer, tags, sizeof(tags), 0, 0) != 0) {
        ogg_opus_writer_close(writer);
        return -1;
    }
    return 0;
}

int ogg_opus_writer_write(ogg_opus_writer_t *writer,
                          const void *packet,
                          size_t packet_len,
                          uint64_t granule)
{
    uint8_t *copy;
    if (writer->stream == NULL || packet == NULL || packet_len == 0) {
        return -1;
    }
    if (writer->has_pending &&
        write_page(writer, writer->pending_data, writer->pending_len, 0, writer->pending_granule) != 0) {
        return -1;
    }
    copy = (uint8_t *)malloc(packet_len);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, packet, packet_len);
    free(writer->pending_data);
    writer->pending_data = copy;
    writer->pending_len = packet_len;
    writer->pending_granule = granule;
    writer->has_pending = 1;
    return 0;
}

int ogg_opus_writer_close(ogg_opus_writer_t *writer)
{
    int result = 0;
    if (writer->stream != NULL && writer->has_pending &&
        write_page(writer, writer->pending_data, writer->pending_len, 0x04U,
                   writer->pending_granule) != 0) {
        result = -1;
    }
    free(writer->pending_data);
    writer->pending_data = NULL;
    writer->has_pending = 0;
    if (writer->stream != NULL && fclose(writer->stream) != 0) {
        result = -1;
    }
    writer->stream = NULL;
    return result;
}
