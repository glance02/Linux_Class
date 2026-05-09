#include "termsync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

<<<<<<< HEAD
=======
/* 构造发给客户端的错误消息，op_id 用于让客户端知道哪次操作失败。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
static void set_message_error(TsMessage *message, const char *op_id, const char *text)
{
    ts_message_init(message);
    snprintf(message->type, sizeof(message->type), "ERROR");
    snprintf(message->op_id, sizeof(message->op_id), "%s", op_id ? op_id : "");
    snprintf(message->message, sizeof(message->message), "%s", text);
}

<<<<<<< HEAD
=======
/* 初始化处理结果，内部消息也先置为默认值，方便后续只填需要的字段。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
void ts_process_result_init(TsProcessResult *result)
{
    memset(result, 0, sizeof(*result));
    ts_message_init(&result->messages[0]);
    ts_message_init(&result->messages[1]);
    ts_message_init(&result->ack);
    ts_message_init(&result->remote);
}

<<<<<<< HEAD
=======
/* 释放处理结果中可能持有的动态内存，尤其是 DOC_STATE 快照和 snapshot_content。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
void ts_process_result_free(TsProcessResult *result)
{
    if (result == NULL) {
        return;
    }
    for (size_t i = 0; i < result->message_count && i < 2; ++i) {
        ts_message_free(&result->messages[i]);
    }
    ts_message_free(&result->ack);
    ts_message_free(&result->remote);
    free(result->snapshot_content);
    result->snapshot_content = NULL;
}

<<<<<<< HEAD
=======
/* 创建服务端权威文档。history_limit 为 0 时使用默认上限，防止历史无限增长。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
void ts_document_init(TsDocument *doc, const char *initial_text, size_t history_limit)
{
    memset(doc, 0, sizeof(*doc));
    doc->text = ts_strdup(initial_text ? initial_text : "");
    doc->version = 0;
    doc->history_start_version = 0;
    doc->history_limit = history_limit == 0 ? 1000 : history_limit;
    doc->history = calloc(doc->history_limit, sizeof(TsHistoryEntry));
    pthread_mutex_init(&doc->lock, NULL);
}

<<<<<<< HEAD
=======
/* 销毁文档及其历史，并清空结构体，避免后续误用旧指针。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
void ts_document_destroy(TsDocument *doc)
{
    if (doc == NULL) {
        return;
    }
    free(doc->text);
    free(doc->history);
    pthread_mutex_destroy(&doc->lock);
    memset(doc, 0, sizeof(*doc));
}

<<<<<<< HEAD
=======
/* 返回当前文档内容副本。调用方拿到的是独立内存，不需要继续持有文档锁。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
char *ts_document_content(TsDocument *doc)
{
    pthread_mutex_lock(&doc->lock);
    char *copy = ts_strdup(doc->text);
    pthread_mutex_unlock(&doc->lock);
    return copy;
}

<<<<<<< HEAD
=======
/* 生成 DOC_STATE 快照，用于新客户端加入或客户端需要重同步。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
void ts_document_snapshot(TsDocument *doc, TsMessage *snapshot)
{
    ts_message_init(snapshot);
    pthread_mutex_lock(&doc->lock);
    snprintf(snapshot->type, sizeof(snapshot->type), "DOC_STATE");
    snapshot->version = doc->version;
    snapshot->history_start_version = doc->history_start_version;
    snapshot->content = ts_strdup(doc->text);
    pthread_mutex_unlock(&doc->lock);
}

<<<<<<< HEAD
=======
/* 将一个操作转换到“已经应用 previous 之后”的坐标系。
 * 规则说明：
 * - 前面插入字符会把后续同位置/更后位置的操作右移；
 * - 前面删除字符会把后续位置左移；
 * - 两个删除命中同一个字符时，后来的删除变成 NOOP。
 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
TsOperation ts_transform_against(const TsOperation *op, const TsOperation *previous)
{
    TsOperation result = ts_operation_clone(op);
    if (result.kind == TS_OP_NOOP || previous->kind == TS_OP_NOOP) {
        return result;
    }
    if (previous->kind == TS_OP_INSERT) {
        if ((result.kind == TS_OP_INSERT || result.kind == TS_OP_DELETE) && previous->pos <= result.pos) {
            result.pos++;
        }
        return result;
    }
    if (previous->kind == TS_OP_DELETE) {
        if (result.kind == TS_OP_INSERT && previous->pos < result.pos) {
            result.pos--;
        } else if (result.kind == TS_OP_DELETE) {
            if (previous->pos < result.pos) {
                result.pos--;
            } else if (previous->pos == result.pos) {
                return ts_operation_noop(&result, "same character already deleted");
            }
        }
    }
    return result;
}

<<<<<<< HEAD
=======
/* 在真正应用前做最后规范化：
 * 插入越界夹到末尾，删除越界转成 NOOP，无效操作也转成 NOOP。
 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
static TsOperation normalize_for_apply(TsDocument *doc, const TsOperation *op)
{
    size_t text_len = ts_utf8_char_count(doc->text);
    if (op->kind == TS_OP_NOOP) {
        return ts_operation_clone(op);
    }
    if (op->kind == TS_OP_INSERT) {
        TsOperation result = ts_operation_clone(op);
        if (result.pos > text_len) {
            result.pos = text_len;
        }
        return result;
    }
    if (op->kind == TS_OP_DELETE) {
        if (op->pos < text_len) {
            return ts_operation_clone(op);
        }
        return ts_operation_noop(op, "delete position out of range");
    }
    return ts_operation_noop(op, "invalid operation");
}

<<<<<<< HEAD
=======
/* 追加服务端历史。超过上限时丢弃最旧条目，并更新可支持的最早 base_version。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
static void append_history(TsDocument *doc, const TsHistoryEntry *entry)
{
    if (doc->history_count == doc->history_limit) {
        memmove(doc->history, doc->history + 1, sizeof(TsHistoryEntry) * (doc->history_limit - 1));
        doc->history_count--;
    }
    doc->history[doc->history_count++] = *entry;
    if (doc->history_count == 0) {
        doc->history_start_version = doc->version;
    } else {
        doc->history_start_version = doc->history[0].server_version - 1;
        if (doc->history_start_version < 0) {
            doc->history_start_version = 0;
        }
    }
}

<<<<<<< HEAD
=======
/* 生成一条 JSON Lines 日志，记录原始操作、转换后操作和文档长度，供服务端终端和日志文件使用。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
static void build_operation_log(
    char *out,
    size_t out_size,
    int server_version,
    const char *client_id,
    const char *op_id,
    int base_version,
    const TsOperation *original,
    const TsOperation *transformed,
    size_t content_length
)
{
    snprintf(
        out,
        out_size,
        "{\"event\":\"operation\",\"server_version\":%d,\"client_id\":\"%s\","
        "\"op_id\":\"%s\",\"base_version\":%d,"
        "\"original_op\":{\"kind\":\"%s\",\"pos\":%zu%s%s%s},"
        "\"transformed_op\":{\"kind\":\"%s\",\"pos\":%zu%s%s%s},"
        "\"content_length\":%zu}",
        server_version,
        client_id,
        op_id,
        base_version,
        ts_operation_kind_name(original->kind),
        original->pos,
        original->kind == TS_OP_INSERT ? ",\"char\":\"" : "",
        original->kind == TS_OP_INSERT ? original->ch : "",
        original->kind == TS_OP_INSERT ? "\"" : "",
        ts_operation_kind_name(transformed->kind),
        transformed->pos,
        transformed->kind == TS_OP_INSERT ? ",\"char\":\"" : "",
        transformed->kind == TS_OP_INSERT ? transformed->ch : "",
        transformed->kind == TS_OP_INSERT ? "\"" : "",
        content_length
    );
}

<<<<<<< HEAD
=======
/* 处理一次客户端提交的编辑操作。
 * 这是服务端文档的临界区：进入锁后确定全局顺序，随后基于历史做 OT 转换，
 * 最后一次性产出 ACK、REMOTE_OP、保存快照和日志事件。
 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
TsProcessResult ts_document_process_operation(
    TsDocument *doc,
    const char *client_id,
    const char *op_id,
    int base_version,
    const TsOperation *op
)
{
    TsProcessResult result;
    ts_process_result_init(&result);

<<<<<<< HEAD
=======
    /* 协议层已经做过基本解析，这里再守住文档层的操作合法性。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
    if (op == NULL || op->kind == TS_OP_INVALID) {
        result.status = TS_PROCESS_ERROR;
        result.message_count = 1;
        set_message_error(&result.messages[0], op_id, "invalid operation");
        return result;
    }
    if (op->kind == TS_OP_INSERT && ts_utf8_char_count(op->ch) != 1) {
        result.status = TS_PROCESS_ERROR;
        result.message_count = 1;
        set_message_error(&result.messages[0], op_id, "insert operation requires one UTF-8 character");
        return result;
    }

    pthread_mutex_lock(&doc->lock);
<<<<<<< HEAD
=======
    /* 客户端版本太旧时历史已经被裁剪，太新则说明客户端状态不可信，二者都要求重同步。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
    if (base_version < doc->history_start_version || base_version > doc->version) {
        result.status = TS_PROCESS_RESYNC;
        result.message_count = 2;
        ts_message_init(&result.messages[0]);
        snprintf(result.messages[0].type, sizeof(result.messages[0].type), "RESYNC_REQUIRED");
        snprintf(result.messages[0].op_id, sizeof(result.messages[0].op_id), "%s", op_id);
        snprintf(
            result.messages[0].reason,
            sizeof(result.messages[0].reason),
            "%s",
            base_version < doc->history_start_version ? "operation history expired" : "client version is ahead of server"
        );
        result.messages[0].server_version = doc->version;
        result.messages[0].history_start_version = doc->history_start_version;

        ts_message_init(&result.messages[1]);
        snprintf(result.messages[1].type, sizeof(result.messages[1].type), "DOC_STATE");
        result.messages[1].version = doc->version;
        result.messages[1].history_start_version = doc->history_start_version;
        result.messages[1].content = ts_strdup(doc->text);
        pthread_mutex_unlock(&doc->lock);
        return result;
    }

<<<<<<< HEAD
=======
    /* 从客户端的 base_version 开始，把之后每一条服务端历史操作折叠到当前操作上。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
    TsOperation transformed = ts_operation_clone(op);
    for (size_t i = 0; i < doc->history_count; ++i) {
        if (doc->history[i].server_version > base_version) {
            transformed = ts_transform_against(&transformed, &doc->history[i].op);
        }
    }
    transformed = normalize_for_apply(doc, &transformed);
    doc->version++;
    int server_version = doc->version;
<<<<<<< HEAD
=======
    /* 即使转换结果是 NOOP，也会推进版本号，保证所有客户端看到一致的全局操作序列。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
    char *updated = ts_apply_operation_to_text(doc->text, &transformed);
    if (updated != NULL) {
        free(doc->text);
        doc->text = updated;
    }

    TsHistoryEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.server_version = server_version;
    entry.base_version = base_version;
    snprintf(entry.client_id, sizeof(entry.client_id), "%s", client_id);
    snprintf(entry.op_id, sizeof(entry.op_id), "%s", op_id);
    entry.original_op = ts_operation_clone(op);
    entry.op = ts_operation_clone(&transformed);
    append_history(doc, &entry);

    result.status = TS_PROCESS_OK;
    result.has_ack = true;
<<<<<<< HEAD
=======
    /* ACK 发回提交者：包含原始操作和转换后操作，便于客户端确认 inflight 已被服务端接收。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
    snprintf(result.ack.type, sizeof(result.ack.type), "ACK");
    snprintf(result.ack.client_id, sizeof(result.ack.client_id), "%s", client_id);
    snprintf(result.ack.op_id, sizeof(result.ack.op_id), "%s", op_id);
    result.ack.base_version = base_version;
    result.ack.server_version = server_version;
    result.ack.version = doc->version;
    result.ack.op = transformed;
    result.ack.has_op = true;
    result.ack.original_op = *op;
    result.ack.has_original_op = true;

    result.has_remote = true;
<<<<<<< HEAD
=======
    /* REMOTE_OP 广播给其他客户端，只需要携带服务端最终应用的操作。 */
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
    snprintf(result.remote.type, sizeof(result.remote.type), "REMOTE_OP");
    snprintf(result.remote.client_id, sizeof(result.remote.client_id), "%s", client_id);
    snprintf(result.remote.op_id, sizeof(result.remote.op_id), "%s", op_id);
    result.remote.server_version = server_version;
    result.remote.version = doc->version;
    result.remote.op = transformed;
    result.remote.has_op = true;

    result.snapshot_content = ts_strdup(doc->text);
    result.snapshot_version = doc->version;
    build_operation_log(
        result.log_json,
        sizeof(result.log_json),
        server_version,
        client_id,
        op_id,
        base_version,
        op,
        &transformed,
        ts_utf8_char_count(doc->text)
    );
    pthread_mutex_unlock(&doc->lock);
    return result;
}

