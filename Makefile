CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -O2
LDFLAGS ?=
LDLIBS_COMMON = -pthread
LDLIBS_CLIENT = -lncursesw

BUILD_DIR = build
COMMON_OBJS = $(BUILD_DIR)/util.o $(BUILD_DIR)/protocol.o $(BUILD_DIR)/document.o $(BUILD_DIR)/client_state.o $(BUILD_DIR)/autosave.o $(BUILD_DIR)/events.o

.PHONY: all test clean

all: $(BUILD_DIR)/termsync_server $(BUILD_DIR)/termsync_client $(BUILD_DIR)/test_termsync

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

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
