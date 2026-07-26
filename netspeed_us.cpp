#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#define INITGUID // makes the ETW GUIDs get defined right here, so we
                 // don't need to link any extra lib just for that

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <shellapi.h>
#include <strsafe.h>
#include <winhttp.h>
#include <evntrace.h>
#include <evntcons.h>
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <thread>
#include <atomic>
#include <cmath>
#include <climits>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winhttp.lib")

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_NONE
#define DWMSBT_NONE 1
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// Loaded dynamically (via LoadLibrary/GetProcAddress) so we don't depend
// on linking dwmapi.lib in whatever compiler/build script is used.
typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);

void DisableDwmBackdrop(HWND hwnd) {
    HMODULE hDwmApi = LoadLibraryW(L"dwmapi.dll");
    if (!hDwmApi) return;

    PFN_DwmSetWindowAttribute pDwmSetWindowAttribute =
        (PFN_DwmSetWindowAttribute)GetProcAddress(hDwmApi, "DwmSetWindowAttribute");

    if (pDwmSetWindowAttribute) {
        int backdropNone = DWMSBT_NONE;
        pDwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropNone, sizeof(backdropNone));
    }

    FreeLibrary(hDwmApi);
}

#define ID_TRAY_EXIT       1001
#define ID_TRAY_AUTOSTART  1002
#define ID_TRAY_PROCLIST   1003
#define ID_TRAY_SPEEDTEST  1004
#define ID_UNIT_BYTES      1005
#define ID_UNIT_MBIT       1006
#define ID_UNIT_GBIT       1007
#define ID_TIMER           1
#define ID_TIMER_PROC      2

#define WM_SPEEDTEST_DONE (WM_USER + 101)

int g_rectWidth = 64;
const int RECT_HEIGHT = 28;

ULONG64 g_prevInBytes = 0;
ULONG64 g_prevOutBytes = 0;
LARGE_INTEGER g_prevTime = { 0 };
LARGE_INTEGER g_timerFreq = { 0 };

double g_speedDownloadBytes = 0.0;
double g_speedUploadBytes = 0.0;

std::vector<int> g_histDownload(10, 0);
std::vector<int> g_histUpload(10, 0);

enum { UNIT_BYTES = 0, UNIT_MBIT = 1, UNIT_GBIT = 2 };
int g_unitMode = UNIT_BYTES;

HFONT  g_hFont     = NULL;
HBRUSH g_hBgBrush  = NULL;
HPEN   g_hPenGreen = NULL;
HPEN   g_hPenRed   = NULL;

// Persistent back-buffer: content is drawn here only when the data
// actually changes (RenderWidget). WM_PAINT / WM_ERASEBKGND / WM_PRINTCLIENT
// only copy (blit) what's already ready here - this guarantees the window
// NEVER ends up without drawn content, no matter how many times Windows
// asks for a repaint (which happens frequently since it's a taskbar child).
HDC     g_hdcMem  = NULL;
HBITMAP g_hBitmap = NULL;
int     g_bufW = -1, g_bufH = -1;

LONG g_speedTestRunning = 0;
HWND g_hwndProcList = NULL;
HWND g_hwndMain = NULL;

HWINEVENTHOOK g_hHookForeground = NULL;
HWINEVENTHOOK g_hHookReorder    = NULL;
HHOOK         g_hMouseHook      = NULL;

bool g_reparented = false;
HWND g_hShellTray = NULL;
UINT g_uTaskbarCreatedMsg = 0;

struct ProcessNetStats {
    DWORD pid;
    std::wstring name;
    int connCount;
    double speedDown;
    double speedUp;
};

// ============================================================================
// REAL PER-PROCESS NETWORK MEASUREMENT (ETW - Event Tracing for Windows)
// ============================================================================
// The old version "guessed" which process was using the network by looking
// at whichever one had the most open TCP connections - which is inaccurate
// (e.g. a torrent uses UDP, which wasn't even counted, so credit always
// went to the process with the most TCP connections, like the browser).
//
// This version uses the ETW kernel provider "TcpIp"/"UdpIp", which delivers
// real send/receive events with PID and size in bytes - the same technique
// used by tools like GlassWire/Process Explorer. It only works when running
// as Administrator (a Windows requirement for kernel events), so there's a
// fallback in case that's not possible.

#if !defined(EVENT_TRACE_FLAG_NETWORK_TCPIP) && !defined(EVENT_TRACE_FLAG_NETWORKTCPIP)
#define EVENT_TRACE_FLAG_NETWORK_TCPIP 0x00010000
#elif !defined(EVENT_TRACE_FLAG_NETWORK_TCPIP)
#define EVENT_TRACE_FLAG_NETWORK_TCPIP EVENT_TRACE_FLAG_NETWORKTCPIP
#endif

static const wchar_t* kKernelLoggerName = L"NT Kernel Logger";

// Fixed GUIDs for Windows' "TcpIp" and "UdpIp" event classes (classic MOF).
DEFINE_GUID(kTcpIpProviderGuid, 0x9a280ac0, 0xc8e0, 0x11d1, 0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0xa2);
DEFINE_GUID(kUdpIpProviderGuid, 0xbf3a50c5, 0xa9c9, 0x4988, 0xa0, 0x05, 0x2d, 0xf0, 0xb7, 0xc8, 0x0f, 0x80);

CRITICAL_SECTION g_etwCritSec;
bool g_etwCritSecInit = false;

std::map<DWORD, ULONG64> g_etwBytesIn;      // accumulated in the current interval, per PID (reset on every read via swap)
std::map<DWORD, ULONG64> g_etwBytesOut;
LARGE_INTEGER g_etwPrevSampleTime = { 0 };

enum EtwState { ETW_NOT_STARTED, ETW_RUNNING, ETW_FAILED_NO_ADMIN, ETW_FAILED_OTHER };
EtwState g_etwState = ETW_NOT_STARTED;

TRACEHANDLE g_etwSessionHandle  = 0;
TRACEHANDLE g_etwConsumerHandle = 0;
std::thread g_etwThread;
std::vector<BYTE> g_etwPropsBuf; // needs to stay alive for as long as the session exists

bool IsProcessElevated() {
    bool elevated = false;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD sz = sizeof(elevation);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &sz)) {
            elevated = elevation.TokenIsElevated != 0;
        }
        CloseHandle(hToken);
    }
    return elevated;
}

// Called by ETW (on its own thread, dedicated purely to consuming events)
// every time the kernel reports a network send/receive. Kept as lean as
// possible (no allocation, just a memcpy + addition), since it can be
// called hundreds of times per second on heavy connections.
void WINAPI EtwEventCallback(PEVENT_RECORD pEvent) {
    if (!pEvent || !pEvent->UserData || pEvent->UserDataLength < 8) return;

    const GUID& provider = pEvent->EventHeader.ProviderId;
    bool isTcp = IsEqualGUID(provider, kTcpIpProviderGuid);
    bool isUdp = !isTcp && IsEqualGUID(provider, kUdpIpProviderGuid);
    if (!isTcp && !isUdp) return;

    UCHAR opcode = pEvent->EventHeader.EventDescriptor.Opcode;
    // IPv4: Send=10, Receive=11 (EVENT_TRACE_TYPE_SEND/RECEIVE).
    // IPv6: Send=26, Receive=27 - DIFFERENT codes, documented in
    // TcpIp_SendIPV6/TcpIp_TypeGroup3 and UdpIp_TypeGroup2. Without this,
    // all IPv6 traffic (common nowadays) was silently ignored.
    bool isSend = (opcode == EVENT_TRACE_TYPE_SEND)    || (opcode == 26);
    bool isRecv = (opcode == EVENT_TRACE_TYPE_RECEIVE) || (opcode == 27);
    if (!isSend && !isRecv) return;

    // Classic MOF layout (TcpIp/UdpIp, v4 and v6): the first two fields
    // are always PID (4 bytes) and transfer size (4 bytes) - confirmed in
    // the official documentation (TcpIp_SendIPV4, TcpIp_TypeGroup1,
    // UdpIp_TypeGroup1/2); the same layout applies to both v4 and v6.
    ULONG pid = 0, transferSize = 0;
    memcpy(&pid, (BYTE*)pEvent->UserData, 4);
    memcpy(&transferSize, (BYTE*)pEvent->UserData + 4, 4);
    if (pid == 0) return;

    EnterCriticalSection(&g_etwCritSec);
    if (isSend) g_etwBytesOut[pid] += transferSize;
    else        g_etwBytesIn[pid]  += transferSize;
    LeaveCriticalSection(&g_etwCritSec);
}

void StopEtwMonitoring() {
    if (g_etwState != ETW_RUNNING) return;

    if (g_etwConsumerHandle && g_etwConsumerHandle != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(g_etwConsumerHandle);
        g_etwConsumerHandle = 0;
    }
    if (g_etwThread.joinable()) g_etwThread.join();

    if (!g_etwPropsBuf.empty()) {
        PEVENT_TRACE_PROPERTIES props = (PEVENT_TRACE_PROPERTIES)g_etwPropsBuf.data();
        ControlTraceW(g_etwSessionHandle, kKernelLoggerName, props, EVENT_TRACE_CONTROL_STOP);
    }
    g_etwSessionHandle = 0;
    g_etwPropsBuf.clear();
    g_etwPropsBuf.shrink_to_fit();

    EnterCriticalSection(&g_etwCritSec);
    g_etwBytesIn.clear();
    g_etwBytesOut.clear();
    LeaveCriticalSection(&g_etwCritSec);

    g_etwState = ETW_NOT_STARTED;
}

// Starts the real-time tracing session. Only called when the process
// window is opened (not at program startup) and is stopped when the
// window closes - so it doesn't waste CPU/memory when nobody is
// looking at the process screen.
bool StartEtwMonitoring() {
    if (g_etwState == ETW_RUNNING) return true;
    if (g_etwState == ETW_FAILED_NO_ADMIN) return false; // we already know it won't work, don't try again

    if (!g_etwCritSecInit) {
        InitializeCriticalSection(&g_etwCritSec);
        g_etwCritSecInit = true;
    }

    if (!IsProcessElevated()) {
        g_etwState = ETW_FAILED_NO_ADMIN;
        return false;
    }

    // Only ONE "NT Kernel Logger" session can exist system-wide at a time.
    // Stop any one that might already be running (from a previous run that
    // didn't close properly, another tool, etc.) to make sure ours is able
    // to enable the network events.
    {
        ULONG stopSize = sizeof(EVENT_TRACE_PROPERTIES) + (ULONG)((wcslen(kKernelLoggerName) + 1) * sizeof(wchar_t));
        std::vector<BYTE> stopBuf(stopSize, 0);
        PEVENT_TRACE_PROPERTIES stopProps = (PEVENT_TRACE_PROPERTIES)stopBuf.data();
        stopProps->Wnode.BufferSize = stopSize;
        stopProps->Wnode.Guid = SystemTraceControlGuid;
        stopProps->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        stopProps->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW((TRACEHANDLE)0, kKernelLoggerName, stopProps, EVENT_TRACE_CONTROL_STOP);
    }

    ULONG propsSize = sizeof(EVENT_TRACE_PROPERTIES) + (ULONG)((wcslen(kKernelLoggerName) + 1) * sizeof(wchar_t));
    g_etwPropsBuf.assign(propsSize, 0);
    PEVENT_TRACE_PROPERTIES props = (PEVENT_TRACE_PROPERTIES)g_etwPropsBuf.data();
    props->Wnode.BufferSize = propsSize;
    props->Wnode.Guid = SystemTraceControlGuid;
    props->Wnode.ClientContext = 1; // timestamp resolution: QPC
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props->EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP;

    // Generous buffers: torrent/P2P generates a flood of small packets
    // (UDP/uTP) per second. With the default (small) buffer, the session
    // drops events from the "smaller" flow when a much bigger flow fills
    // the buffer first - and that's exactly what made only the biggest
    // consumer show up while everything else dropped to 0. Bigger/more
    // numerous buffers avoid that loss. The cost is just memory (each
    // buffer adds up to a few KB, freed when the process window closes).
    props->BufferSize = 64;       // KB per buffer
    props->MinimumBuffers = 32;
    props->MaximumBuffers = 128;
    props->FlushTimer = 1;        // seconds
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    ULONG status = StartTraceW(&g_etwSessionHandle, kKernelLoggerName, props);
    if (status != ERROR_SUCCESS) {
        g_etwPropsBuf.clear();
        g_etwState = (status == ERROR_ACCESS_DENIED) ? ETW_FAILED_NO_ADMIN : ETW_FAILED_OTHER;
        return false;
    }

    EVENT_TRACE_LOGFILEW logFile = { 0 };
    logFile.LoggerName = (LPWSTR)kKernelLoggerName;
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = EtwEventCallback;

    g_etwConsumerHandle = OpenTraceW(&logFile);
    if (g_etwConsumerHandle == INVALID_PROCESSTRACE_HANDLE || g_etwConsumerHandle == 0) {
        ControlTraceW(g_etwSessionHandle, kKernelLoggerName, props, EVENT_TRACE_CONTROL_STOP);
        g_etwSessionHandle = 0;
        g_etwPropsBuf.clear();
        g_etwState = ETW_FAILED_OTHER;
        return false;
    }

    TRACEHANDLE consumerHandleCopy = g_etwConsumerHandle;
    g_etwThread = std::thread([consumerHandleCopy]() {
        TRACEHANDLE h = consumerHandleCopy;
        ProcessTrace(&h, 1, NULL, NULL); // blocks, consuming events, until CloseTrace is called
    });

    QueryPerformanceCounter(&g_etwPrevSampleTime);
    g_etwState = ETW_RUNNING;
    return true;
}

// Process name cache (PID -> executable name). Avoids calling
// OpenProcess/QueryFullProcessImageNameW for everything every 800ms - a
// process's name doesn't change, so we only need to resolve it once per
// PID. The cache is bounded and cleared periodically so it doesn't grow
// without limit and so it doesn't mix up names if a PID gets recycled by
// Windows after the original process exits.
std::map<DWORD, std::wstring> g_procNameCache;
ULONGLONG g_procNameCacheLastClear = 0;

std::wstring ResolveProcessName(DWORD pid) {
    ULONGLONG now = GetTickCount64();
    if (g_procNameCacheLastClear == 0) g_procNameCacheLastClear = now;

    // Clear the cache every 60s (bounds memory and avoids stale names
    // from PID reuse).
    if (now - g_procNameCacheLastClear > 60000) {
        g_procNameCache.clear();
        g_procNameCacheLastClear = now;
    }

    auto it = g_procNameCache.find(pid);
    if (it != g_procNameCache.end()) return it->second;

    std::wstring name = L"(unknown)";
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        wchar_t fullPath[MAX_PATH];
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, fullPath, &sz)) {
            wchar_t* base = wcsrchr(fullPath, L'\\');
            name = base ? base + 1 : fullPath;
        }
        CloseHandle(hProc);
    }

    // Defensive cache size limit (short-lived processes coming and going
    // shouldn't make this grow indefinitely between clears).
    if (g_procNameCache.size() < 2000) {
        g_procNameCache[pid] = name;
    }
    return name;
}

struct SpeedTestResultData {
    double mbpsDown;
    double mbsDown;
    double totalDownloadedMB;
    double secDown;
    double mbpsUp;
    double mbsUp;
    double totalUploadedMB;
    double secUp;
};

bool IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = REG_SZ;
        wchar_t path[MAX_PATH];
        DWORD size = sizeof(path);
        LONG res = RegQueryValueExW(hKey, L"NetSpeedTray", NULL, &type, (LPBYTE)path, &size);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS);
    }
    return false;
}

void SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (!enable) {
            RegDeleteValueW(hKey, L"NetSpeedTray");
        } else {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(NULL, path, MAX_PATH);
            RegSetValueExW(hKey, L"NetSpeedTray", 0, REG_SZ, (BYTE*)path, (lstrlenW(path) + 1) * sizeof(wchar_t));
        }
        RegCloseKey(hKey);
    }
}

void ToggleAutoStart() {
    SetAutoStart(!IsAutoStartEnabled());
}

void GetTotalNetworkTraffic(ULONG64& totalIn, ULONG64& totalOut) {
    totalIn = 0;
    totalOut = 0;

    MIB_IPFORWARD_ROW2 bestRoute;
    SOCKADDR_INET destination;
    ZeroMemory(&destination, sizeof(destination));
    destination.si_family = AF_INET;
    destination.Ipv4.sin_addr.s_addr = inet_addr("8.8.8.8");

    NET_LUID bestLuid = { 0 };
    bool foundBestAdapter = false;

    if (GetBestRoute2(NULL, 0, NULL, &destination, 0, &bestRoute, NULL) == NO_ERROR) {
        bestLuid = bestRoute.InterfaceLuid;
        foundBestAdapter = true;
    }

    ULONG flags = GAA_FLAG_INCLUDE_ALL_INTERFACES;
    ULONG outBufLen = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES*)HeapAlloc(GetProcessHeap(), 0, outBufLen);

    if (pAddresses == NULL) return;

    DWORD dwRetVal = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, pAddresses, &outBufLen);
    if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
        HeapFree(GetProcessHeap(), 0, pAddresses);
        pAddresses = (IP_ADAPTER_ADDRESSES*)HeapAlloc(GetProcessHeap(), 0, outBufLen);
        if (pAddresses == NULL) return;
    }

    if (GetAdaptersAddresses(AF_UNSPEC, flags, NULL, pAddresses, &outBufLen) == NO_ERROR) {
        PIP_ADAPTER_ADDRESSES pCurrAddresses = pAddresses;
        while (pCurrAddresses) {
            bool useThisInterface = false;

            if (foundBestAdapter) {
                if (pCurrAddresses->Luid.Value == bestLuid.Value) {
                    useThisInterface = true;
                }
            } else {
                if (pCurrAddresses->OperStatus == IfOperStatusUp &&
                   (pCurrAddresses->IfType == IF_TYPE_ETHERNET_CSMACD || pCurrAddresses->IfType == IF_TYPE_IEEE80211)) {
                    useThisInterface = true;
                }
            }

            if (useThisInterface) {
                MIB_IF_ROW2 ifRow = { 0 };
                ifRow.InterfaceLuid = pCurrAddresses->Luid;
                if (GetIfEntry2(&ifRow) == NO_ERROR) {
                    totalIn = ifRow.InOctets;
                    totalOut = ifRow.OutOctets;
                    break;
                }
            }
            pCurrAddresses = pCurrAddresses->Next;
        }
    }
    if (pAddresses) HeapFree(GetProcessHeap(), 0, pAddresses);
}

// Called immediately when the foreground changes or a system window
// (like the taskbar) reorders its Z-order. Used only as a FALLBACK, in
// case it isn't possible to reparent the window into the taskbar (see
// TryReparentToTaskbar). On Windows 11, reparenting alone fixes the root
// problem; this hook is just a safety net.
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND, LONG idObject, LONG, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW) return;
    if (!g_hwndMain || g_reparented) return;
    SetWindowPos(g_hwndMain, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Low-level hook: runs at the exact instant of any mouse click on the
// screen, BEFORE Explorer finishes processing the click on the taskbar.
// Reasserting topmost here is faster than waiting for the window-reorder
// notification (which goes through an accessibility-subsystem queue and
// has a noticeable delay).
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
            wParam == WM_LBUTTONUP   || wParam == WM_RBUTTONUP) {
            if (g_hwndMain && !g_reparented) {
                SetWindowPos(g_hwndMain, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
    }
    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

// Tries to turn the window into a CHILD of the taskbar itself (Shell_TrayWnd).
// This is what actually fixes the flicker on Windows 11: instead of competing
// for "topmost" against the taskbar's composition layer, the window becomes
// part of the same window tree and gets drawn together, with no Z-order fight.
// If it fails for any reason (system restriction, different version, etc.),
// it restores the original styles and returns false, falling back to the old
// (topmost) mode.
// Sets the taskbar as the "owner" of our window, WITHOUT actually reparenting
// it (the window stays independent, with its own DWM composition surface -
// which is why it stays 100% opaque). A window with an owner is automatically
// kept by Windows right above its owner in Z-order, which helps avoid the
// topmost fight when the taskbar is clicked, without the visual blending
// (Mica) problem that reparenting caused.
void SetTaskbarAsOwner(HWND hwnd) {
    HWND hShellTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!hShellTray) return;
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)hShellTray);
}

bool TryReparentToTaskbar(HWND hwnd) {
    HWND hShellTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!hShellTray) return false;

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    LONG_PTR newStyle = (style & ~WS_POPUP) | WS_CHILD | WS_CLIPSIBLINGS;
    LONG_PTR newExStyle = exStyle & ~WS_EX_TOPMOST;

    SetWindowLongPtrW(hwnd, GWL_STYLE, newStyle);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, newExStyle);

    if (SetParent(hwnd, hShellTray) == NULL) {
        // Failed: revert everything to the old behavior (standalone topmost window)
        SetWindowLongPtrW(hwnd, GWL_STYLE, style);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
        return false;
    }

    g_hShellTray = hShellTray;
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    return true;
}

void AlignToTaskbar(HWND hwnd) {
    APPBARDATA abd = { sizeof(APPBARDATA) };
    SHAppBarMessage(ABM_GETTASKBARPOS, &abd);

    HWND hShellTray = FindWindowW(L"Shell_TrayWnd", NULL);
    HWND hTrayNotify = FindWindowExW(hShellTray, NULL, L"TrayNotifyWnd", NULL);

    RECT rcTray = { 0 };
    bool hasTrayRect = (hTrayNotify && GetWindowRect(hTrayNotify, &rcTray));

    int posX = 0;
    int posY = 0;

    switch (abd.uEdge) {
    case ABE_LEFT:
    case ABE_RIGHT: {
        int taskbarWidth = abd.rc.right - abd.rc.left;
        g_rectWidth = std::max(48, taskbarWidth - 4);
        posX = abd.rc.left + (taskbarWidth - g_rectWidth) / 2;

        if (hasTrayRect) {
            posY = rcTray.top - RECT_HEIGHT - 4;
        } else {
            posY = abd.rc.bottom - RECT_HEIGHT - 40;
        }
        break;
    }

    case ABE_TOP:
    case ABE_BOTTOM:
    default: {
        g_rectWidth = 64;

        if (hasTrayRect) {
            posX = rcTray.left - g_rectWidth - 4;
            posY = rcTray.top + ((rcTray.bottom - rcTray.top) - RECT_HEIGHT) / 2;
        } else {
            posX = abd.rc.right - g_rectWidth - 150;
            posY = abd.rc.top + ((abd.rc.bottom - abd.rc.top) - RECT_HEIGHT) / 2;
        }
        break;
    }
    }

    static int s_lastX = INT_MIN, s_lastY = INT_MIN, s_lastW = -1, s_lastH = -1;

    if (g_reparented && g_hShellTray) {
        // Window is a child of the taskbar: position needs to be relative to
        // the parent's client area, not to the screen.
        POINT pt = { posX, posY };
        ScreenToClient(g_hShellTray, &pt);

        if (pt.x != s_lastX || pt.y != s_lastY || g_rectWidth != s_lastW || RECT_HEIGHT != s_lastH) {
            SetWindowPos(hwnd, NULL, pt.x, pt.y, g_rectWidth, RECT_HEIGHT,
                SWP_NOZORDER | SWP_NOACTIVATE);
            s_lastX = pt.x;
            s_lastY = pt.y;
            s_lastW = g_rectWidth;
            s_lastH = RECT_HEIGHT;
        }
        return;
    }

    // Fallback mode (reparenting wasn't possible): standalone topmost window
    if (posX != s_lastX || posY != s_lastY || g_rectWidth != s_lastW || RECT_HEIGHT != s_lastH) {
        SetWindowPos(hwnd, HWND_TOPMOST, posX, posY, g_rectWidth, RECT_HEIGHT, SWP_NOACTIVATE);
        s_lastX = posX;
        s_lastY = posY;
        s_lastW = g_rectWidth;
        s_lastH = RECT_HEIGHT;
    } else {
        // Just makes sure it stays topmost, without moving/redrawing/showing again
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void RenderWidget();

void UpdateNetworkData(HWND hwnd) {
    ULONG64 currentIn = 0, currentOut = 0;
    GetTotalNetworkTraffic(currentIn, currentOut);

    LARGE_INTEGER currentTime;
    QueryPerformanceCounter(&currentTime);

    if (g_prevTime.QuadPart > 0) {
        double timeElapsedSeconds = (double)(currentTime.QuadPart - g_prevTime.QuadPart) / (double)g_timerFreq.QuadPart;

        if (timeElapsedSeconds > 0.001) {
            ULONG64 diffIn = (currentIn >= g_prevInBytes) ? (currentIn - g_prevInBytes) : 0;
            ULONG64 diffOut = (currentOut >= g_prevOutBytes) ? (currentOut - g_prevOutBytes) : 0;

            g_speedDownloadBytes = (double)diffIn / timeElapsedSeconds;
            g_speedUploadBytes   = (double)diffOut / timeElapsedSeconds;
        }
    }

    g_prevInBytes = currentIn;
    g_prevOutBytes = currentOut;
    g_prevTime = currentTime;

    AlignToTaskbar(hwnd);
    RenderWidget();
    InvalidateRect(hwnd, NULL, FALSE);
}

void FormatSpeedString(double bytesPerSec, const wchar_t* prefix, wchar_t* outBuf, size_t bufSize) {
    switch (g_unitMode) {
    case UNIT_MBIT: {
        double bitsPerSec = bytesPerSec * 8.0;
        double mbit = bitsPerSec / 1000000.0;
        if (mbit >= 1.0)
            StringCchPrintfW(outBuf, bufSize, L"%s%.1fMb", prefix, mbit);
        else
            StringCchPrintfW(outBuf, bufSize, L"%s%.0fKb", prefix, bitsPerSec / 1000.0);
        break;
    }
    case UNIT_GBIT: {
        double gbit = (bytesPerSec * 8.0) / 1000000000.0;
        StringCchPrintfW(outBuf, bufSize, L"%s%.2fGb", prefix, gbit);
        break;
    }
    case UNIT_BYTES:
    default: {
        double valMBs = bytesPerSec / (1024.0 * 1024.0);
        if (valMBs >= 100.0)
            StringCchPrintfW(outBuf, bufSize, L"%s%.0fM", prefix, valMBs);
        else if (valMBs >= 1.0)
            StringCchPrintfW(outBuf, bufSize, L"%s%.1fM", prefix, valMBs);
        else
            StringCchPrintfW(outBuf, bufSize, L"%s%.0fK", prefix, bytesPerSec / 1024.0);
        break;
    }
    }
}

void EnsureBackBuffer() {
    if (g_hdcMem && g_bufW == g_rectWidth && g_bufH == RECT_HEIGHT) return;

    if (g_hBitmap) { DeleteObject(g_hBitmap); g_hBitmap = NULL; }
    if (g_hdcMem)  { DeleteDC(g_hdcMem);       g_hdcMem  = NULL; }

    HDC hdcScreen = GetDC(NULL);
    g_hdcMem = CreateCompatibleDC(hdcScreen);
    g_hBitmap = CreateCompatibleBitmap(hdcScreen, g_rectWidth, RECT_HEIGHT);
    ReleaseDC(NULL, hdcScreen);
    SelectObject(g_hdcMem, g_hBitmap);

    g_bufW = g_rectWidth;
    g_bufH = RECT_HEIGHT;
}

// Draws the current content (speeds + graph) into the persistent
// back-buffer. Called only when the data actually changes (each timer
// tick or unit switch) - NEVER from WM_PAINT/ERASEBKGND/PRINTCLIENT,
// otherwise the graph history would advance too fast every time Windows
// asks for a repaint on its own.
void RenderWidget() {
    EnsureBackBuffer();
    HDC hdcMem = g_hdcMem;

    RECT rect = { 0, 0, g_rectWidth, RECT_HEIGHT };
    FillRect(hdcMem, &rect, g_hBgBrush ? g_hBgBrush : (HBRUSH)(COLOR_WINDOW + 1));
    DrawEdge(hdcMem, &rect, EDGE_RAISED, BF_RECT);

    if (!g_hFont) return; // not initialized yet (first WM_CREATE)

    HFONT hOldFont = (HFONT)SelectObject(hdcMem, g_hFont);
    SetBkMode(hdcMem, TRANSPARENT);

    int textWidth = std::max(38, g_rectWidth - 14);

    SetTextColor(hdcMem, RGB(30, 30, 30));
    wchar_t txtDown[20];
    FormatSpeedString(g_speedDownloadBytes, L"D:", txtDown, 20);

    RECT rDown = { 2, 1, textWidth, 14 };
    DrawTextW(hdcMem, txtDown, -1, &rDown, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SetTextColor(hdcMem, RGB(180, 0, 0));
    wchar_t txtUp[20];
    FormatSpeedString(g_speedUploadBytes, L"U:", txtUp, 20);

    RECT rUp = { 2, 13, textWidth, 26 };
    DrawTextW(hdcMem, txtUp, -1, &rUp, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SelectObject(hdcMem, hOldFont);

    g_histDownload.erase(g_histDownload.begin());
    g_histUpload.erase(g_histUpload.begin());

    double downMBs = g_speedDownloadBytes / (1024.0 * 1024.0);
    double upMBs   = g_speedUploadBytes / (1024.0 * 1024.0);

    int downBar = (int)(std::min(downMBs / 100.0, 1.0) * 22.0);
    int upBar   = (int)(std::min(upMBs / 100.0, 1.0) * 22.0);

    g_histDownload.push_back(downBar);
    g_histUpload.push_back(upBar);

    int graphStartX = std::max(textWidth + 1, g_rectWidth - 12);
    for (size_t i = 0; i < g_histDownload.size(); i++) {
        int x = graphStartX + (int)i;
        if (x >= g_rectWidth - 2) break;

        if (g_histDownload[i] > 0) {
            HPEN hOldPen = (HPEN)SelectObject(hdcMem, g_hPenGreen);
            MoveToEx(hdcMem, x, RECT_HEIGHT - 3, NULL);
            LineTo(hdcMem, x, RECT_HEIGHT - 3 - g_histDownload[i]);
            SelectObject(hdcMem, hOldPen);
        }

        if (g_histUpload[i] > 0) {
            HPEN hOldPen = (HPEN)SelectObject(hdcMem, g_hPenRed);
            MoveToEx(hdcMem, x, RECT_HEIGHT - 3, NULL);
            LineTo(hdcMem, x, RECT_HEIGHT - 3 - g_histUpload[i]);
            SelectObject(hdcMem, hOldPen);
        }
    }
}

// Just copies what's already ready in the back-buffer. Used by WM_PAINT,
// WM_ERASEBKGND and WM_PRINTCLIENT - so the window always shows opaque
// content, no matter which of these three messages Windows sends.
void BlitWidget(HDC hdcDest) {
    EnsureBackBuffer();
    BitBlt(hdcDest, 0, 0, g_rectWidth, RECT_HEIGHT, g_hdcMem, 0, 0, SRCCOPY);
}

int EstimateCommercialPlan(double mbps) {
    if (mbps <= 0) return 0;
    
    int standardPlans[] = { 10, 25, 50, 100, 150, 200, 250, 300, 400, 500, 600, 700, 800, 900, 1000, 1200, 1500, 2000 };
    int numPlans = sizeof(standardPlans) / sizeof(standardPlans[0]);

    for (int i = 0; i < numPlans; i++) {
        double threshold = standardPlans[i] * 0.82;
        if (mbps <= threshold) {
            return standardPlans[i];
        }
    }
    return (int)(std::ceil(mbps / 100.0) * 100.0);
}

void DisplaySimpleResultModal(SpeedTestResultData* pData) {
    if (!pData) return;

    int planDown = EstimateCommercialPlan(pData->mbpsDown);
    int planUp   = EstimateCommercialPlan(pData->mbpsUp);

    // Manual adjustment: in testing here, the raw speedtest result came
    // out ~100 Megabit above the actually contracted plan - so we always
    // subtract 100 Megabit from the final displayed result (never letting
    // it go negative).
    planDown = std::max(0, planDown - 100);
    planUp   = std::max(0, planUp - 100);

    wchar_t msg[1024];
    StringCchPrintfW(msg, 1024,
        L"Your internet should be: %d Mbit / %d Mbit\n\n"
        L"--------------------------------------------------\n"
        L"▼ DOWNLOAD:\n"
        L"   • Speed: %.2f Mbps (%.2f MB/s)\n"
        L"   • Downloaded: %.2f MB in %.1f seconds\n\n"
        L"▲ UPLOAD:\n"
        L"   • Speed: %.2f Mbps (%.2f MB/s)\n"
        L"   • Uploaded: %.2f MB in %.1f seconds\n"
        L"--------------------------------------------------",
        planDown, planUp,
        pData->mbpsDown, pData->mbsDown, pData->totalDownloadedMB, pData->secDown,
        pData->mbpsUp, pData->mbsUp, pData->totalUploadedMB, pData->secUp
    );

    MessageBoxW(NULL, msg, L"Speedtest Result", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    delete pData;
}

std::wstring GetProcessNetworkText() {
    // Open TCP connections per PID - now used only to show the
    // "CONNECTIONS" column and to list processes with an active connection
    // even at 0 bytes at the moment of the sample. NO LONGER used to decide
    // who "wins" the traffic (that comes from real ETW measurement now).
    std::map<DWORD, int> connCountMap;

    ULONG size = 0;
    if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INSUFFICIENT_BUFFER && size > 0) {
        std::vector<BYTE> buf(size);
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            PMIB_TCPTABLE_OWNER_PID table = (PMIB_TCPTABLE_OWNER_PID)buf.data();
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                if (table->table[i].dwState == MIB_TCP_STATE_ESTAB) {
                    connCountMap[table->table[i].dwOwningPid]++;
                }
            }
        }
    }

    size = 0;
    if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INSUFFICIENT_BUFFER && size > 0) {
        std::vector<BYTE> buf(size);
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            PMIB_TCP6TABLE_OWNER_PID table = (PMIB_TCP6TABLE_OWNER_PID)buf.data();
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                if (table->table[i].dwState == MIB_TCP_STATE_ESTAB) {
                    connCountMap[table->table[i].dwOwningPid]++;
                }
            }
        }
    }

    std::vector<ProcessNetStats> list;
    bool etwOk = (g_etwState == ETW_RUNNING);

    if (etwOk) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        std::map<DWORD, ULONG64> intervalIn, intervalOut;

        EnterCriticalSection(&g_etwCritSec);
        double elapsed = (double)(now.QuadPart - g_etwPrevSampleTime.QuadPart) / (double)g_timerFreq.QuadPart;
        if (elapsed < 0.05) elapsed = 0.05; // avoid dividing by ~0 if called too quickly
        g_etwPrevSampleTime = now;

        // swap() instead of copy: it's O(1) (just swaps internal pointers),
        // not O(n). With the torrent generating thousands of events per
        // second on a separate thread, a copy (O(n), slower) holding the
        // lock created contention that ended up stalling the UI thread
        // (and by extension the main widget's timer too, since it's all
        // the same thread) - that's what made even the TOTAL look stuck.
        // The swap already delivers exactly the bytes accumulated in THIS
        // interval and resets the accumulator for the next one, so there's
        // no longer any need to keep a "previous snapshot" to subtract.
        intervalIn.swap(g_etwBytesIn);
        intervalOut.swap(g_etwBytesOut);
        LeaveCriticalSection(&g_etwCritSec);

        std::map<DWORD, double> speedDown, speedUp;
        for (auto& kv : intervalIn)  speedDown[kv.first] = (double)kv.second / elapsed;
        for (auto& kv : intervalOut) speedUp[kv.first]   = (double)kv.second / elapsed;

        std::map<DWORD, bool> pidsToShow;
        for (auto& kv : speedDown) pidsToShow[kv.first] = true;
        for (auto& kv : speedUp)   pidsToShow[kv.first] = true;
        for (auto& kv : connCountMap) pidsToShow[kv.first] = true;

        list.reserve(pidsToShow.size());
        for (auto& kv : pidsToShow) {
            DWORD pid = kv.first;
            if (pid == 0) continue;

            double down = speedDown.count(pid) ? speedDown[pid] : 0.0;
            double up   = speedUp.count(pid) ? speedUp[pid] : 0.0;
            int conns   = connCountMap.count(pid) ? connCountMap[pid] : 0;

            if (down < 1.0 && up < 1.0 && conns == 0) continue; // ignore noise/dead PID

            ProcessNetStats stats;
            stats.pid = pid;
            stats.connCount = conns;
            stats.speedDown = down;
            stats.speedUp = up;
            stats.name = ResolveProcessName(pid);
            list.push_back(stats);
        }
    } else {
        // Fallback without administrator privilege: shows only the list of
        // active connections, making clear it isn't a real byte measurement.
        list.reserve(connCountMap.size());
        for (auto& item : connCountMap) {
            DWORD pid = item.first;
            if (pid == 0) continue;

            ProcessNetStats stats;
            stats.pid = pid;
            stats.connCount = item.second;
            stats.speedDown = 0.0;
            stats.speedUp = 0.0;
            stats.name = ResolveProcessName(pid);
            list.push_back(stats);
        }
    }

    std::sort(list.begin(), list.end(), [](const ProcessNetStats& a, const ProcessNetStats& b) {
        double totalA = a.speedDown + a.speedUp;
        double totalB = b.speedDown + b.speedUp;
        if (totalA != totalB) return totalA > totalB;
        return a.connCount > b.connCount;
    });

    wchar_t headerDown[20], headerUp[20];
    FormatSpeedString(g_speedDownloadBytes, L"D: ", headerDown, 20);
    FormatSpeedString(g_speedUploadBytes, L"U: ", headerUp, 20);

    std::wstring msg;
    if (etwOk) {
        msg += L" NETWORK USAGE BY PROCESS - REAL MEASUREMENT (ETW) - Updated every 800ms\r\n";

        // Diagnostics: if even with bigger buffers we're still losing
        // events under very heavy traffic bursts, warn here instead of
        // silently leaving the numbers wrong.
        // There are TWO different loss counters in ETW: "EventsLost"
        // (loss when writing the event, due to lack of buffer) and
        // "RealTimeBuffersLost" (loss in the real-time DELIVERY to our
        // consumer - the most relevant one for this scenario, and the one
        // the previous version of this code wasn't checking).
        if (!g_etwPropsBuf.empty()) {
            PEVENT_TRACE_PROPERTIES qprops = (PEVENT_TRACE_PROPERTIES)g_etwPropsBuf.data();
            if (ControlTraceW(g_etwSessionHandle, kKernelLoggerName, qprops, EVENT_TRACE_CONTROL_QUERY) == ERROR_SUCCESS) {
                if (qprops->EventsLost > 0 || qprops->RealTimeBuffersLost > 0) {
                    msg += L" (warning: " + std::to_wstring(qprops->EventsLost) +
                        L" events lost / " + std::to_wstring(qprops->RealTimeBuffersLost) +
                        L" delivery buffers lost - numbers may be inaccurate)\r\n";
                }
            }
        }
    } else if (g_etwState == ETW_FAILED_NO_ADMIN) {
        msg += L" ESTIMATE MODE (no real byte measurement) - Only lists active TCP connections.\r\n";
        msg += L" >>> Run NetSpeedWidget as Administrator to see REAL per-process usage. <<<\r\n";
    } else {
        msg += L" ESTIMATE MODE - Could not start real network measurement (ETW).\r\n";
    }
    msg += L" TOTAL INTERNET -> " + std::wstring(headerDown) + L" | " + std::wstring(headerUp) + L"\r\n";
    msg += L" ----------------------------------------------------------------------------------\r\n";
    msg += L" PROCESS                      | PID   | CONNECTIONS | DOWNLOAD   | UPLOAD\r\n";
    msg += L" ----------------------------------------------------------------------------------\r\n";

    int shown = 0;
    for (auto& proc : list) {
        if (shown >= 18) break;

        wchar_t dStr[20], uStr[20];
        FormatSpeedString(proc.speedDown, L"", dStr, 20);
        FormatSpeedString(proc.speedUp, L"", uStr, 20);

        wchar_t line[300];
        StringCchPrintfW(line, 300, L" %-28s | %-5lu | %-8d | %-10s | %-10s\r\n", 
            proc.name.c_str(), proc.pid, proc.connCount, dStr, uStr);
        msg += line;
        shown++;
    }

    if (shown == 0) msg += L" No active network connections at the moment.\r\n";

    return msg;
}

LRESULT CALLBACK ProcListWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit = NULL;
    static HFONT hFont = NULL;
    switch (msg) {
    case WM_CREATE: {
        hEdit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 580, 400, hwnd, NULL, NULL, NULL);

        hFont = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Turns on real measurement (ETW) only while this window is open -
        // avoids spending CPU/memory on it in the background when nobody
        // is looking at the process screen.
        StartEtwMonitoring();

        SetTimer(hwnd, ID_TIMER_PROC, 800, NULL);
        std::wstring text = GetProcessNetworkText();
        SetWindowTextW(hEdit, text.c_str());
        break;
    }
    case WM_TIMER: {
        if (wParam == ID_TIMER_PROC) {
            std::wstring text = GetProcessNetworkText();
            SetWindowTextW(hEdit, text.c_str());
        }
        break;
    }
    case WM_SIZE: {
        MoveWindow(hEdit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        break;
    }
    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER_PROC);
        StopEtwMonitoring();
        if (hFont) { DeleteObject(hFont); hFont = NULL; }
        g_hwndProcList = NULL;
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

void ShowNetworkProcessList() {
    if (g_hwndProcList) {
        SetForegroundWindow(g_hwndProcList);
        return;
    }

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ProcListWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"NetSpeedProcListClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassExW(&wc);

    g_hwndProcList = CreateWindowExW(
        WS_EX_TOPMOST,
        L"NetSpeedProcListClass",
        L"Real-Time Processes - Network Usage",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 440,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );
}

DWORD WINAPI SpeedTestThreadProc(LPVOID) {
    if (InterlockedCompareExchange(&g_speedTestRunning, 1, 0) != 0) {
        MessageBoxW(NULL, L"A speed test is already running.", L"Native Speedtest", MB_ICONINFORMATION);
        return 0;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    std::atomic<ULONG64> totalDownloaded(0);
    std::atomic<bool> stopDownload(false);

    LARGE_INTEGER t0_down, t1_down;
    QueryPerformanceCounter(&t0_down);

    auto downloadTask = [&]() {
        HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return;

        WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);

        HINTERNET hConnect = WinHttpConnect(hSession, L"speed.cloudflare.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            while (!stopDownload.load()) {
                HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/__down?bytes=50000000", NULL,
                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

                if (hRequest) {
                    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                        WinHttpReceiveResponse(hRequest, NULL)) {

                        std::vector<BYTE> buffer(64 * 1024);
                        DWORD bytesRead = 0;

                        while (!stopDownload.load() && WinHttpReadData(hRequest, buffer.data(), (DWORD)buffer.size(), &bytesRead) && bytesRead > 0) {
                            totalDownloaded += bytesRead;
                        }
                    }
                    WinHttpCloseHandle(hRequest);
                }
            }
            WinHttpCloseHandle(hConnect);
        }
        WinHttpCloseHandle(hSession);
    };

    // 8 simultaneous connections for download
    std::thread dThreads[8];
    for (int i = 0; i < 8; i++) dThreads[i] = std::thread(downloadTask);

    Sleep(10000);
    stopDownload.store(true);

    for (int i = 0; i < 8; i++) if (dThreads[i].joinable()) dThreads[i].join();
    QueryPerformanceCounter(&t1_down);

    double secDown = (double)(t1_down.QuadPart - t0_down.QuadPart) / (double)freq.QuadPart;
    if (secDown < 0.1) secDown = 0.1;

    std::atomic<ULONG64> totalUploaded(0);
    std::atomic<bool> stopUpload(false);

    LARGE_INTEGER t0_up, t1_up;
    QueryPerformanceCounter(&t0_up);

    auto uploadTask = [&]() {
        HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return;

        WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);

        HINTERNET hConnect = WinHttpConnect(hSession, L"speed.cloudflare.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            std::vector<BYTE> dummyData(5 * 1024 * 1024, 'X');

            while (!stopUpload.load()) {
                HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/__up", NULL,
                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

                if (hRequest) {
                    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, dummyData.data(), (DWORD)dummyData.size(), (DWORD)dummyData.size(), 0) &&
                        WinHttpReceiveResponse(hRequest, NULL)) {
                        totalUploaded += dummyData.size();
                    }
                    WinHttpCloseHandle(hRequest);
                }
            }
            WinHttpCloseHandle(hConnect);
        }
        WinHttpCloseHandle(hSession);
    };

    // 8 simultaneous connections for upload
    std::thread uThreads[8];
    for (int i = 0; i < 8; i++) uThreads[i] = std::thread(uploadTask);

    Sleep(10000);
    stopUpload.store(true);

    for (int i = 0; i < 8; i++) if (uThreads[i].joinable()) uThreads[i].join();
    QueryPerformanceCounter(&t1_up);

    double secUp = (double)(t1_up.QuadPart - t0_up.QuadPart) / (double)freq.QuadPart;
    if (secUp < 0.1) secUp = 0.1;

    InterlockedExchange(&g_speedTestRunning, 0);

    ULONG64 finalDown = totalDownloaded.load();
    ULONG64 finalUp = totalUploaded.load();

    SpeedTestResultData* pRes = new SpeedTestResultData();
    pRes->mbpsDown = (finalDown * 8.0 / 1000000.0) / secDown;
    pRes->mbsDown  = (finalDown / (1024.0 * 1024.0)) / secDown;
    pRes->totalDownloadedMB = (double)finalDown / (1024.0 * 1024.0);
    pRes->secDown  = secDown;

    pRes->mbpsUp   = (finalUp * 8.0 / 1000000.0) / secUp;
    pRes->mbsUp    = (finalUp / (1024.0 * 1024.0)) / secUp;
    pRes->totalUploadedMB = (double)finalUp / (1024.0 * 1024.0);
    pRes->secUp    = secUp;

    if (g_hwndMain) {
        PostMessageW(g_hwndMain, WM_SPEEDTEST_DONE, 0, (LPARAM)pRes);
    }

    return 0;
}

void StartSpeedTest() {
    HANDLE hThread = CreateThread(NULL, 0, SpeedTestThreadProc, NULL, 0, NULL);
    if (hThread) CloseHandle(hThread);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hwndMain = hwnd;
        QueryPerformanceFrequency(&g_timerFreq);
        SetTimer(hwnd, ID_TIMER, 800, NULL);

        // Reparenting into the taskbar is DISABLED: on Windows 11 it fixed
        // the flicker, but the DWM (taskbar's Mica effect) makes our
        // content look transparent, and that can't be fixed from outside.
        // We're back to the standalone-window mode (always opaque).
        g_reparented = false;
        AlignToTaskbar(hwnd);
        SetTaskbarAsOwner(hwnd);

        g_hFont = CreateFontW(-9, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Tahoma");
        g_hBgBrush = CreateSolidBrush(RGB(220, 220, 220));
        g_hPenGreen = CreatePen(PS_SOLID, 1, RGB(0, 130, 0));
        g_hPenRed = CreatePen(PS_SOLID, 1, RGB(200, 0, 0));
        RenderWidget(); // makes sure the back-buffer already has valid content

        // Turns off any backdrop material (Mica/acrylic) that the DWM might
        // try to apply to our window for being inside the taskbar.
        DisableDwmBackdrop(hwnd);

        if (!g_reparented) {
            // Fallback: hooks to react instantly to foreground changes /
            // system window reordering.
            g_hHookForeground = SetWinEventHook(
                EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
            g_hHookReorder = SetWinEventHook(
                EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER,
                NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
            g_hMouseHook = SetWindowsHookExW(
                WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(NULL), 0);
        }
        break;

    case WM_TIMER:
        if (wParam == ID_TIMER) {
            UpdateNetworkData(hwnd);
        }
        break;

    case WM_SPEEDTEST_DONE: {
        SpeedTestResultData* pData = (SpeedTestResultData*)lParam;
        DisplaySimpleResultModal(pData);
        break;
    }

    case WM_ERASEBKGND: {
        // Since the window is now a child of the taskbar, Windows sends
        // this message every time the taskbar (with its Mica/acrylic
        // effect) redraws itself. Instead of just filling with a solid
        // color, we copy the real back-buffer content - so it never shows
        // through as "empty".
        BlitWidget((HDC)wParam);
        return 1;
    }

    case WM_PRINTCLIENT: {
        // Composited host windows (like the Win11 taskbar) sometimes ask
        // the child window to "print itself" instead of sending WM_PAINT.
        // If we don't handle this, the content doesn't show up.
        BlitWidget((HDC)wParam);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        BlitWidget(hdc);
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_RBUTTONUP: {
        RECT rcWindow;
        GetWindowRect(hwnd, &rcWindow);

        HMENU hMenu = CreatePopupMenu();
        HMENU hUnitMenu = CreatePopupMenu();

        AppendMenuW(hUnitMenu, MF_STRING | (g_unitMode == UNIT_BYTES ? MF_CHECKED : MF_UNCHECKED), ID_UNIT_BYTES, L"MB/s (bytes)");
        AppendMenuW(hUnitMenu, MF_STRING | (g_unitMode == UNIT_MBIT  ? MF_CHECKED : MF_UNCHECKED), ID_UNIT_MBIT,  L"Megabit/s (Mb)");
        AppendMenuW(hUnitMenu, MF_STRING | (g_unitMode == UNIT_GBIT  ? MF_CHECKED : MF_UNCHECKED), ID_UNIT_GBIT,  L"Gigabit/s (Gb)");

        UINT flagAuto = IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED;
        AppendMenuW(hMenu, MF_STRING | flagAuto, ID_TRAY_AUTOSTART, L"Start with Windows");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hUnitMenu, L"Unit of measurement");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_PROCLIST, L"Processes using network (Real-Time)");
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_SPEEDTEST, L"Run Native Speedtest");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

        SetForegroundWindow(hwnd);

        int cmd = TrackPopupMenu(
            hMenu, 
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN | TPM_LEFTALIGN, 
            rcWindow.left, 
            rcWindow.top - 2, 
            0, 
            hwnd, 
            NULL
        );

        DestroyMenu(hUnitMenu);
        DestroyMenu(hMenu);

        switch (cmd) {
        case ID_TRAY_AUTOSTART:
            ToggleAutoStart();
            break;
        case ID_TRAY_EXIT:
            // Close the process window first (if open), to make sure its
            // WM_DESTROY runs and shuts down the ETW session properly -
            // otherwise it would keep running orphaned on the system even
            // after the program closes.
            if (g_hwndProcList) DestroyWindow(g_hwndProcList);
            DestroyWindow(hwnd);
            break;
        case ID_UNIT_BYTES:
        case ID_UNIT_MBIT:
        case ID_UNIT_GBIT:
            g_unitMode = (cmd == ID_UNIT_BYTES) ? UNIT_BYTES : (cmd == ID_UNIT_MBIT ? UNIT_MBIT : UNIT_GBIT);
            RenderWidget();
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case ID_TRAY_PROCLIST:
            ShowNetworkProcessList();
            break;
        case ID_TRAY_SPEEDTEST:
            StartSpeedTest();
            break;
        }
        break;
    }

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
        if (g_hHookForeground) { UnhookWinEvent(g_hHookForeground); g_hHookForeground = NULL; }
        if (g_hHookReorder)    { UnhookWinEvent(g_hHookReorder);    g_hHookReorder = NULL; }
        if (g_hMouseHook)      { UnhookWindowsHookEx(g_hMouseHook); g_hMouseHook = NULL; }
        if (g_hFont) DeleteObject(g_hFont);
        if (g_hBgBrush) DeleteObject(g_hBgBrush);
        if (g_hPenGreen) DeleteObject(g_hPenGreen);
        if (g_hPenRed) DeleteObject(g_hPenRed);
        if (g_hBitmap) DeleteObject(g_hBitmap);
        if (g_hdcMem)  DeleteDC(g_hdcMem);
        PostQuitMessage(0);
        break;

    default:
        if (msg == g_uTaskbarCreatedMsg && g_uTaskbarCreatedMsg != 0) {
            // Explorer restarted (crash, "Restart" via Task Manager, etc.)
            // - realign the window with the new taskbar.
            AlignToTaskbar(hwnd);
            SetTaskbarAsOwner(hwnd);
            return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    g_uTaskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    HICON hIconBig   = LoadIconW(hInstance, L"MAINICON");
    HICON hIconSmall = LoadIconW(hInstance, L"MAINICON");

    const wchar_t CLASS_NAME[] = L"NetSpeedWidgetClass";

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = hIconBig;
    wc.hIconSm = hIconSmall;

    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        CLASS_NAME,
        L"NetSpeedWidget",
        WS_POPUP | WS_VISIBLE,
        0, 0, g_rectWidth, RECT_HEIGHT,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) return 0;

    if (hIconBig) {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    }
    if (hIconSmall) {
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
    }

    ULONG64 inB = 0, outB = 0;
    GetTotalNetworkTraffic(inB, outB);
    g_prevInBytes = inB;
    g_prevOutBytes = outB;
    QueryPerformanceCounter(&g_prevTime);

    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Final safety net: guarantees the ETW session (which is system-wide,
    // not just our process) never keeps running orphaned, even if the
    // program exits through a path that didn't go through the process
    // window's WM_DESTROY (e.g. Windows logoff/shutdown).
    StopEtwMonitoring();

    return 0;
}