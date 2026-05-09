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

static void append_json_field_prefix(TsStringBuilder *sb, bool *first, const char *name)
{
    if (!*first) {
        sb_append(sb, ",");
    }
    *first = false;
    sb_appendf(sb, "\"%s\":", name);
}

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

static void append_int_field(TsStringBuilder *sb, bool *first, const char *name, int value)
{
    if (value < 0) {
        return;
    }
    append_json_field_prefix(sb, first, name);
    sb_appendf(sb, "%d", value);
}

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

void ts_message_init(TsMessage *message)
{
    memset(message, 0, sizeof(*message));
    message->base_version = -1;
    message->server_version = -1;
    message->version = -1;
    message->history_start_version = -1;
}

void ts_message_free(TsMessage *message)
{
    if (message != NULL) {
        free(message->content);
        message->content = NULL;
    }
}

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

static const char *skip_ws(const char *p)
{
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

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

void ts_transport_init(TsTransport *transport, int fd)
{
    transport->fd = fd;
    transport->buffer = malloc(TS_MAX_LINE_BYTES + 1);
    transport->buffer_len = 0;
    pthread_mutex_init(&transport->send_lock, NULL);
}

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

