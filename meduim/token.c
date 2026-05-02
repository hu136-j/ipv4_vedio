



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <getopt.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include"token.h"

#define TOKEN_MAXNUM 100

struct token_entyr
{
    uint32_t brust_token;   // 桶容量上限
    uint32_t speed_token;   // 每秒补充多少令牌
    uint32_t my_token;      // 当前可用令牌数
    int      pos_token;     // -1 表示空闲，其余表示所在槽位
    pthread_mutex_t token_mutex;
    pthread_cond_t  token_cond;
};

static struct token_entyr token_buff[TOKEN_MAXNUM];
static pthread_mutex_t buff_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t pid;
static pthread_once_t once = PTHREAD_ONCE_INIT;

/* 找空闲槽位 */
static int find_free(void)
{
    int i;
    for (i = 0; i < TOKEN_MAXNUM; i++)
    {
        if (token_buff[i].pos_token == -1)
            return i;
    }
    return -1;
}

/*
 * 后台线程：按 100ms 周期补一次令牌。
 * 现有服务端参数是按这个补充节奏调过的，改成 1s 会直接导致推流速率过低。
 */
static void* gene_token(void* p)
{
    int i;
    (void)p;

    while (1)
    {
        pthread_mutex_lock(&buff_mutex);

        for (i = 0; i < TOKEN_MAXNUM; i++)
        {
            if (token_buff[i].pos_token != -1)
            {
                pthread_mutex_lock(&token_buff[i].token_mutex);

                token_buff[i].my_token += token_buff[i].speed_token;
                if (token_buff[i].my_token > token_buff[i].brust_token)
                    token_buff[i].my_token = token_buff[i].brust_token;

                pthread_cond_broadcast(&token_buff[i].token_cond);
                pthread_mutex_unlock(&token_buff[i].token_mutex);
            }
        }

        pthread_mutex_unlock(&buff_mutex);
        usleep(100000);
    }

    return NULL;
}

/* 初始化固定池 */
static void buff_init(void)
{
    int i;
    for (i = 0; i < TOKEN_MAXNUM; i++)
    {
        token_buff[i].brust_token = 0;
        token_buff[i].speed_token = 0;
        token_buff[i].my_token = 0;
        token_buff[i].pos_token = -1;
    }
}

/* 进程退出时清理模块 */
static void moudle_destory(void)
{
    int i;

    pthread_cancel(pid);
    pthread_join(pid, NULL);

    for (i = 0; i < TOKEN_MAXNUM; i++)
    {
        if (token_buff[i].pos_token != -1)
        {
            pthread_mutex_destroy(&token_buff[i].token_mutex);
            pthread_cond_destroy(&token_buff[i].token_cond);
            token_buff[i].pos_token = -1;
        }
    }
}

/* 只初始化一次 */
static void moudle_once(void)
{
    int ret;

    buff_init();

    ret = pthread_create(&pid, NULL, gene_token, NULL);
    if (ret != 0)
    {
        fprintf(stderr, "pthread_create() error\n");
        exit(1);
    }

    atexit(moudle_destory);
}

/*
 * 创建一个令牌桶
 * 成功返回桶指针
 * 失败返回 NULL
 */
struct token_entyr* token_init(uint32_t brust_token, uint32_t speed_token)
{
    int pos;

    pthread_once(&once, moudle_once);

    pthread_mutex_lock(&buff_mutex);

    pos = find_free();
    if (pos < 0)
    {
        fprintf(stdout, "no token free\n");
        pthread_mutex_unlock(&buff_mutex);
        return NULL;
    }

    token_buff[pos].brust_token = brust_token;
    token_buff[pos].speed_token = speed_token;
    token_buff[pos].my_token = brust_token;   // 初始给满，更适合流媒体启动
    token_buff[pos].pos_token = pos;

    pthread_mutex_init(&token_buff[pos].token_mutex, NULL);
    pthread_cond_init(&token_buff[pos].token_cond, NULL);

    pthread_mutex_unlock(&buff_mutex);

    return &token_buff[pos];
}

/* 销毁单个令牌桶：归还到固定池，不 free */
void token_alldestry(struct token_entyr* ptr)
{
    if (ptr == NULL)
        return;

    pthread_mutex_lock(&buff_mutex);

    if (ptr->pos_token != -1)
    {
        pthread_mutex_destroy(&ptr->token_mutex);
        pthread_cond_destroy(&ptr->token_cond);

        ptr->brust_token = 0;
        ptr->speed_token = 0;
        ptr->my_token = 0;
        ptr->pos_token = -1;
    }

    pthread_mutex_unlock(&buff_mutex);
}

/*
 * 取令牌
 * 成功返回实际取到的令牌数（这里等于请求值）
 * 失败返回 -1
 */
int32_t get_token(struct token_entyr* ptr, uint32_t need_token)
{
    if (ptr == NULL)
        return -1;

    pthread_mutex_lock(&ptr->token_mutex);

    if (ptr->pos_token == -1)
    {
        fprintf(stdout, "token is free\n");
        pthread_mutex_unlock(&ptr->token_mutex);
        return -1;
    }

    while (ptr->my_token < need_token)
    {
        pthread_cond_wait(&ptr->token_cond, &ptr->token_mutex);
    }

    ptr->my_token -= need_token;

    pthread_mutex_unlock(&ptr->token_mutex);
    return (int32_t)need_token;
}

static uint32_t minn(uint32_t m, uint32_t n)
{
    return (m < n) ? m : n;
}

/*
 * 归还令牌
 * 返回实际归还的令牌数
 */
int32_t ret_token(struct token_entyr* ptr, uint32_t ret_token_num)
{
    uint32_t can_ret;

    if (ptr == NULL)
        return -1;

    pthread_mutex_lock(&ptr->token_mutex);

    if (ptr->pos_token == -1)
    {
        fprintf(stdout, "token is free\n");
        pthread_mutex_unlock(&ptr->token_mutex);
        return -1;
    }

    can_ret = minn(ptr->brust_token - ptr->my_token, ret_token_num);
    ptr->my_token += can_ret;

    pthread_cond_broadcast(&ptr->token_cond);
    pthread_mutex_unlock(&ptr->token_mutex);

    return (int32_t)can_ret;
}
