// Minimal stand-ins for the Windows / D3D12 / DXGI SDK headers, ONLY to syntax-check the glue on Linux.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#define STDMETHODCALLTYPE
#define WINAPI
#define CALLBACK
#define __declspec(x)
typedef int BOOL; typedef unsigned long DWORD; typedef unsigned int UINT; typedef int INT; typedef unsigned long ULONG;
typedef uint64_t UINT64; typedef uint64_t SIZE_T; typedef int32_t HRESULT; typedef void* LPVOID; typedef void* HANDLE;
typedef HANDLE HWND; typedef HANDLE HINSTANCE; typedef HANDLE HMODULE; typedef HANDLE HGLOBAL; typedef HANDLE HFONT; typedef HANDLE HDC; typedef HANDLE HBRUSH; typedef HANDLE HGDIOBJ; typedef HANDLE HMENU;
typedef uint64_t WPARAM; typedef int64_t LPARAM; typedef int64_t LRESULT; typedef uint16_t WORD; typedef wchar_t WCHAR; typedef unsigned char UINT8;
typedef const wchar_t* LPCWSTR; typedef void* FARPROC; typedef int64_t LONGLONG;
typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID;
typedef const GUID& REFIID;
typedef union { struct { DWORD LowPart; int32_t HighPart; }; LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { int left, top, right, bottom; } RECT; typedef struct { int x, y; } POINT; typedef unsigned long COLORREF;
typedef struct { HDC hdc; BOOL fErase; RECT rcPaint; } PAINTSTRUCT;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; } MSG;
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct { UINT style; WNDPROC lpfnWndProc; HINSTANCE hInstance; HBRUSH hbrBackground; LPCWSTR lpszClassName; } WNDCLASSW;
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define SUCCEEDED(h) ((h) >= 0)
#define FAILED(h) ((h) < 0)
#define E_FAIL ((HRESULT)0x80004005)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define PAGE_EXECUTE_READWRITE 0x40
#define CP_UTF8 65001
#define GENERIC_READ 0x80000000
#define GENERIC_WRITE 0x40000000
#define FILE_SHARE_READ 1
#define OPEN_EXISTING 3
#define CREATE_ALWAYS 2
#define FILE_ATTRIBUTE_NORMAL 0x80
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define ERROR_ALREADY_EXISTS 183
#define _SH_DENYNO 0x40
#define WM_TIMER 0x113
#define WM_PAINT 0xF
#define WM_NCHITTEST 0x84
#define WM_ERASEBKGND 0x14
#define WM_QUIT 0x12
#define HTTRANSPARENT (-1)
#define WS_POPUP 0x80000000
#define WS_EX_TOPMOST 8
#define WS_EX_LAYERED 0x80000
#define WS_EX_TRANSPARENT 0x20
#define WS_EX_NOACTIVATE 0x8000000
#define WS_EX_TOOLWINDOW 0x80
#define LWA_COLORKEY 1
#define SW_HIDE 0
#define SWP_NOACTIVATE 0x10
#define SWP_SHOWWINDOW 0x40
#define HWND_TOPMOST ((HWND)(intptr_t)-1)
#define BLACK_BRUSH 4
#define DT_CALCRECT 0x400
#define DT_NOPREFIX 0x800
#define RGB(r,g,b) ((COLORREF)((r)|((g)<<8)|((b)<<16)))
#define FW_BOLD 700
#define DEFAULT_CHARSET 1
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define NONANTIALIASED_QUALITY 3
#define FIXED_PITCH 1
#define FF_MODERN 48
#define TRANSPARENT 1
#define GMEM_MOVEABLE 2
#define CF_TEXT 1
extern "C" {
BOOL VirtualProtect(void*, size_t, DWORD, DWORD*);
DWORD GetModuleFileNameW(HMODULE, wchar_t*, DWORD); UINT GetSystemDirectoryW(wchar_t*, UINT);
HMODULE LoadLibraryW(LPCWSTR); FARPROC GetProcAddress(HMODULE, const char*); BOOL DisableThreadLibraryCalls(HMODULE);
int MultiByteToWideChar(UINT, DWORD, const char*, int, wchar_t*, int); int WideCharToMultiByte(UINT, DWORD, const wchar_t*, int, char*, int, const char*, BOOL*);
HANDLE CreateFileW(LPCWSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE); BOOL GetFileSizeEx(HANDLE, LARGE_INTEGER*);
BOOL ReadFile(HANDLE, void*, DWORD, DWORD*, void*); BOOL WriteFile(HANDLE, const void*, DWORD, DWORD*, void*); BOOL CloseHandle(HANDLE);
BOOL CreateDirectoryW(LPCWSTR, void*); BOOL CreateHardLinkW(LPCWSTR, LPCWSTR, void*); DWORD GetLastError();
void GetLocalTime(SYSTEMTIME*); BOOL QueryPerformanceCounter(LARGE_INTEGER*); BOOL QueryPerformanceFrequency(LARGE_INTEGER*);
HWND GetForegroundWindow(); DWORD GetWindowThreadProcessId(HWND, DWORD*); DWORD GetCurrentProcessId(); short GetAsyncKeyState(int);
BOOL OpenClipboard(HWND); BOOL EmptyClipboard(); HGLOBAL GlobalAlloc(UINT, size_t); void* GlobalLock(HGLOBAL); BOOL GlobalUnlock(HGLOBAL); HANDLE SetClipboardData(UINT, HANDLE); BOOL CloseClipboard();
HMODULE GetModuleHandleW(LPCWSTR); HANDLE CreateThread(void*, size_t, LPTHREAD_START_ROUTINE, LPVOID, DWORD, DWORD*);
WORD RegisterClassW(const WNDCLASSW*); HFONT CreateFontW(int,int,int,int,int,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,LPCWSTR);
HWND CreateWindowExW(DWORD, LPCWSTR, LPCWSTR, DWORD, int,int,int,int, HWND, HMENU, HINSTANCE, LPVOID);
BOOL SetLayeredWindowAttributes(HWND, COLORREF, uint8_t, DWORD); uint64_t SetTimer(HWND, uint64_t, UINT, void*);
BOOL GetMessageW(MSG*, HWND, UINT, UINT); BOOL TranslateMessage(const MSG*); LRESULT DispatchMessageW(const MSG*); LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
HDC BeginPaint(HWND, PAINTSTRUCT*); BOOL EndPaint(HWND, const PAINTSTRUCT*); BOOL GetClientRect(HWND, RECT*); int FillRect(HDC, const RECT*, HBRUSH); HGDIOBJ GetStockObject(int);
int SetBkMode(HDC, int); COLORREF SetTextColor(HDC, COLORREF); HGDIOBJ SelectObject(HDC, HGDIOBJ); int DrawTextA(HDC, const char*, int, RECT*, UINT);
HDC GetDC(HWND); int ReleaseDC(HWND, HDC); BOOL ClientToScreen(HWND, POINT*); BOOL SetWindowPos(HWND, HWND, int,int,int,int, UINT); BOOL ShowWindow(HWND, int);
BOOL InvalidateRect(HWND, const RECT*, BOOL); BOOL IsWindow(HWND); BOOL IsIconic(HWND); BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM);
FILE* _wfsopen(const wchar_t*, const wchar_t*, int); int wcscat_s(wchar_t*, size_t, const wchar_t*);
HRESULT CreateDXGIFactory1(const GUID&, void**);
}
