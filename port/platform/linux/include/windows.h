/* The subset of the Win32 API the port's host code uses, for Linux (POSIX threads, mmap, signals).

   Only on the Linux include path (port/tools/link.py): the host files keep including <windows.h>
   and build unchanged. Paths keep Windows' form on the port's side: GetModuleFileName returns one
   with '\\' separators, which the callers cut at '\\', and every file function here (and fopen,
   msvc_compat.h) turns '\\' into '/' before it reaches the system.

   Handles are pointers to a small tagged object (threads, events, files, searches). WINAPI is
   empty: every caller and callee is built by the port with the same convention. */
#ifndef DHD_LINUX_WINDOWS_H
#define DHD_LINUX_WINDOWS_H

#include "msvc_compat.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WINAPI
#define CALLBACK
#define APIENTRY
#define WINAPIV
#define WIN32_LEAN_AND_MEAN_DONE 1

typedef int BOOL;
typedef unsigned char BOOLEAN, BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD, ULONG, UINT;
typedef int32_t LONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG, DWORD64;
typedef size_t SIZE_T;
typedef uintptr_t ULONG_PTR, DWORD_PTR, UINT_PTR;
typedef intptr_t LONG_PTR, INT_PTR;
typedef wchar_t WCHAR;
typedef char CHAR;
typedef void *HANDLE, *HMODULE, *HINSTANCE, *HWND, *HMONITOR, *HGLOBAL, *LPVOID, *PVOID;
typedef const void *LPCVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef DWORD *LPDWORD;

#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_ATTRIBUTE_READONLY 0x01u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_ALREADY_EXISTS 183u
#define CP_ACP 0u
#define CP_UTF8 65001u

#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15

#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE_READWRITE 0x40u
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_RELEASE 0x8000u

#define MB_OK 0x0u
#define MB_ICONERROR 0x10u
#define MB_ICONWARNING 0x30u
#define MB_SETFOREGROUND 0x10000u
#define CF_UNICODETEXT 13u
#define MONITOR_DEFAULTTOPRIMARY 1u

#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_DELETE 0x2E

typedef union {
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER;
typedef union {
    struct { DWORD LowPart; DWORD HighPart; };
    ULONGLONG QuadPart;
} ULARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { LONG x, y; } POINT;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO;
typedef struct { DWORD nLength; void *lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES;

typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char cFileName[MAX_PATH], cAlternateFileName[14];
} WIN32_FIND_DATAA;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    WCHAR cFileName[MAX_PATH], cAlternateFileName[14];
} WIN32_FIND_DATAW;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef enum { FindExInfoStandard, FindExInfoBasic } FINDEX_INFO_LEVELS;
typedef enum { FindExSearchNameMatch } FINDEX_SEARCH_OPS;
#define FIND_FIRST_EX_LARGE_FETCH 2u

/* Locks: a critical section is a recursive mutex, as on Windows. */
typedef pthread_mutex_t CRITICAL_SECTION;
typedef pthread_rwlock_t SRWLOCK;
typedef pthread_cond_t CONDITION_VARIABLE;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
#define CONDITION_VARIABLE_INIT PTHREAD_COND_INITIALIZER

void InitializeCriticalSection(CRITICAL_SECTION *cs);
static inline void DeleteCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_destroy(cs); }
static inline void EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock(cs); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock(cs); }
static inline BOOL TryEnterCriticalSection(CRITICAL_SECTION *cs) { return !pthread_mutex_trylock(cs); }
static inline void InitializeSRWLock(SRWLOCK *l) { pthread_rwlock_init(l, NULL); }
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_wrlock(l); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_unlock(l); }
static inline void AcquireSRWLockShared(SRWLOCK *l) { pthread_rwlock_rdlock(l); }
static inline void ReleaseSRWLockShared(SRWLOCK *l) { pthread_rwlock_unlock(l); }
static inline void InitializeConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_init(c, NULL); }
static inline void WakeConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_signal(c); }
static inline void WakeAllConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_broadcast(c); }
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *c, CRITICAL_SECTION *cs, DWORD ms);
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *c, SRWLOCK *l, DWORD ms, ULONG flags);

/* Interlocked operations: full barriers, as on x86 Windows. */
static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(volatile LONG *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
static inline void *InterlockedExchangePointer(void *volatile *p, void *v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline void *InterlockedCompareExchangePointer(void *volatile *p, void *v, void *cmp)
{
    __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
typedef int64_t LONG64;
static inline LONG64 InterlockedExchangeAdd64(volatile LONG64 *p, LONG64 v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedExchange64(volatile LONG64 *p, LONG64 v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 v, LONG64 cmp)
{
    __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
static inline void MemoryBarrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

/* Threads and events. */
typedef DWORD (*LPTHREAD_START_ROUTINE)(void *);
HANDLE CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *id);
uintptr_t _beginthreadex(void *security, unsigned stack, unsigned (*fn)(void *), void *arg, unsigned flags, unsigned *id);
HANDLE CreateEventA(SECURITY_ATTRIBUTES *sa, BOOL manual_reset, BOOL initial, const char *name);
BOOL SetEvent(HANDLE event);
BOOL ResetEvent(HANDLE event);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
BOOL CloseHandle(HANDLE h);
HANDLE GetCurrentThread(void);
DWORD GetCurrentThreadId(void);
DWORD GetCurrentProcessId(void);
BOOL SetThreadPriority(HANDLE thread, int priority);
BOOL GetExitCodeThread(HANDLE thread, DWORD *code);
void Sleep(DWORD ms);
BOOL QueryPerformanceCounter(LARGE_INTEGER *count);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency);
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
DWORD GetLastError(void);
void SetLastError(DWORD error);
/* CPU time of a thread (GetCurrentThread() or a CreateThread handle), 100-ns units. */
BOOL GetThreadTimes(HANDLE thread, FILETIME *created, FILETIME *exited, FILETIME *kernel, FILETIME *user);
void ExitProcess(UINT code);

typedef struct {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;
/* FILETIME: 100-ns ticks since 1601-01-01 UTC. */
BOOL FileTimeToLocalFileTime(const FILETIME *utc, FILETIME *local);
BOOL FileTimeToSystemTime(const FILETIME *time, SYSTEMTIME *out);
void GetLocalTime(SYSTEMTIME *out);
void GetSystemTimeAsFileTime(FILETIME *out);

/* Files and folders, with '\\' paths. */
DWORD GetModuleFileNameA(HMODULE module, char *path, DWORD size);
DWORD GetModuleFileNameW(HMODULE module, WCHAR *path, DWORD size);
HMODULE GetModuleHandleA(const char *name);
void *GetProcAddress(HMODULE module, const char *name);
DWORD GetFileAttributesA(const char *path);
DWORD GetFileAttributesW(const WCHAR *path);
BOOL GetFileAttributesExA(const char *path, GET_FILEEX_INFO_LEVELS level, void *info);
BOOL CreateDirectoryA(const char *path, SECURITY_ATTRIBUTES *sa);
BOOL DeleteFileA(const char *path);
BOOL MoveFileExA(const char *from, const char *to, DWORD flags);
HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *found);
BOOL FindNextFileA(HANDLE search, WIN32_FIND_DATAA *found);
HANDLE FindFirstFileExW(const WCHAR *pattern, FINDEX_INFO_LEVELS level, void *found, FINDEX_SEARCH_OPS op,
                        void *filter, DWORD flags);
BOOL FindNextFileW(HANDLE search, WIN32_FIND_DATAW *found);
BOOL FindClose(HANDLE search);
HANDLE CreateFileA(const char *path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disposition,
                   DWORD flags, HANDLE templ);
BOOL ReadFile(HANDLE file, void *buffer, DWORD size, DWORD *read, void *overlapped);
BOOL WriteFile(HANDLE file, const void *buffer, DWORD size, DWORD *written, void *overlapped);
BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER *size);
int MultiByteToWideChar(UINT page, DWORD flags, const char *in, int in_size, WCHAR *out, int out_size);
int WideCharToMultiByte(UINT page, DWORD flags, const WCHAR *in, int in_size, char *out, int out_size,
                        const char *fallback, BOOL *used);

/* Memory protection. */
BOOL VirtualProtect(void *address, SIZE_T size, DWORD protection, DWORD *old);

/* Vectored exception handlers, from POSIX signals: SIGSEGV/SIGBUS is an access violation with
   ExceptionInformation[1] the address. A handler returning EXCEPTION_CONTINUE_EXECUTION retries
   the faulting instruction. */
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_SINGLE_STEP 0x80000004u
#define EXCEPTION_FLT_DIVIDE_BY_ZERO 0xC000008Eu
#define EXCEPTION_FLT_INVALID_OPERATION 0xC0000090u
#define EXCEPTION_FLT_OVERFLOW 0xC0000091u
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct EXCEPTION_RECORD {
    DWORD ExceptionCode, ExceptionFlags;
    struct EXCEPTION_RECORD *ExceptionRecord;
    void *ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD;
typedef struct CONTEXT { void *ucontext; } CONTEXT;
typedef struct { EXCEPTION_RECORD *ExceptionRecord; CONTEXT *ContextRecord; } EXCEPTION_POINTERS;
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS *);
void *AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
ULONG RemoveVectoredExceptionHandler(void *handle);

/* Desktop: no monitor query or clipboard yet (SDL takes these over); a message box goes to stderr. */
HMONITOR MonitorFromPoint(POINT point, DWORD flags);
BOOL GetMonitorInfoA(HMONITOR monitor, MONITORINFO *info);
int MessageBoxA(HWND owner, const char *text, const char *caption, UINT type);
short GetAsyncKeyState(int key);
BOOL OpenClipboard(HWND owner);
HANDLE GetClipboardData(UINT format);
BOOL CloseClipboard(void);
void *GlobalLock(HGLOBAL h);
BOOL GlobalUnlock(HGLOBAL h);
SIZE_T GlobalSize(HGLOBAL h);
void OutputDebugStringA(const char *text);

/* The display's refresh rate: unknown here, so callers keep their default (hle/fps.c). */
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
typedef struct { WORD dmSize, dmDriverExtra; DWORD dmFields, dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFrequency; } DEVMODEA;
BOOL EnumDisplaySettingsA(const char *device, DWORD mode, DEVMODEA *out);

#ifdef __cplusplus
}
#endif

#endif
