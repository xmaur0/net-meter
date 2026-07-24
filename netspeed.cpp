#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <shellapi.h>
#include <strsafe.h>
#include <iostream>
#include <vector>
#include <algorithm>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

#define ID_TRAY_EXIT 1001
#define ID_TRAY_AUTOSTART 1002
#define ID_TIMER 1

// Largura dinâmica (ajustada automaticamente) e altura fixa
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

// Alinhamento e Redimensionamento Dinâmico conforme a espessura da Barra
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
        // Barra Vertical: Ajusta a LARGURA do widget automaticamente ao tamanho da barra
        int taskbarWidth = abd.rc.right - abd.rc.left;
        
        // Define a largura do retângulo com margem de 4px (2px de cada lado)
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
        // Barra Horizontal: Usa largura padrão de 64px
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

    SetWindowPos(hwnd, HWND_TOPMOST, posX, posY, g_rectWidth, RECT_HEIGHT, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void UpdateNetworkData(HWND hwnd) {
    ULONG64 currentIn = 0, currentOut = 0;
    GetTotalNetworkTraffic(currentIn, currentOut);

    LARGE_INTEGER currentTime;
    QueryPerformanceCounter(&currentTime);

    if (g_prevInBytes > 0 && g_prevOutBytes > 0 && g_prevTime.QuadPart > 0) {
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
    InvalidateRect(hwnd, NULL, FALSE);
}

void FormatSpeedString(double bytesPerSec, const wchar_t* prefix, wchar_t* outBuf, size_t bufSize) {
    double valMBs = bytesPerSec / (1024.0 * 1024.0);

    if (valMBs >= 100.0)
        StringCchPrintfW(outBuf, bufSize, L"%s%.0fM", prefix, valMBs);
    else if (valMBs >= 1.0)
        StringCchPrintfW(outBuf, bufSize, L"%s%.1fM", prefix, valMBs);
    else
        StringCchPrintfW(outBuf, bufSize, L"%s%.0fK", prefix, bytesPerSec / 1024.0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        QueryPerformanceFrequency(&g_timerFreq);
        SetTimer(hwnd, ID_TIMER, 1000, NULL);
        AlignToTaskbar(hwnd);
        break;

    case WM_TIMER:
        if (wParam == ID_TIMER) {
            UpdateNetworkData(hwnd);
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hBitmap = CreateCompatibleBitmap(hdc, g_rectWidth, RECT_HEIGHT);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBitmap);

        HBRUSH hBgBrush = CreateSolidBrush(RGB(220, 220, 220));
        RECT rect = { 0, 0, g_rectWidth, RECT_HEIGHT };
        FillRect(hdcMem, &rect, hBgBrush);
        DeleteObject(hBgBrush);

        DrawEdge(hdcMem, &rect, EDGE_RAISED, BF_RECT);

        HFONT hFont = CreateFontW(-9, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Tahoma");
        HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);

        SetBkMode(hdcMem, TRANSPARENT);

        // Calcula dinamicamente a área do texto e do gráfico baseada na largura atual
        int textWidth = std::max(38, g_rectWidth - 14);

        // Download
        SetTextColor(hdcMem, RGB(30, 30, 30));
        wchar_t txtDown[20];
        FormatSpeedString(g_speedDownloadBytes, L"D:", txtDown, 20);

        RECT rDown = { 2, 1, textWidth, 14 };
        DrawTextW(hdcMem, txtDown, -1, &rDown, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        // Upload
        SetTextColor(hdcMem, RGB(180, 0, 0));
        wchar_t txtUp[20];
        FormatSpeedString(g_speedUploadBytes, L"U:", txtUp, 20);

        RECT rUp = { 2, 13, textWidth, 26 };
        DrawTextW(hdcMem, txtUp, -1, &rUp, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        SelectObject(hdcMem, hOldFont);
        DeleteObject(hFont);

        // Atualiza histórico do gráfico
        g_histDownload.erase(g_histDownload.begin());
        g_histUpload.erase(g_histUpload.begin());

        double downMBs = g_speedDownloadBytes / (1024.0 * 1024.0);
        double upMBs   = g_speedUploadBytes / (1024.0 * 1024.0);

        int downBar = (int)(std::min(downMBs / 100.0, 1.0) * 22.0);
        int upBar   = (int)(std::min(upMBs / 100.0, 1.0) * 22.0);

        g_histDownload.push_back(downBar);
        g_histUpload.push_back(upBar);

        HPEN hPenG = CreatePen(PS_SOLID, 1, RGB(0, 130, 0));
        HPEN hPenR = CreatePen(PS_SOLID, 1, RGB(200, 0, 0));

        // Posição x do gráfico ajustada dinamicamente à borda direita
        int graphStartX = std::max(textWidth + 1, g_rectWidth - 12);
        for (size_t i = 0; i < g_histDownload.size(); i++) {
            int x = graphStartX + (int)i;
            if (x >= g_rectWidth - 2) break;

            if (g_histDownload[i] > 0) {
                HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPenG);
                MoveToEx(hdcMem, x, RECT_HEIGHT - 3, NULL);
                LineTo(hdcMem, x, RECT_HEIGHT - 3 - g_histDownload[i]);
                SelectObject(hdcMem, hOldPen);
            }

            if (g_histUpload[i] > 0) {
                HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPenR);
                MoveToEx(hdcMem, x, RECT_HEIGHT - 3, NULL);
                LineTo(hdcMem, x, RECT_HEIGHT - 3 - g_histUpload[i]);
                SelectObject(hdcMem, hOldPen);
            }
        }

        DeleteObject(hPenG);
        DeleteObject(hPenR);

        BitBlt(hdc, 0, 0, g_rectWidth, RECT_HEIGHT, hdcMem, 0, 0, SRCCOPY);

        SelectObject(hdcMem, hOldBmp);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_RBUTTONUP: {
        POINT pt;
        GetCursorPos(&pt);
        HMENU hMenu = CreatePopupMenu();

        UINT flagAuto = IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED;
        AppendMenuW(hMenu, MF_STRING | flagAuto, ID_TRAY_AUTOSTART, L"Iniciar com o Windows");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Sair");

        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(hMenu);

        if (cmd == ID_TRAY_AUTOSTART) { 
            ToggleAutoStart(); 
        }
        else if (cmd == ID_TRAY_EXIT) { 
            DestroyWindow(hwnd); 
        }
        break;
    }

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
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
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
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

    HWND hShellTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hShellTray) {
        SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, (LONG_PTR)hShellTray);
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

    return 0;
}