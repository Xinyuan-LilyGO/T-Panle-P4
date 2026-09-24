#pragma once

/* Starts the shared display-dimming watchdog. Rect builds may provide a no-op. */
void display_dim_timer_start(void);
