#include "termsync.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/* 每个 TCP 连接对应一个客户端结构，由独立线程负责读取和分发消息。 */
typedef struct TsClientConnection {
    char client_id[TS_MAX_CLIENT_ID];
    char address[128];
    TsTransport transport;
    pthread_t thread;
    struct TsServer *server;
    struct TsClientConnection *next;
} TsClientConnection;

/* 服务端全局状态。
 * document 是唯一权威文档；clients 链表记录当前在线连接；autosave 负责异步落盘。
 */
typedef struct TsServer {
    char host[128];
    int port;
    char document_path[512];
    char log_path[1024];
    int history_limit;
    long autosave_interval_ms;
    int server_fd;
    bool stopping;
    int next_client_number;
    pthread_mutex_t clients_lock;
    TsClientConnection *clients;
    TsDocument document;
    TsAutosaveProcess autosave;
} TsServer;

/* 信号处理函数只设置标志位，真正清理工作在主循环中完成。 */
static volatile sig_atomic_t g_stop_requested = 0;
static TsServer *g_server = NULL;

static void signal_stop(int signum)
{
    (void)signum;
    g_stop_requested = 1;
}

/* 安装 Ctrl-C/SIGTERM 退出处理，并忽略 SIGPIPE，避免断开连接上的写入杀死进程。 */
static int install_signal_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &action, NULL) != 0) {
        return -1;
    }

    struct sigaction ignore;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, NULL) != 0) {
        return -1;
    }
    return 0;
}

/* 服务端终端输出统一走这里，确保每条事件立即 flush。 */
static void print_event(const char *message)
{
    if (message != NULL && message[0] != '\0') {
        printf("%s\n", message);
        fflush(stdout);
    }
}

/* 事件既写入 JSON 日志，也格式化成人类可读文本打印到终端。 */
static void server_log(TsServer *server, const char *entry_json)
{
    char line[512];
    ts_autosave_send_log(server->autosave.write_fd, entry_json);
    ts_format_server_event(entry_json, line, sizeof(line));
    print_event(line);
}

/* 调用方已经持有 clients_lock，因此函数名带 locked，避免误以为这里会自己加锁。 */
static bool client_id_exists_locked(TsServer *server, const char *client_id)
{
    for (TsClientConnection *client = server->clients; client != NULL; client = client->next) {
        if (strcmp(client->client_id, client_id) == 0) {
            return true;
        }
    }
    return false;
}

/* 为新连接选择客户端 ID。
 * 如果请求的 ID 未被占用就尊重请求，否则按 u1/u2/... 自动分配。
 */
static void choose_client_id(TsServer *server, const char *requested, char *out, size_t out_size)
{
    pthread_mutex_lock(&server->clients_lock);
    if (requested != NULL && requested[0] != '\0' && !client_id_exists_locked(server, requested)) {
        snprintf(out, out_size, "%s", requested);
    } else {
        do {
            snprintf(out, out_size, "u%d", server->next_client_number++);
        } while (client_id_exists_locked(server, out));
    }
    pthread_mutex_unlock(&server->clients_lock);
}

/* 把客户端加入在线链表。链表只在持锁时修改。 */
static void register_client(TsServer *server, TsClientConnection *client)
{
    pthread_mutex_lock(&server->clients_lock);
    client->next = server->clients;
    server->clients = client;
    pthread_mutex_unlock(&server->clients_lock);
}

/* 客户端线程退出前从在线链表移除自己。 */
static void unregister_client(TsServer *server, const char *client_id)
{
    pthread_mutex_lock(&server->clients_lock);
    TsClientConnection **cursor = &server->clients;
    while (*cursor != NULL) {
        if (strcmp((*cursor)->client_id, client_id) == 0) {
            *cursor = (*cursor)->next;
            break;
        }
        cursor = &(*cursor)->next;
    }
    pthread_mutex_unlock(&server->clients_lock);
}

/* 广播消息给所有在线客户端，可排除提交者，避免对方重复收到自己的 REMOTE_OP。 */
static void broadcast_message(TsServer *server, const TsMessage *message, const char *exclude_client_id)
{
    pthread_mutex_lock(&server->clients_lock);
    for (TsClientConnection *client = server->clients; client != NULL; client = client->next) {
        if (exclude_client_id != NULL && strcmp(client->client_id, exclude_client_id) == 0) {
            continue;
        }
        ts_transport_send(&client->transport, message);
    }
    pthread_mutex_unlock(&server->clients_lock);
}

/* 处理单条客户端消息。
 * OP 会进入文档临界区并产生 ACK/广播/保存快照；其他消息多是控制类请求。
 */
static void dispatch_message(TsClientConnection *client, const TsMessage *message)
{
    TsServer *server = client->server;
    if (strcmp(message->type, "OP") == 0) {
        TsProcessResult result = ts_document_process_operation(
            &server->document,
            client->client_id,
            message->op_id,
            message->base_version,
            message->has_op ? &message->op : NULL
        );
        if (result.status == TS_PROCESS_OK) {
            /* 文档修改成功后，把最新快照交给自动保存进程，并把最终操作广播出去。 */
            ts_autosave_send_snapshot(server->autosave.write_fd, result.snapshot_content, result.snapshot_version);
            server_log(server, result.log_json);
            if (result.has_ack) {
                ts_transport_send(&client->transport, &result.ack);
            }
            if (result.has_remote) {
                broadcast_message(server, &result.remote, client->client_id);
            }
        } else {
            for (size_t i = 0; i < result.message_count; ++i) {
                ts_transport_send(&client->transport, &result.messages[i]);
            }
        }
        ts_process_result_free(&result);
    } else if (strcmp(message->type, "SAVE") == 0) {
        /* 手动保存只排队给子进程执行，服务端线程不直接写磁盘。 */
        TsMessage snapshot;
        ts_document_snapshot(&server->document, &snapshot);
        ts_autosave_send_save_now(server->autosave.write_fd, snapshot.content, snapshot.version);
        char line[512];
        ts_format_save_event("requested", client->client_id, snapshot.version, server->document_path, line, sizeof(line));
        print_event(line);
        TsMessage response;
        ts_message_init(&response);
        snprintf(response.type, sizeof(response.type), "SAVE_QUEUED");
        response.version = snapshot.version;
        ts_transport_send(&client->transport, &response);
        ts_message_free(&snapshot);
    } else if (strcmp(message->type, "REQUEST_DOC") == 0) {
        /* 客户端检测到版本缺口或收到 RESYNC_REQUIRED 后，会请求完整文档快照。 */
        TsMessage snapshot;
        ts_document_snapshot(&server->document, &snapshot);
        ts_transport_send(&client->transport, &snapshot);
        ts_message_free(&snapshot);
    } else if (strcmp(message->type, "PING") == 0) {
        TsMessage pong;
        ts_message_init(&pong);
        snprintf(pong.type, sizeof(pong.type), "PONG");
        ts_transport_send(&client->transport, &pong);
    } else {
        TsMessage error;
        ts_message_init(&error);
        snprintf(error.type, sizeof(error.type), "ERROR");
        snprintf(error.message, sizeof(error.message), "unsupported message type: %s", message->type);
        ts_transport_send(&client->transport, &error);
    }
}

/* 客户端工作线程。
 * 第一个消息必须是 JOIN；握手成功后发送 WELCOME 和 DOC_STATE，然后持续读消息直到断开。
 */
static void *client_thread_main(void *arg)
{
    TsClientConnection *client = arg;
    char error[256];
    TsMessage message;
    ts_message_init(&message);
    if (ts_transport_recv(&client->transport, &message, error, sizeof(error)) != 0 || strcmp(message.type, "JOIN") != 0) {
        TsMessage response;
        ts_message_init(&response);
        snprintf(response.type, sizeof(response.type), "ERROR");
        snprintf(response.message, sizeof(response.message), "first message must be JOIN");
        ts_transport_send(&client->transport, &response);
        ts_message_free(&message);
        goto done;
    }

    choose_client_id(client->server, message.client_id, client->client_id, sizeof(client->client_id));
    register_client(client->server, client);

    TsMessage welcome;
    ts_message_init(&welcome);
    snprintf(welcome.type, sizeof(welcome.type), "WELCOME");
    snprintf(welcome.client_id, sizeof(welcome.client_id), "%s", client->client_id);
    ts_transport_send(&client->transport, &welcome);

    TsMessage snapshot;
    ts_document_snapshot(&client->server->document, &snapshot);
    ts_transport_send(&client->transport, &snapshot);
    ts_message_free(&snapshot);

    char log_entry[512];
    snprintf(log_entry, sizeof(log_entry), "{\"event\":\"join\",\"client_id\":\"%s\",\"address\":\"%s\"}", client->client_id, client->address);
    server_log(client->server, log_entry);
    ts_message_free(&message);

    while (!client->server->stopping) {
        ts_message_init(&message);
        if (ts_transport_recv(&client->transport, &message, error, sizeof(error)) != 0) {
            ts_message_free(&message);
            break;
        }
        dispatch_message(client, &message);
        ts_message_free(&message);
    }

    snprintf(log_entry, sizeof(log_entry), "{\"event\":\"leave\",\"client_id\":\"%s\"}", client->client_id);
    unregister_client(client->server, client->client_id);
    server_log(client->server, log_entry);

done:
    close(client->transport.fd);
    ts_transport_destroy(&client->transport);
    free(client);
    return NULL;
}

/* 简单命令行整数参数解析，未知或缺失时使用 fallback。 */
static int parse_int_arg(int argc, char **argv, const char *name, int fallback)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], name) == 0) {
            return atoi(argv[i + 1]);
        }
    }
    return fallback;
}

/* 简单命令行字符串参数解析。 */
static const char *parse_str_arg(int argc, char **argv, const char *name, const char *fallback)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return fallback;
}

/* 创建监听 socket，并设置为非阻塞，主循环通过 select 周期性检查退出标志。 */
static int server_listen(TsServer *server)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)server->port);
    if (inet_pton(AF_INET, server->host, &addr.sin_addr) != 1) {
        if (strcmp(server->host, "0.0.0.0") == 0) {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            fprintf(stderr, "invalid IPv4 host: %s\n", server->host);
            close(fd);
            return -1;
        }
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 32) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        perror("fcntl");
        close(fd);
        return -1;
    }
    server->server_fd = fd;
    return 0;
}

/* 等待新连接最多 200ms，这样 Ctrl-C 或 SIGTERM 能被主循环及时响应。 */
static int wait_for_client(TsServer *server)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(server->server_fd, &set);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    int ready = select(server->server_fd + 1, &set, NULL, NULL, &tv);
    if (ready < 0) {
        if (errno == EINTR) {
            return 0;
        }
        perror("select");
        return -1;
    }
    return ready > 0 && FD_ISSET(server->server_fd, &set);
}

/* 关闭服务端：停止接收新连接、保存最终文档、停止自动保存子进程。 */
static void server_shutdown(TsServer *server)
{
    if (server->stopping) {
        return;
    }
    server->stopping = true;
    if (server->server_fd >= 0) {
        close(server->server_fd);
        server->server_fd = -1;
    }
    char *content = ts_document_content(&server->document);
    ts_autosave_send_save_now(server->autosave.write_fd, content ? content : "", server->document.version);
    free(content);
    char line[512];
    ts_format_save_event("shutdown", NULL, server->document.version, server->document_path, line, sizeof(line));
    print_event(line);
    ts_stop_autosave_process(&server->autosave);
}

/* 服务端入口：加载初始文件、启动保存进程、监听连接并为每个客户端创建线程。 */
int main(int argc, char **argv)
{
    TsServer server;
    memset(&server, 0, sizeof(server));
    snprintf(server.host, sizeof(server.host), "%s", parse_str_arg(argc, argv, "--host", "127.0.0.1"));
    server.port = parse_int_arg(argc, argv, "--port", 8765);
    snprintf(server.document_path, sizeof(server.document_path), "%s", parse_str_arg(argc, argv, "--file", "shared.c"));
    snprintf(server.log_path, sizeof(server.log_path), "%s.edit.log", server.document_path);
    server.history_limit = parse_int_arg(argc, argv, "--history", 1000);
    server.autosave_interval_ms = parse_int_arg(argc, argv, "--autosave-interval-ms", 5000);
    server.server_fd = -1;
    server.autosave.write_fd = -1;
    server.autosave.pid = -1;
    server.next_client_number = 1;
    pthread_mutex_init(&server.clients_lock, NULL);

    if (install_signal_handlers() != 0) {
        perror("sigaction");
        pthread_mutex_destroy(&server.clients_lock);
        return 1;
    }

    char *initial_text = ts_read_file_text(server.document_path);
    ts_document_init(&server.document, initial_text ? initial_text : "", (size_t)server.history_limit);
    free(initial_text);
    if (ts_start_autosave_process(&server.autosave, server.document_path, server.log_path, server.autosave_interval_ms) != 0) {
        fprintf(stderr, "failed to start autosave process\n");
        ts_document_destroy(&server.document);
        return 1;
    }
    char *snapshot = ts_document_content(&server.document);
    ts_autosave_send_snapshot(server.autosave.write_fd, snapshot ? snapshot : "", server.document.version);
    free(snapshot);

    if (server_listen(&server) != 0) {
        server_shutdown(&server);
        ts_document_destroy(&server.document);
        return 1;
    }

    g_server = &server;

    printf("Collaborative server listening on %s:%d\n", server.host, server.port);
    printf("Document: %s\n", server.document_path);
    fflush(stdout);

    /* 主线程只负责 accept；每个连接的协议处理在独立线程中进行。 */
    while (!server.stopping && !g_stop_requested) {
        int ready = wait_for_client(&server);
        if (ready < 0) {
            break;
        }
        if (ready == 0) {
            continue;
        }
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept(server.server_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        int yes = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        /* 连接对象交给客户端线程，线程结束时会负责释放。 */
        TsClientConnection *client = calloc(1, sizeof(*client));
        if (client == NULL) {
            close(client_fd);
            continue;
        }
        client->server = &server;
        inet_ntop(AF_INET, &peer.sin_addr, client->address, sizeof(client->address));
        size_t used = strlen(client->address);
        snprintf(client->address + used, sizeof(client->address) - used, ":%d", ntohs(peer.sin_port));
        ts_transport_init(&client->transport, client_fd);
        pthread_create(&client->thread, NULL, client_thread_main, client);
        pthread_detach(client->thread);
    }

    server_shutdown(&server);
    ts_document_destroy(&server.document);
    pthread_mutex_destroy(&server.clients_lock);
    g_server = NULL;
    return 0;
}
