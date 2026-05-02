#include <arpa/inet.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "meduim.h"

#define DEFAULT_AUDIO_BYTES_PER_SEC 12000U
#define MIN_AUDIO_BYTES_PER_SEC      4000U
#define MAX_AUDIO_BYTES_PER_SEC      64000U

struct chanal_st *chanal_buff[CHANAL_NUM] = { NULL };
struct listentry_st *list_buff[CHANAL_NUM] = { NULL };
struct token_entyr *tbf_arr[CHANAL_NUM] = { NULL };
int sfd;
struct sockaddr_in ser_sock;

static int has_extension(const char *path, const char *ext)
{
    const char *dot;

    if (path == NULL || ext == NULL)
        return 0;

    dot = strrchr(path, '.');
    if (dot == NULL)
        return 0;

    return strcmp(dot + 1, ext) == 0;
}

static struct listentry_st *alloc_listentry(uint8_t channel_id, const char *name)
{
    size_t name_len;
    struct listentry_st *entry;

    if (name == NULL)
        return NULL;

    name_len = strlen(name);
    if (name_len == 0 || name_len > UINT16_MAX)
        return NULL;

    entry = malloc(sizeof(*entry) + name_len);
    if (entry == NULL)
        return NULL;

    memset(entry, 0, sizeof(*entry) + name_len);
    entry->chanal_id = channel_id;
    entry->length = (uint16_t)name_len;
    memcpy(entry->name, name, name_len);
    entry->name[name_len] = '\0';

    return entry;
}

static uint32_t clamp_audio_rate(uint32_t bytes_per_sec)
{
    if (bytes_per_sec < MIN_AUDIO_BYTES_PER_SEC)
        return MIN_AUDIO_BYTES_PER_SEC;

    if (bytes_per_sec > MAX_AUDIO_BYTES_PER_SEC)
        return MAX_AUDIO_BYTES_PER_SEC;

    return bytes_per_sec;
}

static uint32_t bitrate_kbps_to_bytes_per_sec(uint32_t bitrate_kbps)
{
    if (bitrate_kbps == 0)
        return DEFAULT_AUDIO_BYTES_PER_SEC;

    return clamp_audio_rate((bitrate_kbps * 1000U) / 8U);
}

static uint32_t parse_mp3_bitrate_bytes_per_sec(const char *audio_path)
{
    static const int bitrate_table[2][3][16] = {
        {
            { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },
            { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 },
            { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 }
        },
        {
            { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
            { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
            { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 0 }
        }
    };
    int fd;
    unsigned char buf[10];
    ssize_t nread;
    off_t offset = 0;
    uint32_t header;
    int version_id;
    int layer_bits;
    int bitrate_index;
    int version_group;
    int layer_index;
    int bitrate_kbps;

    if (audio_path == NULL)
        return DEFAULT_AUDIO_BYTES_PER_SEC;

    fd = open(audio_path, O_RDONLY);
    if (fd < 0)
        return DEFAULT_AUDIO_BYTES_PER_SEC;

    nread = read(fd, buf, sizeof(buf));
    if (nread == (ssize_t)sizeof(buf) &&
        memcmp(buf, "ID3", 3) == 0)
    {
        offset = 10 +
                 ((off_t)(buf[6] & 0x7f) << 21) +
                 ((off_t)(buf[7] & 0x7f) << 14) +
                 ((off_t)(buf[8] & 0x7f) << 7) +
                 (off_t)(buf[9] & 0x7f);
    }

    if (lseek(fd, offset, SEEK_SET) < 0)
    {
        close(fd);
        return DEFAULT_AUDIO_BYTES_PER_SEC;
    }

    while (1)
    {
        nread = read(fd, buf, 4);
        if (nread < 4)
            break;

        header = ((uint32_t)buf[0] << 24) |
                 ((uint32_t)buf[1] << 16) |
                 ((uint32_t)buf[2] << 8) |
                 (uint32_t)buf[3];

        if ((header & 0xffe00000U) != 0xffe00000U)
        {
            if (lseek(fd, -3, SEEK_CUR) < 0)
                break;
            continue;
        }

        version_id = (int)((header >> 19) & 0x3U);
        layer_bits = (int)((header >> 17) & 0x3U);
        bitrate_index = (int)((header >> 12) & 0xfU);

        if (version_id == 1 || layer_bits == 0 || bitrate_index == 0 || bitrate_index == 15)
        {
            if (lseek(fd, -3, SEEK_CUR) < 0)
                break;
            continue;
        }

        version_group = (version_id == 3) ? 0 : 1;

        if (layer_bits == 3)
            layer_index = 0;
        else if (layer_bits == 2)
            layer_index = 1;
        else
            layer_index = 2;

        bitrate_kbps = bitrate_table[version_group][layer_index][bitrate_index];
        close(fd);

        if (bitrate_kbps <= 0)
            return DEFAULT_AUDIO_BYTES_PER_SEC;

        return bitrate_kbps_to_bytes_per_sec((uint32_t)bitrate_kbps);
    }

    close(fd);
    return DEFAULT_AUDIO_BYTES_PER_SEC;
}

static struct chanal_st *alloc_channel(uint8_t channel_id, const char *audio_path)
{
    size_t path_len;
    struct chanal_st *channel;

    if (audio_path == NULL)
        return NULL;

    path_len = strlen(audio_path);
    if (path_len == 0 || path_len > MAX_GAME)
        return NULL;

    channel = malloc(sizeof(*channel) + path_len);
    if (channel == NULL)
        return NULL;

    memset(channel, 0, sizeof(*channel) + path_len);
    channel->chanal_id = channel_id;
    channel->bitrate_bytes_per_sec = parse_mp3_bitrate_bytes_per_sec(audio_path);
    channel->game_len = (uint32_t)path_len;
    memcpy(channel->game, audio_path, path_len);
    channel->game[path_len] = '\0';

    return channel;
}

static int load_desc_file(const char *path, struct chanal_st *channel)
{
    int fd;
    ssize_t len;

    if (path == NULL || channel == NULL)
        return -1;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    len = read(fd, channel->desc, DESC_MAX);
    close(fd);

    if (len < 0)
        return -1;

    channel->desc_len = (uint16_t)len;
    return 0;
}

static int load_channel_dir(const char *dir_path, uint8_t channel_id)
{
    char pattern[PATH_MAX];
    glob_t glob_res;
    char *audio_path = NULL;
    char *desc_path = NULL;
    size_t i;
    int ret;
    const char *audio_name;
    struct chanal_st *channel = NULL;
    struct listentry_st *entry = NULL;

    if (dir_path == NULL)
        return -1;

    ret = snprintf(pattern, sizeof(pattern), "%s/*", dir_path);
    if (ret < 0 || (size_t)ret >= sizeof(pattern))
        return -1;

    ret = glob(pattern, 0, NULL, &glob_res);
    if (ret != 0)
        return -1;

    for (i = 0; i < glob_res.gl_pathc; i++)
    {
        if (audio_path == NULL && has_extension(glob_res.gl_pathv[i], "mp3"))
            audio_path = glob_res.gl_pathv[i];
        else if (desc_path == NULL && has_extension(glob_res.gl_pathv[i], "txt"))
            desc_path = glob_res.gl_pathv[i];
    }

    if (audio_path == NULL)
    {
        globfree(&glob_res);
        return 0;
    }

    audio_name = strrchr(audio_path, '/');
    if (audio_name == NULL)
        audio_name = audio_path;
    else
        audio_name++;

    channel = alloc_channel(channel_id, audio_path);
    entry = alloc_listentry(channel_id, audio_name);
    if (channel == NULL || entry == NULL)
    {
        free(channel);
        free(entry);
        globfree(&glob_res);
        return -1;
    }

    if (desc_path != NULL && load_desc_file(desc_path, channel) < 0)
    {
        free(channel);
        free(entry);
        globfree(&glob_res);
        return -1;
    }

    chanal_buff[channel_id - 1] = channel;
    list_buff[channel_id - 1] = entry;

    fprintf(stderr,
            "loaded channel %u: %s, rate=%u B/s\n",
            channel_id, audio_name, channel->bitrate_bytes_per_sec);

    globfree(&glob_res);
    return 1;
}

int load_channels(const char *dir_path)
{
    char pattern[PATH_MAX];
    glob_t glob_res;
    size_t i;
    int ret;
    int channel_count = 0;

    if (dir_path == NULL)
        return -1;

    ret = snprintf(pattern, sizeof(pattern), "%s/*", dir_path);
    if (ret < 0 || (size_t)ret >= sizeof(pattern))
        return -1;

    ret = glob(pattern, GLOB_ONLYDIR, NULL, &glob_res);
    if (ret != 0)
        return -1;

    for (i = 0; i < glob_res.gl_pathc && channel_count < MAX_CHANAL_ID; i++)
    {
        int load_ret;

        load_ret = load_channel_dir(glob_res.gl_pathv[i], (uint8_t)(channel_count + 1));
        if (load_ret < 0)
        {
            globfree(&glob_res);
            return -1;
        }

        if (load_ret == 0)
        {
            fprintf(stderr, "skip invalid channel dir: %s\n", glob_res.gl_pathv[i]);
            continue;
        }

        channel_count++;
    }

    globfree(&glob_res);
    return channel_count;
}

void buff_destory(void)
{
    int i;

    for (i = 0; i < CHANAL_NUM; i++)
    {
        free(list_buff[i]);
        list_buff[i] = NULL;

        free(chanal_buff[i]);
        chanal_buff[i] = NULL;

        if (tbf_arr[i] != NULL)
        {
            token_alldestry(tbf_arr[i]);
            tbf_arr[i] = NULL;
        }
    }
}

int send_desc_packet(struct chanal_st *opt)
{
    char buf[NET_DESC_HDR_LEN + DESC_MAX];
    uint16_t net_desc_len;
    int pkt_len;

    if (opt == NULL)
        return -1;

    net_desc_len = htons(opt->desc_len);

    buf[0] = opt->chanal_id;
    buf[1] = PKT_TYPE_DESC;
    memcpy(buf + 2, &net_desc_len, sizeof(net_desc_len));
    buf[4] = 0;
    memcpy(buf + NET_DESC_HDR_LEN, opt->desc, opt->desc_len);

    pkt_len = NET_DESC_HDR_LEN + opt->desc_len;

    return sendto(sfd, buf, pkt_len, 0,
                  (struct sockaddr *)&ser_sock, sizeof(ser_sock));
}

int send_audio_packet(struct chanal_st *opt, uint32_t stream_epoch, uint32_t seq,
                      const char *payload, uint16_t payload_len)
{
    char buf[NET_AUDIO_HDR_LEN + AUDIO_CHUNK_SIZE];
    uint32_t net_epoch;
    uint32_t net_seq;
    uint16_t net_payload_len;
    int pkt_len;

    if (opt == NULL || payload == NULL || payload_len > AUDIO_CHUNK_SIZE)
        return -1;

    net_epoch = htonl(stream_epoch);
    net_seq = htonl(seq);
    net_payload_len = htons(payload_len);

    buf[0] = opt->chanal_id;
    buf[1] = PKT_TYPE_AUDIO;
    memcpy(buf + 2, &net_epoch, sizeof(net_epoch));
    memcpy(buf + 6, &net_seq, sizeof(net_seq));
    memcpy(buf + 10, &net_payload_len, sizeof(net_payload_len));
    memcpy(buf + NET_AUDIO_HDR_LEN, payload, payload_len);

    pkt_len = NET_AUDIO_HDR_LEN + payload_len;

    return sendto(sfd, buf, pkt_len, 0,
                  (struct sockaddr *)&ser_sock, sizeof(ser_sock));
}

void *send_chanal(void *ptr)
{
    struct chanal_st *opt = ptr;
    uint32_t stream_epoch = 0;
    uint32_t seq = 0;
    int ch_id;
    int ret;
    int fd;
    ssize_t read_len;
    char audio_buf[AUDIO_CHUNK_SIZE];

    if (opt == NULL)
        return NULL;

    ch_id = opt->chanal_id - 1;

    while (!server_stop)
    {
        ret = send_desc_packet(opt);
        if (ret < 0)
        {
            perror("send_desc_packet");
            usleep(10000);
        }

        fd = open(opt->game, O_RDONLY);
        if (fd < 0)
        {
            fprintf(stderr, "open mp3 failed, chanal_id=%d path=%s\n",
                    opt->chanal_id, opt->game);
            usleep(100000);
            continue;
        }

        stream_epoch++;
        seq = 0;

        while (!server_stop)
        {
            read_len = read(fd, audio_buf, sizeof(audio_buf));
            if (read_len < 0)
            {
                perror("read mp3");
                break;
            }
            if (read_len == 0)
                break;

            if (ch_id >= 0 && ch_id < CHANAL_NUM && tbf_arr[ch_id] != NULL)
            {
                ret = get_token(tbf_arr[ch_id], (uint32_t)read_len);
                if (ret < 0)
                {
                    fprintf(stderr, "get_token error, chanal_id=%d\n", opt->chanal_id);
                    usleep(10000);
                    continue;
                }
            }

            ret = send_audio_packet(opt, stream_epoch, seq,
                                    audio_buf, (uint16_t)read_len);
            if (ret < 0)
            {
                perror("send_audio_packet");
                usleep(10000);
                continue;
            }

            seq++;
        }

        close(fd);
    }

    return NULL;
}

int build_list_packet(char *buf, int buf_size)
{
    int i;
    int pos = 0;
    uint16_t net_len;

    if (buf == NULL || buf_size < NET_LIST_HDR_LEN)
        return -1;

    buf[pos++] = LIST_ID;
    buf[pos++] = PKT_TYPE_LIST;

    for (i = 0; i < CHANAL_NUM && list_buff[i] != NULL; i++)
    {
        int name_len = list_buff[i]->length;

        if (pos + 1 + 2 + name_len > buf_size)
            break;

        buf[pos++] = list_buff[i]->chanal_id;

        net_len = htons((uint16_t)name_len);
        memcpy(buf + pos, &net_len, sizeof(net_len));
        pos += sizeof(net_len);

        memcpy(buf + pos, list_buff[i]->name, name_len);
        pos += name_len;
    }

    return pos;
}
