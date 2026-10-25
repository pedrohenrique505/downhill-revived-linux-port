/* The SHA-256 part of Windows' CNG (bcrypt), for Linux: the port hashes files with it
   (hle/online.c, hle/online_menu.c). Implemented in port/platform/linux/win32.c. */
#ifndef DHD_LINUX_BCRYPT_H
#define DHD_LINUX_BCRYPT_H

#include <windows.h>

typedef LONG NTSTATUS;
typedef void *BCRYPT_ALG_HANDLE, *BCRYPT_HASH_HANDLE;
#define BCRYPT_SHA256_ALGORITHM L"SHA256"

NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *alg, const WCHAR *id, const WCHAR *impl, ULONG flags);
NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE alg, ULONG flags);
NTSTATUS BCryptCreateHash(BCRYPT_ALG_HANDLE alg, BCRYPT_HASH_HANDLE *hash, BYTE *object, ULONG object_size,
                          BYTE *secret, ULONG secret_size, ULONG flags);
NTSTATUS BCryptHashData(BCRYPT_HASH_HANDLE hash, const BYTE *data, ULONG size, ULONG flags);
NTSTATUS BCryptFinishHash(BCRYPT_HASH_HANDLE hash, BYTE *digest, ULONG size, ULONG flags);
NTSTATUS BCryptDestroyHash(BCRYPT_HASH_HANDLE hash);

#endif
