#ifndef TOKEN_H__
#define TOKEN_H__

#include <stdint.h>

struct token_entyr;

/* 创建一个令牌桶
 * brust_token : 桶容量上限
 * speed_token : 每秒补充的令牌数
 * 返回值:
 *   成功: 令牌桶指针
 *   失败: NULL
 */
struct token_entyr* token_init(uint32_t brust_token, uint32_t speed_token);

/* 销毁单个令牌桶 */
void token_alldestry(struct token_entyr* ptr);

/* 获取令牌
 * need_token : 需要的令牌数
 * 返回值:
 *   成功: 实际获取到的令牌数
 *   失败: -1
 */
int32_t get_token(struct token_entyr* ptr, uint32_t need_token);

/* 归还令牌
 * ret_token_num : 归还的令牌数
 * 返回值:
 *   成功: 实际归还的令牌数
 *   失败: -1
 */
int32_t ret_token(struct token_entyr* ptr, uint32_t ret_token_num);

#endif
