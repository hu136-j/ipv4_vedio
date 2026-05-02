#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>

#include "proto.h"
#include "token.h"
#include "meduim.h"
#include "config.h"
#include "logger.h"

#define MODULE_NAME "server"

#define SERVER_DAEMON_ARG            "-d"
#define MULTICAST_ALL_ENABLED        1
#define TOKEN_BURST_BYTES            12288
#define TOKEN_REFILL_BYTES_PER_TICK  1600
#define LIST_BROADCAST_INTERVAL_SEC  5

volatile sig_atomic_t server_stop = 0;

static void sig_handler(int signo)
{
    (void)signo;
    server_stop = 1;
}

static void setup_signal(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static int daemonize(void)
{
    pid_t pid;
    int fd;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        exit(0);

    if (setsid() < 0)
        return -1;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        exit(0);

    umask(0);
    chdir("/");

    fd = open("/dev/null", O_RDWR);
    if (fd < 0)
        return -1;

    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);

    if (fd > 2)
        close(fd);

    return 0;
}

static int is_readable_dir(const char *path)
{
    struct stat st;

    if (path == NULL)
        return 0;

    if (stat(path, &st) < 0)
        return 0;

    if (!S_ISDIR(st.st_mode))
        return 0;

    return access(path, R_OK | X_OK) == 0;
}

static const char *resolve_media_dir(const char *arg_dir)
{
    static char cwd_path[PATH_MAX];
    static char repo_path[PATH_MAX];

    if (arg_dir != NULL)
        return arg_dir;

    snprintf(cwd_path, sizeof(cwd_path), "./meduim_data");
    if (is_readable_dir(cwd_path))
        return cwd_path;

    snprintf(repo_path, sizeof(repo_path), "./meduim/meduim_data");
    if (is_readable_dir(repo_path))
        return repo_path;

    return NULL;
}

int main(int argc, char *argv[])
{
    const char *file_name = NULL;
    const char *arg_dir = NULL;
    const char *config_file = "./server.conf";
    config_t *config = NULL;
    struct logger_config logger_cfg;
    const char *log_level_str;
    const char *log_file_str;
    char multicast_group[INET_ADDRSTRLEN];
    uint16_t multicast_port;
    int token_burst_bytes = TOKEN_BURST_BYTES;
    int token_refill_bytes = TOKEN_REFILL_BYTES_PER_TICK;
    int list_broadcast_interval = LIST_BROADCAST_INTERVAL_SEC;
    int i;
    int ret;
    int daemon_mode = 0;
    int thread_num = 0;
    pthread_t tid[CHANAL_NUM] = {0};
    char *send_buff_list = NULL;
    int list_pkt_len;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
        {
            config_file = argv[++i];
            continue;
        }

        if (strcmp(argv[i], SERVER_DAEMON_ARG) == 0)
        {
            daemon_mode = 1;
            continue;
        }

        arg_dir = argv[i];
    }

    config = config_load(config_file);

    memset(&logger_cfg, 0, sizeof(logger_cfg));
    logger_cfg.min_level = LOG_LEVEL_INFO;
    logger_cfg.output = stdout;
    logger_cfg.use_color = 1;
    logger_cfg.show_timestamp = 1;
    logger_cfg.show_thread_id = 0;

    if (config != NULL)
    {
        log_level_str = config_get_string(config, "log_level", "INFO");
        if (strcasecmp(log_level_str, "DEBUG") == 0)
            logger_cfg.min_level = LOG_LEVEL_DEBUG;
        else if (strcasecmp(log_level_str, "INFO") == 0)
            logger_cfg.min_level = LOG_LEVEL_INFO;
        else if (strcasecmp(log_level_str, "WARN") == 0)
            logger_cfg.min_level = LOG_LEVEL_WARN;
        else if (strcasecmp(log_level_str, "ERROR") == 0)
            logger_cfg.min_level = LOG_LEVEL_ERROR;

        log_file_str = config_get_string(config, "log_file", "");
        if (log_file_str != NULL && log_file_str[0] != '\0')
        {
            logger_cfg.output = fopen(log_file_str, "a");
            if (logger_cfg.output == NULL)
            {
                fprintf(stderr, "failed to open log file %s, using stdout\n", log_file_str);
                logger_cfg.output = stdout;
            }
        }

        logger_cfg.use_color = config_get_int(config, "log_use_color", 1);
        logger_cfg.show_timestamp = config_get_int(config, "log_show_timestamp", 1);
        logger_cfg.show_thread_id = config_get_int(config, "log_show_thread_id", 0);

        daemon_mode = config_get_int(config, "daemon_mode", daemon_mode);
    }

    if (daemon_mode)
    {
        if (daemonize() < 0)
        {
            fprintf(stderr, "daemonize() failed\n");
            exit(1);
        }
        logger_cfg.output = NULL;
    }

    logger_init(&logger_cfg);
    LOG_INFO(MODULE_NAME, "server starting");

    if (config != NULL)
    {
        const char *media_dir_cfg = config_get_string(config, "media_dir", NULL);
        if (media_dir_cfg != NULL && arg_dir == NULL)
            arg_dir = media_dir_cfg;

        strncpy(multicast_group, config_get_string(config, "multicast_group", MYGRUOP),
                sizeof(multicast_group) - 1);
        multicast_group[sizeof(multicast_group) - 1] = '\0';

        multicast_port = config_get_uint16(config, "multicast_port", (uint16_t)atoi(PORT));
        token_burst_bytes = config_get_int(config, "token_burst_bytes", TOKEN_BURST_BYTES);
        token_refill_bytes = config_get_int(config, "token_refill_bytes_per_tick", TOKEN_REFILL_BYTES_PER_TICK);
        list_broadcast_interval = config_get_int(config, "list_broadcast_interval", LIST_BROADCAST_INTERVAL_SEC);

        config_free(config);
    }
    else
    {
        strncpy(multicast_group, MYGRUOP, sizeof(multicast_group) - 1);
        multicast_group[sizeof(multicast_group) - 1] = '\0';
        multicast_port = (uint16_t)atoi(PORT);
    }

    setup_signal();

    file_name = resolve_media_dir(arg_dir);
    if (file_name == NULL)
    {
        LOG_ERROR(MODULE_NAME, "media directory not found");
        exit(1);
    }

    LOG_INFO(MODULE_NAME, "media directory: %s", file_name);
    LOG_INFO(MODULE_NAME, "multicast: %s:%u", multicast_group, multicast_port);
    LOG_INFO(MODULE_NAME, "token bucket: burst=%d refill=%d", token_burst_bytes, token_refill_bytes);

    get_list(file_name);
    chanale_init(file_name);

    sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0)
    {
        LOG_ERROR(MODULE_NAME, "socket() failed: %s", strerror(errno));
        exit(1);
    }

    {
        int val = MULTICAST_ALL_ENABLED;
        ret = setsockopt(sfd, IPPROTO_IP, IP_MULTICAST_ALL, &val, sizeof(val));
        if (ret < 0)
        {
            LOG_ERROR(MODULE_NAME, "setsockopt() failed: %s", strerror(errno));
            close(sfd);
            exit(1);
        }
    }

    memset(&ser_sock, 0, sizeof(ser_sock));
    ser_sock.sin_family = AF_INET;

    ret = inet_pton(AF_INET, multicast_group, &ser_sock.sin_addr.s_addr);
    if (ret <= 0)
    {
        LOG_ERROR(MODULE_NAME, "inet_pton ip error: %s", multicast_group);
        close(sfd);
        exit(1);
    }

    ser_sock.sin_port = htons(multicast_port);

    for (i = 0; i < CHANAL_NUM && chanal_buff[i] != NULL; i++)
    {
        tbf_arr[i] = token_init(token_burst_bytes, token_refill_bytes);
        if (tbf_arr[i] == NULL)
        {
            LOG_ERROR(MODULE_NAME, "token_init failed for channel %d", i + 1);
            server_stop = 1;
            break;
        }

        ret = pthread_create(&tid[i], NULL, send_chanal, (void*)chanal_buff[i]);
        if (ret != 0)
        {
            LOG_ERROR(MODULE_NAME, "pthread_create failed for channel %d: %s", i + 1, strerror(ret));
            token_alldestry(tbf_arr[i]);
            tbf_arr[i] = NULL;
            server_stop = 1;
            break;
        }

        thread_num++;
    }

    LOG_INFO(MODULE_NAME, "started %d channel threads", thread_num);

    send_buff_list = malloc(sizeof(char) * MAX_LIST_ST);
    if (send_buff_list == NULL)
    {
        LOG_ERROR(MODULE_NAME, "malloc send_buff_list failed");
        server_stop = 1;
    }

    if (!server_stop)
    {
        list_pkt_len = build_list_packet(send_buff_list, MAX_LIST_ST);
        if (list_pkt_len < 0)
        {
            LOG_ERROR(MODULE_NAME, "build_list_packet failed");
            server_stop = 1;
        }
    }

    while (!server_stop)
    {
        ret = sendto(sfd, send_buff_list, list_pkt_len, 0,
                     (struct sockaddr*)&ser_sock, sizeof(ser_sock));
        if (ret < 0)
            LOG_ERROR(MODULE_NAME, "sendto list failed: %s", strerror(errno));

        for (i = 0; i < list_broadcast_interval && !server_stop; i++)
            sleep(1);
    }

    LOG_INFO(MODULE_NAME, "server shutting down");

    for (i = 0; i < thread_num; i++)
    {
        if (tid[i] != 0)
            pthread_join(tid[i], NULL);
    }

    for (i = 0; i < CHANAL_NUM; i++)
    {
        if (tbf_arr[i] != NULL)
        {
            token_alldestry(tbf_arr[i]);
            tbf_arr[i] = NULL;
        }
    }

    free(send_buff_list);
    buff_destory();

    if (sfd >= 0)
        close(sfd);

    logger_destroy();

    return 0;
}
