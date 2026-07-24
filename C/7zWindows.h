#ifndef __7Z_WINDOWS_H
#define __7Z_WINDOWS_H

#ifdef _WIN32
#include <windows.h>
#else
#define WINAPI
typedef void* HANDLE;
typedef long HRESULT;
#endif

#endif