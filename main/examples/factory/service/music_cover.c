#include "music_cover.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage.h"

#if LV_USE_LODEPNG
#include "src/libs/lodepng/lodepng.h"
#endif

#define MUSIC_COVER_MAX_ENCODED_BYTES (4U * 1024U * 1024U)
#define MUSIC_COVER_MAX_DECODED_BYTES (16U * 1024U * 1024U)

static const char *TAG = "music_cover";

typedef struct
{
    uint8_t *data;
    size_t size;
} music_cover_encoded_t;

static uint32_t music_cover_be24(const uint8_t *data)
{
    return ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
}

static uint32_t music_cover_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static uint32_t music_cover_synchsafe32(const uint8_t *data)
{
    return ((uint32_t)(data[0] & 0x7f) << 21) |
           ((uint32_t)(data[1] & 0x7f) << 14) |
           ((uint32_t)(data[2] & 0x7f) << 7) |
           (data[3] & 0x7f);
}

static bool music_cover_frame_id_valid(const uint8_t *id, size_t length)
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

static size_t music_cover_unsync_remove(uint8_t *data, size_t size)
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

static size_t music_cover_description_end(const uint8_t *data, size_t size, uint8_t encoding)
{
    if (encoding == 1 || encoding == 2)
    {
        for (size_t i = 0; i + 1 < size; i += 2)
        {
            if (data[i] == 0 && data[i + 1] == 0)
            {
                return i + 2;
            }
        }
    }
    else
    {
        const uint8_t *end = memchr(data, 0, size);
        if (end)
        {
            return (size_t)(end - data) + 1;
        }
    }
    return 0;
}

static bool music_cover_apic_payload_extract(uint8_t *frame, size_t frame_size,
                                             bool id3v22, music_cover_encoded_t *encoded)
{
    if (frame_size < (id3v22 ? 6U : 5U))
    {
        return false;
    }

    uint8_t encoding = frame[0];
    size_t pos = 1;
    if (id3v22)
    {
        pos += 3;
    }
    else
    {
        const uint8_t *mime_end = memchr(frame + pos, 0, frame_size - pos);
        if (mime_end == NULL)
        {
            return false;
        }
        pos = (size_t)(mime_end - frame) + 1;
    }

    if (pos >= frame_size)
    {
        return false;
    }
    pos++;

    size_t description_size = music_cover_description_end(frame + pos, frame_size - pos, encoding);
    if (description_size == 0 || description_size > frame_size - pos)
    {
        return false;
    }
    pos += description_size;
    if (pos >= frame_size)
    {
        return false;
    }

    size_t image_size = frame_size - pos;
    if (image_size > MUSIC_COVER_MAX_ENCODED_BYTES)
    {
        ESP_LOGW(TAG, "Embedded cover is too large: %u bytes", (unsigned)image_size);
        return false;
    }

    uint8_t *image_data = heap_caps_malloc(image_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (image_data == NULL)
    {
        image_data = heap_caps_malloc(image_size, MALLOC_CAP_DEFAULT);
    }
    if (image_data == NULL)
    {
        return false;
    }
    memcpy(image_data, frame + pos, image_size);
    encoded->data = image_data;
    encoded->size = image_size;
    return true;
}

static bool music_cover_mp3_extract_locked(const char *path, music_cover_encoded_t *encoded)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return false;
    }

    uint8_t id3[10] = {0};
    if (fread(id3, 1, sizeof(id3), file) != sizeof(id3) || memcmp(id3, "ID3", 3) != 0 ||
        id3[3] < 2 || id3[3] > 4)
    {
        fclose(file);
        return false;
    }

    const uint8_t version = id3[3];
    const bool tag_unsync = (id3[5] & 0x80) != 0;
    const uint32_t tag_size = music_cover_synchsafe32(id3 + 6);
    long cursor = 10;
    const long tag_end = cursor + (long)tag_size;

    if ((id3[5] & 0x40) != 0 && version >= 3)
    {
        uint8_t ext_size_data[4] = {0};
        if (fread(ext_size_data, 1, sizeof(ext_size_data), file) != sizeof(ext_size_data))
        {
            fclose(file);
            return false;
        }
        uint32_t ext_size = version == 4 ? music_cover_synchsafe32(ext_size_data)
                                         : music_cover_be32(ext_size_data) + 4U;
        if (ext_size < 4 || cursor + (long)ext_size > tag_end ||
            fseek(file, cursor + (long)ext_size, SEEK_SET) != 0)
        {
            fclose(file);
            return false;
        }
        cursor += (long)ext_size;
    }

    const size_t header_size = version == 2 ? 6U : 10U;
    uint8_t frame_header[10] = {0};
    bool found = false;
    while (cursor + (long)header_size <= tag_end)
    {
        if (fseek(file, cursor, SEEK_SET) != 0 ||
            fread(frame_header, 1, header_size, file) != header_size)
        {
            break;
        }

        const size_t id_length = version == 2 ? 3U : 4U;
        if (frame_header[0] == 0 || !music_cover_frame_id_valid(frame_header, id_length))
        {
            break;
        }

        uint32_t frame_size = version == 2 ? music_cover_be24(frame_header + 3)
                              : version == 4 ? music_cover_synchsafe32(frame_header + 4)
                                             : music_cover_be32(frame_header + 4);
        long payload_pos = cursor + (long)header_size;
        if (frame_size == 0 || payload_pos + (long)frame_size > tag_end)
        {
            break;
        }

        bool is_cover = (version == 2 && memcmp(frame_header, "PIC", 3) == 0) ||
                        (version >= 3 && memcmp(frame_header, "APIC", 4) == 0);
        if (is_cover && frame_size <= MUSIC_COVER_MAX_ENCODED_BYTES + 1024U)
        {
            uint8_t format_flags = version >= 3 ? frame_header[9] : 0;
            bool unsupported = (version == 3 && (format_flags & 0xc0) != 0) ||
                               (version == 4 && (format_flags & 0x0c) != 0);
            if (!unsupported)
            {
                uint8_t *frame = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (frame == NULL)
                {
                    frame = heap_caps_malloc(frame_size, MALLOC_CAP_DEFAULT);
                }
                if (frame != NULL && fread(frame, 1, frame_size, file) == frame_size)
                {
                    size_t payload_skip = 0;
                    if (version == 3 && (format_flags & 0x20) != 0)
                    {
                        payload_skip = 1;
                    }
                    else if (version == 4)
                    {
                        if ((format_flags & 0x40) != 0)
                        {
                            payload_skip++;
                        }
                        if ((format_flags & 0x01) != 0)
                        {
                            payload_skip += 4;
                        }
                    }

                    size_t payload_size = frame_size;
                    if (tag_unsync || (version == 4 && (format_flags & 0x02) != 0))
                    {
                        payload_size = music_cover_unsync_remove(frame, payload_size);
                    }
                    if (payload_skip < payload_size)
                    {
                        found = music_cover_apic_payload_extract(frame + payload_skip,
                                                                 payload_size - payload_skip,
                                                                 version == 2, encoded);
                    }
                }
                free(frame);
            }
            if (found)
            {
                break;
            }
        }
        cursor = payload_pos + (long)frame_size;
    }

    fclose(file);
    return found;
}

static bool music_cover_resize_rgb(const uint8_t *source, uint32_t source_width,
                                   uint32_t source_height, uint32_t source_stride,
                                   uint8_t source_bpp, uint32_t target_width,
                                   uint32_t target_height, music_cover_image_t *image)
{
    if (source == NULL || source_width == 0 || source_height == 0 ||
        target_width == 0 || target_height == 0)
    {
        return false;
    }

    size_t target_size = (size_t)target_width * target_height * 3U;
    uint8_t *target = heap_caps_malloc(target_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (target == NULL)
    {
        target = heap_caps_malloc(target_size, MALLOC_CAP_DEFAULT);
    }
    if (target == NULL)
    {
        return false;
    }

    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_width = source_width;
    uint32_t crop_height = source_height;
    if ((uint64_t)source_width * target_height > (uint64_t)source_height * target_width)
    {
        crop_width = (uint32_t)(((uint64_t)source_height * target_width) / target_height);
        crop_x = (source_width - crop_width) / 2U;
    }
    else
    {
        crop_height = (uint32_t)(((uint64_t)source_width * target_height) / target_width);
        crop_y = (source_height - crop_height) / 2U;
    }

    for (uint32_t y = 0; y < target_height; y++)
    {
        uint32_t source_y = crop_y + (uint32_t)(((uint64_t)y * crop_height) / target_height);
        uint8_t *target_row = target + (size_t)y * target_width * 3U;
        const uint8_t *source_row = source + (size_t)source_y * source_stride;
        for (uint32_t x = 0; x < target_width; x++)
        {
            uint32_t source_x = crop_x + (uint32_t)(((uint64_t)x * crop_width) / target_width);
            const uint8_t *pixel = source_row + (size_t)source_x * source_bpp;
            target_row[x * 3U] = pixel[2];
            target_row[x * 3U + 1U] = pixel[1];
            target_row[x * 3U + 2U] = pixel[0];
        }
    }

    image->pixels = target;
    image->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    image->dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    image->dsc.header.flags = 0;
    image->dsc.header.w = target_width;
    image->dsc.header.h = target_height;
    image->dsc.header.stride = target_width * 3U;
    image->dsc.data_size = target_size;
    image->dsc.data = target;
    return true;
}

static bool music_cover_decode_jpeg(const music_cover_encoded_t *encoded,
                                    uint32_t target_width, uint32_t target_height,
                                    music_cover_image_t *image)
{
    jpeg_decode_picture_info_t info = {0};
    if (jpeg_decoder_get_info(encoded->data, encoded->size, &info) != ESP_OK ||
        info.width == 0 || info.height == 0)
    {
        return false;
    }

    uint32_t aligned_width = (info.width + 15U) & ~15U;
    uint32_t aligned_height = (info.height + 15U) & ~15U;
    uint64_t decoded_bytes = (uint64_t)aligned_width * aligned_height * 3U;
    if (decoded_bytes > MUSIC_COVER_MAX_DECODED_BYTES)
    {
        ESP_LOGW(TAG, "JPEG cover dimensions are too large: %" PRIu32 "x%" PRIu32,
                 info.width, info.height);
        return false;
    }

    jpeg_decode_memory_alloc_cfg_t input_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t input_size = 0;
    uint8_t *input = jpeg_alloc_decoder_mem(encoded->size, &input_cfg, &input_size);
    if (input == NULL || input_size < encoded->size)
    {
        free(input);
        return false;
    }
    memcpy(input, encoded->data, encoded->size);

    jpeg_decode_memory_alloc_cfg_t output_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t output_size = 0;
    uint8_t *output = jpeg_alloc_decoder_mem((size_t)decoded_bytes, &output_cfg, &output_size);
    if (output == NULL || output_size < decoded_bytes)
    {
        free(input);
        free(output);
        return false;
    }

    jpeg_decoder_handle_t decoder = NULL;
    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 3000,
    };
    if (jpeg_new_decoder_engine(&engine_cfg, &decoder) != ESP_OK)
    {
        free(input);
        free(output);
        return false;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t actual_size = 0;
    esp_err_t ret = jpeg_decoder_process(decoder, &decode_cfg, input, encoded->size,
                                         output, output_size, &actual_size);
    esp_err_t delete_ret = jpeg_del_decoder_engine(decoder);
    free(input);
    bool ok = ret == ESP_OK && delete_ret == ESP_OK && actual_size > 0 &&
              music_cover_resize_rgb(output, info.width, info.height,
                                     aligned_width * 3U, 3, target_width,
                                     target_height, image);
    free(output);
    return ok;
}

#if LV_USE_LODEPNG
static bool music_cover_decode_png(const music_cover_encoded_t *encoded,
                                   uint32_t target_width, uint32_t target_height,
                                   music_cover_image_t *image)
{
    if (encoded->size < 24 || memcmp(encoded->data, "\x89PNG\r\n\x1a\n", 8) != 0)
    {
        return false;
    }
    uint32_t width = music_cover_be32(encoded->data + 16);
    uint32_t height = music_cover_be32(encoded->data + 20);
    if (width == 0 || height == 0 ||
        (uint64_t)width * height * 4U > MUSIC_COVER_MAX_DECODED_BYTES)
    {
        return false;
    }

    unsigned char *rgba = NULL;
    unsigned decoded_width = 0;
    unsigned decoded_height = 0;
    unsigned error = lodepng_decode32(&rgba, &decoded_width, &decoded_height,
                                      encoded->data, encoded->size);
    if (error != 0 || rgba == NULL)
    {
        free(rgba);
        return false;
    }
    bool ok = music_cover_resize_rgb(rgba, decoded_width, decoded_height,
                                     decoded_width * 4U, 4, target_width,
                                     target_height, image);
    free(rgba);
    return ok;
}
#endif

bool music_cover_image_load_mp3(const char *path, uint32_t target_width,
                                uint32_t target_height, music_cover_image_t *image)
{
    if (path == NULL || image == NULL)
    {
        return false;
    }
    memset(image, 0, sizeof(*image));
    if (!storage_app_access_begin())
    {
        return false;
    }

    music_cover_encoded_t encoded = {0};
    bool extracted = music_cover_mp3_extract_locked(path, &encoded);
    storage_app_access_end();
    if (!extracted)
    {
        return false;
    }

    bool decoded = false;
    if (encoded.size >= 3 && encoded.data[0] == 0xff && encoded.data[1] == 0xd8)
    {
        decoded = music_cover_decode_jpeg(&encoded, target_width, target_height, image);
    }
#if LV_USE_LODEPNG
    else if (encoded.size >= 8 && memcmp(encoded.data, "\x89PNG\r\n\x1a\n", 8) == 0)
    {
        decoded = music_cover_decode_png(&encoded, target_width, target_height, image);
    }
#endif
    else
    {
        ESP_LOGW(TAG, "Unsupported embedded cover format: %s", path);
    }
    free(encoded.data);
    if (decoded)
    {
        ESP_LOGI(TAG, "Loaded embedded cover: %s", path);
    }
    return decoded;
}

void music_cover_image_release(music_cover_image_t *image)
{
    if (image == NULL)
    {
        return;
    }
    free(image->pixels);
    memset(image, 0, sizeof(*image));
}
