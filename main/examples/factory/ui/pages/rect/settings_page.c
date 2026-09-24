#include "ui_internal.h"
#include "settings_page.h"

#include "esp_flash.h"
#include "display_dim.h"

static const char *TAG = "[UI][settings_page]";

void display_dim_timer_start(void)
{
    /* Rectangular builds do not expose the settings-page dimming controls. */
}

static void set_page_create(Page *page)
{
   
}

static void set_page_enter(Page *page)
{
    
}

static void set_page_leave(Page *page)
{

}

static void set_page_destroy(Page *page)
{

}

static void set_page_gesture(Page *page, GestureDirection direction)
{

}

static void set_page_timer(Page *page)
{

}

void settings_page_register(void)
{
    Page page = {
        .id = PAGE_SET,
        .name = "set",
        .on_create = set_page_create,
        .on_leave = set_page_leave,
        .on_destroy = set_page_destroy,
        .on_gesture = app_page_back_gesture,
        .page_timer = set_page_timer,
        .timer_interval_ms = 1000,
    };
    ui_page_register(&page);
}
