#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <ctype.h>
#include <getopt.h>
#include <strings.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>

#include "proto.h"
#include "config.h"
#include "logger.h"
#include "heartbeat.h"
#include "resource_monitor.h"
#include "watchdog.h"

#define MODULE_NAME "client"

#define RECV_BUF_SIZE        (NET_AUDIO_HDR_LEN + AUDIO_CHUNK_SIZE + 64)
#define JITTER_BUFFER_SIZE   64
#define START_BUFFER_PACKETS 6
#define SKIP_THRESHOLD       12

#define MAX_EVENTS           16
#define OUTBUF_CAPACITY      (1024 * 1024)
#define CONTROL_LINE_MAX     1024
#define PROGRAM_NAME_MAX     512
#define CONTROL_LISTEN_IP    "127.0.0.1"
#define DEFAULT_CONTROL_PORT 9000
#define CONTROL_LISTEN_BACKLOG 4
#define EPOLL_WAIT_TIMEOUT_MS 500
#define PLAYER_STARTUP_WAIT_US 100000
#define CHILD_EXIT_POLL_RETRIES 10
#define CHILD_EXIT_POLL_INTERVAL_US 20000
#define CONTROL_RECV_CHUNK_SIZE 256

#define FD_TAG_SOCK          1
#define FD_TAG_PIPE          2
#define FD_TAG_CTRL_LISTEN   3
#define FD_TAG_CTRL_CONN     4

#define INVALID_FD           (-1)
#define INVALID_PID          (-1)

struct audio_frame
{
    int valid;
    uint32_t seq;
    uint16_t payload_len;
    char data[AUDIO_CHUNK_SIZE];
};

struct jitter_buffer
{
    struct audio_frame frames[JITTER_BUFFER_SIZE];
    uint32_t expected_seq;
    uint32_t max_seq_seen;
    int is_first_packet;
    int started;
};

struct outbuf
{
    char *buf;
    size_t cap;
    size_t head;
    size_t tail;
};

struct optentry
{
    uint16_t udp_port;
    uint16_t control_port;
    char group[INET_ADDRSTRLEN];
    char control_listen_ip[INET_ADDRSTRLEN];
    const char *player_path;
    int auto_channel;
    int interactive_mode;
    int enable_control;
    int jitter_buffer_size;
    int start_buffer_packets;
    int skip_threshold;
    size_t outbuf_capacity;
    int heartbeat_timeout;
    int heartbeat_check_interval;
    int max_reconnect_attempts;
    int reconnect_delay_ms;
    /* 资源监控配置 */
    int enable_resource_monitor;
    int resource_sample_interval_ms;
    float resource_cpu_critical;
    float resource_cpu_low;
    float resource_mem_critical;
    float resource_mem_low;
    /* 看门狗配置 */
    int enable_watchdog;
    const char *watchdog_device;
    int watchdog_timeout_sec;
    int watchdog_feed_interval_ms;
};

struct fd_ctx
{
    int tag;
    int fd;
};

struct player_state
{
    pid_t pid;
    int pipe_wfd;
    struct fd_ctx pipe_ctx;
};

struct controller_state
{
    int listen_fd;
    int conn_fd;
    char recv_buf[CONTROL_LINE_MAX];
    size_t recv_len;
    struct fd_ctx listen_ctx;
    struct fd_ctx conn_ctx;
};

struct program_entry
{
    int valid;
    uint8_t id;
    char name[PROGRAM_NAME_MAX];
};

static struct jitter_buffer g_jitter_buf;
static struct outbuf g_outbuf;
static struct heartbeat_state g_heartbeat;
static struct player_state g_player = {
    .pid = INVALID_PID,
    .pipe_wfd = INVALID_FD,
    .pipe_ctx = { FD_TAG_PIPE, INVALID_FD }
};
static struct controller_state g_controller = {
    .listen_fd = INVALID_FD,
    .conn_fd = INVALID_FD,
    .recv_len = 0,
    .listen_ctx = { FD_TAG_CTRL_LISTEN, INVALID_FD },
    .conn_ctx = { FD_TAG_CTRL_CONN, INVALID_FD }
};
static struct program_entry g_programs[CHANAL_NUM];
static int g_program_count = 0;
static int g_epfd = INVALID_FD;
static int g_current_channel = 0;
static int g_desc_printed = 0;
static int g_list_announced = 0;
static volatile sig_atomic_t g_stop = 0;

/* 资源监控和看门狗 */
static resource_monitor_t *g_resource_monitor = NULL;
static watchdog_t *g_watchdog = NULL;

static void signal_handler(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-f config_file] [-g multicast_ip] [-p udp_port] [-t tcp_port] [-P player] [-c channel] [-i] [-n]\n"
            "  -f config file path, default ./client.conf\n"
            "  -g multicast group, default %s\n"
            "  -p UDP port, default %s\n"
            "  -t TCP control port, default %d\n"
            "  -P player program, default mpg123\n"
            "  -c start playing a channel immediately\n"
            "  -i interactive channel selection after first list packet\n"
            "  -n disable TCP control server\n",
            prog, MYGRUOP, PORT, DEFAULT_CONTROL_PORT);
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return -1;

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;

    return 0;
}

static int epoll_add_fd(int epfd, int fd, uint32_t events, void *ptr)
{
    struct epoll_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.ptr = ptr;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

static int epoll_mod_fd(int epfd, int fd, uint32_t events, void *ptr)
{
    struct epoll_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.ptr = ptr;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

static void epoll_del_fd_if_needed(int epfd, int fd)
{
    if (epfd >= 0 && fd >= 0)
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}

static void outbuf_init(struct outbuf *ob, size_t cap)
{
    ob->buf = malloc(cap);
    if (ob->buf == NULL)
    {
        fprintf(stderr, "malloc outbuf error\n");
        exit(1);
    }

    ob->cap = cap;
    ob->head = 0;
    ob->tail = 0;
}

static void outbuf_clear(struct outbuf *ob)
{
    ob->head = 0;
    ob->tail = 0;
}

static int outbuf_empty(const struct outbuf *ob)
{
    return ob->head == ob->tail;
}

static void outbuf_compact(struct outbuf *ob)
{
    if (ob->head == 0)
        return;

    if (ob->head == ob->tail)
    {
        outbuf_clear(ob);
        return;
    }

    memmove(ob->buf, ob->buf + ob->head, ob->tail - ob->head);
    ob->tail -= ob->head;
    ob->head = 0;
}

static int outbuf_append(struct outbuf *ob, const char *data, size_t len)
{
    if (len > ob->cap)
        return -1;

    if (ob->cap - ob->tail < len)
        outbuf_compact(ob);

    if (ob->cap - ob->tail < len)
        return -1;

    memcpy(ob->buf + ob->tail, data, len);
    ob->tail += len;
    return 0;
}

static int player_active(void)
{
    return g_player.pipe_wfd >= 0;
}

static int update_pipe_epoll_events(void)
{
    uint32_t events = EPOLLERR | EPOLLHUP;

    if (!player_active())
        return 0;

    if (!outbuf_empty(&g_outbuf))
        events |= EPOLLOUT;

    return epoll_mod_fd(g_epfd, g_player.pipe_wfd, events, &g_player.pipe_ctx);
}

static int flush_outbuf_to_pipe(void)
{
    while (player_active() && !outbuf_empty(&g_outbuf))
    {
        ssize_t ret = write(g_player.pipe_wfd,
                            g_outbuf.buf + g_outbuf.head,
                            g_outbuf.tail - g_outbuf.head);
        if (ret > 0)
        {
            g_outbuf.head += (size_t)ret;
            if (g_outbuf.head == g_outbuf.tail)
            {
                outbuf_clear(&g_outbuf);
                return 0;
            }
            continue;
        }

        if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;

        if (ret < 0 && errno == EINTR)
            continue;

        perror("write player pipe");
        return -1;
    }

    return 0;
}

static void jitter_reset(void)
{
    memset(&g_jitter_buf, 0, sizeof(g_jitter_buf));
    g_jitter_buf.is_first_packet = 1;
}

static int queue_audio_to_player(const char *data, uint16_t len)
{
    if (!player_active())
        return 0;

    if (outbuf_append(&g_outbuf, data, len) < 0)
    {
        fprintf(stderr, "[Error] output buffer overflow, drop audio\n");
        return -1;
    }

    if (flush_outbuf_to_pipe() < 0)
        return -1;

    if (update_pipe_epoll_events() < 0)
    {
        perror("epoll_mod pipe");
        return -1;
    }

    return 0;
}

static void jitter_reset_slot(int idx)
{
    g_jitter_buf.frames[idx].valid = 0;
    g_jitter_buf.frames[idx].seq = 0;
    g_jitter_buf.frames[idx].payload_len = 0;
}

static int jitter_store_packet(uint32_t seq, const char *payload, uint16_t payload_len)
{
    int idx = (int)(seq % JITTER_BUFFER_SIZE);

    if (g_jitter_buf.frames[idx].valid && g_jitter_buf.frames[idx].seq != seq)
    {
        if (seq > g_jitter_buf.frames[idx].seq)
            jitter_reset_slot(idx);
        else
            return 0;
    }

    if (!g_jitter_buf.frames[idx].valid)
    {
        g_jitter_buf.frames[idx].seq = seq;
        g_jitter_buf.frames[idx].payload_len = payload_len;
        memcpy(g_jitter_buf.frames[idx].data, payload, payload_len);
        g_jitter_buf.frames[idx].valid = 1;
    }

    return 0;
}

static int jitter_should_start(void)
{
    if (g_jitter_buf.started)
        return 1;

    if (g_jitter_buf.max_seq_seen >= g_jitter_buf.expected_seq + (START_BUFFER_PACKETS - 1))
        return 1;

    return 0;
}

static int jitter_maybe_skip_missing(void)
{
    if (!g_jitter_buf.started)
        return 0;

    if (g_jitter_buf.max_seq_seen >= g_jitter_buf.expected_seq + SKIP_THRESHOLD)
    {
        fprintf(stderr, "[Warning] missing seq=%u, skip 1 packet\n",
                g_jitter_buf.expected_seq);
        g_jitter_buf.expected_seq++;
        return 1;
    }

    return 0;
}

static int jitter_drain_to_outbuf(void)
{
    int idx;

    if (!player_active())
        return 0;

    if (!jitter_should_start())
        return 0;

    g_jitter_buf.started = 1;

    while (1)
    {
        idx = (int)(g_jitter_buf.expected_seq % JITTER_BUFFER_SIZE);

        if (!g_jitter_buf.frames[idx].valid ||
            g_jitter_buf.frames[idx].seq != g_jitter_buf.expected_seq)
        {
            if (jitter_maybe_skip_missing())
                continue;
            break;
        }

        if (queue_audio_to_player(g_jitter_buf.frames[idx].data,
                                  g_jitter_buf.frames[idx].payload_len) < 0)
        {
            return -1;
        }

        jitter_reset_slot(idx);
        g_jitter_buf.expected_seq++;
    }

    return 0;
}

static int parse_audio_packet_and_queue(const char *buf, int len, uint8_t choose_id)
{
    uint8_t ch_id;
    uint8_t pkt_type;
    uint32_t net_seq;
    uint16_t net_payload_len;
    uint32_t seq;
    uint16_t payload_len;

    if (len < NET_AUDIO_HDR_LEN)
        return 0;

    ch_id = (uint8_t)buf[0];
    pkt_type = (uint8_t)buf[1];

    if (ch_id != choose_id || pkt_type != PKT_TYPE_AUDIO)
        return 0;

    memcpy(&net_seq, buf + 2, sizeof(net_seq));
    memcpy(&net_payload_len, buf + 6, sizeof(net_payload_len));

    seq = ntohl(net_seq);
    payload_len = ntohs(net_payload_len);

    if (len < NET_AUDIO_HDR_LEN + (int)payload_len)
        return 0;

    if (payload_len > AUDIO_CHUNK_SIZE)
        return 0;

    if (g_jitter_buf.is_first_packet)
    {
        g_jitter_buf.expected_seq = seq;
        g_jitter_buf.max_seq_seen = seq;
        g_jitter_buf.is_first_packet = 0;
    }

    if (seq > g_jitter_buf.max_seq_seen)
        g_jitter_buf.max_seq_seen = seq;

    if (seq < g_jitter_buf.expected_seq)
        return 0;

    if (seq >= g_jitter_buf.expected_seq + JITTER_BUFFER_SIZE)
    {
        uint32_t new_expected = seq - (JITTER_BUFFER_SIZE - 1);
        uint32_t s;

        for (s = g_jitter_buf.expected_seq; s < new_expected; s++)
        {
            int idx = (int)(s % JITTER_BUFFER_SIZE);
            if (g_jitter_buf.frames[idx].valid && g_jitter_buf.frames[idx].seq == s)
                jitter_reset_slot(idx);
        }

        fprintf(stderr, "[Warning] buffer move forward: %u -> %u\n",
                g_jitter_buf.expected_seq, new_expected);
        g_jitter_buf.expected_seq = new_expected;
    }

    if (jitter_store_packet(seq, buf + NET_AUDIO_HDR_LEN, payload_len) < 0)
        return -1;

    /* Update heartbeat on valid audio packet */
    heartbeat_update(&g_heartbeat);

    return jitter_drain_to_outbuf();
}

static int parse_desc_packet(const char *buf, int len, uint8_t choose_id)
{
    uint8_t ch_id;
    uint8_t pkt_type;
    uint16_t net_desc_len;
    uint16_t desc_len;

    if (len < NET_DESC_HDR_LEN)
        return 0;

    ch_id = (uint8_t)buf[0];
    pkt_type = (uint8_t)buf[1];

    if (ch_id != choose_id || pkt_type != PKT_TYPE_DESC)
        return 0;

    memcpy(&net_desc_len, buf + 2, sizeof(net_desc_len));
    desc_len = ntohs(net_desc_len);

    if (len < NET_DESC_HDR_LEN + (int)desc_len)
        return 0;

    if (!g_desc_printed)
    {
        printf("desc[%u]: %.*s\n", choose_id, desc_len, buf + NET_DESC_HDR_LEN);
        fflush(stdout);
        g_desc_printed = 1;
    }

    return 1;
}

static void clear_program_list(void)
{
    memset(g_programs, 0, sizeof(g_programs));
    g_program_count = 0;
}

static void print_program_list_stdout(void)
{
    int i;

    printf("Program list:\n");
    for (i = 0; i < CHANAL_NUM; i++)
    {
        if (!g_programs[i].valid)
            continue;

        printf("  %u  %s\n", g_programs[i].id, g_programs[i].name);
    }
    fflush(stdout);
}

static void update_program_list_from_packet(const char *buf, int len)
{
    const char *pos;
    int remain;

    if (len < NET_LIST_HDR_LEN)
        return;

    if ((uint8_t)buf[0] != LIST_ID || (uint8_t)buf[1] != PKT_TYPE_LIST)
        return;

    clear_program_list();

    pos = buf + NET_LIST_HDR_LEN;
    remain = len - NET_LIST_HDR_LEN;

    while (remain >= 3)
    {
        uint8_t ch_id;
        uint16_t net_name_len;
        uint16_t name_len;
        size_t copy_len;

        ch_id = (uint8_t)pos[0];
        memcpy(&net_name_len, pos + 1, sizeof(net_name_len));
        name_len = ntohs(net_name_len);

        if (remain < 3 + (int)name_len)
            break;

        if (ch_id >= MIN_CHANAL_ID && ch_id <= MAX_CHANAL_ID)
        {
            struct program_entry *entry = &g_programs[ch_id - 1];

            copy_len = name_len;
            if (copy_len >= sizeof(entry->name))
                copy_len = sizeof(entry->name) - 1;

            entry->valid = 1;
            entry->id = ch_id;
            memcpy(entry->name, pos + 3, copy_len);
            entry->name[copy_len] = '\0';
            g_program_count++;
        }

        pos += 3 + name_len;
        remain -= 3 + name_len;
    }

    if (!g_list_announced && g_program_count > 0)
    {
        printf("received %d program(s) from multicast server\n", g_program_count);
        fflush(stdout);
        g_list_announced = 1;
    }
}

static int program_exists(int channel)
{
    if (channel < MIN_CHANAL_ID || channel > MAX_CHANAL_ID)
        return 0;

    return g_programs[channel - 1].valid;
}

static int send_controller_text(const char *text)
{
    size_t len = strlen(text);
    size_t sent = 0;

    if (g_controller.conn_fd < 0)
        return 0;

    while (sent < len)
    {
        ssize_t ret = send(g_controller.conn_fd, text + sent, len - sent, 0);
        if (ret > 0)
        {
            sent += (size_t)ret;
            continue;
        }

        if (ret < 0 && errno == EINTR)
            continue;

        if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;

        perror("send controller");
        return -1;
    }

    return 0;
}

static int send_controllerf(const char *fmt, ...)
{
    char buf[CONTROL_LINE_MAX];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len < 0)
        return -1;

    if ((size_t)len >= sizeof(buf))
        len = (int)sizeof(buf) - 1;

    return send_controller_text(buf);
}

static void close_controller_conn(void)
{
    if (g_controller.conn_fd >= 0)
    {
        epoll_del_fd_if_needed(g_epfd, g_controller.conn_fd);
        close(g_controller.conn_fd);
    }

    g_controller.conn_fd = INVALID_FD;
    g_controller.conn_ctx.fd = INVALID_FD;
    g_controller.recv_len = 0;
}

static int wait_child_exit(pid_t pid)
{
    int i;
    int status;

    for (i = 0; i < CHILD_EXIT_POLL_RETRIES; i++)
    {
        pid_t ret = waitpid(pid, &status, WNOHANG);
        if (ret == pid)
            return 0;
        if (ret < 0 && errno != EINTR)
            return -1;
        usleep(CHILD_EXIT_POLL_INTERVAL_US);
    }

    kill(pid, SIGTERM);
    while (waitpid(pid, &status, 0) < 0)
    {
        if (errno == EINTR)
            continue;
        return -1;
    }

    return 0;
}

static void player_stop(void)
{
    pid_t pid = g_player.pid;

    outbuf_clear(&g_outbuf);
    epoll_del_fd_if_needed(g_epfd, g_player.pipe_wfd);

    if (g_player.pipe_wfd >= 0)
        close(g_player.pipe_wfd);

    g_player.pipe_wfd = INVALID_FD;
    g_player.pipe_ctx.fd = INVALID_FD;

    if (pid > 0)
        wait_child_exit(pid);

    g_player.pid = INVALID_PID;
}

static int player_check_alive(void)
{
    int status;

    if (g_player.pid <= 0)
        return 0;

    /* Check if player process is still alive */
    pid_t ret = waitpid(g_player.pid, &status, WNOHANG);
    if (ret == g_player.pid)
    {
        /* Player has exited */
        LOG_WARN(MODULE_NAME, "player process exited unexpectedly, pid=%d", g_player.pid);
        return 0;
    }

    return 1;
}

static int rejoin_multicast(int sfd, const struct optentry *opt)
{
    struct ip_mreqn mreq;

    LOG_INFO(MODULE_NAME, "attempting to rejoin multicast group %s", opt->group);

    /* Leave the multicast group first */
    memset(&mreq, 0, sizeof(mreq));
    if (inet_pton(AF_INET, opt->group, &mreq.imr_multiaddr.s_addr) <= 0)
    {
        LOG_ERROR(MODULE_NAME, "inet_pton group ip error: %s", opt->group);
        return -1;
    }

    mreq.imr_address.s_addr = htonl(INADDR_ANY);
    mreq.imr_ifindex = 0;

    /* Try to leave (ignore errors as we might not be in the group) */
    setsockopt(sfd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));

    /* Rejoin the multicast group */
    if (setsockopt(sfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    {
        LOG_ERROR(MODULE_NAME, "setsockopt IP_ADD_MEMBERSHIP failed: %s", strerror(errno));
        return -1;
    }

    LOG_INFO(MODULE_NAME, "successfully rejoined multicast group");
    return 0;
}

static int player_start(const char *player_path)
{
    int pipefd[2];
    pid_t pid;
    int status;

    if (pipe(pipefd) < 0)
    {
        perror("pipe");
        return -1;
    }

    pid = fork();
    if (pid < 0)
    {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0)
    {
        close(pipefd[1]);

        if (dup2(pipefd[0], STDIN_FILENO) < 0)
        {
            perror("dup2");
            _exit(1);
        }

        close(pipefd[0]);
        execlp(player_path, player_path, "-q", "-", NULL);
        perror("execlp");
        _exit(1);
    }

    close(pipefd[0]);

    if (set_nonblock(pipefd[1]) < 0)
    {
        perror("set_nonblock pipe");
        close(pipefd[1]);
        wait_child_exit(pid);
        return -1;
    }

    g_player.pid = pid;
    g_player.pipe_wfd = pipefd[1];
    g_player.pipe_ctx.fd = pipefd[1];

    if (epoll_add_fd(g_epfd, pipefd[1], EPOLLERR | EPOLLHUP, &g_player.pipe_ctx) < 0)
    {
        perror("epoll_add pipe");
        player_stop();
        return -1;
    }

    usleep(PLAYER_STARTUP_WAIT_US);
    if (waitpid(pid, &status, WNOHANG) == pid)
    {
        fprintf(stderr, "player exited immediately\n");
        player_stop();
        return -1;
    }

    return 0;
}

static void playback_buffers_reset(void)
{
    outbuf_clear(&g_outbuf);
    jitter_reset();
    g_desc_printed = 0;
}

static int start_playback(int channel, const struct optentry *opt)
{
    if (channel < MIN_CHANAL_ID || channel > MAX_CHANAL_ID)
        return -1;

    player_stop();
    playback_buffers_reset();

    if (player_start(opt->player_path) < 0)
    {
        g_current_channel = 0;
        LOG_ERROR(MODULE_NAME, "failed to start player for channel %d", channel);
        return -1;
    }

    g_current_channel = channel;
    LOG_INFO(MODULE_NAME, "playing channel %d", channel);
    printf("play channel %d\n", channel);
    fflush(stdout);
    return 0;
}

static void stop_playback(void)
{
    if (g_current_channel != 0)
    {
        LOG_INFO(MODULE_NAME, "stopping playback of channel %d", g_current_channel);
        printf("stop playback\n");
        fflush(stdout);
    }

    g_current_channel = 0;
    playback_buffers_reset();
    player_stop();
}

static int controller_send_programs(void)
{
    int i;

    if (g_program_count == 0)
    {
        if (send_controller_text("INFO waiting for multicast list\nEND\n") < 0)
            return -1;
        return 0;
    }

    for (i = 0; i < CHANAL_NUM; i++)
    {
        if (!g_programs[i].valid)
            continue;

        if (send_controllerf("PROGRAM %u %s\n",
                             g_programs[i].id,
                             g_programs[i].name) < 0)
        {
            return -1;
        }
    }

    return send_controller_text("END\n");
}

static void trim_line(char *line)
{
    char *start = line;
    char *end;

    while (*start != '\0' && isspace((unsigned char)*start))
        start++;

    if (start != line)
        memmove(line, start, strlen(start) + 1);

    end = line + strlen(line);
    while (end > line && isspace((unsigned char)*(end - 1)))
        end--;
    *end = '\0';
}

static int handle_control_command(char *line, const struct optentry *opt)
{
    trim_line(line);

    if (line[0] == '\0')
        return 0;

    if (strcasecmp(line, "LIST") == 0)
        return controller_send_programs();

    if (strcasecmp(line, "STOP") == 0)
    {
        stop_playback();
        return send_controller_text("OK STOP\n");
    }

    if (strcasecmp(line, "QUIT") == 0)
    {
        send_controller_text("BYE\n");
        close_controller_conn();
        return 0;
    }

    if (strncasecmp(line, "PLAY", 4) == 0)
    {
        char *arg = line + 4;
        char *endptr;
        long channel;

        while (*arg != '\0' && isspace((unsigned char)*arg))
            arg++;

        if (*arg == '\0')
            return send_controller_text("ERR PLAY requires channel id\n");

        errno = 0;
        channel = strtol(arg, &endptr, 10);
        if (errno != 0 || *endptr != '\0')
            return send_controller_text("ERR invalid channel id\n");

        if (channel < MIN_CHANAL_ID || channel > MAX_CHANAL_ID)
            return send_controller_text("ERR channel out of range\n");

        if (g_program_count > 0 && !program_exists((int)channel))
            return send_controller_text("ERR channel not in latest program list\n");

        if (start_playback((int)channel, opt) < 0)
            return send_controller_text("ERR failed to start player\n");

        return send_controllerf("OK PLAY %ld\n", channel);
    }

    return send_controller_text("ERR unsupported command\n");
}

static int accept_controller_connection(void)
{
    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    int conn_fd;

    conn_fd = accept(g_controller.listen_fd,
                     (struct sockaddr *)&peer_addr,
                     &peer_len);
    if (conn_fd < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        perror("accept");
        return -1;
    }

    if (set_nonblock(conn_fd) < 0)
    {
        perror("set_nonblock controller conn");
        close(conn_fd);
        return -1;
    }

    close_controller_conn();

    g_controller.conn_fd = conn_fd;
    g_controller.conn_ctx.fd = conn_fd;
    g_controller.recv_len = 0;

    if (epoll_add_fd(g_epfd, conn_fd, EPOLLIN | EPOLLERR | EPOLLHUP,
                     &g_controller.conn_ctx) < 0)
    {
        perror("epoll_add controller conn");
        close_controller_conn();
        return -1;
    }

    return send_controller_text("OK CONNECTED\n");
}

static int process_controller_input(const struct optentry *opt)
{
    char buf[CONTROL_RECV_CHUNK_SIZE];

    while (g_controller.conn_fd >= 0)
    {
        ssize_t ret = recv(g_controller.conn_fd, buf, sizeof(buf), 0);
        if (ret == 0)
        {
            close_controller_conn();
            return 0;
        }

        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            perror("recv controller");
            close_controller_conn();
            return -1;
        }

        if (g_controller.recv_len + (size_t)ret >= sizeof(g_controller.recv_buf))
        {
            send_controller_text("ERR command too long\n");
            g_controller.recv_len = 0;
            continue;
        }

        memcpy(g_controller.recv_buf + g_controller.recv_len, buf, (size_t)ret);
        g_controller.recv_len += (size_t)ret;
        g_controller.recv_buf[g_controller.recv_len] = '\0';

        while (1)
        {
            char *newline = memchr(g_controller.recv_buf, '\n', g_controller.recv_len);
            char line[CONTROL_LINE_MAX];
            size_t line_len;

            if (newline == NULL)
                break;

            line_len = (size_t)(newline - g_controller.recv_buf);
            if (line_len >= sizeof(line))
                line_len = sizeof(line) - 1;

            memcpy(line, g_controller.recv_buf, line_len);
            line[line_len] = '\0';

            memmove(g_controller.recv_buf,
                    newline + 1,
                    g_controller.recv_len - line_len - 1);
            g_controller.recv_len -= line_len + 1;

            if (handle_control_command(line, opt) < 0)
                return -1;

            if (g_controller.conn_fd < 0)
                return 0;
        }
    }

    return 0;
}

static int create_control_listener(const struct optentry *opt)
{
    struct sockaddr_in addr;
    int fd;
    int reuse = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        LOG_ERROR(MODULE_NAME, "socket tcp failed: %s", strerror(errno));
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        LOG_ERROR(MODULE_NAME, "setsockopt SO_REUSEADDR tcp failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(opt->control_port);
    if (inet_pton(AF_INET, opt->control_listen_ip, &addr.sin_addr) <= 0)
    {
        LOG_ERROR(MODULE_NAME, "invalid control listen ip: %s", opt->control_listen_ip);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_ERROR(MODULE_NAME, "bind tcp failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, CONTROL_LISTEN_BACKLOG) < 0)
    {
        LOG_ERROR(MODULE_NAME, "listen tcp failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (set_nonblock(fd) < 0)
    {
        LOG_ERROR(MODULE_NAME, "set_nonblock control listener failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int create_udp_receiver(const struct optentry *opt)
{
    struct sockaddr_in laddr;
    struct ip_mreqn mreq;
    int sfd;
    int reuse = 1;

    sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0)
    {
        perror("socket udp");
        return -1;
    }

    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        perror("setsockopt SO_REUSEADDR udp");
        close(sfd);
        return -1;
    }

    memset(&laddr, 0, sizeof(laddr));
    laddr.sin_family = AF_INET;
    laddr.sin_port = htons(opt->udp_port);
    laddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sfd, (struct sockaddr *)&laddr, sizeof(laddr)) < 0)
    {
        perror("bind udp");
        close(sfd);
        return -1;
    }

    memset(&mreq, 0, sizeof(mreq));
    if (inet_pton(AF_INET, opt->group, &mreq.imr_multiaddr.s_addr) <= 0)
    {
        fprintf(stderr, "inet_pton group ip error\n");
        close(sfd);
        return -1;
    }

    mreq.imr_address.s_addr = htonl(INADDR_ANY);
    mreq.imr_ifindex = 0;

    if (setsockopt(sfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    {
        perror("setsockopt IP_ADD_MEMBERSHIP");
        close(sfd);
        return -1;
    }

    return sfd;
}

static int wait_for_first_list_and_choose(int sfd, char *recv_buf)
{
    while (!g_stop)
    {
        int read_num = recvfrom(sfd, recv_buf, RECV_BUF_SIZE, 0, NULL, NULL);
        int channel;

        if (read_num < 0)
        {
            if (errno == EINTR)
                continue;
            perror("recvfrom list");
            return -1;
        }

        if (read_num < NET_LIST_HDR_LEN)
            continue;

        if ((uint8_t)recv_buf[0] != LIST_ID || (uint8_t)recv_buf[1] != PKT_TYPE_LIST)
            continue;

        update_program_list_from_packet(recv_buf, read_num);
        print_program_list_stdout();

        printf("choose id: ");
        fflush(stdout);

        if (scanf("%d", &channel) != 1)
        {
            fprintf(stderr, "scanf choose id error\n");
            return -1;
        }

        return channel;
    }

    return -1;
}

static int parse_args(int argc, char *argv[], struct optentry *opt, const char **config_file)
{
    int ch;

    *config_file = "./client.conf";

    memset(opt, 0, sizeof(*opt));
    opt->udp_port = (uint16_t)atoi(PORT);
    opt->control_port = DEFAULT_CONTROL_PORT;
    strncpy(opt->group, MYGRUOP, sizeof(opt->group) - 1);
    opt->group[sizeof(opt->group) - 1] = '\0';
    strncpy(opt->control_listen_ip, CONTROL_LISTEN_IP, sizeof(opt->control_listen_ip) - 1);
    opt->control_listen_ip[sizeof(opt->control_listen_ip) - 1] = '\0';
    opt->player_path = "mpg123";
    opt->auto_channel = 0;
    opt->interactive_mode = 0;
    opt->enable_control = 1;
    opt->jitter_buffer_size = JITTER_BUFFER_SIZE;
    opt->start_buffer_packets = START_BUFFER_PACKETS;
    opt->skip_threshold = SKIP_THRESHOLD;
    opt->outbuf_capacity = OUTBUF_CAPACITY;
    opt->heartbeat_timeout = 10;
    opt->heartbeat_check_interval = 500;
    opt->max_reconnect_attempts = 0;
    opt->reconnect_delay_ms = 2000;
    /* 资源监控默认配置 */
    opt->enable_resource_monitor = 0;
    opt->resource_sample_interval_ms = 1000;
    opt->resource_cpu_critical = 90.0f;
    opt->resource_cpu_low = 70.0f;
    opt->resource_mem_critical = 90.0f;
    opt->resource_mem_low = 75.0f;
    /* 看门狗默认配置 */
    opt->enable_watchdog = 0;
    opt->watchdog_device = "/dev/watchdog";
    opt->watchdog_timeout_sec = 30;
    opt->watchdog_feed_interval_ms = 10000;

    while ((ch = getopt(argc, argv, "f:g:p:t:P:c:inh")) != -1)
    {
        switch (ch)
        {
            case 'f':
                *config_file = optarg;
                break;
            case 'g':
                strncpy(opt->group, optarg, sizeof(opt->group) - 1);
                opt->group[sizeof(opt->group) - 1] = '\0';
                break;
            case 'p':
                opt->udp_port = (uint16_t)atoi(optarg);
                break;
            case 't':
                opt->control_port = (uint16_t)atoi(optarg);
                break;
            case 'P':
                opt->player_path = optarg;
                break;
            case 'c':
                opt->auto_channel = atoi(optarg);
                break;
            case 'i':
                opt->interactive_mode = 1;
                break;
            case 'n':
                opt->enable_control = 0;
                break;
            case 'h':
            default:
                usage(argv[0]);
                return -1;
        }
    }

    if (opt->udp_port == 0 || opt->control_port == 0)
    {
        usage(argv[0]);
        return -1;
    }

    if (opt->auto_channel != 0 &&
        (opt->auto_channel < MIN_CHANAL_ID || opt->auto_channel > MAX_CHANAL_ID))
    {
        fprintf(stderr, "channel id out of range\n");
        return -1;
    }

    return 0;
}

static int load_config(const char *config_file, struct optentry *opt)
{
    config_t *config;
    const char *str_val;

    config = config_load(config_file);
    if (config == NULL)
    {
        LOG_WARN(MODULE_NAME, "failed to load config file %s, using defaults", config_file);
        return 0;
    }

    LOG_INFO(MODULE_NAME, "loaded config from %s", config_file);

    str_val = config_get_string(config, "multicast_group", NULL);
    if (str_val != NULL)
    {
        strncpy(opt->group, str_val, sizeof(opt->group) - 1);
        opt->group[sizeof(opt->group) - 1] = '\0';
    }

    opt->udp_port = config_get_uint16(config, "multicast_port", opt->udp_port);
    opt->control_port = config_get_uint16(config, "control_port", opt->control_port);

    str_val = config_get_string(config, "control_listen_ip", NULL);
    if (str_val != NULL)
    {
        strncpy(opt->control_listen_ip, str_val, sizeof(opt->control_listen_ip) - 1);
        opt->control_listen_ip[sizeof(opt->control_listen_ip) - 1] = '\0';
    }

    str_val = config_get_string(config, "player_path", NULL);
    if (str_val != NULL)
        opt->player_path = strdup(str_val);

    opt->auto_channel = config_get_int(config, "auto_channel", opt->auto_channel);
    opt->interactive_mode = config_get_int(config, "interactive_mode", opt->interactive_mode);
    opt->enable_control = config_get_int(config, "enable_control", opt->enable_control);

    opt->jitter_buffer_size = config_get_int(config, "jitter_buffer_size", opt->jitter_buffer_size);
    opt->start_buffer_packets = config_get_int(config, "start_buffer_packets", opt->start_buffer_packets);
    opt->skip_threshold = config_get_int(config, "skip_threshold", opt->skip_threshold);
    opt->outbuf_capacity = (size_t)config_get_int(config, "outbuf_capacity", (int)opt->outbuf_capacity);

    opt->heartbeat_timeout = config_get_int(config, "heartbeat_timeout", opt->heartbeat_timeout);
    opt->heartbeat_check_interval = config_get_int(config, "heartbeat_check_interval", opt->heartbeat_check_interval);
    opt->max_reconnect_attempts = config_get_int(config, "max_reconnect_attempts", opt->max_reconnect_attempts);
    opt->reconnect_delay_ms = config_get_int(config, "reconnect_delay_ms", opt->reconnect_delay_ms);

    /* 资源监控配置 */
    opt->enable_resource_monitor = config_get_int(config, "enable_resource_monitor", opt->enable_resource_monitor);
    opt->resource_sample_interval_ms = config_get_int(config, "resource_sample_interval_ms", opt->resource_sample_interval_ms);
    opt->resource_cpu_critical = (float)config_get_int(config, "resource_cpu_critical", (int)opt->resource_cpu_critical);
    opt->resource_cpu_low = (float)config_get_int(config, "resource_cpu_low", (int)opt->resource_cpu_low);
    opt->resource_mem_critical = (float)config_get_int(config, "resource_mem_critical", (int)opt->resource_mem_critical);
    opt->resource_mem_low = (float)config_get_int(config, "resource_mem_low", (int)opt->resource_mem_low);

    /* 看门狗配置 */
    opt->enable_watchdog = config_get_int(config, "enable_watchdog", opt->enable_watchdog);
    str_val = config_get_string(config, "watchdog_device", NULL);
    if (str_val != NULL)
        opt->watchdog_device = strdup(str_val);
    opt->watchdog_timeout_sec = config_get_int(config, "watchdog_timeout_sec", opt->watchdog_timeout_sec);
    opt->watchdog_feed_interval_ms = config_get_int(config, "watchdog_feed_interval_ms", opt->watchdog_feed_interval_ms);

    config_free(config);
    return 0;
}

int main(int argc, char *argv[])
{
    struct optentry opt;
    const char *config_file;
    config_t *log_config;
    struct logger_config logger_cfg;
    const char *log_level_str;
    const char *log_file_str;
    int sfd = INVALID_FD;
    int exit_code = 1;
    int fatal_error = 0;
    char *recv_buf = NULL;
    struct epoll_event events[MAX_EVENTS];
    struct fd_ctx sock_ctx;

    if (parse_args(argc, argv, &opt, &config_file) < 0)
        return 1;

    log_config = config_load(config_file);
    memset(&logger_cfg, 0, sizeof(logger_cfg));
    logger_cfg.min_level = LOG_LEVEL_INFO;
    logger_cfg.output = stdout;
    logger_cfg.use_color = 1;
    logger_cfg.show_timestamp = 1;
    logger_cfg.show_thread_id = 0;

    if (log_config != NULL)
    {
        log_level_str = config_get_string(log_config, "log_level", "INFO");
        if (strcasecmp(log_level_str, "DEBUG") == 0)
            logger_cfg.min_level = LOG_LEVEL_DEBUG;
        else if (strcasecmp(log_level_str, "INFO") == 0)
            logger_cfg.min_level = LOG_LEVEL_INFO;
        else if (strcasecmp(log_level_str, "WARN") == 0)
            logger_cfg.min_level = LOG_LEVEL_WARN;
        else if (strcasecmp(log_level_str, "ERROR") == 0)
            logger_cfg.min_level = LOG_LEVEL_ERROR;

        log_file_str = config_get_string(log_config, "log_file", "");
        if (log_file_str != NULL && log_file_str[0] != '\0')
        {
            logger_cfg.output = fopen(log_file_str, "a");
            if (logger_cfg.output == NULL)
            {
                fprintf(stderr, "failed to open log file %s, using stdout\n", log_file_str);
                logger_cfg.output = stdout;
            }
        }

        logger_cfg.use_color = config_get_int(log_config, "log_use_color", 1);
        logger_cfg.show_timestamp = config_get_int(log_config, "log_show_timestamp", 1);
        logger_cfg.show_thread_id = config_get_int(log_config, "log_show_thread_id", 0);
    }

    logger_init(&logger_cfg);
    LOG_INFO(MODULE_NAME, "client starting");

    if (log_config != NULL)
    {
        load_config(config_file, &opt);
        config_free(log_config);
    }

    install_signal_handlers();
    outbuf_init(&g_outbuf, opt.outbuf_capacity);
    jitter_reset();

    LOG_INFO(MODULE_NAME, "multicast group=%s port=%u", opt.group, opt.udp_port);
    LOG_INFO(MODULE_NAME, "control server=%s:%u enabled=%d",
             opt.control_listen_ip, opt.control_port, opt.enable_control);

    sfd = create_udp_receiver(&opt);
    if (sfd < 0)
        goto out;

    recv_buf = malloc(RECV_BUF_SIZE);
    if (recv_buf == NULL)
    {
        LOG_ERROR(MODULE_NAME, "malloc recv_buf error");
        goto out;
    }

    if (opt.interactive_mode && opt.auto_channel == 0)
    {
        opt.auto_channel = wait_for_first_list_and_choose(sfd, recv_buf);
        if (opt.auto_channel < 0)
            goto out;
    }

    if (set_nonblock(sfd) < 0)
    {
        LOG_ERROR(MODULE_NAME, "set_nonblock udp failed: %s", strerror(errno));
        goto out;
    }

    g_epfd = epoll_create1(0);
    if (g_epfd < 0)
    {
        LOG_ERROR(MODULE_NAME, "epoll_create1 failed: %s", strerror(errno));
        goto out;
    }

    sock_ctx.tag = FD_TAG_SOCK;
    sock_ctx.fd = sfd;
    if (epoll_add_fd(g_epfd, sfd, EPOLLIN | EPOLLERR | EPOLLHUP, &sock_ctx) < 0)
    {
        perror("epoll_add udp");
        goto out;
    }

    if (opt.enable_control)
    {
        g_controller.listen_fd = create_control_listener(&opt);
        if (g_controller.listen_fd < 0)
            goto out;

        g_controller.listen_ctx.fd = g_controller.listen_fd;
        if (epoll_add_fd(g_epfd, g_controller.listen_fd,
                         EPOLLIN | EPOLLERR | EPOLLHUP,
                         &g_controller.listen_ctx) < 0)
        {
            perror("epoll_add control listener");
            goto out;
        }

        printf("control server listening on %s:%u\n",
               opt.control_listen_ip, opt.control_port);
        fflush(stdout);
    }

    if (opt.auto_channel != 0 && start_playback(opt.auto_channel, &opt) < 0)
        goto out;

    /* Initialize heartbeat */
    {
        struct heartbeat_config hb_config;
        hb_config.timeout_seconds = opt.heartbeat_timeout;
        hb_config.check_interval_ms = opt.heartbeat_check_interval;
        hb_config.max_reconnect_attempts = opt.max_reconnect_attempts;
        hb_config.reconnect_delay_ms = opt.reconnect_delay_ms;
        heartbeat_init(&g_heartbeat, &hb_config);
    }

    LOG_INFO(MODULE_NAME, "heartbeat enabled: timeout=%ds check_interval=%dms max_reconnect=%d",
             opt.heartbeat_timeout, opt.heartbeat_check_interval, opt.max_reconnect_attempts);

    /* Initialize resource monitor */
    if (opt.enable_resource_monitor)
    {
        resource_monitor_config_t res_config;
        res_config.sample_interval_ms = opt.resource_sample_interval_ms;
        res_config.cpu_critical_threshold = opt.resource_cpu_critical;
        res_config.cpu_low_threshold = opt.resource_cpu_low;
        res_config.mem_critical_threshold = opt.resource_mem_critical;
        res_config.mem_low_threshold = opt.resource_mem_low;
        res_config.enable_adaptive = 1;

        g_resource_monitor = resource_monitor_create(&res_config);
        if (g_resource_monitor == NULL)
        {
            LOG_ERROR(MODULE_NAME, "resource_monitor_create failed");
            goto out;
        }
        LOG_INFO(MODULE_NAME, "resource monitor enabled: sample_interval=%dms cpu_critical=%.1f%% mem_critical=%.1f%%",
                 opt.resource_sample_interval_ms, opt.resource_cpu_critical, opt.resource_mem_critical);
    }

    /* Initialize watchdog */
    if (opt.enable_watchdog)
    {
        watchdog_config_t wd_config;
        wd_config.type = WATCHDOG_TYPE_HARDWARE;
        wd_config.device_path = opt.watchdog_device;
        wd_config.timeout_sec = opt.watchdog_timeout_sec;
        wd_config.feed_interval_ms = opt.watchdog_feed_interval_ms;
        wd_config.enable_magic_close = 1;

        g_watchdog = watchdog_create(&wd_config);
        if (g_watchdog == NULL)
        {
            LOG_WARN(MODULE_NAME, "watchdog_create failed, continuing without watchdog");
            opt.enable_watchdog = 0;
        }
        else
        {
            if (watchdog_start(g_watchdog) < 0)
            {
                LOG_WARN(MODULE_NAME, "watchdog_start failed, continuing without watchdog");
                watchdog_destroy(g_watchdog);
                g_watchdog = NULL;
                opt.enable_watchdog = 0;
            }
            else
            {
                LOG_INFO(MODULE_NAME, "watchdog enabled: device=%s timeout=%ds feed_interval=%dms",
                         opt.watchdog_device, opt.watchdog_timeout_sec, opt.watchdog_feed_interval_ms);
            }
        }
    }

    LOG_INFO(MODULE_NAME, "entering main event loop");

    while (!g_stop)
    {
        int nfds;
        int i;

        nfds = epoll_wait(g_epfd, events, MAX_EVENTS, opt.heartbeat_check_interval);
        if (nfds < 0)
        {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            fatal_error = 1;
            break;
        }

        /* Check heartbeat timeout and player health */
        if (g_current_channel != 0)
        {
            /* Check if player is still alive */
            if (player_active() && !player_check_alive())
            {
                LOG_WARN(MODULE_NAME, "player died, attempting restart for channel %d", g_current_channel);
                player_stop();

                if (player_start(opt.player_path) < 0)
                {
                    LOG_ERROR(MODULE_NAME, "failed to restart player");
                    g_current_channel = 0;
                }
                else
                {
                    LOG_INFO(MODULE_NAME, "player restarted successfully");
                    playback_buffers_reset();
                }
            }

            /* Check heartbeat timeout */
            if (heartbeat_check_timeout(&g_heartbeat))
            {
                int idle_time = heartbeat_get_idle_time(&g_heartbeat);
                LOG_WARN(MODULE_NAME, "no data received for %d seconds, attempting reconnect", idle_time);

                if (heartbeat_increment_reconnect(&g_heartbeat) < 0)
                {
                    LOG_ERROR(MODULE_NAME, "max reconnect attempts reached, giving up");
                    fatal_error = 1;
                    g_stop = 1;
                    break;
                }

                if (rejoin_multicast(sfd, &opt) < 0)
                {
                    LOG_ERROR(MODULE_NAME, "failed to rejoin multicast group");
                }
                else
                {
                    usleep(opt.reconnect_delay_ms * 1000);
                }
            }
        }

        /* Update resource monitor and apply adaptive policy */
        if (opt.enable_resource_monitor && g_resource_monitor)
        {
            static time_t last_resource_check = 0;
            time_t now = time(NULL);

            if (now - last_resource_check >= opt.resource_sample_interval_ms / 1000)
            {
                resource_stats_t stats;
                resource_level_t level;
                adaptive_policy_t policy;

                if (resource_monitor_update(g_resource_monitor) == 0 &&
                    resource_monitor_get_stats(g_resource_monitor, &stats) == 0)
                {
                    level = resource_monitor_get_level(g_resource_monitor);

                    if (level == RESOURCE_LEVEL_CRITICAL || level == RESOURCE_LEVEL_LOW)
                    {
                        LOG_WARN(MODULE_NAME, "resource level: %s (cpu=%.1f%% mem=%.1f%%)",
                                 resource_level_to_string(level),
                                 stats.cpu_usage_percent,
                                 stats.mem_usage_percent);

                        /* Apply adaptive policy */
                        if (resource_monitor_get_policy(g_resource_monitor, &policy) == 0)
                        {
                            /* TODO: Apply policy adjustments to jitter buffer and bitrate */
                            LOG_INFO(MODULE_NAME, "adaptive policy: buffer=%d bitrate=%dkbps",
                                     policy.jitter_buffer_size, policy.max_bitrate_kbps);
                        }
                    }
                }

                last_resource_check = now;
            }
        }

        /* Feed watchdog */
        if (opt.enable_watchdog && g_watchdog)
        {
            if (watchdog_should_feed(g_watchdog))
            {
                if (watchdog_feed(g_watchdog) < 0)
                {
                    LOG_ERROR(MODULE_NAME, "watchdog_feed failed");
                }
                else
                {
                    LOG_DEBUG(MODULE_NAME, "watchdog fed");
                }
            }
        }

        if (nfds == 0)
            continue;

        for (i = 0; i < nfds; i++)
        {
            struct fd_ctx *ctx = (struct fd_ctx *)events[i].data.ptr;
            uint32_t ev = events[i].events;

            if (ctx == NULL)
                continue;

            if (ev & (EPOLLERR | EPOLLHUP))
            {
                if (ctx->tag == FD_TAG_CTRL_CONN)
                {
                    close_controller_conn();
                    continue;
                }

                if (ctx->tag == FD_TAG_PIPE)
                {
                    player_stop();
                    continue;
                }

                fprintf(stderr, "fd error/hup, exit\n");
                fatal_error = 1;
                g_stop = 1;
                break;
            }

            if (ctx->tag == FD_TAG_SOCK)
            {
                while (1)
                {
                    int read_num = recvfrom(sfd, recv_buf, RECV_BUF_SIZE, 0, NULL, NULL);
                    if (read_num < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        if (errno == EINTR)
                            continue;
                        perror("recvfrom udp");
                        fatal_error = 1;
                        g_stop = 1;
                        break;
                    }

                    if (read_num < 2)
                        continue;

                    if ((uint8_t)recv_buf[0] == LIST_ID &&
                        (uint8_t)recv_buf[1] == PKT_TYPE_LIST)
                    {
                        update_program_list_from_packet(recv_buf, read_num);
                        continue;
                    }

                    if (g_current_channel == 0)
                        continue;

                    if (parse_desc_packet(recv_buf, read_num, (uint8_t)g_current_channel))
                        continue;

                    if ((uint8_t)recv_buf[1] == PKT_TYPE_AUDIO)
                    {
                        if (parse_audio_packet_and_queue(recv_buf,
                                                         read_num,
                                                         (uint8_t)g_current_channel) < 0)
                        {
                            fatal_error = 1;
                            g_stop = 1;
                            break;
                        }
                    }
                }
            }
            else if (ctx->tag == FD_TAG_CTRL_LISTEN)
            {
                if (accept_controller_connection() < 0)
                {
                    fatal_error = 1;
                    g_stop = 1;
                    break;
                }
            }
            else if (ctx->tag == FD_TAG_CTRL_CONN)
            {
                if (process_controller_input(&opt) < 0)
                {
                    fatal_error = 1;
                    g_stop = 1;
                    break;
                }
            }
            else if (ctx->tag == FD_TAG_PIPE)
            {
                if (ev & EPOLLOUT)
                {
                    if (flush_outbuf_to_pipe() < 0)
                    {
                        fatal_error = 1;
                        g_stop = 1;
                        break;
                    }

                    if (update_pipe_epoll_events() < 0)
                    {
                        perror("epoll_mod pipe");
                        fatal_error = 1;
                        g_stop = 1;
                        break;
                    }

                    if (jitter_drain_to_outbuf() < 0)
                    {
                        fatal_error = 1;
                        g_stop = 1;
                        break;
                    }
                }
            }
        }
    }

    exit_code = fatal_error ? 1 : 0;

out:
    LOG_INFO(MODULE_NAME, "shutting down, exit_code=%d", exit_code);

    close_controller_conn();

    if (g_controller.listen_fd >= 0)
    {
        epoll_del_fd_if_needed(g_epfd, g_controller.listen_fd);
        close(g_controller.listen_fd);
    }

    stop_playback();

    if (g_epfd >= 0)
        close(g_epfd);
    if (sfd >= 0)
        close(sfd);

    free(recv_buf);
    free(g_outbuf.buf);

    /* Cleanup resource monitor and watchdog */
    if (g_resource_monitor)
    {
        resource_monitor_destroy(g_resource_monitor);
        g_resource_monitor = NULL;
    }

    if (g_watchdog)
    {
        watchdog_stop(g_watchdog);
        watchdog_destroy(g_watchdog);
        g_watchdog = NULL;
    }

    logger_destroy();
    return exit_code;
}
