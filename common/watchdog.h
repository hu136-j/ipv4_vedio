#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>

/* 看门狗类型 */
typedef enum {
    WATCHDOG_TYPE_HARDWARE,     /* 硬件看门狗 (/dev/watchdog) */
    WATCHDOG_TYPE_SOFTWARE      /* 软件看门狗（仅监控） */
} watchdog_type_t;

/* 看门狗配置 */
typedef struct {
    watchdog_type_t type;       /* 看门狗类型 */
    const char *device_path;    /* 硬件看门狗设备路径，默认 /dev/watchdog */
    uint32_t timeout_sec;       /* 超时时间（秒），0 表示使用默认值 */
    uint32_t feed_interval_ms;  /* 喂狗间隔（毫秒） */
    int enable_magic_close;     /* 是否启用魔术关闭（写入 'V' 后关闭） */
} watchdog_config_t;

/* 看门狗实例 */
typedef struct watchdog watchdog_t;

/**
 * 创建并初始化看门狗
 * @param config 配置参数
 * @return 看门狗实例，失败返回 NULL
 */
watchdog_t* watchdog_create(const watchdog_config_t *config);

/**
 * 销毁看门狗
 * @param wdt 看门狗实例
 */
void watchdog_destroy(watchdog_t *wdt);

/**
 * 启动看门狗
 * @param wdt 看门狗实例
 * @return 0 成功，-1 失败
 */
int watchdog_start(watchdog_t *wdt);

/**
 * 停止看门狗
 * @param wdt 看门狗实例
 * @return 0 成功，-1 失败
 */
int watchdog_stop(watchdog_t *wdt);

/**
 * 喂狗（重置看门狗定时器）
 * @param wdt 看门狗实例
 * @return 0 成功，-1 失败
 */
int watchdog_feed(watchdog_t *wdt);

/**
 * 检查是否需要喂狗
 * @param wdt 看门狗实例
 * @return 1 需要喂狗，0 不需要，-1 错误
 */
int watchdog_should_feed(watchdog_t *wdt);

/**
 * 获取看门狗超时时间
 * @param wdt 看门狗实例
 * @return 超时时间（秒），失败返回 -1
 */
int watchdog_get_timeout(watchdog_t *wdt);

/**
 * 设置看门狗超时时间
 * @param wdt 看门狗实例
 * @param timeout_sec 超时时间（秒）
 * @return 0 成功，-1 失败
 */
int watchdog_set_timeout(watchdog_t *wdt, uint32_t timeout_sec);

/**
 * 获取看门狗剩余时间
 * @param wdt 看门狗实例
 * @return 剩余时间（秒），失败返回 -1
 */
int watchdog_get_timeleft(watchdog_t *wdt);

/**
 * 检查看门狗是否运行
 * @param wdt 看门狗实例
 * @return 1 运行中，0 未运行
 */
int watchdog_is_running(watchdog_t *wdt);

#endif /* WATCHDOG_H */
