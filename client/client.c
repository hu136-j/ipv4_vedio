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
    const char *player_path;
    int auto_channel;
    int interactive_mode;
    int enable_control;
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
            "Usage: %s [-g multicast_ip] [-p udp_port] [-t tcp_port] [-P player] [-c channel] [-i] [-n]\n"
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
        return -1;
    }

    g_current_channel = channel;
    printf("play channel %d\n", channel);
    fflush(stdout);
    return 0;
}

static void stop_playback(void)
{
    if (g_current_channel != 0)
    {
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

static int create_control_listener(uint16_t port)
{
    struct sockaddr_in addr;
    int fd;
    int reuse = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket tcp");
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        perror("setsockopt SO_REUSEADDR tcp");
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, CONTROL_LISTEN_IP, &addr.sin_addr) <= 0)
    {
        fprintf(stderr, "invalid control listen ip\n");
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind tcp");
        close(fd);
        return -1;
    }

    if (listen(fd, CONTROL_LISTEN_BACKLOG) < 0)
    {
        perror("listen tcp");
        close(fd);
        return -1;
    }

    if (set_nonblock(fd) < 0)
    {
        perror("set_nonblock control listener");
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

static int parse_args(int argc, char *argv[], struct optentry *opt)
{
    int ch;

    memset(opt, 0, sizeof(*opt));
    opt->udp_port = (uint16_t)atoi(PORT);
    opt->control_port = DEFAULT_CONTROL_PORT;
    strncpy(opt->group, MYGRUOP, sizeof(opt->group) - 1);
    opt->group[sizeof(opt->group) - 1] = '\0';
    opt->player_path = "mpg123";
    opt->auto_channel = 0;
    opt->interactive_mode = 0;
    opt->enable_control = 1;

    while ((ch = getopt(argc, argv, "g:p:t:P:c:inh")) != -1)
    {
        switch (ch)
        {
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

int main(int argc, char *argv[])
{
    struct optentry opt;
    int sfd = INVALID_FD;
    int exit_code = 1;
    int fatal_error = 0;
    char *recv_buf = NULL;
    struct epoll_event events[MAX_EVENTS];
    struct fd_ctx sock_ctx;

    if (parse_args(argc, argv, &opt) < 0)
        return 1;

    install_signal_handlers();
    outbuf_init(&g_outbuf, OUTBUF_CAPACITY);
    jitter_reset();

    sfd = create_udp_receiver(&opt);
    if (sfd < 0)
        goto out;

    recv_buf = malloc(RECV_BUF_SIZE);
    if (recv_buf == NULL)
    {
        fprintf(stderr, "malloc recv_buf error\n");
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
        perror("set_nonblock udp");
        goto out;
    }

    g_epfd = epoll_create1(0);
    if (g_epfd < 0)
    {
        perror("epoll_create1");
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
        g_controller.listen_fd = create_control_listener(opt.control_port);
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
               CONTROL_LISTEN_IP, opt.control_port);
        fflush(stdout);
    }

    if (opt.auto_channel != 0 && start_playback(opt.auto_channel, &opt) < 0)
        goto out;

    while (!g_stop)
    {
        int nfds;
        int i;

        nfds = epoll_wait(g_epfd, events, MAX_EVENTS, EPOLL_WAIT_TIMEOUT_MS);
        if (nfds < 0)
        {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            fatal_error = 1;
            break;
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
    return exit_code;
}
