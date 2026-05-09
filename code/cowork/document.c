#include "termsync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_message_error(TsMessage *message, const char *op_id, const char *text)
{
    ts_message_init(message);
    snprintf(message->type, sizeof(message->type), "ERROR");
    snprintf(message->op_id, sizeof(message->op_id), "%s", op_id ? op_id : "");
    snprintf(message->message, sizeof(message->message), "%s", text);
}

void ts_process_result_init(TsProcessResult *result)
{
    memset(result, 0, sizeof(*result));
    ts_message_init(&result->messages[0]);
    ts_message_init(&result->messages[1]);
    ts_message_init(&result->ack);
    ts_message_init(&result->remote);
}

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

char *ts_document_content(TsDocument *doc)
{
    pthread_mutex_lock(&doc->lock);
    char *copy = ts_strdup(doc->text);
    pthread_mutex_unlock(&doc->lock);
    return copy;
}

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

    TsOperation transformed = ts_operation_clone(op);
    for (size_t i = 0; i < doc->history_count; ++i) {
        if (doc->history[i].server_version > base_version) {
            transformed = ts_transform_against(&transformed, &doc->history[i].op);
        }
    }
    transformed = normalize_for_apply(doc, &transformed);
    doc->version++;
    int server_version = doc->version;
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

