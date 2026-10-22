// Desktop-only Windows functions that the statically linked SDL references although the UWP app
// never reaches the code that calls them: its helper and message windows, process windows, folder
// lookup and URL opening, all in subsystems the build disables or the app does not use.
// The Xbox has none of the desktop DLLs, and a package that imports them does not start, so the
// import pointers (__imp_<name>) are defined here and the calls fail like on a system without them.
// check-imports.ps1 lists any desktop import that is still left.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

namespace {

HWND WINAPI CreateWindowExWFails(DWORD,LPCWSTR,LPCWSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return nullptr;
}
LRESULT WINAPI DefWindowProcWFails(HWND,UINT,WPARAM,LPARAM) { return 0; }
BOOL WINAPI DestroyWindowFails(HWND) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
BOOL WINAPI EnumWindowsFinds(WNDENUMPROC,LPARAM) { return TRUE; }
DWORD WINAPI GetWindowThreadProcessIdFails(HWND,LPDWORD process) { if(process) *process=0; return 0; }
int WINAPI MessageBoxAFails(HWND,LPCSTR,LPCSTR,UINT) { SetLastError(ERROR_NOT_SUPPORTED); return 0; }
BOOL WINAPI PostMessageWFails(HWND,UINT,WPARAM,LPARAM) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
BOOL WINAPI PostThreadMessageWFails(DWORD,UINT,WPARAM,LPARAM) { SetLastError(ERROR_INVALID_THREAD_ID); return FALSE; }
ATOM WINAPI RegisterClassWFails(const WNDCLASSW *) { SetLastError(ERROR_NOT_SUPPORTED); return 0; }
BOOL WINAPI UnregisterClassWFails(LPCWSTR,HINSTANCE) { SetLastError(ERROR_CLASS_DOES_NOT_EXIST); return FALSE; }
HRESULT WINAPI SHGetFolderPathWFails(HWND,int,HANDLE,DWORD,LPWSTR path) { if(path) *path=0; return E_NOTIMPL; }
HINSTANCE WINAPI ShellExecuteWFails(HWND,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,INT) {
    return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(SE_ERR_NOASSOC));
}

}

#define UNCHARTED_DESKTOP_SHIM(name,stub) extern "C" decltype(&name) __imp_##name=stub;
UNCHARTED_DESKTOP_SHIM(CreateWindowExW,CreateWindowExWFails)
UNCHARTED_DESKTOP_SHIM(DefWindowProcW,DefWindowProcWFails)
UNCHARTED_DESKTOP_SHIM(DestroyWindow,DestroyWindowFails)
UNCHARTED_DESKTOP_SHIM(EnumWindows,EnumWindowsFinds)
UNCHARTED_DESKTOP_SHIM(GetWindowThreadProcessId,GetWindowThreadProcessIdFails)
UNCHARTED_DESKTOP_SHIM(MessageBoxA,MessageBoxAFails)
UNCHARTED_DESKTOP_SHIM(PostMessageW,PostMessageWFails)
UNCHARTED_DESKTOP_SHIM(PostThreadMessageW,PostThreadMessageWFails)
UNCHARTED_DESKTOP_SHIM(RegisterClassW,RegisterClassWFails)
UNCHARTED_DESKTOP_SHIM(UnregisterClassW,UnregisterClassWFails)
UNCHARTED_DESKTOP_SHIM(SHGetFolderPathW,SHGetFolderPathWFails)
UNCHARTED_DESKTOP_SHIM(ShellExecuteW,ShellExecuteWFails)
