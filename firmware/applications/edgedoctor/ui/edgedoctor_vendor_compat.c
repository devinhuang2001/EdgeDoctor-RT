#include "edgedoctor_emoji_compat.h"

/* XiaoZhi's speaker adapter keeps this legacy notification call. */
void qday_show_emoji_by_rtt_info(int index)
{
    (void)index;
}

/* The official touch port references EdgePet even when that optional app is
 * not part of the EdgeDoctor image.  A weak no-op keeps the vendor port ABI;
 * an EdgePet build can provide its existing strong implementation. */
__attribute__((weak)) void edgepet_app_touch(void)
{
}
