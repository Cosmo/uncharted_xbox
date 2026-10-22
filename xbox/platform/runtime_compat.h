#pragma once
/* UWP-only adapters. The desktop build retains its existing MinGW path. */
#include <winsock2.h>
#include <windows.h>
#include <memoryapi.h>
#include <fileapifromapp.h>
#include <stdio.h>
#include <io.h>
#include <stdint.h>
#include <time.h>
#include <pthread_compat.h>
#include <sys/stat.h>
#include <pthread_time.h>
#ifndef S_ISDIR
#define S_ISDIR(m) (((m)&_S_IFMT)==_S_IFDIR)
#define S_ISREG(m) (((m)&_S_IFMT)==_S_IFREG)
#endif
#ifndef ELOOP
#define ELOOP 114
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#ifdef __cplusplus
extern "C" {
#endif
FILE *uncharted_uwp_fopen(const char *path,const char *mode);
int uncharted_uwp_open(const char *path,int flags,...);
int uncharted_uwp_stat(const char *path,struct _stat64 *out);
int uncharted_uwp_mkdir(const char *path);
HANDLE uncharted_uwp_mapping(HANDLE file,LPSECURITY_ATTRIBUTES security,ULONG access,ULONG protect,
                       ULONG attributes,ULONG64 bytes,PCWSTR name,MEM_EXTENDED_PARAMETER *params,ULONG count);
DWORD uncharted_uwp_attributes(const char *path);
BOOL uncharted_uwp_delete(const char *path);
BOOL uncharted_uwp_remove_directory(const char *path);
HANDLE uncharted_uwp_find(const char *pattern,WIN32_FIND_DATAA *out);
BOOL uncharted_uwp_find_next(HANDLE handle,WIN32_FIND_DATAA *out);
#ifdef __cplusplus
}
#endif
#ifndef UNCHARTED_UWP_ADAPTER_IMPLEMENTATION
#define fopen uncharted_uwp_fopen
#define open uncharted_uwp_open
#define _stat64(path,out) uncharted_uwp_stat(path,out)
#define _mkdir uncharted_uwp_mkdir
#define VirtualAlloc VirtualAllocFromApp
#define VirtualProtect VirtualProtectFromApp
#define VirtualAlloc2 VirtualAlloc2FromApp
#define MapViewOfFile3 MapViewOfFile3FromApp
#define CreateFileMapping2 uncharted_uwp_mapping
#define GetFileAttributesA uncharted_uwp_attributes
#define DeleteFileA uncharted_uwp_delete
#define RemoveDirectoryA uncharted_uwp_remove_directory
#define FindFirstFileA uncharted_uwp_find
#define FindNextFileA uncharted_uwp_find_next
#endif
