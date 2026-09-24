#include "ui_internal.h"
#include "music_page.h"

static const char *TAG = "[UI][music_page]";


static void music_page_create(Page *page)
{
   
}

static void music_page_enter(Page *page)
{
    
}

static void music_page_leave(Page *page)
{

}

static void music_page_destroy(Page *page)
{

}

static void music_page_gesture(Page *page, GestureDirection direction)
{

}

static void music_page_timer(Page *page)
{

}

void music_page_register(void)
{
    Page page = {
        .id = PAGE_MUSIC,
        .name = "music",
        .on_create = music_page_create,
        .on_enter = music_page_enter,
        .on_leave = music_page_leave,
        .on_destroy = music_page_destroy,
        .on_gesture = app_page_back_gesture,
        .page_timer = music_page_timer,
        .timer_interval_ms = 50,
    };
    ui_page_register(&page);
}
