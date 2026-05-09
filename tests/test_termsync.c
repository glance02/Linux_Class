#include "../termsync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#endif

typedef struct {
    TsMessage items[32];
    size_t len;
} SentMessages;

static int failures = 0;

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
        return; \
    } \
} while (0)

#define ASSERT_INT(expected, actual) do { \
    int e__ = (expected); \
    int a__ = (actual); \
    if (e__ != a__) { \
        fprintf(stderr, "FAIL %s:%d: expected %d got %d\n", __FILE__, __LINE__, e__, a__); \
        failures++; \
        return; \
    } \
} while (0)

#define ASSERT_STR(expected, actual) do { \
    const char *e__ = (expected); \
    const char *a__ = (actual); \
    if (strcmp(e__, a__) != 0) { \
        fprintf(stderr, "FAIL %s:%d: expected [%s] got [%s]\n", __FILE__, __LINE__, e__, a__); \
        failures++; \
        return; \
    } \
} while (0)

static int record_send(const TsMessage *message, void *userdata)
{
    SentMessages *sent = userdata;
    if (sent->len >= 32) {
        failures++;
        return -1;
    }
    sent->items[sent->len++] = *message;
    return 0;
}

static void test_delete_position_transforms_after_prior_inserts(void)
{
    TsDocument doc;
    ts_document_init(&doc, "hello world", 1000);
    for (int i = 0; i < 4; ++i) {
        TsOperation op;
        ts_operation_init(&op, TS_OP_INSERT, (size_t)(5 + i), "!");
        TsProcessResult result = ts_document_process_operation(&doc, "A", "A-op", i, &op);
        ASSERT_INT(TS_PROCESS_OK, result.status);
        ts_process_result_free(&result);
    }
    TsOperation del;
    ts_operation_init(&del, TS_OP_DELETE, 5, NULL);
    TsProcessResult result = ts_document_process_operation(&doc, "B", "B-1", 0, &del);
    ASSERT_INT(TS_PROCESS_OK, result.status);
    ASSERT_INT(5, result.ack.server_version);
    ASSERT_INT(9, (int)result.ack.op.pos);
    char *content = ts_document_content(&doc);
    ASSERT_STR("hello!!!!world", content);
    free(content);
    ts_process_result_free(&result);
    ts_document_destroy(&doc);
}

static void test_same_character_delete_becomes_noop(void)
{
    TsDocument doc;
    ts_document_init(&doc, "abc", 1000);
    TsOperation del;
    ts_operation_init(&del, TS_OP_DELETE, 1, NULL);
    TsProcessResult first = ts_document_process_operation(&doc, "A", "A-1", 0, &del);
    TsProcessResult second = ts_document_process_operation(&doc, "B", "B-1", 0, &del);
    ASSERT_INT(TS_PROCESS_OK, first.status);
    ASSERT_INT(TS_PROCESS_OK, second.status);
    ASSERT_INT(TS_OP_NOOP, second.ack.op.kind);
    char *content = ts_document_content(&doc);
    ASSERT_STR("ac", content);
    ASSERT_INT(2, doc.version);
    free(content);
    ts_process_result_free(&first);
    ts_process_result_free(&second);
    ts_document_destroy(&doc);
}

static void test_history_expiry_requests_resync(void)
{
    TsDocument doc;
    ts_document_init(&doc, "", 2);
    for (int i = 0; i < 3; ++i) {
        TsOperation op;
        ts_operation_init(&op, TS_OP_INSERT, (size_t)i, "x");
        TsProcessResult result = ts_document_process_operation(&doc, "A", "A-1", i, &op);
        ASSERT_INT(TS_PROCESS_OK, result.status);
        ts_process_result_free(&result);
    }
    ASSERT_INT(1, doc.history_start_version);
    TsOperation old;
    ts_operation_init(&old, TS_OP_INSERT, 0, "y");
    TsProcessResult expired = ts_document_process_operation(&doc, "B", "B-1", 0, &old);
    ASSERT_INT(TS_PROCESS_RESYNC, expired.status);
    ASSERT_STR("RESYNC_REQUIRED", expired.messages[0].type);
    ASSERT_STR("DOC_STATE", expired.messages[1].type);
    ts_process_result_free(&expired);
    ts_document_destroy(&doc);
}

static void test_server_version_increment_and_utf8(void)
{
    TsDocument doc;
    ts_document_init(&doc, "", 1000);
    const char *chars[] = {"a", "你", "b"};
    for (int i = 0; i < 3; ++i) {
        TsOperation op;
        ts_operation_init(&op, TS_OP_INSERT, (size_t)i, chars[i]);
        TsProcessResult result = ts_document_process_operation(&doc, "A", "A-1", i, &op);
        ASSERT_INT(i + 1, result.ack.server_version);
        ts_process_result_free(&result);
    }
    char *content = ts_document_content(&doc);
    ASSERT_STR("a你b", content);
    ASSERT_INT(3, (int)ts_utf8_char_count(content));
    free(content);
    ts_document_destroy(&doc);
}

static void test_client_pending_queue_sends_after_ack(void)
{
    SentMessages sent = {0};
    TsClientState state;
    ts_client_state_init(&state, "u1", record_send, &sent);
    ts_client_state_set_document(&state, "abc", 0, 0);
    TsOperation x;
    TsOperation y;
    ts_operation_init(&x, TS_OP_INSERT, 3, "x");
    ts_operation_init(&y, TS_OP_INSERT, 4, "y");
    char first[TS_MAX_OP_ID];
    char second[TS_MAX_OP_ID];
    ts_client_state_queue_operation(&state, &x, first, sizeof(first));
    ts_client_state_queue_operation(&state, &y, second, sizeof(second));
    ASSERT_INT(1, (int)sent.len);
    ASSERT_STR(first, sent.items[0].op_id);
    ASSERT_INT(0, sent.items[0].base_version);
    ASSERT_INT(1, (int)state.queue_len);

    TsMessage ack;
    ts_message_init(&ack);
    snprintf(ack.type, sizeof(ack.type), "ACK");
    snprintf(ack.op_id, sizeof(ack.op_id), "%s", first);
    ack.version = 1;
    ack.op = x;
    ack.has_op = true;
    ts_client_state_handle_message(&state, &ack);
    ASSERT_STR("abcx", state.content);
    ASSERT_INT(2, (int)sent.len);
    ASSERT_STR(second, sent.items[1].op_id);
    ASSERT_INT(1, sent.items[1].base_version);
    ts_client_state_destroy(&state);
}

static void test_remote_gap_requests_document(void)
{
    SentMessages sent = {0};
    TsClientState state;
    ts_client_state_init(&state, "u1", record_send, &sent);
    ts_client_state_set_document(&state, "", 0, 0);
    TsMessage remote;
    ts_message_init(&remote);
    snprintf(remote.type, sizeof(remote.type), "REMOTE_OP");
    remote.version = 3;
    ts_operation_init(&remote.op, TS_OP_INSERT, 0, "z");
    remote.has_op = true;
    ts_client_state_handle_message(&state, &remote);
    ASSERT_TRUE(sent.len > 0);
    ASSERT_STR("REQUEST_DOC", sent.items[sent.len - 1].type);
    ts_client_state_destroy(&state);
}

static void test_protocol_json_lines_handles_newline_and_utf8(void)
{
    TsMessage message;
    ts_message_init(&message);
    snprintf(message.type, sizeof(message.type), "DOC_STATE");
    message.version = 7;
    message.content = ts_strdup("hello\n你");
    char *encoded = ts_encode_message(&message);
    ASSERT_TRUE(strstr(encoded, "\\n") != NULL);
    TsMessage decoded;
    char error[128];
    ASSERT_INT(0, ts_decode_message(encoded, &decoded, error, sizeof(error)));
    ASSERT_STR("DOC_STATE", decoded.type);
    ASSERT_INT(7, decoded.version);
    ASSERT_STR("hello\n你", decoded.content);
    free(encoded);
    ts_message_free(&message);
    ts_message_free(&decoded);
}

static char *read_small_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    char *data = calloc(1, 256);
    size_t n = fread(data, 1, 255, fp);
    data[n] = '\0';
    fclose(fp);
    return data;
}

static void test_save_backup_and_autosave_file(void)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/termsync-test-%ld", (long)getpid());
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0775);
#endif
    char path[512];
    char log_path[512];
    snprintf(path, sizeof(path), "%s/shared.c", dir);
    snprintf(log_path, sizeof(log_path), "%s/shared.c.edit.log", dir);
    ASSERT_INT(0, ts_atomic_write_text(path, "old"));
    ASSERT_INT(0, ts_write_with_backup(path, "new"));
    char backup[1024];
    snprintf(backup, sizeof(backup), "%s.bak", path);
    char *backup_content = read_small_file(backup);
    ASSERT_STR("old", backup_content);
    free(backup_content);

    TsAutosaveProcess process = {.pid = -1, .write_fd = -1};
    ASSERT_INT(0, ts_start_autosave_process(&process, path, log_path, 50));
    ASSERT_INT(0, ts_autosave_send_snapshot(process.write_fd, "autosaved", 3));
    ts_msleep(120);
    ts_stop_autosave_process(&process);
    char autosave[1024];
    snprintf(autosave, sizeof(autosave), "%s.autosave", path);
    char *autosave_content = read_small_file(autosave);
    ASSERT_STR("autosaved", autosave_content);
    free(autosave_content);
}

static void test_server_event_formatting(void)
{
    char out[256];
    ts_format_operation_event(
        "{\"event\":\"operation\",\"server_version\":12,\"client_id\":\"u1\",\"base_version\":11,"
        "\"transformed_op\":{\"kind\":\"insert\",\"pos\":5,\"char\":\"你\"}}",
        out,
        sizeof(out)
    );
    ASSERT_STR("OP v12 client=u1 insert pos=5 char='你' base=11", out);
    ts_format_save_event("requested", "u1", 13, "shared.c", out, sizeof(out));
    ASSERT_STR("SAVE requested client=u1 version=13 file=shared.c", out);
    ts_format_server_event("{\"event\":\"leave\",\"client_id\":\"u1\"}", out, sizeof(out));
    ASSERT_STR("LEAVE client=u1", out);
}

int main(void)
{
    test_delete_position_transforms_after_prior_inserts();
    test_same_character_delete_becomes_noop();
    test_history_expiry_requests_resync();
    test_server_version_increment_and_utf8();
    test_client_pending_queue_sends_after_ack();
    test_remote_gap_requests_document();
    test_protocol_json_lines_handles_newline_and_utf8();
    test_save_backup_and_autosave_file();
    test_server_event_formatting();
    if (failures == 0) {
        printf("All TermSync C tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
