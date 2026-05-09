#ifndef TERMSYNC_H
#define TERMSYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <pthread.h>

#define TS_MAX_LINE_BYTES (1024 * 1024)
#define TS_MAX_TYPE 32
#define TS_MAX_CLIENT_ID 64
#define TS_MAX_OP_ID 64
#define TS_MAX_STATUS 160
#define TS_MAX_REASON 160
#define TS_MAX_UTF8_CHAR 8

typedef enum {
    TS_OP_INVALID = 0,
    TS_OP_INSERT,
    TS_OP_DELETE,
    TS_OP_NOOP
} TsOpKind;

typedef struct {
    TsOpKind kind;
    size_t pos;
    char ch[TS_MAX_UTF8_CHAR];
    char reason[TS_MAX_REASON];
} TsOperation;

typedef struct {
    char type[TS_MAX_TYPE];
    char client_id[TS_MAX_CLIENT_ID];
    char op_id[TS_MAX_OP_ID];
    char message[TS_MAX_STATUS];
    char reason[TS_MAX_REASON];
    int base_version;
    int server_version;
    int version;
    int history_start_version;
    TsOperation op;
    TsOperation original_op;
    bool has_op;
    bool has_original_op;
    char *content;
} TsMessage;

typedef struct {
    int fd;
    char *buffer;
    size_t buffer_len;
    pthread_mutex_t send_lock;
} TsTransport;

typedef struct {
    int server_version;
    char client_id[TS_MAX_CLIENT_ID];
    char op_id[TS_MAX_OP_ID];
    int base_version;
    TsOperation original_op;
    TsOperation op;
} TsHistoryEntry;

typedef struct {
    char *text;
    int version;
    int history_start_version;
    size_t history_limit;
    size_t history_count;
    TsHistoryEntry *history;
    pthread_mutex_t lock;
} TsDocument;

typedef enum {
    TS_PROCESS_OK,
    TS_PROCESS_ERROR,
    TS_PROCESS_RESYNC
} TsProcessStatus;

typedef struct {
    TsProcessStatus status;
    TsMessage messages[2];
    size_t message_count;
    TsMessage ack;
    TsMessage remote;
    bool has_ack;
    bool has_remote;
    char *snapshot_content;
    int snapshot_version;
    char log_json[4096];
} TsProcessResult;

typedef int (*TsSendFn)(const TsMessage *message, void *userdata);

typedef struct {
    char op_id[TS_MAX_OP_ID];
    TsOperation op;
    int base_version;
} TsPendingOperation;

typedef struct {
    char client_id[TS_MAX_CLIENT_ID];
    char *content;
    int version;
    int history_start_version;
    TsPendingOperation *queue;
    size_t queue_len;
    size_t queue_cap;
    bool has_inflight;
    TsPendingOperation inflight;
    int next_op_number;
    char status[TS_MAX_STATUS];
    TsSendFn send_fn;
    void *send_userdata;
} TsClientState;

typedef struct {
    pid_t pid;
    int write_fd;
} TsAutosaveProcess;

void ts_operation_init(TsOperation *op, TsOpKind kind, size_t pos, const char *ch);
const char *ts_operation_kind_name(TsOpKind kind);
TsOpKind ts_operation_kind_from_name(const char *name);
TsOperation ts_operation_clone(const TsOperation *op);
TsOperation ts_operation_noop(const TsOperation *op, const char *reason);

char *ts_strdup(const char *value);
char *ts_read_file_text(const char *path);
int ts_write_all_fd(int fd, const void *data, size_t len);
size_t ts_utf8_char_count(const char *text);
size_t ts_utf8_byte_offset(const char *text, size_t char_pos);
size_t ts_utf8_char_bytes(const char *text);
char *ts_apply_operation_to_text(const char *text, const TsOperation *op);
void ts_msleep(long milliseconds);

void ts_message_init(TsMessage *message);
void ts_message_free(TsMessage *message);
char *ts_encode_message(const TsMessage *message);
int ts_decode_message(const char *line, TsMessage *message, char *error, size_t error_size);
void ts_transport_init(TsTransport *transport, int fd);
void ts_transport_destroy(TsTransport *transport);
int ts_transport_send(TsTransport *transport, const TsMessage *message);
int ts_transport_recv(TsTransport *transport, TsMessage *message, char *error, size_t error_size);

void ts_document_init(TsDocument *doc, const char *initial_text, size_t history_limit);
void ts_document_destroy(TsDocument *doc);
char *ts_document_content(TsDocument *doc);
void ts_document_snapshot(TsDocument *doc, TsMessage *snapshot);
TsOperation ts_transform_against(const TsOperation *op, const TsOperation *previous);
void ts_process_result_init(TsProcessResult *result);
void ts_process_result_free(TsProcessResult *result);
TsProcessResult ts_document_process_operation(
    TsDocument *doc,
    const char *client_id,
    const char *op_id,
    int base_version,
    const TsOperation *op
);

void ts_client_state_init(
    TsClientState *state,
    const char *client_id,
    TsSendFn send_fn,
    void *send_userdata
);
void ts_client_state_destroy(TsClientState *state);
void ts_client_state_set_document(TsClientState *state, const char *content, int version, int history_start_version);
int ts_client_state_queue_operation(TsClientState *state, const TsOperation *op, char *op_id_out, size_t out_size);
void ts_client_state_handle_message(TsClientState *state, const TsMessage *message);

int ts_atomic_write_text(const char *path, const char *content);
int ts_write_with_backup(const char *path, const char *content);
int ts_append_json_log(const char *path, const char *entry_json);
int ts_start_autosave_process(
    TsAutosaveProcess *process,
    const char *document_path,
    const char *log_path,
    long interval_ms
);
int ts_autosave_send_snapshot(int fd, const char *content, int version);
int ts_autosave_send_save_now(int fd, const char *content, int version);
int ts_autosave_send_log(int fd, const char *entry_json);
int ts_autosave_send_stop(int fd);
void ts_stop_autosave_process(TsAutosaveProcess *process);

void ts_format_operation_event(const char *entry_json, char *out, size_t out_size);
void ts_format_save_event(const char *reason, const char *client_id, int version, const char *path, char *out, size_t out_size);
void ts_format_server_event(const char *entry_json, char *out, size_t out_size);

#endif
