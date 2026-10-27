#include "device.h"
#include "log.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

static int opens;
static int closes;
static int log_argument_evaluations;

static int evaluated_log_argument(void)
{
    return ++log_argument_evaluations;
}

static int32_t test_open(void **context)
{
    ++opens;
    *context = &opens;
    return 0;
}

static void test_close(void *context)
{
    assert(context == &opens);
    ++closes;
}

static int32_t test_ioctl(void *context, uint32_t request, const void *input,
                          uint32_t input_size, void *output,
                          uint32_t output_size)
{
    assert(context == &opens);
    assert(request == 0x123u);
    assert(input_size == sizeof(uint32_t));
    assert(output_size == sizeof(uint32_t));
    memcpy(output, input, sizeof(uint32_t));
    return -7;
}

int main(void)
{
    static const primeshim_device_driver_t driver = {
        .name = "TEST",
        .open = test_open,
        .close = test_close,
        .io_control = test_ioctl
    };
    uint32_t input = 0xfeedbeefu;
    uint32_t output = 0;
    uint32_t result = 0;
    uint32_t handle;

    primeshim_log_set_level(PRIMESHIM_LOG_OFF);
    PS_LOG_TRACE("test", "suppressed argument %d", evaluated_log_argument());
    assert(log_argument_evaluations == 0);
    primeshim_log_set_level(PRIMESHIM_LOG_WARN);
    PS_LOG_DEBUG("test", "suppressed argument %d", evaluated_log_argument());
    assert(log_argument_evaluations == 0);
    primeshim_log_set_level(PRIMESHIM_LOG_OFF);
    assert(device_driver_register(&driver) == 0);
    assert(device_driver_register(&driver) == -1 && errno == EEXIST);
    assert(device_open("\\\\?\\missing") == 0 && errno == ENODEV);

    handle = device_open("\\\\?\\test");
    assert(handle != 0);
    assert(opens == 1);
    assert(device_io_control(handle, 0x123u, &input, sizeof(input), &output,
                             sizeof(output), &result) == -7);
    assert(output == input);
    assert(result == (uint32_t)-7);
    assert(device_close(handle) == 1);
    assert(closes == 1);
    assert(device_close(handle) == 0 && errno == EBADF);
    return 0;
}
