#ifndef PROTO_H__
#define PROTO_H__

#include<stdio.h>
#include<stdint.h>
#include<stdlib.h>
#include<unistd.h>
#include<string.h>


#define MYGRUOP    "224.1.1.1"
#define port       "8888"

#define CHANAL_NUM  100
#define LIST_ID     0

#define MIN_CHANAL_ID 1
#define MAX_CHANAL_ID 99

#define MAX_CHANAL_ST   (1024*8 - 20 - 8)
#define MAX_GAME         MAX_CHANAL_ST - sizeof(uint8_t)

struct  chanal_st
{
	uint8_t chanal_id;
	char    game[1];
}__attribute__((packed));


#define MAX_LIST_NAME   (1024*8 - 20 - 8)
#define MAX_NAME		MAX_LIST_ST- sizeof(uint8_t)

struct listentry_st
{
	uint8_t  chanal_id;
	uint16_t length;
	char     name[1];
}__attribute__((packed));


#define MAX_LIST_ST   (1024*8 - 20 - 8)
#define MAX_LIST      MAX_LIST_ST- sizeof(uint8_t)

struct list_st
{
	uint8_t			chanal_id;
	listentry_st    entry[1];
}__attribute__((packed));


#endif
