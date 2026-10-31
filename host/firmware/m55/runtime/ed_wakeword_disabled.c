#include <rtthread.h>

/* EdgeDoctor variant does not own a wake-word model or microphone reader.
 * Preserve the XiaoZhi ABI without reporting a disabled feature as started. */
int xz_wakeword_init(void) { return -RT_ENOSYS; }
int xz_wakeword_deinit(void) { return RT_EOK; }
rt_bool_t xz_wakeword_is_enabled(void) { return RT_FALSE; }
int xz_wakeword_start(void) { return -RT_ENOSYS; }
int xz_wakeword_stop(void) { return RT_EOK; }
void xz_wakeword_info(void)
{
    rt_kprintf("wakeword: disabled in EdgeDoctor variant\n");
}
