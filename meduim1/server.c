#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "meduim.h"

volatile sig_atomic_t server_stop = 0;

struct server_options
{
    int daemon_mode;
    uint16_t port;
    const char *group;
    const char *media_dir;
};

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

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-d] [-m media_dir] [-g multicast_group] [-p port]\n",
            prog);
}

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    long value;

    if (text == NULL || port == NULL)
        return -1;

    value = strtol(text, &end, 10);
    if (*text == '\0' || *end != '\0' || value < 1 || value > 65535)
        return -1;

    *port = (uint16_t)value;
    return 0;
}

static int resolve_media_dir(const char *input_dir, char *resolved, size_t resolved_size)
{
    if (resolved == NULL || resolved_size == 0)
        return -1;

    if (input_dir != NULL)
    {
        if (realpath(input_dir, resolved) == NULL)
            return -1;
        return 0;
    }

    if (realpath("meduim_data", resolved) != NULL)
        return 0;

    if (realpath("./meduim1/meduim_data", resolved) != NULL)
        return 0;

    return -1;
}

static int parse_args(int argc, char *argv[], struct server_options *opts)
{
    int ch;

    if (opts == NULL)
        return -1;

    memset(opts, 0, sizeof(*opts));
    opts->group = MYGRUOP;
    opts->media_dir = NULL;
    if (parse_port(PORT, &opts->port) < 0)
        return -1;

    while ((ch = getopt(argc, argv, "dhm:g:p:")) != -1)
    {
        switch (ch)
        {
        case 'd':
            opts->daemon_mode = 1;
            break;
        case 'm':
            opts->media_dir = optarg;
            break;
        case 'g':
            opts->group = optarg;
            break;
        case 'p':
            if (parse_port(optarg, &opts->port) < 0)
            {
                fprintf(stderr, "invalid port: %s\n", optarg);
                return -1;
            }
            break;
        case 'h':
        default:
            usage(argv[0]);
            return -1;
        }
    }

    return 0;
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

int main(int argc, char *argv[])
{
    char media_dir[PATH_MAX];
    struct server_options opts;
    int channel_count;
    int i;
    int ret;
    int thread_num = 0;
    pthread_t tid[CHANAL_NUM] = { 0 };
    char *send_buff_list = NULL;
    int list_pkt_len = -1;

    if (parse_args(argc, argv, &opts) < 0)
        exit(1);

    if (resolve_media_dir(opts.media_dir, media_dir, sizeof(media_dir)) < 0)
    {
        perror("resolve media_dir");
        exit(1);
    }

    setup_signal();

    if (opts.daemon_mode && daemonize() < 0)
    {
        perror("daemonize");
        exit(1);
    }

    channel_count = load_channels(media_dir);
    if (channel_count <= 0)
    {
        fprintf(stderr, "no valid channels found in %s\n", media_dir);
        buff_destory();
        exit(1);
    }

    sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0)
    {
        perror("socket");
        buff_destory();
        exit(1);
    }

    memset(&ser_sock, 0, sizeof(ser_sock));
    ser_sock.sin_family = AF_INET;
    ser_sock.sin_port = htons(opts.port);

    ret = inet_pton(AF_INET, opts.group, &ser_sock.sin_addr.s_addr);
    if (ret <= 0)
    {
        fprintf(stderr, "invalid multicast group: %s\n", opts.group);
        close(sfd);
        buff_destory();
        exit(1);
    }

    for (i = 0; i < channel_count; i++)
    {
        uint32_t audio_rate = chanal_buff[i]->bitrate_bytes_per_sec;
        uint32_t burst = audio_rate;

        if (burst < AUDIO_CHUNK_SIZE)
            burst = AUDIO_CHUNK_SIZE;

        tbf_arr[i] = token_init(burst, audio_rate);
        if (tbf_arr[i] == NULL)
        {
            fprintf(stderr, "token_init failed for chanal %d\n", i + 1);
            server_stop = 1;
            break;
        }

        ret = pthread_create(&tid[i], NULL, send_chanal, chanal_buff[i]);
        if (ret != 0)
        {
            fprintf(stderr, "pthread_create failed for chanal %d\n", i + 1);
            token_alldestry(tbf_arr[i]);
            tbf_arr[i] = NULL;
            server_stop = 1;
            break;
        }

        thread_num++;
    }

    send_buff_list = malloc(MAX_LIST_ST);
    if (send_buff_list == NULL)
    {
        fprintf(stderr, "malloc send_buff_list error\n");
        server_stop = 1;
    }

    if (!server_stop)
    {
        list_pkt_len = build_list_packet(send_buff_list, MAX_LIST_ST);
        if (list_pkt_len < 0)
        {
            fprintf(stderr, "build_list_packet error\n");
            server_stop = 1;
        }
    }

    while (!server_stop)
    {
        ret = sendto(sfd, send_buff_list, list_pkt_len, 0,
                     (struct sockaddr *)&ser_sock, sizeof(ser_sock));
        if (ret < 0)
            perror("sendto list");

        for (i = 0; i < 5 && !server_stop; i++)
            sleep(1);
    }

    for (i = 0; i < thread_num; i++)
    {
        if (tid[i] != 0)
            pthread_join(tid[i], NULL);
    }

    free(send_buff_list);
    buff_destory();

    if (sfd >= 0)
        close(sfd);

    return 0;
}
