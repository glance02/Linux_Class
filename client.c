#define _XOPEN_SOURCE_EXTENDED 1
#include "termsync.h"

#include <errno.h>
#include <locale.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <curses.h>

#define CTRL_Q 17
#define CTRL_S 19

/* 后台网络线程收到的消息节点。消息内容按值搬进队列，主线程稍后处理。 */
typedef struct IncomingNode {
    TsMessage message;
    struct IncomingNode *next;
} IncomingNode;

/* 简单线程安全队列：reader 线程 push，UI 主线程 pop。 */
typedef struct {
    pthread_mutex_t lock;
    IncomingNode *head;
    IncomingNode *tail;
} IncomingQueue;

/* 终端客户端总状态。
 * state 负责协议和编辑队列；cursor_pos/scroll_line 负责本地 UI 视图。
 */
typedef struct {
    char host[128];
    int port;
    char client_id[TS_MAX_CLIENT_ID];
    TsTransport transport;
    TsClientState state;
    IncomingQueue incoming;
    pthread_t reader;
    bool running;
    size_t cursor_pos;
    int scroll_line;
} TerminalClient;

/* 初始化入站消息队列。 */
static void queue_init(IncomingQueue *queue)
{
    memset(queue, 0, sizeof(*queue));
    pthread_mutex_init(&queue->lock, NULL);
}

/* reader 线程把服务端消息推入队列，避免直接在后台线程里操作 ncurses。 */
static void queue_push(IncomingQueue *queue, const TsMessage *message)
{
    IncomingNode *node = calloc(1, sizeof(*node));
    if (node == NULL) {
        return;
    }
    node->message = *message;
    pthread_mutex_lock(&queue->lock);
    if (queue->tail == NULL) {
        queue->head = queue->tail = node;
    } else {
        queue->tail->next = node;
        queue->tail = node;
    }
    pthread_mutex_unlock(&queue->lock);
}

/* UI 主线程取出一条消息处理。取出的 TsMessage 由调用方负责 ts_message_free。 */
static bool queue_pop(IncomingQueue *queue, TsMessage *message)
{
    pthread_mutex_lock(&queue->lock);
    IncomingNode *node = queue->head;
    if (node == NULL) {
        pthread_mutex_unlock(&queue->lock);
        return false;
    }
    queue->head = node->next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    pthread_mutex_unlock(&queue->lock);
    *message = node->message;
    free(node);
    return true;
}

/* TsClientState 的发送回调，最终走同一个 transport 发送 JSON Lines。 */
static int send_from_state(const TsMessage *message, void *userdata)
{
    TerminalClient *client = userdata;
    return ts_transport_send(&client->transport, message);
}

/* 使用 getaddrinfo 支持域名、IPv4/IPv6，连接成功后返回 socket fd。 */
static int connect_tcp(const char *host, int port)
{
    char port_text[32];
    snprintf(port_text, sizeof(port_text), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *results = NULL;
    if (getaddrinfo(host, port_text, &hints, &results) != 0) {
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *rp = results; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);
    return fd;
}

/* 后台读线程：持续接收服务端消息并交给主线程。
 * 一旦连接断开，会把错误包装成 ERROR 消息进入同一个处理通道。
 */
static void *reader_main(void *arg)
{
    TerminalClient *client = arg;
    while (client->running) {
        TsMessage message;
        char error[256];
        ts_message_init(&message);
        if (ts_transport_recv(&client->transport, &message, error, sizeof(error)) != 0) {
            ts_message_init(&message);
            snprintf(message.type, sizeof(message.type), "ERROR");
            snprintf(message.message, sizeof(message.message), "%.150s", error);
            queue_push(&client->incoming, &message);
            return NULL;
        }
        queue_push(&client->incoming, &message);
    }
    return NULL;
}

/* 根据 UTF-8 字符位置计算所在行号，用于滚动和光标定位。 */
static int line_for_pos(const char *content, size_t pos)
{
    size_t byte_limit = ts_utf8_byte_offset(content, pos);
    int line = 0;
    for (size_t i = 0; i < byte_limit && content[i] != '\0'; ++i) {
        if (content[i] == '\n') {
            line++;
        }
    }
    return line;
}

/* 在给定 [start, end) 范围内安全读取一个 UTF-8 字符长度，避免跨过行尾/缓冲区尾。 */
static size_t bounded_utf8_char_bytes(const char *start, const char *end)
{
    size_t bytes = ts_utf8_char_bytes(start);
    size_t remaining = (size_t)(end - start);
    if (bytes == 0 || bytes > remaining) {
        return 1;
    }
    return bytes;
}

/* 把一个 UTF-8 字符转换成 wchar_t 后计算终端显示宽度；中文通常宽度为 2。 */
static int utf8_display_width(const char *start, size_t bytes)
{
    wchar_t wc;
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    size_t converted = mbrtowc(&wc, start, bytes, &state);
    if (converted == (size_t)-1 || converted == (size_t)-2) {
        return 1;
    }
    if (converted == 0) {
        return 0;
    }
    int width = wcwidth(wc);
    return width < 0 ? 1 : width;
}

/* 根据 UTF-8 字符位置计算屏幕列号，不能简单用字节数，否则中文会错位。 */
static int col_for_pos(const char *content, size_t pos)
{
    size_t byte_limit = ts_utf8_byte_offset(content, pos);
    int col = 0;
    for (size_t i = 0; i < byte_limit && content[i] != '\0';) {
        if (content[i] == '\n') {
            col = 0;
            i++;
        } else {
            const char *start = content + i;
            const char *end = content + byte_limit;
            size_t bytes = bounded_utf8_char_bytes(start, end);
            col += utf8_display_width(start, bytes);
            i += bytes;
        }
    }
    return col;
}

/* 渲染单行的可见片段。超过窗口宽度时停止，避免 ncurses 写出屏幕边界。 */
static void render_line_segment(int row, int col, const char *start, const char *end, int max_cols)
{
    int used = 0;
    const char *p = start;
    while (p < end && used < max_cols) {
        size_t bytes = bounded_utf8_char_bytes(p, end);
        int width = utf8_display_width(p, bytes);
        if (used + width > max_cols) {
            break;
        }
        mvaddnstr(row, col + used, p, (int)bytes);
        used += width;
        p += bytes;
    }
}

/* 渲染文档正文和行号，同时根据光标所在行调整滚动位置。 */
static void render_lines(WINDOW *stdscr, TerminalClient *client)
{
    int height;
    int width;
    getmaxyx(stdscr, height, width);
    const char *content = client->state.content;
    int cursor_line = line_for_pos(content, client->cursor_pos);
    if (cursor_line < client->scroll_line) {
        client->scroll_line = cursor_line;
    }
    if (cursor_line >= client->scroll_line + height - 2) {
        client->scroll_line = cursor_line - height + 3;
    }

    int line = 0;
    int row = 1;
    const char *start = content;
    for (const char *p = content;; ++p) {
        if (*p == '\n' || *p == '\0') {
            if (line >= client->scroll_line && row < height - 1) {
                char prefix[16];
                snprintf(prefix, sizeof(prefix), "%4d ", line + 1);
                mvaddnstr(row, 0, prefix, width - 1);
                int body_width = width - (int)strlen(prefix) - 1;
                render_line_segment(row, (int)strlen(prefix), start, p, body_width < 0 ? 0 : body_width);
                row++;
            }
            if (*p == '\0') {
                break;
            }
            line++;
            start = p + 1;
        }
    }
}

/* 完整重绘终端界面：顶部状态栏、正文、底部快捷键和光标位置。 */
static void render(TerminalClient *client)
{
    erase();
    int height;
    int width;
    getmaxyx(stdscr, height, width);
    char status[256];
    snprintf(
        status,
        sizeof(status),
        "%s | v%d | pending=%zu | %s",
        client->state.client_id,
        client->state.version,
        client->state.queue_len,
        client->state.status
    );
    attron(A_REVERSE);
    mvaddnstr(0, 0, status, width - 1);
    attroff(A_REVERSE);
    render_lines(stdscr, client);
    attron(A_REVERSE);
    mvaddnstr(height - 1, 0, "Ctrl-S save | Ctrl-Q quit", width - 1);
    attroff(A_REVERSE);

    int screen_row = line_for_pos(client->state.content, client->cursor_pos) - client->scroll_line + 1;
    int screen_col = col_for_pos(client->state.content, client->cursor_pos) + 5;
    if (screen_row > 0 && screen_row < height - 1 && screen_col >= 0 && screen_col < width) {
        move(screen_row, screen_col);
    }
    refresh();
}

/* 估算一个尚未确认的本地操作对文档字符长度的影响。 */
static long pending_operation_delta(const TsOperation *op)
{
    if (op->kind == TS_OP_INSERT) {
        return 1;
    }
    if (op->kind == TS_OP_DELETE) {
        return -1;
    }
    return 0;
}

/* 根据权威内容 + inflight + 队列估算最终长度，让光标能跟随连续输入移动。 */
static size_t projected_content_length(TerminalClient *client)
{
    long len = (long)ts_utf8_char_count(client->state.content);
    if (client->state.has_inflight) {
        len += pending_operation_delta(&client->state.inflight.op);
    }
    for (size_t i = 0; i < client->state.queue_len; ++i) {
        len += pending_operation_delta(&client->state.queue[i].op);
    }
    return len < 0 ? 0 : (size_t)len;
}

/* 服务端快照或远程操作可能缩短文档，因此每次处理消息后都要夹紧光标。 */
static void clamp_cursor_to_projected_content(TerminalClient *client)
{
    size_t len = projected_content_length(client);
    if (client->cursor_pos > len) {
        client->cursor_pos = len;
    }
}

/* 把后台线程积累的服务端消息全部处理掉，再进入下一轮渲染/按键读取。 */
static void drain_messages(TerminalClient *client)
{
    TsMessage message;
    while (queue_pop(&client->incoming, &message)) {
        ts_client_state_handle_message(&client->state, &message);
        clamp_cursor_to_projected_content(client);
        ts_message_free(&message);
    }
}

/* 在当前光标位置排队插入一个 UTF-8 字符，并让本地光标先向后移动。 */
static void queue_insert(TerminalClient *client, const char *utf8)
{
    TsOperation op;
    ts_operation_init(&op, TS_OP_INSERT, client->cursor_pos, utf8);
    ts_client_state_queue_operation(&client->state, &op, NULL, 0);
    client->cursor_pos++;
}

/* 删除光标前一个字符。实际是否成功由服务端最终 ACK 的权威操作决定。 */
static void delete_before_cursor(TerminalClient *client)
{
    if (client->cursor_pos == 0) {
        return;
    }
    TsOperation op;
    ts_operation_init(&op, TS_OP_DELETE, client->cursor_pos - 1, NULL);
    ts_client_state_queue_operation(&client->state, &op, NULL, 0);
    client->cursor_pos--;
}

/* 处理 ncurses 读到的按键。
 * 特殊键走 KEY_CODE_YES 分支，普通可打印字符会转成 UTF-8 后进入编辑队列。
 */
static void handle_key(TerminalClient *client, wint_t key, int key_result)
{
    if (key_result == KEY_CODE_YES) {
        if (key == KEY_LEFT && client->cursor_pos > 0) {
            client->cursor_pos--;
        } else if (key == KEY_RIGHT) {
            size_t len = projected_content_length(client);
            if (client->cursor_pos < len) {
                client->cursor_pos++;
            }
        } else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
            delete_before_cursor(client);
        }
        return;
    }
    if (key == CTRL_Q) {
        client->running = false;
        return;
    }
    if (key == CTRL_S) {
        TsMessage save;
        ts_message_init(&save);
        snprintf(save.type, sizeof(save.type), "SAVE");
        ts_transport_send(&client->transport, &save);
        return;
    }
    if (key == L'\n' || key == L'\r') {
        queue_insert(client, "\n");
        return;
    }
    if (key == 127 || key == 8) {
        delete_before_cursor(client);
        return;
    }
    if (iswprint((wint_t)key)) {
        char buffer[MB_CUR_MAX + 1];
        memset(buffer, 0, sizeof(buffer));
        mbstate_t state;
        memset(&state, 0, sizeof(state));
        size_t len = wcrtomb(buffer, (wchar_t)key, &state);
        if (len != (size_t)-1) {
            buffer[len] = '\0';
            queue_insert(client, buffer);
        }
    }
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

/* 简单命令行整数参数解析。 */
static int parse_int_arg(int argc, char **argv, const char *name, int fallback)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], name) == 0) {
            return atoi(argv[i + 1]);
        }
    }
    return fallback;
}

/* 客户端入口：连接服务端、发送 JOIN、启动 reader 线程并进入 ncurses 主循环。 */
int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    TerminalClient client;
    memset(&client, 0, sizeof(client));
    snprintf(client.host, sizeof(client.host), "%s", parse_str_arg(argc, argv, "--host", "127.0.0.1"));
    client.port = parse_int_arg(argc, argv, "--port", 8765);
    snprintf(client.client_id, sizeof(client.client_id), "%s", parse_str_arg(argc, argv, "--client-id", "u1"));
    client.running = true;
    queue_init(&client.incoming);

    int fd = connect_tcp(client.host, client.port);
    if (fd < 0) {
        fprintf(stderr, "failed to connect %s:%d\n", client.host, client.port);
        return 1;
    }
    ts_transport_init(&client.transport, fd);
    ts_client_state_init(&client.state, client.client_id, send_from_state, &client);

    TsMessage join;
    ts_message_init(&join);
    snprintf(join.type, sizeof(join.type), "JOIN");
    snprintf(join.client_id, sizeof(join.client_id), "%s", client.client_id);
    ts_transport_send(&client.transport, &join);
    /* 网络读取放到后台线程，UI 主线程保持响应键盘和绘制。 */
    pthread_create(&client.reader, NULL, reader_main, &client);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    timeout(50);
    curs_set(1);
    /* 主循环以较短 timeout 轮询键盘，同时定期处理网络消息并重绘。 */
    while (client.running) {
        drain_messages(&client);
        render(&client);
        wint_t key = 0;
        int rc = get_wch(&key);
        if (rc == ERR) {
            continue;
        }
        handle_key(&client, key, rc);
    }
    endwin();

    close(client.transport.fd);
    ts_transport_destroy(&client.transport);
    ts_client_state_destroy(&client.state);
    return 0;
}
