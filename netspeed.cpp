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

// Menu IDs
#define ID_TRAY_EXIT       1001
#define ID_TRAY_AUTOSTART  1002
#define ID_TRAY_PROCLIST   1003
#define ID_TRAY_SPEEDTEST  1004
#define ID_UNIT_BYTES      1005
#define ID_UNIT_MBIT       1006
#define ID_UNIT_GBIT       1007
#define ID_TIMER           1
#define ID_TIMER_PROC      2

// Idiomas (10 no total)
#define ID_LANG_PT         1010
#define ID_LANG_EN         1011
#define ID_LANG_ES         1012
#define ID_LANG_FR         1013
#define ID_LANG_DE         1014
#define ID_LANG_IT         1015
#define ID_LANG_RU         1016
#define ID_LANG_JA         1017
#define ID_LANG_ZH         1018
#define ID_LANG_KO         1019

// Escalas
#define ID_SCALE_NORMAL    1020
#define ID_SCALE_MEDIUM    1021
#define ID_SCALE_LARGE     1022

#define WM_SPEEDTEST_DONE (WM_USER + 101)

// Enumeradores de Idioma e Escala
enum Language { 
    LANG_PT = 0, LANG_EN, LANG_ES, LANG_FR, LANG_DE, 
    LANG_IT, LANG_RU, LANG_JA, LANG_ZH, LANG_KO 
};
enum UIScale { SCALE_NORMAL = 0, SCALE_MEDIUM, SCALE_LARGE };

Language g_language = LANG_PT;
UIScale  g_scale    = SCALE_NORMAL;

// Fator global de redução de 30% aplicado a todas as escalas
const double K_SCALE_REDUCTION = 0.70;

// Variáveis dinâmicas de dimensionamento
int g_rectWidth = 71;
int g_rectHeight = 31;
int g_fontSizeWidget = -11;

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

// Map de Dicionário Multilingue Ampliado (10 Idiomas)
std::map<std::wstring, std::map<Language, std::wstring>> g_dict = {
    {L"START_WIN", {
        {LANG_PT, L"Iniciar com o Windows"}, {LANG_EN, L"Start with Windows"}, {LANG_ES, L"Iniciar con Windows"},
        {LANG_FR, L"Démarrer avec Windows"}, {LANG_DE, L"Mit Windows starten"}, {LANG_IT, L"Avvia con Windows"},
        {LANG_RU, L"Запускать с Windows"}, {LANG_JA, L"Windows起動時に実行"}, {LANG_ZH, L"随 Windows 启动"}, {LANG_KO, L"Windows 시작 시 실행"}
    }},
    {L"UNIT_MENU", {
        {LANG_PT, L"Unidade de medida"}, {LANG_EN, L"Unit of measurement"}, {LANG_ES, L"Unidad de medida"},
        {LANG_FR, L"Unité de mesure"}, {LANG_DE, L"Messeinheit"}, {LANG_IT, L"Unità di misura"},
        {LANG_RU, L"Единицы измерения"}, {LANG_JA, L"計測単位"}, {LANG_ZH, L"测量单位"}, {LANG_KO, L"측정 단위"}
    }},
    {L"LANG_MENU", {
        {LANG_PT, L"Idioma"}, {LANG_EN, L"Language"}, {LANG_ES, L"Idioma"},
        {LANG_FR, L"Langue"}, {LANG_DE, L"Sprache"}, {LANG_IT, L"Lingua"},
        {LANG_RU, L"Язык"}, {LANG_JA, L"言語"}, {LANG_ZH, L"语言"}, {LANG_KO, L"언어"}
    }},
    {L"SCALE_MENU", {
        {LANG_PT, L"Escala do Menu / Janelas"}, {LANG_EN, L"Menu / Window Scale"}, {LANG_ES, L"Escala de Menú / Ventanas"},
        {LANG_FR, L"Échelle de la fenêtre"}, {LANG_DE, L"Fensterskalierung"}, {LANG_IT, L"Scala Menu / Finestre"},
        {LANG_RU, L"Масштаб интерфейса"}, {LANG_JA, L"UIスケール"}, {LANG_ZH, L"界面缩放"}, {LANG_KO, L"인터페이스 크기"}
    }},
    {L"SCALE_NORM", {
        {LANG_PT, L"Normal (-30%)"}, {LANG_EN, L"Normal (-30%)"}, {LANG_ES, L"Normal (-30%)"},
        {LANG_FR, L"Normal (-30%)"}, {LANG_DE, L"Normal (-30%)"}, {LANG_IT, L"Normale (-30%)"},
        {LANG_RU, L"Обычный (-30%)"}, {LANG_JA, L"標準 (-30%)"}, {LANG_ZH, L"标准 (-30%)"}, {LANG_KO, L"보통 (-30%)"}
    }},
    {L"SCALE_MED", {
        {LANG_PT, L"Médio (-16%)"}, {LANG_EN, L"Medium (-16%)"}, {LANG_ES, L"Mediano (-16%)"},
        {LANG_FR, L"Moyen (-16%)"}, {LANG_DE, L"Mittel (-16%)"}, {LANG_IT, L"Medio (-16%)"},
        {LANG_RU, L"Средний (-16%)"}, {LANG_JA, L"中 (-16%)"}, {LANG_ZH, L"中 (-16%)"}, {LANG_KO, L"중간 (-16%)"}
    }},
    {L"SCALE_LRG", {
        {LANG_PT, L"Grande (-2%)"}, {LANG_EN, L"Large (-2%)"}, {LANG_ES, L"Grande (-2%)"},
        {LANG_FR, L"Grand (-2%)"}, {LANG_DE, L"Groß (-2%)"}, {LANG_IT, L"Grande (-2%)"},
        {LANG_RU, L"Крупный (-2%)"}, {LANG_JA, L"大 (-2%)"}, {LANG_ZH, L"大 (-2%)"}, {LANG_KO, L"크게 (-2%)"}
    }},
    {L"PROC_MENU", {
        {LANG_PT, L"Processos utilizando rede (Tempo Real)"}, {LANG_EN, L"Processes using network (Real-Time)"},
        {LANG_ES, L"Procesos usando la red (Tiempo Real)"}, {LANG_FR, L"Processus utilisant le réseau"},
        {LANG_DE, L"Prozesse mit Netzwerknutzung"}, {LANG_IT, L"Processi che usano la rete"},
        {LANG_RU, L"Сетевые процессы (Реальное время)"}, {LANG_JA, L"ネットワーク使用プロセス"},
        {LANG_ZH, L"网络使用进程 (实时)"}, {LANG_KO, L"실시간 네트워크 프로세스"}
    }},
    {L"TEST_MENU", {
        {LANG_PT, L"Executar Teste de Velocidade"}, {LANG_EN, L"Run Native Speedtest"}, {LANG_ES, L"Ejecutar Prueba de Velocidad"},
        {LANG_FR, L"Lancer le Test de Vitesse"}, {LANG_DE, L"Speedtest ausführen"}, {LANG_IT, L"Esegui Test di Velocità"},
        {LANG_RU, L"Запустить тест скорости"}, {LANG_JA, L"スピードテストを実行"}, {LANG_ZH, L"运行网络测速"}, {LANG_KO, L"속도 테스트 실행"}
    }},
    {L"EXIT_MENU", {
        {LANG_PT, L"Sair"}, {LANG_EN, L"Exit"}, {LANG_ES, L"Salir"},
        {LANG_FR, L"Quitter"}, {LANG_DE, L"Beenden"}, {LANG_IT, L"Esci"},
        {LANG_RU, L"Выход"}, {LANG_JA, L"終了"}, {LANG_ZH, L"退出"}, {LANG_KO, L"종료"}
    }},
    {L"PROC_TITLE", {
        {LANG_PT, L"Processos em Tempo Real - Uso da Rede"}, {LANG_EN, L"Real-Time Processes - Network Usage"},
        {LANG_ES, L"Procesos en Tiempo Real - Uso de Red"}, {LANG_FR, L"Processus en temps réel - Réseau"},
        {LANG_DE, L"Echtzeit-Prozesse - Netzwerknutzung"}, {LANG_IT, L"Processi in tempo reale - Uso rete"},
        {LANG_RU, L"Сетевая активность процессов в реальном времени"}, {LANG_JA, L"リアルタイム ネットワーク プロセス"},
        {LANG_ZH, L"实时进程网络占用"}, {LANG_KO, L"실시간 프로세스 네트워크 사용량"}
    }},
    {L"TEST_TITLE", {
        {LANG_PT, L"Resultado do Teste de Velocidade"}, {LANG_EN, L"Speedtest Result"}, {LANG_ES, L"Resultado de Prueba de Velocidad"},
        {LANG_FR, L"Résultat du Test de Vitesse"}, {LANG_DE, L"Speedtest-Ergebnis"}, {LANG_IT, L"Risultato Test di Velocità"},
        {LANG_RU, L"Результат теста скорости"}, {LANG_JA, L"スピードテスト結果"}, {LANG_ZH, L"测速结果"}, {LANG_KO, L"속도 테스트 결과"}
    }},
    {L"PLAN_EST", {
        {LANG_PT, L"Sua internet contratada deve ser:"}, {LANG_EN, L"Your internet plan should be:"},
        {LANG_ES, L"Su plan de internet debería ser:"}, {LANG_FR, L"Votre abonnement internet devrait être:"},
        {LANG_DE, L"Ihr Internet-Tarif sollte sein:"}, {LANG_IT, L"Il tuo piano internet dovrebbe essere:"},
        {LANG_RU, L"Ваш тарифный план интернета:"}, {LANG_JA, L"推定契約回線速度:"},
        {LANG_ZH, L"您的签约宽带预估为:"}, {LANG_KO, L"추정 인터넷 요금제 속도:"}
    }},
    {L"NO_CONN", {
        {LANG_PT, L" Nenhuma conexão de rede ativa no momento.\r\n"}, {LANG_EN, L" No active network connections at the moment.\r\n"},
        {LANG_ES, L" No hay conexiones de red activas en este momento.\r\n"}, {LANG_FR, L" Aucune connexion réseau active pour le moment.\r\n"},
        {LANG_DE, L" Derzeit keine aktiven Netzwerkverbindungen.\r\n"}, {LANG_IT, L" Nessuna connessione di rete attiva al momento.\r\n"},
        {LANG_RU, L" Нет активных сетевых соединений.\r\n"}, {LANG_JA, L" アクティブなネットワーク接続がありません。\r\n"},
        {LANG_ZH, L" 当前无活动的网络连接。\r\n"}, {LANG_KO, L" 현재 활성화된 네트워크 연결이 없습니다.\r\n"}
    }}
};

std::wstring Tr(const std::wstring& key) {
    if (g_dict.count(key) && g_dict[key].count(g_language)) {
        return g_dict[key][g_language];
    }
    return key;
}

// Salva e Carrega Configurações no Registro
void SaveSettings() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\NetSpeedTray", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD lang = (DWORD)g_language;
        DWORD scale = (DWORD)g_scale;
        DWORD unit = (DWORD)g_unitMode;
        RegSetValueExW(hKey, L"Language", 0, REG_DWORD, (BYTE*)&lang, sizeof(lang));
        RegSetValueExW(hKey, L"Scale", 0, REG_DWORD, (BYTE*)&scale, sizeof(scale));
        RegSetValueExW(hKey, L"UnitMode", 0, REG_DWORD, (BYTE*)&unit, sizeof(unit));
        RegCloseKey(hKey);
    }
}

void LoadSettings() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\NetSpeedTray", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD lang = 0, scale = 0, unit = 0, sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"Language", NULL, NULL, (LPBYTE)&lang, &sz) == ERROR_SUCCESS) g_language = (Language)lang;
        if (RegQueryValueExW(hKey, L"Scale", NULL, NULL, (LPBYTE)&scale, &sz) == ERROR_SUCCESS) g_scale = (UIScale)scale;
        if (RegQueryValueExW(hKey, L"UnitMode", NULL, NULL, (LPBYTE)&unit, &sz) == ERROR_SUCCESS) g_unitMode = (int)unit;
        RegCloseKey(hKey);
    }
}

typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);

void DisableDwmBackdrop(HWND hwnd) {
    HMODULE hDwmApi = LoadLibraryW(L"dwmapi.dll");
    if (!hDwmApi) return;
    PFN_DwmSetWindowAttribute pDwmSetWindowAttribute = (PFN_DwmSetWindowAttribute)GetProcAddress(hDwmApi, "DwmSetWindowAttribute");
    if (pDwmSetWindowAttribute) {
        int backdropNone = DWMSBT_NONE;
        pDwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropNone, sizeof(backdropNone));
    }
    FreeLibrary(hDwmApi);
}

#if !defined(EVENT_TRACE_FLAG_NETWORK_TCPIP) && !defined(EVENT_TRACE_FLAG_NETWORKTCPIP)
#define EVENT_TRACE_FLAG_NETWORK_TCPIP 0x00010000
#elif !defined(EVENT_TRACE_FLAG_NETWORK_TCPIP)
#define EVENT_TRACE_FLAG_NETWORK_TCPIP EVENT_TRACE_FLAG_NETWORKTCPIP
#endif

static const wchar_t* kKernelLoggerName = L"NT Kernel Logger";
DEFINE_GUID(kTcpIpProviderGuid, 0x9a280ac0, 0xc8e0, 0x11d1, 0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0xa2);
DEFINE_GUID(kUdpIpProviderGuid, 0xbf3a50c5, 0xa9c9, 0x4988, 0xa0, 0x05, 0x2d, 0xf0, 0xb7, 0xc8, 0x0f, 0x80);

CRITICAL_SECTION g_etwCritSec;
bool g_etwCritSecInit = false;
std::map<DWORD, ULONG64> g_etwBytesIn;
std::map<DWORD, ULONG64> g_etwBytesOut;
LARGE_INTEGER g_etwPrevSampleTime = { 0 };

enum EtwState { ETW_NOT_STARTED, ETW_RUNNING, ETW_FAILED_NO_ADMIN, ETW_FAILED_OTHER };
EtwState g_etwState = ETW_NOT_STARTED;

TRACEHANDLE g_etwSessionHandle  = 0;
TRACEHANDLE g_etwConsumerHandle = 0;
std::thread g_etwThread;
std::vector<BYTE> g_etwPropsBuf;

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

void WINAPI EtwEventCallback(PEVENT_RECORD pEvent) {
    if (!pEvent || !pEvent->UserData || pEvent->UserDataLength < 8) return;
    const GUID& provider = pEvent->EventHeader.ProviderId;
    bool isTcp = IsEqualGUID(provider, kTcpIpProviderGuid);
    bool isUdp = !isTcp && IsEqualGUID(provider, kUdpIpProviderGuid);
    if (!isTcp && !isUdp) return;

    UCHAR opcode = pEvent->EventHeader.EventDescriptor.Opcode;
    bool isSend = (opcode == EVENT_TRACE_TYPE_SEND)    || (opcode == 26);
    bool isRecv = (opcode == EVENT_TRACE_TYPE_RECEIVE) || (opcode == 27);
    if (!isSend && !isRecv) return;

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

bool StartEtwMonitoring() {
    if (g_etwState == ETW_RUNNING) return true;
    if (g_etwState == ETW_FAILED_NO_ADMIN) return false;

    if (!g_etwCritSecInit) {
        InitializeCriticalSection(&g_etwCritSec);
        g_etwCritSecInit = true;
    }
    if (!IsProcessElevated()) {
        g_etwState = ETW_FAILED_NO_ADMIN;
        return false;
    }

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
    props->Wnode.ClientContext = 1;
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props->EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP;
    props->BufferSize = 64;
    props->MinimumBuffers = 32;
    props->MaximumBuffers = 128;
    props->FlushTimer = 1;
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
        ProcessTrace(&h, 1, NULL, NULL);
    });

    QueryPerformanceCounter(&g_etwPrevSampleTime);
    g_etwState = ETW_RUNNING;
    return true;
}

std::map<DWORD, std::wstring> g_procNameCache;
ULONGLONG g_procNameCacheLastClear = 0;

std::wstring ResolveProcessName(DWORD pid) {
    ULONGLONG now = GetTickCount64();
    if (g_procNameCacheLastClear == 0) g_procNameCacheLastClear = now;
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

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND, LONG idObject, LONG, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW) return;
    if (!g_hwndMain || g_reparented) return;
    SetWindowPos(g_hwndMain, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
            wParam == WM_LBUTTONUP   || wParam == WM_RBUTTONUP) {
            if (g_hwndMain && !g_reparented) {
                SetWindowPos(g_hwndMain, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
    }
    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

void SetTaskbarAsOwner(HWND hwnd) {
    HWND hShellTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!hShellTray) return;
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)hShellTray);
}

// Atualização de Dimensões (Fonte +1 tamanho maior: alterada base de -15 para -16)
void UpdateScaleDimensions() {
    double baseScale = (g_scale == SCALE_LARGE) ? 1.4 : ((g_scale == SCALE_MEDIUM) ? 1.2 : 1.0);
    double factor = baseScale * K_SCALE_REDUCTION;

    g_rectWidth = (int)(102 * factor);
    g_rectHeight = (int)(45 * factor);
    
    // Aumentado em +1pt o tamanho da fonte (-16)
    g_fontSizeWidget = (int)(-16 * factor);

    if (g_fontSizeWidget > -10) g_fontSizeWidget = -10;

    if (g_hFont) DeleteObject(g_hFont);
    g_hFont = CreateFontW(g_fontSizeWidget, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Tahoma");
}

void AlignToTaskbar(HWND hwnd) {
    APPBARDATA abd = { sizeof(APPBARDATA) };
    SHAppBarMessage(ABM_GETTASKBARPOS, &abd);

    HWND hShellTray = FindWindowW(L"Shell_TrayWnd", NULL);
    HWND hTrayNotify = FindWindowExW(hShellTray, NULL, L"TrayNotifyWnd", NULL);

    RECT rcTray = { 0 };
    bool hasTrayRect = (hTrayNotify && GetWindowRect(hTrayNotify, &rcTray));

    int posX = 0, posY = 0;

    switch (abd.uEdge) {
    case ABE_LEFT:
    case ABE_RIGHT: {
        int taskbarWidth = abd.rc.right - abd.rc.left;
        g_rectWidth = std::max(35, taskbarWidth - 4);
        posX = abd.rc.left + (taskbarWidth - g_rectWidth) / 2;
        posY = hasTrayRect ? (rcTray.top - g_rectHeight - 4) : (abd.rc.bottom - g_rectHeight - 40);
        break;
    }
    case ABE_TOP:
    case ABE_BOTTOM:
    default: {
        if (hasTrayRect) {
            posX = rcTray.left - g_rectWidth - 4;
            posY = rcTray.top + ((rcTray.bottom - rcTray.top) - g_rectHeight) / 2;
        } else {
            posX = abd.rc.right - g_rectWidth - 150;
            posY = abd.rc.top + ((abd.rc.bottom - abd.rc.top) - g_rectHeight) / 2;
        }
        break;
    }
    }

    SetWindowPos(hwnd, HWND_TOPMOST, posX, posY, g_rectWidth, g_rectHeight, SWP_NOACTIVATE);
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
        if (mbit >= 1.0) StringCchPrintfW(outBuf, bufSize, L"%s%.1fMb", prefix, mbit);
        else StringCchPrintfW(outBuf, bufSize, L"%s%.0fKb", prefix, bitsPerSec / 1000.0);
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
        if (valMBs >= 100.0) StringCchPrintfW(outBuf, bufSize, L"%s%.0fM", prefix, valMBs);
        else if (valMBs >= 1.0) StringCchPrintfW(outBuf, bufSize, L"%s%.1fM", prefix, valMBs);
        else StringCchPrintfW(outBuf, bufSize, L"%s%.0fK", prefix, bytesPerSec / 1024.0);
        break;
    }
    }
}

void EnsureBackBuffer() {
    if (g_hdcMem && g_bufW == g_rectWidth && g_bufH == g_rectHeight) return;

    if (g_hBitmap) { DeleteObject(g_hBitmap); g_hBitmap = NULL; }
    if (g_hdcMem)  { DeleteDC(g_hdcMem);       g_hdcMem  = NULL; }

    HDC hdcScreen = GetDC(NULL);
    g_hdcMem = CreateCompatibleDC(hdcScreen);
    g_hBitmap = CreateCompatibleBitmap(hdcScreen, g_rectWidth, g_rectHeight);
    ReleaseDC(NULL, hdcScreen);
    SelectObject(g_hdcMem, g_hBitmap);

    g_bufW = g_rectWidth;
    g_bufH = g_rectHeight;
}

void RenderWidget() {
    EnsureBackBuffer();
    HDC hdcMem = g_hdcMem;

    RECT rect = { 0, 0, g_rectWidth, g_rectHeight };
    FillRect(hdcMem, &rect, g_hBgBrush ? g_hBgBrush : (HBRUSH)(COLOR_WINDOW + 1));
    DrawEdge(hdcMem, &rect, EDGE_RAISED, BF_RECT);

    if (!g_hFont) return;

    HFONT hOldFont = (HFONT)SelectObject(hdcMem, g_hFont);
    SetBkMode(hdcMem, TRANSPARENT);

    int textWidth = std::max(25, g_rectWidth - (int)(10 * (g_rectWidth / 71.0)));
    int halfH = g_rectHeight / 2;

    SetTextColor(hdcMem, RGB(30, 30, 30));
    wchar_t txtDown[20];
    FormatSpeedString(g_speedDownloadBytes, L"D:", txtDown, 20);
    RECT rDown = { 2, 2, textWidth, halfH + 1 };
    DrawTextW(hdcMem, txtDown, -1, &rDown, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SetTextColor(hdcMem, RGB(180, 0, 0));
    wchar_t txtUp[20];
    FormatSpeedString(g_speedUploadBytes, L"U:", txtUp, 20);
    RECT rUp = { 2, halfH - 1, textWidth, g_rectHeight - 1 };
    DrawTextW(hdcMem, txtUp, -1, &rUp, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SelectObject(hdcMem, hOldFont);

    g_histDownload.erase(g_histDownload.begin());
    g_histUpload.erase(g_histUpload.begin());

    double downMBs = g_speedDownloadBytes / (1024.0 * 1024.0);
    double upMBs   = g_speedUploadBytes / (1024.0 * 1024.0);

    int maxBarH = std::max(2, g_rectHeight - 6);
    int downBar = (int)(std::min(downMBs / 100.0, 1.0) * maxBarH);
    int upBar   = (int)(std::min(upMBs / 100.0, 1.0) * maxBarH);

    g_histDownload.push_back(downBar);
    g_histUpload.push_back(upBar);

    int graphStartX = std::max(textWidth + 1, g_rectWidth - 10);
    for (size_t i = 0; i < g_histDownload.size(); i++) {
        int x = graphStartX + (int)i;
        if (x >= g_rectWidth - 2) break;

        if (g_histDownload[i] > 0) {
            HPEN hOldPen = (HPEN)SelectObject(hdcMem, g_hPenGreen);
            MoveToEx(hdcMem, x, g_rectHeight - 2, NULL);
            LineTo(hdcMem, x, g_rectHeight - 2 - g_histDownload[i]);
            SelectObject(hdcMem, hOldPen);
        }

        if (g_histUpload[i] > 0) {
            HPEN hOldPen = (HPEN)SelectObject(hdcMem, g_hPenRed);
            MoveToEx(hdcMem, x, g_rectHeight - 2, NULL);
            LineTo(hdcMem, x, g_rectHeight - 2 - g_histUpload[i]);
            SelectObject(hdcMem, hOldPen);
        }
    }
}

void BlitWidget(HDC hdcDest) {
    EnsureBackBuffer();
    BitBlt(hdcDest, 0, 0, g_rectWidth, g_rectHeight, g_hdcMem, 0, 0, SRCCOPY);
}

int EstimateCommercialPlan(double mbps) {
    if (mbps <= 0) return 0;
    int standardPlans[] = { 10, 25, 50, 100, 150, 200, 250, 300, 400, 500, 600, 700, 800, 900, 1000, 1200, 1500, 2000 };
    int numPlans = sizeof(standardPlans) / sizeof(standardPlans[0]);

    for (int i = 0; i < numPlans; i++) {
        double threshold = standardPlans[i] * 0.82;
        if (mbps <= threshold) return standardPlans[i];
    }
    return (int)(std::ceil(mbps / 100.0) * 100.0);
}

#define ID_RESULT_OK 2001

LRESULT CALLBACK SpeedTestResultWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HFONT hFont = NULL;
    static HFONT hFontButton = NULL;
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        wchar_t* text = (wchar_t*)cs->lpCreateParams;

        double baseScale = (g_scale == SCALE_LARGE) ? 1.4 : ((g_scale == SCALE_MEDIUM) ? 1.2 : 1.0);
        double factor = baseScale * K_SCALE_REDUCTION;

        hFont = CreateFontW((int)(-24 * factor), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        hFontButton = CreateFontW((int)(-22 * factor), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        HWND hEdit = CreateWindowExW(0, L"EDIT", text,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_LEFT,
            (int)(20 * factor), (int)(20 * factor), (int)(760 * factor), (int)(480 * factor), hwnd, NULL, NULL, NULL);
        SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hBtn = CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            (int)(330 * factor), (int)(520 * factor), (int)(140 * factor), (int)(50 * factor), hwnd, (HMENU)ID_RESULT_OK, NULL, NULL);
        SendMessage(hBtn, WM_SETFONT, (WPARAM)hFontButton, TRUE);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_RESULT_OK) DestroyWindow(hwnd);
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        if (hFont) { DeleteObject(hFont); hFont = NULL; }
        if (hFontButton) { DeleteObject(hFontButton); hFontButton = NULL; }
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

void DisplaySimpleResultModal(SpeedTestResultData* pData) {
    if (!pData) return;

    int planDown = EstimateCommercialPlan(pData->mbpsDown);
    int planUp   = EstimateCommercialPlan(pData->mbpsUp);

    planDown = std::max(0, planDown - 100);
    planUp   = std::max(0, planUp - 100);

    wchar_t* msg = new wchar_t[1024];
    StringCchPrintfW(msg, 1024,
        L"%s\r\n%d Mbit / %d Mbit\r\n\r\n"
        L"--------------------------------------------------\r\n"
        L"DOWNLOAD:\r\n"
        L"  Speed: %.2f Mbps (%.2f MB/s)\r\n"
        L"  Downloaded: %.2f MB in %.1f seconds\r\n\r\n"
        L"UPLOAD:\r\n"
        L"  Speed: %.2f Mbps (%.2f MB/s)\r\n"
        L"  Uploaded: %.2f MB in %.1f seconds\r\n"
        L"--------------------------------------------------",
        Tr(L"PLAN_EST").c_str(), planDown, planUp,
        pData->mbpsDown, pData->mbsDown, pData->totalDownloadedMB, pData->secDown,
        pData->mbpsUp, pData->mbsUp, pData->totalUploadedMB, pData->secUp
    );

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc = { 0 };
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = SpeedTestResultWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"NetSpeedResultClass";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    double baseScale = (g_scale == SCALE_LARGE) ? 1.4 : ((g_scale == SCALE_MEDIUM) ? 1.2 : 1.0);
    double factor = baseScale * K_SCALE_REDUCTION;

    HWND hResult = CreateWindowExW(
        WS_EX_TOPMOST,
        L"NetSpeedResultClass",
        Tr(L"TEST_TITLE").c_str(),
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, (int)(820 * factor), (int)(620 * factor),
        NULL, NULL, GetModuleHandle(NULL), msg
    );

    if (hResult) SetForegroundWindow(hResult);

    delete[] msg;
    delete pData;
}

std::wstring GetProcessNetworkText() {
    std::map<DWORD, int> connCountMap;
    ULONG size = 0;
    if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INSUFFICIENT_BUFFER && size > 0) {
        std::vector<BYTE> buf(size);
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            PMIB_TCPTABLE_OWNER_PID table = (PMIB_TCPTABLE_OWNER_PID)buf.data();
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                if (table->table[i].dwState == MIB_TCP_STATE_ESTAB) connCountMap[table->table[i].dwOwningPid]++;
            }
        }
    }

    size = 0;
    if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INSUFFICIENT_BUFFER && size > 0) {
        std::vector<BYTE> buf(size);
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            PMIB_TCP6TABLE_OWNER_PID table = (PMIB_TCP6TABLE_OWNER_PID)buf.data();
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                if (table->table[i].dwState == MIB_TCP_STATE_ESTAB) connCountMap[table->table[i].dwOwningPid]++;
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
        if (elapsed < 0.05) elapsed = 0.05;
        g_etwPrevSampleTime = now;

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

            if (down < 1.0 && up < 1.0 && conns == 0) continue;

            ProcessNetStats stats;
            stats.pid = pid;
            stats.connCount = conns;
            stats.speedDown = down;
            stats.speedUp = up;
            stats.name = ResolveProcessName(pid);
            list.push_back(stats);
        }
    } else {
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
        msg += L" NETWORK USAGE BY PROCESS - REAL MEASUREMENT (ETW)\r\n";
    } else {
        msg += L" ESTIMATE MODE - No real byte measurement\r\n";
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

    if (shown == 0) msg += Tr(L"NO_CONN");

    return msg;
}

LRESULT CALLBACK ProcListWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit = NULL;
    static HFONT hFont = NULL;
    switch (msg) {
    case WM_CREATE: {
        double baseScale = (g_scale == SCALE_LARGE) ? 1.4 : ((g_scale == SCALE_MEDIUM) ? 1.2 : 1.0);
        double factor = baseScale * K_SCALE_REDUCTION;

        hEdit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, (int)(928 * factor), (int)(640 * factor), hwnd, NULL, NULL, NULL);

        hFont = CreateFontW((int)(-18 * factor), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        StartEtwMonitoring();
        SetTimer(hwnd, ID_TIMER_PROC, 900, NULL);
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

    double baseScale = (g_scale == SCALE_LARGE) ? 1.4 : ((g_scale == SCALE_MEDIUM) ? 1.2 : 1.0);
    double factor = baseScale * K_SCALE_REDUCTION;

    g_hwndProcList = CreateWindowExW(
        WS_EX_TOPMOST,
        L"NetSpeedProcListClass",
        Tr(L"PROC_TITLE").c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, (int)(960 * factor), (int)(704 * factor),
        NULL, NULL, GetModuleHandle(NULL), NULL
    );
}

DWORD WINAPI SpeedTestThreadProc(LPVOID) {
    if (InterlockedCompareExchange(&g_speedTestRunning, 1, 0) != 0) return 0;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    std::atomic<ULONG64> totalDownloaded(0);
    std::atomic<bool> stopDownload(false);

    LARGE_INTEGER t0_down, t1_down;
    QueryPerformanceCounter(&t0_down);

    auto downloadTask = [&]() {
        HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return;
        WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);
        HINTERNET hConnect = WinHttpConnect(hSession, L"speed.cloudflare.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            while (!stopDownload.load()) {
                HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/__down?bytes=50000000", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
                if (hRequest) {
                    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(hRequest, NULL)) {
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
        HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return;
        WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);
        HINTERNET hConnect = WinHttpConnect(hSession, L"speed.cloudflare.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            std::vector<BYTE> dummyData(5 * 1024 * 1024, 'X');
            while (!stopUpload.load()) {
                HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/__up", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
                if (hRequest) {
                    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, dummyData.data(), (DWORD)dummyData.size(), (DWORD)dummyData.size(), 0) && WinHttpReceiveResponse(hRequest, NULL)) {
                        totalUploaded += dummyData.size();
                    }
                    WinHttpCloseHandle(hRequest);
                }
            }
            WinHttpCloseHandle(hConnect);
        }
        WinHttpCloseHandle(hSession);
    };

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

    if (g_hwndMain) PostMessageW(g_hwndMain, WM_SPEEDTEST_DONE, 0, (LPARAM)pRes);
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
        LoadSettings();
        QueryPerformanceFrequency(&g_timerFreq);
        SetTimer(hwnd, ID_TIMER, 900, NULL);

        g_reparented = false;
        UpdateScaleDimensions();
        AlignToTaskbar(hwnd);
        SetTaskbarAsOwner(hwnd);

        g_hBgBrush = CreateSolidBrush(RGB(220, 220, 220));
        g_hPenGreen = CreatePen(PS_SOLID, 1, RGB(0, 130, 0));
        g_hPenRed = CreatePen(PS_SOLID, 1, RGB(200, 0, 0));
        RenderWidget();

        DisableDwmBackdrop(hwnd);

        if (!g_reparented) {
            g_hHookForeground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
            g_hHookReorder = SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
            g_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(NULL), 0);
        }
        break;

    case WM_TIMER:
        if (wParam == ID_TIMER) UpdateNetworkData(hwnd);
        break;

    case WM_SPEEDTEST_DONE: {
        SpeedTestResultData* pData = (SpeedTestResultData*)lParam;
        DisplaySimpleResultModal(pData);
        break;
    }

    case WM_ERASEBKGND:
        BlitWidget((HDC)wParam);
        return 1;

    case WM_PRINTCLIENT:
        BlitWidget((HDC)wParam);
        return 1;

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
        HMENU hLangMenu = CreatePopupMenu();
        HMENU hScaleMenu = CreatePopupMenu();

        // Menu Unidades
        AppendMenuW(hUnitMenu, MF_STRING | (g_unitMode == UNIT_BYTES ? MF_CHECKED : MF_UNCHECKED), ID_UNIT_BYTES, L"MB/s (bytes)");
        AppendMenuW(hUnitMenu, MF_STRING | (g_unitMode == UNIT_MBIT  ? MF_CHECKED : MF_UNCHECKED), ID_UNIT_MBIT,  L"Megabit/s (Mb)");
        AppendMenuW(hUnitMenu, MF_STRING | (g_unitMode == UNIT_GBIT  ? MF_CHECKED : MF_UNCHECKED), ID_UNIT_GBIT,  L"Gigabit/s (Gb)");

        // Menu Idiomas ampliados (10 idiomas)
        AppendMenuW(hLangMenu, MF_STRING | (g_language == LANG_PT ? MF_CHECKED : MF_UNCHECKED), ID_LANG_PT, L"Português");
        AppendMenuW(hLangMenu, MF_STRING | (g_language == LANG_EN ? MF_CHECKED : MF_UNCHECKED), ID_LANG_EN, L"English");
        AppendMenuW(hLangMenu, MF_STRING | (g_language == LANG_ES ? MF_CHECKED : MF_UNCHECKED), ID_LANG_ES, L"Español");
        AppendMenuW(hLangMenu, MF_STRING | (g_language == LANG_FR ? MF_CHECKED : MF_UNCHECKED), ID_LANG_FR, L"Français");
        AppendMenuW(hLangMenu, MF_STRING | (g_language == LANG_DE ? MF_CHECKED : MF_UNCHECKED), ID_LANG_DE, L"Deutsch");
        AppendMenuW(hLangMenu, MF_STRING | (g_language == LANG_IT ? MF_CHECKED : MF_UNCHECKED), ID_LANG_IT, L"Italiano");
        AppendMenuW(hLangMenu, MF_STRING | (g_language == LANG_RU ? MF_CHECKED : MF_UNCHECKED), ID_LANG_RU, L"Русский");
        AppendMenuW(hLangMenu, MF_STRING | (g_language == LANG_JA ? MF_CHECKED : MF_UNCHECKED), ID_LANG_JA, L"日本語");
        AppendMenuW(hLangMenu, MF_STRING | (g_language == LANG_ZH ? MF_CHECKED : MF_UNCHECKED), ID_LANG_ZH, L"中文 (简体)");
        AppendMenuW(hLangMenu, MF_STRING | (g_language == LANG_KO ? MF_CHECKED : MF_UNCHECKED), ID_LANG_KO, L"한국어");

        // Menu Escalas
        AppendMenuW(hScaleMenu, MF_STRING | (g_scale == SCALE_NORMAL ? MF_CHECKED : MF_UNCHECKED), ID_SCALE_NORMAL, Tr(L"SCALE_NORM").c_str());
        AppendMenuW(hScaleMenu, MF_STRING | (g_scale == SCALE_MEDIUM ? MF_CHECKED : MF_UNCHECKED), ID_SCALE_MEDIUM, Tr(L"SCALE_MED").c_str());
        AppendMenuW(hScaleMenu, MF_STRING | (g_scale == SCALE_LARGE  ? MF_CHECKED : MF_UNCHECKED), ID_SCALE_LARGE,  Tr(L"SCALE_LRG").c_str());

        UINT flagAuto = IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED;
        AppendMenuW(hMenu, MF_STRING | flagAuto, ID_TRAY_AUTOSTART, Tr(L"START_WIN").c_str());
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hUnitMenu, Tr(L"UNIT_MENU").c_str());
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hLangMenu, Tr(L"LANG_MENU").c_str());
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hScaleMenu, Tr(L"SCALE_MENU").c_str());
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_PROCLIST, Tr(L"PROC_MENU").c_str());
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_SPEEDTEST, Tr(L"TEST_MENU").c_str());
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, Tr(L"EXIT_MENU").c_str());

        SetForegroundWindow(hwnd);

        int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN | TPM_LEFTALIGN, rcWindow.left, rcWindow.top - 2, 0, hwnd, NULL);

        DestroyMenu(hScaleMenu);
        DestroyMenu(hLangMenu);
        DestroyMenu(hUnitMenu);
        DestroyMenu(hMenu);

        switch (cmd) {
        case ID_TRAY_AUTOSTART:
            ToggleAutoStart();
            break;
        case ID_TRAY_EXIT:
            if (g_hwndProcList) DestroyWindow(g_hwndProcList);
            DestroyWindow(hwnd);
            break;
        case ID_UNIT_BYTES:
        case ID_UNIT_MBIT:
        case ID_UNIT_GBIT:
            g_unitMode = (cmd == ID_UNIT_BYTES) ? UNIT_BYTES : (cmd == ID_UNIT_MBIT ? UNIT_MBIT : UNIT_GBIT);
            SaveSettings();
            RenderWidget();
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case ID_LANG_PT: g_language = LANG_PT; SaveSettings(); break;
        case ID_LANG_EN: g_language = LANG_EN; SaveSettings(); break;
        case ID_LANG_ES: g_language = LANG_ES; SaveSettings(); break;
        case ID_LANG_FR: g_language = LANG_FR; SaveSettings(); break;
        case ID_LANG_DE: g_language = LANG_DE; SaveSettings(); break;
        case ID_LANG_IT: g_language = LANG_IT; SaveSettings(); break;
        case ID_LANG_RU: g_language = LANG_RU; SaveSettings(); break;
        case ID_LANG_JA: g_language = LANG_JA; SaveSettings(); break;
        case ID_LANG_ZH: g_language = LANG_ZH; SaveSettings(); break;
        case ID_LANG_KO: g_language = LANG_KO; SaveSettings(); break;
        case ID_SCALE_NORMAL: g_scale = SCALE_NORMAL; UpdateScaleDimensions(); SaveSettings(); RenderWidget(); InvalidateRect(hwnd, NULL, FALSE); break;
        case ID_SCALE_MEDIUM: g_scale = SCALE_MEDIUM; UpdateScaleDimensions(); SaveSettings(); RenderWidget(); InvalidateRect(hwnd, NULL, FALSE); break;
        case ID_SCALE_LARGE:  g_scale = SCALE_LARGE;  UpdateScaleDimensions(); SaveSettings(); RenderWidget(); InvalidateRect(hwnd, NULL, FALSE); break;
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
        0, 0, g_rectWidth, g_rectHeight,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) return 0;

    if (hIconBig) SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSmall) SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

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

    StopEtwMonitoring();
    return 0;
}