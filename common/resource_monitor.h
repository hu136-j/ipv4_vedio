#ifndef RESOURCE_MONITOR_H
#define RESOURCE_MONITOR_H

#include <stdint.h>
#include <time.h>

/* 资源使用统计 */
typedef struct {
    float cpu_usage_percent;        /* CPU 使用率 (0-100) */
    uint64_t mem_total_kb;          /* 总内存 (KB) */
    uint64_t mem_available_kb;      /* 可用内存 (KB) */
    float mem_usage_percent;        /* 内存使用率 (0-100) */
    uint64_t net_rx_bytes;          /* 网络接收字节数 */
    uint64_t net_tx_bytes;          /* 网络发送字节数 */
    uint64_t net_rx_bps;            /* 接收速率 (bytes/sec) */
    uint64_t net_tx_bps;            /* 发送速率 (bytes/sec) */
    time_t timestamp;               /* 采样时间戳 */
} resource_stats_t;

/* 资源监控器 */
typedef struct resource_monitor resource_monitor_t;

/* 资源等级 */
typedef enum {
    RESOURCE_LEVEL_CRITICAL = 0,    /* 严重不足 */
    RESOURCE_LEVEL_LOW,             /* 资源紧张 */
    RESOURCE_LEVEL_NORMAL,          /* 正常 */
    RESOURCE_LEVEL_ABUNDANT         /* 充足 */
} resource_level_t;

/* 自适应策略 */
typedef struct {
    int jitter_buffer_size;         /* 建议的 jitter buffer 大小 */
    int max_bitrate_kbps;           /* 建议的最大比特率 (kbps) */
    int drop_threshold;             /* 丢包阈值 */
    int enable_buffering;           /* 是否启用额外缓冲 */
} adaptive_policy_t;

/* 资源监控配置 */
typedef struct {
    int sample_interval_ms;         /* 采样间隔 (毫秒) */
    float cpu_critical_threshold;   /* CPU 严重阈值 (%) */
    float cpu_low_threshold;        /* CPU 紧张阈值 (%) */
    float mem_critical_threshold;   /* 内存严重阈值 (%) */
    float mem_low_threshold;        /* 内存紧张阈值 (%) */
    int enable_adaptive;            /* 是否启用自适应 */
} resource_monitor_config_t;

/**
 * 创建资源监控器
 * @param config 配置参数
 * @return 监控器实例，失败返回 NULL
 */
resource_monitor_t* resource_monitor_create(const resource_monitor_config_t *config);

/**
 * 销毁资源监控器
 * @param monitor 监控器实例
 */
void resource_monitor_destroy(resource_monitor_t *monitor);

/**
 * 更新资源统计信息
 * @param monitor 监控器实例
 * @return 0 成功，-1 失败
 */
int resource_monitor_update(resource_monitor_t *monitor);

/**
 * 获取当前资源统计
 * @param monitor 监控器实例
 * @param stats 输出统计信息
 * @return 0 成功，-1 失败
 */
int resource_monitor_get_stats(resource_monitor_t *monitor, resource_stats_t *stats);

/**
 * 获取当前资源等级
 * @param monitor 监控器实例
 * @return 资源等级
 */
resource_level_t resource_monitor_get_level(resource_monitor_t *monitor);

/**
 * 获取自适应策略建议
 * @param monitor 监控器实例
 * @param policy 输出策略建议
 * @return 0 成功，-1 失败
 */
int resource_monitor_get_policy(resource_monitor_t *monitor, adaptive_policy_t *policy);

/**
 * 获取资源等级描述字符串
 * @param level 资源等级
 * @return 描述字符串
 */
const char* resource_level_to_string(resource_level_t level);

#endif /* RESOURCE_MONITOR_H */
