#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

static struct logger_config g_logger_config = {
    .min_level = LOG_LEVEL_INFO,
    .output = NULL,
    .use_color = 0,
    .show_timestamp = 1,
    .show_thread_id = 0
};

static pthread_mutex_t g_logger_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_names[] = {
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR"
};

static const char *level_colors[] = {
    "\033[36m",
    "\033[32m",
    "\033[33m",
    "\033[31m"
};

static const char *color_reset = "\033[0m";

int logger_init(const struct logger_config *config)
{
    pthread_mutex_lock(&g_logger_mutex);

    if (config != NULL)
    {
        g_logger_config.min_level = config->min_level;
        g_logger_config.output = config->output ? config->output : stdout;
        g_logger_config.use_color = config->use_color;
        g_logger_config.show_timestamp = config->show_timestamp;
        g_logger_config.show_thread_id = config->show_thread_id;
    }
    else
    {
        g_logger_config.output = stdout;
    }

    if (g_logger_config.use_color && !isatty(fileno(g_logger_config.output)))
        g_logger_config.use_color = 0;

    pthread_mutex_unlock(&g_logger_mutex);
    return 0;
}

void logger_destroy(void)
{
    pthread_mutex_lock(&g_logger_mutex);

    if (g_logger_config.output != NULL &&
        g_logger_config.output != stdout &&
        g_logger_config.output != stderr)
    {
        fclose(g_logger_config.output);
    }

    g_logger_config.output = NULL;
    pthread_mutex_unlock(&g_logger_mutex);
}

void logger_set_level(log_level_t level)
{
    pthread_mutex_lock(&g_logger_mutex);
    g_logger_config.min_level = level;
    pthread_mutex_unlock(&g_logger_mutex);
}

log_level_t logger_get_level(void)
{
    log_level_t level;
    pthread_mutex_lock(&g_logger_mutex);
    level = g_logger_config.min_level;
    pthread_mutex_unlock(&g_logger_mutex);
    return level;
}

void logger_log(log_level_t level, const char *module, const char *fmt, ...)
{
    char timestamp[32];
    time_t now;
    struct tm tm_info;
    va_list args;
    pthread_t tid;

    pthread_mutex_lock(&g_logger_mutex);

    if (level < g_logger_config.min_level || g_logger_config.output == NULL)
    {
        pthread_mutex_unlock(&g_logger_mutex);
        return;
    }

    if (g_logger_config.show_timestamp)
    {
        now = time(NULL);
        localtime_r(&now, &tm_info);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_info);
        fprintf(g_logger_config.output, "[%s] ", timestamp);
    }

    if (g_logger_config.use_color)
        fprintf(g_logger_config.output, "%s", level_colors[level]);

    fprintf(g_logger_config.output, "[%s]", level_names[level]);

    if (g_logger_config.use_color)
        fprintf(g_logger_config.output, "%s", color_reset);

    if (g_logger_config.show_thread_id)
    {
        tid = pthread_self();
        fprintf(g_logger_config.output, "[%lu]", (unsigned long)tid);
    }

    if (module != NULL && module[0] != '\0')
        fprintf(g_logger_config.output, "[%s]", module);

    fprintf(g_logger_config.output, " ");

    va_start(args, fmt);
    vfprintf(g_logger_config.output, fmt, args);
    va_end(args);

    fprintf(g_logger_config.output, "\n");
    fflush(g_logger_config.output);

    pthread_mutex_unlock(&g_logger_mutex);
}
