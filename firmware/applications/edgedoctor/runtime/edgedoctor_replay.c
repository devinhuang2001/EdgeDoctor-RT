#include "edgedoctor_replay.h"

#include <dfs_posix.h>
#include <fcntl.h>
#include <rtthread.h>
#include <string.h>
#include <unistd.h>

#define EDGEDOCTOR_REPLAY_IO_BYTES 1152U

static int read_exact(int descriptor, void *destination, size_t bytes)
{
    uint8_t *cursor = (uint8_t *)destination;
    size_t received = 0U;
    while (received < bytes)
    {
        int count = read(descriptor, cursor + received, bytes - received);
        if (count <= 0)
        {
            return 0;
        }
        received += (size_t)count;
    }
    return 1;
}

static int validate_stream(int descriptor,
                           const edgedoctor_edgepack_view_t *view,
                           uint8_t *buffer)
{
    uint32_t remaining = view->header.payload_bytes;
    uint32_t crc = edgedoctor_crc32_init();
    if (lseek(descriptor, EDGEDOCTOR_EDGEPACK_HEADER_BYTES, SEEK_SET) < 0)
    {
        return EDGEDOCTOR_REPLAY_IO;
    }
    while (remaining > 0U)
    {
        size_t request = (remaining < EDGEDOCTOR_REPLAY_IO_BYTES)
            ? remaining : EDGEDOCTOR_REPLAY_IO_BYTES;
        if (!read_exact(descriptor, buffer, request))
        {
            return EDGEDOCTOR_REPLAY_IO;
        }
        crc = edgedoctor_crc32_update(crc, buffer, request);
        remaining -= (uint32_t)request;
    }
    return (edgedoctor_crc32_finish(crc) == view->header.payload_crc32)
        ? EDGEDOCTOR_REPLAY_OK : EDGEDOCTOR_REPLAY_FORMAT;
}

int edgedoctor_replay_file(const char *path,
                           int real_time,
                           edgedoctor_replay_callback_t callback,
                           void *context)
{
    uint8_t header_bytes[EDGEDOCTOR_EDGEPACK_HEADER_BYTES];
    uint8_t io_buffer[EDGEDOCTOR_REPLAY_IO_BYTES];
    edgedoctor_edgepack_view_t view;
    edgedoctor_chunk_t chunk;
    off_t file_bytes;
    uint32_t index;
    int descriptor;
    int status;

    if ((path == NULL) || (callback == NULL))
    {
        return EDGEDOCTOR_REPLAY_ARGUMENT;
    }
    descriptor = open(path, O_RDONLY, 0);
    if (descriptor < 0)
    {
        return EDGEDOCTOR_REPLAY_OPEN;
    }
    if (!read_exact(descriptor, header_bytes, sizeof(header_bytes)) ||
        (edgedoctor_edgepack_parse_header(header_bytes, sizeof(header_bytes), &view) !=
         EDGEDOCTOR_EDGEPACK_OK))
    {
        close(descriptor);
        return EDGEDOCTOR_REPLAY_FORMAT;
    }
    file_bytes = lseek(descriptor, 0, SEEK_END);
    if ((file_bytes < 0) || ((size_t)file_bytes != view.file_bytes))
    {
        close(descriptor);
        return EDGEDOCTOR_REPLAY_FORMAT;
    }
    if ((view.header.audio_rate != EDGEDOCTOR_AUDIO_RATE_HZ) ||
        (view.header.vibration_rate != EDGEDOCTOR_VIBRATION_RATE_HZ) ||
        (view.header.chunk_ms != EDGEDOCTOR_CHUNK_MS) ||
        (view.header.vibration_channels != EDGEDOCTOR_VIBRATION_CHANNELS) ||
        (view.chunk_bytes != EDGEDOCTOR_REPLAY_IO_BYTES))
    {
        close(descriptor);
        return EDGEDOCTOR_REPLAY_UNSUPPORTED;
    }
    status = validate_stream(descriptor, &view, io_buffer);
    if (status != EDGEDOCTOR_REPLAY_OK)
    {
        close(descriptor);
        return status;
    }
    if (lseek(descriptor, EDGEDOCTOR_EDGEPACK_HEADER_BYTES, SEEK_SET) < 0)
    {
        close(descriptor);
        return EDGEDOCTOR_REPLAY_IO;
    }
    memset(&chunk, 0, sizeof(chunk));
    chunk.audio_count = EDGEDOCTOR_AUDIO_CHUNK_SAMPLES;
    chunk.vibration_frames = EDGEDOCTOR_VIBRATION_CHUNK_FRAMES;
    for (index = 0U; index < view.chunk_count; ++index)
    {
        if (!read_exact(descriptor, io_buffer, view.chunk_bytes))
        {
            close(descriptor);
            return EDGEDOCTOR_REPLAY_IO;
        }
        chunk.frame_id = index;
        chunk.timestamp_us = ((uint64_t)index + 1U) * EDGEDOCTOR_CHUNK_MS * 1000U;
        memcpy(chunk.audio, io_buffer, sizeof(chunk.audio));
        memcpy(chunk.vibration, io_buffer + sizeof(chunk.audio), sizeof(chunk.vibration));
        if (callback(&view.header, &chunk, context) != 0)
        {
            close(descriptor);
            return EDGEDOCTOR_REPLAY_CALLBACK;
        }
        if (real_time != 0)
        {
            rt_thread_mdelay(EDGEDOCTOR_CHUNK_MS);
        }
    }
    close(descriptor);
    return EDGEDOCTOR_REPLAY_OK;
}
