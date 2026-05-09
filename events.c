#include "termsync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 事件格式化只面对本项目自己生成的 JSON 日志，因此用轻量字符串查找即可。
 * 这里不是通用 JSON 解析器，目标是把终端输出保持简单、可读。
 */
static void json_get_for_format(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[96];
    const char *source = json ? json : "";
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(source, pattern);
    if (p == NULL) {
        out[0] = '\0';
        return;
    }
    p += strlen(pattern);
    size_t len = 0;
    while (*p != '\0' && *p != '"' && len + 1 < out_size) {
        out[len++] = *p++;
    }
    out[len] = '\0';
}

/* 从日志 JSON 中读取整数字段，缺失时使用 fallback。 */
static int json_int_for_format(const char *json, const char *key, int fallback)
{
    char pattern[96];
    const char *source = json ? json : "";
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(source, pattern);
    if (p == NULL) {
        return fallback;
    }
    return atoi(p + strlen(pattern));
}

/* 将 operation 日志转成单行终端事件，优先展示转换后的最终操作。 */
void ts_format_operation_event(const char *entry_json, char *out, size_t out_size)
{
    char client_id[TS_MAX_CLIENT_ID];
    char kind[TS_MAX_TYPE];
    char ch[TS_MAX_UTF8_CHAR];
    json_get_for_format(entry_json, "client_id", client_id, sizeof(client_id));
    const char *transformed = strstr(entry_json ? entry_json : "", "\"transformed_op\"");
    json_get_for_format(transformed ? transformed : entry_json, "kind", kind, sizeof(kind));
    json_get_for_format(transformed ? transformed : entry_json, "char", ch, sizeof(ch));
    int version = json_int_for_format(entry_json, "server_version", 0);
    int base = json_int_for_format(entry_json, "base_version", 0);
    int pos = json_int_for_format(transformed ? transformed : entry_json, "pos", 0);
    if (strcmp(kind, "insert") == 0) {
        snprintf(out, out_size, "OP v%d client=%s insert pos=%d char='%s' base=%d", version, client_id, pos, ch, base);
    } else {
        snprintf(out, out_size, "OP v%d client=%s %s pos=%d base=%d", version, client_id, kind[0] ? kind : "unknown", pos, base);
    }
}

/* 格式化保存事件。client_id 为空时表示服务端自身触发，例如关闭前保存。 */
void ts_format_save_event(const char *reason, const char *client_id, int version, const char *path, char *out, size_t out_size)
{
    if (client_id != NULL && client_id[0] != '\0') {
        snprintf(out, out_size, "SAVE %s client=%s version=%d file=%s", reason, client_id, version, path);
    } else {
        snprintf(out, out_size, "SAVE %s version=%d file=%s", reason, version, path);
    }
}

/* 服务端统一事件入口：按 event 字段分派到具体格式化逻辑。 */
void ts_format_server_event(const char *entry_json, char *out, size_t out_size)
{
    char event[32];
    char client_id[TS_MAX_CLIENT_ID];
    char address[128];
    json_get_for_format(entry_json, "event", event, sizeof(event));
    if (strcmp(event, "operation") == 0) {
        ts_format_operation_event(entry_json, out, out_size);
        return;
    }
    if (strcmp(event, "join") == 0) {
        json_get_for_format(entry_json, "client_id", client_id, sizeof(client_id));
        json_get_for_format(entry_json, "address", address, sizeof(address));
        snprintf(out, out_size, "JOIN client=%s address=%s", client_id, address);
        return;
    }
    if (strcmp(event, "leave") == 0) {
        json_get_for_format(entry_json, "client_id", client_id, sizeof(client_id));
        snprintf(out, out_size, "LEAVE client=%s", client_id);
        return;
    }
    out[0] = '\0';
}

