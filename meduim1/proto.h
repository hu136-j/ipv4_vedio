#ifndef PROTO_H__
#define PROTO_H__

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#define MYGRUOP    "224.1.1.1"
#define PORT       "8888"

#define CHANAL_NUM  100
#define LIST_ID     0

#define MIN_CHANAL_ID 1
#define MAX_CHANAL_ID 99

#define DESC_MAX    256

#define MAX_CHANAL_ST   (1024*32 - 20 - 8)
#define CHANAL_ST_BASE_SIZE (sizeof(uint8_t) + sizeof(uint16_t) + DESC_MAX + sizeof(uint32_t) + sizeof(char))
#define MAX_GAME         (MAX_CHANAL_ST - CHANAL_ST_BASE_SIZE)

struct chanal_st
{
    uint8_t  chanal_id;
    uint16_t desc_len;
    char     desc[DESC_MAX];
    uint32_t bitrate_bytes_per_sec;
    uint32_t game_len;
    char     game[1];
} __attribute__((packed));

#define MAX_LIST_ST   (1024*8 - 20 - 8)

struct listentry_st
{
    uint8_t  chanal_id;
    uint16_t length;
    char     name[1];
} __attribute__((packed));

struct list_st
{
    uint8_t chanal_id;
    struct listentry_st entry[1];
} __attribute__((packed));

/* 包类型 */
#define PKT_TYPE_LIST   1
#define PKT_TYPE_DESC   2
#define PKT_TYPE_AUDIO  3

/* 每个音频 UDP 包的负载大小，尽量控制在 MTU 内 */
#define AUDIO_CHUNK_SIZE 1024

/* 线上的头字段长度（手动拼包，不直接发结构体） */
#define NET_LIST_HDR_LEN   2   /* chanal_id(1) + pkt_type(1) */
#define NET_DESC_HDR_LEN   5   /* chanal_id(1) + pkt_type(1) + desc_len(2) + reserved(1) */
#define NET_AUDIO_HDR_LEN  12  /* chanal_id(1) + pkt_type(1) + stream_epoch(4) + seq(4) + payload_len(2) */

#endif
