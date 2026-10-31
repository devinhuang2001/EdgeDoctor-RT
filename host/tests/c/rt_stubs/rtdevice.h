#ifndef ED_TEST_RTDEVICE_H
#define ED_TEST_RTDEVICE_H
#include "rtthread.h"
struct rt_i2c_bus_device { int unused; };
typedef void *rt_device_t;
struct rt_i2c_msg { rt_uint16_t addr, flags, len; rt_uint8_t *buf; };
#define RT_I2C_WR 0
#define RT_I2C_RD 1
#define RT_I2C_DEV_CTRL_CLK 1
rt_device_t rt_device_find(const char *name);
int rt_device_control(rt_device_t device, int cmd, void *value);
int rt_i2c_transfer(struct rt_i2c_bus_device *bus, struct rt_i2c_msg *messages, unsigned count);
#endif
