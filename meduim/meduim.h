#ifndef MEDUIM_H__
#define MEDUIM_H__

#include <stdint.h>
#include <arpa/inet.h>
#include "proto.h"
#include "token.h"
#include <signal.h>

extern struct chanal_st* chanal_buff[CHANAL_NUM];
extern struct listentry_st* list_buff[CHANAL_NUM];
extern struct token_entyr* tbf_arr[CHANAL_NUM];
extern int sfd;
extern struct sockaddr_in ser_sock;

extern volatile sig_atomic_t server_stop;



void get_list(const char* dir_path);
int8_t chanale_init(const char* dir_path);
void buff_destory(void);

int send_desc_packet(struct chanal_st* opt);
int send_audio_packet(struct chanal_st* opt, uint32_t seq,
    const char* payload, uint16_t payload_len);
void* send_chanal(void* ptr);

int build_list_packet(char* buf, int buf_size);

#endif

