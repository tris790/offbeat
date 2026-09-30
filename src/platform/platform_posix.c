/*
 * POSIX part of the Linux platform layer: time, threads, sync, files,
 * directories, well-known paths and process memory info.
 */

#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(pthread_mutex_t) <= sizeof(Platform_Mutex), "Platform_Mutex too small");
_Static_assert(sizeof(pthread_cond_t)  <= sizeof(Platform_Cond),  "Platform_Cond too small");

/* ---- time ---- */

f64 platform_time_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}

void platform_sleep(f64 seconds) {
    if (seconds <= 0) return;
    struct timespec ts;
    ts.tv_sec  = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (f64)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

/* ---- threads ---- */

struct Platform_Thread {
    pthread_t           handle;
    Platform_ThreadProc proc;
    void               *user;
    char                name[16];
};

static void *thread_trampoline(void *arg) {
    Platform_Thread *t = arg;
    if (t->name[0]) pthread_setname_np(pthread_self(), t->name);
    t->proc(t->user);
    return 0;
}

Platform_Thread *platform_thread_start(Platform_ThreadProc proc, void *user, const char *name) {
    Platform_Thread *t = calloc(1, sizeof(*t));
    if (!t) return 0;
    t->proc = proc;
    t->user = user;
    if (name) snprintf(t->name, sizeof(t->name), "%s", name);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024); /* keep RSS small */
    int rc = pthread_create(&t->handle, &attr, thread_trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(t); return 0; }
    return t;
}

void platform_thread_join(Platform_Thread *t) {
    if (!t) return;
    pthread_join(t->handle, 0);
    free(t);
}

void platform_thread_set_background(void) {
    /* Per-thread nice value on Linux (setpriority on the tid). */
    pid_t tid = (pid_t)syscall(SYS_gettid);
    setpriority(PRIO_PROCESS, (id_t)tid, 10);
}

u32 platform_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (u32)n : 1;
}

void platform_mutex_init(Platform_Mutex *m)   { pthread_mutex_init((pthread_mutex_t *)m->opaque, 0); }
void platform_mutex_lock(Platform_Mutex *m)   { pthread_mutex_lock((pthread_mutex_t *)m->opaque); }
void platform_mutex_unlock(Platform_Mutex *m) { pthread_mutex_unlock((pthread_mutex_t *)m->opaque); }

void platform_cond_init(Platform_Cond *c) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init((pthread_cond_t *)c->opaque, &attr);
    pthread_condattr_destroy(&attr);
}

void platform_cond_wait(Platform_Cond *c, Platform_Mutex *m) {
    pthread_cond_wait((pthread_cond_t *)c->opaque, (pthread_mutex_t *)m->opaque);
}

b32 platform_cond_wait_timeout(Platform_Cond *c, Platform_Mutex *m, f64 seconds) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    f64 whole = (f64)(s64)seconds;
    ts.tv_sec  += (time_t)whole;
    ts.tv_nsec += (long)((seconds - whole) * 1e9);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait((pthread_cond_t *)c->opaque,
                                  (pthread_mutex_t *)m->opaque, &ts) == 0;
}

void platform_cond_signal(Platform_Cond *c)    { pthread_cond_signal((pthread_cond_t *)c->opaque); }
void platform_cond_broadcast(Platform_Cond *c) { pthread_cond_broadcast((pthread_cond_t *)c->opaque); }

/* ---- files ---- */

Platform_File platform_file_open_read(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    return (Platform_File){ .handle = fd };
}

b32 platform_file_valid(Platform_File f) { return f.handle >= 0; }

u64 platform_file_size(Platform_File f) {
    struct stat st;
    if (f.handle < 0 || fstat((int)f.handle, &st) != 0) return 0;
    return (u64)st.st_size;
}

u64 platform_file_read_at(Platform_File f, u64 offset, void *dst, u64 size) {
    if (f.handle < 0) return 0;
    u64 done = 0;
    while (done < size) {
        ssize_t n = pread((int)f.handle, (u8 *)dst + done, size - done, (off_t)(offset + done));
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        done += (u64)n;
    }
    return done;
}

void platform_file_close(Platform_File f) {
    if (f.handle >= 0) close((int)f.handle);
}

Core_String platform_file_read_all(Core_Arena *arena, const char *path) {
    Platform_File f = platform_file_open_read(path);
    if (!platform_file_valid(f)) return (Core_String){0};
    u64 size = platform_file_size(f);
    u8 *data = core_arena_push(arena, size + 1, 16);
    if (!data) { platform_file_close(f); return (Core_String){0}; }
    u64 got = platform_file_read_at(f, 0, data, size);
    platform_file_close(f);
    if (got != size) return (Core_String){0};
    data[size] = 0;
    return (Core_String){ .str = data, .len = size };
}

b32 platform_file_write_all(const char *path, const void *data, u64 size) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    u64 done = 0;
    while (done < size) {
        ssize_t n = write(fd, (const u8 *)data + done, size - done);
        if (n < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return false; }
        done += (u64)n;
    }
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

Platform_FileInfo platform_file_info(const char *path) {
    Platform_FileInfo info = {0};
    struct stat st;
    if (stat(path, &st) != 0) return info;
    info.exists   = true;
    info.is_dir   = S_ISDIR(st.st_mode);
    info.size     = (u64)st.st_size;
    info.mtime_ns = (s64)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
    return info;
}

b32 platform_make_dirs(const char *path) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

/* Iterative-recursive walk with a fixed path buffer; depth-limited so symlink
   loops can't run away. */
b32 platform_file_rename(const char *from, const char *to) { return rename(from, to) == 0; }
b32 platform_file_remove(const char *path) { return unlink(path) == 0; }

void platform_remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char child[2048];
                if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child)) continue;
                platform_remove_tree(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static b32 walk_rec(char *path, size_t len, u32 depth, Platform_WalkProc visit, void *user) {
    if (depth > 32) return true;
    DIR *dir = opendir(path);
    if (!dir) return true;
    int dfd = dirfd(dir);
    struct dirent *e;
    b32 keep_going = true;
    while (keep_going && (e = readdir(dir))) {
        if (e->d_name[0] == '.') continue; /* ., .., hidden */
        size_t nlen = strlen(e->d_name);
        if (len + 1 + nlen + 1 >= 4096) continue;

        struct stat st;
        if (fstatat(dfd, e->d_name, &st, 0) != 0) continue; /* follows symlinks */

        path[len] = '/';
        memcpy(path + len + 1, e->d_name, nlen + 1);
        if (S_ISDIR(st.st_mode)) {
            keep_going = walk_rec(path, len + 1 + nlen, depth + 1, visit, user);
        } else if (S_ISREG(st.st_mode)) {
            s64 mt = (s64)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
            keep_going = visit(user, path, (u64)st.st_size, mt);
        }
        path[len] = 0;
    }
    closedir(dir);
    return keep_going;
}

void platform_walk_dir(const char *root, Platform_WalkProc visit, void *user) {
    char path[4096];
    snprintf(path, sizeof(path), "%s", root);
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') path[--len] = 0;
    walk_rec(path, len, 0, visit, user);
}

static const char *push_cstr(Core_Arena *arena, const char *s);

static int cmp_name_ci(const void *a, const void *b) {
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

u32 platform_list_dirs(Core_Arena *arena, const char *path, const char ***out_names) {
    *out_names = 0;
    DIR *dir = opendir(path);
    if (!dir) return 0;
    int dfd = dirfd(dir);
    u32 count = 0, cap = 0;
    const char **names = 0;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        struct stat st;
        if (fstatat(dfd, e->d_name, &st, 0) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (count == cap) { /* grow by copying: listings are small and rare */
            u32 ncap = cap ? cap * 2 : 64;
            const char **n = core_push_array(arena, const char *, ncap);
            if (!n) break;
            if (count) memcpy(n, names, sizeof(*names) * count);
            names = n;
            cap = ncap;
        }
        names[count++] = push_cstr(arena, e->d_name);
    }
    closedir(dir);
    if (count) qsort(names, count, sizeof(*names), cmp_name_ci);
    *out_names = names;
    return count;
}

/* Read-only whole-file mapping (fonts). Pages are shared/file-backed. */
const u8 *platform_file_map(const char *path, u64 *out_size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return 0; }
    void *p = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return 0;
    if (out_size) *out_size = (u64)st.st_size;
    return p;
}

/* ---- well-known paths ---- */

static const char *push_cstr(Core_Arena *arena, const char *s) {
    size_t n = strlen(s);
    char *out = core_arena_push(arena, n + 1, 1);
    memcpy(out, s, n + 1);
    return out;
}

const char *platform_env(const char *name) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : 0;
}

const char *platform_home_dir(Core_Arena *arena) {
    const char *h = platform_env("HOME");
    return push_cstr(arena, h ? h : "/tmp");
}

static const char *xdg_dir(Core_Arena *arena, const char *env, const char *fallback) {
    char buf[4096];
    const char *base = platform_env(env);
    if (base) snprintf(buf, sizeof(buf), "%s/offbeat", base);
    else {
        const char *h = platform_env("HOME");
        snprintf(buf, sizeof(buf), "%s/%s/offbeat", h ? h : "/tmp", fallback);
    }
    return push_cstr(arena, buf);
}

const char *platform_cache_dir(Core_Arena *arena)  { return xdg_dir(arena, "XDG_CACHE_HOME", ".cache"); }
const char *platform_config_dir(Core_Arena *arena) { return xdg_dir(arena, "XDG_CONFIG_HOME", ".config"); }

const char *platform_music_dir(Core_Arena *arena) {
    const char *h = platform_env("HOME");
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s/Music", h ? h : "/tmp");
    return push_cstr(arena, buf);
}

const char *platform_exe_dir(Core_Arena *arena) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return push_cstr(arena, ".");
    buf[n] = 0;
    char *slash = strrchr(buf, '/');
    if (slash) *slash = 0;
    return push_cstr(arena, buf);
}

Platform_MemInfo platform_mem_info(void) {
    Platform_MemInfo m = {0};
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return m;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long kb;
        if (sscanf(line, "VmRSS: %llu kB", &kb) == 1)    m.rss = kb * 1024;
        else if (sscanf(line, "RssAnon: %llu kB", &kb) == 1) m.rss_anon = kb * 1024;
        else if (sscanf(line, "RssFile: %llu kB", &kb) == 1) m.rss_file = kb * 1024;
    }
    fclose(f);
    return m;
}

/* ---- child processes ---- */

extern char **environ;

struct Platform_Process {
    pid_t pid;
    int   fd;          /* read end of the merged stdout/stderr pipe */
    b32   eof;
    u32   len;         /* bytes buffered in `buf` */
    char  buf[4096];
};

Platform_Process *platform_process_spawn(const char *const *argv) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return 0;

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0); /* its own group: kill() reaches grandchildren too */

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(fds[1]);
    if (rc != 0) { close(fds[0]); return 0; }

    Platform_Process *p = calloc(1, sizeof(*p));
    if (!p) { kill(pid, SIGKILL); waitpid(pid, 0, 0); close(fds[0]); return 0; }
    p->pid = pid;
    p->fd = fds[0];
    return p;
}

s32 platform_process_read_line(Platform_Process *p, char *buf, u32 cap, f64 timeout_s) {
    f64 deadline = platform_time_seconds() + timeout_s;
    for (;;) {
        char *nl = memchr(p->buf, '\n', p->len);
        if (nl || (p->eof && p->len) || p->len == sizeof(p->buf)) {
            u32 n = nl ? (u32)(nl - p->buf) : p->len;      /* line length */
            u32 used = nl ? n + 1 : n;                     /* bytes consumed */
            u32 out = n;
            while (out && p->buf[out - 1] == '\r') out--;
            u32 copy = out < cap - 1 ? out : cap - 1;
            memcpy(buf, p->buf, copy);
            buf[copy] = 0;
            memmove(p->buf, p->buf + used, p->len - used);
            p->len -= used;
            return (s32)copy;
        }
        if (p->eof) return -1;

        f64 left = deadline - platform_time_seconds();
        if (left < 0) left = 0;
        struct pollfd pfd = { .fd = p->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)(left * 1000.0));
        if (pr < 0) { if (errno == EINTR) continue; p->eof = true; continue; }
        if (pr == 0) return -2;
        ssize_t got = read(p->fd, p->buf + p->len, sizeof(p->buf) - p->len);
        if (got > 0) p->len += (u32)got;
        else if (got == 0 || (errno != EINTR && errno != EAGAIN)) p->eof = true;
    }
}

void platform_process_kill(Platform_Process *p) {
    if (!p || p->pid <= 0) return;
    kill(-p->pid, SIGTERM);
}

s32 platform_process_finish(Platform_Process *p) {
    if (!p) return -1;
    close(p->fd);
    int status = 0;
    while (waitpid(p->pid, &status, 0) < 0 && errno == EINTR) {}
    s32 code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    free(p);
    return code;
}

static b32 is_runnable(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

b32 platform_find_executable(const char *name, char *out, u64 cap) {
    if (!name || !name[0]) return false;
    if (strchr(name, '/')) {
        if (!is_runnable(name) || strlen(name) >= cap) return false;
        memcpy(out, name, strlen(name) + 1);
        return true;
    }
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    while (*path) {
        const char *end = strchr(path, ':');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        char cand[1024];
        if (n && n + strlen(name) + 2 < sizeof(cand)) {
            memcpy(cand, path, n);
            snprintf(cand + n, sizeof(cand) - n, "/%s", name);
            if (is_runnable(cand) && strlen(cand) < cap) {
                memcpy(out, cand, strlen(cand) + 1);
                return true;
            }
        }
        if (!end) break;
        path = end + 1;
    }
    return false;
}
