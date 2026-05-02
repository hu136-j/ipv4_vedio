#ifndef LOGGER_H__
#define LOGGER_H__

#include <stdio.h>
#include <time.h>
#include <pthread.h>

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3
} log_level_t;

struct logger_config {
    log_level_t min_level;
    FILE *output;
    int use_color;
    int show_timestamp;
    int show_thread_id;
};

int logger_init(const struct logger_config *config);
void logger_destroy(void);
void logger_set_level(log_level_t level);
log_level_t logger_get_level(void);

void logger_log(log_level_t level, const char *module, const char *fmt, ...);

#define LOG_DEBUG(module, ...) logger_log(LOG_LEVEL_DEBUG, module, __VA_ARGS__)
#define LOG_INFO(module, ...)  logger_log(LOG_LEVEL_INFO, module, __VA_ARGS__)
#define LOG_WARN(module, ...)  logger_log(LOG_LEVEL_WARN, module, __VA_ARGS__)
#define LOG_ERROR(module, ...) logger_log(LOG_LEVEL_ERROR, module, __VA_ARGS__)

#endif
