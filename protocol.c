#include "termsync.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} TsStringBuilder;

/* 简单动态字符串，供 JSON 编码/解析时逐步追加内容。 */
static int sb_reserve(TsStringBuilder *sb, size_t extra)
{
    if (sb->len + extra + 1 <= sb->cap) {
        return 0;
    }
    size_t next = sb->cap == 0 ? 256 : sb->cap;
    while (next < sb->len + extra + 1) {
        next *= 2;
    }
    char *data = realloc(sb->data, next);
    if (data == NULL) {
        return -1;
    }
    sb->data = data;
    sb->cap = next;
    return 0;
}

/* 追加普通字符串，内部会保证末尾始终有 '\0'，方便后续作为 C 字符串使用。 */
static int sb_append(TsStringBuilder *sb, const char *text)
{
    size_t len = strlen(text);
    if (sb_reserve(sb, len) != 0) {
        return -1;
    }
    memcpy(sb->data + sb->len, text, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
    return 0;
}

/* 追加格式化文本。先用 vsnprintf 计算长度，再一次性扩容避免截断。 */
static int sb_appendf(TsStringBuilder *sb, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return -1;
    }
    if (sb_reserve(sb, (size_t)needed) != 0) {
        va_end(args);
        return -1;
    }
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, args);
    sb->len += (size_t)needed;
    va_end(args);
    return 0;
}

/* JSON 字符串转义：处理引号、反斜杠、换行等协议中最容易破坏一行消息的字符。 */
static char *json_escape(const char *value)
{
    TsStringBuilder sb = {0};
    const unsigned char *cursor = (const unsigned char *)(value ? value : "");
    while (*cursor != '\0') {
        unsigned char ch = *cursor++;
        switch (ch) {
        case '\\':
            sb_append(&sb, "\\\\");
            break;
        case '"':
            sb_append(&sb, "\\\"");
            break;
        case '\n':
            sb_append(&sb, "\\n");
            break;
        case '\r':
            sb_append(&sb, "\\r");
            break;
        case '\t':
            sb_append(&sb, "\\t");
            break;
        default:
            if (ch < 0x20u) {
                sb_appendf(&sb, "\\u%04x", ch);
            } else {
                char one[2] = {(char)ch, '\0'};
                sb_append(&sb, one);
            }
            break;
        }
    }
    if (sb.data == NULL) {
        return ts_strdup("");
    }
    return sb.data;
}

/* 写字段名前处理逗号。first 由调用方维护，避免编码时产生尾逗号。 */
static void append_json_field_prefix(TsStringBuilder *sb, bool *first, const char *name)
{
    if (!*first) {
        sb_append(sb, ",");
    }
    *first = false;
    sb_appendf(sb, "\"%s\":", name);
}

/* 空字符串字段不会编码进消息，减少网络包体积，也让默认值保持简单。 */
static void append_string_field(TsStringBuilder *sb, bool *first, const char *name, const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return;
    }
    char *escaped = json_escape(value);
    append_json_field_prefix(sb, first, name);
    sb_appendf(sb, "\"%s\"", escaped ? escaped : "");
    free(escaped);
}

/* 版本号字段用 -1 表示未设置，因此只编码非负值。 */
static void append_int_field(TsStringBuilder *sb, bool *first, const char *name, int value)
{
    if (value < 0) {
        return;
    }
    append_json_field_prefix(sb, first, name);
    sb_appendf(sb, "%d", value);
}

/* 把编辑操作编码成嵌套对象。insert 才需要 char，NOOP 可额外携带 reason。 */
static void append_operation_field(TsStringBuilder *sb, bool *first, const char *name, const TsOperation *op)
{
    char *escaped_char = json_escape(op->ch);
    char *escaped_reason = json_escape(op->reason);
    append_json_field_prefix(sb, first, name);
    sb_appendf(sb, "{\"kind\":\"%s\",\"pos\":%zu", ts_operation_kind_name(op->kind), op->pos);
    if (op->kind == TS_OP_INSERT) {
        sb_appendf(sb, ",\"char\":\"%s\"", escaped_char ? escaped_char : "");
    }
    if (op->reason[0] != '\0') {
        sb_appendf(sb, ",\"reason\":\"%s\"", escaped_reason ? escaped_reason : "");
    }
    sb_append(sb, "}");
    free(escaped_char);
    free(escaped_reason);
}

/* 消息初始化时把版本字段设为 -1，后续编码即可区分“未设置”和版本 0。 */
void ts_message_init(TsMessage *message)
{
    memset(message, 0, sizeof(*message));
    message->base_version = -1;
    message->server_version = -1;
    message->version = -1;
    message->history_start_version = -1;
}

/* 当前只有 content 是动态分配字段，释放后置空便于重复调用。 */
void ts_message_free(TsMessage *message)
{
    if (message != NULL) {
        free(message->content);
        message->content = NULL;
    }
}

/* 编码为一行 JSON，并以 '\n' 结尾，和 ts_transport_recv 的按行解析配套。 */
char *ts_encode_message(const TsMessage *message)
{
    TsStringBuilder sb = {0};
    bool first = true;
    sb_append(&sb, "{");
    append_string_field(&sb, &first, "type", message->type);
    append_string_field(&sb, &first, "client_id", message->client_id);
    append_string_field(&sb, &first, "op_id", message->op_id);
    append_int_field(&sb, &first, "base_version", message->base_version);
    append_int_field(&sb, &first, "server_version", message->server_version);
    append_int_field(&sb, &first, "version", message->version);
    append_int_field(&sb, &first, "history_start_version", message->history_start_version);
    if (message->has_op) {
        append_operation_field(&sb, &first, "op", &message->op);
    }
    if (message->has_original_op) {
        append_operation_field(&sb, &first, "original_op", &message->original_op);
    }
    append_string_field(&sb, &first, "message", message->message);
    append_string_field(&sb, &first, "reason", message->reason);
    if (message->content != NULL) {
        char *escaped = json_escape(message->content);
        append_json_field_prefix(&sb, &first, "content");
        sb_appendf(&sb, "\"%s\"", escaped ? escaped : "");
        free(escaped);
    }
    sb_append(&sb, "}\n");
    return sb.data;
}

/* 解析辅助：跳过空白字符，允许收到的 JSON 行前后带少量空格。 */
static const char *skip_ws(const char *p)
{
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

/* 在一个扁平 JSON 对象中查找字段值起点。
 * 这是项目自用的轻量解析器，不是通用 JSON 库；输入来自本项目编码器或受控客户端。
 */
static const char *find_key(const char *json, const char *key)
{
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *after = skip_ws(p + strlen(pattern));
        if (*after == ':') {
            return skip_ws(after + 1);
        }
        p += strlen(pattern);
    }
    return NULL;
}

/* 解析 \u00xx 时把单个十六进制字符转成数值。 */
static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

/* 解析 JSON 字符串并分配新内存。
 * 当前只完整还原 ASCII 范围的 \u 转义；普通 UTF-8 字节会原样保留。
 */
static char *parse_json_string_alloc(const char *start, const char **end_out)
{
    if (*start != '"') {
        return NULL;
    }
    start++;
    TsStringBuilder sb = {0};
    while (*start != '\0') {
        char ch = *start++;
        if (ch == '"') {
            if (end_out != NULL) {
                *end_out = start;
            }
            if (sb.data == NULL) {
                return ts_strdup("");
            }
            return sb.data;
        }
        if (ch == '\\') {
            char esc = *start++;
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                sb_appendf(&sb, "%c", esc);
                break;
            case 'n':
                sb_append(&sb, "\n");
                break;
            case 'r':
                sb_append(&sb, "\r");
                break;
            case 't':
                sb_append(&sb, "\t");
                break;
            case 'u': {
                int h1 = hex_value(start[0]);
                int h2 = hex_value(start[1]);
                int h3 = hex_value(start[2]);
                int h4 = hex_value(start[3]);
                if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) {
                    free(sb.data);
                    return NULL;
                }
                int code = (h1 << 12) | (h2 << 8) | (h3 << 4) | h4;
                start += 4;
                if (code > 0 && code < 0x80) {
                    sb_appendf(&sb, "%c", code);
                } else {
                    sb_append(&sb, "?");
                }
                break;
            }
            default:
                free(sb.data);
                return NULL;
            }
        } else {
            char one[2] = {ch, '\0'};
            sb_append(&sb, one);
        }
    }
    free(sb.data);
    return NULL;
}

/* 读取字符串字段到固定缓冲区，超长内容会被 snprintf 安全截断。 */
static bool json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    const char *value = find_key(json, key);
    if (value == NULL || *value != '"') {
        return false;
    }
    char *parsed = parse_json_string_alloc(value, NULL);
    if (parsed == NULL) {
        return false;
    }
    snprintf(out, out_size, "%s", parsed);
    free(parsed);
    return true;
}

/* content 可能是整份文档，必须动态分配，不能塞进固定大小字段。 */
static bool json_get_content(const char *json, const char *key, char **out)
{
    const char *value = find_key(json, key);
    if (value == NULL || *value != '"') {
        return false;
    }
    char *parsed = parse_json_string_alloc(value, NULL);
    if (parsed == NULL) {
        return false;
    }
    *out = parsed;
    return true;
}

/* 读取整数版本号。调用方会保留初始化时的 -1，表示字段不存在。 */
static bool json_get_int(const char *json, const char *key, int *out)
{
    const char *value = find_key(json, key);
    char *end = NULL;
    long parsed;
    if (value == NULL) {
        return false;
    }
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value) {
        return false;
    }
    *out = (int)parsed;
    return true;
}

/* 截取嵌套对象字段，例如 op/original_op。
 * 扫描时会跟踪字符串状态，避免把字符串里的花括号误认为对象边界。
 */
static char *json_get_object(const char *json, const char *key)
{
    const char *value = find_key(json, key);
    if (value == NULL || *value != '{') {
        return NULL;
    }
    const char *start = value;
    bool in_string = false;
    bool escape = false;
    int depth = 0;
    for (; *value != '\0'; ++value) {
        char ch = *value;
        if (in_string) {
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            depth++;
        } else if (ch == '}') {
            depth--;
            if (depth == 0) {
                size_t len = (size_t)(value - start + 1);
                char *copy = malloc(len + 1);
                if (copy == NULL) {
                    return NULL;
                }
                memcpy(copy, start, len);
                copy[len] = '\0';
                return copy;
            }
        }
    }
    return NULL;
}

/* 将 op 对象还原为 TsOperation，并在这里完成必填字段和 UTF-8 单字符校验。 */
static int parse_operation_object(const char *json, TsOperation *op, char *error, size_t error_size)
{
    char kind[TS_MAX_TYPE] = "";
    char ch[TS_MAX_UTF8_CHAR] = "";
    int pos = 0;
    memset(op, 0, sizeof(*op));
    if (!json_get_string(json, "kind", kind, sizeof(kind))) {
        snprintf(error, error_size, "operation kind is required");
        return -1;
    }
    op->kind = ts_operation_kind_from_name(kind);
    if (op->kind == TS_OP_INVALID) {
        snprintf(error, error_size, "unsupported operation kind: %s", kind);
        return -1;
    }
    if (json_get_int(json, "pos", &pos) && pos > 0) {
        op->pos = (size_t)pos;
    }
    if (op->kind == TS_OP_INSERT) {
        if (!json_get_string(json, "char", ch, sizeof(ch)) || ts_utf8_char_count(ch) != 1) {
            snprintf(error, error_size, "insert operation requires one UTF-8 character");
            return -1;
        }
        snprintf(op->ch, sizeof(op->ch), "%s", ch);
    }
    json_get_string(json, "reason", op->reason, sizeof(op->reason));
    return 0;
}

/* 解码一行协议消息。出错时 error 会带上便于发送 ERROR 或测试断言的信息。 */
int ts_decode_message(const char *line, TsMessage *message, char *error, size_t error_size)
{
    ts_message_init(message);
    const char *json = skip_ws(line);
    if (*json != '{') {
        snprintf(error, error_size, "protocol message must be a JSON object");
        return -1;
    }
    if (!json_get_string(json, "type", message->type, sizeof(message->type))) {
        snprintf(error, error_size, "message type is required");
        return -1;
    }
    json_get_string(json, "client_id", message->client_id, sizeof(message->client_id));
    json_get_string(json, "op_id", message->op_id, sizeof(message->op_id));
    json_get_string(json, "message", message->message, sizeof(message->message));
    json_get_string(json, "reason", message->reason, sizeof(message->reason));
    json_get_int(json, "base_version", &message->base_version);
    json_get_int(json, "server_version", &message->server_version);
    json_get_int(json, "version", &message->version);
    json_get_int(json, "history_start_version", &message->history_start_version);
    json_get_content(json, "content", &message->content);

    char *op_json = json_get_object(json, "op");
    if (op_json != NULL) {
        if (parse_operation_object(op_json, &message->op, error, error_size) != 0) {
            free(op_json);
            ts_message_free(message);
            return -1;
        }
        message->has_op = true;
        free(op_json);
    }
    char *original_json = json_get_object(json, "original_op");
    if (original_json != NULL) {
        if (parse_operation_object(original_json, &message->original_op, error, error_size) == 0) {
            message->has_original_op = true;
        }
        free(original_json);
    }
    return 0;
}

/* 初始化传输对象；接收缓冲区按最大行长度一次分配，避免频繁 realloc。 */
void ts_transport_init(TsTransport *transport, int fd)
{
    transport->fd = fd;
    transport->buffer = malloc(TS_MAX_LINE_BYTES + 1);
    transport->buffer_len = 0;
    pthread_mutex_init(&transport->send_lock, NULL);
}

/* 销毁传输层资源。fd 的关闭由调用方负责，以便明确控制连接生命周期。 */
void ts_transport_destroy(TsTransport *transport)
{
    if (transport == NULL) {
        return;
    }
    free(transport->buffer);
    transport->buffer = NULL;
    transport->buffer_len = 0;
    pthread_mutex_destroy(&transport->send_lock);
}

/* 发送时加锁，保证同一个 socket 被多个线程写入时仍然是一条完整 JSON 行。 */
int ts_transport_send(TsTransport *transport, const TsMessage *message)
{
    char *encoded = ts_encode_message(message);
    if (encoded == NULL) {
        return -1;
    }
    pthread_mutex_lock(&transport->send_lock);
    int rc = ts_write_all_fd(transport->fd, encoded, strlen(encoded));
    pthread_mutex_unlock(&transport->send_lock);
    free(encoded);
    return rc;
}

/* 从 socket 中读取一整行 JSON。
 * 如果一次 read 读到多条消息，剩余字节会留在 buffer 中供下次调用继续解析。
 */
int ts_transport_recv(TsTransport *transport, TsMessage *message, char *error, size_t error_size)
{
    while (true) {
        for (size_t i = 0; i < transport->buffer_len; ++i) {
            if (transport->buffer[i] == '\n') {
                transport->buffer[i] = '\0';
                int rc = ts_decode_message(transport->buffer, message, error, error_size);
                size_t remaining = transport->buffer_len - i - 1;
                memmove(transport->buffer, transport->buffer + i + 1, remaining);
                transport->buffer_len = remaining;
                return rc;
            }
        }

        if (transport->buffer_len >= TS_MAX_LINE_BYTES) {
            snprintf(error, error_size, "protocol message exceeds maximum line length");
            return -1;
        }

        ssize_t n = read(
            transport->fd,
            transport->buffer + transport->buffer_len,
            TS_MAX_LINE_BYTES - transport->buffer_len
        );
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            snprintf(error, error_size, "socket read failed: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            snprintf(error, error_size, "connection closed");
            return -1;
        }
        transport->buffer_len += (size_t)n;
    }
}

