#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>

#include "proto.h"

#define RECV_BUF_SIZE  (NET_AUDIO_HDR_LEN + AUDIO_CHUNK_SIZE + 64)

/* ---------------- Jitter Buffer 参数 ---------------- */
#define JITTER_BUFFER_SIZE   64
#define START_BUFFER_PACKETS 6
#define SKIP_THRESHOLD       12
/* --------------------------------------------------- */

/* ---------------- epoll / 输出缓冲参数 ------------- */
#define MAX_EVENTS           8
#define OUTBUF_CAPACITY      (1024 * 1024)   /* 1MB 输出缓冲 */
#define FD_TAG_SOCK          1
#define FD_TAG_PIPE          2
/* --------------------------------------------------- */

struct audio_frame
{
    int valid;
    uint32_t seq;
    uint16_t payload_len;
    char data[AUDIO_CHUNK_SIZE];
};

static struct
{
    struct audio_frame frames[JITTER_BUFFER_SIZE];
    uint32_t stream_epoch;
    uint32_t expected_seq;
    uint32_t max_seq_seen;
    int is_first_packet;
    int started;
} jitter_buf;

struct outbuf
{
    char *buf;
    size_t cap;
    size_t head;   /* 已发送到的位置 */
    size_t tail;   /* 已写入数据末尾 */
};

struct optentry
{
    uint16_t port;
    uint32_t ip;
    char *player_path;
};

struct fd_ctx
{
    int tag;
    int fd;
};

static int g_epfd = -1;
static int g_pipe_wfd = -1;
static struct outbuf g_outbuf;

/* ======================= 工具函数 ======================= */

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

static void print_list_packet(const char *buf, int len)
{
    const char *pos;
    int remain;

    if (len < NET_LIST_HDR_LEN)
        return;

    if ((uint8_t)buf[0] != LIST_ID)
        return;

    if ((uint8_t)buf[1] != PKT_TYPE_LIST)
        return;

    pos = buf + NET_LIST_HDR_LEN;
    remain = len - NET_LIST_HDR_LEN;

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

        printf("list_id : %d  list_name : %.*s\n",
               ch_id, name_len, pos + 3);

        pos += 3 + name_len;
        remain -= 3 + name_len;
    }
}

static int parse_desc_packet(const char *buf, int len, uint8_t choose_id, int *desc_printed)
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

    if (*desc_printed == 0)
    {
        printf("desc: %.*s\n", desc_len, buf + NET_DESC_HDR_LEN);
        *desc_printed = 1;
    }

    return 1;
}

/* ======================= 输出缓冲 ======================= */

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
        ob->head = 0;
        ob->tail = 0;
        return;
    }

    memmove(ob->buf, ob->buf + ob->head, ob->tail - ob->head);
    ob->tail -= ob->head;
    ob->head = 0;
}

static int update_pipe_epoll_events(uint32_t extra_events, void *pipe_ctx)
{
    uint32_t ev = EPOLLERR | EPOLLHUP | extra_events;

    if (!outbuf_empty(&g_outbuf))
        ev |= EPOLLOUT;

    return epoll_mod_fd(g_epfd, g_pipe_wfd, ev, pipe_ctx);
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

static int flush_outbuf_to_pipe(int pipefd)
{
    while (!outbuf_empty(&g_outbuf))
    {
        ssize_t ret = write(pipefd,
                            g_outbuf.buf + g_outbuf.head,
                            g_outbuf.tail - g_outbuf.head);
        if (ret > 0)
        {
            g_outbuf.head += (size_t)ret;
            if (g_outbuf.head == g_outbuf.tail)
            {
                g_outbuf.head = 0;
                g_outbuf.tail = 0;
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
    if (outbuf_append(&g_outbuf, data, len) < 0)
    {
        fprintf(stderr, "\n[Error] output buffer overflow, drop audio\n");
        return -1;
    }

    if (flush_outbuf_to_pipe(g_pipe_wfd) < 0)
        return -1;

    if (update_pipe_epoll_events(0, pipe_ctx) < 0)
    {
        perror("epoll_mod pipe");
        return -1;
    }

    return 0;
}

/* ======================= Jitter Buffer ======================= */

static void jitter_reset_slot(int idx)
{
    jitter_buf.frames[idx].valid = 0;
    jitter_buf.frames[idx].seq = 0;
    jitter_buf.frames[idx].payload_len = 0;
}

static void jitter_reset_all(uint32_t stream_epoch, uint32_t first_seq)
{
    int i;

    for (i = 0; i < JITTER_BUFFER_SIZE; i++)
        jitter_reset_slot(i);

    jitter_buf.stream_epoch = stream_epoch;
    jitter_buf.expected_seq = first_seq;
    jitter_buf.max_seq_seen = first_seq;
    jitter_buf.is_first_packet = 0;
    jitter_buf.started = 0;
}

static int jitter_store_packet(uint32_t seq, const char *payload, uint16_t payload_len)
{
    int idx = seq % JITTER_BUFFER_SIZE;

    if (jitter_buf.frames[idx].valid && jitter_buf.frames[idx].seq != seq)
    {
        if (seq > jitter_buf.frames[idx].seq)
            jitter_reset_slot(idx);
        else
            return 0;
    }

    if (!jitter_buf.frames[idx].valid)
    {
        jitter_buf.frames[idx].seq = seq;
        jitter_buf.frames[idx].payload_len = payload_len;
        memcpy(jitter_buf.frames[idx].data, payload, payload_len);
        jitter_buf.frames[idx].valid = 1;
    }

    return 0;
}

static int jitter_should_start(void)
{
    if (jitter_buf.started)
        return 1;

    if (jitter_buf.max_seq_seen >= jitter_buf.expected_seq + (START_BUFFER_PACKETS - 1))
        return 1;

    return 0;
}

static int jitter_maybe_skip_missing(void)
{
    if (!jitter_buf.started)
        return 0;

    if (jitter_buf.max_seq_seen >= jitter_buf.expected_seq + SKIP_THRESHOLD)
    {
        fprintf(stderr,
                "\r[Warning] missing seq=%u, skip 1 packet...",
                jitter_buf.expected_seq);
        jitter_buf.expected_seq++;
        return 1;
    }

    return 0;
}

static int jitter_drain_to_outbuf(void *pipe_ctx)
{
    int idx;

    if (!jitter_should_start())
        return 0;

    jitter_buf.started = 1;

    while (1)
    {
        idx = jitter_buf.expected_seq % JITTER_BUFFER_SIZE;

        if (!jitter_buf.frames[idx].valid ||
            jitter_buf.frames[idx].seq != jitter_buf.expected_seq)
        {
            if (jitter_maybe_skip_missing())
                continue;
            break;
        }

        if (queue_audio_to_player(jitter_buf.frames[idx].data,
                                  jitter_buf.frames[idx].payload_len,
                                  pipe_ctx) < 0)
        {
            return -1;
        }

        jitter_reset_slot(idx);
        jitter_buf.expected_seq++;
    }

    return 0;
}

static int parse_audio_packet_and_queue(const char *buf, int len,
                                        uint8_t choose_id, void *pipe_ctx)
{
    uint8_t ch_id;
    uint8_t pkt_type;
    uint32_t net_epoch;
    uint32_t net_seq;
    uint16_t net_payload_len;
    uint32_t stream_epoch;
    uint32_t seq;
    uint16_t payload_len;

    if (len < NET_AUDIO_HDR_LEN)
        return 0;

    ch_id = (uint8_t)buf[0];
    pkt_type = (uint8_t)buf[1];

    if (ch_id != choose_id || pkt_type != PKT_TYPE_AUDIO)
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

    if (jitter_buf.is_first_packet)
    {
        jitter_reset_all(stream_epoch, seq);
    }
    else if (stream_epoch < jitter_buf.stream_epoch)
    {
        return 0;
    }
    else if (stream_epoch > jitter_buf.stream_epoch)
    {
        jitter_reset_all(stream_epoch, seq);
    }

    if (seq > jitter_buf.max_seq_seen)
        jitter_buf.max_seq_seen = seq;

    if (seq < jitter_buf.expected_seq)
        return 0;

    if (seq >= jitter_buf.expected_seq + JITTER_BUFFER_SIZE)
    {
        uint32_t new_expected = seq - (JITTER_BUFFER_SIZE - 1);
        uint32_t s;

        for (s = jitter_buf.expected_seq; s < new_expected; s++)
        {
            int idx = s % JITTER_BUFFER_SIZE;
            if (jitter_buf.frames[idx].valid && jitter_buf.frames[idx].seq == s)
                jitter_reset_slot(idx);
        }

        fprintf(stderr,
                "\r[Warning] buffer move forward: %u -> %u",
                jitter_buf.expected_seq, new_expected);

        jitter_buf.expected_seq = new_expected;
    }

    if (jitter_store_packet(seq, buf + NET_AUDIO_HDR_LEN, payload_len) < 0)
        return -1;

    return jitter_drain_to_outbuf(pipe_ctx);
}

/* ======================= 主逻辑 ======================= */

static int recv_until_list_packet(int sfd, char *recv_buf)
{
    int read_num;

    while (1)
    {
        read_num = recvfrom(sfd, recv_buf, RECV_BUF_SIZE, 0, NULL, NULL);
        if (read_num < 0)
        {
            if (errno == EINTR)
                continue;
            perror("recvfrom list");
            return -1;
        }

        if (read_num < NET_LIST_HDR_LEN)
            continue;

        if ((uint8_t)recv_buf[0] != LIST_ID)
            continue;

        if ((uint8_t)recv_buf[1] != PKT_TYPE_LIST)
            continue;

        print_list_packet(recv_buf, read_num);
        return 0;
    }
}

int main(int argc, char *argv[])
{
    struct optentry optentry_st;
    int sfd;
    int ret;
    struct sockaddr_in laddr;
    struct ip_mreqn mreq;
    char *recv_buf = NULL;
    int pipefd[2];
    pid_t pid;
    int read_num;
    int id_choose;
    int desc_printed = 0;
    struct epoll_event events[MAX_EVENTS];
    struct fd_ctx sock_ctx;
    struct fd_ctx pipe_ctx;

    (void)argc;
    (void)argv;

    signal(SIGPIPE, SIG_IGN);

    memset(&optentry_st, 0, sizeof(optentry_st));
    optentry_st.port = atoi(PORT);
    optentry_st.player_path = "mpg123";

    sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0)
    {
        perror("socket()");
        exit(1);
    }

    {
        int reuse = 1;
        ret = setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (ret < 0)
        {
            perror("setsockopt SO_REUSEADDR");
            close(sfd);
            exit(1);
        }
    }

    memset(&laddr, 0, sizeof(laddr));
    laddr.sin_family = AF_INET;
    laddr.sin_port = htons(optentry_st.port);
    laddr.sin_addr.s_addr = htonl(INADDR_ANY);

    ret = bind(sfd, (struct sockaddr *)&laddr, sizeof(laddr));
    if (ret < 0)
    {
        perror("bind()");
        close(sfd);
        exit(1);
    }

    memset(&mreq, 0, sizeof(mreq));
    ret = inet_pton(AF_INET, MYGRUOP, &mreq.imr_multiaddr.s_addr);
    if (ret <= 0)
    {
        fprintf(stderr, "inet_pton() group ip error\n");
        close(sfd);
        exit(1);
    }

    mreq.imr_address.s_addr = htonl(INADDR_ANY);
    mreq.imr_ifindex = 0;

    ret = setsockopt(sfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    if (ret < 0)
    {
        perror("setsockopt IP_ADD_MEMBERSHIP");
        close(sfd);
        exit(1);
    }

    recv_buf = malloc(RECV_BUF_SIZE);
    if (recv_buf == NULL)
    {
        fprintf(stderr, "malloc recv_buf error\n");
        close(sfd);
        exit(1);
    }

    if (recv_until_list_packet(sfd, recv_buf) < 0)
    {
        free(recv_buf);
        close(sfd);
        exit(1);
    }

    printf("choose id: ");
    fflush(stdout);

    if (scanf("%d", &id_choose) != 1)
    {
        fprintf(stderr, "scanf choose id error\n");
        free(recv_buf);
        close(sfd);
        exit(1);
    }

    if (id_choose < MIN_CHANAL_ID || id_choose > MAX_CHANAL_ID)
    {
        fprintf(stderr, "invalid chanal id\n");
        free(recv_buf);
        close(sfd);
        exit(1);
    }

    memset(&jitter_buf, 0, sizeof(jitter_buf));
    jitter_buf.is_first_packet = 1;
    jitter_buf.started = 0;

    outbuf_init(&g_outbuf, OUTBUF_CAPACITY);

    ret = pipe(pipefd);
    if (ret < 0)
    {
        perror("pipe()");
        free(g_outbuf.buf);
        free(recv_buf);
        close(sfd);
        exit(1);
    }

    pid = fork();
    if (pid < 0)
    {
        perror("fork()");
        free(g_outbuf.buf);
        free(recv_buf);
        close(pipefd[0]);
        close(pipefd[1]);
        close(sfd);
        exit(1);
    }
    else if (pid == 0)
    {
        close(pipefd[1]);

        ret = dup2(pipefd[0], STDIN_FILENO);
        if (ret < 0)
        {
            perror("dup2()");
            exit(1);
        }

        close(pipefd[0]);

        execlp(optentry_st.player_path, optentry_st.player_path, "-", "-q", NULL);
        perror("execlp()");
        exit(1);
    }

    close(pipefd[0]);

    if (set_nonblock(sfd) < 0)
    {
        perror("set_nonblock sfd");
        free(g_outbuf.buf);
        free(recv_buf);
        close(pipefd[1]);
        close(sfd);
        exit(1);
    }

    if (set_nonblock(pipefd[1]) < 0)
    {
        perror("set_nonblock pipefd[1]");
        free(g_outbuf.buf);
        free(recv_buf);
        close(pipefd[1]);
        close(sfd);
        exit(1);
    }

    g_pipe_wfd = pipefd[1];

    g_epfd = epoll_create1(0);
    if (g_epfd < 0)
    {
        perror("epoll_create1()");
        free(g_outbuf.buf);
        free(recv_buf);
        close(pipefd[1]);
        close(sfd);
        exit(1);
    }

    sock_ctx.tag = FD_TAG_SOCK;
    sock_ctx.fd = sfd;
    pipe_ctx.tag = FD_TAG_PIPE;
    pipe_ctx.fd = pipefd[1];

    if (epoll_add_fd(g_epfd, sfd, EPOLLIN | EPOLLERR | EPOLLHUP, &sock_ctx) < 0)
    {
        perror("epoll_add sfd");
        free(g_outbuf.buf);
        free(recv_buf);
        close(g_epfd);
        close(pipefd[1]);
        close(sfd);
        exit(1);
    }

    if (epoll_add_fd(g_epfd, pipefd[1], EPOLLERR | EPOLLHUP, &pipe_ctx) < 0)
    {
        perror("epoll_add pipe");
        free(g_outbuf.buf);
        free(recv_buf);
        close(g_epfd);
        close(pipefd[1]);
        close(sfd);
        exit(1);
    }

    while (1)
    {
        int nfds, i;

        nfds = epoll_wait(g_epfd, events, MAX_EVENTS, -1);
        if (nfds < 0)
        {
            if (errno == EINTR)
                continue;
            perror("epoll_wait()");
            break;
        }

        for (i = 0; i < nfds; i++)
        {
            struct fd_ctx *ctx = (struct fd_ctx *)events[i].data.ptr;
            uint32_t ev = events[i].events;

            if (ev & (EPOLLERR | EPOLLHUP))
            {
                fprintf(stderr, "\nfd error/hup, exit\n");
                goto out;
            }

            if (ctx->tag == FD_TAG_SOCK)
            {
                while (1)
                {
                    read_num = recvfrom(sfd, recv_buf, RECV_BUF_SIZE, 0, NULL, NULL);
                    if (read_num < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        if (errno == EINTR)
                            continue;
                        perror("recvfrom data");
                        goto out;
                    }

                    if (read_num < 2)
                        continue;

                    if ((uint8_t)recv_buf[0] == LIST_ID &&
                        (uint8_t)recv_buf[1] == PKT_TYPE_LIST)
                    {
                        continue;
                    }

                    if (parse_desc_packet(recv_buf, read_num,
                                          (uint8_t)id_choose,
                                          &desc_printed))
                    {
                        continue;
                    }

                    if ((uint8_t)recv_buf[1] == PKT_TYPE_AUDIO)
                    {
                        ret = parse_audio_packet_and_queue(recv_buf,
                                                           read_num,
                                                           (uint8_t)id_choose,
                                                           &pipe_ctx);
                        if (ret < 0)
                            goto out;
                    }
                }
            }
            else if (ctx->tag == FD_TAG_PIPE)
            {
                if (ev & EPOLLOUT)
                {
                    if (flush_outbuf_to_pipe(pipefd[1]) < 0)
                        goto out;

                    if (update_pipe_epoll_events(0, &pipe_ctx) < 0)
                    {
                        perror("epoll_mod pipe");
                        goto out;
                    }

                    /*
                     * 管道重新可写后，再尝试从 jitter buffer 继续排水，
                     * 防止之前因为输出阻塞导致缓冲里还有顺序包没排完。
                     */
                    if (jitter_drain_to_outbuf(&pipe_ctx) < 0)
                        goto out;
                }
            }
        }
    }

out:
    if (g_epfd >= 0)
        close(g_epfd);
    if (pipefd[1] >= 0)
        close(pipefd[1]);
    if (sfd >= 0)
        close(sfd);
    free(recv_buf);
    free(g_outbuf.buf);
    return 0;
}
