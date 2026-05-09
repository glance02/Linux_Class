CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -O2
LDFLAGS ?=
<<<<<<< HEAD
=======
# 公共模块都依赖 pthread；只有 ncurses 客户端需要额外链接 ncursesw。
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
LDLIBS_COMMON = -pthread
LDLIBS_CLIENT = -lncursesw

BUILD_DIR = build
<<<<<<< HEAD
=======
# server/client/test 共用同一批核心对象文件，入口文件分别单独编译。
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
COMMON_OBJS = $(BUILD_DIR)/util.o $(BUILD_DIR)/protocol.o $(BUILD_DIR)/document.o $(BUILD_DIR)/client_state.o $(BUILD_DIR)/autosave.o $(BUILD_DIR)/events.o

.PHONY: all server client test test-bin clean check-ncurses

<<<<<<< HEAD
=======
# 默认构建服务端、客户端和测试二进制。
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
all: server client test-bin

server: $(BUILD_DIR)/termsync_server

client: check-ncurses $(BUILD_DIR)/termsync_client

test-bin: $(BUILD_DIR)/test_termsync

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

<<<<<<< HEAD
=======
# 编译客户端前先做一个最小 ncursesw 链接检查，缺依赖时给出安装提示。
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
check-ncurses: | $(BUILD_DIR)
	@printf '%s\n' '#include <curses.h>' 'int main(void) { return 0; }' | $(CC) $(CFLAGS) -x c - -o $(BUILD_DIR)/.ncurses_check $(LDLIBS_CLIENT) >/dev/null 2>&1 || { \
		echo "error: missing ncursesw development headers/libraries."; \
		echo "  Ubuntu/Debian/WSL: apt update && apt install -y libncurses-dev"; \
		echo "  CentOS/RHEL/Alibaba Cloud Linux: yum install -y ncurses-devel"; \
		echo "  If this machine only runs the service: make server"; \
		exit 1; \
	}
	@rm -f $(BUILD_DIR)/.ncurses_check

$(BUILD_DIR)/%.o: %.c termsync.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/server.o: server.c termsync.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/client.o: client.c termsync.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_termsync.o: tests/test_termsync.c termsync.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/termsync_server: $(COMMON_OBJS) $(BUILD_DIR)/server.o
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS_COMMON)

$(BUILD_DIR)/termsync_client: $(COMMON_OBJS) $(BUILD_DIR)/client.o
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS_COMMON) $(LDLIBS_CLIENT)

$(BUILD_DIR)/test_termsync: $(COMMON_OBJS) $(BUILD_DIR)/test_termsync.o
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS_COMMON)

<<<<<<< HEAD
=======
# 运行自带 C 测试 runner，覆盖 OT、协议、客户端队列、保存和事件格式化。
>>>>>>> 3554b095a6dd8b680c68b535d0d32507d5098e5e
test: $(BUILD_DIR)/test_termsync
	./$(BUILD_DIR)/test_termsync

clean:
	rm -rf $(BUILD_DIR)
