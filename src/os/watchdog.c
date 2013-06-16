#include <unistd.h>

#include "os/watchdog.h"
#include "os/os.h"


static int watchdog_period_msec;
static int watchdog_ticks = 0;

//! Thread running a watchdog.
static void* watchdog_thread(void* arg)
{
    while (1)
    {
        usleep(((useconds_t)1000) * watchdog_period_msec);
        if (++watchdog_ticks > 1)
        {
#ifdef __FreeRTOS__
            diewith(BLINK_DIE_WATCHDOG);
#else
            abort();
#endif
        }
    }
    return NULL;
}

void start_watchdog(int period_msec)
{
    watchdog_period_msec = period_msec;
    reset_watchdog();
#ifdef __FreeRTOS__
    const int kStackSize = 256;
#else
    const int kStackSize = 2048;
#endif
    os_thread_create(NULL, "watchdog", 0, kStackSize,
                     &watchdog_thread, NULL);
}

void reset_watchdog(void)
{
    watchdog_ticks = 0;
}

static long long watchdog_reset_timer(void* unused1, void* unused2)
{
    reset_watchdog();
    return OS_TIMER_RESTART;
}

void add_watchdog_reset_timer(int period_msec)
{
    os_timer_start(os_timer_create(&watchdog_reset_timer, NULL, NULL),
                   MSEC_TO_NSEC(period_msec));
}
