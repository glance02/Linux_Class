#include "termsync.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int mkdir_parent_recursive(const char *path)
{
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", path);
    char *slash = strrchr(copy, '/');
    if (slash == NULL) {
        return 0;
    }
    *slash = '\0';
    if (copy[0] == '\0') {
        return 0;
    }
    for (char *p = copy + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0775) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(copy, 0775) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void suffix_path(const char *path, const char *suffix, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s%s", path, suffix);
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (in == NULL) {
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }
    char buffer[8192];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    fclose(in);
    fclose(out);
    return 0;
}

int ts_atomic_write_text(const char *path, const char *content)
{
    char tmp_path[PATH_MAX];
    if (mkdir_parent_recursive(path) != 0) {
        return -1;
    }
    suffix_path(path, ".tmp", tmp_path, sizeof(tmp_path));
    FILE *fp = fopen(tmp_path, "wb");
    if (fp == NULL) {
        return -1;
    }
    const char *text = content ? content : "";
    size_t len = strlen(text);
    if (fwrite(text, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }
    if (fclose(fp) != 0) {
        return -1;
    }
    return rename(tmp_path, path);
}

int ts_write_with_backup(const char *path, const char *content)
{
    char backup_path[PATH_MAX];
    if (mkdir_parent_recursive(path) != 0) {
        return -1;
    }
    if (access(path, F_OK) == 0) {
        suffix_path(path, ".bak", backup_path, sizeof(backup_path));
        if (copy_file(path, backup_path) != 0) {
            return -1;
        }
    }
    return ts_atomic_write_text(path, content);
}

int ts_append_json_log(const char *path, const char *entry_json)
{
    if (mkdir_parent_recursive(path) != 0) {
        return -1;
    }
    FILE *fp = fopen(path, "ab");
    if (fp == NULL) {
        return -1;
    }
    fprintf(fp, "%s\n", entry_json ? entry_json : "{}");
    return fclose(fp);
}

static long monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}

static int read_line_fd(int fd, char *line, size_t line_size)
{
    size_t len = 0;
    while (len + 1 < line_size) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return 0;
        }
        if (ch == '\n') {
            line[len] = '\0';
            return 1;
        }
        line[len++] = ch;
    }
    line[len] = '\0';
    return -1;
}

static char *read_exact_alloc(int fd, size_t len)
{
    char *data = malloc(len + 1);
    if (data == NULL) {
        return NULL;
    }
    size_t used = 0;
    while (used < len) {
        ssize_t n = read(fd, data + used, len - used);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(data);
            return NULL;
        }
        if (n == 0) {
            free(data);
            return NULL;
        }
        used += (size_t)n;
    }
    data[len] = '\0';
    return data;
}

static int send_payload_event(int fd, const char *name, const char *content, int version)
{
    const char *payload = content ? content : "";
    char header[128];
    int header_len = snprintf(header, sizeof(header), "%s %d %zu\n", name, version, strlen(payload));
    if (header_len < 0) {
        return -1;
    }
    if (ts_write_all_fd(fd, header, (size_t)header_len) != 0) {
        return -1;
    }
    return ts_write_all_fd(fd, payload, strlen(payload));
}

int ts_autosave_send_snapshot(int fd, const char *content, int version)
{
    return send_payload_event(fd, "SNAPSHOT", content, version);
}

int ts_autosave_send_save_now(int fd, const char *content, int version)
{
    return send_payload_event(fd, "SAVE_NOW", content, version);
}

int ts_autosave_send_log(int fd, const char *entry_json)
{
    const char *payload = entry_json ? entry_json : "{}";
    char header[128];
    int header_len = snprintf(header, sizeof(header), "LOG 0 %zu\n", strlen(payload));
    if (header_len < 0) {
        return -1;
    }
    if (ts_write_all_fd(fd, header, (size_t)header_len) != 0) {
        return -1;
    }
    return ts_write_all_fd(fd, payload, strlen(payload));
}

int ts_autosave_send_stop(int fd)
{
    return ts_write_all_fd(fd, "STOP\n", 5);
}

static void autosave_loop(int fd, const char *document_path, const char *log_path, long interval_ms)
{
    char autosave_path[PATH_MAX];
    suffix_path(document_path, ".autosave", autosave_path, sizeof(autosave_path));
    char *latest_content = NULL;
    int latest_version = 0;
    long next_save = monotonic_ms() + interval_ms;

    while (true) {
        long now = monotonic_ms();
        long wait_ms = next_save > now ? next_save - now : 0;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval tv;
        tv.tv_sec = wait_ms / 1000;
        tv.tv_usec = (wait_ms % 1000) * 1000;
        int ready = select(fd + 1, &set, NULL, NULL, &tv);
        if (ready > 0 && FD_ISSET(fd, &set)) {
            char header[128];
            int rc = read_line_fd(fd, header, sizeof(header));
            if (rc <= 0) {
                break;
            }
            if (strcmp(header, "STOP") == 0) {
                if (latest_content != NULL) {
                    ts_atomic_write_text(autosave_path, latest_content);
                }
                break;
            }
            char event[32];
            int version = 0;
            size_t len = 0;
            if (sscanf(header, "%31s %d %zu", event, &version, &len) != 3) {
                continue;
            }
            char *payload = read_exact_alloc(fd, len);
            if (payload == NULL) {
                break;
            }
            if (strcmp(event, "SNAPSHOT") == 0) {
                free(latest_content);
                latest_content = payload;
                latest_version = version;
                payload = NULL;
            } else if (strcmp(event, "SAVE_NOW") == 0) {
                free(latest_content);
                latest_content = ts_strdup(payload);
                latest_version = version;
                ts_write_with_backup(document_path, payload);
                char entry[PATH_MAX + 160];
                snprintf(entry, sizeof(entry), "{\"event\":\"manual_save\",\"version\":%d,\"path\":\"%s\"}", version, document_path);
                ts_append_json_log(log_path, entry);
            } else if (strcmp(event, "LOG") == 0) {
                ts_append_json_log(log_path, payload);
            }
            free(payload);
        }

        if (monotonic_ms() >= next_save) {
            if (latest_content != NULL) {
                ts_atomic_write_text(autosave_path, latest_content);
                char entry[PATH_MAX + 160];
                snprintf(entry, sizeof(entry), "{\"event\":\"autosave\",\"version\":%d,\"path\":\"%s\"}", latest_version, autosave_path);
                ts_append_json_log(log_path, entry);
            }
            next_save = monotonic_ms() + interval_ms;
        }
    }
    free(latest_content);
}

int ts_start_autosave_process(
    TsAutosaveProcess *process,
    const char *document_path,
    const char *log_path,
    long interval_ms
)
{
    int fds[2];
    if (pipe(fds) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[1]);
        autosave_loop(fds[0], document_path, log_path, interval_ms <= 0 ? 5000 : interval_ms);
        close(fds[0]);
        _exit(0);
    }
    close(fds[0]);
    process->pid = pid;
    process->write_fd = fds[1];
    return 0;
}

void ts_stop_autosave_process(TsAutosaveProcess *process)
{
    if (process == NULL || process->write_fd < 0) {
        return;
    }
    ts_autosave_send_stop(process->write_fd);
    close(process->write_fd);
    process->write_fd = -1;
    if (process->pid > 0) {
        waitpid(process->pid, NULL, 0);
        process->pid = -1;
    }
}

