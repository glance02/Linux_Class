#ifndef TERMSYNC_H
#define TERMSYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <pthread.h>

/* 单条 JSON Lines 协议消息的最大长度，避免恶意或异常客户端撑爆接收缓冲区。 */
#define TS_MAX_LINE_BYTES (1024 * 1024)
/* 以下长度限制用于固定大小字段，便于跨线程传递消息时少做动态分配。 */
#define TS_MAX_TYPE 32
#define TS_MAX_CLIENT_ID 64
#define TS_MAX_OP_ID 64
#define TS_MAX_STATUS 160
#define TS_MAX_REASON 160
/* UTF-8 单字符最多 4 字节，这里留 8 字节空间包含 '\0' 和一定冗余。 */
#define TS_MAX_UTF8_CHAR 8

/* 文档操作类型：插入、删除和 NOOP。NOOP 用于 OT 冲突后“无事可做”的结果。 */
typedef enum {
    TS_OP_INVALID = 0,
    TS_OP_INSERT,
    TS_OP_DELETE,
    TS_OP_NOOP
} TsOpKind;

/* 单个编辑操作。
 * pos 使用“UTF-8 字符位置”而不是字节位置，避免中文等多字节字符被截断。
 */
typedef struct {
    TsOpKind kind;
    size_t pos;
    char ch[TS_MAX_UTF8_CHAR];
    char reason[TS_MAX_REASON];
} TsOperation;

/* 网络协议中的通用消息结构。
 * 不同 type 会使用其中不同字段：例如 OP/ACK/REMOTE_OP 带 op，DOC_STATE 带 content。
 */
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

/* socket 传输层封装。
 * buffer 保存未读完的一行 JSON；send_lock 保证多个线程不会把消息写交错。
 */
typedef struct {
    int fd;
    char *buffer;
    size_t buffer_len;
    pthread_mutex_t send_lock;
} TsTransport;

/* 服务端历史记录条目。
 * original_op 是客户端基于旧版本提交的原始操作，op 是经 OT 转换后真正应用的操作。
 */
typedef struct {
    int server_version;
    char client_id[TS_MAX_CLIENT_ID];
    char op_id[TS_MAX_OP_ID];
    int base_version;
    TsOperation original_op;
    TsOperation op;
} TsHistoryEntry;

/* 服务端权威文档。
 * text/version 是唯一可信状态；history 用于把旧 base_version 的客户端操作转换到最新版本。
 */
typedef struct {
    char *text;
    int version;
    int history_start_version;
    size_t history_limit;
    size_t history_count;
    TsHistoryEntry *history;
    pthread_mutex_t lock;
} TsDocument;

/* 文档处理结果：正常应用、协议错误，或历史过期/版本异常导致客户端必须重同步。 */
typedef enum {
    TS_PROCESS_OK,
    TS_PROCESS_ERROR,
    TS_PROCESS_RESYNC
} TsProcessStatus;

/* 服务端处理一次 OP 后产出的所有副作用。
 * ack 发回提交者，remote 广播给其他客户端，snapshot/log_json 交给保存进程。
 */
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

/* 客户端等待发送或正在等待 ACK 的操作。
 * base_version 在真正发送时才确定，这样连续按键会按服务端 ACK 节奏串行提交。
 */
typedef struct {
    char op_id[TS_MAX_OP_ID];
    TsOperation op;
    int base_version;
} TsPendingOperation;

/* 客户端本地状态。
 * 客户端不做乐观合并，只维护输入队列和一个 inflight 操作，避免本地状态与服务端乱序。
 */
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

/* 独立自动保存进程的信息。父进程只通过 write_fd 往子进程发送事件。 */
typedef struct {
    pid_t pid;
    int write_fd;
} TsAutosaveProcess;

/* 操作和通用工具函数。 */
void ts_operation_init(TsOperation *op, TsOpKind kind, size_t pos, const char *ch);
const char *ts_operation_kind_name(TsOpKind kind);
TsOpKind ts_operation_kind_from_name(const char *name);
TsOperation ts_operation_clone(const TsOperation *op);
TsOperation ts_operation_noop(const TsOperation *op, const char *reason);

/* UTF-8 和文件/FD 辅助函数，所有位置相关 API 都按字符数而非字节数工作。 */
char *ts_strdup(const char *value);
char *ts_read_file_text(const char *path);
int ts_write_all_fd(int fd, const void *data, size_t len);
size_t ts_utf8_char_count(const char *text);
size_t ts_utf8_byte_offset(const char *text, size_t char_pos);
size_t ts_utf8_char_bytes(const char *text);
char *ts_apply_operation_to_text(const char *text, const TsOperation *op);
void ts_msleep(long milliseconds);

/* JSON Lines 协议编解码和带锁 socket 收发。 */
void ts_message_init(TsMessage *message);
void ts_message_free(TsMessage *message);
char *ts_encode_message(const TsMessage *message);
int ts_decode_message(const char *line, TsMessage *message, char *error, size_t error_size);
void ts_transport_init(TsTransport *transport, int fd);
void ts_transport_destroy(TsTransport *transport);
int ts_transport_send(TsTransport *transport, const TsMessage *message);
int ts_transport_recv(TsTransport *transport, TsMessage *message, char *error, size_t error_size);

/* 服务端权威文档和 OT 处理。 */
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

/* 客户端发送队列和服务端消息处理。 */
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

/* 持久化与自动保存。写正式文件前会先备份，自动保存运行在 fork 出来的子进程中。 */
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

/* 面向终端输出的事件格式化函数，把 JSON 日志转成更适合人读的一行文本。 */
void ts_format_operation_event(const char *entry_json, char *out, size_t out_size);
void ts_format_save_event(const char *reason, const char *client_id, int version, const char *path, char *out, size_t out_size);
void ts_format_server_event(const char *entry_json, char *out, size_t out_size);

#endif
