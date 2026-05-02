#include <glob.h>
#include <stdio.h>
#include "proto.h"
#include "token.h"
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include "meduim.h"

struct chanal_st* chanal_buff[CHANAL_NUM] = { NULL };
struct listentry_st* list_buff[CHANAL_NUM] = { NULL };
struct token_entyr* tbf_arr[CHANAL_NUM] = { NULL };
int sfd;
struct sockaddr_in ser_sock;
char type_id[CHANAL_NUM] = { 0 };

static struct chanal_st* get_filentry(glob_t* dir_path, char* file_type)
{
    int fd;
    int count;
    char count1 = 0;

    struct chanal_st* ptr = malloc(sizeof(char) * MAX_CHANAL_ST);
    if (ptr == NULL)
    {
        return NULL;
    }

    memset(ptr, 0, MAX_CHANAL_ST);

    while (*file_type != 0)
    {
        if (*file_type == 1)
        {
            fd = open(dir_path->gl_pathv[(int)count1], O_RDONLY);
            if (fd < 0)
            {
                free(ptr);
                return NULL;
            }

            count = read(fd, ptr->desc, DESC_MAX);
            if (count < 0)
            {
                close(fd);
                free(ptr);
                return NULL;
            }

            ptr->desc_len = count;
            close(fd);
        }
        else if (*file_type == 2)
        {
            strncpy(ptr->game, dir_path->gl_pathv[(int)count1], MAX_GAME - 1);
            ptr->game[MAX_GAME - 1] = '\0';
            ptr->game_len = 0;
        }

        file_type++;
        count1++;
    }

    return ptr;
}

static int8_t get_filetype(const char* dir_path)
{
    if (dir_path == NULL)
        return -1;

    const char* opt = strrchr(dir_path, '.');
    if (opt == NULL)
        return -1;

    if (strcmp(opt + 1, "txt") == 0)
        return 1;
    else if (strcmp(opt + 1, "mp3") == 0)
        return 2;
    else
        return -2;
}

static int8_t get_chanalentry(const char* dir_path, uint8_t pos)
{
    if (dir_path == NULL)
        return -1;

    char pattern[256] = { 0 };
    glob_t glob_res;
    int i;
    int8_t ret;

    snprintf(pattern, sizeof(pattern), "%s/*", dir_path);

    ret = glob(pattern, 0, NULL, &glob_res);
    if (ret != 0)
    {
        perror("get_chanalentry glob()");
        exit(1);
    }

    memset(type_id, 0, sizeof(type_id));

    for (i = 0; i < (int)glob_res.gl_pathc && i < CHANAL_NUM; i++)
    {
        type_id[i] = get_filetype(glob_res.gl_pathv[i]);
    }

    chanal_buff[pos] = get_filentry(&glob_res, type_id);

    if (chanal_buff[pos] != NULL)
    {
        chanal_buff[pos]->chanal_id = pos + 1;
    }
    else
    {
        globfree(&glob_res);
        return -1;
    }

    globfree(&glob_res);
    return 1;
}

static struct listentry_st* get_listentry(const char* dir_path, uint8_t pos)
{
    if (dir_path == NULL)
    {
        return NULL;
    }

    int8_t ret;
    glob_t glob_res;
    char pattern[256];
    int i;
    char* filename;
    char* opt;
    struct listentry_st* ptr = NULL;

    memset(pattern, 0, sizeof(pattern));
    snprintf(pattern, sizeof(pattern), "%s/*", dir_path);

    ret = glob(pattern, 0, NULL, &glob_res);
    if (ret != 0)
    {
        perror("get_listentry glob()");
        exit(1);
    }

    for (i = 0; i < (int)glob_res.gl_pathc; i++)
    {
        filename = strrchr(glob_res.gl_pathv[i], '/');
        if (filename == NULL)
            continue;

        opt = strrchr(filename, '.');
        if (opt == NULL)
            continue;

        if (strcmp(opt + 1, "mp3") == 0)
        {
            ptr = malloc(sizeof(uint8_t) * MAX_LIST_ST);
            if (ptr == NULL)
            {
                fprintf(stderr, "listentry malloc error\n");
                globfree(&glob_res);
                exit(1);
            }

            memset(ptr, 0, MAX_LIST_ST);

            ptr->chanal_id = pos + 1;
            ptr->length = strlen(filename) - 1;

            strncpy(ptr->name, filename + 1, ptr->length);
            ptr->name[ptr->length] = '\0';
        }
    }

    globfree(&glob_res);
    return ptr;
}

void get_list(const char* dir_path)
{
    if (dir_path == NULL)
    {
        exit(1);
    }

    char pattern[256];
    glob_t glob_res;
    int i;
    int8_t ret;

    sprintf(pattern, "%s/*", dir_path);

    ret = glob(pattern, GLOB_ONLYDIR, NULL, &glob_res);
    if (ret != 0)
    {
        perror("get_list glob()");
        exit(1);
    }

    for (i = 0; i < (int)glob_res.gl_pathc && i < CHANAL_NUM; i++)
    {
        list_buff[i] = get_listentry(glob_res.gl_pathv[i], i);
    }

    globfree(&glob_res);
}

int8_t chanale_init(const char* dir_path)
{
    if (dir_path == NULL)
    {
        return -1;
    }

    char pattern[256];
    glob_t glob_res;
    int i;
    int8_t ret;

    sprintf(pattern, "%s/*", dir_path);

    ret = glob(pattern, GLOB_ONLYDIR, NULL, &glob_res);
    if (ret != 0)
    {
        perror("chanale_init glob()");
        exit(1);
    }

    for (i = 0; i < (int)glob_res.gl_pathc && i < CHANAL_NUM; i++)
    {
        ret = get_chanalentry(glob_res.gl_pathv[i], i);
        if (ret < 0)
        {
            fprintf(stdout, "%s error\n", glob_res.gl_pathv[i]);
            globfree(&glob_res);
            return -1;
        }
    }

    globfree(&glob_res);
    return 1;
}

void buff_destory(void)
{
    int i;
    for (i = 0; i < CHANAL_NUM; i++)
    {
        if (list_buff[i] != NULL)
            free(list_buff[i]);

        if (chanal_buff[i] != NULL)
            free(chanal_buff[i]);

        if (tbf_arr[i] != NULL)
            token_alldestry(tbf_arr[i]);
    }
}

int send_desc_packet(struct chanal_st* opt)
{
    char buf[NET_DESC_HDR_LEN + DESC_MAX];
    uint16_t net_desc_len;
    int pkt_len;
    int ret;

    net_desc_len = htons(opt->desc_len);

    buf[0] = opt->chanal_id;
    buf[1] = PKT_TYPE_DESC;
    memcpy(buf + 2, &net_desc_len, sizeof(net_desc_len));
    buf[4] = 0;

    memcpy(buf + NET_DESC_HDR_LEN, opt->desc, opt->desc_len);

    pkt_len = NET_DESC_HDR_LEN + opt->desc_len;

    ret = sendto(sfd, buf, pkt_len, 0,
        (struct sockaddr*)&ser_sock, sizeof(ser_sock));
    return ret;
}

int send_audio_packet(struct chanal_st* opt, uint32_t stream_epoch, uint32_t seq,
    const char* payload, uint16_t payload_len)
{
    char buf[NET_AUDIO_HDR_LEN + AUDIO_CHUNK_SIZE];
    uint32_t net_epoch;
    uint32_t net_seq;
    uint16_t net_payload_len;
    int pkt_len;
    int ret;

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

    ret = sendto(sfd, buf, pkt_len, 0,
        (struct sockaddr*)&ser_sock, sizeof(ser_sock));
    return ret;
}

void* send_chanal(void* ptr)
{
    struct chanal_st* opt = ptr;
    uint32_t stream_epoch = 0;
    uint32_t seq = 0;
    int ch_id;
    int ret;
    int fd;
    int read_len;
    char audio_buf[AUDIO_CHUNK_SIZE];

    ch_id = opt->chanal_id - 1;

    while (!server_stop)
    {
        ret = send_desc_packet(opt);
        if (ret < 0)
        {
            perror("send_desc_packet()");
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
            read_len = read(fd, audio_buf, AUDIO_CHUNK_SIZE);
            if (read_len < 0)
            {
                perror("read mp3");
                break;
            }
            if (read_len == 0)
            {
                break;
            }

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

            ret = send_audio_packet(opt, stream_epoch, seq, audio_buf, (uint16_t)read_len);
            if (ret < 0)
            {
                perror("send_audio_packet()");
                usleep(10000);
                continue;
            }

            seq++;
        }

        close(fd);
    }

    return NULL;
}

int build_list_packet(char* buf, int buf_size)
{
    int i;
    int pos = 0;
    uint16_t net_len;

    if (buf_size < NET_LIST_HDR_LEN)
        return -1;

    buf[pos++] = LIST_ID;
    buf[pos++] = PKT_TYPE_LIST;

    for (i = 0; i < CHANAL_NUM && list_buff[i] != NULL; i++)
    {
        int name_len = list_buff[i]->length;

        if (pos + 1 + 2 + name_len > buf_size)
            break;

        buf[pos++] = list_buff[i]->chanal_id;

        net_len = htons(name_len);
        memcpy(buf + pos, &net_len, sizeof(net_len));
        pos += sizeof(net_len);

        memcpy(buf + pos, list_buff[i]->name, name_len);
        pos += name_len;
    }

    return pos;
}

