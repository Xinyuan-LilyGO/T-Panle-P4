#include "ui_internal.h"
#include "sensor_page.h"

static const char *TAG = "[UI][sensor_page]";

static void sensor_page_create(Page *page)
{
   
}

static void sensor_page_enter(Page *page)
{
    
}

static void sensor_page_leave(Page *page)
{

}

static void sensor_page_destroy(Page *page)
{

}

void sensor_page_register(void)
{
    Page page = {
        .id = PAGE_SENSOR,
        .name = "sensor",
        .on_create = sensor_page_create,
        .on_enter = sensor_page_enter,
        .on_leave = sensor_page_leave,
        .on_destroy = sensor_page_destroy,
        .on_gesture = app_page_back_gesture,
    };
    ui_page_register(&page);
}
