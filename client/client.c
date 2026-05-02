#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "proto.h"

#define RECV_BUF_SIZE  (NET_AUDIO_HDR_LEN + AUDIO_CHUNK_SIZE + 64)
#define CONTROL_BUFFER_SIZE 4096
#define MAX_PROGRAMS        MAX_CHANAL_ID
#define MAX_PROGRAM_NAME    256

#define JITTER_BUFFER_SIZE   64
#define START_BUFFER_PACKETS 6
#define SKIP_THRESHOLD       12

#define MAX_EVENTS      16
#define OUTBUF_CAPACITY (1024 * 1024)

#define DEFAULT_CONTROL_PORT 9000

enum
{
    FD_TAG_UDP = 1,
    FD_TAG_PIPE,
    FD_TAG_TCP_LISTEN,
    FD_TAG_TCP_CLIENT
};

struct audio_frame
{
    int valid;
    uint32_t seq;
    uint16_t payload_len;
    char data[AUDIO_CHUNK_SIZE];
};

struct playback_state
{
    struct audio_frame frames[JITTER_BUFFER_SIZE];
    uint32_t stream_epoch;
    uint32_t expected_seq;
    uint32_t max_seq_seen;
    int is_first_packet;
    int started;
    int current_channel;
    int desc_printed;
};

struct outbuf
{
    char *buf;
    size_t cap;
    size_t head;
    size_t tail;
};

struct program_entry
{
    uint8_t id;
    char name[MAX_PROGRAM_NAME];
};

struct optentry
{
    uint16_t multicast_port;
    uint16_t control_port;
    const char *group;
    const char *player_path;
    int auto_channel_id;
};

struct fd_ctx
{
    int tag;
    int fd;
};

struct control_state
{
    int listen_fd;
    int client_fd;
    char recvbuf[CONTROL_BUFFER_SIZE];
    size_t recvlen;
    struct fd_ctx listen_ctx;
    struct fd_ctx client_ctx;
};

static struct playback_state g_playback;
static struct outbuf g_outbuf;
static struct control_state g_control = { -1, -1, {0}, 0, { FD_TAG_TCP_LISTEN, -1 }, { FD_TAG_TCP_CLIENT, -1 } };
static struct program_entry g_programs[MAX_PROGRAMS];
static int g_program_count = 0;

static int g_epfd = -1;
static int g_udp_fd = -1;
static int g_pipe_wfd = -1;
static pid_t g_player_pid = -1;
static int g_running = 1;
static const char *g_player_path = "mpg123";

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-g multicast_group] [-p multicast_port] [-t control_port] "
            "[-P player] [-c channel_id]\n",
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

static int parse_channel_id(const char *text, int *channel_id)
{
    char *end = NULL;
    long value;

    if (text == NULL || channel_id == NULL)
        return -1;

    value = strtol(text, &end, 10);
    if (*text == '\0' || *end != '\0' ||
        value < MIN_CHANAL_ID || value > MAX_CHANAL_ID)
    {
        return -1;
    }

    *channel_id = (int)value;
    return 0;
}

static int parse_args(int argc, char *argv[], struct optentry *opts)
{
    int ch;

    if (opts == NULL)
        return -1;

    memset(opts, 0, sizeof(*opts));
    opts->group = MYGRUOP;
    opts->player_path = "mpg123";
    opts->auto_channel_id = 0;

    if (parse_port(PORT, &opts->multicast_port) < 0)
        return -1;

    opts->control_port = DEFAULT_CONTROL_PORT;

    while ((ch = getopt(argc, argv, "hg:p:t:P:c:")) != -1)
    {
        switch (ch)
        {
        case 'g':
            opts->group = optarg;
            break;
        case 'p':
            if (parse_port(optarg, &opts->multicast_port) < 0)
            {
                fprintf(stderr, "invalid multicast port: %s\n", optarg);
                return -1;
            }
            break;
        case 't':
            if (parse_port(optarg, &opts->control_port) < 0)
            {
                fprintf(stderr, "invalid control port: %s\n", optarg);
                return -1;
            }
            break;
        case 'P':
            opts->player_path = optarg;
            break;
        case 'c':
            if (parse_channel_id(optarg, &opts->auto_channel_id) < 0)
            {
                fprintf(stderr, "invalid channel id: %s\n", optarg);
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

static void sig_handler(int signo)
{
    (void)signo;
    g_running = 0;
}

static void setup_signal(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
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

static void playback_reset_state(void)
{
    memset(&g_playback, 0, sizeof(g_playback));
    g_playback.is_first_packet = 1;
    outbuf_clear(&g_outbuf);
}

static int control_send_line(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    int len;
    ssize_t sent;

    if (g_control.client_fd < 0)
        return -1;

    va_start(ap, fmt);
    len = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (len < 0)
        return -1;

    if ((size_t)len >= sizeof(line) - 2)
        len = (int)sizeof(line) - 3;

    line[len++] = '\n';
    line[len] = '\0';

    sent = send(g_control.client_fd, line, (size_t)len, MSG_NOSIGNAL);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        perror("send tcp");

    return 0;
}

static void control_close_client(void)
{
    if (g_control.client_fd >= 0)
    {
        epoll_del_fd_if_needed(g_epfd, g_control.client_fd);
        close(g_control.client_fd);
        g_control.client_fd = -1;
    }

    g_control.recvlen = 0;
}

static void program_list_clear(void)
{
    g_program_count = 0;
    memset(g_programs, 0, sizeof(g_programs));
}

static void add_program(uint8_t id, const char *name, uint16_t name_len)
{
    if (g_program_count >= MAX_PROGRAMS || name == NULL)
        return;

    g_programs[g_program_count].id = id;
    if (name_len >= MAX_PROGRAM_NAME)
        name_len = MAX_PROGRAM_NAME - 1;

    memcpy(g_programs[g_program_count].name, name, name_len);
    g_programs[g_program_count].name[name_len] = '\0';
    g_program_count++;
}

static void cache_programs_from_list_packet(const char *buf, int len)
{
    const char *pos;
    int remain;

    if (len < NET_LIST_HDR_LEN)
        return;

    if ((uint8_t)buf[0] != LIST_ID || (uint8_t)buf[1] != PKT_TYPE_LIST)
        return;

    pos = buf + NET_LIST_HDR_LEN;
    remain = len - NET_LIST_HDR_LEN;

    program_list_clear();

    while (remain >= 3)
    {
        uint8_t ch_id;
        uint16_t net_name_len;
        uint16_t name_len;

        ch_id = (uint8_t)pos[0];
        memcpy(&net_name_len, pos + 1, sizeof(net_name_len));
        name_len = ntohs(net_name_len);

        if (remain < 3 + (int)name_len)
            break;

        add_program(ch_id, pos + 3, name_len);
        pos += 3 + name_len;
        remain -= 3 + name_len;
    }
}

static void send_program_list_to_control(void)
{
    int i;

    for (i = 0; i < g_program_count; i++)
    {
        control_send_line("PROGRAM %u %s",
                          g_programs[i].id,
                          g_programs[i].name);
    }

    control_send_line("END");
}

static int update_pipe_epoll_events(void *pipe_ctx)
{
    uint32_t ev = EPOLLERR | EPOLLHUP;

    if (g_pipe_wfd < 0)
        return 0;

    if (!outbuf_empty(&g_outbuf))
        ev |= EPOLLOUT;

    return epoll_mod_fd(g_epfd, g_pipe_wfd, ev, pipe_ctx);
}

static int flush_outbuf_to_pipe(void)
{
    while (g_pipe_wfd >= 0 && !outbuf_empty(&g_outbuf))
    {
        ssize_t ret = write(g_pipe_wfd,
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

        perror("write pipe");
        return -1;
    }

    return 0;
}

static int queue_audio_to_player(const char *data, uint16_t len, void *pipe_ctx)
{
    if (g_pipe_wfd < 0)
        return 0;

    if (outbuf_append(&g_outbuf, data, len) < 0)
    {
        fprintf(stderr, "\n[Error] output buffer overflow, drop audio\n");
        return -1;
    }

    if (flush_outbuf_to_pipe() < 0)
        return -1;

    if (update_pipe_epoll_events(pipe_ctx) < 0)
    {
        perror("epoll_mod pipe");
        return -1;
    }

    return 0;
}

static void jitter_reset_slot(int idx)
{
    g_playback.frames[idx].valid = 0;
    g_playback.frames[idx].seq = 0;
    g_playback.frames[idx].payload_len = 0;
}

static void jitter_reset_all(uint32_t stream_epoch, uint32_t first_seq)
{
    int i;

    for (i = 0; i < JITTER_BUFFER_SIZE; i++)
        jitter_reset_slot(i);

    g_playback.stream_epoch = stream_epoch;
    g_playback.expected_seq = first_seq;
    g_playback.max_seq_seen = first_seq;
    g_playback.is_first_packet = 0;
    g_playback.started = 0;
}

static int jitter_store_packet(uint32_t seq, const char *payload, uint16_t payload_len)
{
    int idx = seq % JITTER_BUFFER_SIZE;

    if (g_playback.frames[idx].valid && g_playback.frames[idx].seq != seq)
    {
        if (seq > g_playback.frames[idx].seq)
            jitter_reset_slot(idx);
        else
            return 0;
    }

    if (!g_playback.frames[idx].valid)
    {
        g_playback.frames[idx].seq = seq;
        g_playback.frames[idx].payload_len = payload_len;
        memcpy(g_playback.frames[idx].data, payload, payload_len);
        g_playback.frames[idx].valid = 1;
    }

    return 0;
}

static int jitter_should_start(void)
{
    if (g_playback.started)
        return 1;

    if (g_playback.max_seq_seen >= g_playback.expected_seq + (START_BUFFER_PACKETS - 1))
        return 1;

    return 0;
}

static int jitter_maybe_skip_missing(void)
{
    if (!g_playback.started)
        return 0;

    if (g_playback.max_seq_seen >= g_playback.expected_seq + SKIP_THRESHOLD)
    {
        fprintf(stderr,
                "\r[Warning] missing seq=%u, skip 1 packet...",
                g_playback.expected_seq);
        g_playback.expected_seq++;
        return 1;
    }

    return 0;
}

static int jitter_drain_to_outbuf(void *pipe_ctx)
{
    int idx;

    if (g_pipe_wfd < 0)
        return 0;

    if (!jitter_should_start())
        return 0;

    g_playback.started = 1;

    while (1)
    {
        idx = g_playback.expected_seq % JITTER_BUFFER_SIZE;

        if (!g_playback.frames[idx].valid ||
            g_playback.frames[idx].seq != g_playback.expected_seq)
        {
            if (jitter_maybe_skip_missing())
                continue;
            break;
        }

        if (queue_audio_to_player(g_playback.frames[idx].data,
                                  g_playback.frames[idx].payload_len,
                                  pipe_ctx) < 0)
        {
            return -1;
        }

        jitter_reset_slot(idx);
        g_playback.expected_seq++;
    }

    return 0;
}

static int parse_desc_packet(const char *buf, int len, uint8_t current_channel)
{
    uint8_t ch_id;
    uint8_t pkt_type;
    uint16_t net_desc_len;
    uint16_t desc_len;

    if (len < NET_DESC_HDR_LEN)
        return 0;

    ch_id = (uint8_t)buf[0];
    pkt_type = (uint8_t)buf[1];

    if (ch_id != current_channel || pkt_type != PKT_TYPE_DESC)
        return 0;

    memcpy(&net_desc_len, buf + 2, sizeof(net_desc_len));
    desc_len = ntohs(net_desc_len);

    if (len < NET_DESC_HDR_LEN + (int)desc_len)
        return 0;

    if (!g_playback.desc_printed)
    {
        printf("desc: %.*s\n", desc_len, buf + NET_DESC_HDR_LEN);
        control_send_line("INFO DESC %.*s", desc_len, buf + NET_DESC_HDR_LEN);
        g_playback.desc_printed = 1;
    }

    return 1;
}

static int parse_audio_packet_and_queue(const char *buf, int len,
                                        uint8_t current_channel, void *pipe_ctx)
{
    uint8_t ch_id;
    uint8_t pkt_type;
    uint32_t net_epoch;
    uint32_t net_seq;
    uint16_t net_payload_len;
    uint32_t stream_epoch;
    uint32_t seq;
    uint16_t payload_len;

    if (len < NET_AUDIO_HDR_LEN || current_channel == 0 || g_pipe_wfd < 0)
        return 0;

    ch_id = (uint8_t)buf[0];
    pkt_type = (uint8_t)buf[1];

    if (ch_id != current_channel || pkt_type != PKT_TYPE_AUDIO)
        return 0;

    memcpy(&net_epoch, buf + 2, sizeof(net_epoch));
    memcpy(&net_seq, buf + 6, sizeof(net_seq));
    memcpy(&net_payload_len, buf + 10, sizeof(net_payload_len));

    stream_epoch = ntohl(net_epoch);
    seq = ntohl(net_seq);
    payload_len = ntohs(net_payload_len);

    if (len < NET_AUDIO_HDR_LEN + (int)payload_len)
        return 0;

    if (payload_len > AUDIO_CHUNK_SIZE)
        return 0;

    if (g_playback.is_first_packet)
    {
        jitter_reset_all(stream_epoch, seq);
    }
    else if (stream_epoch < g_playback.stream_epoch)
    {
        return 0;
    }
    else if (stream_epoch > g_playback.stream_epoch)
    {
        jitter_reset_all(stream_epoch, seq);
    }

    if (seq > g_playback.max_seq_seen)
        g_playback.max_seq_seen = seq;

    if (seq < g_playback.expected_seq)
        return 0;

    if (seq >= g_playback.expected_seq + JITTER_BUFFER_SIZE)
    {
        uint32_t new_expected = seq - (JITTER_BUFFER_SIZE - 1);
        uint32_t s;

        for (s = g_playback.expected_seq; s < new_expected; s++)
        {
            int idx = s % JITTER_BUFFER_SIZE;
            if (g_playback.frames[idx].valid && g_playback.frames[idx].seq == s)
                jitter_reset_slot(idx);
        }

        fprintf(stderr,
                "\r[Warning] buffer move forward: %u -> %u",
                g_playback.expected_seq, new_expected);

        g_playback.expected_seq = new_expected;
    }

    if (jitter_store_packet(seq, buf + NET_AUDIO_HDR_LEN, payload_len) < 0)
        return -1;

    return jitter_drain_to_outbuf(pipe_ctx);
}

static int create_udp_socket(const struct optentry *opts)
{
    struct sockaddr_in laddr;
    struct ip_mreqn mreq;
    int reuse = 1;
    int fd;
    int ret;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (ret < 0)
    {
        close(fd);
        return -1;
    }

    memset(&laddr, 0, sizeof(laddr));
    laddr.sin_family = AF_INET;
    laddr.sin_port = htons(opts->multicast_port);
    laddr.sin_addr.s_addr = htonl(INADDR_ANY);

    ret = bind(fd, (struct sockaddr *)&laddr, sizeof(laddr));
    if (ret < 0)
    {
        close(fd);
        return -1;
    }

    memset(&mreq, 0, sizeof(mreq));
    ret = inet_pton(AF_INET, opts->group, &mreq.imr_multiaddr.s_addr);
    if (ret <= 0)
    {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    mreq.imr_address.s_addr = htonl(INADDR_ANY);
    mreq.imr_ifindex = 0;

    ret = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    if (ret < 0)
    {
        close(fd);
        return -1;
    }

    if (set_nonblock(fd) < 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

static int create_tcp_listener(uint16_t port)
{
    struct sockaddr_in addr;
    int fd;
    int reuse = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    if (listen(fd, 4) < 0)
    {
        close(fd);
        return -1;
    }

    if (set_nonblock(fd) < 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

static int start_player(void *pipe_ctx)
{
    int pipefd[2] = { -1, -1 };
    pid_t pid;

    if (g_pipe_wfd >= 0 || g_player_pid > 0)
        return 0;

    if (pipe(pipefd) < 0)
        return -1;

    pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0)
    {
        close(pipefd[1]);
        if (dup2(pipefd[0], STDIN_FILENO) < 0)
            _exit(1);

        close(pipefd[0]);
        execlp(g_player_path, g_player_path, "-", "-q", NULL);
        _exit(1);
    }

    close(pipefd[0]);

    if (set_nonblock(pipefd[1]) < 0)
    {
        close(pipefd[1]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }

    g_pipe_wfd = pipefd[1];
    g_player_pid = pid;

    if (epoll_add_fd(g_epfd, g_pipe_wfd, EPOLLERR | EPOLLHUP, pipe_ctx) < 0)
    {
        close(g_pipe_wfd);
        g_pipe_wfd = -1;
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        g_player_pid = -1;
        return -1;
    }

    return 0;
}

static void stop_player(void)
{
    if (g_pipe_wfd >= 0)
    {
        epoll_del_fd_if_needed(g_epfd, g_pipe_wfd);
        close(g_pipe_wfd);
        g_pipe_wfd = -1;
    }

    outbuf_clear(&g_outbuf);

    if (g_player_pid > 0)
    {
        kill(g_player_pid, SIGTERM);
        waitpid(g_player_pid, NULL, 0);
        g_player_pid = -1;
    }
}

static int begin_playback(int channel_id, void *pipe_ctx)
{
    stop_player();
    playback_reset_state();
    g_playback.current_channel = channel_id;

    if (start_player(pipe_ctx) < 0)
    {
        g_playback.current_channel = 0;
        return -1;
    }

    printf("start playback channel %d\n", channel_id);
    return 0;
}

static void stop_playback(void)
{
    stop_player();
    playback_reset_state();
    g_playback.current_channel = 0;
    printf("playback stopped\n");
}

static void handle_udp_packets(char *recv_buf, void *pipe_ctx)
{
    while (1)
    {
        int read_num = recvfrom(g_udp_fd, recv_buf, RECV_BUF_SIZE, 0, NULL, NULL);
        if (read_num < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            perror("recvfrom data");
            g_running = 0;
            break;
        }

        if (read_num < 2)
            continue;

        if ((uint8_t)recv_buf[0] == LIST_ID &&
            (uint8_t)recv_buf[1] == PKT_TYPE_LIST)
        {
            cache_programs_from_list_packet(recv_buf, read_num);
            continue;
        }

        if (g_playback.current_channel == 0)
            continue;

        if (parse_desc_packet(recv_buf, read_num, (uint8_t)g_playback.current_channel))
            continue;

        if ((uint8_t)recv_buf[1] == PKT_TYPE_AUDIO)
        {
            if (parse_audio_packet_and_queue(recv_buf,
                                             read_num,
                                             (uint8_t)g_playback.current_channel,
                                             pipe_ctx) < 0)
            {
                stop_playback();
                control_send_line("ERR player output failure");
            }
        }
    }
}

static int handle_play_command(const char *arg, void *pipe_ctx)
{
    int channel_id = 0;

    if (parse_channel_id(arg, &channel_id) < 0)
    {
        control_send_line("ERR invalid channel id");
        return -1;
    }

    if (begin_playback(channel_id, pipe_ctx) < 0)
    {
        control_send_line("ERR failed to start player");
        return -1;
    }

    control_send_line("OK PLAY %d", channel_id);
    return 0;
}

static void process_control_command(char *line, void *pipe_ctx)
{
    char *arg;

    while (*line == ' ' || *line == '\t')
        line++;

    if (*line == '\0')
        return;

    arg = strchr(line, ' ');
    if (arg != NULL)
    {
        *arg = '\0';
        arg++;
        while (*arg == ' ' || *arg == '\t')
            arg++;
    }

    if (strcmp(line, "LIST") == 0)
    {
        send_program_list_to_control();
    }
    else if (strcmp(line, "PLAY") == 0)
    {
        if (arg == NULL || *arg == '\0')
        {
            control_send_line("ERR PLAY requires channel id");
            return;
        }

        handle_play_command(arg, pipe_ctx);
    }
    else if (strcmp(line, "STOP") == 0)
    {
        stop_playback();
        control_send_line("OK STOP");
    }
    else if (strcmp(line, "QUIT") == 0)
    {
        control_send_line("OK QUIT");
        control_close_client();
    }
    else
    {
        control_send_line("ERR unknown command");
    }
}

static void handle_control_client_io(void *pipe_ctx)
{
    while (1)
    {
        ssize_t ret = recv(g_control.client_fd,
                           g_control.recvbuf + g_control.recvlen,
                           sizeof(g_control.recvbuf) - g_control.recvlen - 1,
                           0);
        if (ret > 0)
        {
            char *line_start;
            char *newline;

            g_control.recvlen += (size_t)ret;
            g_control.recvbuf[g_control.recvlen] = '\0';

            line_start = g_control.recvbuf;
            while ((newline = strchr(line_start, '\n')) != NULL)
            {
                *newline = '\0';
                if (newline > line_start && newline[-1] == '\r')
                    newline[-1] = '\0';

                process_control_command(line_start, pipe_ctx);
                if (g_control.client_fd < 0)
                    return;

                line_start = newline + 1;
            }

            if (line_start != g_control.recvbuf)
            {
                size_t remain = g_control.recvbuf + g_control.recvlen - line_start;
                memmove(g_control.recvbuf, line_start, remain);
                g_control.recvlen = remain;
                g_control.recvbuf[g_control.recvlen] = '\0';
            }

            if (g_control.recvlen == sizeof(g_control.recvbuf) - 1)
            {
                control_send_line("ERR command too long");
                g_control.recvlen = 0;
                g_control.recvbuf[0] = '\0';
            }

            continue;
        }

        if (ret == 0)
        {
            control_close_client();
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        if (errno == EINTR)
            continue;

        perror("recv tcp");
        control_close_client();
        return;
    }
}

static void accept_control_client(void)
{
    while (1)
    {
        int client_fd = accept(g_control.listen_fd, NULL, NULL);
        if (client_fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            perror("accept");
            return;
        }

        if (set_nonblock(client_fd) < 0)
        {
            close(client_fd);
            continue;
        }

        control_close_client();

        g_control.client_fd = client_fd;
        g_control.client_ctx.fd = client_fd;
        g_control.recvlen = 0;
        g_control.recvbuf[0] = '\0';

        if (epoll_add_fd(g_epfd, client_fd, EPOLLIN | EPOLLERR | EPOLLHUP, &g_control.client_ctx) < 0)
        {
            perror("epoll_add tcp client");
            control_close_client();
            continue;
        }

        control_send_line("INFO CONNECTED");
    }
}

int main(int argc, char *argv[])
{
    struct optentry opts;
    struct epoll_event events[MAX_EVENTS];
    struct fd_ctx udp_ctx;
    struct fd_ctx pipe_ctx;
    char *recv_buf = NULL;

    if (parse_args(argc, argv, &opts) < 0)
        return 1;

    setup_signal();
    g_player_path = opts.player_path;

    recv_buf = malloc(RECV_BUF_SIZE);
    if (recv_buf == NULL)
    {
        fprintf(stderr, "malloc recv_buf error\n");
        return 1;
    }

    outbuf_init(&g_outbuf, OUTBUF_CAPACITY);
    playback_reset_state();

    g_udp_fd = create_udp_socket(&opts);
    if (g_udp_fd < 0)
    {
        perror("create_udp_socket");
        free(recv_buf);
        free(g_outbuf.buf);
        return 1;
    }

    g_control.listen_fd = create_tcp_listener(opts.control_port);
    if (g_control.listen_fd < 0)
    {
        perror("create_tcp_listener");
        close(g_udp_fd);
        free(recv_buf);
        free(g_outbuf.buf);
        return 1;
    }

    g_control.listen_ctx.fd = g_control.listen_fd;

    g_epfd = epoll_create1(0);
    if (g_epfd < 0)
    {
        perror("epoll_create1");
        close(g_control.listen_fd);
        close(g_udp_fd);
        free(recv_buf);
        free(g_outbuf.buf);
        return 1;
    }

    udp_ctx.tag = FD_TAG_UDP;
    udp_ctx.fd = g_udp_fd;
    pipe_ctx.tag = FD_TAG_PIPE;
    pipe_ctx.fd = -1;

    if (epoll_add_fd(g_epfd, g_udp_fd, EPOLLIN | EPOLLERR | EPOLLHUP, &udp_ctx) < 0)
    {
        perror("epoll_add udp");
        g_running = 0;
    }

    if (g_running &&
        epoll_add_fd(g_epfd,
                     g_control.listen_fd,
                     EPOLLIN | EPOLLERR | EPOLLHUP,
                     &g_control.listen_ctx) < 0)
    {
        perror("epoll_add tcp listen");
        g_running = 0;
    }

    if (g_running && opts.auto_channel_id != 0)
    {
        if (begin_playback(opts.auto_channel_id, &pipe_ctx) < 0)
            fprintf(stderr, "failed to auto start playback for channel %d\n", opts.auto_channel_id);
    }

    printf("client core ready: udp %s:%u, tcp control 0.0.0.0:%u\n",
           opts.group, opts.multicast_port, opts.control_port);

    while (g_running)
    {
        int nfds;
        int i;

        nfds = epoll_wait(g_epfd, events, MAX_EVENTS, -1);
        if (nfds < 0)
        {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        for (i = 0; i < nfds; i++)
        {
            struct fd_ctx *ctx = (struct fd_ctx *)events[i].data.ptr;
            uint32_t ev = events[i].events;

            if (ctx == NULL)
                continue;

            if (ctx->tag == FD_TAG_UDP)
            {
                if (ev & (EPOLLERR | EPOLLHUP))
                {
                    fprintf(stderr, "udp socket error\n");
                    g_running = 0;
                    break;
                }

                handle_udp_packets(recv_buf, &pipe_ctx);
            }
            else if (ctx->tag == FD_TAG_TCP_LISTEN)
            {
                if (ev & (EPOLLERR | EPOLLHUP))
                {
                    fprintf(stderr, "tcp listen socket error\n");
                    g_running = 0;
                    break;
                }

                accept_control_client();
            }
            else if (ctx->tag == FD_TAG_TCP_CLIENT)
            {
                if (ev & (EPOLLERR | EPOLLHUP))
                {
                    control_close_client();
                    continue;
                }

                handle_control_client_io(&pipe_ctx);
            }
            else if (ctx->tag == FD_TAG_PIPE)
            {
                if (ev & (EPOLLERR | EPOLLHUP))
                {
                    control_send_line("ERR player exited");
                    stop_playback();
                    continue;
                }

                if (ev & EPOLLOUT)
                {
                    if (flush_outbuf_to_pipe() < 0)
                    {
                        control_send_line("ERR player output failure");
                        stop_playback();
                        continue;
                    }

                    if (update_pipe_epoll_events(&pipe_ctx) < 0)
                    {
                        perror("epoll_mod pipe");
                        stop_playback();
                        continue;
                    }

                    if (jitter_drain_to_outbuf(&pipe_ctx) < 0)
                    {
                        control_send_line("ERR player output failure");
                        stop_playback();
                    }
                }
            }
        }
    }

    control_close_client();
    stop_player();

    if (g_control.listen_fd >= 0)
        close(g_control.listen_fd);
    if (g_udp_fd >= 0)
        close(g_udp_fd);
    if (g_epfd >= 0)
        close(g_epfd);

    free(recv_buf);
    free(g_outbuf.buf);
    return 0;
}
