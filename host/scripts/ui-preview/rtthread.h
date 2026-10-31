#ifndef PREVIEW_RTTHREAD_H
#define PREVIEW_RTTHREAD_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef unsigned char rt_uint8_t;
typedef intptr_t rt_ssize_t;
struct rt_messagequeue { unsigned entry; };
#define RT_EOK 0
#define RT_WAITING_NO 0
#define RT_IPC_FLAG_FIFO 0
#define RT_IPC_FLAG_PRIO 0
#define rt_snprintf snprintf
#define rt_kprintf printf
static inline int rt_mq_init(struct rt_messagequeue *q, const char *n, void *p, size_t s, size_t c, int f) { (void)n;(void)p;(void)s;(void)c;(void)f;q->entry=0;return 0; }
static inline int rt_mq_send(struct rt_messagequeue *q, const void *p, size_t s) { (void)q;(void)p;(void)s;return -1; }
static inline rt_ssize_t rt_mq_recv(struct rt_messagequeue *q, void *p, size_t s, int t) { (void)q;(void)p;(void)s;(void)t;return 0; }
#endif
