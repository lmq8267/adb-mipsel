#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "webpage.h"

#ifndef BUILD_VERSION
#define BUILD_VERSION "unknown"
#endif

#define RECV_BUF        (64 * 1024)
#define MAX_UPLOAD      (512 * 1024 * 1024)
#define MAX_JSON_BODY   (16 * 1024)
#define MAX_ARG         256
#define ADB_PATH_MAX    (PATH_MAX + 16)
#define CAPTURE_LIMIT   (256 * 1024)

static int g_port = 9585;
static char g_default_adb[ADB_PATH_MAX] = "./adb";

typedef struct {
    char *data;
    size_t len;
} HttpBody;

typedef struct {
    int fd;
    bool chunked;
    bool broken;
} Stream;

typedef struct {
    unsigned char *apk_data;
    size_t apk_len;
    char filename[256];
    char opts[64];
} UploadResult;


static void logmsg(const char *fmt, ...) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);

    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "%s  ", buf);
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    fflush(stdout);
    va_end(ap);
}

static void http_send(int fd, int code, const char *status,
                      const char *type, const char *body) {
    size_t len = body ? strlen(body) : 0;
    dprintf(fd,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            code, status, type, len);
    if (len) {
        ssize_t ignored = write(fd, body, len);
        (void)ignored;
    }
}

static void http_200_html(int fd, const char *html) {
    http_send(fd, 200, "OK", "text/html; charset=utf-8", html);
}

static void http_200_text(int fd, const char *txt) {
    http_send(fd, 200, "OK", "text/plain; charset=utf-8", txt ? txt : "");
}

static void http_400(int fd, const char *msg) {
    http_send(fd, 400, "Bad Request", "text/plain; charset=utf-8", msg);
}

static void http_404(int fd) {
    http_send(fd, 404, "Not Found", "text/plain; charset=utf-8", "Not Found");
}

static void http_500(int fd, const char *msg) {
    http_send(fd, 500, "Internal Server Error", "text/plain; charset=utf-8", msg);
}

static void http_507(int fd, const char *msg) {
    http_send(fd, 507, "Insufficient Storage", "text/plain; charset=utf-8", msg);
}

static void json_escape(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;
    for (size_t i = 0; src[i] && j + 1 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c == '"' || c == '\\') && j + 2 < dst_sz) {
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c >= 32 && c != 127) {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

static void http_stream_begin(Stream *s, int fd) {
    s->fd = fd;
    s->chunked = true;
    s->broken = false;
    dprintf(fd,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Cache-Control: no-cache\r\n"
            "X-Accel-Buffering: no\r\n"
            "Connection: close\r\n\r\n");
}

static int stream_write(Stream *s, const void *data, size_t len) {
    if (!s || s->broken || len == 0) return s && !s->broken ? 0 : -1;
    if (dprintf(s->fd, "%zx\r\n", len) < 0) {
        s->broken = true;
        return -1;
    }
    if (write(s->fd, data, len) != (ssize_t)len) {
        s->broken = true;
        return -1;
    }
    if (write(s->fd, "\r\n", 2) != 2) {
        s->broken = true;
        return -1;
    }
    return 0;
}

static int stream_printf(Stream *s, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
    return stream_write(s, buf, (size_t)n);
}

static void stream_end(Stream *s) {
    if (!s || s->broken) return;
    dprintf(s->fd, "0\r\n\r\n");
}

static void url_decode(char *dst, const char *src, size_t dst_sz) {
    size_t i = 0, j = 0;
    if (!dst || !src || dst_sz == 0) return;
    while (src[i] && j + 1 < dst_sz) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            unsigned int c = 0;
            sscanf(src + i + 1, "%2x", &c);
            dst[j++] = (char)c;
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static const char *get_query_param(const char *path, const char *key,
                                   char *val, size_t val_sz) {
    const char *q;
    size_t klen;
    if (!path || !key || !val || val_sz == 0) return NULL;
    val[0] = '\0';
    q = strchr(path, '?');
    if (!q) return NULL;
    q++;
    klen = strlen(key);
    while (*q) {
        if (*q == '&') q++;
        if (strncmp(q, key, klen) == 0 && q[klen] == '=') {
            const char *vstart = q + klen + 1;
            const char *vend = strchr(vstart, '&');
            size_t vlen = vend ? (size_t)(vend - vstart) : strlen(vstart);
            char tmp[1024];
            if (vlen >= sizeof(tmp)) vlen = sizeof(tmp) - 1;
            memcpy(tmp, vstart, vlen);
            tmp[vlen] = '\0';
            url_decode(val, tmp, val_sz);
            return val;
        }
        q = strchr(q, '&');
        if (!q) break;
    }
    return NULL;
}

static void path_only(const char *full, char *out, size_t sz) {
    const char *q = strchr(full, '?');
    size_t len = q ? (size_t)(q - full) : strlen(full);
    if (len >= sz) len = sz - 1;
    memcpy(out, full, len);
    out[len] = '\0';
}

static const char *get_header(const char *buf, const char *name,
                              char *out, size_t out_sz) {
    size_t nlen;
    const char *p;
    if (!buf || !name || !out || out_sz == 0) return NULL;
    out[0] = '\0';
    nlen = strlen(name);
    p = buf;
    while ((p = strcasestr(p, name)) != NULL) {
        if ((p == buf || p[-1] == '\n') && strncasecmp(p, name, nlen) == 0 &&
            p[nlen] == ':') {
            const char *v = p + nlen + 1;
            size_t i = 0;
            while (*v == ' ' || *v == '\t') v++;
            while (*v && *v != '\r' && *v != '\n' && i + 1 < out_sz)
                out[i++] = *v++;
            out[i] = '\0';
            return out;
        }
        p++;
    }
    return NULL;
}

static void json_get_str(const char *body, const char *key,
                         char *out, size_t out_sz) {
    char pattern[128];
    const char *p;
    if (!body || !key || !out || out_sz == 0) return;
    out[0] = '\0';
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(body, pattern);
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '"') {
        size_t i = 0;
        p++;
        while (*p && *p != '"' && i + 1 < out_sz) {
            if (*p == '\\' && p[1]) p++;
            out[i++] = *p++;
        }
        out[i] = '\0';
    } else {
        size_t i = 0;
        while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p) &&
               i + 1 < out_sz)
            out[i++] = *p++;
        out[i] = '\0';
    }
}

static bool valid_adb_path(const char *s) {
    if (!s || !s[0] || strlen(s) >= ADB_PATH_MAX) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 32 || *p == 127) return false;
    }
    return true;
}

static bool valid_host(const char *s) {
    if (!s || !s[0] || strlen(s) >= MAX_ARG) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!(isalnum(*p) || *p == '.' || *p == '-' || *p == '_' || *p == ':'))
            return false;
    }
    return true;
}

static bool valid_port(const char *s) {
    long n;
    char *end = NULL;
    if (!s || !s[0]) return false;
    n = strtol(s, &end, 10);
    return *end == '\0' && n > 0 && n <= 65535;
}

static bool valid_serial(const char *s) {
    if (!s || !s[0] || strlen(s) >= MAX_ARG) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!(isalnum(*p) || *p == '.' || *p == '-' || *p == '_' || *p == ':' ||
              *p == '/'))
            return false;
    }
    return true;
}

static bool valid_package(const char *s) {
    if (!s || !s[0]) return true;
    if (strlen(s) >= MAX_ARG) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!(isalnum(*p) || *p == '.' || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static void choose_adb_from_json(const char *body, char *adb, size_t adb_sz) {
    json_get_str(body, "adb", adb, adb_sz);
    if (!adb[0]) snprintf(adb, adb_sz, "%s", g_default_adb);
}

static void choose_adb_from_query(const char *path, char *adb, size_t adb_sz) {
    if (!get_query_param(path, "adb", adb, adb_sz) || !adb[0])
        snprintf(adb, adb_sz, "%s", g_default_adb);
}

static void exec_adb_child(const char *adb, char *const argv[]) {
    if (strchr(adb, '/')) execv(adb, argv);
    else execvp(adb, argv);
    fprintf(stderr, "exec adb failed: %s: %s\n", adb, strerror(errno));
    _exit(127);
}

static void set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int run_process_stream(char *const argv[], Stream *stream,
                              const char *filter) {
    int pipefd[2];
    pid_t pid;
    int status = 0;
    char buf[4096];
    char line[8192];
    size_t line_len = 0;

    if (pipe(pipefd) < 0) {
        stream_printf(stream, "pipe failed: %s\n", strerror(errno));
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        stream_printf(stream, "fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        exec_adb_child(argv[0], argv);
    }

    close(pipefd[1]);
    while (1) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (filter && filter[0]) {
            for (ssize_t i = 0; i < n; i++) {
                if (line_len + 1 < sizeof(line)) line[line_len++] = buf[i];
                if (buf[i] == '\n' || line_len + 1 >= sizeof(line)) {
                    line[line_len] = '\0';
                    if (strstr(line, filter)) {
                        if (stream_write(stream, line, line_len) < 0) {
                            kill(pid, SIGTERM);
                            close(pipefd[0]);
                            waitpid(pid, &status, 0);
                            return -1;
                        }
                    }
                    line_len = 0;
                }
            }
        } else if (stream_write(stream, buf, (size_t)n) < 0) {
            kill(pid, SIGTERM);
            close(pipefd[0]);
            waitpid(pid, &status, 0);
            return -1;
        }
    }

    if (filter && filter[0] && line_len > 0) {
        line[line_len] = '\0';
        if (strstr(line, filter)) stream_write(stream, line, line_len);
    }

    close(pipefd[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static int run_process_capture(char *const argv[], char *out, size_t out_sz) {
    int pipefd[2];
    pid_t pid;
    int status = 0;
    size_t pos = 0;
    char buf[4096];

    if (!out || out_sz == 0) return -1;
    out[0] = '\0';
    if (pipe(pipefd) < 0) return -1;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        exec_adb_child(argv[0], argv);
    }

    close(pipefd[1]);
    while (1) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pos + (size_t)n >= out_sz) n = (ssize_t)(out_sz - pos - 1);
        if (n > 0) {
            memcpy(out + pos, buf, (size_t)n);
            pos += (size_t)n;
            out[pos] = '\0';
        }
        if (pos + 1 >= out_sz) break;
    }
    close(pipefd[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static void upload_free(UploadResult *u) {
    if (u && u->apk_data) free(u->apk_data);
    if (u) memset(u, 0, sizeof(*u));
}

static const char *memfind(const char *hay, size_t haylen,
                           const char *needle, size_t needle_len) {
    if (needle_len == 0) return hay;
    if (haylen < needle_len) return NULL;
    for (size_t i = 0; i <= haylen - needle_len; i++)
        if (memcmp(hay + i, needle, needle_len) == 0) return hay + i;
    return NULL;
}

static int parse_multipart(const char *body, size_t body_len,
                           const char *boundary, UploadResult *result) {
    char delim[256];
    size_t dlen;
    const char *p;
    const char *end;

    memset(result, 0, sizeof(*result));
    if (!body || !boundary || !boundary[0]) return -1;
    snprintf(delim, sizeof(delim), "--%s", boundary);
    dlen = strlen(delim);
    p = body;
    end = body + body_len;

    while (p < end) {
        const char *part = memfind(p, (size_t)(end - p), delim, dlen);
        const char *next;
        const char *hdr_end;
        const char *part_body;
        size_t part_len;
        if (!part) break;
        p = part + dlen;
        if (p + 2 <= end && p[0] == '-' && p[1] == '-') break;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;

        next = memfind(p, (size_t)(end - p), delim, dlen);
        if (!next) break;
        hdr_end = memfind(p, (size_t)(next - p), "\r\n\r\n", 4);
        if (!hdr_end) {
            p = next;
            continue;
        }
        part_body = hdr_end + 4;
        part_len = (size_t)(next - part_body);
        if (part_len >= 2 && part_body[part_len - 2] == '\r' &&
            part_body[part_len - 1] == '\n')
            part_len -= 2;

        if (memfind(p, (size_t)(hdr_end - p), "name=\"apk\"", 10)) {
            const char *fn = memfind(p, (size_t)(hdr_end - p), "filename=\"", 10);
            result->apk_data = malloc(part_len);
            if (!result->apk_data) return -1;
            memcpy(result->apk_data, part_body, part_len);
            result->apk_len = part_len;
            if (fn) {
                size_t i = 0;
                fn += 10;
                while (fn < hdr_end && *fn != '"' && i + 1 < sizeof(result->filename))
                    result->filename[i++] = *fn++;
                result->filename[i] = '\0';
            }
        } else if (memfind(p, (size_t)(hdr_end - p), "name=\"opts\"", 11)) {
            size_t n = part_len < sizeof(result->opts) - 1 ?
                       part_len : sizeof(result->opts) - 1;
            memcpy(result->opts, part_body, n);
            result->opts[n] = '\0';
        }
        p = next;
    }

    return result->apk_data && result->apk_len > 0 ? 0 : -1;
}

static int save_upload_to_tmp(const UploadResult *u, char *path, size_t path_sz) {
    char tmpl[] = "/tmp/adb-web_apk_XXXXXX";
    int fd = mkstemp(tmpl);
    size_t pos = 0;
    if (fd < 0) return -1;
    while (pos < u->apk_len) {
        ssize_t n = write(fd, u->apk_data + pos, u->apk_len - pos);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmpl);
            return -1;
        }
        pos += (size_t)n;
    }
    close(fd);
    snprintf(path, path_sz, "%s", tmpl);
    return 0;
}

static void handle_api_check(int fd) {
    char body[ADB_PATH_MAX + 128];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"mode\":\"external-adb\",\"default_adb\":\"%s\"}",
             g_default_adb);
    http_send(fd, 200, "OK", "application/json; charset=utf-8", body);
}

static bool token_is_adb_port(const char *local) {
    const char *colon = strrchr(local, ':');
    if (!colon) return false;
    return strcasecmp(colon + 1, "13AD") == 0;  // 5037
}

static bool find_adb_server_inode(char *inode, size_t inode_sz) {
    const char *files[] = {"/proc/net/tcp", "/proc/net/tcp6"};
    char line[512];
    for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
        FILE *fp = fopen(files[f], "r");
        if (!fp) continue;
        while (fgets(line, sizeof(line), fp)) {
            char *tok[12];
            int ntok = 0;
            char *save = NULL;
            char *p = strtok_r(line, " \t\r\n", &save);
            while (p && ntok < (int)(sizeof(tok) / sizeof(tok[0]))) {
                tok[ntok++] = p;
                p = strtok_r(NULL, " \t\r\n", &save);
            }
            if (ntok > 9 && token_is_adb_port(tok[1]) &&
                strcmp(tok[3], "0A") == 0 && tok[9][0]) {
                snprintf(inode, inode_sz, "%s", tok[9]);
                fclose(fp);
                return true;
            }
        }
        fclose(fp);
    }
    return false;
}

static bool proc_fd_has_inode(const char *pid, const char *inode) {
    char fd_dir[PATH_MAX], target[128], expect[96];
    DIR *dir;
    struct dirent *de;
    snprintf(fd_dir, sizeof(fd_dir), "/proc/%s/fd", pid);
    snprintf(expect, sizeof(expect), "socket:[%s]", inode);
    dir = opendir(fd_dir);
    if (!dir) return false;
    while ((de = readdir(dir)) != NULL) {
        char fd_path[PATH_MAX];
        ssize_t n;
        if (de->d_name[0] == '.') continue;
        if (snprintf(fd_path, sizeof(fd_path), "%s/%s", fd_dir, de->d_name) >=
            (int)sizeof(fd_path))
            continue;
        n = readlink(fd_path, target, sizeof(target) - 1);
        if (n < 0) continue;
        target[n] = '\0';
        if (strcmp(target, expect) == 0) {
            closedir(dir);
            return true;
        }
    }
    closedir(dir);
    return false;
}

static void read_proc_cmd(const char *pid, char *cmd, size_t cmd_sz) {
    char path[PATH_MAX];
    FILE *fp;
    size_t n;
    if (!cmd || cmd_sz == 0) return;
    cmd[0] = '\0';
    snprintf(path, sizeof(path), "/proc/%s/cmdline", pid);
    fp = fopen(path, "rb");
    if (fp) {
        n = fread(cmd, 1, cmd_sz - 1, fp);
        fclose(fp);
        cmd[n] = '\0';
        for (size_t i = 0; i < n; i++) {
            if (cmd[i] == '\0') cmd[i] = ' ';
        }
        while (n > 0 && isspace((unsigned char)cmd[n - 1])) cmd[--n] = '\0';
        if (cmd[0]) return;
    }
    snprintf(path, sizeof(path), "/proc/%s/comm", pid);
    fp = fopen(path, "r");
    if (fp) {
        if (fgets(cmd, cmd_sz, fp)) {
            size_t len = strlen(cmd);
            while (len > 0 && isspace((unsigned char)cmd[len - 1]))
                cmd[--len] = '\0';
        }
        fclose(fp);
    }
}

static bool find_adb_server_process(char *pid, size_t pid_sz,
                                    char *cmd, size_t cmd_sz) {
    char inode[64];
    DIR *proc;
    struct dirent *de;
    if (!find_adb_server_inode(inode, sizeof(inode))) return false;
    proc = opendir("/proc");
    if (!proc) return false;
    while ((de = readdir(proc)) != NULL) {
        bool numeric = de->d_name[0] != '\0';
        for (const char *p = de->d_name; *p; p++) {
            if (!isdigit((unsigned char)*p)) {
                numeric = false;
                break;
            }
        }
        if (!numeric) continue;
        if (proc_fd_has_inode(de->d_name, inode)) {
            size_t len = strlen(de->d_name);
            if (len >= pid_sz) continue;
            memcpy(pid, de->d_name, len + 1);
            read_proc_cmd(de->d_name, cmd, cmd_sz);
            closedir(proc);
            return true;
        }
    }
    closedir(proc);
    return false;
}

static void handle_api_adb_status(int fd, const char *path) {
    char adb[ADB_PATH_MAX], pid[32] = "", cmd[256] = "", esc_cmd[512], esc_adb[ADB_PATH_MAX * 2];
    char body[10000];
    bool running;
    choose_adb_from_query(path, adb, sizeof(adb));
    if (!valid_adb_path(adb)) {
        http_400(fd, "adb 路径无效");
        return;
    }
    running = find_adb_server_process(pid, sizeof(pid), cmd, sizeof(cmd));
    json_escape(esc_cmd, sizeof(esc_cmd), cmd);
    json_escape(esc_adb, sizeof(esc_adb), adb);
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"adb\":\"%s\",\"running\":%s,\"pid\":\"%s\","
             "\"cmd\":\"%s\",\"port\":5037}",
             esc_adb, running ? "true" : "false", running ? pid : "", esc_cmd);
    http_send(fd, 200, "OK", "application/json; charset=utf-8", body);
}

static void handle_api_adb_stop(int fd, const char *body) {
    char adb[ADB_PATH_MAX], out[4096], esc_out[8192];
    char resp[9000];
    char *argv[3];
    int rc;
    choose_adb_from_json(body, adb, sizeof(adb));
    if (!valid_adb_path(adb)) {
        http_400(fd, "adb 路径无效");
        return;
    }
    argv[0] = adb;
    argv[1] = "kill-server";
    argv[2] = NULL;
    rc = run_process_capture(argv, out, sizeof(out));
    json_escape(esc_out, sizeof(esc_out), out);
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"exit\":%d,\"output\":\"%s\"}", rc, esc_out);
    http_send(fd, 200, "OK", "application/json; charset=utf-8", resp);
}

static void handle_api_devices(int fd, const char *path) {
    char adb[ADB_PATH_MAX];
    char *argv[4];
    char out[CAPTURE_LIMIT];
    choose_adb_from_query(path, adb, sizeof(adb));
    if (!valid_adb_path(adb)) {
        http_400(fd, "adb 路径无效");
        return;
    }
    argv[0] = adb;
    argv[1] = "devices";
    argv[2] = "-l";
    argv[3] = NULL;
    run_process_capture(argv, out, sizeof(out));
    http_200_text(fd, out);
}

static void handle_api_tmp_space(int fd) {
    struct statvfs st;
    char body[256];
    if (statvfs("/tmp", &st) != 0) {
        http_500(fd, "无法读取 /tmp 可用空间");
        return;
    }
    unsigned long long free_bytes =
        (unsigned long long)st.f_bavail * (unsigned long long)st.f_frsize;
    unsigned long long total_bytes =
        (unsigned long long)st.f_blocks * (unsigned long long)st.f_frsize;
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"path\":\"/tmp\",\"free\":%llu,\"total\":%llu}",
             free_bytes, total_bytes);
    http_send(fd, 200, "OK", "application/json; charset=utf-8", body);
}

static void handle_api_pair(int fd, const char *body) {
    char adb[ADB_PATH_MAX], ip[MAX_ARG], port[32], code[64], target[MAX_ARG + 40];
    char *argv[5];
    Stream st;
    choose_adb_from_json(body, adb, sizeof(adb));
    json_get_str(body, "ip", ip, sizeof(ip));
    json_get_str(body, "port", port, sizeof(port));
    json_get_str(body, "code", code, sizeof(code));
    if (!valid_adb_path(adb) || !valid_host(ip) || !valid_port(port) || !code[0]) {
        http_400(fd, "参数无效");
        return;
    }
    snprintf(target, sizeof(target), "%s:%s", ip, port);
    argv[0] = adb;
    argv[1] = "pair";
    argv[2] = target;
    argv[3] = code;
    argv[4] = NULL;
    logmsg("adb pair %s", target);
    http_stream_begin(&st, fd);
    stream_printf(&st, "$ %s pair %s ******\n", adb, target);
    int rc = run_process_stream(argv, &st, NULL);
    stream_printf(&st, "\n[exit:%d]\n", rc);
    stream_end(&st);
}

static void handle_api_connect(int fd, const char *body) {
    char adb[ADB_PATH_MAX], ip[MAX_ARG], port[32], target[MAX_ARG + 40];
    char *argv[4];
    Stream st;
    choose_adb_from_json(body, adb, sizeof(adb));
    json_get_str(body, "ip", ip, sizeof(ip));
    json_get_str(body, "port", port, sizeof(port));
    if (!port[0]) snprintf(port, sizeof(port), "5555");
    if (!valid_adb_path(adb) || !valid_host(ip) || !valid_port(port)) {
        http_400(fd, "参数无效");
        return;
    }
    snprintf(target, sizeof(target), "%s:%s", ip, port);
    argv[0] = adb;
    argv[1] = "connect";
    argv[2] = target;
    argv[3] = NULL;
    logmsg("adb connect %s", target);
    http_stream_begin(&st, fd);
    stream_printf(&st, "$ %s connect %s\n", adb, target);
    int rc = run_process_stream(argv, &st, NULL);
    stream_printf(&st, "\n[exit:%d]\n", rc);
    stream_end(&st);
}

static void handle_api_install(int fd, const char *req, const char *body,
                               size_t blen) {
    char method[8], path[1024], adb[ADB_PATH_MAX], device[MAX_ARG], ct[256], boundary[160];
    char tmp_path[256] = {0};
    char *argv[9];
    int argc = 0;
    int rc = -1;
    UploadResult upload;
    Stream st;
    bool tmp_saved = false;

    sscanf(req, "%7s %1023s", method, path);
    choose_adb_from_query(path, adb, sizeof(adb));
    get_query_param(path, "device", device, sizeof(device));
    if (!valid_adb_path(adb) || !valid_serial(device)) {
        http_400(fd, "adb 路径或 device 参数无效");
        return;
    }

    if (!get_header(req, "Content-Type", ct, sizeof(ct))) {
        http_400(fd, "需要 multipart/form-data");
        return;
    }
    char *b = strstr(ct, "boundary=");
    if (!b) {
        http_400(fd, "缺少 multipart boundary");
        return;
    }
    b += 9;
    if (*b == '"') b++;
    size_t i = 0;
    while (*b && *b != '"' && *b != ';' && !isspace((unsigned char)*b) &&
           i + 1 < sizeof(boundary))
        boundary[i++] = *b++;
    boundary[i] = '\0';

    if (parse_multipart(body, blen, boundary, &upload) != 0) {
        http_400(fd, "无法解析上传 APK");
        return;
    }

    struct statvfs tmpfs;
    if (statvfs("/tmp", &tmpfs) == 0) {
        unsigned long long free_bytes =
            (unsigned long long)tmpfs.f_bavail * (unsigned long long)tmpfs.f_frsize;
        if (free_bytes < (unsigned long long)upload.apk_len) {
            upload_free(&upload);
            http_507(fd, "/tmp 可用空间不足，APK 未保存");
            return;
        }
    }

    if (save_upload_to_tmp(&upload, tmp_path, sizeof(tmp_path)) != 0) {
        upload_free(&upload);
        http_500(fd, "保存 APK 到 /tmp 失败");
        return;
    }
    tmp_saved = true;

    argv[argc++] = adb;
    argv[argc++] = "-s";
    argv[argc++] = device;
    argv[argc++] = "install";
    if (strstr(upload.opts, "-r")) argv[argc++] = "-r";
    if (strstr(upload.opts, "-d")) argv[argc++] = "-d";
    argv[argc++] = tmp_path;
    argv[argc] = NULL;

    logmsg("adb install %s -> %s", upload.filename[0] ? upload.filename : "APK",
           device);
    http_stream_begin(&st, fd);
    stream_printf(&st, "APK 已完整保存到 /tmp: %s (%zu bytes)\n",
                  upload.filename[0] ? upload.filename : "APK", upload.apk_len);
    stream_printf(&st, "开始安装...\n");
    stream_printf(&st, "$ %s -s %s install %s%s%s\n", adb, device,
                  strstr(upload.opts, "-r") ? "-r " : "",
                  strstr(upload.opts, "-d") ? "-d " : "", tmp_path);
    rc = run_process_stream(argv, &st, NULL);
    stream_printf(&st, "\n[exit:%d]\n", rc);
    stream_end(&st);

    if (tmp_saved) unlink(tmp_path);
    upload_free(&upload);
}

static void trim_ascii(char *s) {
    size_t len;
    while (*s && isspace((unsigned char)*s)) memmove(s, s + 1, strlen(s));
    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

static bool first_pid(char *out, char *pid, size_t pid_sz) {
    char *p = out;
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) return false;
    size_t i = 0;
    while (isdigit((unsigned char)*p) && i + 1 < pid_sz) pid[i++] = *p++;
    pid[i] = '\0';
    return i > 0;
}

static void handle_api_logcat(int fd, const char *path) {
    char adb[ADB_PATH_MAX], device[MAX_ARG], package[MAX_ARG], filter[MAX_ARG];
    char pid[32] = {0};
    char pid_out[4096];
    char *argv[10];
    int argc = 0;
    Stream st;

    choose_adb_from_query(path, adb, sizeof(adb));
    get_query_param(path, "device", device, sizeof(device));
    get_query_param(path, "package", package, sizeof(package));
    get_query_param(path, "filter", filter, sizeof(filter));
    trim_ascii(package);
    trim_ascii(filter);

    if (!valid_adb_path(adb) || !valid_serial(device) || !valid_package(package)) {
        http_400(fd, "参数无效");
        return;
    }

    http_stream_begin(&st, fd);
    if (package[0]) {
        char *pid_argv[7];
        pid_argv[0] = adb;
        pid_argv[1] = "-s";
        pid_argv[2] = device;
        pid_argv[3] = "shell";
        pid_argv[4] = "pidof";
        pid_argv[5] = package;
        pid_argv[6] = NULL;
        run_process_capture(pid_argv, pid_out, sizeof(pid_out));
        if (first_pid(pid_out, pid, sizeof(pid))) {
            stream_printf(&st, "已定位应用 %s PID: %s\n", package, pid);
        } else {
            stream_printf(&st,
                          "未找到运行中的应用进程: %s，将抓取全量 logcat%s。\n",
                          package, filter[0] ? " 并按关键词过滤" : "");
        }
    }

    argv[argc++] = adb;
    argv[argc++] = "-s";
    argv[argc++] = device;
    argv[argc++] = "logcat";
    argv[argc++] = "-v";
    argv[argc++] = "time";
    if (pid[0]) {
        argv[argc++] = "--pid";
        argv[argc++] = pid;
    }
    argv[argc] = NULL;

    stream_printf(&st, "$ %s -s %s logcat -v time%s%s\n", adb, device,
                  pid[0] ? " --pid " : "", pid[0] ? pid : "");
    int rc = run_process_stream(argv, &st, filter[0] ? filter : NULL);
    stream_printf(&st, "\n[exit:%d]\n", rc);
    stream_end(&st);
}

static int read_http_request(int cfd, char **req_out, size_t *req_len_out,
                             HttpBody *body_out) {
    char *req = malloc(RECV_BUF + 1);
    size_t cap = RECV_BUF;
    size_t total = 0;
    char *hdr_end;
    size_t header_len;
    size_t content_len = 0;
    char cl[64];

    if (!req) return -1;
    memset(body_out, 0, sizeof(*body_out));

    while (1) {
        ssize_t n;
        if (total == cap) {
            free(req);
            return -1;
        }
        n = recv(cfd, req + total, cap - total, 0);
        if (n <= 0) {
            free(req);
            return -1;
        }
        total += (size_t)n;
        req[total] = '\0';
        hdr_end = strstr(req, "\r\n\r\n");
        if (hdr_end) break;
    }

    header_len = (size_t)(hdr_end + 4 - req);
    if (get_header(req, "Content-Length", cl, sizeof(cl))) {
        content_len = strtoul(cl, NULL, 10);
        if (content_len > MAX_UPLOAD) {
            free(req);
            return -2;
        }
    }

    if (content_len > 0) {
        size_t have = total > header_len ? total - header_len : 0;
        char *body = malloc(content_len + 1);
        size_t pos = 0;
        if (!body) {
            free(req);
            return -1;
        }
        if (have > content_len) have = content_len;
        if (have > 0) {
            memcpy(body, req + header_len, have);
            pos = have;
        }
        while (pos < content_len) {
            ssize_t n = recv(cfd, body + pos, content_len - pos, 0);
            if (n <= 0) {
                free(body);
                free(req);
                return -1;
            }
            pos += (size_t)n;
        }
        body[content_len] = '\0';
        body_out->data = body;
        body_out->len = content_len;
    }

    req[header_len] = '\0';
    *req_out = req;
    *req_len_out = header_len;
    return 0;
}

static void handle_client(int cfd) {
    char *req = NULL;
    size_t req_len = 0;
    HttpBody body;
    char method[8] = {0}, path[1024] = {0}, route[1024] = {0};
    int rr;

    rr = read_http_request(cfd, &req, &req_len, &body);
    if (rr == -2) {
        http_400(cfd, "上传文件过大");
        close(cfd);
        return;
    }
    if (rr != 0) {
        close(cfd);
        return;
    }
    (void)req_len;
    sscanf(req, "%7s %1023s", method, path);
    path_only(path, route, sizeof(route));

    if (strcmp(method, "GET") == 0 && strcmp(route, "/") == 0) {
        http_200_html(cfd, webpage_html);
    } else if (strcmp(method, "GET") == 0 && strcmp(route, "/api/check") == 0) {
        handle_api_check(cfd);
    } else if (strcmp(method, "GET") == 0 && strcmp(route, "/api/adb_status") == 0) {
        handle_api_adb_status(cfd, path);
    } else if (strcmp(method, "GET") == 0 && strcmp(route, "/api/tmp_space") == 0) {
        handle_api_tmp_space(cfd);
    } else if (strcmp(method, "GET") == 0 && strcmp(route, "/api/devices") == 0) {
        handle_api_devices(cfd, path);
    } else if (strcmp(method, "GET") == 0 && strcmp(route, "/api/logcat") == 0) {
        handle_api_logcat(cfd, path);
    } else if (strcmp(method, "POST") == 0 && strcmp(route, "/api/pair") == 0) {
        if (!body.data || body.len > MAX_JSON_BODY) http_400(cfd, "请求体无效");
        else handle_api_pair(cfd, body.data);
    } else if (strcmp(method, "POST") == 0 && strcmp(route, "/api/connect") == 0) {
        if (!body.data || body.len > MAX_JSON_BODY) http_400(cfd, "请求体无效");
        else handle_api_connect(cfd, body.data);
    } else if (strcmp(method, "POST") == 0 && strcmp(route, "/api/adb_stop") == 0) {
        if (!body.data || body.len > MAX_JSON_BODY) http_400(cfd, "请求体无效");
        else handle_api_adb_stop(cfd, body.data);
    } else if (strcmp(method, "POST") == 0 && strcmp(route, "/api/install") == 0) {
        if (!body.data) http_400(cfd, "请求体无效");
        else handle_api_install(cfd, req, body.data, body.len);
    } else {
        http_404(cfd);
    }

    free(body.data);
    free(req);
    close(cfd);
}

static void *client_thread(void *arg) {
    int cfd = (int)(intptr_t)arg;
    handle_client(cfd);
    return NULL;
}

static void resolve_default_adb_from_prog(const char *prog) {
    char buf[PATH_MAX];
    if (prog && prog[0] && realpath(prog, buf) && buf[0]) {
        char *slash = strrchr(buf, '/');
        if (slash) {
            size_t dirlen = (size_t)(slash - buf + 1);  // 包含尾部 /
            char candidate[ADB_PATH_MAX];
            memcpy(g_default_adb, buf, dirlen);
            memcpy(g_default_adb + dirlen, "adb", 4);   // 含 \0
            snprintf(candidate, sizeof(candidate), "%s", g_default_adb);
            if (access(candidate, X_OK) == 0) return;
        }
    }
    snprintf(g_default_adb, sizeof(g_default_adb), "adb");
}

static void print_help(const char *prog) {
    printf("\n用法: %s [-p 端口] [-a adb路径] [-v] [-h]\n\n", prog);
    printf("  -p 端口     指定监听端口，默认 %d\n", g_port);
    printf("  -a adb路径  指定默认 adb 路径，默认 %s\n", g_default_adb);
    printf("  -v          显示版本\n");
    printf("  -h          显示帮助\n\n");
    printf("说明: adb-web 只提供 Web 控制界面，实际操作全部调用外部 adb。\n");
}

int main(int argc, char **argv) {
    int opt;
    int sfd = -1;
    int on = 1;
    pthread_attr_t attr;

    signal(SIGPIPE, SIG_IGN);

    // 默认 adb 路径优先取程序所在目录下的 adb
    resolve_default_adb_from_prog(argv[0]);

    while ((opt = getopt(argc, argv, "p:a:vh")) != -1) {
        switch (opt) {
            case 'p':
                g_port = atoi(optarg);
                break;
            case 'a':
                snprintf(g_default_adb, sizeof(g_default_adb), "%s", optarg);
                break;
            case 'v':
                printf("%s\n", BUILD_VERSION);
                return 0;
            case 'h':
                print_help(argv[0]);
                return 0;
            default:
                print_help(argv[0]);
                return 1;
        }
    }

    sfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (sfd >= 0) {
        struct sockaddr_in6 addr6;
        set_cloexec(sfd);
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        int v6only = 0;
        setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons((uint16_t)g_port);
        addr6.sin6_addr = in6addr_any;
        if (bind(sfd, (struct sockaddr *)&addr6, sizeof(addr6)) < 0 ||
            listen(sfd, 32) < 0) {
            close(sfd);
            sfd = -1;
        }
    }

    if (sfd < 0) {
        struct sockaddr_in addr4;
        sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd < 0) {
            perror("socket");
            return 1;
        }
        set_cloexec(sfd);
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons((uint16_t)g_port);
        addr4.sin_addr.s_addr = INADDR_ANY;
        if (bind(sfd, (struct sockaddr *)&addr4, sizeof(addr4)) < 0 ||
            listen(sfd, 32) < 0) {
            perror("listen");
            close(sfd);
            return 1;
        }
    }

    logmsg("ADB Web Tool v%s", BUILD_VERSION);
    logmsg("默认 adb: %s", g_default_adb);
    logmsg("监听端口: %d", g_port);

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    while (1) {
        struct sockaddr_storage cli;
        socklen_t cl = sizeof(cli);
        int cfd = accept(sfd, (struct sockaddr *)&cli, &cl);
        pthread_t tid;
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        if (pthread_create(&tid, &attr, client_thread, (void *)(intptr_t)cfd) != 0) {
            close(cfd);
        }
    }

    pthread_attr_destroy(&attr);
    close(sfd);
    return 0;
}
