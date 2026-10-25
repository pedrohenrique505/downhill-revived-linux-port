/* The Win32 subset of port/platform/linux/include/windows.h, on POSIX (see that header).

   Paths: the port builds them with '\\'; every function here turns '\\' into '/' first.
   GetModuleFileName gives the executable's path with '\\', so the callers can cut it at '\\'. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define DHD_NO_PATH_WRAP
#include <windows.h>
#include <bcrypt.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* The real C library calls, also when msvc_compat.h came first (-include). */
#undef fopen
#undef remove
#undef rename

/* ---------------- paths ---------------- */

static void posix_path(const char *in, char *out, size_t size)
{
    size_t i = 0;
    for (; in[i] && i + 1 < size; i++) out[i] = in[i] == '\\' ? '/' : in[i];
    out[i] = 0;
}

static void posix_path_w(const WCHAR *in, char *out, size_t size)
{
    char narrow[4 * MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, in, -1, narrow, sizeof narrow, NULL, NULL);
    posix_path(narrow, out, size);
}

FILE *dhd_fopen(const char *path, const char *mode)
{
    char p[4096];
    posix_path(path, p, sizeof p);
    return fopen(p, mode);
}

int dhd_remove(const char *path)
{
    char p[4096];
    posix_path(path, p, sizeof p);
    return remove(p);
}

int dhd_rename(const char *from, const char *to)
{
    char a[4096], b[4096];
    posix_path(from, a, sizeof a);
    posix_path(to, b, sizeof b);
    return rename(a, b);
}

int dhd_putenv(const char *assignment)
{
    const char *eq = strchr(assignment, '=');
    if (!eq) return -1;
    char name[256];
    size_t n = (size_t)(eq - assignment);
    if (n >= sizeof name) return -1;
    memcpy(name, assignment, n);
    name[n] = 0;
    return eq[1] ? setenv(name, eq + 1, 1) : unsetenv(name);
}

int dhd_putenv_s(const char *name, const char *value)
{
    return value && *value ? setenv(name, value, 1) : unsetenv(name);
}

/* ---------------- errors, time ---------------- */

static __thread DWORD last_error;
DWORD GetLastError(void) { return last_error; }
void SetLastError(DWORD error) { last_error = error; }

static DWORD error_from_errno(int e)
{
    return e == ENOENT || e == ENOTDIR ? ERROR_FILE_NOT_FOUND : e == EEXIST ? ERROR_ALREADY_EXISTS : (DWORD)e + 0x10000u;
}

BOOL QueryPerformanceCounter(LARGE_INTEGER *count)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    count->QuadPart = (LONGLONG)t.tv_sec * 1000000000 + t.tv_nsec;
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency)
{
    frequency->QuadPart = 1000000000;
    return TRUE;
}

ULONGLONG GetTickCount64(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (ULONGLONG)t.tv_sec * 1000 + (ULONGLONG)t.tv_nsec / 1000000;
}

DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }

void Sleep(DWORD ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}

/* 100-ns ticks between 1601-01-01 and 1970-01-01 */
#define EPOCH_DIFFERENCE 116444736000000000ULL

static void filetime_set(FILETIME *f, ULONGLONG ticks)
{
    f->dwLowDateTime = (DWORD)ticks;
    f->dwHighDateTime = (DWORD)(ticks >> 32);
}

static ULONGLONG filetime_get(const FILETIME *f) { return (ULONGLONG)f->dwHighDateTime << 32 | f->dwLowDateTime; }

static void filetime_from_timespec(FILETIME *f, const struct timespec *t)
{
    filetime_set(f, (ULONGLONG)t->tv_sec * 10000000 + (ULONGLONG)t->tv_nsec / 100 + EPOCH_DIFFERENCE);
}

void GetSystemTimeAsFileTime(FILETIME *out)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    filetime_from_timespec(out, &t);
}

BOOL FileTimeToLocalFileTime(const FILETIME *utc, FILETIME *local)
{
    time_t seconds = (time_t)((filetime_get(utc) - EPOCH_DIFFERENCE) / 10000000);
    struct tm tm;
    localtime_r(&seconds, &tm);
    filetime_set(local, filetime_get(utc) + (ULONGLONG)((LONGLONG)tm.tm_gmtoff * 10000000));
    return TRUE;
}

static void system_time(const struct tm *tm, unsigned ms, SYSTEMTIME *out)
{
    out->wYear = (WORD)(tm->tm_year + 1900);
    out->wMonth = (WORD)(tm->tm_mon + 1);
    out->wDayOfWeek = (WORD)tm->tm_wday;
    out->wDay = (WORD)tm->tm_mday;
    out->wHour = (WORD)tm->tm_hour;
    out->wMinute = (WORD)tm->tm_min;
    out->wSecond = (WORD)tm->tm_sec;
    out->wMilliseconds = (WORD)ms;
}

BOOL FileTimeToSystemTime(const FILETIME *time, SYSTEMTIME *out)
{
    ULONGLONG ticks = filetime_get(time) - EPOCH_DIFFERENCE;
    time_t seconds = (time_t)(ticks / 10000000);
    struct tm tm;
    gmtime_r(&seconds, &tm);
    system_time(&tm, (unsigned)(ticks % 10000000 / 10000), out);
    return TRUE;
}

void GetLocalTime(SYSTEMTIME *out)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    struct tm tm;
    localtime_r(&t.tv_sec, &tm);
    system_time(&tm, (unsigned)(t.tv_nsec / 1000000), out);
}

/* ---------------- handles: threads, events, files, searches ---------------- */

enum { H_THREAD = 1, H_EVENT, H_FILE, H_FIND };

typedef struct {
    int kind;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int signaled, manual;          /* events; threads: finished */
    pthread_t thread;
    DWORD exit_code;
    DWORD (*start)(void *);
    unsigned (*start_ex)(void *);
    void *arg;
    int fd;
    DIR *dir;
    char dir_path[4096], pattern[MAX_PATH];
} Handle;

#define PSEUDO_CURRENT_THREAD ((HANDLE)(intptr_t)-2)

static Handle *handle_new(int kind)
{
    Handle *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->kind = kind;
    h->fd = -1;
    pthread_mutex_init(&h->lock, NULL);
    pthread_cond_init(&h->changed, NULL);
    return h;
}

static void signal_handle(Handle *h)
{
    pthread_mutex_lock(&h->lock);
    h->signaled = 1;
    pthread_cond_broadcast(&h->changed);
    pthread_mutex_unlock(&h->lock);
}

static void *thread_main(void *arg)
{
    Handle *h = arg;
    DWORD code = h->start ? h->start(h->arg) : h->start_ex(h->arg);
    h->exit_code = code;
    signal_handle(h);
    return NULL;
}

static Handle *thread_start(Handle *h, SIZE_T stack)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack) pthread_attr_setstacksize(&attr, stack < 65536 ? 65536 : stack);
    int failed = pthread_create(&h->thread, &attr, thread_main, h);
    pthread_attr_destroy(&attr);
    if (failed) {
        free(h);
        return NULL;
    }
    pthread_detach(h->thread);
    return h;
}

HANDLE CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *id)
{
    (void)sa, (void)flags;
    Handle *h = handle_new(H_THREAD);
    if (!h) return NULL;
    h->start = fn;
    h->arg = arg;
    h = thread_start(h, stack);
    if (h && id) *id = (DWORD)(uintptr_t)h;
    return h;
}

uintptr_t _beginthreadex(void *security, unsigned stack, unsigned (*fn)(void *), void *arg, unsigned flags, unsigned *id)
{
    (void)security, (void)flags;
    Handle *h = handle_new(H_THREAD);
    if (!h) return 0;
    h->start_ex = fn;
    h->arg = arg;
    h = thread_start(h, stack);
    if (h && id) *id = (unsigned)(uintptr_t)h;
    return (uintptr_t)h;
}

HANDLE CreateEventA(SECURITY_ATTRIBUTES *sa, BOOL manual_reset, BOOL initial, const char *name)
{
    (void)sa, (void)name;
    Handle *h = handle_new(H_EVENT);
    if (!h) return NULL;
    h->manual = manual_reset;
    h->signaled = initial;
    return h;
}

BOOL SetEvent(HANDLE event)
{
    signal_handle(event);
    return TRUE;
}

BOOL ResetEvent(HANDLE event)
{
    Handle *h = event;
    pthread_mutex_lock(&h->lock);
    h->signaled = 0;
    pthread_mutex_unlock(&h->lock);
    return TRUE;
}

static void deadline_after(struct timespec *t, DWORD ms)
{
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += ms / 1000;
    t->tv_nsec += (long)(ms % 1000) * 1000000;
    if (t->tv_nsec >= 1000000000) t->tv_sec++, t->tv_nsec -= 1000000000;
}

DWORD WaitForSingleObject(HANDLE handle, DWORD ms)
{
    Handle *h = handle;
    if (!h || h == INVALID_HANDLE_VALUE || (h->kind != H_EVENT && h->kind != H_THREAD)) return WAIT_FAILED;
    struct timespec deadline;
    if (ms != INFINITE) deadline_after(&deadline, ms);
    pthread_mutex_lock(&h->lock);
    while (!h->signaled) {
        if (ms == INFINITE) pthread_cond_wait(&h->changed, &h->lock);
        else if (pthread_cond_timedwait(&h->changed, &h->lock, &deadline) == ETIMEDOUT) break;
    }
    DWORD result = h->signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    if (h->signaled && h->kind == H_EVENT && !h->manual) h->signaled = 0;
    pthread_mutex_unlock(&h->lock);
    return result;
}

BOOL GetExitCodeThread(HANDLE thread, DWORD *code)
{
    Handle *h = thread;
    pthread_mutex_lock(&h->lock);
    *code = h->signaled ? h->exit_code : 259 /* STILL_ACTIVE */;
    pthread_mutex_unlock(&h->lock);
    return TRUE;
}

BOOL CloseHandle(HANDLE handle)
{
    Handle *h = handle;
    if (!h || h == INVALID_HANDLE_VALUE || h == PSEUDO_CURRENT_THREAD) return FALSE;
    if (h->kind == H_FILE && h->fd >= 0) close(h->fd);
    /* A thread object stays allocated: its thread still writes to it when it ends. */
    if (h->kind != H_THREAD) free(h);
    return TRUE;
}

HANDLE GetCurrentThread(void) { return PSEUDO_CURRENT_THREAD; }
DWORD GetCurrentThreadId(void) { return (DWORD)syscall(SYS_gettid); }
DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }

BOOL SetThreadPriority(HANDLE thread, int priority)
{
    (void)thread, (void)priority;  /* normal scheduling: raising priority needs privileges on Linux */
    return TRUE;
}

BOOL GetThreadTimes(HANDLE thread, FILETIME *created, FILETIME *exited, FILETIME *kernel, FILETIME *user)
{
    clockid_t clock = CLOCK_THREAD_CPUTIME_ID;
    if (thread != PSEUDO_CURRENT_THREAD) {
        Handle *h = thread;
        if (!h || h->kind != H_THREAD || h->signaled || pthread_getcpuclockid(h->thread, &clock)) return FALSE;
    }
    struct timespec t;
    if (clock_gettime(clock, &t)) return FALSE;
    filetime_set(created, 0);
    filetime_set(exited, 0);
    filetime_set(kernel, 0);  /* user and kernel time together, in user */
    filetime_set(user, (ULONGLONG)t.tv_sec * 10000000 + (ULONGLONG)t.tv_nsec / 100);
    return TRUE;
}

void ExitProcess(UINT code) { exit((int)code); }

void InitializeCriticalSection(CRITICAL_SECTION *cs)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(cs, &attr);
    pthread_mutexattr_destroy(&attr);
}

BOOL SleepConditionVariableCS(CONDITION_VARIABLE *c, CRITICAL_SECTION *cs, DWORD ms)
{
    if (ms == INFINITE) return !pthread_cond_wait(c, cs);
    struct timespec deadline;
    deadline_after(&deadline, ms);
    if (pthread_cond_timedwait(c, cs, &deadline) == ETIMEDOUT) {
        last_error = WAIT_TIMEOUT;
        return FALSE;
    }
    return TRUE;
}

/* ---------------- files ---------------- */

DWORD GetModuleFileNameA(HMODULE module, char *path, DWORD size)
{
    (void)module;
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0 || !size) return 0;
    exe[n] = 0;
    DWORD i = 0;
    for (; exe[i] && i + 1 < size; i++) path[i] = exe[i] == '/' ? '\\' : exe[i];
    path[i] = 0;
    return i;
}

DWORD GetModuleFileNameW(HMODULE module, WCHAR *path, DWORD size)
{
    char narrow[4096];
    if (!GetModuleFileNameA(module, narrow, sizeof narrow)) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, narrow, -1, path, (int)size);
    return n > 0 ? (DWORD)n - 1 : 0;
}

HMODULE GetModuleHandleA(const char *name) { (void)name; return NULL; }
void *GetProcAddress(HMODULE module, const char *name) { (void)module, (void)name; return NULL; }

static DWORD attributes(const struct stat *st)
{
    return (S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL)
           | (st->st_mode & S_IWUSR ? 0 : FILE_ATTRIBUTE_READONLY);
}

DWORD GetFileAttributesA(const char *path)
{
    char p[4096];
    struct stat st;
    posix_path(path, p, sizeof p);
    if (stat(p, &st)) {
        last_error = error_from_errno(errno);
        return INVALID_FILE_ATTRIBUTES;
    }
    return attributes(&st);
}

DWORD GetFileAttributesW(const WCHAR *path)
{
    char p[4096];
    struct stat st;
    posix_path_w(path, p, sizeof p);
    if (stat(p, &st)) {
        last_error = error_from_errno(errno);
        return INVALID_FILE_ATTRIBUTES;
    }
    return attributes(&st);
}

BOOL GetFileAttributesExA(const char *path, GET_FILEEX_INFO_LEVELS level, void *info)
{
    (void)level;
    char p[4096];
    struct stat st;
    posix_path(path, p, sizeof p);
    if (stat(p, &st)) {
        last_error = error_from_errno(errno);
        return FALSE;
    }
    WIN32_FILE_ATTRIBUTE_DATA *d = info;
    d->dwFileAttributes = attributes(&st);
    filetime_from_timespec(&d->ftCreationTime, &st.st_ctim);
    filetime_from_timespec(&d->ftLastAccessTime, &st.st_atim);
    filetime_from_timespec(&d->ftLastWriteTime, &st.st_mtim);
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    d->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}

BOOL CreateDirectoryA(const char *path, SECURITY_ATTRIBUTES *sa)
{
    (void)sa;
    char p[4096];
    posix_path(path, p, sizeof p);
    if (mkdir(p, 0777)) {
        last_error = error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL DeleteFileA(const char *path)
{
    char p[4096];
    posix_path(path, p, sizeof p);
    return !unlink(p);
}

BOOL MoveFileExA(const char *from, const char *to, DWORD flags)
{
    (void)flags;  /* rename replaces, as MOVEFILE_REPLACE_EXISTING */
    return !dhd_rename(from, to);
}

HANDLE CreateFileA(const char *path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disposition,
                   DWORD flags, HANDLE templ)
{
    (void)share, (void)sa, (void)flags, (void)templ;
    char p[4096];
    posix_path(path, p, sizeof p);
    int mode = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW: mode |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: mode |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: mode |= O_CREAT; break;
    default: break;
    }
    int fd = open(p, mode | O_CLOEXEC, 0666);
    if (fd < 0) {
        last_error = error_from_errno(errno);
        return INVALID_HANDLE_VALUE;
    }
    Handle *h = handle_new(H_FILE);
    if (!h) {
        close(fd);
        return INVALID_HANDLE_VALUE;
    }
    h->fd = fd;
    return h;
}

BOOL ReadFile(HANDLE file, void *buffer, DWORD size, DWORD *read_bytes, void *overlapped)
{
    (void)overlapped;
    Handle *h = file;
    ssize_t n;
    do n = read(h->fd, buffer, size); while (n < 0 && errno == EINTR);
    if (read_bytes) *read_bytes = n > 0 ? (DWORD)n : 0;
    return n >= 0;
}

BOOL WriteFile(HANDLE file, const void *buffer, DWORD size, DWORD *written, void *overlapped)
{
    (void)overlapped;
    Handle *h = file;
    ssize_t n;
    do n = write(h->fd, buffer, size); while (n < 0 && errno == EINTR);
    if (written) *written = n > 0 ? (DWORD)n : 0;
    return n >= 0;
}

BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER *size)
{
    Handle *h = file;
    struct stat st;
    if (fstat(h->fd, &st)) return FALSE;
    size->QuadPart = st.st_size;
    return TRUE;
}

/* A search: "dir\\pattern" with the pattern on the last component only, as the port uses it. */
static Handle *find_open(const char *posix_pattern)
{
    Handle *h = handle_new(H_FIND);
    if (!h) return NULL;
    const char *slash = strrchr(posix_pattern, '/');
    if (slash) {
        size_t n = (size_t)(slash - posix_pattern);
        if (n >= sizeof h->dir_path) n = sizeof h->dir_path - 1;
        memcpy(h->dir_path, posix_pattern, n);
        h->dir_path[n ? n : 1] = 0;
        if (!n) h->dir_path[0] = '/';
        snprintf(h->pattern, sizeof h->pattern, "%s", slash + 1);
    } else {
        strcpy(h->dir_path, ".");
        snprintf(h->pattern, sizeof h->pattern, "%s", posix_pattern);
    }
    h->dir = opendir(h->dir_path);
    if (!h->dir) {
        last_error = error_from_errno(errno);
        free(h);
        return NULL;
    }
    return h;
}

/* The next match: its name and stat, or 0 at the end. */
static int find_next(Handle *h, char *name, size_t size, struct stat *st)
{
    struct dirent *e;
    while ((e = readdir(h->dir))) {
        if (fnmatch(h->pattern, e->d_name, FNM_CASEFOLD)) continue;
        char full[8192];
        snprintf(full, sizeof full, "%s/%s", h->dir_path, e->d_name);
        if (stat(full, st)) continue;
        snprintf(name, size, "%s", e->d_name);
        return 1;
    }
    last_error = 18; /* ERROR_NO_MORE_FILES */
    return 0;
}

static void find_fill_a(WIN32_FIND_DATAA *out, const char *name, const struct stat *st)
{
    memset(out, 0, sizeof *out);
    out->dwFileAttributes = attributes(st);
    filetime_from_timespec(&out->ftCreationTime, &st->st_ctim);
    filetime_from_timespec(&out->ftLastAccessTime, &st->st_atim);
    filetime_from_timespec(&out->ftLastWriteTime, &st->st_mtim);
    out->nFileSizeHigh = (DWORD)((uint64_t)st->st_size >> 32);
    out->nFileSizeLow = (DWORD)st->st_size;
    snprintf(out->cFileName, sizeof out->cFileName, "%s", name);
}

static void find_fill_w(WIN32_FIND_DATAW *out, const char *name, const struct stat *st)
{
    WIN32_FIND_DATAA a;
    find_fill_a(&a, name, st);
    memset(out, 0, sizeof *out);
    out->dwFileAttributes = a.dwFileAttributes;
    out->ftCreationTime = a.ftCreationTime;
    out->ftLastAccessTime = a.ftLastAccessTime;
    out->ftLastWriteTime = a.ftLastWriteTime;
    out->nFileSizeHigh = a.nFileSizeHigh;
    out->nFileSizeLow = a.nFileSizeLow;
    MultiByteToWideChar(CP_UTF8, 0, name, -1, out->cFileName, MAX_PATH);
}

HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *found)
{
    char p[4096], name[MAX_PATH];
    struct stat st;
    posix_path(pattern, p, sizeof p);
    Handle *h = find_open(p);
    if (!h) return INVALID_HANDLE_VALUE;
    if (!find_next(h, name, sizeof name, &st)) {
        FindClose(h);
        return INVALID_HANDLE_VALUE;
    }
    find_fill_a(found, name, &st);
    return h;
}

BOOL FindNextFileA(HANDLE search, WIN32_FIND_DATAA *found)
{
    char name[MAX_PATH];
    struct stat st;
    if (!find_next(search, name, sizeof name, &st)) return FALSE;
    find_fill_a(found, name, &st);
    return TRUE;
}

HANDLE FindFirstFileExW(const WCHAR *pattern, FINDEX_INFO_LEVELS level, void *found, FINDEX_SEARCH_OPS op,
                        void *filter, DWORD flags)
{
    (void)level, (void)op, (void)filter, (void)flags;
    char p[4096], name[MAX_PATH];
    struct stat st;
    posix_path_w(pattern, p, sizeof p);
    Handle *h = find_open(p);
    if (!h) return INVALID_HANDLE_VALUE;
    if (!find_next(h, name, sizeof name, &st)) {
        FindClose(h);
        return INVALID_HANDLE_VALUE;
    }
    find_fill_w(found, name, &st);
    return h;
}

BOOL FindNextFileW(HANDLE search, WIN32_FIND_DATAW *found)
{
    char name[MAX_PATH];
    struct stat st;
    if (!find_next(search, name, sizeof name, &st)) return FALSE;
    find_fill_w(found, name, &st);
    return TRUE;
}

BOOL FindClose(HANDLE search)
{
    Handle *h = search;
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    if (h->dir) closedir(h->dir);
    free(h);
    return TRUE;
}

/* UTF-8 <-> wchar_t (UTF-32 on Linux). The ANSI code page is UTF-8 here. */
int MultiByteToWideChar(UINT page, DWORD flags, const char *in, int in_size, WCHAR *out, int out_size)
{
    (void)page, (void)flags;
    const unsigned char *s = (const unsigned char *)in;
    size_t len = in_size < 0 ? strlen(in) + 1 : (size_t)in_size;
    int n = 0;
    for (size_t i = 0; i < len;) {
        uint32_t c = s[i++];
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3Fu >> extra;
        while (extra-- > 0 && i < len) c = c << 6 | (s[i++] & 0x3Fu);
        if (out_size) {
            if (n >= out_size) return 0;
            out[n] = (WCHAR)c;
        }
        n++;
    }
    return n;
}

int WideCharToMultiByte(UINT page, DWORD flags, const WCHAR *in, int in_size, char *out, int out_size,
                        const char *fallback, BOOL *used)
{
    (void)page, (void)flags, (void)fallback;
    if (used) *used = FALSE;
    size_t len = in_size < 0 ? wcslen(in) + 1 : (size_t)in_size;
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        uint32_t c = (uint32_t)in[i];
        char b[4];
        int k;
        if (c < 0x80) b[0] = (char)c, k = 1;
        else if (c < 0x800) b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 63)), k = 2;
        else if (c < 0x10000) b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 63)), b[2] = (char)(0x80 | (c & 63)), k = 3;
        else b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | (c >> 12 & 63)), b[2] = (char)(0x80 | (c >> 6 & 63)), b[3] = (char)(0x80 | (c & 63)), k = 4;
        if (out_size) {
            if (n + k > out_size) return 0;
            memcpy(out + n, b, (size_t)k);
        }
        n += k;
    }
    return n;
}

/* ---------------- memory protection and vectored exception handlers ---------------- */

BOOL VirtualProtect(void *address, SIZE_T size, DWORD protection, DWORD *old)
{
    uintptr_t page = (uintptr_t)address & ~(uintptr_t)4095;
    size_t length = ((uintptr_t)address + size - page + 4095) & ~(size_t)4095;
    int prot = protection == PAGE_NOACCESS ? PROT_NONE : protection == PAGE_READONLY ? PROT_READ
             : protection == PAGE_EXECUTE_READWRITE ? PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_WRITE;
    if (old) *old = PAGE_READWRITE;  /* not tracked: the port only toggles NOACCESS/READWRITE */
    return !mprotect((void *)page, length, prot);
}

enum { MAX_HANDLERS = 8 };
static PVECTORED_EXCEPTION_HANDLER handlers[MAX_HANDLERS];
static unsigned handler_count;
static struct sigaction previous_segv, previous_bus;

static void fault(int sig, siginfo_t *info, void *ucontext)
{
    EXCEPTION_RECORD record = {0};
    CONTEXT context = {ucontext};
    EXCEPTION_POINTERS pointers = {&record, &context};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = info->si_addr;
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 0;  /* read or write: not known from the signal */
    record.ExceptionInformation[1] = (ULONG_PTR)info->si_addr;
    for (unsigned i = 0; i < handler_count; i++)
        if (handlers[i](&pointers) == EXCEPTION_CONTINUE_EXECUTION) return;
    /* not ours: the previous disposition handles it (the crash report, or the default) */
    const struct sigaction *prev = sig == SIGBUS ? &previous_bus : &previous_segv;
    if (prev->sa_flags & SA_SIGINFO && prev->sa_sigaction) {
        prev->sa_sigaction(sig, info, ucontext);
        return;
    }
    signal(sig, prev->sa_handler == SIG_IGN ? SIG_DFL : prev->sa_handler);
}

void *AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)
{
    if (handler_count == MAX_HANDLERS) return NULL;
    if (!handler_count) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = fault;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &previous_segv);
        sigaction(SIGBUS, &sa, &previous_bus);
    }
    if (first) {
        memmove(handlers + 1, handlers, handler_count * sizeof *handlers);
        handlers[0] = handler;
    } else {
        handlers[handler_count] = handler;
    }
    handler_count++;
    return (void *)handler;
}

ULONG RemoveVectoredExceptionHandler(void *handle)
{
    for (unsigned i = 0; i < handler_count; i++)
        if ((void *)handlers[i] == handle) {
            memmove(handlers + i, handlers + i + 1, (handler_count - i - 1) * sizeof *handlers);
            handler_count--;
            return 1;
        }
    return 0;
}

/* ---------------- desktop ---------------- */

HMONITOR MonitorFromPoint(POINT point, DWORD flags) { (void)point, (void)flags; return NULL; }
BOOL GetMonitorInfoA(HMONITOR monitor, MONITORINFO *info) { (void)monitor, (void)info; return FALSE; }
BOOL EnumDisplaySettingsA(const char *device, DWORD mode, DEVMODEA *out) { (void)device, (void)mode, (void)out; return FALSE; }

int MessageBoxA(HWND owner, const char *text, const char *caption, UINT type)
{
    (void)owner, (void)type;
    fprintf(stderr, "%s: %s\n", caption ? caption : "", text ? text : "");
    return 1; /* IDOK */
}

short GetAsyncKeyState(int key) { (void)key; return 0; }
BOOL OpenClipboard(HWND owner) { (void)owner; return FALSE; }
HANDLE GetClipboardData(UINT format) { (void)format; return NULL; }
BOOL CloseClipboard(void) { return TRUE; }
void *GlobalLock(HGLOBAL h) { return h; }
BOOL GlobalUnlock(HGLOBAL h) { (void)h; return TRUE; }
SIZE_T GlobalSize(HGLOBAL h) { (void)h; return 0; }
void OutputDebugStringA(const char *text) { fputs(text, stderr); }

/* ---------------- SHA-256 (bcrypt.h): FIPS 180-4 ---------------- */

typedef struct {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[64];
    unsigned used;
} Sha256;

static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t ror(uint32_t x, unsigned n) { return x >> n | x << (32 - n); }

static void sha_block(Sha256 *s, const uint8_t *p)
{
    uint32_t w[64], v[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7]
               + (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >> 10);
    memcpy(v, s->state, sizeof v);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = v[7] + (ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + sha_k[i] + w[i];
        uint32_t t2 = (ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof *v);
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->state[i] += v[i];
}

NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *alg, const WCHAR *id, const WCHAR *impl, ULONG flags)
{
    (void)impl, (void)flags;
    if (wcscmp(id, L"SHA256")) return (NTSTATUS)0xC0000225; /* STATUS_NOT_FOUND */
    *alg = (void *)1;
    return 0;
}

NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE alg, ULONG flags) { (void)alg, (void)flags; return 0; }

NTSTATUS BCryptCreateHash(BCRYPT_ALG_HANDLE alg, BCRYPT_HASH_HANDLE *hash, BYTE *object, ULONG object_size,
                          BYTE *secret, ULONG secret_size, ULONG flags)
{
    (void)alg, (void)object, (void)object_size, (void)secret, (void)secret_size, (void)flags;
    static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    Sha256 *s = calloc(1, sizeof *s);
    if (!s) return (NTSTATUS)0xC0000017; /* STATUS_NO_MEMORY */
    memcpy(s->state, initial, sizeof initial);
    *hash = s;
    return 0;
}

NTSTATUS BCryptHashData(BCRYPT_HASH_HANDLE hash, const BYTE *data, ULONG size, ULONG flags)
{
    (void)flags;
    Sha256 *s = hash;
    s->length += size;
    while (size) {
        unsigned take = 64 - s->used < size ? 64 - s->used : size;
        memcpy(s->block + s->used, data, take);
        s->used += take, data += take, size -= take;
        if (s->used == 64) sha_block(s, s->block), s->used = 0;
    }
    return 0;
}

NTSTATUS BCryptFinishHash(BCRYPT_HASH_HANDLE hash, BYTE *digest, ULONG size, ULONG flags)
{
    (void)flags;
    Sha256 *s = hash;
    if (size < 32) return (NTSTATUS)0xC000000D; /* STATUS_INVALID_PARAMETER */
    uint64_t bits = s->length * 8;
    uint8_t pad = 0x80;
    BCryptHashData(s, &pad, 1, 0);
    pad = 0;
    while (s->used != 56) BCryptHashData(s, &pad, 1, 0);
    uint8_t length[8];
    for (int i = 0; i < 8; i++) length[i] = (uint8_t)(bits >> (56 - 8 * i));
    BCryptHashData(s, length, 8, 0);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) digest[i * 4 + j] = (BYTE)(s->state[i] >> (24 - 8 * j));
    return 0;
}

NTSTATUS BCryptDestroyHash(BCRYPT_HASH_HANDLE hash)
{
    free(hash);
    return 0;
}
