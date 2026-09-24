#include "ui_internal.h"
#include "file_page.h"

static const char *TAG = "[UI][file_page]";

static void file_page_create(Page *page)
{
   
}

static void file_page_enter(Page *page)
{
    
}

static void file_page_leave(Page *page)
{

}

static void file_page_destroy(Page *page)
{

}

static void file_page_gesture(Page *page, GestureDirection direction)
{

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
