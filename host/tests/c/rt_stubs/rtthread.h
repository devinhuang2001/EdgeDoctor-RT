#ifndef ED_TEST_RTTHREAD_H
#define ED_TEST_RTTHREAD_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
typedef uint8_t rt_uint8_t;
typedef uint16_t rt_uint16_t;
typedef uint32_t rt_uint32_t;
typedef int rt_base_t;
typedef int rt_ssize_t;
typedef int rt_bool_t;
typedef unsigned rt_size_t;
struct rt_mutex { int unused; };
struct rt_thread { int unused; };
typedef struct rt_thread *rt_thread_t;
struct rt_messagequeue { unsigned char *pool; size_t size, capacity, entry; };
#define RT_NULL NULL
#define RT_EOK 0
#define RT_ERROR 1
#define RT_TRUE 1
#define RT_FALSE 0
#define RT_WAITING_NO 0
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_FIFO 0
#define RT_IPC_FLAG_PRIO 0
#define RT_IPC_CMD_RESET 0
#define RT_THREAD_PRIORITY_MAX 32
#define RT_TICK_PER_SECOND 1000
static inline uint32_t rt_tick_get(void) { return 1; }
#define INIT_APP_EXPORT(fn) typedef int ed_export_##fn
#define rt_snprintf snprintf
#define rt_kprintf printf
extern jmp_buf ed_test_idle;
static inline int rt_mq_init(struct rt_messagequeue *q, const char *name, void *pool, size_t size, size_t bytes, int flags)
{ (void)name; (void)flags; q->pool=pool; q->size=size; q->capacity=bytes/size; q->entry=0; return 0; }
static inline int rt_mq_send(struct rt_messagequeue *q, const void *value, size_t size)
{ if (size != q->size || q->entry == q->capacity) return -1; memcpy(q->pool + q->entry++ * size,value,size); return 0; }
static inline int rt_mq_recv(struct rt_messagequeue *q, void *value, size_t size, int wait)
{ if (!q->entry) { if (wait == RT_WAITING_FOREVER) longjmp(ed_test_idle,1); return -1; }
  memcpy(value,q->pool,size); --q->entry; memmove(q->pool,q->pool+size,q->entry*size); return (int)size; }
static inline int rt_mq_control(struct rt_messagequeue *q,int cmd,void *arg)
{ (void)cmd; (void)arg; q->entry=0; return 0; }
static inline int rt_mutex_init(struct rt_mutex *m,const char *n,int flags) { (void)m;(void)n;(void)flags;return 0; }
static inline int rt_mutex_take(struct rt_mutex *m,int wait) { (void)m;(void)wait;return 0; }
static inline int rt_mutex_release(struct rt_mutex *m) { (void)m;return 0; }
static inline int rt_thread_init(struct rt_thread *t,const char *n,void (*fn)(void *),void *p,void *s,size_t size,int priority,int tick)
{ (void)t;(void)n;(void)fn;(void)p;(void)s;(void)size;(void)priority;(void)tick;return 0; }
static inline int rt_thread_startup(struct rt_thread *t) { (void)t;return 0; }
static inline void rt_thread_mdelay(unsigned ms) { (void)ms; }
static inline int rt_tick_from_millisecond(unsigned ms) { return (int)ms; }
static inline rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static inline void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
#endif
