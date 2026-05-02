#include "resource_monitor.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define MODULE_NAME "resource_monitor"

struct resource_monitor {
    resource_monitor_config_t config;
    resource_stats_t current_stats;
    resource_stats_t prev_stats;
    resource_level_t current_level;
    int initialized;
};

/* 读取 CPU 使用率 */
static int read_cpu_usage(float *cpu_percent) {
    static unsigned long long prev_total = 0;
    static unsigned long long prev_idle = 0;

    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        LOG_ERROR(MODULE_NAME, "failed to open /proc/stat: %s", strerror(errno));
        return -1;
    }

    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    if (fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) != 8) {
        fclose(fp);
        LOG_ERROR(MODULE_NAME, "failed to parse /proc/stat");
        return -1;
    }
    fclose(fp);

    unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal;

    if (prev_total == 0) {
        /* 首次调用，无法计算使用率 */
        prev_total = total;
        prev_idle = idle;
        *cpu_percent = 0.0f;
        return 0;
    }

    unsigned long long total_diff = total - prev_total;
    unsigned long long idle_diff = idle - prev_idle;

    if (total_diff == 0) {
        *cpu_percent = 0.0f;
    } else {
        *cpu_percent = 100.0f * (1.0f - (float)idle_diff / (float)total_diff);
    }

    prev_total = total;
    prev_idle = idle;

    return 0;
}

/* 读取内存使用情况 */
static int read_memory_usage(uint64_t *total_kb, uint64_t *available_kb, float *usage_percent) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        LOG_ERROR(MODULE_NAME, "failed to open /proc/meminfo: %s", strerror(errno));
        return -1;
    }

    char line[256];
    uint64_t mem_total = 0, mem_free = 0, mem_available = 0;
    uint64_t buffers = 0, cached = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %lu kB", &mem_total) == 1) continue;
        if (sscanf(line, "MemFree: %lu kB", &mem_free) == 1) continue;
        if (sscanf(line, "MemAvailable: %lu kB", &mem_available) == 1) continue;
        if (sscanf(line, "Buffers: %lu kB", &buffers) == 1) continue;
        if (sscanf(line, "Cached: %lu kB", &cached) == 1) continue;
    }
    fclose(fp);

    if (mem_total == 0) {
        LOG_ERROR(MODULE_NAME, "failed to parse /proc/meminfo");
        return -1;
    }

    *total_kb = mem_total;

    /* 优先使用 MemAvailable，如果不存在则估算 */
    if (mem_available > 0) {
        *available_kb = mem_available;
    } else {
        *available_kb = mem_free + buffers + cached;
    }

    uint64_t used = mem_total - *available_kb;
    *usage_percent = 100.0f * (float)used / (float)mem_total;

    return 0;
}

/* 读取网络流量 */
static int read_network_stats(const char *interface, uint64_t *rx_bytes, uint64_t *tx_bytes) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes", interface);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        /* 接口不存在，尝试使用所有接口的总和 */
        *rx_bytes = 0;
        *tx_bytes = 0;

        fp = fopen("/proc/net/dev", "r");
        if (!fp) {
            LOG_ERROR(MODULE_NAME, "failed to open /proc/net/dev: %s", strerror(errno));
            return -1;
        }

        char line[256];
        /* 跳过前两行标题 */
        fgets(line, sizeof(line), fp);
        fgets(line, sizeof(line), fp);

        while (fgets(line, sizeof(line), fp)) {
            char iface[32];
            uint64_t rx, tx;
            unsigned long dummy;

            if (sscanf(line, "%31[^:]: %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                       iface, &rx, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &tx) >= 10) {
                /* 跳过 lo 接口 */
                if (strcmp(iface, "lo") != 0) {
                    *rx_bytes += rx;
                    *tx_bytes += tx;
                }
            }
        }
        fclose(fp);
        return 0;
    }

    if (fscanf(fp, "%lu", rx_bytes) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_bytes", interface);
    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    if (fscanf(fp, "%lu", tx_bytes) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return 0;
}

/* 计算资源等级 */
static resource_level_t calculate_resource_level(resource_monitor_t *monitor) {
    float cpu = monitor->current_stats.cpu_usage_percent;
    float mem = monitor->current_stats.mem_usage_percent;

    /* 任一资源达到严重阈值 */
    if (cpu >= monitor->config.cpu_critical_threshold ||
        mem >= monitor->config.mem_critical_threshold) {
        return RESOURCE_LEVEL_CRITICAL;
    }

    /* 任一资源达到紧张阈值 */
    if (cpu >= monitor->config.cpu_low_threshold ||
        mem >= monitor->config.mem_low_threshold) {
        return RESOURCE_LEVEL_LOW;
    }

    /* CPU 和内存都很充足 */
    if (cpu < monitor->config.cpu_low_threshold * 0.5f &&
        mem < monitor->config.mem_low_threshold * 0.5f) {
        return RESOURCE_LEVEL_ABUNDANT;
    }

    return RESOURCE_LEVEL_NORMAL;
}

/* 生成自适应策略 */
static void generate_adaptive_policy(resource_monitor_t *monitor, adaptive_policy_t *policy) {
    resource_level_t level = monitor->current_level;

    /* 默认策略 */
    policy->jitter_buffer_size = 64;
    policy->max_bitrate_kbps = 320;
    policy->drop_threshold = 10;
    policy->enable_buffering = 1;

    switch (level) {
    case RESOURCE_LEVEL_CRITICAL:
        /* 严重不足：最小化资源使用 */
        policy->jitter_buffer_size = 16;
        policy->max_bitrate_kbps = 64;
        policy->drop_threshold = 5;
        policy->enable_buffering = 0;
        LOG_WARN(MODULE_NAME, "resource critical, applying minimal policy");
        break;

    case RESOURCE_LEVEL_LOW:
        /* 资源紧张：降低质量 */
        policy->jitter_buffer_size = 32;
        policy->max_bitrate_kbps = 128;
        policy->drop_threshold = 8;
        policy->enable_buffering = 0;
        LOG_INFO(MODULE_NAME, "resource low, applying reduced policy");
        break;

    case RESOURCE_LEVEL_NORMAL:
        /* 正常：标准策略 */
        LOG_DEBUG(MODULE_NAME, "resource normal, applying standard policy");
        break;

    case RESOURCE_LEVEL_ABUNDANT:
        /* 充足：提升质量 */
        policy->jitter_buffer_size = 128;
        policy->max_bitrate_kbps = 512;
        policy->drop_threshold = 15;
        policy->enable_buffering = 1;
        LOG_DEBUG(MODULE_NAME, "resource abundant, applying enhanced policy");
        break;
    }
}

resource_monitor_t* resource_monitor_create(const resource_monitor_config_t *config) {
    resource_monitor_t *monitor = calloc(1, sizeof(resource_monitor_t));
    if (!monitor) {
        LOG_ERROR(MODULE_NAME, "failed to allocate monitor");
        return NULL;
    }

    if (config) {
        monitor->config = *config;
    } else {
        /* 默认配置 */
        monitor->config.sample_interval_ms = 1000;
        monitor->config.cpu_critical_threshold = 90.0f;
        monitor->config.cpu_low_threshold = 70.0f;
        monitor->config.mem_critical_threshold = 90.0f;
        monitor->config.mem_low_threshold = 75.0f;
        monitor->config.enable_adaptive = 1;
    }

    monitor->current_level = RESOURCE_LEVEL_NORMAL;
    monitor->initialized = 0;

    LOG_INFO(MODULE_NAME, "resource monitor created (cpu_crit=%.1f%%, mem_crit=%.1f%%)",
             monitor->config.cpu_critical_threshold,
             monitor->config.mem_critical_threshold);

    return monitor;
}

void resource_monitor_destroy(resource_monitor_t *monitor) {
    if (monitor) {
        LOG_INFO(MODULE_NAME, "resource monitor destroyed");
        free(monitor);
    }
}

int resource_monitor_update(resource_monitor_t *monitor) {
    if (!monitor) {
        return -1;
    }

    /* 保存上一次的统计 */
    monitor->prev_stats = monitor->current_stats;

    /* 更新时间戳 */
    monitor->current_stats.timestamp = time(NULL);

    /* 读取 CPU 使用率 */
    if (read_cpu_usage(&monitor->current_stats.cpu_usage_percent) != 0) {
        LOG_WARN(MODULE_NAME, "failed to read CPU usage");
    }

    /* 读取内存使用情况 */
    if (read_memory_usage(&monitor->current_stats.mem_total_kb,
                          &monitor->current_stats.mem_available_kb,
                          &monitor->current_stats.mem_usage_percent) != 0) {
        LOG_WARN(MODULE_NAME, "failed to read memory usage");
    }

    /* 读取网络流量 */
    uint64_t rx_bytes, tx_bytes;
    if (read_network_stats(NULL, &rx_bytes, &tx_bytes) == 0) {
        monitor->current_stats.net_rx_bytes = rx_bytes;
        monitor->current_stats.net_tx_bytes = tx_bytes;

        /* 计算速率 */
        if (monitor->initialized && monitor->prev_stats.timestamp > 0) {
            time_t time_diff = monitor->current_stats.timestamp - monitor->prev_stats.timestamp;
            if (time_diff > 0) {
                monitor->current_stats.net_rx_bps =
                    (rx_bytes - monitor->prev_stats.net_rx_bytes) / time_diff;
                monitor->current_stats.net_tx_bps =
                    (tx_bytes - monitor->prev_stats.net_tx_bytes) / time_diff;
            }
        }
    }

    /* 计算资源等级 */
    monitor->current_level = calculate_resource_level(monitor);

    monitor->initialized = 1;

    LOG_DEBUG(MODULE_NAME, "stats updated: cpu=%.1f%%, mem=%.1f%% (avail=%luMB), level=%s",
              monitor->current_stats.cpu_usage_percent,
              monitor->current_stats.mem_usage_percent,
              monitor->current_stats.mem_available_kb / 1024,
              resource_level_to_string(monitor->current_level));

    return 0;
}

int resource_monitor_get_stats(resource_monitor_t *monitor, resource_stats_t *stats) {
    if (!monitor || !stats) {
        return -1;
    }

    *stats = monitor->current_stats;
    return 0;
}

resource_level_t resource_monitor_get_level(resource_monitor_t *monitor) {
    if (!monitor) {
        return RESOURCE_LEVEL_NORMAL;
    }

    return monitor->current_level;
}

int resource_monitor_get_policy(resource_monitor_t *monitor, adaptive_policy_t *policy) {
    if (!monitor || !policy) {
        return -1;
    }

    if (!monitor->config.enable_adaptive) {
        /* 自适应未启用，返回默认策略 */
        policy->jitter_buffer_size = 64;
        policy->max_bitrate_kbps = 320;
        policy->drop_threshold = 10;
        policy->enable_buffering = 1;
        return 0;
    }

    generate_adaptive_policy(monitor, policy);
    return 0;
}

const char* resource_level_to_string(resource_level_t level) {
    switch (level) {
    case RESOURCE_LEVEL_CRITICAL:
        return "CRITICAL";
    case RESOURCE_LEVEL_LOW:
        return "LOW";
    case RESOURCE_LEVEL_NORMAL:
        return "NORMAL";
    case RESOURCE_LEVEL_ABUNDANT:
        return "ABUNDANT";
    default:
        return "UNKNOWN";
    }
}
