#include <rtthread.h>
#include <cy_pdl.h>
#include "ethosu_driver.h"

static struct ethosu_driver driver;
static struct rt_mutex driver_mutex;
static struct rt_semaphore semaphores[2];
static unsigned int semaphore_count;

void *ethosu_mutex_create(void)
{
    return rt_mutex_init(&driver_mutex, "u55lock", RT_IPC_FLAG_PRIO) == RT_EOK
        ? &driver_mutex : RT_NULL;
}
void ethosu_mutex_destroy(void *mutex) { rt_mutex_detach(mutex); }
int ethosu_mutex_lock(void *mutex) { return rt_mutex_take(mutex, RT_WAITING_FOREVER); }
int ethosu_mutex_unlock(void *mutex) { return rt_mutex_release(mutex); }
void *ethosu_semaphore_create(void)
{
    struct rt_semaphore *sem;
    if (semaphore_count >= 2) return RT_NULL;
    sem = &semaphores[semaphore_count++];
    return rt_sem_init(sem, "u55sem", 0, RT_IPC_FLAG_PRIO) == RT_EOK ? sem : RT_NULL;
}
void ethosu_semaphore_destroy(void *sem) { rt_sem_detach(sem); }
int ethosu_semaphore_give(void *sem) { return rt_sem_release(sem); }
int ethosu_semaphore_take(void *sem, uint64_t timeout)
{
    /* Driver timeout units are defined by the port as RT-Thread ticks. */
    rt_int32_t ticks = timeout == UINT64_MAX ? RT_WAITING_FOREVER :
        (timeout > INT32_MAX ? INT32_MAX : (rt_int32_t)timeout);
    return rt_sem_take(sem, ticks);
}
void ethosu_flush_dcache(uint32_t *address, size_t bytes)
{
    if (address) SCB_CleanDCache_by_Addr(address, (int32_t)bytes);
    else SCB_CleanDCache();
    __DSB();
}
void ethosu_invalidate_dcache(uint32_t *address, size_t bytes)
{
    if (address) SCB_InvalidateDCache_by_Addr(address, (int32_t)bytes);
    else SCB_CleanInvalidateDCache();
    __DSB();
}
static void npu_interrupt(void)
{
    rt_interrupt_enter();
    ethosu_irq_handler(&driver);
    rt_interrupt_leave();
}
int ed_npu_init(void)
{
    cy_stc_sysint_t interrupt = {mxu55_interrupt_npu_IRQn, 5};
    Cy_SysEnableU55(true);
    if (Cy_SysInt_Init(&interrupt, npu_interrupt) != CY_SYSINT_SUCCESS) return -1;
    if (ethosu_init(&driver, (void *)U550_BASE, RT_NULL, 0, 0, 1) != 0) return -1;
    NVIC_ClearPendingIRQ(mxu55_interrupt_npu_IRQn);
    NVIC_EnableIRQ(mxu55_interrupt_npu_IRQn);
    return 0;
}
