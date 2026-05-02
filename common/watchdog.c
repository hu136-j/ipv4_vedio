#include "watchdog.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/watchdog.h>
#include <time.h>

#define MODULE_NAME "watchdog"
#define DEFAULT_DEVICE "/dev/watchdog"

struct watchdog {
    watchdog_config_t config;
    int fd;                     /* 设备文件描述符 */
    int running;                /* 是否运行中 */
    struct timespec last_feed;  /* 上次喂狗时间 */
    uint32_t actual_timeout;    /* 实际超时时间 */
};

/* 获取单调时钟时间 */
static int get_monotonic_time(struct timespec *ts) {
    if (clock_gettime(CLOCK_MONOTONIC, ts) != 0) {
        LOG_ERROR(MODULE_NAME, "clock_gettime failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* 计算时间差（毫秒） */
static uint64_t timespec_diff_ms(const struct timespec *start, const struct timespec *end) {
    uint64_t diff_sec = end->tv_sec - start->tv_sec;
    int64_t diff_nsec = end->tv_nsec - start->tv_nsec;
    return diff_sec * 1000 + diff_nsec / 1000000;
}

watchdog_t* watchdog_create(const watchdog_config_t *config) {
    watchdog_t *wdt = calloc(1, sizeof(watchdog_t));
    if (!wdt) {
        LOG_ERROR(MODULE_NAME, "failed to allocate watchdog");
        return NULL;
    }

    if (config) {
        wdt->config = *config;
    } else {
        /* 默认配置 */
        wdt->config.type = WATCHDOG_TYPE_HARDWARE;
        wdt->config.device_path = DEFAULT_DEVICE;
        wdt->config.timeout_sec = 0;  /* 使用默认值 */
        wdt->config.feed_interval_ms = 5000;  /* 5秒喂一次 */
        wdt->config.enable_magic_close = 1;
    }

    if (!wdt->config.device_path) {
        wdt->config.device_path = DEFAULT_DEVICE;
    }

    wdt->fd = -1;
    wdt->running = 0;

    LOG_INFO(MODULE_NAME, "watchdog created (type=%s, device=%s, feed_interval=%ums)",
             wdt->config.type == WATCHDOG_TYPE_HARDWARE ? "hardware" : "software",
             wdt->config.device_path,
             wdt->config.feed_interval_ms);

    return wdt;
}

void watchdog_destroy(watchdog_t *wdt) {
    if (!wdt) {
        return;
    }

    if (wdt->running) {
        watchdog_stop(wdt);
    }

    LOG_INFO(MODULE_NAME, "watchdog destroyed");
    free(wdt);
}

int watchdog_start(watchdog_t *wdt) {
    if (!wdt) {
        return -1;
    }

    if (wdt->running) {
        LOG_WARN(MODULE_NAME, "watchdog already running");
        return 0;
    }

    if (wdt->config.type == WATCHDOG_TYPE_HARDWARE) {
        /* 打开硬件看门狗设备 */
        wdt->fd = open(wdt->config.device_path, O_WRONLY);
        if (wdt->fd < 0) {
            LOG_ERROR(MODULE_NAME, "failed to open %s: %s",
                      wdt->config.device_path, strerror(errno));
            return -1;
        }

        /* 设置超时时间 */
        if (wdt->config.timeout_sec > 0) {
            int timeout = wdt->config.timeout_sec;
            if (ioctl(wdt->fd, WDIOC_SETTIMEOUT, &timeout) != 0) {
                LOG_WARN(MODULE_NAME, "failed to set timeout: %s", strerror(errno));
            } else {
                wdt->actual_timeout = timeout;
                LOG_INFO(MODULE_NAME, "watchdog timeout set to %u seconds", timeout);
            }
        }

        /* 获取实际超时时间 */
        int timeout;
        if (ioctl(wdt->fd, WDIOC_GETTIMEOUT, &timeout) == 0) {
            wdt->actual_timeout = timeout;
            LOG_INFO(MODULE_NAME, "watchdog actual timeout: %u seconds", timeout);
        }

        /* 获取设备信息 */
        struct watchdog_info info;
        if (ioctl(wdt->fd, WDIOC_GETSUPPORT, &info) == 0) {
            LOG_INFO(MODULE_NAME, "watchdog device: %s (options=0x%x)",
                     info.identity, info.options);
        }
    }

    wdt->running = 1;
    get_monotonic_time(&wdt->last_feed);

    LOG_INFO(MODULE_NAME, "watchdog started");
    return 0;
}

int watchdog_stop(watchdog_t *wdt) {
    if (!wdt) {
        return -1;
    }

    if (!wdt->running) {
        return 0;
    }

    if (wdt->config.type == WATCHDOG_TYPE_HARDWARE && wdt->fd >= 0) {
        if (wdt->config.enable_magic_close) {
            /* 写入魔术字符 'V' 以安全关闭看门狗 */
            if (write(wdt->fd, "V", 1) != 1) {
                LOG_WARN(MODULE_NAME, "failed to write magic close character");
            } else {
                LOG_INFO(MODULE_NAME, "magic close character written");
            }
        } else {
            LOG_WARN(MODULE_NAME, "closing watchdog without magic close - system will reboot!");
        }

        close(wdt->fd);
        wdt->fd = -1;
    }

    wdt->running = 0;
    LOG_INFO(MODULE_NAME, "watchdog stopped");
    return 0;
}

int watchdog_feed(watchdog_t *wdt) {
    if (!wdt) {
        return -1;
    }

    if (!wdt->running) {
        LOG_WARN(MODULE_NAME, "watchdog not running");
        return -1;
    }

    if (wdt->config.type == WATCHDOG_TYPE_HARDWARE && wdt->fd >= 0) {
        /* 写入任意字符喂狗 */
        if (ioctl(wdt->fd, WDIOC_KEEPALIVE, 0) != 0) {
            /* 如果 ioctl 不支持，尝试写入 */
            if (write(wdt->fd, "\0", 1) != 1) {
                LOG_ERROR(MODULE_NAME, "failed to feed watchdog: %s", strerror(errno));
                return -1;
            }
        }
    }

    get_monotonic_time(&wdt->last_feed);
    LOG_DEBUG(MODULE_NAME, "watchdog fed");

    return 0;
}

int watchdog_should_feed(watchdog_t *wdt) {
    if (!wdt) {
        return -1;
    }

    if (!wdt->running) {
        return 0;
    }

    struct timespec now;
    if (get_monotonic_time(&now) != 0) {
        return -1;
    }

    uint64_t elapsed_ms = timespec_diff_ms(&wdt->last_feed, &now);

    return elapsed_ms >= wdt->config.feed_interval_ms ? 1 : 0;
}

int watchdog_get_timeout(watchdog_t *wdt) {
    if (!wdt) {
        return -1;
    }

    if (wdt->config.type == WATCHDOG_TYPE_HARDWARE && wdt->fd >= 0) {
        int timeout;
        if (ioctl(wdt->fd, WDIOC_GETTIMEOUT, &timeout) == 0) {
            return timeout;
        }
    }

    return wdt->actual_timeout;
}

int watchdog_set_timeout(watchdog_t *wdt, uint32_t timeout_sec) {
    if (!wdt) {
        return -1;
    }

    if (wdt->config.type == WATCHDOG_TYPE_HARDWARE && wdt->fd >= 0) {
        int timeout = timeout_sec;
        if (ioctl(wdt->fd, WDIOC_SETTIMEOUT, &timeout) != 0) {
            LOG_ERROR(MODULE_NAME, "failed to set timeout: %s", strerror(errno));
            return -1;
        }

        wdt->actual_timeout = timeout;
        LOG_INFO(MODULE_NAME, "watchdog timeout set to %u seconds", timeout);
        return 0;
    }

    wdt->config.timeout_sec = timeout_sec;
    wdt->actual_timeout = timeout_sec;
    return 0;
}

int watchdog_get_timeleft(watchdog_t *wdt) {
    if (!wdt) {
        return -1;
    }

    if (wdt->config.type == WATCHDOG_TYPE_HARDWARE && wdt->fd >= 0) {
        int timeleft;
        if (ioctl(wdt->fd, WDIOC_GETTIMELEFT, &timeleft) == 0) {
            return timeleft;
        }
    }

    /* 软件看门狗或不支持 GETTIMELEFT，估算剩余时间 */
    struct timespec now;
    if (get_monotonic_time(&now) != 0) {
        return -1;
    }

    uint64_t elapsed_ms = timespec_diff_ms(&wdt->last_feed, &now);
    uint64_t timeout_ms = wdt->actual_timeout * 1000;

    if (elapsed_ms >= timeout_ms) {
        return 0;
    }

    return (timeout_ms - elapsed_ms) / 1000;
}

int watchdog_is_running(watchdog_t *wdt) {
    if (!wdt) {
        return 0;
    }

    return wdt->running;
}
