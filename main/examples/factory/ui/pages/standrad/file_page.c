#include "ui_internal.h"
#include "file_page.h"

static const char *TAG = "[UI][file_page]";

#define FILE_TXT_MAX_BYTES (32 * 1024)
#define FILE_BMP_MAX_DECODE_BYTES (8 * 1024 * 1024)
#define FILE_JPEG_MAX_INPUT_BYTES (8 * 1024 * 1024)

static file_page_ui_t s_file_ui;
static uint8_t *s_file_image_buf;
static lv_image_dsc_t s_file_image_dsc;
static char s_file_open_pending_path[FILE_PATH_MAX_LEN];
static char s_file_open_pending_name[FILE_NAME_MAX_LEN];
static char s_file_current_path[FILE_PATH_MAX_LEN] = FILE_SCAN_DIR;
static char s_file_pending_path[FILE_PATH_MAX_LEN] = FILE_SCAN_DIR;

static void file_page_glass_style(lv_obj_t *obj, uint32_t background,
                                  lv_opa_t background_opa, uint32_t border,
                                  lv_opa_t border_opa, uint32_t shadow,
                                  lv_opa_t shadow_opa, int32_t shadow_width,
                                  int32_t radius)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(obj, background_opa, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
    lv_obj_set_style_border_opa(obj, border_opa, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(shadow), 0);
    lv_obj_set_style_shadow_opa(obj, shadow_opa, 0);
    lv_obj_set_style_shadow_width(obj, shadow_width, 0);
    lv_obj_set_style_shadow_spread(obj, 2, 0);
    lv_obj_set_style_shadow_offset_x(obj, 0, 0);
    lv_obj_set_style_shadow_offset_y(obj, 0, 0);
}

/*********file Page************/
static int file_page_scan_dir(const char *dir_path);

static const char *file_page_ext_name(const char *name, bool is_dir)
{
    if (is_dir)
    {
        return strcmp(name, "..") == 0 ? "UP" : "FOLDER";
    }

    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot[1] == '\0')
    {
        return "FILE";
    }
    return dot + 1;
}

static void file_page_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0)
    {
        return;
    }

    size_t len = strlen(src);
    if (len >= dst_size)
    {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool file_page_build_path(char *dst, size_t dst_size, const char *dir_path, const char *name)
{
    size_t dir_len = strlen(dir_path);
    size_t name_len = strlen(name);
    if (dir_len + 1 + name_len >= dst_size)
    {
        return false;
    }

    memcpy(dst, dir_path, dir_len);
    dst[dir_len] = '/';
    memcpy(dst + dir_len + 1, name, name_len + 1);
    return true;
}

static bool file_page_parent_path(char *path)
{
    if (strcmp(path, FILE_SCAN_DIR) == 0)
    {
        return false;
    }

    char *slash = strrchr(path, '/');
    if (slash == NULL || slash <= path + strlen(FILE_SCAN_DIR))
    {
        file_page_copy_text(path, FILE_PATH_MAX_LEN, FILE_SCAN_DIR);
        return true;
    }

    *slash = '\0';
    return true;
}

static bool file_page_is_dir(const char *dir_path, const char *name)
{
    char path[FILE_PATH_MAX_LEN] = {0};
    if (!file_page_build_path(path, sizeof(path), dir_path, name))
    {
        return false;
    }

    struct stat st = {0};
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_page_has_ext(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    return dot != NULL && strcasecmp(dot + 1, ext) == 0;
}

static uint16_t file_read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t file_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t file_read_le32s(const uint8_t *p)
{
    return (int32_t)file_read_le32(p);
}

static void file_page_image_buf_free(void)
{
    if (s_file_image_buf)
    {
        if (s_file_ui.viewer_img)
        {
            lv_image_set_src(s_file_ui.viewer_img, NULL);
        }
        lv_image_cache_drop(&s_file_image_dsc);
        heap_caps_free(s_file_image_buf);
        s_file_image_buf = NULL;
        memset(&s_file_image_dsc, 0, sizeof(s_file_image_dsc));
    }
}

static void file_page_image_show_scaled(uint32_t width, uint32_t height)
{
    if (s_file_ui.viewer_img == NULL)
    {
        return;
    }

    lv_obj_clear_flag(s_file_ui.viewer_img, LV_OBJ_FLAG_HIDDEN);
    lv_image_cache_drop(&s_file_image_dsc);
    lv_image_set_src(s_file_ui.viewer_img, &s_file_image_dsc);

    const uint32_t stage_w = 620;
    const uint32_t stage_h = 500;
    uint32_t zoom_w = width > 0 ? (stage_w * 256U) / width : 256U;
    uint32_t zoom_h = height > 0 ? (stage_h * 256U) / height : 256U;
    uint32_t zoom = zoom_w < zoom_h ? zoom_w : zoom_h;
    if (zoom > 256U)
    {
        zoom = 256U;
    }
    if (zoom == 0)
    {
        zoom = 1;
    }
    lv_image_set_scale(s_file_ui.viewer_img, zoom);
    lv_obj_center(s_file_ui.viewer_img);
}

static void file_page_viewer_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    if (s_file_ui.viewer_cont)
    {
        lv_obj_add_flag(s_file_ui.viewer_cont, LV_OBJ_FLAG_HIDDEN);
    }
    file_page_image_buf_free();
}

static void file_page_viewer_show(const char *title, const char *status)
{
    if (s_file_ui.viewer_cont == NULL)
    {
        return;
    }

    lv_obj_clear_flag(s_file_ui.viewer_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_file_ui.viewer_cont);
    if (s_file_ui.viewer_title_label)
    {
        lv_label_set_text(s_file_ui.viewer_title_label, title ? title : "VIEWER");
    }
    if (s_file_ui.viewer_status_label)
    {
        lv_label_set_text(s_file_ui.viewer_status_label, status ? status : "");
        lv_obj_set_style_text_color(s_file_ui.viewer_status_label, lv_color_hex(UI_MUTED), 0);
    }
    if (s_file_ui.viewer_textarea)
    {
        lv_obj_add_flag(s_file_ui.viewer_textarea, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_file_ui.viewer_img)
    {
        lv_obj_add_flag(s_file_ui.viewer_img, LV_OBJ_FLAG_HIDDEN);
    }
}

static void file_page_viewer_status_set(const char *status, uint32_t color)
{
    if (s_file_ui.viewer_status_label)
    {
        lv_label_set_text(s_file_ui.viewer_status_label, status ? status : "");
        lv_obj_set_style_text_color(s_file_ui.viewer_status_label, lv_color_hex(color), 0);
    }
}

static void file_page_open_txt(const char *path, const char *name)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        file_page_viewer_show(name, "TXT open failed");
        file_page_viewer_status_set("TXT open failed", UI_ERROR);
        ESP_LOGW(TAG, "Open TXT failed: %s errno=%d", path, errno);
        return;
    }

    char *buf = heap_caps_malloc(FILE_TXT_MAX_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL)
    {
        buf = heap_caps_malloc(FILE_TXT_MAX_BYTES + 1, MALLOC_CAP_DEFAULT);
    }
    if (buf == NULL)
    {
        fclose(file);
        file_page_viewer_show(name, "TXT no memory");
        file_page_viewer_status_set("TXT no memory", UI_ERROR);
        return;
    }

    size_t got = fread(buf, 1, FILE_TXT_MAX_BYTES, file);
    bool truncated = !feof(file);
    fclose(file);
    buf[got] = '\0';

    file_page_image_buf_free();
    file_page_viewer_show(name, truncated ? "TXT preview truncated" : "TXT");
    if (s_file_ui.viewer_textarea)
    {
        lv_obj_clear_flag(s_file_ui.viewer_textarea, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(s_file_ui.viewer_textarea, buf);
        lv_obj_scroll_to_y(s_file_ui.viewer_textarea, 0, LV_ANIM_OFF);
    }
    file_page_viewer_status_set(truncated ? "TXT preview truncated" : "TXT", truncated ? UI_WARN : UI_OK);
    heap_caps_free(buf);
}

static bool file_page_decode_bmp_to_rgb888(const char *path, uint32_t *out_w, uint32_t *out_h)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        ESP_LOGW(TAG, "Open BMP failed: %s errno=%d", path, errno);
        return false;
    }

    uint8_t header[54] = {0};
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        header[0] != 'B' ||
        header[1] != 'M')
    {
        fclose(file);
        return false;
    }

    uint32_t pixel_offset = file_read_le32(&header[10]);
    uint32_t dib_size = file_read_le32(&header[14]);
    int32_t width = file_read_le32s(&header[18]);
    int32_t height_signed = file_read_le32s(&header[22]);
    uint16_t planes = file_read_le16(&header[26]);
    uint16_t bpp = file_read_le16(&header[28]);
    uint32_t compression = file_read_le32(&header[30]);
    if (dib_size < 40 || width <= 0 || height_signed == 0 || planes != 1 ||
        compression != 0 || (bpp != 24 && bpp != 32))
    {
        fclose(file);
        ESP_LOGW(TAG, "Unsupported BMP: %s %ldx%ld bpp=%u comp=%" PRIu32,
                 path,
                 (long)width,
                 (long)height_signed,
                 bpp,
                 compression);
        return false;
    }

    bool top_down = height_signed < 0;
    uint32_t height = top_down ? (uint32_t)(-height_signed) : (uint32_t)height_signed;
    uint32_t row_bytes = ((uint32_t)width * bpp + 31U) / 32U * 4U;
    uint32_t rgb_bytes = (uint32_t)width * height * 3U;
    if (rgb_bytes == 0 || rgb_bytes > FILE_BMP_MAX_DECODE_BYTES)
    {
        fclose(file);
        ESP_LOGW(TAG, "BMP decode buffer too large: %s %ldx%" PRIu32 " %" PRIu32 " bytes",
                 path,
                 (long)width,
                 height,
                 rgb_bytes);
        return false;
    }

    file_page_image_buf_free();

    uint8_t *rgb = heap_caps_malloc(rgb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rgb == NULL)
    {
        rgb = heap_caps_malloc(rgb_bytes, MALLOC_CAP_DEFAULT);
    }
    uint8_t *row = heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (row == NULL)
    {
        row = heap_caps_malloc(row_bytes, MALLOC_CAP_DEFAULT);
    }
    if (rgb == NULL || row == NULL)
    {
        heap_caps_free(rgb);
        heap_caps_free(row);
        fclose(file);
        return false;
    }

    bool ok = true;
    for (uint32_t y = 0; y < height; y++)
    {
        uint32_t file_y = top_down ? y : height - 1U - y;
        if (fseek(file, (long)pixel_offset + (long)file_y * (long)row_bytes, SEEK_SET) != 0 ||
            fread(row, 1, row_bytes, file) != row_bytes)
        {
            ok = false;
            break;
        }

        uint8_t *dst = rgb + ((size_t)y * (uint32_t)width * 3U);
        for (uint32_t x = 0; x < (uint32_t)width; x++)
        {
            const uint8_t *src = row + x * (bpp / 8U);
            dst[x * 3U + 0U] = src[2];
            dst[x * 3U + 1U] = src[1];
            dst[x * 3U + 2U] = src[0];
        }
    }

    heap_caps_free(row);
    fclose(file);
    if (!ok)
    {
        heap_caps_free(rgb);
        return false;
    }

    s_file_image_buf = rgb;
    s_file_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_file_image_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    s_file_image_dsc.header.flags = 0;
    s_file_image_dsc.header.w = (uint32_t)width;
    s_file_image_dsc.header.h = height;
    s_file_image_dsc.header.stride = (uint32_t)width * 3U;
    s_file_image_dsc.data_size = rgb_bytes;
    s_file_image_dsc.data = s_file_image_buf;
    *out_w = (uint32_t)width;
    *out_h = height;
    return true;
}

static void file_page_open_bmp(const char *path, const char *name)
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (!file_page_decode_bmp_to_rgb888(path, &width, &height))
    {
        file_page_viewer_show(name, "BMP decode failed");
        file_page_viewer_status_set("BMP decode failed", UI_ERROR);
        return;
    }

    char status[64] = {0};
    snprintf(status, sizeof(status), "BMP %" PRIu32 "x%" PRIu32, width, height);
    file_page_viewer_show(name, status);
    file_page_image_show_scaled(width, height);
    file_page_viewer_status_set(status, UI_OK);
}

static bool file_page_decode_jpeg_to_rgb888(const char *path, uint32_t *out_w, uint32_t *out_h)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        ESP_LOGW(TAG, "Open JPEG failed: %s errno=%d", path, errno);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return false;
    }
    long file_size = ftell(file);
    if (file_size <= 0 || file_size > FILE_JPEG_MAX_INPUT_BYTES)
    {
        fclose(file);
        ESP_LOGW(TAG, "JPEG input too large: %s %ld bytes", path, file_size);
        return false;
    }
    rewind(file);

    jpeg_decode_memory_alloc_cfg_t in_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t in_buf_size = 0;
    uint8_t *in_buf = jpeg_alloc_decoder_mem((size_t)file_size, &in_mem_cfg, &in_buf_size);
    if (in_buf == NULL)
    {
        fclose(file);
        ESP_LOGW(TAG, "Alloc JPEG input failed: %s %ld bytes", path, file_size);
        return false;
    }

    size_t read_size = fread(in_buf, 1, (size_t)file_size, file);
    fclose(file);
    if (read_size != (size_t)file_size)
    {
        free(in_buf);
        ESP_LOGW(TAG, "Read JPEG failed: %s %u/%ld", path, (unsigned)read_size, file_size);
        return false;
    }

    jpeg_decode_picture_info_t pic_info = {0};
    esp_err_t ret = jpeg_decoder_get_info(in_buf, (uint32_t)file_size, &pic_info);
    if (ret != ESP_OK)
    {
        free(in_buf);
        ESP_LOGW(TAG, "Get JPEG info failed: %s %s", path, esp_err_to_name(ret));
        return false;
    }

    uint32_t aligned_w = (pic_info.width + 15U) & ~15U;
    uint32_t aligned_h = (pic_info.height + 15U) & ~15U;
    uint32_t out_bytes = aligned_w * aligned_h * 3U;
    if (pic_info.width == 0 || pic_info.height == 0 || out_bytes == 0 || out_bytes > FILE_BMP_MAX_DECODE_BYTES)
    {
        free(in_buf);
        ESP_LOGW(TAG, "JPEG decode buffer too large: %s %" PRIu32 "x%" PRIu32 " %" PRIu32 " bytes",
                 path,
                 pic_info.width,
                 pic_info.height,
                 out_bytes);
        return false;
    }

    file_page_image_buf_free();

    jpeg_decode_memory_alloc_cfg_t out_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t out_buf_size = 0;
    uint8_t *out_buf = jpeg_alloc_decoder_mem(out_bytes, &out_mem_cfg, &out_buf_size);
    if (out_buf == NULL)
    {
        free(in_buf);
        ESP_LOGW(TAG, "Alloc JPEG output failed: %s %" PRIu32 " bytes", path, out_bytes);
        return false;
    }

    jpeg_decoder_handle_t decoder = NULL;
    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 3000,
    };
    ret = jpeg_new_decoder_engine(&engine_cfg, &decoder);
    if (ret != ESP_OK)
    {
        free(out_buf);
        free(in_buf);
        ESP_LOGW(TAG, "Create JPEG decoder failed: %s", esp_err_to_name(ret));
        return false;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t decoded_size = 0;
    ret = jpeg_decoder_process(decoder,
                               &decode_cfg,
                               in_buf,
                               (uint32_t)file_size,
                               out_buf,
                               (uint32_t)out_buf_size,
                               &decoded_size);
    esp_err_t del_ret = jpeg_del_decoder_engine(decoder);
    free(in_buf);
    if (ret != ESP_OK)
    {
        free(out_buf);
        ESP_LOGW(TAG, "Decode JPEG failed: %s %s", path, esp_err_to_name(ret));
        return false;
    }
    if (del_ret != ESP_OK)
    {
        free(out_buf);
        ESP_LOGW(TAG, "Delete JPEG decoder failed: %s", esp_err_to_name(del_ret));
        return false;
    }

    s_file_image_buf = out_buf;
    s_file_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_file_image_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    s_file_image_dsc.header.flags = 0;
    s_file_image_dsc.header.w = pic_info.width;
    s_file_image_dsc.header.h = pic_info.height;
    s_file_image_dsc.header.stride = aligned_w * 3U;
    s_file_image_dsc.data_size = decoded_size > 0 ? decoded_size : out_bytes;
    s_file_image_dsc.data = s_file_image_buf;
    *out_w = pic_info.width;
    *out_h = pic_info.height;
    return true;
}

static void file_page_open_jpeg(const char *path, const char *name)
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (!file_page_decode_jpeg_to_rgb888(path, &width, &height))
    {
        file_page_viewer_show(name, "JPEG decode failed");
        file_page_viewer_status_set("JPEG decode failed", UI_ERROR);
        return;
    }

    char status[64] = {0};
    snprintf(status, sizeof(status), "JPEG %" PRIu32 "x%" PRIu32, width, height);
    file_page_viewer_show(name, status);
    file_page_image_show_scaled(width, height);
    file_page_viewer_status_set(status, UI_OK);
}

static void file_page_empty_show(int shown)
{
    if (shown == 0 && s_file_ui.list_cont)
    {
        lv_obj_t *empty = ui_label_create(s_file_ui.list_cont, "No files found", &lv_font_SourceHanSansSC_Regular_2_16, UI_MUTED);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void file_page_scan_async_cb(void *user_data)
{
    (void)user_data;
    Page *current = ui_page_get_current();
    if (current == NULL || current->id != PAGE_FILE || s_file_ui.root == NULL || s_file_ui.list_cont == NULL)
    {
        return;
    }

    file_page_copy_text(s_file_current_path, sizeof(s_file_current_path), s_file_pending_path);
    int shown = file_page_scan_dir(s_file_current_path);
    file_page_empty_show(shown);
}

static void file_page_open_async_cb(void *user_data)
{
    (void)user_data;
    Page *current = ui_page_get_current();
    if (current == NULL || current->id != PAGE_FILE || s_file_ui.root == NULL)
    {
        return;
    }

    if (file_page_has_ext(s_file_open_pending_path, "txt"))
    {
        file_page_open_txt(s_file_open_pending_path, s_file_open_pending_name);
    }
    else if (file_page_has_ext(s_file_open_pending_path, "bmp"))
    {
        file_page_open_bmp(s_file_open_pending_path, s_file_open_pending_name);
    }
    else if (file_page_has_ext(s_file_open_pending_path, "jpg") ||
             file_page_has_ext(s_file_open_pending_path, "jpeg"))
    {
        file_page_open_jpeg(s_file_open_pending_path, s_file_open_pending_name);
    }
}

static void file_page_open_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    file_page_open_async_cb(NULL);
}

static void file_page_row_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    const char *name = (const char *)lv_event_get_user_data(e);
    if (name == NULL || name[0] == '\0')
    {
        return;
    }

    if (strcmp(name, "..") == 0)
    {
        if (file_page_parent_path(s_file_current_path))
        {
            file_page_copy_text(s_file_pending_path, sizeof(s_file_pending_path), s_file_current_path);
            lv_async_call(file_page_scan_async_cb, NULL);
        }
        return;
    }

    char next_path[FILE_PATH_MAX_LEN] = {0};
    if (!file_page_build_path(next_path, sizeof(next_path), s_file_current_path, name))
    {
        return;
    }

    struct stat st = {0};
    if (stat(next_path, &st) == 0 && S_ISDIR(st.st_mode))
    {
        file_page_copy_text(s_file_pending_path, sizeof(s_file_pending_path), next_path);
        lv_async_call(file_page_scan_async_cb, NULL);
        return;
    }

    if (file_page_has_ext(next_path, "txt") ||
        file_page_has_ext(next_path, "bmp") ||
        file_page_has_ext(next_path, "jpg") ||
        file_page_has_ext(next_path, "jpeg"))
    {
        file_page_copy_text(s_file_open_pending_path, sizeof(s_file_open_pending_path), next_path);
        file_page_copy_text(s_file_open_pending_name, sizeof(s_file_open_pending_name), name);
        file_page_viewer_show(name, "LOADING...");
        file_page_viewer_status_set("LOADING...", UI_PRIMARY);
        lv_timer_t *timer = lv_timer_create(file_page_open_timer_cb, 60, NULL);
        if (timer)
        {
            lv_timer_set_repeat_count(timer, 1);
        }
    }
    else
    {
        file_page_viewer_show(name, "Unsupported file");
        file_page_viewer_status_set("Only TXT/BMP/JPG/JPEG preview now", UI_WARN);
    }
}

static void file_page_add_row(lv_obj_t *parent, int index, const char *name, bool is_dir, const char *event_name)
{
    lv_obj_t *row = ui_flex_container_create(parent,
                                             LV_PCT(100),
                                             40,
                                             LV_FLEX_FLOW_ROW,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_CENTER,
                                             LV_FLEX_ALIGN_CENTER);
    file_page_glass_style(row, UI_PANEL_HL,
                          index % 2 == 0 ? LV_OPA_20 : LV_OPA_40,
                          UI_TEXT, LV_OPA_20, UI_PRIMARY, LV_OPA_30, 0, 12);
    lv_obj_set_style_bg_opa(row, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_TEXT), LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(row, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(row, lv_color_hex(UI_PRIMARY), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(row, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(row, 8, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(row, 1, LV_STATE_PRESSED);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon = ui_label_create(row, is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, &lv_font_SourceHanSansSC_Regular_2_16, is_dir ? UI_PRIMARY : UI_SECONDARY);
    lv_obj_set_width(icon, 28);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *name_label = ui_label_create(row, name, MUSIC_NAME_FONT, UI_TEXT);
    lv_obj_set_width(name_label, 430);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_MODE_DOTS);

    ui_flex_spacer_create(row);

    lv_obj_t *type_label = ui_label_create(row, file_page_ext_name(name, is_dir), &lv_font_montserrat_14, is_dir ? UI_PRIMARY : UI_MUTED);
    lv_obj_set_width(type_label, 86);
    lv_label_set_long_mode(type_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(type_label, LV_TEXT_ALIGN_RIGHT, 0);

    if (event_name)
    {
        lv_obj_add_event_cb(row, file_page_row_event_cb, LV_EVENT_CLICKED, (void *)event_name);
    }
}

static int file_page_scan_dir(const char *dir_path)
{
    if (s_file_ui.list_cont == NULL)
    {
        return -1;
    }

    if (s_file_ui.list_cont)
    {
        lv_obj_clean(s_file_ui.list_cont);
    }
    if (s_file_ui.path_label)
    {
        lv_label_set_text(s_file_ui.path_label, dir_path);
    }

    DIR *dir = opendir(dir_path);
    if (dir == NULL)
    {
        if (s_file_ui.count_label)
        {
            lv_label_set_text(s_file_ui.count_label, "OPEN FAILED");
            lv_obj_set_style_text_color(s_file_ui.count_label, lv_color_hex(UI_ERROR), 0);
        }
        ESP_LOGW(TAG, "Failed to open file dir: %s", dir_path);
        return -1;
    }

    int shown = 0;
    int total = 0;
    static char row_names[FILE_LIST_MAX_ROWS][FILE_NAME_MAX_LEN];
    if (strcmp(dir_path, FILE_SCAN_DIR) != 0 && shown < FILE_LIST_MAX_ROWS)
    {
        file_page_copy_text(row_names[shown], sizeof(row_names[shown]), "..");
        file_page_add_row(s_file_ui.list_cont, shown, row_names[shown], true, row_names[shown]);
        shown++;
    }

    for (int pass = 0; pass < 2; pass++)
    {
        rewinddir(dir);
        struct dirent *entry = NULL;
        while ((entry = readdir(dir)) != NULL)
        {
            if (entry->d_name[0] == '.')
            {
                continue;
            }

            bool is_dir = file_page_is_dir(dir_path, entry->d_name);
            if ((pass == 0 && !is_dir) || (pass == 1 && is_dir))
            {
                continue;
            }

            total++;
            if (shown < FILE_LIST_MAX_ROWS)
            {
                file_page_copy_text(row_names[shown], sizeof(row_names[shown]), entry->d_name);
                file_page_add_row(s_file_ui.list_cont, shown, row_names[shown], is_dir, row_names[shown]);
                shown++;
            }
        }
    }
    closedir(dir);

    int shown_items = shown;
    if (strcmp(dir_path, FILE_SCAN_DIR) != 0 && shown_items > 0)
    {
        shown_items--;
    }
    if (s_file_ui.count_label)
    {
        if (shown_items < total)
        {
            lv_label_set_text_fmt(s_file_ui.count_label, "%d/%d ITEMS", shown_items, total);
        }
        else
        {
            lv_label_set_text_fmt(s_file_ui.count_label, "%d ITEMS", total);
        }
        lv_obj_set_style_text_color(s_file_ui.count_label, lv_color_hex(UI_MUTED), 0);
    }
    return shown;
}

static void file_page_viewer_create(lv_obj_t *parent)
{
    s_file_ui.viewer_cont = lv_obj_create(parent);
    lv_obj_set_size(s_file_ui.viewer_cont, 660, 580);
    lv_obj_align(s_file_ui.viewer_cont, LV_ALIGN_CENTER, 0, 30);
    file_page_glass_style(s_file_ui.viewer_cont, UI_PANEL, LV_OPA_70,
                          UI_TEXT, LV_OPA_50, UI_PRIMARY, LV_OPA_40, 22, 18);
    lv_obj_set_style_pad_all(s_file_ui.viewer_cont, 14, 0);
    lv_obj_set_scrollbar_mode(s_file_ui.viewer_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_file_ui.viewer_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_file_ui.viewer_cont, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *head = ui_flex_container_create(s_file_ui.viewer_cont,
                                              LV_PCT(100),
                                              38,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 10, 0);

    s_file_ui.viewer_title_label = ui_label_create(head, "VIEWER", MUSIC_NAME_FONT, UI_TEXT);
    lv_obj_set_width(s_file_ui.viewer_title_label, 390);
    lv_label_set_long_mode(s_file_ui.viewer_title_label, LV_LABEL_LONG_MODE_DOTS);

    s_file_ui.viewer_status_label = ui_label_create(head, "", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(s_file_ui.viewer_status_label, 150);
    lv_label_set_long_mode(s_file_ui.viewer_status_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_file_ui.viewer_status_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *close_btn = lv_button_create(head);
    lv_obj_set_size(close_btn, 36, 32);
    file_page_glass_style(close_btn, UI_PANEL_HL, LV_OPA_50,
                          UI_TEXT, LV_OPA_40, UI_PRIMARY, LV_OPA_30, 10, 10);
    lv_obj_add_event_cb(close_btn, file_page_viewer_close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = ui_label_create(close_btn, LV_SYMBOL_CLOSE, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(close_label);

    s_file_ui.viewer_body = lv_obj_create(s_file_ui.viewer_cont);
    lv_obj_set_size(s_file_ui.viewer_body, LV_PCT(100), 500);
    lv_obj_align(s_file_ui.viewer_body, LV_ALIGN_BOTTOM_MID, 0, 0);
    file_page_glass_style(s_file_ui.viewer_body, UI_BG, LV_OPA_50,
                          UI_TEXT, LV_OPA_20, UI_PRIMARY, LV_OPA_20, 8, 12);
    lv_obj_set_style_pad_all(s_file_ui.viewer_body, 10, 0);
    lv_obj_set_scrollbar_mode(s_file_ui.viewer_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_file_ui.viewer_body, LV_OBJ_FLAG_SCROLLABLE);

    s_file_ui.viewer_textarea = lv_textarea_create(s_file_ui.viewer_body);
    lv_obj_set_size(s_file_ui.viewer_textarea, LV_PCT(100), LV_PCT(100));
    lv_textarea_set_text(s_file_ui.viewer_textarea, "");
    lv_textarea_set_one_line(s_file_ui.viewer_textarea, false);
    lv_textarea_set_max_length(s_file_ui.viewer_textarea, FILE_TXT_MAX_BYTES);
    lv_obj_set_style_bg_opa(s_file_ui.viewer_textarea, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_file_ui.viewer_textarea, 0, 0);
    lv_obj_set_style_text_color(s_file_ui.viewer_textarea, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_file_ui.viewer_textarea, MUSIC_NAME_FONT, 0);
    lv_obj_set_scrollbar_mode(s_file_ui.viewer_textarea, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_file_ui.viewer_textarea, LV_OBJ_FLAG_HIDDEN);

    s_file_ui.viewer_img = lv_image_create(s_file_ui.viewer_body);
    lv_obj_center(s_file_ui.viewer_img);
    lv_obj_add_flag(s_file_ui.viewer_img, LV_OBJ_FLAG_HIDDEN);
}

static void file_page_create(Page *page)
{
    file_page_copy_text(s_file_current_path, sizeof(s_file_current_path), FILE_SCAN_DIR);
    file_page_copy_text(s_file_pending_path, sizeof(s_file_pending_path), FILE_SCAN_DIR);
    s_file_ui.root = page->root;
    lv_obj_set_style_bg_color(s_file_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_file_ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_file_ui.root, 0, 0);
    lv_obj_add_flag(s_file_ui.root, LV_OBJ_FLAG_CLICKABLE);

    ui_background_create(s_file_ui.root);
    // ui_standard_page_title_create(s_file_ui.root, "FILES");

    lv_obj_t *main_cont = lv_obj_create(s_file_ui.root);
    ui_main_cont_style_init(main_cont);
    lv_obj_set_style_pad_top(main_cont, 20, 0);
    lv_obj_set_style_bg_opa(main_cont, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(main_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = ui_flex_container_create(main_cont,
                                               640,
                                               640,
                                               LV_FLEX_FLOW_COLUMN,
                                               LV_FLEX_ALIGN_START,
                                               LV_FLEX_ALIGN_CENTER,
                                               LV_FLEX_ALIGN_CENTER);
    file_page_glass_style(panel, UI_PANEL, LV_OPA_30,
                          UI_TEXT, LV_OPA_40, UI_PRIMARY, LV_OPA_30, 24, 18);
    lv_obj_set_style_pad_all(panel, 16, 0);
    lv_obj_set_style_pad_row(panel, 10, 0);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *path_row = ui_flex_container_create(panel,
                                                  LV_PCT(100),
                                                  32,
                                                  LV_FLEX_FLOW_ROW,
                                                  LV_FLEX_ALIGN_START,
                                                  LV_FLEX_ALIGN_CENTER,
                                                  LV_FLEX_ALIGN_CENTER);
    file_page_glass_style(path_row, UI_PANEL_HL, LV_OPA_40,
                          UI_TEXT, LV_OPA_40, UI_PRIMARY, LV_OPA_20, 10, 14);
    lv_obj_set_style_pad_left(path_row, 12, 0);
    lv_obj_set_style_pad_right(path_row, 12, 0);
    lv_obj_set_style_pad_column(path_row, 8, 0);

    ui_label_create(path_row, LV_SYMBOL_SD_CARD, &lv_font_montserrat_16, UI_SECONDARY);
    s_file_ui.path_label = ui_label_create(path_row, FILE_SCAN_DIR, &lv_font_montserrat_16, UI_TEXT);
    lv_label_set_long_mode(s_file_ui.path_label, LV_LABEL_LONG_MODE_DOTS);
    ui_flex_spacer_create(path_row);
    s_file_ui.count_label = ui_label_create(path_row, "-- ITEMS", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(s_file_ui.count_label, 100);
    lv_obj_set_style_text_align(s_file_ui.count_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *head = ui_flex_container_create(panel,
                                              LV_PCT(100),
                                              28,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(head, 50, 0);
    lv_obj_set_style_pad_right(head, 12, 0);
    ui_label_create(head, "NAME", &lv_font_montserrat_12, UI_MUTED);
    ui_flex_spacer_create(head);
    lv_obj_t *format_head = ui_label_create(head, "FORMAT", &lv_font_montserrat_12, UI_MUTED);
    lv_obj_set_width(format_head, 86);
    lv_obj_set_style_text_align(format_head, LV_TEXT_ALIGN_RIGHT, 0);

    s_file_ui.list_cont = lv_obj_create(panel);
    lv_obj_set_size(s_file_ui.list_cont, LV_PCT(100), LV_PCT(90));
    /* Keep the scrolling surface opaque so each frame does not blend the
     * full-screen background image underneath every file row. */
    lv_obj_set_style_bg_color(s_file_ui.list_cont, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_file_ui.list_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_file_ui.list_cont, 12, 0);
    lv_obj_set_style_border_width(s_file_ui.list_cont, 0, 0);
    lv_obj_set_style_shadow_width(s_file_ui.list_cont, 0, 0);
    lv_obj_set_style_pad_all(s_file_ui.list_cont, 0, 0);
    lv_obj_set_style_pad_row(s_file_ui.list_cont, 4, 0);
    lv_obj_set_flex_flow(s_file_ui.list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_file_ui.list_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_file_ui.list_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_file_ui.list_cont, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(s_file_ui.list_cont, LV_SCROLLBAR_MODE_OFF);

    file_page_viewer_create(s_file_ui.root);

    int shown = file_page_scan_dir(s_file_current_path);
    file_page_empty_show(shown);
}

static void file_page_leave(Page *page)
{
    if (page)
    {
        lv_async_call(app_page_destroy_async_cb, (void *)(uintptr_t)page->id);
    }
}

static void file_page_destroy(Page *page)
{
    (void)page;
    file_page_image_buf_free();
    memset(&s_file_ui, 0, sizeof(s_file_ui));
}

const char *file_page_get_current_path(void)
{
    return s_file_current_path;
}

void file_page_register(void)
{
    Page page = {
        .id = PAGE_FILE,
        .name = "file",
        .on_create = file_page_create,
        .on_leave = file_page_leave,
        .on_destroy = file_page_destroy,
        .on_gesture = app_page_back_gesture,
    };
    ui_page_register(&page);
}
