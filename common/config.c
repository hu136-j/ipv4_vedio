#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void trim_whitespace(char *str)
{
    char *start = str;
    char *end;

    while (*start != '\0' && isspace((unsigned char)*start))
        start++;

    if (start != str)
        memmove(str, start, strlen(start) + 1);

    end = str + strlen(str);
    while (end > str && isspace((unsigned char)*(end - 1)))
        end--;
    *end = '\0';
}

static int parse_line(const char *line, char *key, char *value)
{
    const char *eq;
    size_t key_len;
    size_t value_len;

    eq = strchr(line, '=');
    if (eq == NULL)
        return -1;

    key_len = (size_t)(eq - line);
    if (key_len == 0 || key_len >= CONFIG_MAX_KEY_LEN)
        return -1;

    strncpy(key, line, key_len);
    key[key_len] = '\0';
    trim_whitespace(key);

    if (key[0] == '\0')
        return -1;

    value_len = strlen(eq + 1);
    if (value_len >= CONFIG_MAX_VALUE_LEN)
        return -1;

    strcpy(value, eq + 1);
    trim_whitespace(value);

    return 0;
}

config_t *config_load(const char *file_path)
{
    FILE *fp;
    char line[CONFIG_MAX_LINE_LEN];
    char key[CONFIG_MAX_KEY_LEN];
    char value[CONFIG_MAX_VALUE_LEN];
    config_t *head = NULL;
    config_t *tail = NULL;

    fp = fopen(file_path, "r");
    if (fp == NULL)
        return NULL;

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        config_t *entry;

        trim_whitespace(line);

        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
            continue;

        if (parse_line(line, key, value) < 0)
            continue;

        entry = malloc(sizeof(config_t));
        if (entry == NULL)
        {
            fclose(fp);
            config_free(head);
            return NULL;
        }

        strncpy(entry->key, key, sizeof(entry->key) - 1);
        entry->key[sizeof(entry->key) - 1] = '\0';

        strncpy(entry->value, value, sizeof(entry->value) - 1);
        entry->value[sizeof(entry->value) - 1] = '\0';

        entry->next = NULL;

        if (head == NULL)
        {
            head = entry;
            tail = entry;
        }
        else
        {
            tail->next = entry;
            tail = entry;
        }
    }

    fclose(fp);
    return head;
}

void config_free(config_t *config)
{
    config_t *current = config;
    config_t *next;

    while (current != NULL)
    {
        next = current->next;
        free(current);
        current = next;
    }
}

const char *config_get_string(config_t *config, const char *key, const char *default_value)
{
    config_t *current = config;

    while (current != NULL)
    {
        if (strcmp(current->key, key) == 0)
            return current->value;
        current = current->next;
    }

    return default_value;
}

int config_get_int(config_t *config, const char *key, int default_value)
{
    const char *value = config_get_string(config, key, NULL);
    if (value == NULL)
        return default_value;

    return atoi(value);
}

uint16_t config_get_uint16(config_t *config, const char *key, uint16_t default_value)
{
    int val = config_get_int(config, key, (int)default_value);
    if (val < 0 || val > 65535)
        return default_value;
    return (uint16_t)val;
}
