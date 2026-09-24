#include "ui_internal.h"
#include "record_page.h"

static const char *TAG = "[UI][record_page]";

static void record_page_create(Page *page)
{
   
}

static void record_page_enter(Page *page)
{
    
}

static void record_page_leave(Page *page)
{

}

static void record_page_destroy(Page *page)
{

}

static void record_page_gesture(Page *page, GestureDirection direction)
{

}

static void record_page_timer(Page *page)
{

}

void record_page_register(void)
{
    Page page = {
        .id = PAGE_RECORD,
        .name = "record",
        .on_create = record_page_create,
        .on_leave = record_page_leave,
        .on_destroy = record_page_destroy,
        .on_gesture = app_page_back_gesture,
        .page_timer = record_page_timer,
        .timer_interval_ms = 100,
    };
    ui_page_register(&page);
}
