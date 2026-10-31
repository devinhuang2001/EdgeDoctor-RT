#include "edgedoctor_logger.h"

#include "edgedoctor_csv.h"

#include <dfs_posix.h>
#include <fcntl.h>
#include <rtthread.h>
#include <string.h>
#include <unistd.h>

#define LOGGER_QUEUE_CAPACITY 16U
#define LOGGER_STACK_BYTES 4096U
#define LOGGER_PATH_MAX 128U

typedef struct
{
    struct rt_messagequeue queue;
    uint8_t queue_pool[
        RT_ALIGN(sizeof(edgedoctor_result_t), RT_ALIGN_SIZE) * LOGGER_QUEUE_CAPACITY];
    struct rt_thread thread;
    rt_uint8_t stack[LOGGER_STACK_BYTES];
    char path[LOGGER_PATH_MAX];
    edgedoctor_logger_stats_t stats;
    uint8_t initialized;
} logger_state_t;

static logger_state_t logger;

static int write_exact(int descriptor, const char *data, size_t bytes)
{
    size_t written = 0U;
    while (written < bytes)
    {
        int count = write(descriptor, data + written, bytes - written);
        if (count <= 0)
        {
            return 0;
        }
        written += (size_t)count;
    }
    return 1;
}

static int open_log(void)
{
    int descriptor = open(logger.path, O_WRONLY | O_CREAT | O_APPEND, 0);
    if (descriptor >= 0)
    {
        off_t length = lseek(descriptor, 0, SEEK_END);
        if (length == 0)
        {
            const char *header = edgedoctor_csv_header();
            if (!write_exact(descriptor, header, strlen(header)))
            {
                close(descriptor);
                return -1;
            }
        }
    }
    return descriptor;
}

static void logger_entry(void *parameter)
{
    edgedoctor_result_t result;
    int descriptor = -1;
    (void)parameter;
    while (1)
    {
        char row[EDGEDOCTOR_CSV_ROW_MAX];
        int length;
        if (rt_mq_recv(&logger.queue, &result, sizeof(result), RT_WAITING_FOREVER) != RT_EOK)
        {
            continue;
        }
        if (descriptor < 0)
        {
            descriptor = open_log();
        }
        length = edgedoctor_csv_format(row, sizeof(row), &result);
        if ((descriptor < 0) || (length <= 0) ||
            !write_exact(descriptor, row, (size_t)length))
        {
            logger.stats.write_errors++;
            if (descriptor >= 0)
            {
                close(descriptor);
                descriptor = -1;
            }
            continue;
        }
        logger.stats.written++;
    }
}

int edgedoctor_logger_init(const char *path)
{
    size_t length;
    rt_err_t status;
    if (path == RT_NULL)
    {
        return -1;
    }
    length = strlen(path);
    if ((length == 0U) || (length >= sizeof(logger.path)))
    {
        return -1;
    }
    memset(&logger, 0, sizeof(logger));
    memcpy(logger.path, path, length + 1U);
    status = rt_mq_init(&logger.queue,
                        "edge_log_q",
                        logger.queue_pool,
                        sizeof(edgedoctor_result_t),
                        sizeof(logger.queue_pool),
                        RT_IPC_FLAG_FIFO);
    if (status != RT_EOK)
    {
        return -1;
    }
    status = rt_thread_init(&logger.thread,
                            "edge_log",
                            logger_entry,
                            RT_NULL,
                            logger.stack,
                            sizeof(logger.stack),
                            /* Keep CSV persistence responsive without running
                             * at audio/sensor priority. */
                            18U,
                            10U);
    if ((status != RT_EOK) || (rt_thread_startup(&logger.thread) != RT_EOK))
    {
        return -1;
    }
    logger.initialized = 1U;
    return 0;
}

int edgedoctor_logger_submit(const edgedoctor_result_t *result)
{
    if ((logger.initialized == 0U) || (result == RT_NULL))
    {
        return -1;
    }
    if (rt_mq_send(&logger.queue, result, sizeof(*result)) != RT_EOK)
    {
        logger.stats.dropped++;
        return 0;
    }
    logger.stats.queued++;
    return 1;
}

void edgedoctor_logger_stats(edgedoctor_logger_stats_t *stats)
{
    if (stats != RT_NULL)
    {
        *stats = logger.stats;
    }
}

const char *edgedoctor_logger_path(void)
{
    return (logger.initialized != 0U) ? logger.path : RT_NULL;
}
