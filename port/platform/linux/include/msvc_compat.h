/* MSVC C runtime names the port's host code uses, for Linux. Included before every host file on
   Linux (port/tools/link.py, -include), and by the Linux <windows.h>. */
#ifndef DHD_LINUX_MSVC_COMPAT_H
#define DHD_LINUX_MSVC_COMPAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One calling convention for everything the port builds (clang honours stdcall on i386 Linux). */
#define __stdcall
#define __cdecl
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _snwprintf swprintf
#define _wcsicmp wcscasecmp
/* "NAME=value", or "NAME=" to remove it, as MSVC's _putenv. */
int dhd_putenv(const char *assignment);
#define _putenv dhd_putenv
/* MSVC's _putenv_s: an empty value removes the variable. */
int dhd_putenv_s(const char *name, const char *value);
#define _putenv_s dhd_putenv_s

/* fopen with a '\\' path, as the port builds them (port/platform/linux/win32.c). */
FILE *dhd_fopen(const char *path, const char *mode);
int dhd_remove(const char *path);
int dhd_rename(const char *from, const char *to);
#ifndef DHD_NO_PATH_WRAP
#define fopen dhd_fopen
#define remove dhd_remove
#define rename dhd_rename
#endif

#ifdef __cplusplus
}
#endif

#endif
