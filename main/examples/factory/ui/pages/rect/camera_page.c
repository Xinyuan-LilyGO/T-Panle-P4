#include "ui_internal.h"
#include "camera_page.h"

static const char *TAG = "[UI][camera_page]";

static void camera_page_create(Page *page)
{
   
}

static void camera_page_enter(Page *page)
{
    
}

static void camera_page_leave(Page *page)
{

}

static void camera_page_destroy(Page *page)
{

}

void camera_page_register(void)
{
    Page page = {
        .id = PAGE_CAMERA,
        .name = "camera",
        .on_create = camera_page_create,
        .on_enter = camera_page_enter,
        .on_leave = camera_page_leave,
        .on_destroy = camera_page_destroy,
        .on_gesture = app_page_back_gesture,
    };
    ui_page_register(&page);
}
