/* port/hle/host.c on Linux: the host side of the native PS2 system (hle.h). The settings, the disc
   check and the pad scripts are the same as there; fibers, time, the crash report and the disc
   chooser are POSIX. The Windows diagnostics that suspend the game thread (DHD_DUMP, DHD_PROFILE,
   DHD_WATCH) are not available here yet. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hle.h"
#include "text_input.h"
#include "timer.h"
#include "ps2asm.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* The game's code and the port's: [__executable_start, etext) (the linker defines both). */
extern char __executable_start[], etext[];

/* ---------------- the crash report ---------------- */

static int readable(const void *p, size_t n)
{
    char probe[256];
    struct iovec local = {probe, n < sizeof probe ? n : sizeof probe}, remote = {(void *)p, local.iov_len};
    return p && process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)local.iov_len;
}

/* Like Windows' IsBadStringPtrA(s, 256): nonzero if s can't be read up to its NUL (or 256 bytes). */
static int bad_string(const char *s)
{
    for (size_t i = 0; i < 256;) {
        size_t page_left = 4096 - ((uintptr_t)(s + i) & 4095), n = 256 - i < page_left ? 256 - i : page_left;
        char chunk[256];
        struct iovec local = {chunk, n}, remote = {(void *)(s + i), n};
        if (!s || process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)n) return 1;
        if (memchr(chunk, 0, n)) return 0;
        i += n;
    }
    return 0;
}

/* The likely return addresses on a stack: words in the code range, as Windows' report prints. */
static void print_returns(const uint32_t *esp)
{
    unsigned found = 0;
    fprintf(stderr, "  stack code addresses:");
    for (unsigned i = 0; i < 2048 && found < 32; i++) {
        uint32_t word;
        if (!readable(esp + i, 4)) break;
        word = esp[i];
        if (word >= (uintptr_t)__executable_start && word < (uintptr_t)etext) {
            fprintf(stderr, " %08X", word);
            found++;
        }
    }
    fprintf(stderr, "\n");
}

static void crashed(int sig, siginfo_t *info, void *context)
{
    const ucontext_t *u = context;
    const greg_t *r = u->uc_mcontext.gregs;
    fprintf(stderr, "crash: signal %d (%s) at EIP=%08X, address %p\n", sig, strsignal(sig),
            (unsigned)r[REG_EIP], info->si_addr);
    fprintf(stderr, "  EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X\n",
            (unsigned)r[REG_EAX], (unsigned)r[REG_EBX], (unsigned)r[REG_ECX], (unsigned)r[REG_EDX],
            (unsigned)r[REG_ESI], (unsigned)r[REG_EDI], (unsigned)r[REG_EBP], (unsigned)r[REG_ESP]);
    print_returns((const uint32_t *)(uintptr_t)r[REG_ESP]);
    fprintf(stderr, "  (port/tools/addr2sym.py names them from build/port/link/dhd.map)\n");
    fflush(stderr);
    _exit(3);
}

void host_backtrace(void)
{
    fprintf(stderr, "backtrace from %p:\n", __builtin_return_address(0));
    print_returns((const uint32_t *)__builtin_frame_address(0));
}

void host_safe_vprint(FILE *f, const char *fmt, va_list ap)
{
    char spec[32];
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            fputc(*p, f);
            continue;
        }
        size_t n = strspn(p + 1, "-+ #0123456789.lhzjt");
        if (n + 3 > sizeof spec || !p[n + 1]) {
            fputs(p, f);
            break;
        }
        memcpy(spec, p, n + 2);
        spec[n + 2] = 0;
        char conv = p[n + 1];
        p += n + 1;
        if (conv == '%')
            fputc('%', f);
        else if (conv == 's') {
            const char *s = va_arg(ap, const char *);
            fprintf(f, bad_string(s) ? "<bad %p>" : spec, s);
        } else if (strchr("eEfgGaA", conv))
            fprintf(f, spec, va_arg(ap, double));
        else if (strstr(spec, "ll"))
            fprintf(f, spec, va_arg(ap, long long));
        else if (conv == 'n')
            (void)va_arg(ap, void *);
        else
            fprintf(f, spec, va_arg(ap, int));
    }
}

static double dump_due;
/* The EE's next kernel boundary supplies a coherent fiber/kernel snapshot. */
void host_poll_dump(void)
{
    if (!dump_due || host_wall_seconds() < dump_due) return;
    dump_due = 0;
    fprintf(stderr, "dump: EE at kernel boundary\n");
    host_backtrace();
    kernel_dump();
}

/* ---------------- named threads ---------------- */

enum { NAMED_THREADS = 16 };
static struct { const char *name; clockid_t clock; } named[NAMED_THREADS];
static volatile int named_count;

void host_thread_name(const char *name)
{
    int at = __atomic_fetch_add(&named_count, 1, __ATOMIC_SEQ_CST);
    pthread_setname_np(pthread_self(), name);
    if (at >= NAMED_THREADS) return;
    if (pthread_getcpuclockid(pthread_self(), &named[at].clock)) return;
    __atomic_store_n(&named[at].name, name, __ATOMIC_RELEASE);  /* published last */
}

unsigned host_thread_cpu(const char **names, double *seconds, unsigned max)
{
    unsigned n = 0;
    for (int i = 0; i < named_count && i < NAMED_THREADS && n < max; i++) {
        const char *name = __atomic_load_n(&named[i].name, __ATOMIC_ACQUIRE);
        struct timespec t;
        if (!name || clock_gettime(named[i].clock, &t)) continue;
        names[n] = name;
        seconds[n++] = (double)t.tv_sec + t.tv_nsec * 1e-9;
    }
    return n;
}

void host_init(void)
{
    /* the report runs on its own stack, so a stack overflow is reported too */
    static char alt[65536];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crashed;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    static const int fatal[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    for (unsigned i = 0; i < sizeof fatal / sizeof *fatal; i++) sigaction(fatal[i], &sa, NULL);
    host_thread_name("game");
    const char *dump = getenv("DHD_DUMP");
    double delay = dump ? atof(dump) : 0;
    if (delay > 0) dump_due = host_wall_seconds() + delay;
    if (getenv("DHD_PROFILE") || getenv("DHD_WATCH"))
        fprintf(stderr, "DHD_PROFILE and DHD_WATCH are not available on Linux yet\n");
}

/* The log goes to stderr as it is (a terminal, or the starter's file or pipe). First thing at
   start, so also where SDL is told to leave SIGINT and SIGTERM alone: it would turn them into a
   quit event that only the game thread reads, once a field, and a stuck game could not be ended
   with Ctrl+C. */
void host_console_init(void)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    setenv("SDL_NO_SIGNAL_HANDLERS", "1", 0);
}

/* Linux needs no relaunch: memory.c maps the PS2 RAM's own addresses directly. */
int host_relaunch_reserving(uintptr_t at, size_t size)
{
    (void)at, (void)size;
    return -1;
}

/* ---------------- pads ---------------- */

static const uint16_t pressure_order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                            PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};

static int riding_controls(void) { return *(volatile int32_t *)0x0829E290 != 0; }

/* The keyboard's state by Windows virtual-key code (keyboard.c's names), from the game window
   (port/platform/linux/present.c); 0 when no window of the game has the focus. */
__attribute__((weak)) int host_keyboard_state(uint8_t down[256])
{
    (void)down;
    return 0;
}

static void keyboard_merge(HostPad *out)
{
    uint8_t state[256] = {0}, down[256] = {0};
    if (host_keyboard_state(state)) {
        int count;
        const uint8_t *keys = keyboard_keys(&count);
        for (int i = 0; i < count; i++)
            if (state[keys[i]] && text_input_key_allowed(keys[i])) down[keys[i]] = 1;
    }
    keyboard_pad(down, riding_controls(), host_seconds(), out);
}

static void keys_script(HostPad *out)
{
    const char *script = getenv("DHD_KEYS");
    double seconds = race_sync_seconds();
    uint8_t down[256];
    if (!script || seconds < 0) return;
    if (!keyboard_script(script, seconds, down)) {
        fprintf(stderr, "Invalid DHD_KEYS: expected start:duration:key[+key],...\n");
        exit(1);
    }
    keyboard_pad(down, riding_controls(), host_seconds(), out);
    uintptr_t rider = *(volatile uint32_t *)0x082FF3F0;
    int right = 0, left = 0;
    if (rider >= 0x400000 && rider < 0x0A000000) right = *(volatile int32_t *)(rider + 0x1190), left = *(volatile int32_t *)(rider + 0x1194);
    static int last[5] = {-1};
    int now[5] = {out->buttons, out->left_x, out->left_y, right, left};
    if (memcmp(now, last, sizeof now)) {
        hle_log("keys race=%.3f riding=%d buttons=%04X lx=%d ly=%d steer right=%d left=%d\n", seconds,
                riding_controls(), out->buttons, out->left_x, out->left_y, right, left);
        memcpy(last, now, sizeof now);
    }
}

int host_pad_script(int player, HostPad *out)
{
    size_t i;
    memset(out, 0, sizeof *out);
    out->right_x = out->right_y = out->left_x = out->left_y = 128;
    const char *script = getenv("DHD_AUTOPAD");
    const char *frame_script = getenv("DHD_AUTOPAD_FRAMES");
    const char *nav = getenv("DHD_NAV");
    if (frame_script) script = frame_script;
    if (nav) script = nav;
    if (!script)
        return 0;
    extern uint64_t kernel_vsync_count;
    double input_time = frame_script ? (double)(kernel_vsync_count ? kernel_vsync_count - 1 : 0) : host_seconds();
    if (nav ? !autopad_nav(nav, kernel_vsync_count, &out->buttons) :
              !autopad_read(script, input_time, &out->buttons)) {
        if (nav) fprintf(stderr, "Invalid DHD_NAV: expected menu[/phase]:button[+button],...\n");
        else fprintf(stderr, "Invalid %s: expected start:duration:button[+button],...\n",
                     frame_script ? "DHD_AUTOPAD_FRAMES" : "DHD_AUTOPAD");
        exit(1);
    }
    race_sync_vsync(kernel_vsync_count);
    race_sync_pad(player, &out->buttons);
    if (player == 0) keys_script(out);
    static uint16_t previous;
    if (player == 0 && previous != out->buttons) {
        if (frame_script || nav)
            hle_log("autopad field=%llu game=%.3f wall=%.3f: %04X\n",
                    (unsigned long long)kernel_vsync_count, host_seconds(), host_wall_seconds(), out->buttons);
        else hle_log("autopad %.3f: %04X\n", host_seconds(), out->buttons);
        previous = out->buttons;
    }
    for (i = 0; i < 12; i++) out->pressure[i] = out->buttons & pressure_order[i] ? 255 : 0;
    return 1;
}

void host_pad_read(int port, HostPad *out)
{
    if (!host_pad_script(port, out))
        host_pad_device(port, out);
}

/* The controller through SDL (gamepad.c), and for player 1 the keyboard too. */
void host_pad_device(int port, HostPad *out)
{
    memset(out, 0, sizeof *out);
    out->right_x = out->right_y = out->left_x = out->left_y = 128;
    if (gamepad_start()) gamepad_read(port, out);
    if (port == 0) keyboard_merge(out);
}

/* Vibration goes through SDL (gamepad.c) on Linux; this is Windows' XInput fallback. */
void host_pad_vibrate(int port, int light, int heavy) { (void)port, (void)light, (void)heavy; }

/* ---------------- fibers ----------------
   The EE threads are fibers: a switch saves the callee-saved registers and the stack pointer on
   the fiber's own stack (fiber_swap), and host_fiber_switch saves the whole x87/SSE state around it
   (fxsave), as on Windows. glibc's swapcontext would make a system call at every switch. */

typedef struct Fiber {
    void *sp;
    void (*fn)(void *);
    void *arg;
    char *stack;      /* mmap'd, with a guard page below; NULL for a thread's own fiber */
    size_t size;
} Fiber;

enum { FIBER_STACK = 0x40000 };
static __thread Fiber *current_fiber;

void fiber_swap(void **save_sp, void *new_sp);
__asm__(".text\n"
        ".globl fiber_swap\n"
        ".type fiber_swap, @function\n"
        "fiber_swap:\n"
        "    pushl %ebp\n"
        "    pushl %ebx\n"
        "    pushl %esi\n"
        "    pushl %edi\n"
        "    movl 20(%esp), %eax\n"   /* save_sp */
        "    movl 24(%esp), %ecx\n"   /* new_sp */
        "    movl %esp, (%eax)\n"
        "    movl %ecx, %esp\n"
        "    popl %edi\n"
        "    popl %esi\n"
        "    popl %ebx\n"
        "    popl %ebp\n"
        "    ret\n"
        ".size fiber_swap, .-fiber_swap\n");

static uintptr_t stack_range[64][2];
static int stack_ranges;
static pthread_mutex_t stack_lock = PTHREAD_MUTEX_INITIALIZER;

static void note_range(uintptr_t low, uintptr_t high)
{
    pthread_mutex_lock(&stack_lock);
    int known = 0;
    for (int i = 0; i < stack_ranges; i++) known |= stack_range[i][1] == high;
    if (!known && stack_ranges < 64) {
        stack_range[stack_ranges][0] = low;
        stack_range[stack_ranges++][1] = high;
    }
    pthread_mutex_unlock(&stack_lock);
}

int host_stack_ranges(uintptr_t (**ranges)[2])
{
    *ranges = stack_range;
    return stack_ranges;
}

static void fiber_entry(void)
{
    Fiber *f = current_fiber;
    f->fn(f->arg);
    fprintf(stderr, "host: a fiber returned\n");  /* never: EE threads end by switching away */
    abort();
}

void *host_fiber_main(void)
{
    Fiber *f = calloc(1, sizeof *f);
    pthread_attr_t attr;
    void *base;
    size_t size;
    if (!f) return NULL;
    if (!pthread_getattr_np(pthread_self(), &attr)) {
        if (!pthread_attr_getstack(&attr, &base, &size)) note_range((uintptr_t)base, (uintptr_t)base + size);
        pthread_attr_destroy(&attr);
    }
    current_fiber = f;
    return f;
}

/* The game keeps the stack address of a local as a PS2 address: func_002195F0 and func_0019CB08
   build an image packet in a local array and pass it to sceGsExecLoadImage, whose DMA takes it
   ANDed with 0x0FFFFFFF. That is the same address only below 0x10000000, where Windows happens to
   put fiber stacks; Linux's mmap gives 0xE0000000 and up, and the GIF transfer then reads unmapped
   memory. Fiber stacks go between the PS2 RAM view (PS2_RAM_BASE + 32 MB) and 0x10000000. */
static char *low_stack(size_t size)
{
    char *stack = MAP_FAILED;
    /* first fit, so the stacks of deleted fibers are used again */
    for (uintptr_t at = PS2_RAM_BASE + PS2_RAM_SIZE; at + size <= 0x10000000 && stack == MAP_FAILED; at += size) {
        stack = mmap((void *)at, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK | MAP_FIXED_NOREPLACE, -1, 0);
        if (stack != MAP_FAILED && stack != (char *)at) munmap(stack, size), stack = MAP_FAILED;
    }
    if (stack == MAP_FAILED) fprintf(stderr, "host: no stack below 0x10000000 for a fiber\n");
    return stack;
}

void *host_fiber_create(void (*fn)(void *), void *arg)
{
    Fiber *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->size = FIBER_STACK + 4096;
    f->stack = low_stack(f->size);
    if (f->stack == MAP_FAILED) {
        free(f);
        return NULL;
    }
    mprotect(f->stack, 4096, PROT_NONE);  /* guard page */
    f->fn = fn;
    f->arg = arg;
    uintptr_t top = ((uintptr_t)f->stack + f->size - 16) & ~(uintptr_t)15;
    uint32_t *sp = (uint32_t *)top;
    /* fiber_swap pops edi, esi, ebx, ebp, then returns into fiber_entry with esp % 16 == 12 */
    *--sp = 0;                                   /* fiber_entry's (absent) return address */
    *--sp = (uint32_t)(uintptr_t)fiber_entry;
    *--sp = 0, *--sp = 0, *--sp = 0, *--sp = 0;  /* ebp, ebx, esi, edi */
    f->sp = sp;
    note_range((uintptr_t)f->stack + 4096, (uintptr_t)f->stack + f->size);
    return f;
}

void host_fiber_switch(void *fiber)
{
    unsigned char state[512] __attribute__((aligned(16)));
    __asm__ volatile("fxsave %0" : "=m"(state) : : "memory");
    Fiber *from = current_fiber, *to = fiber;
    current_fiber = to;
    fiber_swap(&from->sp, to->sp);
    __asm__ volatile("fxrstor %0" : : "m"(state) : "memory");
}

void host_fiber_delete(void *fiber)
{
    Fiber *f = fiber;
    if (!f || f == current_fiber) return;
    if (f->stack) munmap(f->stack, f->size);
    free(f);
}

void *host_fiber_current(void) { return current_fiber; }

/* ---------------- time ---------------- */

static double monotonic(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

double host_wall_seconds(void)
{
    static double start = -1;
    if (start < 0) start = monotonic();
    return monotonic() - start;
}

double host_seconds(void)
{
    return ee_frame_clock_enabled() ? (double)ee_frame_clock_cycles() / 147456000.0 : host_wall_seconds();
}

void host_sleep_ms(unsigned ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}

/* Until host_wall_seconds() reaches seconds: sleep to half a millisecond before, then spin. */
void host_wait_wall(double seconds)
{
    double left = seconds - host_wall_seconds();
    if (left > 0.0005) {
        double until = monotonic() + left - 0.0005;
        struct timespec t = {(time_t)until, (long)((until - (double)(time_t)until) * 1e9)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
    }
    while (host_wall_seconds() < seconds)
        __builtin_ia32_pause();
}

int host_skip_movies(void)
{
    static int logged;
    const char *skip = getenv("DHD_SKIP_MOVIES");
    if (!skip || !atoi(skip)) return 0;
    if (!logged++) hle_log("DHD_SKIP_MOVIES: bypassing startup movie sequence\n");
    return 1;
}

/* ---------------- settings: dhd.ini, as in port/hle/host.c ---------------- */

static void setting(const char *name, const char *value)
{
    if (!getenv(name)) setenv(name, value, 1);
}

static void apply_setting(const char *key, const char *value)
{
    if (!strcasecmp(key, "game_options")) return;
    static const struct { const char *key, *name; } plain[] = {
        {"iso", "DHD_ISO"}, {"skip_movies", "DHD_SKIP_MOVIES"}, {"window", "DHD_WINDOW"},
        {"width", "DHD_WINDOW_WIDTH"}, {"height", "DHD_WINDOW_HEIGHT"}, {"monitor", "DHD_MONITOR"},
        {"scale", "DHD_SCALE"}, {"widescreen", "DHD_WIDESCREEN"}, {"msaa", "DHD_MSAA"}, {"fxaa", "DHD_FXAA"},
        {"filter", "DHD_FILTER"}, {"vibration", "DHD_VIBRATION"}, {"remap", "DHD_REMAP"}, {"memory_card", "DHD_MEMCARD"},
        {"sound_latency_ms", "DHD_SND_LATENCY_MS"},
        {"server", "DHD_NET_SERVER"}, {"name", "DHD_NET_NAME"}, {"rider", "DHD_NET_RIDER"},
        {"update_url", "DHD_UPDATE_URL"}, {"auto_update", "DHD_AUTO_UPDATE"},
        {"texture_pack", "DHD_TEXTURE_PACK"}, {"textures", "DHD_TEXTURES"},
        {"texture_cache_mb", "DHD_TEXTURE_CACHE_MB"},
        {"font", "DHD_FONT"},
        {"fps", "DHD_FPS"},
    };
    if (!strncasecmp(key, "DHD_", 4)) { setting(key, value); return; }
    if (!strcasecmp(key, "renderer")) { setting("DHD_RENDER", strcasecmp(value, "native") ? "exact" : "native"); return; }
    for (unsigned i = 0; i < sizeof plain / sizeof *plain; i++)
        if (!strcasecmp(key, plain[i].key)) { setting(plain[i].name, value); return; }
    hle_log("settings: unknown key %s\n", key);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = 0;
    return s;
}

/* The executable's folder, with a '/' at the end. */
static void exe_folder(char *path, size_t size)
{
    ssize_t n = readlink("/proc/self/exe", path, size - 1);
    if (n < 0) n = 0;
    path[n] = 0;
    while (n && path[n - 1] != '/') n--;
    path[n] = 0;
}

/* dhd.ini's path: DHD_SETTINGS, else beside the executable. */
static void settings_path(char *path, size_t size)
{
    const char *named_path = getenv("DHD_SETTINGS");
    if (named_path) { snprintf(path, size, "%s", named_path); return; }
    exe_folder(path, size);
    size_t n = strlen(path);
    snprintf(path + n, size - n, "dhd.ini");
}

static char *setting_value(char *value)
{
    if (*value == '"') {
        char *end = strchr(value + 1, '"');
        if (end) { *end = 0; return value + 1; }
    }
    char *comment = strpbrk(value, ";#");
    if (comment && (comment == value || comment[-1] == ' ' || comment[-1] == '\t')) { *comment = 0; value = trim(value); }
    return value;
}

static int iso_from_environment;

static void note_overrides(void)
{
    static const char *const names[] = {"DHD_SCALE", "DHD_WINDOW", "DHD_WINDOW_WIDTH", "DHD_WINDOW_HEIGHT",
                                        "DHD_ASPECT", "DHD_PRESENT_FILTER", "DHD_WIDESCREEN", "DHD_SKIP_MOVIES",
                                        "DHD_VIBRATION", "DHD_RENDER", "DHD_TEXTURES", "DHD_FONT"};
    char list[256] = ",";
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
        if (getenv(names[i]) && strlen(list) + strlen(names[i]) + 2 < sizeof list)
            strcat(strcat(list, names[i]), ",");
    setenv("DHD_ENV_OVERRIDES", list, 1);
}

int host_setting_locked(const char *name)
{
    char key[64];
    const char *list = getenv("DHD_ENV_OVERRIDES");
    snprintf(key, sizeof key, ",%s,", name);
    return list && strstr(list, key) != NULL;
}

#define OFFICIAL_SERVER "wss://dhd.skuza.app"

static int settings_has(const char *key);

/* The player defaults where dhd.ini doesn't say. On Linux the renderer stays exact until the
   native renderer is ported (port/platform/linux/present.c). */
static const char *const player_defaults[][3] = {
    {"renderer", "exact", "DHD_RENDER"}, {"scale", "2", "DHD_SCALE"}, {"window", "windowed", "DHD_WINDOW"},
    {"widescreen", "0", "DHD_WIDESCREEN"}, {"DHD_ASPECT", "4:3", "DHD_ASPECT"}, {"skip_movies", "0", "DHD_SKIP_MOVIES"},
};

void host_settings_load(void)
{
    char path[4096];
    iso_from_environment = getenv("DHD_ISO") != NULL;
    note_overrides();
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    char line[1024];
    while (f && fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq = strchr(s, '=');
        if (!*s || *s == ';' || *s == '#' || *s == '[' || !eq) continue;
        *eq = 0;
        char *key = trim(s), *value = setting_value(trim(eq + 1));
        if (*key && *value) apply_setting(key, value);
    }
    if (f) {
        fclose(f);
        for (unsigned i = 0; i < sizeof player_defaults / sizeof *player_defaults; i++)
            if (!settings_has(player_defaults[i][0])) setting(player_defaults[i][2], player_defaults[i][1]);
    }
    setting("DHD_NET_SERVER", OFFICIAL_SERVER);
}

static int settings_has(const char *key)
{
    char path[4096], line[1024];
    int found = 0;
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    while (f && !found && fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq = strchr(s, '=');
        if (!*s || *s == ';' || *s == '#' || *s == '[' || !eq) continue;
        *eq = 0;
        found = !strcasecmp(trim(s), key);
    }
    if (f) fclose(f);
    return found;
}

static const char settings_template[] =
    "; Downhill Revived settings: one \"key = value\" per line. The game adds your disc image (iso)\n"
    "; on the first start, and OPTIONS > PC SETTINGS writes the display settings here. DHD_*\n"
    "; environment variables override them.\n"
    "\n"
    "; renderer: exact (the PS2's graphics in software; the native renderer is not on Linux yet).\n"
    "; scale: the 3D picture's resolution, 1-4 times the PlayStation 2's, or window.\n"
    "; window: windowed (width x height), borderless or fullscreen.\n"
    "renderer = exact\n"
    "scale = 2\n"
    "window = windowed\n"
    "widescreen = 0\n"
    "DHD_ASPECT = 4:3\n"
    "skip_movies = 0\n"
    "; Offline real simulation rate: 60 (original), 120, 144 or monitor (max 144).\n"
    "fps = 60\n"
    "\n"
    "; Online races use the official server, wss://dhd.skuza.app, when server is empty.\n"
    "server =\n"
    "\n"
    "; The updater is off on Linux.\n"
    "auto_update = off\n"
    "\n"
    "font = original\n";

static int settings_save(const char *key, const char *value)
{
    char path[4096], temp[4104], line[1024];
    settings_path(path, sizeof path);
    snprintf(temp, sizeof temp, "%s.new", path);
    FILE *in = fopen(path, "r"), *out = fopen(temp, "w");
    if (!out) { if (in) fclose(in); return 0; }
    const char *quote = strpbrk(value, ";#\"") || value[0] == ' ' ? "\"" : "";
    int written = 0;
    if (!in) fputs(settings_template, out);
    while (in && fgets(line, sizeof line, in)) {
        char copy[1024], *s, *eq;
        snprintf(copy, sizeof copy, "%s", line);
        s = trim(copy);
        if (!written && (eq = strchr(s, '=')) && *s != ';' && *s != '#' && *s != '[') {
            *eq = 0;
            if (!strcasecmp(trim(s), key)) { fprintf(out, "%s = %s%s%s\n", key, quote, value, quote); written = 1; continue; }
        }
        fputs(line, out);
        if (!strchr(line, '\n')) fputc('\n', out);
    }
    if (!written) fprintf(out, "%s = %s%s%s\n", key, quote, value, quote);
    if (in) fclose(in);
    int ok = !ferror(out);
    ok &= fclose(out) == 0;
    if (ok) ok = rename(temp, path) == 0;
    if (!ok) unlink(temp);
    return ok;
}

int host_setting_save(const char *key, const char *value) { return settings_save(key, value); }

int host_game_options_read(char *value, size_t size)
{
    char path[4096], line[1024];
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq = strchr(s, '=');
        if (!eq || *s == ';' || *s == '#' || *s == '[') continue;
        *eq = 0;
        if (!strcasecmp(trim(s), "game_options")) {
            snprintf(value, size, "%s", setting_value(trim(eq + 1)));
            found = 1; break;
        }
    }
    fclose(f);
    return found;
}

/* ---------------- the disc image, as in port/hle/host.c ---------------- */

static uint32_t le32_at(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static const char *disc_problem(const char *path, char *why, size_t size)
{
    enum { SECTOR = 2048 };
    static uint8_t sector[SECTOR];
    static char text[SECTOR + 1];
    FILE *f = fopen(path, "rb");
    if (!f) return "The file can't be opened.";
    const char *problem = NULL;
    uint32_t cnf = 0, cnf_size = 0;
#define READ(lsn) (fseeko(f, (off_t)(lsn) * SECTOR, SEEK_SET) == 0 && fread(sector, SECTOR, 1, f) == 1)
    if (!READ(16) || sector[0] != 1 || memcmp(sector + 1, "CD001", 5)) {
        problem = "It isn't a disc image (no ISO 9660 volume in it).";
        goto done;
    }
    uint32_t dir = le32_at(sector + 156 + 2), dir_size = le32_at(sector + 156 + 10);
    for (uint32_t off = 0; off < dir_size && off < 16 * SECTOR && !cnf; off += SECTOR) {
        if (!READ(dir + off / SECTOR)) break;
        for (unsigned i = 0; i + 33 < SECTOR && sector[i]; i += sector[i]) {
            const uint8_t *r = sector + i;
            if (r[32] >= 10 && i + 33 + r[32] <= SECTOR && !strncasecmp((const char *)r + 33, "SYSTEM.CNF", 10) &&
                (r[32] == 10 || r[33 + 10] == ';')) {
                cnf = le32_at(r + 2);
                cnf_size = le32_at(r + 10);
                break;
            }
        }
    }
    if (!cnf || !READ(cnf)) {
        problem = "It has no SYSTEM.CNF, so it isn't a PlayStation 2 game disc.";
        goto done;
    }
    memcpy(text, sector, SECTOR);
    text[cnf_size < SECTOR ? cnf_size : SECTOR] = 0;
    if (strstr(text, "SCUS_971.77")) {
        problem = game_data_read(path, NULL);
        goto done;
    }
    const char *boot = strstr(text, "BOOT2");
    char id[32] = "";
    if (boot && (boot = strpbrk(boot, "\\:"))) {
        while (*boot == '\\' || *boot == ':') boot++;
        size_t n = strcspn(boot, ";\r\n");
        snprintf(id, sizeof id, "%.*s", (int)(n < 31 ? n : 31), boot);
    }
    snprintf(why, size, "It is another PlayStation 2 disc%s%s%s, not Downhill Domination (USA), SCUS-97177.",
             *id ? " (" : "", id, *id ? ")" : "");
    problem = why;
done:
#undef READ
    fclose(f);
    return problem;
}

static int have_program(const char *name)
{
    char command[128];
    snprintf(command, sizeof command, "command -v %s >/dev/null 2>&1", name);
    return system(command) == 0;
}

/* The desktop's file chooser (zenity or kdialog); 0 if there is none or it was cancelled. */
static int choose_disc(char *path, size_t size)
{
    const char *command =
        have_program("zenity") ? "zenity --file-selection --title='Choose your Downhill Domination (USA) disc image'"
                                 " --file-filter='Disc images (*.iso) | *.iso *.ISO' 2>/dev/null"
        : have_program("kdialog") ? "kdialog --title 'Choose your Downhill Domination (USA) disc image'"
                                    " --getopenfilename . '*.iso *.ISO' 2>/dev/null"
        : NULL;
    if (!command) return 0;
    FILE *p = popen(command, "r");
    if (!p) return 0;
    path[0] = 0;
    if (!fgets(path, (int)size, p)) path[0] = 0;
    pclose(p);
    path[strcspn(path, "\r\n")] = 0;
    return path[0] != 0;
}

/* The disc image dhd.ini names (iso), if it is Downhill Domination (USA); else the file chooser,
   the choice checked and saved to dhd.ini. DHD_ISO in the environment overrides the file.
   Returns 0 to go on, 1 if there is no disc image. */
int host_disc_ready(void)
{
    static char why[256], chosen[4096];
    if (iso_from_environment || getenv("DHD_HEADLESS")) return 0;
    const char *iso = getenv("DHD_ISO");
    const char *problem = iso ? disc_problem(iso, why, sizeof why) : NULL;
    if (iso && !problem) return 0;
    if (iso) fprintf(stderr, "The disc image chosen before can't be used:\n%s\n%s\n", iso, problem);
    for (;;) {
        if (!choose_disc(chosen, sizeof chosen)) {
            char path[4096];
            settings_path(path, sizeof path);
            fprintf(stderr, "Downhill Revived needs your own Downhill Domination (USA) disc image (SCUS-97177), "
                            "as an .iso file.\nPut its path on an \"iso = \" line of %s, or in DHD_ISO.\n", path);
            return 1;
        }
        if (!(problem = disc_problem(chosen, why, sizeof why))) break;
        fprintf(stderr, "%s\n%s\n", chosen, problem);
    }
    setenv("DHD_ISO", chosen, 1);
    hle_log("settings: disc image %s\n", chosen);
    int saved = settings_save("iso", chosen);
    for (unsigned i = 0; i < sizeof player_defaults / sizeof *player_defaults; i++)
        if (!settings_has(player_defaults[i][0])) {
            setting(player_defaults[i][2], player_defaults[i][1]);
            if (saved) settings_save(player_defaults[i][0], player_defaults[i][1]);
        }
    if (!saved) fprintf(stderr, "The disc image can't be saved in dhd.ini; the game will ask again next time.\n");
    return 0;
}
