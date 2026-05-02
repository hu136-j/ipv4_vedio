#ifndef CONFIG_H__
#define CONFIG_H__

#include <stdint.h>

#define CONFIG_MAX_LINE_LEN   512
#define CONFIG_MAX_KEY_LEN    64
#define CONFIG_MAX_VALUE_LEN  256

struct config_entry {
    char key[CONFIG_MAX_KEY_LEN];
    char value[CONFIG_MAX_VALUE_LEN];
    struct config_entry *next;
};

typedef struct config_entry config_t;

config_t *config_load(const char *file_path);
void config_free(config_t *config);

const char *config_get_string(config_t *config, const char *key, const char *default_value);
int config_get_int(config_t *config, const char *key, int default_value);
uint16_t config_get_uint16(config_t *config, const char *key, uint16_t default_value);

#endif
