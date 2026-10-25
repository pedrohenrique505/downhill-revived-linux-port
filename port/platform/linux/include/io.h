/* MSVC <io.h>, for Linux. */
#pragma once
#include <unistd.h>
#define _isatty isatty
#define _fileno fileno
#define _dup dup
#define _dup2 dup2
#define _close close
#define _write write
#define _read read
#define _access access
