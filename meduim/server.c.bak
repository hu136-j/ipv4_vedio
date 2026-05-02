#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
    int i;
    int ret;
    int daemon_mode = 0;
    int thread_num = 0;
    pthread_t tid[CHANAL_NUM] = {0};
    char *send_buff_list = NULL;
    int list_pkt_len;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], SERVER_DAEMON_ARG) == 0)
        {
            daemon_mode = 1;
            continue;
        }

        arg_dir = argv[i];
    }

    setup_signal();

    file_name = resolve_media_dir(arg_dir);
    if (file_name == NULL)
    {
        fprintf(stderr,
                "media directory not found, use ./meduim_data, ./meduim/meduim_data or pass a path explicitly\n");
        exit(1);
    }

    if (daemon_mode)
    {
        if (daemonize() < 0)
        {
            perror("daemonize()");
            exit(1);
        }
    }

    get_list(file_name);
    chanale_init(file_name);
    fprintf(stdout, "media dir: %s\n", file_name);

    sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0)
    {
        perror("socket()");
        exit(1);
    }

    {
        int val = MULTICAST_ALL_ENABLED;
        ret = setsockopt(sfd, IPPROTO_IP, IP_MULTICAST_ALL, &val, sizeof(val));
        if (ret < 0)
        {
            perror("setsockopt()");
            close(sfd);
            exit(1);
        }
    }

    memset(&ser_sock, 0, sizeof(ser_sock));
    ser_sock.sin_family = AF_INET;

    ret = inet_pton(AF_INET, MYGRUOP, &ser_sock.sin_addr.s_addr);
    if (ret <= 0)
    {
        fprintf(stderr, "inet_pton ip error\n");
        close(sfd);
        exit(1);
    }

    ser_sock.sin_port = htons(atoi(PORT));

    for (i = 0; i < CHANAL_NUM && chanal_buff[i] != NULL; i++)
    {
        tbf_arr[i] = token_init(TOKEN_BURST_BYTES, TOKEN_REFILL_BYTES_PER_TICK);
        if (tbf_arr[i] == NULL)
        {
            fprintf(stderr, "token_init failed for chanal %d\n", i + 1);
            server_stop = 1;
            break;
        }

        ret = pthread_create(&tid[i], NULL, send_chanal, (void*)chanal_buff[i]);
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

    send_buff_list = malloc(sizeof(char) * MAX_LIST_ST);
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
                     (struct sockaddr*)&ser_sock, sizeof(ser_sock));
        if (ret < 0)
            perror("sendto list");

        for (i = 0; i < LIST_BROADCAST_INTERVAL_SEC && !server_stop; i++)
            sleep(1);
    }

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

    return 0;
}
