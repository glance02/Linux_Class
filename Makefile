CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -O2
LDFLAGS ?=
LDLIBS_COMMON = -pthread
LDLIBS_CLIENT = -lncursesw

BUILD_DIR = build
COMMON_OBJS = $(BUILD_DIR)/util.o $(BUILD_DIR)/protocol.o $(BUILD_DIR)/document.o $(BUILD_DIR)/client_state.o $(BUILD_DIR)/autosave.o $(BUILD_DIR)/events.o

.PHONY: all server client test test-bin clean check-ncurses

all: server client test-bin

server: $(BUILD_DIR)/termsync_server

client: check-ncurses $(BUILD_DIR)/termsync_client

test-bin: $(BUILD_DIR)/test_termsync

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

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

test: $(BUILD_DIR)/test_termsync
	./$(BUILD_DIR)/test_termsync

clean:
	rm -rf $(BUILD_DIR)
