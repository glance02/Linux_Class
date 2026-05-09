#include "termsync.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* 初始化一个编辑操作；ch 只有插入操作会用到，但统一清零能避免旧数据泄漏。 */
void ts_operation_init(TsOperation *op, TsOpKind kind, size_t pos, const char *ch)
{
    memset(op, 0, sizeof(*op));
    op->kind = kind;
    op->pos = pos;
    if (ch != NULL) {
        snprintf(op->ch, sizeof(op->ch), "%s", ch);
    }
}

/* 协议中使用小写字符串表示操作类型，便于 JSON 日志和调试输出阅读。 */
const char *ts_operation_kind_name(TsOpKind kind)
{
    switch (kind) {
    case TS_OP_INSERT:
        return "insert";
    case TS_OP_DELETE:
        return "delete";
    case TS_OP_NOOP:
        return "noop";
    default:
        return "invalid";
    }
}

/* 把协议字符串还原成枚举值；未知字符串统一视为无效操作。 */
TsOpKind ts_operation_kind_from_name(const char *name)
{
    if (name == NULL) {
        return TS_OP_INVALID;
    }
    if (strcmp(name, "insert") == 0) {
        return TS_OP_INSERT;
    }
    if (strcmp(name, "delete") == 0) {
        return TS_OP_DELETE;
    }
    if (strcmp(name, "noop") == 0) {
        return TS_OP_NOOP;
    }
    return TS_OP_INVALID;
}

/* TsOperation 不持有动态内存，可以直接按值复制。 */
TsOperation ts_operation_clone(const TsOperation *op)
{
    TsOperation clone;
    memcpy(&clone, op, sizeof(clone));
    return clone;
}

/* 生成一个不会修改文档的操作，通常用于 OT 后发现目标字符已经被其他人删除。 */
TsOperation ts_operation_noop(const TsOperation *op, const char *reason)
{
    TsOperation result;
    ts_operation_init(&result, TS_OP_NOOP, op ? op->pos : 0, NULL);
    if (reason != NULL) {
        snprintf(result.reason, sizeof(result.reason), "%s", reason);
    }
    return result;
}

/* POSIX strdup 不是 C 标准函数，这里提供一个项目内可控的版本。 */
char *ts_strdup(const char *value)
{
    size_t len = value ? strlen(value) : 0;
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(copy, value, len);
    }
    copy[len] = '\0';
    return copy;
}

/* 读取整个文件作为初始文档内容。文件不存在时返回空字符串，方便首次创建共享文件。 */
char *ts_read_file_text(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return ts_strdup("");
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ts_strdup("");
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return ts_strdup("");
    }
    rewind(fp);
    char *buffer = malloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(fp);
        return NULL;
    }
    size_t read_count = fread(buffer, 1, (size_t)size, fp);
    buffer[read_count] = '\0';
    fclose(fp);
    return buffer;
}

/* write(2) 可能被信号打断或只写入部分字节，所以这里循环直到全部写完。 */
int ts_write_all_fd(int fd, const void *data, size_t len)
{
    const char *cursor = data;
    while (len > 0) {
        ssize_t written = write(fd, cursor, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }
        cursor += written;
        len -= (size_t)written;
    }
    return 0;
}

/* UTF-8 后续字节形如 10xxxxxx；统计字符时只数每个字符的首字节。 */
static bool is_utf8_continuation(unsigned char value)
{
    return (value & 0xC0u) == 0x80u;
}

/* 返回 UTF-8 字符数量。这里假设输入已经是有效 UTF-8，符合客户端/协议的使用场景。 */
size_t ts_utf8_char_count(const char *text)
{
    size_t count = 0;
    if (text == NULL) {
        return 0;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (!is_utf8_continuation((unsigned char)text[i])) {
            count++;
        }
    }
    return count;
}

/* 根据首字节判断当前 UTF-8 字符占几个字节，遇到异常首字节时退化成 1 字节。 */
size_t ts_utf8_char_bytes(const char *text)
{
    if (text == NULL || *text == '\0') {
        return 0;
    }
    unsigned char first = (unsigned char)text[0];
    if (first < 0x80u) {
        return 1;
    }
    if ((first & 0xE0u) == 0xC0u) {
        return 2;
    }
    if ((first & 0xF0u) == 0xE0u) {
        return 3;
    }
    if ((first & 0xF8u) == 0xF0u) {
        return 4;
    }
    return 1;
}

/* 把“第几个字符”的位置转换为字节偏移，插入/删除时都通过它避免切断多字节字符。 */
size_t ts_utf8_byte_offset(const char *text, size_t char_pos)
{
    size_t chars = 0;
    size_t i = 0;
    if (text == NULL) {
        return 0;
    }
    while (text[i] != '\0') {
        if (chars == char_pos) {
            return i;
        }
        size_t bytes = ts_utf8_char_bytes(text + i);
        if (bytes == 0) {
            break;
        }
        i += bytes;
        chars++;
    }
    return i;
}

/* 在一份文本副本上应用操作，并返回新分配的字符串；调用方负责 free。 */
char *ts_apply_operation_to_text(const char *text, const TsOperation *op)
{
    const char *source = text ? text : "";
    size_t source_bytes = strlen(source);
    size_t source_chars = ts_utf8_char_count(source);
    if (op == NULL || op->kind == TS_OP_NOOP) {
        return ts_strdup(source);
    }

    if (op->kind == TS_OP_INSERT) {
        /* 插入位置超过文档末尾时夹到末尾，避免构造非法字节偏移。 */
        size_t pos = op->pos > source_chars ? source_chars : op->pos;
        size_t byte_pos = ts_utf8_byte_offset(source, pos);
        size_t ch_bytes = strlen(op->ch);
        char *result = malloc(source_bytes + ch_bytes + 1);
        if (result == NULL) {
            return NULL;
        }
        memcpy(result, source, byte_pos);
        memcpy(result + byte_pos, op->ch, ch_bytes);
        memcpy(result + byte_pos + ch_bytes, source + byte_pos, source_bytes - byte_pos + 1);
        return result;
    }

    if (op->kind == TS_OP_DELETE) {
        /* 删除越界不报错，保持原文不变；服务端会把这种情况规范化成 NOOP。 */
        if (op->pos >= source_chars) {
            return ts_strdup(source);
        }
        size_t byte_pos = ts_utf8_byte_offset(source, op->pos);
        size_t next_byte = ts_utf8_byte_offset(source, op->pos + 1);
        char *result = malloc(source_bytes - (next_byte - byte_pos) + 1);
        if (result == NULL) {
            return NULL;
        }
        memcpy(result, source, byte_pos);
        memcpy(result + byte_pos, source + next_byte, source_bytes - next_byte + 1);
        return result;
    }

    return ts_strdup(source);
}

/* 毫秒级睡眠；被信号打断时继续睡剩余时间，测试自动保存时会用到。 */
void ts_msleep(long milliseconds)
{
    if (milliseconds <= 0) {
        return;
    }
    struct timespec req;
    req.tv_sec = milliseconds / 1000;
    req.tv_nsec = (milliseconds % 1000) * 1000000L;
    while (nanosleep(&req, &req) < 0 && errno == EINTR) {
    }
}
