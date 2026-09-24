#include "music_lyrics.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage.h"

#define MUSIC_LYRICS_MAX_FRAME_BYTES (512U * 1024U)

static const char *TAG = "music_lyrics";

static uint32_t lyrics_be24(const uint8_t *data)
{
    return ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
}

static uint32_t lyrics_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static uint32_t lyrics_synchsafe32(const uint8_t *data)
{
    return ((uint32_t)(data[0] & 0x7f) << 21) |
           ((uint32_t)(data[1] & 0x7f) << 14) |
           ((uint32_t)(data[2] & 0x7f) << 7) |
           (data[3] & 0x7f);
}

static bool lyrics_frame_id_valid(const uint8_t *id, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9')))
        {
            return false;
        }
    }
    return true;
}

static size_t lyrics_unsync_remove(uint8_t *data, size_t size)
{
    size_t write_pos = 0;
    for (size_t read_pos = 0; read_pos < size; read_pos++)
    {
        data[write_pos++] = data[read_pos];
        if (data[read_pos] == 0xff && read_pos + 1 < size && data[read_pos + 1] == 0x00)
        {
            read_pos++;
        }
    }
    return write_pos;
}

static bool lyrics_find_text_end(const uint8_t *data, size_t size, uint8_t encoding,
                                 size_t *text_size, size_t *terminator_size)
{
    if (encoding == 1 || encoding == 2)
    {
        for (size_t i = 0; i + 1 < size; i += 2)
        {
            if (data[i] == 0 && data[i + 1] == 0)
            {
                *text_size = i;
                *terminator_size = 2;
                return true;
            }
        }
    }
    else
    {
        const uint8_t *end = memchr(data, 0, size);
        if (end != NULL)
        {
            *text_size = (size_t)(end - data);
            *terminator_size = 1;
            return true;
        }
    }
    return false;
}

static bool lyrics_append_utf8(char *dst, size_t dst_size, size_t *written, uint32_t codepoint)
{
    uint8_t encoded[4];
    size_t count = 0;
    if (codepoint <= 0x7f)
    {
        encoded[count++] = (uint8_t)codepoint;
    }
    else if (codepoint <= 0x7ff)
    {
        encoded[count++] = 0xc0 | (uint8_t)(codepoint >> 6);
        encoded[count++] = 0x80 | (uint8_t)(codepoint & 0x3f);
    }
    else if (codepoint <= 0xffff)
    {
        encoded[count++] = 0xe0 | (uint8_t)(codepoint >> 12);
        encoded[count++] = 0x80 | (uint8_t)((codepoint >> 6) & 0x3f);
        encoded[count++] = 0x80 | (uint8_t)(codepoint & 0x3f);
    }
    else if (codepoint <= 0x10ffff)
    {
        encoded[count++] = 0xf0 | (uint8_t)(codepoint >> 18);
        encoded[count++] = 0x80 | (uint8_t)((codepoint >> 12) & 0x3f);
        encoded[count++] = 0x80 | (uint8_t)((codepoint >> 6) & 0x3f);
        encoded[count++] = 0x80 | (uint8_t)(codepoint & 0x3f);
    }
    else
    {
        return true;
    }

    if (*written + count >= dst_size)
    {
        return false;
    }
    memcpy(dst + *written, encoded, count);
    *written += count;
    return true;
}

static void lyrics_decode_text(const uint8_t *data, size_t size, uint8_t encoding,
                               char *dst, size_t dst_size)
{
    size_t written = 0;
    if (dst_size == 0)
    {
        return;
    }

    if (encoding == 0)
    {
        for (size_t i = 0; i < size; i++)
        {
            if (!lyrics_append_utf8(dst, dst_size, &written, data[i]))
            {
                break;
            }
        }
    }
    else if (encoding == 3)
    {
        size_t copy_size = size < dst_size - 1 ? size : dst_size - 1;
        if (copy_size < size)
        {
            size_t character_start = copy_size;
            while (character_start > 0 && (data[character_start] & 0xc0) == 0x80)
            {
                character_start--;
            }
            if (character_start < copy_size)
            {
                copy_size = character_start;
            }
        }
        memcpy(dst, data, copy_size);
        written = copy_size;
    }
    else
    {
        bool little_endian = false;
        size_t pos = 0;
        if (encoding == 1 && size >= 2)
        {
            if (data[0] == 0xff && data[1] == 0xfe)
            {
                little_endian = true;
                pos = 2;
            }
            else if (data[0] == 0xfe && data[1] == 0xff)
            {
                pos = 2;
            }
        }

        while (pos + 1 < size)
        {
            uint16_t first = little_endian ? ((uint16_t)data[pos + 1] << 8) | data[pos]
                                           : ((uint16_t)data[pos] << 8) | data[pos + 1];
            pos += 2;
            uint32_t codepoint = first;
            if (first >= 0xd800 && first <= 0xdbff && pos + 1 < size)
            {
                uint16_t second = little_endian ? ((uint16_t)data[pos + 1] << 8) | data[pos]
                                                : ((uint16_t)data[pos] << 8) | data[pos + 1];
                if (second >= 0xdc00 && second <= 0xdfff)
                {
                    codepoint = 0x10000U + (((uint32_t)first - 0xd800U) << 10) +
                                ((uint32_t)second - 0xdc00U);
                    pos += 2;
                }
            }
            if (!lyrics_append_utf8(dst, dst_size, &written, codepoint))
            {
                break;
            }
        }
    }
    dst[written] = '\0';
}

static void lyrics_sort_lines_stable(music_lyrics_line_t *lines, int count)
{
    for (int i = 1; i < count; i++)
    {
        music_lyrics_line_t current = lines[i];
        int position = i;
        while (position > 0 && lines[position - 1].time_ms > current.time_ms)
        {
            lines[position] = lines[position - 1];
            position--;
        }
        lines[position] = current;
    }
}

static bool lyrics_parse_lrc_timestamp(const char **cursor, uint32_t *out_ms)
{
    const char *p = *cursor;
    if (*p++ != '[' || !isdigit((unsigned char)*p))
    {
        return false;
    }

    uint32_t minutes = 0;
    while (isdigit((unsigned char)*p))
    {
        minutes = minutes * 10U + (uint32_t)(*p++ - '0');
    }
    if (*p++ != ':' || !isdigit((unsigned char)*p))
    {
        return false;
    }

    uint32_t seconds = 0;
    while (isdigit((unsigned char)*p))
    {
        seconds = seconds * 10U + (uint32_t)(*p++ - '0');
    }

    uint32_t fraction_ms = 0;
    if (*p == '.' || *p == ':')
    {
        p++;
        int digits = 0;
        while (isdigit((unsigned char)*p))
        {
            if (digits < 3)
            {
                fraction_ms = fraction_ms * 10U + (uint32_t)(*p - '0');
            }
            digits++;
            p++;
        }
        if (digits == 1)
        {
            fraction_ms *= 100U;
        }
        else if (digits == 2)
        {
            fraction_ms *= 10U;
        }
    }

    if (*p++ != ']')
    {
        return false;
    }
    *out_ms = (minutes * 60U + seconds) * 1000U + fraction_ms;
    *cursor = p;
    return true;
}

static char *lyrics_trim(char *text)
{
    while (*text && isspace((unsigned char)*text))
    {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
    {
        *--end = '\0';
    }
    return text;
}

static void lyrics_copy_utf8(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0)
    {
        return;
    }
    size_t length = strlen(src);
    if (length >= dst_size)
    {
        length = dst_size - 1;
        while (length > 0 && (((uint8_t)src[length] & 0xc0U) == 0x80U))
        {
            length--;
        }
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static int lyrics_parse_lrc_text(char *text, music_lyrics_line_t *lines, int max_lines)
{
    int count = 0;
    char *line = text;
    while (line != NULL && *line != '\0' && count < max_lines)
    {
        char *next_line = strpbrk(line, "\r\n");
        if (next_line != NULL)
        {
            *next_line++ = '\0';
            while (*next_line == '\r' || *next_line == '\n')
            {
                next_line++;
            }
        }

        const char *cursor = line;
        uint32_t timestamps[8] = {0};
        int timestamp_count = 0;
        while (timestamp_count < (int)(sizeof(timestamps) / sizeof(timestamps[0])))
        {
            const char *next = cursor;
            if (!lyrics_parse_lrc_timestamp(&next, &timestamps[timestamp_count]))
            {
                break;
            }
            timestamp_count++;
            cursor = next;
        }

        char *lyric = lyrics_trim((char *)cursor);
        for (int i = 0; i < timestamp_count && lyric[0] != '\0' && count < max_lines; i++)
        {
            lines[count].time_ms = timestamps[i];
            lyrics_copy_utf8(lines[count].text, sizeof(lines[count].text), lyric);
            count++;
        }
        line = next_line;
    }

    lyrics_sort_lines_stable(lines, count);
    return count;
}

static int lyrics_parse_uslt(uint8_t *frame, size_t frame_size,
                             music_lyrics_line_t *lines, int max_lines)
{
    if (frame_size < 5 || max_lines <= 0 || frame[0] > 3)
    {
        return 0;
    }

    const uint8_t encoding = frame[0];
    size_t descriptor_size = 0;
    size_t terminator_size = 0;
    if (!lyrics_find_text_end(frame + 4, frame_size - 4, encoding,
                              &descriptor_size, &terminator_size))
    {
        return 0;
    }

    size_t lyrics_pos = 4 + descriptor_size + terminator_size;
    if (lyrics_pos >= frame_size)
    {
        return 0;
    }

    size_t encoded_size = frame_size - lyrics_pos;
    size_t decoded_size = encoded_size * 2U + 1U;
    char *decoded = heap_caps_malloc(decoded_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (decoded == NULL)
    {
        decoded = heap_caps_malloc(decoded_size, MALLOC_CAP_DEFAULT);
    }
    if (decoded == NULL)
    {
        return 0;
    }

    lyrics_decode_text(frame + lyrics_pos, encoded_size, encoding, decoded, decoded_size);
    int count = lyrics_parse_lrc_text(decoded, lines, max_lines);
    free(decoded);
    return count;
}

static int lyrics_parse_sylt(uint8_t *frame, size_t frame_size,
                             music_lyrics_line_t *lines, int max_lines)
{
    if (frame_size < 7 || max_lines <= 0)
    {
        return 0;
    }

    uint8_t encoding = frame[0];
    uint8_t timestamp_format = frame[4];
    if (encoding > 3 || timestamp_format != 1)
    {
        ESP_LOGW(TAG, "Unsupported SYLT encoding=%u timestamp_format=%u", encoding,
                 timestamp_format);
        return 0;
    }

    size_t descriptor_size = 0;
    size_t terminator_size = 0;
    if (!lyrics_find_text_end(frame + 6, frame_size - 6, encoding,
                              &descriptor_size, &terminator_size))
    {
        return 0;
    }

    size_t pos = 6 + descriptor_size + terminator_size;
    int count = 0;
    while (pos < frame_size && count < max_lines)
    {
        size_t text_size = 0;
        if (!lyrics_find_text_end(frame + pos, frame_size - pos, encoding,
                                  &text_size, &terminator_size))
        {
            break;
        }
        size_t timestamp_pos = pos + text_size + terminator_size;
        if (timestamp_pos + 4 > frame_size)
        {
            break;
        }

        if (text_size > 0)
        {
            lines[count].time_ms = lyrics_be32(frame + timestamp_pos);
            lyrics_decode_text(frame + pos, text_size, encoding, lines[count].text,
                               sizeof(lines[count].text));
            if (lines[count].text[0] != '\0')
            {
                count++;
            }
        }
        pos = timestamp_pos + 4;
    }

    if (count > 1)
    {
        lyrics_sort_lines_stable(lines, count);
    }
    return count;
}

static uint8_t *lyrics_load_frame_payload(FILE *file, long payload_pos,
                                          uint32_t frame_size, uint8_t version,
                                          uint8_t format_flags, bool tag_unsync,
                                          size_t *payload_size, size_t *payload_skip)
{
    uint8_t *frame = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame == NULL)
    {
        frame = heap_caps_malloc(frame_size, MALLOC_CAP_DEFAULT);
    }
    if (frame == NULL || fseek(file, payload_pos, SEEK_SET) != 0 ||
        fread(frame, 1, frame_size, file) != frame_size)
    {
        free(frame);
        return NULL;
    }

    *payload_size = frame_size;
    if (tag_unsync || (version == 4 && (format_flags & 0x02U) != 0))
    {
        *payload_size = lyrics_unsync_remove(frame, *payload_size);
    }

    *payload_skip = 0;
    if (version == 3 && (format_flags & 0x20U) != 0)
    {
        *payload_skip = 1;
    }
    else if (version == 4)
    {
        if ((format_flags & 0x40U) != 0)
        {
            (*payload_skip)++;
        }
        if ((format_flags & 0x01U) != 0)
        {
            *payload_skip += 4;
        }
    }
    return frame;
}

static int lyrics_extract_embedded_locked(const char *path, music_lyrics_line_t *lines,
                                          int max_lines, const char **source_type)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return 0;
    }

    uint8_t id3[10] = {0};
    if (fread(id3, 1, sizeof(id3), file) != sizeof(id3) || memcmp(id3, "ID3", 3) != 0 ||
        id3[3] < 2 || id3[3] > 4)
    {
        fclose(file);
        return 0;
    }

    const uint8_t version = id3[3];
    const bool tag_unsync = (id3[5] & 0x80) != 0;
    const long tag_end = 10L + (long)lyrics_synchsafe32(id3 + 6);
    long cursor = 10;

    if ((id3[5] & 0x40) != 0 && version >= 3)
    {
        uint8_t ext_size_data[4] = {0};
        if (fread(ext_size_data, 1, sizeof(ext_size_data), file) != sizeof(ext_size_data))
        {
            fclose(file);
            return 0;
        }
        uint32_t ext_size = version == 4 ? lyrics_synchsafe32(ext_size_data)
                                         : lyrics_be32(ext_size_data) + 4U;
        if (ext_size < 4 || cursor + (long)ext_size > tag_end)
        {
            fclose(file);
            return 0;
        }
        cursor += (long)ext_size;
    }

    const size_t header_size = version == 2 ? 6U : 10U;
    uint8_t frame_header[10] = {0};
    int count = 0;
    long uslt_payload_pos = -1;
    uint32_t uslt_frame_size = 0;
    uint8_t uslt_format_flags = 0;
    while (cursor + (long)header_size <= tag_end)
    {
        if (fseek(file, cursor, SEEK_SET) != 0 ||
            fread(frame_header, 1, header_size, file) != header_size)
        {
            break;
        }

        size_t id_size = version == 2 ? 3U : 4U;
        if (frame_header[0] == 0 || !lyrics_frame_id_valid(frame_header, id_size))
        {
            break;
        }
        uint32_t frame_size = version == 2 ? lyrics_be24(frame_header + 3)
                              : version == 4 ? lyrics_synchsafe32(frame_header + 4)
                                             : lyrics_be32(frame_header + 4);
        long payload_pos = cursor + (long)header_size;
        if (frame_size == 0 || payload_pos + (long)frame_size > tag_end)
        {
            break;
        }

        bool is_sylt = (version == 2 && memcmp(frame_header, "SLT", 3) == 0) ||
                       (version >= 3 && memcmp(frame_header, "SYLT", 4) == 0);
        bool is_uslt = (version == 2 && memcmp(frame_header, "ULT", 3) == 0) ||
                       (version >= 3 && memcmp(frame_header, "USLT", 4) == 0);
        if (is_sylt || is_uslt)
        {
            uint8_t format_flags = version >= 3 ? frame_header[9] : 0;
            bool unsupported = (version == 3 && (format_flags & 0xc0) != 0) ||
                               (version == 4 && (format_flags & 0x0c) != 0);
            if (!unsupported && frame_size <= MUSIC_LYRICS_MAX_FRAME_BYTES)
            {
                if (is_uslt && uslt_payload_pos < 0)
                {
                    uslt_payload_pos = payload_pos;
                    uslt_frame_size = frame_size;
                    uslt_format_flags = format_flags;
                }
                else if (is_sylt)
                {
                    size_t payload_size = 0;
                    size_t payload_skip = 0;
                    uint8_t *frame = lyrics_load_frame_payload(file, payload_pos,
                                                                frame_size, version,
                                                                format_flags, tag_unsync,
                                                                &payload_size, &payload_skip);
                    if (frame != NULL && payload_skip < payload_size)
                    {
                        count = lyrics_parse_sylt(frame + payload_skip,
                                                  payload_size - payload_skip,
                                                  lines, max_lines);
                    }
                    free(frame);
                    if (count > 0)
                    {
                        *source_type = "SYLT";
                        break;
                    }
                }
            }
        }
        cursor = payload_pos + (long)frame_size;
    }

    if (count == 0 && uslt_payload_pos >= 0)
    {
        size_t payload_size = 0;
        size_t payload_skip = 0;
        uint8_t *frame = lyrics_load_frame_payload(file, uslt_payload_pos,
                                                    uslt_frame_size, version,
                                                    uslt_format_flags, tag_unsync,
                                                    &payload_size, &payload_skip);
        if (frame != NULL && payload_skip < payload_size)
        {
            count = lyrics_parse_uslt(frame + payload_skip,
                                      payload_size - payload_skip,
                                      lines, max_lines);
        }
        free(frame);
        if (count > 0)
        {
            *source_type = "USLT/LRC";
        }
    }

    fclose(file);
    return count;
}

int music_lyrics_load_embedded(const char *path, music_lyrics_line_t *lines, int max_lines)
{
    if (path == NULL || lines == NULL || max_lines <= 0 || !storage_app_access_begin())
    {
        return 0;
    }
    const char *source_type = NULL;
    int count = lyrics_extract_embedded_locked(path, lines, max_lines, &source_type);
    storage_app_access_end();
    if (count > 0)
    {
        ESP_LOGI(TAG, "Loaded %d embedded %s line(s): %s", count,
                 source_type != NULL ? source_type : "timed lyric", path);
    }
    return count;
}
