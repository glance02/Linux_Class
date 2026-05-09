#include "termsync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void state_pump(TsClientState *state);

void ts_client_state_init(
    TsClientState *state,
    const char *client_id,
    TsSendFn send_fn,
    void *send_userdata
)
{
    memset(state, 0, sizeof(*state));
    snprintf(state->client_id, sizeof(state->client_id), "%s", client_id ? client_id : "u1");
    state->content = ts_strdup("");
    state->version = 0;
    state->history_start_version = 0;
    state->next_op_number = 1;
    snprintf(state->status, sizeof(state->status), "disconnected");
    state->send_fn = send_fn;
    state->send_userdata = send_userdata;
}

void ts_client_state_destroy(TsClientState *state)
{
    if (state == NULL) {
        return;
    }
    free(state->content);
    free(state->queue);
    memset(state, 0, sizeof(*state));
}

static int ensure_queue_capacity(TsClientState *state)
{
    if (state->queue_len < state->queue_cap) {
        return 0;
    }
    size_t next = state->queue_cap == 0 ? 8 : state->queue_cap * 2;
    TsPendingOperation *queue = realloc(state->queue, sizeof(*queue) * next);
    if (queue == NULL) {
        return -1;
    }
    state->queue = queue;
    state->queue_cap = next;
    return 0;
}

void ts_client_state_set_document(TsClientState *state, const char *content, int version, int history_start_version)
{
    free(state->content);
    state->content = ts_strdup(content ? content : "");
    state->version = version;
    state->history_start_version = history_start_version;
    state->has_inflight = false;
    snprintf(state->status, sizeof(state->status), "synced at version %d", version);
    state_pump(state);
}

int ts_client_state_queue_operation(TsClientState *state, const TsOperation *op, char *op_id_out, size_t out_size)
{
    if (ensure_queue_capacity(state) != 0) {
        return -1;
    }
    TsPendingOperation pending;
    memset(&pending, 0, sizeof(pending));
    snprintf(pending.op_id, sizeof(pending.op_id), "%.48s-%d", state->client_id, state->next_op_number++);
    pending.op = ts_operation_clone(op);
    pending.base_version = -1;
    state->queue[state->queue_len++] = pending;
    if (op_id_out != NULL && out_size > 0) {
        snprintf(op_id_out, out_size, "%s", pending.op_id);
    }
    snprintf(state->status, sizeof(state->status), "queued %zu operation(s)", state->queue_len);
    state_pump(state);
    return 0;
}

static bool pop_queue(TsClientState *state, TsPendingOperation *out)
{
    if (state->queue_len == 0) {
        return false;
    }
    *out = state->queue[0];
    memmove(state->queue, state->queue + 1, sizeof(*state->queue) * (state->queue_len - 1));
    state->queue_len--;
    return true;
}

static void apply_authoritative_op(TsClientState *state, const TsMessage *message)
{
    if (!message->has_op) {
        return;
    }
    char *updated = ts_apply_operation_to_text(state->content, &message->op);
    if (updated != NULL) {
        free(state->content);
        state->content = updated;
    }
    if (message->version >= 0) {
        state->version = message->version;
    }
}

static void handle_ack(TsClientState *state, const TsMessage *message)
{
    if (!state->has_inflight || strcmp(state->inflight.op_id, message->op_id) != 0) {
        snprintf(state->status, sizeof(state->status), "ignored stale ACK %s", message->op_id);
        return;
    }
    apply_authoritative_op(state, message);
    state->has_inflight = false;
    snprintf(state->status, sizeof(state->status), "ACK %s at version %d", message->op_id, state->version);
    state_pump(state);
}

static void handle_remote(TsClientState *state, const TsMessage *message)
{
    int incoming_version = message->version;
    if (incoming_version <= state->version) {
        return;
    }
    if (incoming_version != state->version + 1) {
        TsMessage request;
        ts_message_init(&request);
        snprintf(request.type, sizeof(request.type), "REQUEST_DOC");
        snprintf(state->status, sizeof(state->status), "missed remote operation; requesting document");
        state->send_fn(&request, state->send_userdata);
        return;
    }
    apply_authoritative_op(state, message);
    snprintf(state->status, sizeof(state->status), "remote op at version %d", state->version);
}

void ts_client_state_handle_message(TsClientState *state, const TsMessage *message)
{
    if (strcmp(message->type, "DOC_STATE") == 0) {
        ts_client_state_set_document(
            state,
            message->content ? message->content : "",
            message->version < 0 ? 0 : message->version,
            message->history_start_version < 0 ? 0 : message->history_start_version
        );
    } else if (strcmp(message->type, "WELCOME") == 0) {
        if (message->client_id[0] != '\0') {
            snprintf(state->client_id, sizeof(state->client_id), "%s", message->client_id);
        }
        snprintf(state->status, sizeof(state->status), "connected as %s", state->client_id);
    } else if (strcmp(message->type, "ACK") == 0) {
        handle_ack(state, message);
    } else if (strcmp(message->type, "REMOTE_OP") == 0) {
        handle_remote(state, message);
    } else if (strcmp(message->type, "RESYNC_REQUIRED") == 0) {
        TsMessage request;
        ts_message_init(&request);
        snprintf(request.type, sizeof(request.type), "REQUEST_DOC");
        state->has_inflight = false;
        snprintf(state->status, sizeof(state->status), "server requested resync");
        state->send_fn(&request, state->send_userdata);
    } else if (strcmp(message->type, "SAVE_QUEUED") == 0) {
        snprintf(state->status, sizeof(state->status), "save queued at version %d", message->version);
    } else if (strcmp(message->type, "ERROR") == 0) {
        snprintf(state->status, sizeof(state->status), "error: %.140s", message->message);
    }
}

static void state_pump(TsClientState *state)
{
    if (state->has_inflight || state->queue_len == 0 || state->send_fn == NULL) {
        return;
    }
    TsPendingOperation next;
    if (!pop_queue(state, &next)) {
        return;
    }
    next.base_version = state->version;
    state->inflight = next;
    state->has_inflight = true;

    TsMessage message;
    ts_message_init(&message);
    snprintf(message.type, sizeof(message.type), "OP");
    snprintf(message.client_id, sizeof(message.client_id), "%s", state->client_id);
    snprintf(message.op_id, sizeof(message.op_id), "%s", next.op_id);
    message.base_version = next.base_version;
    message.op = next.op;
    message.has_op = true;
    state->send_fn(&message, state->send_userdata);
}
