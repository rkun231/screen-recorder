// 녹화 프로그램 — 1단계
// 모니터 전체 녹화 + 컴퓨터 소리(출력 장치 선택) + 하드웨어 가속 압축 + 단축키/트레이/녹화 표시.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <dxgi1_6.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdarg>
#include "audio.h"

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static const wchar_t* kAppTitle = L"녹화 프로그램 v0.5.8";
static const wchar_t* kAppFolder = L"녹화프로그램";

enum : UINT {
    WM_APP_TRAY = WM_APP + 1,
    WM_APP_HOTKEY,
    WM_APP_ENCDETECT,
    WM_APP_SEGSTARTED,
    WM_APP_STARTFAIL,
    WM_APP_PAUSED,
    WM_APP_SEGLOST,
    WM_APP_RECOVERED,
    WM_APP_SAVED,
    WM_APP_SAVEFAIL,
    WM_APP_REGION,
};

enum : int {
    IDC_GRP_TARGET = 100, IDC_LBL_MON, IDC_CB_MON, IDC_CHK_CURSOR, IDC_CHK_HDROFF,
    IDC_RB_FULL, IDC_RB_REGION, IDC_BTN_DRAW, IDC_LBL_REGION, IDC_LBL_PRESET, IDC_BTN_P1080, IDC_BTN_P720, IDC_CHK_BORDER,
    IDC_RB_WINDOW, IDC_CB_WIN, IDC_BTN_PICKWIN, IDC_BTN_WINREFRESH, IDC_CHK_ADMIN,
    IDC_GRP_AUDIO, IDC_CHK_SYS, IDC_CB_DEV, IDC_CHK_MIC, IDC_LBL_SYNC, IDC_CB_SYNC,
    IDC_BTN_OUTDEV, IDC_LBL_SYSVOL, IDC_TB_SYSVOL, IDC_LBL_SYSVOLV, IDC_CB_MICDEV, IDC_LBL_MICVOL, IDC_TB_MICVOL,
    IDC_LBL_MICVOLV, IDC_CHK_SEPARATE,
    IDC_GRP_VIDEO, IDC_LBL_FPS, IDC_RB_30, IDC_RB_60, IDC_RB_120, IDC_LBL_Q, IDC_CB_Q, IDC_LBL_ENC, IDC_CB_ENC, IDC_LBL_ENCST,
    IDC_GRP_KEYS, IDC_LBL_HKREC, IDC_HK_REC, IDC_LBL_HKPAUSE, IDC_HK_PAUSE,
    IDC_GRP_SAVE, IDC_ED_DIR, IDC_BTN_DIR, IDC_BTN_OPEN, IDC_CHK_HIDE, IDC_CHK_NOTIFY,
    IDC_BTN_REC, IDC_BTN_PAUSE, IDC_LBL_TIME, IDC_LBL_STATUS, IDC_BTN_EDITOR,
    IDM_TRAY_REC = 300, IDM_TRAY_PAUSE, IDM_TRAY_SHOW, IDM_TRAY_FOLDER, IDM_TRAY_EXIT, IDM_TRAY_EDIT,
};

// ---------------------------------------------------------------- utilities

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring s(buf, n);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? s : s.substr(0, p);
}
static std::wstring FfmpegPath() { return ExeDir() + L"\\ffmpeg.exe"; }
static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool EnsureDir(const std::wstring& p) {
    if (DirExists(p)) return true;
    int r = SHCreateDirectoryExW(nullptr, p.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || DirExists(p);
}
static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    if (p) CoTaskMemFree(p);
    return s;
}
static std::wstring AppDataDir() {
    std::wstring d = KnownFolder(FOLDERID_RoamingAppData) + L"\\" + kAppFolder;
    EnsureDir(d);
    return d;
}
static std::wstring DefaultSaveDir() { return KnownFolder(FOLDERID_Videos) + L"\\내 녹화"; }
static std::wstring Widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}
static std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring StampNow() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[64];
    swprintf(b, 64, L"%04d-%02d-%02d %02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
static std::wstring UniquePath(const std::wstring& dir, const std::wstring& base, const std::wstring& ext) {
    std::wstring p = dir + L"\\" + base + ext;
    for (int i = 2; FileExists(p); i++) p = dir + L"\\" + base + L" (" + std::to_wstring(i) + L")" + ext;
    return p;
}
static std::wstring FileNameOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}
static long long FileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}
static std::wstring ReadTail(const std::wstring& path, size_t maxBytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    long long start = sz.QuadPart > (long long)maxBytes ? sz.QuadPart - (long long)maxBytes : 0;
    LARGE_INTEGER li;
    li.QuadPart = start;
    SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    std::string buf((size_t)(sz.QuadPart - start), '\0');
    DWORD rd = 0;
    if (!buf.empty()) ReadFile(h, &buf[0], (DWORD)buf.size(), &rd, nullptr);
    CloseHandle(h);
    buf.resize(rd);
    return Widen(buf.c_str());
}
static void WriteTextFile(const std::wstring& path, const std::string& utf8) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &w, nullptr);
    CloseHandle(h);
}
static std::wstring FormatTime(ULONGLONG ms) {
    ULONGLONG s = ms / 1000;
    wchar_t b[32];
    swprintf(b, 32, L"%02llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
    return b;
}

// ---------------------------------------------------------------- monitors (DXGI)

struct MonitorInfo {
    int adapter = 0, output = 0;
    int number = 0;
    std::wstring deviceName;
    RECT rc{};
    bool primary = false;
    UINT vendor = 0;
    bool hdr = false;
    int sdrWhiteNits = 200;  // Windows "SDR content brightness" on HDR monitors
};

// SDR white level (nits) Windows uses for a monitor in HDR mode.
static int SdrWhiteNits(const std::wstring& gdiName) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return 200;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return 200;
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (gdiName != src.viewGdiDeviceName) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL wl{};
        wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        wl.header.size = sizeof(wl);
        wl.header.adapterId = paths[i].targetInfo.adapterId;
        wl.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS && wl.SDRWhiteLevel > 0)
            return std::clamp((int)(wl.SDRWhiteLevel * 80 / 1000), 80, 1000);
    }
    return 200;
}

// Turns Windows HDR on/off for one monitor (same as the HDR switch in display settings).
static bool SetMonitorHdr(const std::wstring& gdiName, bool enable) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return false;
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || gdiName != src.viewGdiDeviceName) continue;
        DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE st{};
        st.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
        st.header.size = sizeof(st);
        st.header.adapterId = paths[i].targetInfo.adapterId;
        st.header.id = paths[i].targetInfo.id;
        st.enableAdvancedColor = enable ? 1 : 0;
        return DisplayConfigSetDeviceInfo(&st.header) == ERROR_SUCCESS;
    }
    return false;
}

static std::vector<MonitorInfo> EnumMonitors() {
    std::vector<MonitorInfo> v;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return v;
    IDXGIAdapter1* a = nullptr;
    for (UINT ai = 0; f->EnumAdapters1(ai, &a) != DXGI_ERROR_NOT_FOUND; ai++) {
        DXGI_ADAPTER_DESC1 ad{};
        a->GetDesc1(&ad);
        IDXGIOutput* o = nullptr;
        for (UINT oi = 0; a->EnumOutputs(oi, &o) != DXGI_ERROR_NOT_FOUND; oi++) {
            DXGI_OUTPUT_DESC od{};
            o->GetDesc(&od);
            if (od.AttachedToDesktop) {
                MonitorInfo m;
                m.adapter = (int)ai;
                m.output = (int)oi;
                m.deviceName = od.DeviceName;
                m.rc = od.DesktopCoordinates;
                m.primary = (m.rc.left == 0 && m.rc.top == 0);
                m.vendor = ad.VendorId;
                IDXGIOutput6* o6 = nullptr;
                if (SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput6), (void**)&o6))) {
                    DXGI_OUTPUT_DESC1 d1{};
                    if (SUCCEEDED(o6->GetDesc1(&d1))) m.hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    o6->Release();
                }
                if (m.hdr) m.sdrWhiteNits = SdrWhiteNits(m.deviceName);
                size_t i = m.deviceName.find_last_not_of(L"0123456789");
                m.number = (i != std::wstring::npos && i + 1 < m.deviceName.size()) ? _wtoi(m.deviceName.c_str() + i + 1) : (int)v.size() + 1;
                v.push_back(m);
            }
            o->Release();
        }
        a->Release();
    }
    f->Release();
    std::sort(v.begin(), v.end(), [](const MonitorInfo& x, const MonitorInfo& y) { return x.number < y.number; });
    return v;
}

// ---------------------------------------------------------------- windows (window mode)

struct WinInfo {
    HWND hwnd = nullptr;
    std::wstring title, exe, cls;
};

static std::wstring WindowTitle(HWND h) {
    wchar_t b[512];
    int n = GetWindowTextW(h, b, 512);
    return std::wstring(b, n > 0 ? n : 0);
}
static std::wstring WindowClass(HWND h) {
    wchar_t b[256];
    int n = GetClassNameW(h, b, 256);
    return std::wstring(b, n > 0 ? n : 0);
}
static std::wstring WindowExe(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    std::wstring r;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p) {
        wchar_t b[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(p, 0, b, &n)) {
            std::wstring full(b, n);
            size_t i = full.find_last_of(L"\\/");
            r = i == std::wstring::npos ? full : full.substr(i + 1);
        }
        CloseHandle(p);
    }
    return r;
}
// Visible bounds of a window (without the invisible resize margins).
static RECT WindowBounds(HWND h) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
    return r;
}
// Normal app windows a user would want to record.
static bool IsRecordableWindow(HWND h) {
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return false;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    if (WindowTitle(h).empty()) return false;
    std::wstring cls = WindowClass(h);
    if (cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd") return false;
    return true;
}
static std::vector<WinInfo> EnumRecordableWindows() {
    std::vector<WinInfo> v;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (IsRecordableWindow(h)) {
            WinInfo w;
            w.hwnd = h;
            w.title = WindowTitle(h);
            w.exe = WindowExe(h);
            w.cls = WindowClass(h);
            ((std::vector<WinInfo>*)lp)->push_back(w);
        }
        return TRUE;
    }, (LPARAM)&v);
    return v;
}
// Topmost recordable window under a screen point (z-order), ignoring our own windows.
static HWND RecordableWindowAt(POINT pt) {
    struct Ctx { POINT pt; HWND found; } c{pt, nullptr};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        if (IsIconic(h) || !IsRecordableWindow(h)) return TRUE;
        RECT r = WindowBounds(h);
        if (PtInRect(&r, c->pt)) { c->found = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&c);
    return c.found;
}

// ---------------------------------------------------------------- settings

struct Settings {
    std::wstring monitor;  // DXGI device name, e.g. \\.\DISPLAY1
    bool cursor = true;
    bool sysAudio = true;
    std::vector<std::wstring> outputs{L""};  // playback devices to record ("" = Windows default)
    bool micOn = false;
    std::wstring micDev;  // "" = default microphone
    int sysVol = 100, micVol = 100;  // percent, 0..200
    bool separate = false;  // also save computer sound and mic as separate tracks
    int fps = 30;
    int quality = 1;  // 0 보통, 1 높음, 2 최고
    int encoder = 0;  // 0 auto, 1 nvidia, 2 intel, 3 cpu
    UINT recVk = VK_F12, recMods = 0;
    UINT pauseVk = VK_F12, pauseMods = 1;  // mods: 1 shift, 2 ctrl, 4 alt
    std::wstring saveDir;
    bool hideOnRec = true;
    bool notify = true;
    bool hdrOff = false;  // turn HDR off on the recorded monitor while recording
    int mode = 0;         // 0 = whole monitor, 1 = region, 2 = window
    std::wstring regionMon;  // monitor the region is on
    int rx = 0, ry = 0, rw = 0, rh = 0;  // region, relative to that monitor's top-left (physical pixels)
    bool border = true;
    std::wstring winExe, winTitle, winClass;  // remembered window for window mode
    bool admin = false;                       // relaunch elevated
    bool hasPos = false;                      // main window position from last time
    int posX = 0, posY = 0;
    int audioOffsetMs = 60;  // sync correction: + makes the sound earlier, − later
};
static Settings S;

static std::wstring IniPath() { return AppDataDir() + L"\\settings.ini"; }
static std::wstring IniGet(const wchar_t* k, const std::wstring& def) {
    wchar_t b[2048];
    GetPrivateProfileStringW(L"settings", k, def.c_str(), b, 2048, IniPath().c_str());
    return b;
}
static int IniInt(const wchar_t* k, int def) { return GetPrivateProfileIntW(L"settings", k, def, IniPath().c_str()); }
static void IniSet(const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"settings", k, v.c_str(), IniPath().c_str()); }
static void IniSetInt(const wchar_t* k, int v) { IniSet(k, std::to_wstring(v)); }

static void LoadSettings() {
    std::wstring ini = IniPath();
    if (!FileExists(ini)) {  // UTF-16 BOM so Korean paths survive
        HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const unsigned char bom[2] = {0xFF, 0xFE};
            DWORD w;
            WriteFile(h, bom, 2, &w, nullptr);
            CloseHandle(h);
        }
    }
    S.monitor = IniGet(L"Monitor", L"");
    S.cursor = IniInt(L"Cursor", 1) != 0;
    S.sysAudio = IniInt(L"SysAudio", 1) != 0;
    {
        std::wstring list = IniGet(L"OutputDevices", L"\x01");
        if (list == L"\x01") list = IniGet(L"AudioDevice", L"");  // older versions: one device
        S.outputs.clear();
        size_t a = 0;
        while (true) {
            size_t b = list.find(L'|', a);
            S.outputs.push_back(list.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a));
            if (b == std::wstring::npos) break;
            a = b + 1;
        }
    }
    S.micOn = IniInt(L"MicOn", 0) != 0;
    S.micDev = IniGet(L"MicDevice", L"");
    S.sysVol = std::clamp(IniInt(L"SysVol", 100), 0, 200);
    S.micVol = std::clamp(IniInt(L"MicVol", 100), 0, 200);
    S.separate = IniInt(L"SeparateTracks", 0) != 0;
    S.fps = IniInt(L"Fps", 30);
    if (S.fps != 60 && S.fps != 120) S.fps = 30;
    S.quality = std::clamp(IniInt(L"Quality", 1), 0, 2);
    S.encoder = std::clamp(IniInt(L"Encoder", 0), 0, 3);
    S.recVk = IniInt(L"RecVk", VK_F12);
    S.recMods = IniInt(L"RecMods", 0);
    S.pauseVk = IniInt(L"PauseVk", VK_F12);
    S.pauseMods = IniInt(L"PauseMods", 1);
    S.saveDir = IniGet(L"SaveDir", DefaultSaveDir());
    if (S.saveDir.empty()) S.saveDir = DefaultSaveDir();
    S.hideOnRec = IniInt(L"HideOnRec", 1) != 0;
    S.notify = IniInt(L"Notify", 1) != 0;
    S.hdrOff = IniInt(L"HdrOff", 0) != 0;
    S.mode = std::clamp(IniInt(L"Mode", 0), 0, 2);
    S.winExe = IniGet(L"WinExe", L"");
    S.winTitle = IniGet(L"WinTitle", L"");
    S.winClass = IniGet(L"WinClass", L"");
    S.admin = IniInt(L"Admin", 0) != 0;
    {
        std::wstring x = IniGet(L"WindowX", L""), y = IniGet(L"WindowY", L"");
        S.hasPos = !x.empty() && !y.empty();
        S.posX = _wtoi(x.c_str());  // may be negative (monitor left of / above the main one)
        S.posY = _wtoi(y.c_str());
    }
    S.regionMon = IniGet(L"RegionMon", L"");
    S.rx = IniInt(L"RegionX", 0);
    S.ry = IniInt(L"RegionY", 0);
    S.rw = IniInt(L"RegionW", 0);
    S.rh = IniInt(L"RegionH", 0);
    S.border = IniInt(L"Border", 1) != 0;
    S.audioOffsetMs = std::clamp(IniInt(L"SyncMs", 60), -300, 300);
}
static void SaveSettings() {
    IniSet(L"Monitor", S.monitor);
    IniSetInt(L"Cursor", S.cursor);
    IniSetInt(L"SysAudio", S.sysAudio);
    {
        std::wstring list;
        for (size_t i = 0; i < S.outputs.size(); i++) list += (i ? L"|" : L"") + S.outputs[i];
        IniSet(L"OutputDevices", list);
    }
    IniSetInt(L"MicOn", S.micOn);
    IniSet(L"MicDevice", S.micDev);
    IniSetInt(L"SysVol", S.sysVol);
    IniSetInt(L"MicVol", S.micVol);
    IniSetInt(L"SeparateTracks", S.separate);
    IniSetInt(L"Fps", S.fps);
    IniSetInt(L"Quality", S.quality);
    IniSetInt(L"Encoder", S.encoder);
    IniSetInt(L"RecVk", S.recVk);
    IniSetInt(L"RecMods", S.recMods);
    IniSetInt(L"PauseVk", S.pauseVk);
    IniSetInt(L"PauseMods", S.pauseMods);
    IniSet(L"SaveDir", S.saveDir);
    IniSetInt(L"HideOnRec", S.hideOnRec);
    IniSetInt(L"Notify", S.notify);
    IniSetInt(L"HdrOff", S.hdrOff);
    IniSetInt(L"Mode", S.mode);
    IniSet(L"WinExe", S.winExe);
    IniSet(L"WinTitle", S.winTitle);
    IniSet(L"WinClass", S.winClass);
    IniSetInt(L"Admin", S.admin);
    if (S.hasPos) {
        IniSetInt(L"WindowX", S.posX);
        IniSetInt(L"WindowY", S.posY);
    }
    IniSet(L"RegionMon", S.regionMon);
    IniSetInt(L"RegionX", S.rx);
    IniSetInt(L"RegionY", S.ry);
    IniSetInt(L"RegionW", S.rw);
    IniSetInt(L"RegionH", S.rh);
    IniSetInt(L"Border", S.border);
    IniSetInt(L"SyncMs", S.audioOffsetMs);
}

// ---------------------------------------------------------------- ffmpeg processes

static HANDLE g_job = nullptr;

struct Proc {
    HANDLE h = nullptr;
    HANDLE in = nullptr;
};

// FFmpeg runs without a window, so Windows treats it as a background task: it gets the
// coarse 15.6 ms timer (which caps capture at ~64 fps) and may be slowed for power saving.
// Give it normal foreground treatment: precise 1 ms timer, no throttling, higher priority.
static void BoostChild(HANDLE h) {
    SetPriorityClass(h, ABOVE_NORMAL_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    st.StateMask = 0;  // not throttled, timer requests honored
    SetProcessInformation(h, ProcessPowerThrottling, &st, sizeof(st));
    // FFmpeg already loads winmm.dll; ask it to call timeBeginPeriod(1) for itself.
    static FARPROC tbp = [] {
        HMODULE wm = LoadLibraryW(L"winmm.dll");
        return wm ? GetProcAddress(wm, "timeBeginPeriod") : nullptr;
    }();
    if (!tbp) return;
    HANDLE t = CreateRemoteThread(h, nullptr, 0, (LPTHREAD_START_ROUTINE)(void*)tbp, (LPVOID)(UINT_PTR)1, 0, nullptr);
    if (t) {
        WaitForSingleObject(t, 3000);
        CloseHandle(t);
    }
}

static bool Launch(const std::wstring& cmd, const std::wstring& logPath, Proc& p) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE hLog = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, 0, nullptr);
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
        return false;
    }
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd;
    si.hStdOutput = hLog != INVALID_HANDLE_VALUE ? hLog : nullptr;
    si.hStdError = si.hStdOutput;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    std::wstring dir = ExeDir();
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, dir.c_str(), &si, &pi);
    CloseHandle(rd);
    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
    if (!ok) {
        CloseHandle(wr);
        return false;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    BoostChild(pi.hProcess);
    p.h = pi.hProcess;
    p.in = wr;
    return true;
}
static void CloseProc(Proc& p) {
    if (p.in) { CloseHandle(p.in); p.in = nullptr; }
    if (p.h) { CloseHandle(p.h); p.h = nullptr; }
}
// Saves a small crash-style snapshot (thread stacks) of a stuck process, using dbghelp.dll.
static bool WriteMiniDump(HANDLE proc, const std::wstring& path) {
    typedef BOOL(WINAPI * Fn)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    static Fn fn = [] {
        HMODULE m = LoadLibraryW(L"dbghelp.dll");
        return m ? (Fn)(void*)GetProcAddress(m, "MiniDumpWriteDump") : (Fn) nullptr;
    }();
    if (!fn) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    // MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory
    const int type = 0x00000000 | 0x00001000 | 0x00000020 | 0x00000040;
    BOOL ok = fn(proc, GetProcessId(proc), f, type, nullptr, nullptr, nullptr);
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    return ok != FALSE;
}

// Appends one line to stop_problems.log (troubleshooting record).
static void LogProblem(const std::string& text) {
    std::wstring log = AppDataDir() + L"\\stop_problems.log";
    HANDLE h = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[64];
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::string line = b + text + "\r\n";
    DWORD w;
    WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
    CloseHandle(h);
}
// Asks FFmpeg to finish ('q'), so the MP4 is finalized properly.
static void QuitProc(Proc& p, DWORD timeoutMs) {
    if (!p.h) return;
    if (p.in) {
        DWORD w;
        WriteFile(p.in, "q", 1, &w, nullptr);
    }
    if (WaitForSingleObject(p.h, timeoutMs) == WAIT_TIMEOUT) {
        TerminateProcess(p.h, 1);
        WaitForSingleObject(p.h, 5000);
        // Remember it for troubleshooting (the recording is still safe thanks to fragmented MP4).
        LogProblem("ffmpeg did not stop within " + std::to_string(timeoutMs) + " ms after 'q'; terminated");
    }
    CloseProc(p);
}
static int RunAndWait(const std::wstring& cmd, const std::wstring& log, DWORD timeoutMs) {
    Proc p;
    if (!Launch(cmd, log, p)) return -1;
    DWORD code = 1;
    if (WaitForSingleObject(p.h, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(p.h, &code);
    else { TerminateProcess(p.h, 1); code = (DWORD)-2; }
    CloseProc(p);
    return (int)code;
}

// ---------------------------------------------------------------- encoders

enum Enc { ENC_NV_DIRECT, ENC_NV_DL, ENC_QSV, ENC_X264 };
static std::atomic<int> g_nvOk{-1}, g_qsvOk{-1};  // -1 unknown, 0 no, 1 yes

static const wchar_t* EncLabel(Enc e) {
    switch (e) {
        case ENC_NV_DIRECT:
        case ENC_NV_DL: return L"NVIDIA (RTX 가속)";
        case ENC_QSV: return L"Intel (내장 그래픽)";
        default: return L"CPU (소프트웨어)";
    }
}

static std::vector<Enc> BuildChain(int mode, UINT vendor, bool hdr) {
    std::vector<Enc> c;
    auto addNv = [&] {
        if (vendor == 0x10DE && !hdr) c.push_back(ENC_NV_DIRECT);
        c.push_back(ENC_NV_DL);
    };
    if (mode == 0) {
        if (g_nvOk != 0) addNv();
        if (g_qsvOk != 0) c.push_back(ENC_QSV);
        c.push_back(ENC_X264);
    } else if (mode == 1) addNv();
    else if (mode == 2) c.push_back(ENC_QSV);
    else c.push_back(ENC_X264);
    return c;
}

struct RecConfig {
    MonitorInfo mon;
    bool cursor = true;
    int fps = 30;
    int quality = 1;
    bool audio = true;                  // any sound at all
    bool sysAudio = true;
    std::vector<std::wstring> outputs;
    bool mic = false;
    std::wstring micId;
    bool separate = false;
    int encoderMode = 0;
    bool region = false;
    int rx = 0, ry = 0, rw = 0, rh = 0;  // relative to mon.rc
    bool window = false;
    HWND hwnd = nullptr;
};

// Area being recorded, in desktop (physical pixel) coordinates.
static RECT CaptureRect(const RecConfig& c) {
    if (c.window) return IsWindow(c.hwnd) ? WindowBounds(c.hwnd) : c.mon.rc;
    if (!c.region) return c.mon.rc;
    RECT r{c.mon.rc.left + c.rx, c.mon.rc.top + c.ry, c.mon.rc.left + c.rx + c.rw, c.mon.rc.top + c.ry + c.rh};
    return r;
}

static std::wstring BuildRecordCmd(const RecConfig& c, Enc enc, const std::vector<std::wstring>& pipes,
                                   long long epochUs, const std::wstring& out) {
    bool ap = !pipes.empty();
    wchar_t dbg[512];
    bool debugSrc = GetEnvironmentVariableW(L"KREC_DEBUG_VIDEO", dbg, 512) > 0;
    std::wstring s = L"\"" + FfmpegPath() + L"\" -hide_banner -stats -stats_period 2 -loglevel info -y";
    if (!debugSrc) s += L" -init_hw_device d3d11va=dd:" + std::to_wstring(c.mon.adapter) + L" -filter_hw_device dd";
    for (auto& pn : pipes)
        s += L" -probesize 32 -analyzeduration 0 -f f32le -ar 48000 -ac 2 -i \"" + pn + L"\"";
    std::wstring g = L"ddagrab=output_idx=" + std::to_wstring(c.mon.output) + L":framerate=" + std::to_wstring(c.fps) +
                     L":draw_mouse=" + (c.cursor ? L"1" : L"0") + L":allow_fallback=1";
    if (c.region) {
        g += L":offset_x=" + std::to_wstring(c.rx) + L":offset_y=" + std::to_wstring(c.ry) +
             L":video_size=" + std::to_wstring(c.rw) + L"x" + std::to_wstring(c.rh);
    }
    if (c.window) {
        // Windows Graphics Capture of one window: only that window, even if others cover it.
        // Sizes rounded down to even numbers; if the window is resized the picture is scaled to fit.
        g = L"gfxcapture=hwnd=" + std::to_wstring((unsigned long long)(UINT_PTR)c.hwnd) +
            L":max_framerate=" + std::to_wstring(c.fps) + L":capture_cursor=" + (c.cursor ? L"1" : L"0") +
            L":width=-2:height=-2:resize_mode=scale_aspect";
    }
    if (ap) {
        // Put the video on the same wall-clock timeline as the audio pipe (sample 0 == epoch).
        // The first frame is stamped with the wall-clock time it actually arrived;
        // later frames keep their smooth spacing from the capture clock.
        g += L",setpts='PTS+if(eq(N,0),st(0,(RTCTIME-" + std::to_wstring(epochUs) + L")/1000000/TB-PTS),ld(0))'";
    }
    if (debugSrc) {
        // Developer test hook: replace the screen source (used for testing without a real desktop).
        g = dbg;
        enc = ENC_X264;
    } else switch (enc) {
        case ENC_NV_DIRECT: break;
        case ENC_NV_DL: g += L",hwdownload,format=bgra"; break;
        case ENC_QSV: g += L",hwdownload,format=bgra,format=nv12"; break;
        case ENC_X264: g += L",hwdownload,format=bgra,format=yuv420p"; break;
    }
    g += L"[v]";
    s += L" -filter_complex \"" + g + L"\" -map \"[v]\"";
    for (size_t i = 0; i < pipes.size(); i++) s += L" -map " + std::to_wstring(i) + L":a";
    if (pipes.size() == 3) {
        s += L" -metadata:s:a:0 title=\"전체\" -metadata:s:a:1 title=\"컴퓨터 소리\" -metadata:s:a:2 title=\"마이크\"";
        s += L" -metadata:s:a:0 handler_name=\"전체\" -metadata:s:a:1 handler_name=\"컴퓨터 소리\" -metadata:s:a:2 handler_name=\"마이크\"";
        s += L" -disposition:a:0 default -disposition:a:1 0 -disposition:a:2 0";
    }
    static const int nvq[3] = {27, 22, 18}, qsvq[3] = {27, 22, 18}, x264q[3] = {26, 21, 18};
    int q = std::clamp(c.quality, 0, 2);
    switch (enc) {
        case ENC_NV_DIRECT:
        case ENC_NV_DL:
            s += L" -c:v h264_nvenc -preset p5 -tune hq -rc vbr -cq " + std::to_wstring(nvq[q]) + L" -b:v 0 -profile:v high";
            break;
        case ENC_QSV:
            s += L" -c:v h264_qsv -preset medium -global_quality " + std::to_wstring(qsvq[q]) + L" -profile:v high";
            break;
        case ENC_X264:
            s += L" -c:v libx264 -preset veryfast -crf " + std::to_wstring(x264q[q]);
            break;
    }
    s += L" -g " + std::to_wstring(c.fps * 2);
    // "twoloop": FFmpeg's long-proven AAC method. The newer default ("nmr") was found looping forever
    // in the middle of long recordings, which froze the whole recording.
    if (ap) s += L" -c:a aac -aac_coder twoloop -b:a 192k";
    // Fragmented MP4 while recording: the file stays playable even if FFmpeg is cut off before it can
    // write the index at the end (that index is what made long recordings unplayable).
    s += L" -movflags +frag_keyframe+empty_moov+default_base_moof";
    s += L" \"" + out + L"\"";
    return s;
}

// ---------------------------------------------------------------- session / state
//
// The UI reacts instantly: pressing a key only changes what the user *wants* (want).
// A single background worker at a time then brings FFmpeg in line with that (Pump()).

enum Want { W_REC, W_PAUSED, W_STOPPED };

struct Session {
    RecConfig cfg;
    std::wstring tempDir, finalPath;
    std::vector<std::wstring> segs;
    std::vector<Enc> chain;
    size_t encPos = 0;
    Proc proc;
    std::unique_ptr<AudioEngine> audio;
    Want want = W_REC;
    bool segActive = false;  // an FFmpeg segment is running
    bool busy = false;       // a worker thread is running
    bool encKnown = false;
    Enc enc = ENC_X264;
    bool wantHdrOff = false;            // user option + monitor is HDR
    std::atomic<bool> hdrIsOff{false};  // we switched HDR off and must restore it
    ULONGLONG accMs = 0, segStart = 0;
    // Watchdog: if FFmpeg dies or stops writing in the middle of a recording, start a new segment.
    long long wdSize = -1, wdFrames = -1;
    ULONGLONG wdTick = 0;
    int wdLost = 0;
    bool waitWindow = false;  // window mode: the target window is minimized/gone, wait for it to come back
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Session() { if (wake) CloseHandle(wake); }
};
static Session* g_sess = nullptr;
static std::atomic<AudioEngine*> g_liveAudio{nullptr};  // for live volume / mic on-off changes

static void ApplyLiveAudio(AudioEngine* a) {
    if (!a) return;
    a->sysVol = S.sysVol / 100.0f;
    a->micVol = S.micVol / 100.0f;
    a->micOn = S.micOn;
}

static std::wstring* NewStr(const std::wstring& s) { return new std::wstring(s); }

static void RestoreHdr(Session* s) {
    if (s->hdrIsOff.exchange(false)) SetMonitorHdr(s->cfg.mon.deviceName, true);
}

static void StartSegmentWorker(Session* s, HWND hwnd) {
    if (s->wantHdrOff && !s->hdrIsOff && s->segs.empty()) {
        if (SetMonitorHdr(s->cfg.mon.deviceName, false)) {
            s->hdrIsOff = true;
            Sleep(2500);  // let the monitor finish switching before capture starts
        }
    }
    std::wstring seg = s->tempDir + L"\\seg" + std::to_wstring(s->segs.size()) + L".mp4";
    std::wstring log = AppDataDir() + L"\\ffmpeg_last.log";
    std::wstring lastErr = L"알 수 없는 오류";
    for (; s->encPos < s->chain.size(); s->encPos++) {
        Enc e = s->chain[s->encPos];
        long long epoch = WallClockUs();
        std::unique_ptr<AudioEngine> ap;
        std::vector<std::wstring> pipes;
        if (s->cfg.audio) {
            AudioEngine::Config ac;
            if (s->cfg.sysAudio) ac.outputs = s->cfg.outputs;
            ac.mic = s->cfg.mic;
            ac.micId = s->cfg.micId;
            ac.separateTracks = s->cfg.separate;
            ac.epochUs = epoch;
            ac.offsetMs = S.audioOffsetMs;
            ap.reset(new AudioEngine());
            ApplyLiveAudio(ap.get());
            std::wstring err;
            if (!ap->Start(ac, pipes, err)) {
                if (s->segs.empty()) RestoreHdr(s);
                PostMessageW(hwnd, WM_APP_STARTFAIL, 1, (LPARAM)NewStr(L"소리를 녹음할 수 없어요.\n" + err));
                return;
            }
        }
        std::wstring cmd = BuildRecordCmd(s->cfg, e, pipes, epoch, seg);
        WriteTextFile(AppDataDir() + L"\\last_command.txt", Narrow(cmd));
        Proc p;
        if (!Launch(cmd, log, p)) {
            if (ap) ap->Stop();
            lastErr = L"ffmpeg.exe를 실행할 수 없어요.";
            break;
        }
        // Give FFmpeg a moment to fail (e.g. encoder not usable). If the user presses
        // pause/stop meanwhile, stop waiting and treat a still-running FFmpeg as started.
        HANDLE hs[2] = {p.h, s->wake};
        DWORD r = WaitForMultipleObjects(2, hs, FALSE, 2500);
        if (r != WAIT_OBJECT_0 && WaitForSingleObject(p.h, 0) == WAIT_TIMEOUT) {
            s->proc = p;
            s->audio = std::move(ap);
            g_liveAudio = s->audio.get();
            s->segs.push_back(seg);
            PostMessageW(hwnd, WM_APP_SEGSTARTED, (WPARAM)e, 0);
            return;
        }
        // This encoder failed — clean up and try the next one.
        CloseProc(p);
        if (ap) ap->Stop();
        DeleteFileW(seg.c_str());
        lastErr = ReadTail(log, 1200);
    }
    if (s->segs.empty()) RestoreHdr(s);
    PostMessageW(hwnd, WM_APP_STARTFAIL, 0, (LPARAM)NewStr(lastErr));
}

static void StopSegment(Session* s, DWORD quitTimeoutMs = 15000) {
    QuitProc(s->proc, quitTimeoutMs);
    g_liveAudio = nullptr;
    if (s->audio) {
        std::string rep = s->audio->ProblemReport();
        if (!rep.empty()) LogProblem("sound: " + rep);
        s->audio->Stop();
        s->audio.reset();
    }
}

// Turns the recorded pieces (seg0.mp4, seg1.mp4, ...) into one normal MP4 at `out`.
// Pieces are fragmented MP4, so even a piece whose FFmpeg was cut off is usable.
static bool JoinSegments(const std::vector<std::wstring>& segs, const std::wstring& workDir, const std::wstring& out,
                         std::wstring& err) {
    std::vector<std::wstring> good;
    for (auto& p : segs)
        if (FileSize(p) > 1024) good.push_back(p);
    if (good.size() > 1) {  // pieces with (almost) nothing in them would only upset the joining step
        std::vector<std::wstring> big;
        for (auto& p : good)
            if (FileSize(p) > 256 * 1024) big.push_back(p);
        if (!big.empty()) good.swap(big);
    }
    if (good.empty()) {
        err = L"녹화된 내용이 없어요.";
        return false;
    }
    if (good.size() == 1) {
        // Turn the fragmented recording into a normal MP4 (stream copy, no quality change) for the best
        // compatibility. If that fails for any reason, keep the fragmented file — it plays too.
        std::wstring cmd = L"\"" + FfmpegPath() + L"\" -hide_banner -loglevel warning -y -fflags +discardcorrupt -i \"" + good[0] +
                           L"\" -map 0 -c copy \"" + out + L"\"";
        std::wstring log = AppDataDir() + L"\\ffmpeg_finalize.log";
        if (RunAndWait(cmd, log, INFINITE) == 0 && FileSize(out) > 1024) return true;
        DeleteFileW(out.c_str());
        if (MoveFileExW(good[0].c_str(), out.c_str(), MOVEFILE_COPY_ALLOWED)) return true;
        err = L"파일을 옮기지 못했어요.";
        return false;
    }
    std::string list;
    for (auto& p : good) list += "file '" + Narrow(FileNameOf(p)) + "'\n";
    std::wstring listPath = workDir + L"\\list.txt";
    WriteTextFile(listPath, list);
    std::wstring cmd = L"\"" + FfmpegPath() + L"\" -hide_banner -loglevel warning -y -fflags +discardcorrupt -f concat -safe 0 -i \"" +
                       listPath + L"\" -c copy \"" + out + L"\"";
    std::wstring log = AppDataDir() + L"\\ffmpeg_concat.log";
    int code = RunAndWait(cmd, log, INFINITE);
    if (code == 0 && FileSize(out) > 0) return true;
    err = L"녹화 조각들을 합치지 못했어요.\n" + ReadTail(log, 800);
    return false;
}

static void FinalizeWorker(Session* s, HWND hwnd) {
    StopSegment(s);
    RestoreHdr(s);
    std::wstring err;
    bool ok = JoinSegments(s->segs, s->tempDir, s->finalPath, err);
    if (ok) {
        for (auto& p : s->segs) DeleteFileW(p.c_str());
        DeleteFileW((s->tempDir + L"\\list.txt").c_str());
        RemoveDirectoryW(s->tempDir.c_str());
        PostMessageW(hwnd, WM_APP_SAVED, 0, (LPARAM)NewStr(s->finalPath));
    } else {
        PostMessageW(hwnd, WM_APP_SAVEFAIL, 0, (LPARAM)NewStr(err + L"\n\n임시 파일 위치: " + s->tempDir));
    }
}

// Like Bandicam's .bfix recovery: a recording that never got saved (PC turned off, program closed by
// force, crash...) leaves its hidden ".녹화중_<time>" folder behind. At startup, turn each one into
// "<time>_복구.mp4" next to it. Works because the pieces are fragmented MP4 (always playable).
static void RecoverLeftovers(HWND hwnd, const std::wstring& dir) {
    // List the leftovers right now, before any new recording can create its own folder.
    const std::wstring prefix = L".녹화중_";
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + prefix + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (names.empty()) return;
    std::thread([hwnd, dir, prefix, names] {
        Sleep(4000);  // let a possible restart-as-administrator happen first
        int done = 0;
        std::wstring last;
        for (auto& n : names) {
            std::wstring td = dir + L"\\" + n;
            std::vector<std::wstring> segs;
            for (int i = 0, miss = 0; i < 2000 && miss < 50; i++) {
                std::wstring p = td + L"\\seg" + std::to_wstring(i) + L".mp4";
                if (FileExists(p)) { segs.push_back(p); miss = 0; }
                else miss++;
            }
            std::wstring tmpOut = td + L"\\recovered.mp4";
            std::wstring err;
            if (segs.empty() || !JoinSegments(segs, td, tmpOut, err)) {
                if (!segs.empty()) LogProblem("could not recover " + Narrow(n) + ": " + Narrow(err));
                else {  // nothing recorded at all: just remove the empty folder
                    DeleteFileW((td + L"\\list.txt").c_str());
                    RemoveDirectoryW(td.c_str());
                }
                continue;
            }
            std::wstring out = UniquePath(dir, n.substr(prefix.size()) + L"_복구", L".mp4");
            if (!MoveFileExW(tmpOut.c_str(), out.c_str(), MOVEFILE_COPY_ALLOWED)) continue;
            for (auto& p : segs) DeleteFileW(p.c_str());
            DeleteFileW((td + L"\\list.txt").c_str());
            RemoveDirectoryW(td.c_str());
            LogProblem("recovered unsaved recording " + Narrow(n) + " -> " + Narrow(FileNameOf(out)));
            done++;
            last = out;
        }
        if (done) PostMessageW(hwnd, WM_APP_RECOVERED, (WPARAM)done, (LPARAM)NewStr(last));
    }).detach();
}

// ---------------------------------------------------------------- UI globals

static HINSTANCE g_inst;
static HWND g_hwnd, g_overlay;
static HFONT g_font, g_fontBig, g_fontBtn;
static UINT g_dpi = 96;
static HICON g_icoIdle, g_icoRec, g_icoPause;
static std::vector<MonitorInfo> g_mons;
static std::vector<AudioDevice> g_devs;
static HHOOK g_hook;
static bool g_regRec = false, g_regPause = false;  // registered as global hotkeys
static bool g_keyDown[256];
static bool g_exitAfterSave = false;
static std::wstring g_lastSaved;
static bool g_ffmpegMissing = false;
static bool g_relaunchAdmin = false;

static bool IsElevated() {
    HANDLE tok = nullptr;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n)) r = e.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    return r;
}
// ---- "Run as administrator" without the UAC prompt every time ----
// The first elevated start registers an on-demand task in Task Scheduler ("run with highest
// privileges"). Later starts just ask Task Scheduler to run it, which needs no prompt.
// The task name includes a hash of the exe path, so a moved/copied program never starts a stale copy.
static std::wstring AdminTaskName() {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    unsigned h = 2166136261u;
    for (wchar_t* p = exe; *p; p++) { h ^= (unsigned)towlower(*p); h *= 16777619u; }
    wchar_t b[64];
    swprintf(b, 64, L"KRecRecorder_%08X", h);
    return b;
}
static int RunHiddenWait(const std::wstring& cmd, DWORD timeoutMs) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else { TerminateProcess(pi.hProcess, 1); code = (DWORD)-2; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
static std::wstring XmlEscape(const std::wstring& v) {
    std::wstring r;
    for (wchar_t c : v) {
        if (c == L'&') r += L"&amp;";
        else if (c == L'<') r += L"&lt;";
        else if (c == L'>') r += L"&gt;";
        else r += c;
    }
    return r;
}
static bool StartAdminTask() {
    return RunHiddenWait(L"schtasks.exe /Run /TN \"" + AdminTaskName() + L"\"", 15000) == 0;
}
static void EnsureAdminTask() {  // must be called while elevated
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo><Description>녹화 프로그램을 관리자 권한으로 확인 창 없이 실행</Description></RegistrationInfo>\r\n"
        L"  <Principals><Principal id=\"Author\"><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>Parallel</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Enabled>true</Enabled>\r\n"
        L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
        L"    <Priority>4</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\"><Exec><Command>" + XmlEscape(exe) + L"</Command><WorkingDirectory>" +
        XmlEscape(ExeDir()) + L"</WorkingDirectory></Exec></Actions>\r\n"
        L"</Task>\r\n";
    std::wstring path = AppDataDir() + L"\\admin_task.xml";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    const unsigned char bom[2] = {0xFF, 0xFE};
    WriteFile(h, bom, 2, &w, nullptr);
    WriteFile(h, xml.data(), (DWORD)(xml.size() * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(h);
    RunHiddenWait(L"schtasks.exe /Create /TN \"" + AdminTaskName() + L"\" /XML \"" + path + L"\" /F", 15000);
    DeleteFileW(path.c_str());
}
static void DeleteAdminTask() {
    RunHiddenWait(L"schtasks.exe /Delete /TN \"" + AdminTaskName() + L"\" /F", 15000);
}

static bool RelaunchElevated() {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    return (INT_PTR)ShellExecuteW(nullptr, L"runas", exe, nullptr, ExeDir().c_str(), SW_SHOWNORMAL) > 32;
}
static NOTIFYICONDATAW g_nid{};

struct CtlDef {
    int id;
    const wchar_t* cls;
    const wchar_t* text;
    DWORD style;
    int x, y, w, h;
    int font;  // 0 normal, 1 big, 2 button
};

static const CtlDef kCtls[] = {
    {IDC_GRP_TARGET, L"BUTTON", L"녹화 대상", BS_GROUPBOX, 10, 6, 480, 160, 0},
    {IDC_LBL_MON, L"STATIC", L"모니터", SS_LEFT, 24, 33, 64, 20, 0},
    {IDC_CB_MON, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 92, 29, 382, 300, 0},
    {IDC_RB_FULL, L"BUTTON", L"모니터 전체", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 24, 59, 100, 22, 0},
    {IDC_RB_REGION, L"BUTTON", L"영역 지정", BS_AUTORADIOBUTTON, 128, 59, 86, 22, 0},
    {IDC_RB_WINDOW, L"BUTTON", L"창 지정", BS_AUTORADIOBUTTON, 218, 59, 80, 22, 0},
    {IDC_LBL_PRESET, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 302, 59, 176, 22, 0},
    // region mode row
    {IDC_BTN_DRAW, L"BUTTON", L"영역 그리기", BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP, 24, 87, 96, 27, 0},
    {IDC_BTN_P1080, L"BUTTON", L"1920×1080", BS_PUSHBUTTON | WS_TABSTOP, 126, 87, 90, 27, 0},
    {IDC_BTN_P720, L"BUTTON", L"1280×720", BS_PUSHBUTTON | WS_TABSTOP, 222, 87, 90, 27, 0},
    {IDC_LBL_REGION, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, 320, 87, 160, 27, 0},
    // window mode row
    {IDC_CB_WIN, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP, 24, 89, 290, 400, 0},
    {IDC_BTN_PICKWIN, L"BUTTON", L"창 고르기", BS_PUSHBUTTON | WS_TABSTOP, 320, 87, 84, 27, 0},
    {IDC_BTN_WINREFRESH, L"BUTTON", L"새로고침", BS_PUSHBUTTON | WS_TABSTOP, 408, 87, 70, 27, 0},
    {IDC_CHK_CURSOR, L"BUTTON", L"마우스 커서 표시", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 118, 140, 22, 0},
    {IDC_CHK_BORDER, L"BUTTON", L"영역 테두리 표시 (대기: 파랑, 녹화: 빨강)", BS_AUTOCHECKBOX | WS_TABSTOP, 172, 118, 306, 22, 0},
    {IDC_CHK_HDROFF, L"BUTTON", L"HDR 모니터는 녹화 중 HDR 끄기 (색 정확, 시작·정지 때 깜빡임)", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 141, 450, 22, 0},

    {IDC_GRP_AUDIO, L"BUTTON", L"소리", BS_GROUPBOX, 10, 172, 480, 172, 0},
    {IDC_CHK_SYS, L"BUTTON", L"컴퓨터 소리", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 197, 110, 22, 0},
    {IDC_BTN_OUTDEV, L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 138, 195, 336, 26, 0},
    {IDC_LBL_SYSVOL, L"STATIC", L"크기", SS_LEFT, 44, 228, 60, 20, 0},
    {IDC_TB_SYSVOL, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 134, 224, 284, 28, 0},
    {IDC_LBL_SYSVOLV, L"STATIC", L"100%", SS_RIGHT, 420, 228, 54, 20, 0},
    {IDC_CHK_MIC, L"BUTTON", L"마이크", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 258, 110, 22, 0},
    {IDC_CB_MICDEV, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 138, 256, 336, 300, 0},
    {IDC_LBL_MICVOL, L"STATIC", L"크기", SS_LEFT, 44, 289, 60, 20, 0},
    {IDC_TB_MICVOL, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 134, 285, 284, 28, 0},
    {IDC_LBL_MICVOLV, L"STATIC", L"100%", SS_RIGHT, 420, 289, 54, 20, 0},
    {IDC_CHK_SEPARATE, L"BUTTON", L"편집용 트랙도 따로 저장", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 316, 200, 22, 0},
    {IDC_LBL_SYNC, L"STATIC", L"소리 싱크", SS_LEFT, 240, 319, 62, 20, 0},
    {IDC_CB_SYNC, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 306, 314, 168, 300, 0},

    {IDC_GRP_VIDEO, L"BUTTON", L"화질", BS_GROUPBOX, 10, 350, 480, 104, 0},
    {IDC_LBL_FPS, L"STATIC", L"프레임(fps)", SS_LEFT, 24, 377, 68, 20, 0},
    {IDC_RB_30, L"BUTTON", L"30", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 92, 374, 50, 24, 0},
    {IDC_RB_60, L"BUTTON", L"60", BS_AUTORADIOBUTTON, 146, 374, 50, 24, 0},
    {IDC_RB_120, L"BUTTON", L"120", BS_AUTORADIOBUTTON, 200, 374, 56, 24, 0},
    {IDC_LBL_Q, L"STATIC", L"화질", SS_LEFT, 270, 377, 36, 20, 0},
    {IDC_CB_Q, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_GROUP, 306, 373, 168, 200, 0},
    {IDC_LBL_ENC, L"STATIC", L"압축 방식", SS_LEFT, 24, 413, 66, 20, 0},
    {IDC_CB_ENC, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 92, 409, 168, 200, 0},
    {IDC_LBL_ENCST, L"STATIC", L"그래픽카드 확인 중...", SS_LEFT, 268, 406, 212, 40, 0},

    {IDC_GRP_KEYS, L"BUTTON", L"단축키", BS_GROUPBOX, 10, 460, 480, 82, 0},
    {IDC_LBL_HKREC, L"STATIC", L"시작/정지", SS_LEFT, 24, 487, 66, 20, 0},
    {IDC_HK_REC, HOTKEY_CLASSW, L"", WS_TABSTOP | WS_BORDER, 92, 483, 140, 24, 0},
    {IDC_LBL_HKPAUSE, L"STATIC", L"일시정지", SS_LEFT, 252, 487, 60, 20, 0},
    {IDC_HK_PAUSE, HOTKEY_CLASSW, L"", WS_TABSTOP | WS_BORDER, 316, 483, 158, 24, 0},
    {IDC_CHK_ADMIN, L"BUTTON", L"관리자 권한으로 실행 (전체화면 프로그램에서 단축키용, 확인 창은 처음 한 번만)", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 512, 450, 22, 0},

    {IDC_GRP_SAVE, L"BUTTON", L"저장", BS_GROUPBOX, 10, 548, 480, 106, 0},
    {IDC_ED_DIR, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_BORDER, 24, 573, 194, 24, 0},
    {IDC_BTN_EDITOR, L"BUTTON", L"편집기", BS_PUSHBUTTON | WS_TABSTOP, 224, 572, 84, 26, 0},
    {IDC_BTN_DIR, L"BUTTON", L"변경", BS_PUSHBUTTON | WS_TABSTOP, 314, 572, 70, 26, 0},
    {IDC_BTN_OPEN, L"BUTTON", L"폴더 열기", BS_PUSHBUTTON | WS_TABSTOP, 390, 572, 84, 26, 0},
    {IDC_CHK_HIDE, L"BUTTON", L"녹화를 시작하면 이 창을 트레이로 숨기기", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 603, 440, 22, 0},

    {IDC_CHK_NOTIFY, L"BUTTON", L"저장되면 오른쪽 아래에 알림 띄우기", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 626, 440, 22, 0},

    {IDC_BTN_REC, L"BUTTON", L"● 녹화 시작", BS_PUSHBUTTON | WS_TABSTOP, 10, 664, 200, 44, 2},
    {IDC_BTN_PAUSE, L"BUTTON", L"일시정지", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, 218, 664, 110, 44, 0},
    {IDC_LBL_TIME, L"STATIC", L"00:00:00", SS_CENTER | SS_CENTERIMAGE, 336, 664, 154, 44, 1},
    {IDC_LBL_STATUS, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 12, 716, 478, 20, 0},
};
static const int kClientW = 500, kClientH = 742;

static HWND Ctl(int id) { return GetDlgItem(g_hwnd, id); }
static int Sc(int v) { return MulDiv(v, (int)g_dpi, 96); }

static void SetStatus(const std::wstring& s) { SetWindowTextW(Ctl(IDC_LBL_STATUS), s.c_str()); }

static std::wstring HotkeyText(UINT vk, UINT mods) {
    std::wstring s;
    if (mods & 2) s += L"Ctrl+";
    if (mods & 4) s += L"Alt+";
    if (mods & 1) s += L"Shift+";
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    if ((vk >= VK_PRIOR && vk <= VK_HELP) || vk == VK_DIVIDE || vk == VK_NUMLOCK) lp |= (1 << 24);
    wchar_t b[64] = L"";
    if (vk >= VK_F1 && vk <= VK_F24) swprintf(b, 64, L"F%u", vk - VK_F1 + 1);
    else GetKeyNameTextW(lp, b, 64);
    return s + b;
}

// ---------------------------------------------------------------- icons

static HICON MakeIcon(int size, COLORREF fill, bool pauseBars) {
    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = size;
    bh.bV5Height = -size;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bh, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    DWORD* px = (DWORD*)bits;
    double c = (size - 1) / 2.0, r = size / 2.0 - 0.5;
    BYTE R = GetRValue(fill), G = GetGValue(fill), B = GetBValue(fill);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            double dx = x - c, dy = y - c;
            double d = sqrt(dx * dx + dy * dy);
            double a = std::clamp(r - d + 0.5, 0.0, 1.0);
            double ring = std::clamp(r - d + 0.5, 0.0, 1.0) - std::clamp(r - 1.6 * size / 16.0 - d + 0.5, 0.0, 1.0);
            BYTE rr = R, gg = G, bb = B;
            if (ring > 0.5) { rr = (BYTE)(R * 0.7); gg = (BYTE)(G * 0.7); bb = (BYTE)(B * 0.7); }
            if (pauseBars) {
                double bw = size * 0.13, bhh = size * 0.42;
                bool inBar = (fabs(dy) < bhh / 2) && (fabs(dx - size * 0.12) < bw / 2 || fabs(dx + size * 0.12) < bw / 2);
                if (inBar) { rr = gg = bb = 255; }
            }
            BYTE A = (BYTE)(a * 255);
            px[y * size + x] = ((DWORD)A << 24) | ((DWORD)(rr * a) << 16) | ((DWORD)(gg * a) << 8) | (DWORD)(bb * a);
        }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return h;
}

// ---------------------------------------------------------------- tray

static void TrayUpdate() {
    g_nid.uFlags = NIF_ICON | NIF_TIP;
    Want w = g_sess ? g_sess->want : W_STOPPED;
    g_nid.hIcon = !g_sess ? g_icoIdle : w == W_REC ? g_icoRec : w == W_PAUSED ? g_icoPause : g_icoIdle;
    std::wstring tip = kAppTitle;
    if (g_sess && w == W_REC) tip = L"녹화 중 — " + HotkeyText(S.recVk, S.recMods) + L"로 정지";
    else if (g_sess && w == W_PAUSED) tip = L"일시정지 중";
    else if (g_sess) tip = L"저장하는 중...";
    wcsncpy(g_nid.szTip, tip.c_str(), 127);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void TrayBalloon(const std::wstring& title, const std::wstring& text) {
    g_nid.uFlags = NIF_INFO;
    g_nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcsncpy(g_nid.szInfoTitle, title.c_str(), 63);
    wcsncpy(g_nid.szInfo, text.c_str(), 255);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void TrayAdd() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icoIdle;
    wcsncpy(g_nid.szTip, kAppTitle, 127);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void ShowMain() {
    ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

// ---------------------------------------------------------------- overlay (recording indicator)

static ULONGLONG ElapsedMs() {
    if (!g_sess) return 0;
    ULONGLONG ms = g_sess->accMs;
    if (g_sess->want == W_REC) ms += GetTickCount64() - g_sess->segStart;
    return ms;
}

static std::wstring g_sizeTag;  // non-empty: overlay shows this (region size while resizing)

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT && !g_sizeTag.empty()) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(18, 18, 18));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        UINT odpi = GetDpiForWindow(h);
        HFONT f = CreateFontW(-MulDiv(10, (int)odpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
        HGDIOBJ of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, g_sizeTag.c_str(), -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        SelectObject(dc, of);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        int W = rc.right, H = rc.bottom;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
        HGDIOBJ ob = SelectObject(mem, bmp);
        HBRUSH bg = CreateSolidBrush(RGB(18, 18, 18));
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        bool paused = g_sess && g_sess->want == W_PAUSED;
        int dot = H * 2 / 5, cx = H / 2, cy = H / 2;
        if (paused) {
            HBRUSH yb = CreateSolidBrush(RGB(255, 196, 0));
            int bw = std::max(2, dot / 3);
            RECT b1{cx - bw - bw / 2, cy - dot / 2, cx - bw / 2, cy + dot / 2};
            RECT b2{cx + bw / 2, cy - dot / 2, cx + bw + bw / 2, cy + dot / 2};
            FillRect(mem, &b1, yb);
            FillRect(mem, &b2, yb);
            DeleteObject(yb);
        } else {
            HBRUSH rb = CreateSolidBrush(RGB(235, 40, 40));
            HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));
            HGDIOBJ obr = SelectObject(mem, rb);
            Ellipse(mem, cx - dot / 2, cy - dot / 2, cx + dot / 2 + 1, cy + dot / 2 + 1);
            SelectObject(mem, obr);
            SelectObject(mem, op);
            DeleteObject(rb);
        }
        UINT odpi = GetDpiForWindow(h);
        HFONT f = CreateFontW(-MulDiv(10, (int)odpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
        HGDIOBJ of = SelectObject(mem, f);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(255, 255, 255));
        std::wstring t = (paused ? L"일시정지  " : L"REC  ") + FormatTime(ElapsedMs());
        RECT tr{H, 0, W - H / 4, H};
        DrawTextW(mem, t.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        SelectObject(mem, of);
        DeleteObject(f);
        BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

static HWND g_border = nullptr;
static COLORREF g_borderColor = RGB(235, 40, 40);
static const COLORREF kKey = RGB(255, 0, 255);  // transparent color for layered windows

// Border thickness: thicker while waiting so the edges are easy to grab.
static int BorderThickness(UINT dpi) { return MulDiv(g_sess ? 3 : 6, (int)dpi, 96); }

struct BorderDrag {
    bool active = false;
    int zone = 0;  // 1 left, 2 right, 4 top, 8 bottom
    POINT start{};
    RECT startRegion{};
    RECT region{};
    RECT mon{};
};
static BorderDrag g_bd;

static void ApplyRegion(RECT r);

static int BorderZone(HWND h, POINT client) {
    RECT rc;
    GetClientRect(h, &rc);
    UINT dpi = GetDpiForWindow(h);
    int t = BorderThickness(dpi) + 1, c = MulDiv(24, (int)dpi, 96);
    int x = client.x, y = client.y, W = rc.right, H = rc.bottom;
    bool L = x < t, R = x >= W - t, T = y < t, B = y >= H - t;
    if (T || B) { if (x < c) L = true; if (x >= W - c) R = true; }
    if (L || R) { if (y < c) T = true; if (y >= H - c) B = true; }
    return (L ? 1 : 0) | (R ? 2 : 0) | (T ? 4 : 0) | (B ? 8 : 0);
}

static void ShowSizeTag(const RECT& r) {
    if (!g_overlay) return;
    wchar_t b[64];
    swprintf(b, 64, L"%ld × %ld", (r.right - r.left) & ~1L, (r.bottom - r.top) & ~1L);
    g_sizeTag = b;
    UINT dx = 96, dy = 96;
    POINT pt{r.left + 1, r.top + 1};
    GetDpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dx, &dy);
    int w = MulDiv(130, (int)dx, 96), h = MulDiv(28, (int)dx, 96), gap = MulDiv(10, (int)dx, 96);
    int y = r.top - h - gap;
    if (y < g_bd.mon.top) y = r.top + gap;
    int x = std::clamp((int)r.left, (int)g_bd.mon.left, (int)g_bd.mon.right - w);
    SetWindowPos(g_overlay, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_overlay, nullptr, FALSE);
}
static void HideSizeTag() {
    g_sizeTag.clear();
    if (g_overlay && !g_sess) ShowWindow(g_overlay, SW_HIDE);
}

static LRESULT CALLBACK BorderProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HBRUSH col = CreateSolidBrush(g_borderColor), key = CreateSolidBrush(kKey);
            FillRect(dc, &rc, col);
            RECT in = rc;
            int t = std::max(2, BorderThickness(GetDpiForWindow(h)));
            InflateRect(&in, -t, -t);
            FillRect(dc, &in, key);  // inside is see-through and click-through
            DeleteObject(col);
            DeleteObject(key);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_NCHITTEST:
            // While recording the border can't be grabbed; clicks go to the window below.
            return g_sess ? HTTRANSPARENT : HTCLIENT;
        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            int z = g_bd.active ? g_bd.zone : BorderZone(h, p);
            LPCWSTR cur = IDC_ARROW;
            if (z == 1 || z == 2) cur = IDC_SIZEWE;
            else if (z == 4 || z == 8) cur = IDC_SIZENS;
            else if (z == 5 || z == 10) cur = IDC_SIZENWSE;
            else if (z == 6 || z == 9) cur = IDC_SIZENESW;
            SetCursor(LoadCursor(nullptr, cur));
            return TRUE;
        }
        case WM_LBUTTONDOWN: {
            if (g_sess) return 0;
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            int z = BorderZone(h, p);
            if (!z) return 0;
            RECT wr;
            GetWindowRect(h, &wr);
            int t = BorderThickness(GetDpiForWindow(h));
            g_bd.active = true;
            g_bd.zone = z;
            GetCursorPos(&g_bd.start);
            g_bd.startRegion = RECT{wr.left + t, wr.top + t, wr.right - t, wr.bottom - t};
            g_bd.region = g_bd.startRegion;
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            POINT c{(wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2};
            GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi);
            g_bd.mon = mi.rcMonitor;
            SetCapture(h);
            ShowSizeTag(g_bd.region);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!g_bd.active) return 0;
            POINT p;
            GetCursorPos(&p);
            int dx = p.x - g_bd.start.x, dy = p.y - g_bd.start.y;
            const int minSz = 32;
            RECT r = g_bd.startRegion;
            const RECT& mn = g_bd.mon;
            if (g_bd.zone & 1) r.left = std::clamp((int)r.left + dx, (int)mn.left, (int)r.right - minSz);
            if (g_bd.zone & 2) r.right = std::clamp((int)r.right + dx, (int)r.left + minSz, (int)mn.right);
            if (g_bd.zone & 4) r.top = std::clamp((int)r.top + dy, (int)mn.top, (int)r.bottom - minSz);
            if (g_bd.zone & 8) r.bottom = std::clamp((int)r.bottom + dy, (int)r.top + minSz, (int)mn.bottom);
            g_bd.region = r;
            int t = BorderThickness(GetDpiForWindow(h));
            SetWindowPos(h, HWND_TOPMOST, r.left - t, r.top - t, (r.right - r.left) + 2 * t, (r.bottom - r.top) + 2 * t, SWP_NOACTIVATE);
            InvalidateRect(h, nullptr, TRUE);
            ShowSizeTag(r);
            return 0;
        }
        case WM_LBUTTONUP:
            if (g_bd.active) {
                g_bd.active = false;
                ReleaseCapture();
                HideSizeTag();
                ApplyRegion(g_bd.region);  // saves it and repositions the border (sizes rounded to even)
            }
            return 0;
        case WM_RBUTTONDOWN:  // cancel resizing
        case WM_CAPTURECHANGED:
            if (g_bd.active) {
                g_bd.active = false;
                if (m == WM_RBUTTONDOWN) ReleaseCapture();
                HideSizeTag();
                ApplyRegion(g_bd.startRegion);
            }
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ShowOverlay(bool show) {
    if (g_sess) g_sizeTag.clear();
    if (!show || !g_sess) {
        if (g_overlay) ShowWindow(g_overlay, SW_HIDE);
        return;
    }
    const RecConfig& c = g_sess->cfg;
    RECT cap = CaptureRect(c);
    const RECT& mon = c.mon.rc;
    POINT pt{cap.left + 1, cap.top + 1};
    UINT dx = 96, dy = 96;
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    int w = MulDiv(170, (int)dx, 96), h = MulDiv(30, (int)dx, 96), m = MulDiv(16, (int)dx, 96);
    int t = MulDiv(3, (int)dx, 96);
    if (g_overlay) {
        int x, y;
        if (!c.region && !c.window) {
            x = cap.right - w - m;
            y = cap.top + m;
        } else {
            // Just outside the region's top-right corner, so it doesn't cover what is recorded.
            x = cap.right - w + t;
            y = cap.top - h - t - MulDiv(4, (int)dx, 96);
            if (y < mon.top) y = cap.bottom + t + MulDiv(4, (int)dx, 96);  // no room above → below
            if (y + h > mon.bottom) y = cap.top + m;                      // no room at all → inside
            x = std::clamp(x, (int)mon.left, (int)mon.right - w);
        }
        SetWindowPos(g_overlay, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(g_overlay, nullptr, FALSE);
    }
}


// ---------------------------------------------------------------- region selection

static HWND g_sel = nullptr;
struct SelState {
    bool active = false;
    bool preset = false;
    int pw = 0, ph = 0;
    bool dragging = false;
    POINT start{};
    RECT mon{};
    RECT sel{};
    bool has = false;
    bool keyMode = false;   // nudged with arrow keys: stop following the mouse until it really moves
    POINT keyAnchor{};
    RECT drawn{};           // area painted for the selection last time (screen coords)
    std::vector<RECT> mons; // all monitors, for the instruction text
    HFONT font = nullptr;
    bool pick = false;      // picking a window by clicking it
    HWND pickHwnd = nullptr;
    std::wstring pickTitle;
};
static SelState g_ss;

static RECT VirtualRect() {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return RECT{x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}
static RECT MonitorRectAt(POINT pt) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}
static void SelUpdatePreset(POINT pt) {
    RECT mon = MonitorRectAt(pt);
    int w = std::min<int>(g_ss.pw, mon.right - mon.left), h = std::min<int>(g_ss.ph, mon.bottom - mon.top);
    int x = std::clamp((int)pt.x - w / 2, (int)mon.left, (int)mon.right - w);
    int y = std::clamp((int)pt.y - h / 2, (int)mon.top, (int)mon.bottom - h);
    g_ss.mon = mon;
    g_ss.sel = RECT{x, y, x + w, y + h};
    g_ss.has = true;
}

// Screen area covered by the selection rectangle, its border and its size label.
static RECT SelBounds() {
    if (!g_ss.has && !g_ss.dragging) return RECT{0, 0, 0, 0};
    UINT dpi = g_sel ? GetDpiForWindow(g_sel) : 96;
    RECT b = g_ss.sel;
    InflateRect(&b, MulDiv(4, (int)dpi, 96), MulDiv(4, (int)dpi, 96));
    RECT lab{g_ss.sel.left - 2, g_ss.sel.top - MulDiv(34, (int)dpi, 96), g_ss.sel.left + MulDiv(380, (int)dpi, 96), g_ss.sel.top};
    RECT lab2{g_ss.sel.left - 2, g_ss.sel.bottom, g_ss.sel.left + MulDiv(380, (int)dpi, 96), g_ss.sel.bottom + MulDiv(34, (int)dpi, 96)};
    UnionRect(&b, &b, &lab);
    UnionRect(&b, &b, &lab2);
    return b;
}
// Repaint only what changed instead of the whole (multi-monitor) screen.
static void SelInvalidate() {
    if (!g_sel) return;
    RECT v = VirtualRect();
    RECT nb = SelBounds();
    RECT a = g_ss.drawn, c = nb;
    OffsetRect(&a, -v.left, -v.top);
    OffsetRect(&c, -v.left, -v.top);
    if (!IsRectEmpty(&a)) InvalidateRect(g_sel, &a, FALSE);
    if (!IsRectEmpty(&c)) InvalidateRect(g_sel, &c, FALSE);
    g_ss.drawn = nb;
}

static void ApplyRegion(RECT r);
static void ApplyWindow(HWND h);
static void ShowMain();
static void UpdateUI();

static void SelUpdatePick(POINT pt) {
    HWND w = RecordableWindowAt(pt);
    g_ss.pickHwnd = w;
    g_ss.has = w != nullptr;
    if (w) {
        RECT r = WindowBounds(w), v = VirtualRect();
        IntersectRect(&r, &r, &v);
        g_ss.sel = r;
        g_ss.mon = MonitorRectAt(POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2});
        g_ss.pickTitle = WindowTitle(w);
    }
}

static void EndSelect(bool ok) {
    if (!g_ss.active) return;
    g_ss.active = false;
    ReleaseCapture();
    ShowWindow(g_sel, SW_HIDE);
    if (g_ss.font) { DeleteObject(g_ss.font); g_ss.font = nullptr; }
    ShowMain();
    if (ok && g_ss.has && g_ss.pick) ApplyWindow(g_ss.pickHwnd);
    else if (ok && g_ss.has) ApplyRegion(g_ss.sel);
    else UpdateUI();  // brings the border back
}

static LRESULT CALLBACK SelectProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    RECT v = VirtualRect();
    auto screenPt = [&](LPARAM lp) { return POINT{GET_X_LPARAM(lp) + v.left, GET_Y_LPARAM(lp) + v.top}; };
    if (!g_ss.active && (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_MOUSEMOVE)) return 0;
    switch (m) {
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, g_ss.pick ? IDC_HAND : g_ss.preset ? IDC_SIZEALL : IDC_CROSS));
            return TRUE;
        case WM_LBUTTONDOWN: {
            POINT pt = screenPt(l);
            if (g_ss.pick) {
                SelUpdatePick(pt);
                if (g_ss.has) PostMessageW(h, WM_USER + 1, 1, 0);
            } else if (g_ss.preset) {
                if (!g_ss.keyMode) SelUpdatePreset(pt);  // after arrow-key nudging, keep the nudged spot
                PostMessageW(h, WM_USER + 1, 1, 0);
            } else {
                g_ss.dragging = true;
                g_ss.start = pt;
                g_ss.mon = MonitorRectAt(pt);
                g_ss.sel = RECT{pt.x, pt.y, pt.x, pt.y};
                g_ss.has = false;
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt = screenPt(l);
            if (g_ss.pick) {
                SelUpdatePick(pt);
            } else if (g_ss.preset) {
                if (g_ss.keyMode) {
                    if (abs(pt.x - g_ss.keyAnchor.x) + abs(pt.y - g_ss.keyAnchor.y) < 6) return 0;
                    g_ss.keyMode = false;
                }
                SelUpdatePreset(pt);
            } else if (g_ss.dragging) {
                LONG x = std::clamp(pt.x, g_ss.mon.left, g_ss.mon.right), y = std::clamp(pt.y, g_ss.mon.top, g_ss.mon.bottom);
                g_ss.sel = RECT{std::min(g_ss.start.x, x), std::min(g_ss.start.y, y), std::max(g_ss.start.x, x), std::max(g_ss.start.y, y)};
                g_ss.has = (g_ss.sel.right - g_ss.sel.left) >= 16 && (g_ss.sel.bottom - g_ss.sel.top) >= 16;
            }
            SelInvalidate();
            return 0;
        }
        case WM_LBUTTONUP:
            if (g_ss.dragging) {
                g_ss.dragging = false;
                if (g_ss.has) PostMessageW(h, WM_USER + 1, 1, 0);
                else SelInvalidate();  // just a click — keep selecting
            }
            return 0;
        case WM_RBUTTONDOWN:
            PostMessageW(h, WM_USER + 1, 0, 0);
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) PostMessageW(h, WM_USER + 1, 0, 0);
            else if (w == VK_RETURN && g_ss.has && !g_ss.dragging) PostMessageW(h, WM_USER + 1, 1, 0);
            else if (g_ss.preset && g_ss.has && (w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN)) {
                // Fine positioning: 1 px per press, 10 px with Shift.
                int st = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
                int dx = w == VK_LEFT ? -st : w == VK_RIGHT ? st : 0, dy = w == VK_UP ? -st : w == VK_DOWN ? st : 0;
                int sw = g_ss.sel.right - g_ss.sel.left, sh = g_ss.sel.bottom - g_ss.sel.top;
                int x = std::clamp((int)g_ss.sel.left + dx, (int)g_ss.mon.left, (int)g_ss.mon.right - sw);
                int y = std::clamp((int)g_ss.sel.top + dy, (int)g_ss.mon.top, (int)g_ss.mon.bottom - sh);
                g_ss.sel = RECT{x, y, x + sw, y + sh};
                g_ss.keyMode = true;
                GetCursorPos(&g_ss.keyAnchor);
                SelInvalidate();
            }
            return 0;
        case WM_USER + 1:
            EndSelect(w != 0);
            return 0;
        case WM_CAPTURECHANGED:
            if (g_ss.active && (HWND)l != h) PostMessageW(h, WM_USER + 1, 0, 0);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT pr = ps.rcPaint;
            int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
            if (pw <= 0 || ph <= 0) { EndPaint(h, &ps); return 0; }
            // Paint into a buffer the size of the dirty area only.
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, pw, ph);
            HGDIOBJ ob = SelectObject(mem, bmp);
            SetViewportOrgEx(mem, -pr.left, -pr.top, nullptr);
            FillRect(mem, &pr, (HBRUSH)GetStockObject(BLACK_BRUSH));
            UINT dpi = GetDpiForWindow(h);
            if (!g_ss.font)
                g_ss.font = CreateFontW(-MulDiv(12, (int)dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
            HGDIOBJ of = SelectObject(mem, g_ss.font);
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, RGB(255, 255, 255));
            const wchar_t* tip = g_ss.pick ? L"녹화할 창을 클릭하세요   ·   Esc 또는 오른쪽 클릭: 취소"
                : g_ss.preset
                ? L"마우스로 옮기고 클릭해서 놓기   ·   방향키: 1픽셀씩 (Shift: 10픽셀)   ·   Enter: 확정   ·   Esc: 취소"
                : L"드래그해서 녹화할 영역을 그리세요   ·   Esc 또는 오른쪽 클릭: 취소";
            for (RECT mon : g_ss.mons) {
                OffsetRect(&mon, -v.left, -v.top);
                RECT tr{mon.left, mon.top + MulDiv(40, (int)dpi, 96), mon.right, mon.top + MulDiv(80, (int)dpi, 96)};
                RECT ix;
                if (IntersectRect(&ix, &tr, &pr)) DrawTextW(mem, tip, -1, &tr, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }
            if (g_ss.has || g_ss.dragging) {
                RECT sr = g_ss.sel;
                OffsetRect(&sr, -v.left, -v.top);
                int t = std::max(2, MulDiv(2, (int)dpi, 96));
                RECT b = sr;
                InflateRect(&b, t, t);
                HBRUSH red = CreateSolidBrush(RGB(235, 40, 40)), key = CreateSolidBrush(kKey);
                FillRect(mem, &b, red);
                FillRect(mem, &sr, key);
                DeleteObject(red);
                DeleteObject(key);
                RECT mon = g_ss.mon;
                wchar_t sz[160];
                if (g_ss.pick) swprintf(sz, 160, L"%.60ls", g_ss.pickTitle.c_str());
                else swprintf(sz, 160, L"%ld × %ld    위치 %ld, %ld", (g_ss.sel.right - g_ss.sel.left) & ~1L, (g_ss.sel.bottom - g_ss.sel.top) & ~1L,
                              g_ss.sel.left - mon.left, g_ss.sel.top - mon.top);
                OffsetRect(&mon, -v.left, -v.top);
                RECT lr{sr.left, sr.top - MulDiv(32, (int)dpi, 96), sr.left + MulDiv(378, (int)dpi, 96), sr.top - t};
                if (lr.top < mon.top) {
                    lr = RECT{sr.left, sr.bottom + t, sr.left + MulDiv(378, (int)dpi, 96), sr.bottom + MulDiv(32, (int)dpi, 96)};
                    if (lr.bottom > mon.bottom) lr = RECT{sr.left + t * 4, sr.top + t * 4, sr.left + MulDiv(378, (int)dpi, 96), sr.top + MulDiv(36, (int)dpi, 96)};
                }
                DrawTextW(mem, sz, -1, &lr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            }
            SelectObject(mem, of);
            BitBlt(dc, pr.left, pr.top, pw, ph, mem, pr.left, pr.top, SRCCOPY);
            SelectObject(mem, ob);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

static void BeginSelect(bool preset, int pw, int ph, bool pick = false) {
    if (!g_sel || g_ss.active) return;
    g_ss = SelState{};
    g_ss.active = true;
    g_ss.pick = pick;
    g_ss.preset = preset;
    g_ss.pw = pw;
    g_ss.ph = ph;
    if (g_border) ShowWindow(g_border, SW_HIDE);
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT r, LPARAM) -> BOOL {
        g_ss.mons.push_back(*r);
        return TRUE;
    }, 0);
    ShowWindow(g_hwnd, SW_HIDE);
    Sleep(150);  // let the main window disappear before the screen dims
    RECT v = VirtualRect();
    if (preset || pick) {
        POINT cp;
        GetCursorPos(&cp);
        if (pick) SelUpdatePick(cp);
        else SelUpdatePreset(cp);
    }
    SetWindowPos(g_sel, HWND_TOPMOST, v.left, v.top, v.right - v.left, v.bottom - v.top, SWP_SHOWWINDOW);
    SetForegroundWindow(g_sel);
    SetFocus(g_sel);
    SetCapture(g_sel);
    g_ss.drawn = SelBounds();
    InvalidateRect(g_sel, nullptr, FALSE);  // first paint covers everything
    if (GetEnvironmentVariableW(L"KREC_DEBUG_SELTEST", nullptr, 0) > 0) {
        // Developer test hook: synthesize a drag (or a click for presets) in client coordinates.
        HWND hs = g_sel;
        bool pr = preset;
        std::thread([hs, pr, v] {
            auto pos = [&](int x, int y) { return MAKELPARAM(x - v.left, y - v.top); };
            Sleep(800);
            if (pr) {
                PostMessageW(hs, WM_MOUSEMOVE, 0, pos(700, 420));
                Sleep(800);
                for (int i = 0; i < 5; i++) PostMessageW(hs, WM_KEYDOWN, VK_DOWN, 0);
                Sleep(300);
                PostMessageW(hs, WM_LBUTTONDOWN, MK_LBUTTON, pos(700, 420));
                return;
            }
            PostMessageW(hs, WM_LBUTTONDOWN, MK_LBUTTON, pos(600, 300));
            Sleep(200);
            PostMessageW(hs, WM_MOUSEMOVE, MK_LBUTTON, pos(800, 400));
            Sleep(200);
            PostMessageW(hs, WM_MOUSEMOVE, MK_LBUTTON, pos(1001, 551));
            Sleep(1200);
            PostMessageW(hs, WM_LBUTTONUP, 0, pos(1001, 551));
        }).detach();
    }
}

// ---------------------------------------------------------------- UI state

static void FillMonitors() {
    g_mons = EnumMonitors();
    HWND cb = Ctl(IDC_CB_MON);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    int sel = 0;
    for (size_t i = 0; i < g_mons.size(); i++) {
        auto& m = g_mons[i];
        wchar_t b[160];
        swprintf(b, 160, L"모니터 %d%ls  ·  %ld×%ld%ls", m.number, m.primary ? L" (주 모니터)" : L"",
                 m.rc.right - m.rc.left, m.rc.bottom - m.rc.top, m.hdr ? L"  ·  HDR" : L"");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)b);
        if (m.deviceName == S.monitor) sel = (int)i;
        else if (S.monitor.empty() && m.primary) sel = (int)i;
    }
    if (g_mons.empty()) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"모니터를 찾을 수 없어요");
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
    if (S.mode == 1) {
        for (size_t i = 0; i < g_mons.size(); i++)
            if (g_mons[i].deviceName == S.regionMon) SendMessageW(cb, CB_SETCURSEL, i, 0);
    }
}
static int FindMonitor(const std::wstring& dev) {
    for (size_t i = 0; i < g_mons.size(); i++)
        if (g_mons[i].deviceName == dev) return (int)i;
    return -1;
}

// Region border: blue while waiting, red while recording. Never captured (excluded from capture).
static void UpdateBorder() {
    if (!g_border) return;
    RECT cap{};
    bool show = false;
    COLORREF col = RGB(40, 150, 255);
    if (g_ss.active || !S.border) {
        show = false;
    } else if (g_sess) {
        if (g_sess->cfg.region || g_sess->cfg.window) {
            cap = CaptureRect(g_sess->cfg);
            show = true;
            if (g_sess->want != W_STOPPED) col = RGB(235, 40, 40);
        }
    } else if (S.mode == 1) {
        int mi = FindMonitor(S.regionMon);
        if (mi >= 0 && S.rw > 0 && S.rh > 0) {
            const RECT& m = g_mons[mi].rc;
            cap = RECT{m.left + S.rx, m.top + S.ry, m.left + S.rx + S.rw, m.top + S.ry + S.rh};
            show = true;
        }
    }
    if (!show) {
        ShowWindow(g_border, SW_HIDE);
        return;
    }
    if (g_bd.active) return;
    UINT t = BorderThickness(GetDpiForWindow(g_border));
    g_borderColor = col;
    SetWindowPos(g_border, HWND_TOPMOST, cap.left - t, cap.top - t, (cap.right - cap.left) + 2 * t,
                 (cap.bottom - cap.top) + 2 * t, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_border, nullptr, TRUE);
}

static void UpdateRegionLabel() {
    std::wstring t = L"아직 영역이 없어요";
    int mi = FindMonitor(S.regionMon);
    if (mi >= 0 && S.rw > 0 && S.rh > 0)
        t = L"모니터 " + std::to_wstring(g_mons[mi].number) + L" · " + std::to_wstring(S.rw) + L"×" + std::to_wstring(S.rh);
    SetWindowTextW(Ctl(IDC_LBL_REGION), t.c_str());
    // In region mode the monitor list just shows which monitor the region is on.
    if (S.mode == 1 && mi >= 0) SendMessageW(Ctl(IDC_CB_MON), CB_SETCURSEL, mi, 0);
}

static std::vector<WinInfo> g_wins;  // window list shown in the combo (may start with a "not open" entry)

static bool IEq(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// The remembered window, found by program name first (titles change), then by class/title.
static HWND FindRememberedWindow() {
    if (S.winExe.empty() && S.winTitle.empty()) return nullptr;
    auto wins = EnumRecordableWindows();
    HWND best = nullptr;
    int bestScore = 0;
    long long bestArea = -1;
    for (auto& w : wins) {
        int sc = 0;
        if (!S.winExe.empty() && IEq(w.exe, S.winExe)) sc += 4;
        if (!S.winClass.empty() && w.cls == S.winClass) sc += 2;
        if (!S.winTitle.empty() && w.title == S.winTitle) sc += 1;
        if (S.winExe.empty() || w.exe.empty()) { if (sc < 2) continue; }  // no program name: need class or title match
        else if (!(sc & 4)) continue;
        RECT r = WindowBounds(w.hwnd);
        long long area = (long long)(r.right - r.left) * (r.bottom - r.top);
        if (sc > bestScore || (sc == bestScore && area > bestArea)) { best = w.hwnd; bestScore = sc; bestArea = area; }
    }
    return best;
}

static void FillWindows() {
    g_wins = EnumRecordableWindows();
    HWND cur = FindRememberedWindow();
    HWND cb = Ctl(IDC_CB_WIN);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    if (!cur && (!S.winExe.empty() || !S.winTitle.empty())) {
        WinInfo missing;
        missing.title = S.winTitle;
        missing.exe = S.winExe;
        g_wins.insert(g_wins.begin(), missing);
    }
    for (size_t i = 0; i < g_wins.size(); i++) {
        auto& w = g_wins[i];
        std::wstring t = w.title.size() > 50 ? w.title.substr(0, 50) + L"…" : w.title;
        if (!w.hwnd) t = L"[지금 안 열려 있음] " + (w.exe.empty() ? t : w.exe);
        else if (!w.exe.empty()) t += L"  —  " + w.exe;
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t.c_str());
        if ((w.hwnd && w.hwnd == cur) || (!w.hwnd && !cur)) sel = (int)i;
    }
    if (g_wins.empty()) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"녹화할 수 있는 창이 없어요");
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

static void ApplyWindow(HWND h) {
    if (!h) return;
    S.winExe = WindowExe(h);
    S.winTitle = WindowTitle(h);
    S.winClass = WindowClass(h);
    S.mode = 2;
    SaveSettings();
    Button_SetCheck(Ctl(IDC_RB_FULL), BST_UNCHECKED);
    Button_SetCheck(Ctl(IDC_RB_REGION), BST_UNCHECKED);
    Button_SetCheck(Ctl(IDC_RB_WINDOW), BST_CHECKED);
    FillWindows();
    UpdateUI();
}

// Show only the controls that belong to the selected mode.
static void UpdateModeRows() {
    static const int regionIds[] = {IDC_BTN_DRAW, IDC_BTN_P1080, IDC_BTN_P720, IDC_LBL_REGION};
    static const int windowIds[] = {IDC_CB_WIN, IDC_BTN_PICKWIN, IDC_BTN_WINREFRESH};
    for (int id : regionIds) ShowWindow(Ctl(id), S.mode == 1 ? SW_SHOW : SW_HIDE);
    for (int id : windowIds) ShowWindow(Ctl(id), S.mode == 2 ? SW_SHOW : SW_HIDE);
    SetWindowTextW(Ctl(IDC_LBL_PRESET), S.mode == 2 ? L"전체화면 프로그램은 '모니터 전체'로" : L"");
}

static void ApplyRegion(RECT r) {
    g_mons = EnumMonitors();
    POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    int mi = -1;
    for (size_t i = 0; i < g_mons.size(); i++)
        if (PtInRect(&g_mons[i].rc, c)) mi = (int)i;
    if (mi < 0) return;
    const RECT& m = g_mons[mi].rc;
    S.regionMon = g_mons[mi].deviceName;
    S.rx = std::max(0L, r.left - m.left);
    S.ry = std::max(0L, r.top - m.top);
    S.rw = (std::min(r.right, m.right) - m.left - S.rx) & ~1;  // encoders need even sizes
    S.rh = (std::min(r.bottom, m.bottom) - m.top - S.ry) & ~1;
    S.mode = 1;
    S.monitor = S.regionMon;
    SaveSettings();
    Button_SetCheck(Ctl(IDC_RB_FULL), BST_UNCHECKED);
    Button_SetCheck(Ctl(IDC_RB_WINDOW), BST_UNCHECKED);
    Button_SetCheck(Ctl(IDC_RB_REGION), BST_CHECKED);
    UpdateRegionLabel();
    UpdateUI();
}

static std::vector<AudioDevice> g_mics;

static std::wstring OutputsText() {
    std::wstring t;
    int n = 0;
    for (auto& id : S.outputs) {
        std::wstring name = L"기본 출력 장치";
        if (!id.empty()) {
            name = L"(연결 안 됨)";
            for (auto& d : g_devs)
                if (d.id == id) name = d.name;
        }
        if (n++) t += L", ";
        t += name;
    }
    if (t.empty()) t = L"기본 출력 장치";
    return L"출력 장치: " + t + L"  ▾";
}

static void FillDevices() {
    g_devs = EnumOutputDevices();
    SetWindowTextW(Ctl(IDC_BTN_OUTDEV), OutputsText().c_str());
    g_mics = EnumInputDevices();
    HWND cb = Ctl(IDC_CB_MICDEV);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"기본 마이크 (윈도우 설정 따라감)");
    int sel = 0;
    for (size_t i = 0; i < g_mics.size(); i++) {
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)g_mics[i].name.c_str());
        if (g_mics[i].id == S.micDev) sel = (int)i + 1;
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

// Pop-up menu with a check mark per playback device; several can be recorded together.
static void PickOutputs() {
    g_devs = EnumOutputDevices();
    HMENU m = CreatePopupMenu();
    auto has = [](const std::wstring& id) { return std::find(S.outputs.begin(), S.outputs.end(), id) != S.outputs.end(); };
    AppendMenuW(m, MF_STRING | (has(L"") ? MF_CHECKED : 0), 1000, L"기본 출력 장치 (윈도우 설정 따라감)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < g_devs.size(); i++)
        AppendMenuW(m, MF_STRING | (has(g_devs[i].id) ? MF_CHECKED : 0), 1001 + (UINT)i, g_devs[i].name.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"여러 개를 체크하면 같이 녹음돼요");
    RECT r;
    GetWindowRect(Ctl(IDC_BTN_OUTDEV), &r);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (!cmd) return;
    if (cmd == 1000) {
        S.outputs = {L""};
    } else if (cmd > 1000 && cmd - 1001 < g_devs.size()) {
        const std::wstring& id = g_devs[cmd - 1001].id;
        auto it = std::find(S.outputs.begin(), S.outputs.end(), id);
        if (it != S.outputs.end()) S.outputs.erase(it);
        else {
            S.outputs.erase(std::remove(S.outputs.begin(), S.outputs.end(), std::wstring()), S.outputs.end());
            S.outputs.push_back(id);
        }
        if (S.outputs.empty()) S.outputs = {L""};
    }
    SaveSettings();
    SetWindowTextW(Ctl(IDC_BTN_OUTDEV), OutputsText().c_str());
}

static void UpdateVolumeLabels() {
    wchar_t b[16];
    swprintf(b, 16, L"%d%%", S.sysVol);
    SetWindowTextW(Ctl(IDC_LBL_SYSVOLV), b);
    swprintf(b, 16, L"%d%%", S.micVol);
    SetWindowTextW(Ctl(IDC_LBL_MICVOLV), b);
}

static void UpdateEncStatus() {
    std::wstring s;
    if (g_ffmpegMissing) s = L"⚠ ffmpeg.exe가 없어요";
    else if (g_nvOk < 0 || g_qsvOk < 0) s = L"그래픽카드 확인 중...";
    else {
        s = std::wstring(L"NVIDIA ") + (g_nvOk ? L"✔" : L"✘") + L"   Intel " + (g_qsvOk ? L"✔" : L"✘") + L"   CPU ✔";
        if (S.encoder == 0) s += std::wstring(L"\n자동 → ") + (g_nvOk ? L"NVIDIA 사용" : g_qsvOk ? L"Intel 사용" : L"CPU 사용");
    }
    SetWindowTextW(Ctl(IDC_LBL_ENCST), s.c_str());
}

static void UpdateStatusForWant() {
    if (!g_sess) return;
    switch (g_sess->want) {
        case W_REC:
            SetStatus(std::wstring(L"녹화 중") + (g_sess->encKnown ? std::wstring(L" · 압축: ") + EncLabel(g_sess->enc) : L"") +
                      (!g_sess->cfg.audio ? L" · 소리 없음"
                       : g_sess->cfg.sysAudio && g_sess->cfg.mic ? L" · 컴퓨터 소리 + 마이크"
                       : g_sess->cfg.mic ? L" · 마이크만" : L" · 컴퓨터 소리"));
            break;
        case W_PAUSED:
            SetStatus(L"일시정지 중 · " + HotkeyText(S.pauseVk, S.pauseMods) + L"를 누르면 이어서 녹화돼요.");
            break;
        case W_STOPPED: SetStatus(L"저장하는 중..."); break;
    }
}

static void UpdateUI() {
    bool idle = !g_sess;
    Want w = g_sess ? g_sess->want : W_STOPPED;
    UpdateModeRows();
    static const int settingIds[] = {IDC_CB_MON, IDC_CHK_CURSOR, IDC_CHK_HDROFF, IDC_RB_FULL, IDC_RB_REGION, IDC_BTN_DRAW,
                                     IDC_BTN_P1080, IDC_BTN_P720, IDC_CHK_BORDER, IDC_RB_WINDOW, IDC_CB_WIN, IDC_BTN_PICKWIN,
                                     IDC_BTN_WINREFRESH, IDC_CHK_ADMIN, IDC_CHK_SYS, IDC_BTN_OUTDEV, IDC_CB_MICDEV, IDC_CHK_SEPARATE, IDC_CB_SYNC, IDC_RB_30, IDC_RB_60, IDC_RB_120,
                                     IDC_CB_Q, IDC_CB_ENC, IDC_HK_REC, IDC_HK_PAUSE, IDC_BTN_DIR};
    for (int id : settingIds) EnableWindow(Ctl(id), idle);
    if (idle) {
        EnableWindow(Ctl(IDC_BTN_OUTDEV), S.sysAudio);
        EnableWindow(Ctl(IDC_CB_MICDEV), S.micOn);
        EnableWindow(Ctl(IDC_CHK_SEPARATE), S.sysAudio && S.micOn);
        EnableWindow(Ctl(IDC_CB_SYNC), S.sysAudio || S.micOn);
        EnableWindow(Ctl(IDC_CB_MON), S.mode == 0);
    }
    // Volumes always adjustable (also while recording). The mic switch works live only if the
    // mic was on when recording started (otherwise the microphone isn't open).
    EnableWindow(Ctl(IDC_TB_SYSVOL), S.sysAudio || !idle);
    EnableWindow(Ctl(IDC_TB_MICVOL), S.micOn || !idle);
    EnableWindow(Ctl(IDC_CHK_MIC), idle || (g_sess && g_sess->cfg.mic));

    std::wstring rt;
    bool recEnabled = true;
    if (idle) { rt = L"● 녹화 시작"; recEnabled = !g_ffmpegMissing && !g_mons.empty(); }
    else if (w == W_STOPPED) { rt = L"저장하는 중..."; recEnabled = false; }
    else rt = L"■ 정지하고 저장";
    if (recEnabled) rt += L"  (" + HotkeyText(S.recVk, S.recMods) + L")";
    SetWindowTextW(Ctl(IDC_BTN_REC), rt.c_str());
    EnableWindow(Ctl(IDC_BTN_REC), recEnabled);

    bool live = !idle && w != W_STOPPED;
    SetWindowTextW(Ctl(IDC_BTN_PAUSE), (!idle && w == W_PAUSED) ? L"▶ 이어서 녹화" : L"Ⅱ 일시정지");
    EnableWindow(Ctl(IDC_BTN_PAUSE), live);
    SetWindowTextW(Ctl(IDC_LBL_TIME), FormatTime(ElapsedMs()).c_str());
    TrayUpdate();
    ShowOverlay(live);
    UpdateBorder();
}

static void ApplyHotkeyControls() {
    auto toCtl = [](UINT mods) {
        WORD f = 0;
        if (mods & 1) f |= HOTKEYF_SHIFT;
        if (mods & 2) f |= HOTKEYF_CONTROL;
        if (mods & 4) f |= HOTKEYF_ALT;
        return f;
    };
    SendMessageW(Ctl(IDC_HK_REC), HKM_SETHOTKEY, MAKEWORD(S.recVk, toCtl(S.recMods)), 0);
    SendMessageW(Ctl(IDC_HK_PAUSE), HKM_SETHOTKEY, MAKEWORD(S.pauseVk, toCtl(S.pauseMods)), 0);
}
static void ReadHotkeyControl(int id, UINT& vk, UINT& mods) {
    LRESULT r = SendMessageW(Ctl(id), HKM_GETHOTKEY, 0, 0);
    UINT v = LOBYTE(LOWORD(r)), f = HIBYTE(LOWORD(r));
    if (!v) return;  // cleared — keep the previous key
    vk = v;
    mods = ((f & HOTKEYF_SHIFT) ? 1 : 0) | ((f & HOTKEYF_CONTROL) ? 2 : 0) | ((f & HOTKEYF_ALT) ? 4 : 0);
}

// ---------------------------------------------------------------- actions

static void Pump();

// Called from the UI timer. FFmpeg normally adds to the file every ~2 seconds (one fragment per
// keyframe). If it quits on its own or the file stops growing, end that piece (it stays playable)
// and start a new one, so the rest of the recording isn't lost.
static void Watchdog(HWND hwnd) {
    Session* s = g_sess;
    if (!s) return;
    if (s->waitWindow) {
        if (s->want != W_REC) { s->waitWindow = false; Pump(); return; }
        if (!IsWindow(s->cfg.hwnd)) {
            HWND w2 = FindRememberedWindow();
            if (w2) s->cfg.hwnd = w2;
        }
        if (IsWindow(s->cfg.hwnd) && !IsIconic(s->cfg.hwnd)) {
            s->waitWindow = false;
            Pump();
        }
        return;
    }
    if (!s->segActive || s->busy || s->want != W_REC || !s->proc.h || s->segs.empty()) return;
    ULONGLONG now = GetTickCount64();
    bool dead = WaitForSingleObject(s->proc.h, 0) == WAIT_OBJECT_0;
    // Progress = the file grew, or FFmpeg's frame counter (printed every 2 s) went up. The file alone
    // isn't enough: with very little motion it can stay the same size for several seconds.
    long long sz = FileSize(s->segs.back());
    long long frames = -1;
    {
        std::wstring t = ReadTail(AppDataDir() + L"\\ffmpeg_last.log", 600);
        size_t k = t.rfind(L"frame=");
        if (k != std::wstring::npos) {
            k += 6;
            while (k < t.size() && t[k] == L' ') k++;
            if (k < t.size() && iswdigit(t[k])) frames = _wtoi64(t.c_str() + k);
        }
    }
    if (sz != s->wdSize || (frames >= 0 && frames != s->wdFrames)) {
        s->wdSize = sz;
        s->wdFrames = frames;
        s->wdTick = now;
    }
    const ULONGLONG kStallMs = 7000;
    bool stalled = now - s->wdTick > kStallMs;
    if (!dead && !stalled) return;
    s->busy = true;
    s->wdLost++;
    int n = s->wdLost;
    std::string why;
    if (dead) {
        DWORD code = 0;
        GetExitCodeProcess(s->proc.h, &code);
        why = "ffmpeg quit by itself (exit code " + std::to_string((long)code) + ")";
    } else {
        why = "no progress for " + std::to_string(kStallMs / 1000) + " s (file " + std::to_string(sz) + " bytes, frame " + std::to_string(frames) + ")";
    }
    static const char* encNames[] = {"nvenc-direct", "nvenc-download", "qsv", "x264"};
    why += std::string(" [video path ") + encNames[s->enc] + (s->cfg.window ? ", window" : s->cfg.region ? ", region" : ", monitor") + "]";
    std::thread([s, hwnd, n, why] {
        std::wstring last = AppDataDir() + L"\\ffmpeg_last.log";
        std::wstring keep = AppDataDir() + L"\\ffmpeg_lost_" + StampNow() + L".log";
        LogProblem("segment " + Narrow(FileNameOf(s->segs.back())) + ": " + why + "; starting a new segment. FFmpeg log saved as " +
                   Narrow(FileNameOf(keep)));
        if (s->audio) LogProblem("  sound at that moment: " + s->audio->PipeReport());
        // Snapshot of where FFmpeg's threads are stuck (only once per recording; a few MB).
        if (n == 1 && s->proc.h) {
            std::wstring dmp = AppDataDir() + L"\\ffmpeg_stall_" + StampNow() + L".dmp";
            // Run it on its own thread with a time limit, so it can never hold up the recording.
            HANDLE dup = nullptr;
            DuplicateHandle(GetCurrentProcess(), s->proc.h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
            if (dup) {
                auto done = std::make_shared<std::atomic<int>>(0);
                std::thread([dup, dmp, done] {
                    bool ok = WriteMiniDump(dup, dmp);
                    CloseHandle(dup);
                    done->store(ok ? 1 : 2);
                }).detach();
                for (int i = 0; i < 50 && done->load() == 0; i++) Sleep(100);
                if (done->load() == 1) LogProblem("  FFmpeg snapshot saved as " + Narrow(FileNameOf(dmp)));
                else LogProblem("  FFmpeg snapshot could not be taken");
            }
        }
        StopSegment(s, 1000);
        CopyFileW(last.c_str(), keep.c_str(), FALSE);
        PostMessageW(hwnd, WM_APP_SEGLOST, 0, 0);
    }).detach();
}

// Starts a worker if FFmpeg isn't yet doing what the user wants.
static void Pump() {
    Session* s = g_sess;
    if (!s || s->busy) return;
    HWND hwnd = g_hwnd;
    if (s->want == W_STOPPED) {
        s->busy = true;
        std::thread([s, hwnd] { FinalizeWorker(s, hwnd); }).detach();
    } else if (s->want == W_REC && !s->segActive && !s->waitWindow) {
        s->busy = true;
        ResetEvent(s->wake);
        std::thread([s, hwnd] { StartSegmentWorker(s, hwnd); }).detach();
    } else if (s->want == W_PAUSED && s->segActive) {
        s->busy = true;
        std::thread([s, hwnd] {
            StopSegment(s);
            PostMessageW(hwnd, WM_APP_PAUSED, 0, 0);
        }).detach();
    }
}

static void SetWant(Want w) {
    Session* s = g_sess;
    if (!s || s->want == w) return;
    ULONGLONG now = GetTickCount64();
    if (s->want == W_REC) s->accMs += now - s->segStart;  // freeze the timer at the moment of the key press
    if (w == W_REC) s->segStart = now;
    s->want = w;
    SetEvent(s->wake);
    UpdateStatusForWant();
    UpdateUI();
    Pump();
}

static void StartRecording() {
    if (g_sess) return;
    if (!FileExists(FfmpegPath())) {
        MessageBoxW(g_hwnd, L"ffmpeg.exe가 프로그램과 같은 폴더에 있어야 해요.\n같은 폴더의 ffmpeg_받기.bat을 먼저 실행해 주세요.", kAppTitle, MB_ICONERROR);
        return;
    }
    int mi = (int)SendMessageW(Ctl(IDC_CB_MON), CB_GETCURSEL, 0, 0);
    bool region = S.mode == 1;
    if (region) {
        g_mons = EnumMonitors();
        mi = FindMonitor(S.regionMon);
        bool ok = mi >= 0 && S.rw >= 16 && S.rh >= 16;
        if (ok) {
            const RECT& m = g_mons[mi].rc;
            ok = S.rx + S.rw <= m.right - m.left && S.ry + S.rh <= m.bottom - m.top;
        }
        if (!ok) {  // no region yet, or the monitor/resolution changed
            ShowMain();
            BeginSelect(false, 0, 0);
            return;
        }
    }
    bool window = S.mode == 2;
    HWND target = nullptr;
    if (window) {
        target = FindRememberedWindow();
        if (!target) {
            ShowMain();
            MessageBoxW(g_hwnd, (L"녹화할 창을 찾을 수 없어요" + (S.winExe.empty() ? std::wstring() : L" (" + S.winExe + L")") +
                                 L".\n프로그램이 켜져 있는지 확인하거나, [창 고르기]로 다시 골라주세요.").c_str(), kAppTitle, MB_ICONWARNING);
            FillWindows();
            return;
        }
        if (IsIconic(target)) {
            ShowMain();
            MessageBoxW(g_hwnd, L"녹화할 창이 최소화돼 있어요. 창을 화면에 띄운 다음 다시 시작해 주세요.", kAppTitle, MB_ICONWARNING);
            return;
        }
        g_mons = EnumMonitors();
        RECT wb = WindowBounds(target);
        POINT c{(wb.left + wb.right) / 2, (wb.top + wb.bottom) / 2};
        mi = -1;
        for (size_t i = 0; i < g_mons.size(); i++)
            if (PtInRect(&g_mons[i].rc, c)) mi = (int)i;
        if (mi < 0)
            for (size_t i = 0; i < g_mons.size(); i++)
                if (g_mons[i].primary) mi = (int)i;
    }
    if (mi < 0 || mi >= (int)g_mons.size()) return;
    if (!EnsureDir(S.saveDir)) {
        MessageBoxW(g_hwnd, (L"저장 폴더를 만들 수 없어요:\n" + S.saveDir).c_str(), kAppTitle, MB_ICONERROR);
        return;
    }
    auto* s = new Session();
    s->cfg.mon = g_mons[mi];
    s->cfg.cursor = S.cursor;
    s->cfg.fps = S.fps;
    s->cfg.quality = S.quality;
    s->cfg.sysAudio = S.sysAudio && !S.outputs.empty();
    s->cfg.outputs = S.outputs;
    s->cfg.mic = S.micOn;
    s->cfg.micId = S.micDev;
    s->cfg.separate = S.separate;
    s->cfg.audio = s->cfg.sysAudio || s->cfg.mic;
    s->cfg.encoderMode = S.encoder;
    if (window) {
        s->cfg.window = true;
        s->cfg.hwnd = target;
    }
    if (region) {
        s->cfg.region = true;
        s->cfg.rx = S.rx;
        s->cfg.ry = S.ry;
        s->cfg.rw = S.rw;
        s->cfg.rh = S.rh;
    }
    s->chain = BuildChain(S.encoder, s->cfg.mon.vendor, false);
    s->wantHdrOff = S.hdrOff && s->cfg.mon.hdr;
    std::wstring stamp = StampNow();
    s->finalPath = UniquePath(S.saveDir, stamp, L".mp4");
    s->tempDir = S.saveDir + L"\\.녹화중_" + stamp;
    EnsureDir(s->tempDir);
    SetFileAttributesW(s->tempDir.c_str(), FILE_ATTRIBUTE_HIDDEN);
    s->want = W_REC;
    s->segStart = GetTickCount64();
    g_sess = s;
    if (S.hideOnRec) ShowWindow(g_hwnd, SW_HIDE);  // hide before capture starts so the window isn't recorded
    SetTimer(g_hwnd, 1, 250, nullptr);
    UpdateStatusForWant();
    UpdateUI();
    Pump();
}

static void StopRecording() { SetWant(W_STOPPED); }

static void TogglePause() {
    if (!g_sess) return;
    if (g_sess->want == W_REC) SetWant(W_PAUSED);
    else if (g_sess->want == W_PAUSED) SetWant(W_REC);
}

static void ToggleRecord() {
    if (!g_sess) StartRecording();
    else if (g_sess->want != W_STOPPED) StopRecording();
}

static void OpenFolder(const std::wstring& selectFile) {
    if (!selectFile.empty() && FileExists(selectFile)) {
        std::wstring args = L"/select,\"" + selectFile + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        EnsureDir(S.saveDir);
        ShellExecuteW(nullptr, L"open", S.saveDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// Newest .mp4 in the save folder (when nothing was recorded since the program started).
static std::wstring NewestRecording() {
    std::wstring best;
    FILETIME bestT{};
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((S.saveDir + L"\\*.mp4").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return best;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (best.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestT) > 0) {
            best = S.saveDir + L"\\" + fd.cFileName;
            bestT = fd.ftLastWriteTime;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return best;
}

static std::wstring LastRecording() {
    if (!g_lastSaved.empty() && FileExists(g_lastSaved)) return g_lastSaved;
    return NewestRecording();
}

// Opens the video editor (편집기프로그램\Editor.exe) with the last recording.
static void OpenEditor() {
    std::wstring exe = ExeDir() + L"\\편집기프로그램\\Editor.exe";
    if (!FileExists(exe)) exe = ExeDir() + L"\\Editor.exe";
    if (!FileExists(exe)) {
        MessageBoxW(g_hwnd, L"편집기를 찾지 못했어요.\n녹화 프로그램 폴더 안의 '편집기프로그램' 폴더에 Editor.exe가 있어야 해요.",
                    kAppTitle, MB_ICONWARNING);
        return;
    }
    std::wstring file = LastRecording();
    std::wstring args = file.empty() ? L"" : L"\"" + file + L"\"";
    std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
    ShellExecuteW(nullptr, L"open", exe.c_str(), args.empty() ? nullptr : args.c_str(), dir.c_str(), SW_SHOWNORMAL);
}

static void PickFolder() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD opts;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"녹화 파일을 저장할 폴더 선택");
    IShellItem* cur = nullptr;
    if (EnsureDir(S.saveDir) && SUCCEEDED(SHCreateItemFromParsingName(S.saveDir.c_str(), nullptr, IID_PPV_ARGS(&cur)))) {
        d->SetFolder(cur);
        cur->Release();
    }
    if (SUCCEEDED(d->Show(g_hwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                S.saveDir = p;
                CoTaskMemFree(p);
                SetWindowTextW(Ctl(IDC_ED_DIR), S.saveDir.c_str());
                SaveSettings();
            }
            it->Release();
        }
    }
    d->Release();
}

static void DetectEncoders() {
    HWND hwnd = g_hwnd;
    std::thread([hwnd] {
        std::wstring ff = L"\"" + FfmpegPath() + L"\" -hide_banner -loglevel error -f lavfi -i color=c=black:s=640x360:r=30 -frames:v 5 ";
        std::wstring dir = AppDataDir();
        int nv = RunAndWait(ff + L"-c:v h264_nvenc -f null -", dir + L"\\test_nvenc.log", 20000) == 0;
        int qsv = RunAndWait(ff + L"-vf format=nv12 -c:v h264_qsv -f null -", dir + L"\\test_qsv.log", 20000) == 0;
        PostMessageW(hwnd, WM_APP_ENCDETECT, nv, qsv);
    }).detach();
}

// ---------------------------------------------------------------- keyboard hook

static LRESULT CALLBACK KbHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        DWORD vk = k->vkCode;
        if (vk < 256) {
            bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
            bool up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
            if (up) g_keyDown[vk] = false;
            if (down && !g_keyDown[vk]) {
                g_keyDown[vk] = true;
                bool isMod = vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL || vk == VK_LCONTROL ||
                             vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
                HWND f = GetFocus();
                bool editing = f && (f == Ctl(IDC_HK_REC) || f == Ctl(IDC_HK_PAUSE));
                if (!isMod && !editing) {
                    UINT mods = ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1 : 0) | ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 2 : 0) |
                                ((GetAsyncKeyState(VK_MENU) & 0x8000) ? 4 : 0);
                    // Only for keys Windows wouldn't let us register as global hotkeys.
                    if (vk == S.recVk && mods == S.recMods && !g_regRec) PostMessageW(g_hwnd, WM_APP_HOTKEY, 1, 0);
                    else if (vk == S.pauseVk && mods == S.pauseMods && !g_regPause) PostMessageW(g_hwnd, WM_APP_HOTKEY, 2, 0);
                }
            }
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static UINT ModFlags(UINT m) {
    return ((m & 1) ? MOD_SHIFT : 0) | ((m & 2) ? MOD_CONTROL : 0) | ((m & 4) ? MOD_ALT : 0) | MOD_NOREPEAT;
}
// Global hotkeys: Windows itself tells us when the key is pressed, whatever window is in front.
// The keyboard hook (which sees every key) is only used for keys that couldn't be registered.
static void RegisterHotkeys() {
    UnregisterHotKey(g_hwnd, 1);
    UnregisterHotKey(g_hwnd, 2);
    g_regRec = RegisterHotKey(g_hwnd, 1, ModFlags(S.recMods), S.recVk) != 0;
    bool same = S.pauseVk == S.recVk && S.pauseMods == S.recMods;
    g_regPause = !same && RegisterHotKey(g_hwnd, 2, ModFlags(S.pauseMods), S.pauseVk) != 0;
    bool needHook = !g_regRec || (!same && !g_regPause);
    if (needHook && !g_hook) g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KbHook, g_inst, 0);
    if (!needHook && g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
}
static std::wstring HotkeyWarning() {
    if (g_regRec && g_regPause) return L"";
    return L" ⚠ " + HotkeyText(!g_regRec ? S.recVk : S.pauseVk, !g_regRec ? S.recMods : S.pauseMods) +
           L"를 다른 프로그램(반디캠 등)이 쓰고 있어서, 전체화면에서는 안 먹을 수 있어요.";
}

// ---------------------------------------------------------------- layout

static void MakeFonts() {
    if (g_font) DeleteObject(g_font);
    if (g_fontBig) DeleteObject(g_fontBig);
    if (g_fontBtn) DeleteObject(g_fontBtn);
    g_font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
    g_fontBig = CreateFontW(-MulDiv(16, (int)g_dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
    g_fontBtn = CreateFontW(-MulDiv(10, (int)g_dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
}
static void Layout() {
    for (auto& c : kCtls) {
        HWND h = Ctl(c.id);
        MoveWindow(h, Sc(c.x), Sc(c.y), Sc(c.w), Sc(c.h), FALSE);
        SendMessageW(h, WM_SETFONT, (WPARAM)(c.font == 1 ? g_fontBig : c.font == 2 ? g_fontBtn : g_font), FALSE);
    }
    InvalidateRect(g_hwnd, nullptr, TRUE);
}
static void ResizeToClient() {
    RECT r{0, 0, Sc(kClientW), Sc(kClientH)};
    AdjustWindowRectExForDpi(&r, GetWindowLongW(g_hwnd, GWL_STYLE), FALSE, 0, g_dpi);
    SetWindowPos(g_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void CreateControls() {
    for (auto& c : kCtls) {
        CreateWindowExW(0, c.cls, c.text, WS_CHILD | WS_VISIBLE | c.style, 0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)c.id, g_inst, nullptr);
    }
    SendMessageW(Ctl(IDC_HK_REC), HKM_SETRULES, 0, 0);
    SendMessageW(Ctl(IDC_HK_PAUSE), HKM_SETRULES, 0, 0);
    HWND q = Ctl(IDC_CB_Q);
    for (const wchar_t* t : {L"보통", L"높음", L"최고"}) SendMessageW(q, CB_ADDSTRING, 0, (LPARAM)t);
    HWND sy = Ctl(IDC_CB_SYNC);
    for (int ms = 300; ms >= -300; ms -= 20) {
        wchar_t b[64];
        if (ms == 0) swprintf(b, 64, L"0ms (보정 없음)");
        else if (ms > 0) swprintf(b, 64, L"%dms 앞당기기", ms);
        else swprintf(b, 64, L"%dms 늦추기", -ms);
        SendMessageW(sy, CB_ADDSTRING, 0, (LPARAM)b);
    }
    HWND e = Ctl(IDC_CB_ENC);
    for (const wchar_t* t : {L"자동 (추천)", L"NVIDIA (RTX 가속)", L"Intel (내장 그래픽)", L"CPU (소프트웨어)"})
        SendMessageW(e, CB_ADDSTRING, 0, (LPARAM)t);
}

static void LoadControlsFromSettings() {
    FillMonitors();
    FillDevices();
    Button_SetCheck(Ctl(IDC_CHK_CURSOR), S.cursor);
    Button_SetCheck(Ctl(IDC_CHK_SYS), S.sysAudio);
    Button_SetCheck(Ctl(IDC_CHK_MIC), S.micOn);
    Button_SetCheck(Ctl(IDC_CHK_SEPARATE), S.separate);
    for (int tb : {IDC_TB_SYSVOL, IDC_TB_MICVOL}) {
        SendMessageW(Ctl(tb), TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
        SendMessageW(Ctl(tb), TBM_SETPAGESIZE, 0, 10);
    }
    SendMessageW(Ctl(IDC_TB_SYSVOL), TBM_SETPOS, TRUE, S.sysVol);
    SendMessageW(Ctl(IDC_TB_MICVOL), TBM_SETPOS, TRUE, S.micVol);
    UpdateVolumeLabels();
    Button_SetCheck(Ctl(IDC_RB_30), S.fps == 30);
    Button_SetCheck(Ctl(IDC_RB_60), S.fps == 60);
    Button_SetCheck(Ctl(IDC_RB_120), S.fps == 120);
    SendMessageW(Ctl(IDC_CB_Q), CB_SETCURSEL, S.quality, 0);
    SendMessageW(Ctl(IDC_CB_ENC), CB_SETCURSEL, S.encoder, 0);
    SendMessageW(Ctl(IDC_CB_SYNC), CB_SETCURSEL, (300 - (S.audioOffsetMs / 20) * 20) / 20, 0);
    SetWindowTextW(Ctl(IDC_ED_DIR), S.saveDir.c_str());
    Button_SetCheck(Ctl(IDC_CHK_HIDE), S.hideOnRec);
    Button_SetCheck(Ctl(IDC_CHK_NOTIFY), S.notify);
    Button_SetCheck(Ctl(IDC_CHK_HDROFF), S.hdrOff);
    Button_SetCheck(Ctl(IDC_CHK_BORDER), S.border);
    Button_SetCheck(Ctl(IDC_RB_FULL), S.mode == 0);
    Button_SetCheck(Ctl(IDC_RB_REGION), S.mode == 1);
    Button_SetCheck(Ctl(IDC_RB_WINDOW), S.mode == 2);
    Button_SetCheck(Ctl(IDC_CHK_ADMIN), S.admin);
    FillWindows();
    UpdateRegionLabel();
    ApplyHotkeyControls();
}

// ---------------------------------------------------------------- window proc

// Remember where the main window is (its normal position, even while hidden in the tray).
static void SaveWindowPos() {
    if (!g_hwnd) return;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(g_hwnd, &wp)) return;
    S.hasPos = true;
    S.posX = wp.rcNormalPosition.left;
    S.posY = wp.rcNormalPosition.top;
    SaveSettings();
}
// Put the window back where it was, keeping it fully on a connected screen.
static void RestoreWindowPos() {
    if (!S.hasPos) return;
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    RECT want{S.posX, S.posY, S.posX + w, S.posY + h};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST), &mi);  // nearest screen if that one is gone
    const RECT& wa = mi.rcWork;
    int x = std::clamp((int)S.posX, (int)wa.left, std::max((int)wa.left, (int)wa.right - w));
    int y = std::clamp((int)S.posY, (int)wa.top, std::max((int)wa.top, (int)wa.bottom - h));
    SetWindowPos(g_hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void RequestExit() {
    if (!g_sess) {
        DestroyWindow(g_hwnd);
        return;
    }
    ShowMain();
    if (g_sess->want != W_STOPPED) {
        int r = MessageBoxW(g_hwnd, L"녹화 중이에요. 지금까지 녹화한 내용을 저장하고 종료할까요?", kAppTitle, MB_YESNO | MB_ICONQUESTION);
        if (r != IDYES || !g_sess) return;
        g_exitAfterSave = true;
        StopRecording();
    } else {
        g_exitAfterSave = true;  // exits when saving finishes
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            g_hwnd = h;
            g_dpi = GetDpiForWindow(h);
            MakeFonts();
            CreateControls();
            Layout();
            ResizeToClient();
            LoadControlsFromSettings();
            return 0;

        case WM_DPICHANGED: {
            g_dpi = HIWORD(w);
            MakeFonts();
            Layout();
            RECT* r = (RECT*)l;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ResizeToClient();
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            switch (id) {
                case IDC_BTN_REC: ToggleRecord(); break;
                case IDC_BTN_PAUSE: TogglePause(); break;
                case IDC_BTN_DIR: PickFolder(); break;
                case IDC_BTN_OPEN: OpenFolder(L""); break;
                case IDC_BTN_EDITOR: OpenEditor(); break;
                case IDC_CB_MON:
                    if (code == CBN_SELCHANGE) {
                        int i = (int)SendMessageW(Ctl(IDC_CB_MON), CB_GETCURSEL, 0, 0);
                        if (i >= 0 && i < (int)g_mons.size()) { S.monitor = g_mons[i].deviceName; SaveSettings(); }
                    } else if (code == CBN_DROPDOWN && !g_sess) FillMonitors();
                    break;
                case IDC_BTN_OUTDEV: PickOutputs(); break;
                case IDC_CB_MICDEV:
                    if (code == CBN_SELCHANGE) {
                        int i = (int)SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0);
                        S.micDev = (i >= 1 && i <= (int)g_mics.size()) ? g_mics[i - 1].id : L"";
                        SaveSettings();
                    } else if (code == CBN_DROPDOWN && !g_sess) {
                        g_mics = EnumInputDevices();
                        FillDevices();
                    }
                    break;
                case IDC_CHK_MIC:
                    S.micOn = Button_GetCheck(Ctl(id)) == BST_CHECKED;
                    SaveSettings();
                    ApplyLiveAudio(g_liveAudio.load());  // works during recording too
                    UpdateUI();
                    if (g_sess) SetStatus(S.micOn ? L"마이크 켜짐" : L"마이크 꺼짐 (녹음 중에도 바로 적용돼요)");
                    break;
                case IDC_CHK_SEPARATE: S.separate = Button_GetCheck(Ctl(id)) == BST_CHECKED; SaveSettings(); break;
                case IDC_CHK_CURSOR: S.cursor = Button_GetCheck(Ctl(id)) == BST_CHECKED; SaveSettings(); break;
                case IDC_CHK_SYS:
                    S.sysAudio = Button_GetCheck(Ctl(id)) == BST_CHECKED;
                    SaveSettings();
                    UpdateUI();
                    break;
                case IDC_RB_30: case IDC_RB_60: case IDC_RB_120:
                    S.fps = Button_GetCheck(Ctl(IDC_RB_120)) == BST_CHECKED ? 120 : Button_GetCheck(Ctl(IDC_RB_60)) == BST_CHECKED ? 60 : 30;
                    SaveSettings();
                    break;
                case IDC_CB_Q:
                    if (code == CBN_SELCHANGE) { S.quality = (int)SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0); SaveSettings(); }
                    break;
                case IDC_CB_ENC:
                    if (code == CBN_SELCHANGE) { S.encoder = (int)SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0); SaveSettings(); UpdateEncStatus(); }
                    break;
                case IDC_HK_REC: case IDC_HK_PAUSE:
                    if (code == EN_CHANGE) {
                        if (id == IDC_HK_REC) ReadHotkeyControl(id, S.recVk, S.recMods);
                        else ReadHotkeyControl(id, S.pauseVk, S.pauseMods);
                        SaveSettings();
                        RegisterHotkeys();
                        UpdateUI();
                    }
                    break;
                case IDC_CHK_HIDE: S.hideOnRec = Button_GetCheck(Ctl(id)) == BST_CHECKED; SaveSettings(); break;
                case IDC_CB_SYNC:
                    if (code == CBN_SELCHANGE) {
                        int i = (int)SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0);
                        if (i >= 0) { S.audioOffsetMs = 300 - i * 20; SaveSettings(); }
                    }
                    break;
                case IDC_RB_FULL:
                case IDC_RB_REGION:
                case IDC_RB_WINDOW: {
                    int nm = Button_GetCheck(Ctl(IDC_RB_WINDOW)) == BST_CHECKED ? 2 : Button_GetCheck(Ctl(IDC_RB_REGION)) == BST_CHECKED ? 1 : 0;
                    if (nm == S.mode) break;
                    S.mode = nm;
                    SaveSettings();
                    if (nm == 0) {
                        int i = (int)SendMessageW(Ctl(IDC_CB_MON), CB_GETCURSEL, 0, 0);
                        if (i >= 0 && i < (int)g_mons.size()) S.monitor = g_mons[i].deviceName;
                    }
                    UpdateRegionLabel();
                    UpdateUI();
                    if (nm == 1 && (FindMonitor(S.regionMon) < 0 || S.rw <= 0)) BeginSelect(false, 0, 0);
                    if (nm == 2) FillWindows();
                    break;
                }
                case IDC_CB_WIN:
                    if (code == CBN_SELCHANGE) {
                        int i = (int)SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0);
                        if (i >= 0 && i < (int)g_wins.size() && g_wins[i].hwnd) ApplyWindow(g_wins[i].hwnd);
                    } else if (code == CBN_DROPDOWN && !g_sess) FillWindows();
                    break;
                case IDC_BTN_PICKWIN: BeginSelect(false, 0, 0, true); break;
                case IDC_BTN_WINREFRESH: FillWindows(); break;
                case IDC_CHK_ADMIN: {
                    S.admin = Button_GetCheck(Ctl(id)) == BST_CHECKED;
                    SaveSettings();
                    if (S.admin && IsElevated()) std::thread(EnsureAdminTask).detach();
                    if (!S.admin && IsElevated()) std::thread(DeleteAdminTask).detach();
                    if (S.admin && !IsElevated() &&
                        MessageBoxW(h, L"관리자 권한으로 다시 시작할까요?\n(확인 창은 이번 한 번만 떠요. 다음부터는 확인 창 없이 관리자 권한으로 켜져요)", kAppTitle,
                                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        g_relaunchAdmin = true;
                        DestroyWindow(h);
                    }
                    break;
                }
                case IDC_BTN_DRAW: BeginSelect(false, 0, 0); break;
                case IDC_BTN_P1080: BeginSelect(true, 1920, 1080); break;
                case IDC_BTN_P720: BeginSelect(true, 1280, 720); break;
                case IDC_CHK_BORDER: S.border = Button_GetCheck(Ctl(id)) == BST_CHECKED; SaveSettings(); UpdateBorder(); break;
                case IDC_CHK_HDROFF: S.hdrOff = Button_GetCheck(Ctl(id)) == BST_CHECKED; SaveSettings(); break;
                case IDC_CHK_NOTIFY: S.notify = Button_GetCheck(Ctl(id)) == BST_CHECKED; SaveSettings(); break;
                case IDM_TRAY_REC: ToggleRecord(); break;
                case IDM_TRAY_PAUSE: TogglePause(); break;
                case IDM_TRAY_SHOW: ShowMain(); break;
                case IDM_TRAY_FOLDER: OpenFolder(g_lastSaved); break;
                case IDM_TRAY_EXIT: RequestExit(); break;
                case IDM_TRAY_EDIT: OpenEditor(); break;
            }
            return 0;
        }

        case WM_HOTKEY: {
            // While a shortcut box is being edited, the pressed key goes into the box instead.
            HWND f = GetFocus();
            if (GetForegroundWindow() == h && f && (f == Ctl(IDC_HK_REC) || f == Ctl(IDC_HK_PAUSE))) {
                UINT mods = LOWORD(l), vk = HIWORD(l);
                WORD hf = ((mods & MOD_SHIFT) ? HOTKEYF_SHIFT : 0) | ((mods & MOD_CONTROL) ? HOTKEYF_CONTROL : 0) | ((mods & MOD_ALT) ? HOTKEYF_ALT : 0);
                SendMessageW(f, HKM_SETHOTKEY, MAKEWORD(vk, hf), 0);
                SendMessageW(h, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(f), EN_CHANGE), (LPARAM)f);
                return 0;
            }
            if (w == 1) ToggleRecord();
            else if (w == 2) TogglePause();
            return 0;
        }

        case WM_APP_HOTKEY:
            if (w == 1) ToggleRecord();
            else TogglePause();
            return 0;

        case WM_HSCROLL: {  // volume sliders (live)
            HWND tb = (HWND)l;
            int v = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
            if (tb == Ctl(IDC_TB_SYSVOL)) S.sysVol = v;
            else if (tb == Ctl(IDC_TB_MICVOL)) S.micVol = v;
            else return 0;
            UpdateVolumeLabels();
            ApplyLiveAudio(g_liveAudio.load());
            if (LOWORD(w) == TB_ENDTRACK || LOWORD(w) == TB_THUMBPOSITION) SaveSettings();
            return 0;
        }

        case WM_APP_ENCDETECT:
            g_nvOk = (int)w;
            g_qsvOk = (int)l;
            UpdateEncStatus();
            if (!g_sess) SetStatus(L"준비됐어요. " + HotkeyText(S.recVk, S.recMods) + L"를 누르면 녹화가 시작돼요." + HotkeyWarning());
            return 0;

        case WM_APP_SEGSTARTED:
            if (g_sess) {
                g_sess->busy = false;
                g_sess->segActive = true;
                g_sess->wdSize = -1;
                g_sess->wdFrames = -1;
                g_sess->wdTick = GetTickCount64();
                g_sess->encKnown = true;
                g_sess->enc = (Enc)w;
                UpdateStatusForWant();
                Pump();  // the user may have pressed pause/stop in the meantime
            }
            return 0;

        case WM_APP_STARTFAIL: {
            std::unique_ptr<std::wstring> msg((std::wstring*)l);
            Session* s = g_sess;
            if (!s) return 0;
            s->busy = false;
            if (!s->segs.empty()) {
                // Resuming failed — save what we already have.
                SetWant(W_STOPPED);
                ShowMain();
                MessageBoxW(h, (L"이어서 녹화하지 못해서 지금까지 녹화한 부분만 저장할게요.\n\n" + *msg).c_str(), kAppTitle, MB_ICONWARNING);
                return 0;
            }
            RemoveDirectoryW(s->tempDir.c_str());
            delete s;
            g_sess = nullptr;
            KillTimer(h, 1);
            UpdateUI();
            SetWindowTextW(Ctl(IDC_LBL_TIME), FormatTime(0).c_str());
            SetStatus(L"녹화를 시작하지 못했어요.");
            ShowMain();
            std::wstring text = L"녹화를 시작하지 못했어요.\n\n" + *msg +
                                L"\n\n자세한 기록: " + AppDataDir() + L"\\ffmpeg_last.log";
            MessageBoxW(h, text.c_str(), kAppTitle, MB_ICONERROR);
            if (g_exitAfterSave) DestroyWindow(h);
            return 0;
        }

        case WM_APP_RECOVERED: {
            std::unique_ptr<std::wstring> path((std::wstring*)l);
            g_lastSaved = *path;
            std::wstring t = (w > 1 ? L"저장되지 못한 녹화 " + std::to_wstring((int)w) + L"개를 복구했어요. 마지막: " : L"저장되지 못한 녹화를 복구했어요: ") +
                             FileNameOf(*path);
            if (!g_sess) SetStatus(t);
            TrayBalloon(L"녹화 복구 완료", t + L"\n클릭하면 저장된 폴더가 열려요.");
            return 0;
        }

        case WM_APP_SEGLOST:
            if (g_sess) {
                Session* s = g_sess;
                s->busy = false;
                s->segActive = false;
                if (s->cfg.window) {
                    if (!IsWindow(s->cfg.hwnd)) {
                        HWND w2 = FindRememberedWindow();
                        if (w2) s->cfg.hwnd = w2;
                    }
                    if (!IsWindow(s->cfg.hwnd) || IsIconic(s->cfg.hwnd)) s->waitWindow = true;
                }
                if (s->want == W_REC)
                    SetStatus(s->waitWindow ? L"녹화 대상 창이 다시 보이면 이어서 녹화할게요."
                                            : L"녹화가 잠깐 끊겨서 바로 이어서 녹화하고 있어요.");
                Pump();
            }
            return 0;

        case WM_APP_PAUSED:
            if (g_sess) {
                g_sess->busy = false;
                g_sess->segActive = false;
                Pump();
            }
            return 0;

        case WM_APP_SAVED:
        case WM_APP_SAVEFAIL: {
            std::unique_ptr<std::wstring> msg((std::wstring*)l);
            ULONGLONG dur = g_sess ? g_sess->accMs : 0;
            delete g_sess;
            g_sess = nullptr;
            KillTimer(h, 1);
            UpdateUI();
            SetWindowTextW(Ctl(IDC_LBL_TIME), FormatTime(dur).c_str());
            if (m == WM_APP_SAVED) {
                g_lastSaved = *msg;
                SetStatus(L"저장했어요: " + FileNameOf(*msg) + L"  (" + FormatTime(dur) + L")");
                if (S.notify) TrayBalloon(L"녹화 저장 완료", FileNameOf(*msg) + L"\n클릭하면 저장된 폴더가 열려요.");
            } else {
                SetStatus(L"저장하지 못했어요.");
                ShowMain();
                MessageBoxW(h, (L"저장하지 못했어요.\n\n" + *msg).c_str(), kAppTitle, MB_ICONERROR);
            }
            if (g_exitAfterSave) DestroyWindow(h);
            return 0;
        }

        case WM_TIMER:
            if (g_sess) {
                Watchdog(h);
                if (!g_sess) return 0;
                SetWindowTextW(Ctl(IDC_LBL_TIME), FormatTime(ElapsedMs()).c_str());
                if (g_sess->cfg.window && g_sess->want != W_STOPPED) {  // follow the window if it moves
                    UpdateBorder();
                    ShowOverlay(true);
                }
                if (g_overlay && IsWindowVisible(g_overlay)) {
                    SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    InvalidateRect(g_overlay, nullptr, FALSE);
                }
            }
            return 0;

        case WM_APP_TRAY:
            switch (LOWORD(l)) {
                case WM_LBUTTONDBLCLK:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    ShowMain();
                    break;
                case NIN_BALLOONUSERCLICK:
                    OpenFolder(g_lastSaved);
                    break;
                case WM_CONTEXTMENU: {
                    HMENU mn = CreatePopupMenu();
                    Want cw = g_sess ? g_sess->want : W_STOPPED;
                    bool busy = g_sess && cw == W_STOPPED;
                    bool live = g_sess && cw != W_STOPPED;
                    std::wstring recItem = (!g_sess ? L"녹화 시작\t" : L"정지하고 저장\t") + HotkeyText(S.recVk, S.recMods);
                    AppendMenuW(mn, MF_STRING | (busy ? MF_GRAYED : 0), IDM_TRAY_REC, recItem.c_str());
                    std::wstring pItem = (live && cw == W_PAUSED ? L"이어서 녹화\t" : L"일시정지\t") + HotkeyText(S.pauseVk, S.pauseMods);
                    AppendMenuW(mn, MF_STRING | (live ? 0 : MF_GRAYED), IDM_TRAY_PAUSE, pItem.c_str());
                    AppendMenuW(mn, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(mn, MF_STRING, IDM_TRAY_SHOW, L"창 열기");
                    AppendMenuW(mn, MF_STRING, IDM_TRAY_FOLDER, L"녹화 폴더 열기");
                    AppendMenuW(mn, MF_STRING, IDM_TRAY_EDIT, LastRecording().empty() ? L"편집기 열기" : L"마지막 녹화 영상 편집");
                    AppendMenuW(mn, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(mn, MF_STRING, IDM_TRAY_EXIT, L"종료");
                    POINT pt;
                    GetCursorPos(&pt);
                    SetForegroundWindow(h);
                    TrackPopupMenu(mn, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
                    PostMessageW(h, WM_NULL, 0, 0);
                    DestroyMenu(mn);
                    break;
                }
            }
            return 0;

        case WM_EXITSIZEMOVE:
            SaveWindowPos();
            return 0;

        case WM_DISPLAYCHANGE:
            if (!g_sess) {
                FillMonitors();
                UpdateRegionLabel();
                UpdateBorder();
            }
            return 0;

        case WM_CLOSE:
            RequestExit();
            return 0;

        case WM_DESTROY:
            SaveWindowPos();
            UnregisterHotKey(h, 1);
            UnregisterHotKey(h, 2);
            if (g_sess) RestoreHdr(g_sess);
            KillTimer(h, 1);
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_hook) UnhookWindowsHookEx(g_hook);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- entry

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    g_inst = hInst;
    LoadSettings();
    // "Run as administrator" option: restart elevated — through Task Scheduler (no prompt) when
    // possible, otherwise with the normal prompt. If the user declines the prompt, just run normally.
    if (S.admin && !IsElevated() && (StartAdminTask() || RelaunchElevated())) return 0;
    if (S.admin && IsElevated()) std::thread(EnsureAdminTask).detach();
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\KRecRecorderSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"KRecMain", nullptr);
        if (other) {
            ShowWindow(other, SW_SHOW);
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS | ICC_WIN95_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }

    LoadSettings();
    g_ffmpegMissing = !FileExists(FfmpegPath());

    g_icoIdle = MakeIcon(32, RGB(130, 130, 130), false);
    g_icoRec = MakeIcon(32, RGB(225, 35, 35), false);
    g_icoPause = MakeIcon(32, RGB(235, 170, 0), true);
    HICON appIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"KRecMain";
    wc.hIcon = appIcon ? appIcon : g_icoRec;
    wc.hIconSm = appIcon ? appIcon : g_icoRec;
    RegisterClassExW(&wc);

    WNDCLASSEXW oc{sizeof(oc)};
    oc.lpfnWndProc = OverlayProc;
    oc.hInstance = hInst;
    oc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    oc.lpszClassName = L"KRecOverlay";
    RegisterClassExW(&oc);
    WNDCLASSEXW bc{sizeof(bc)};
    bc.lpfnWndProc = BorderProc;
    bc.hInstance = hInst;
    bc.lpszClassName = L"KRecBorder";
    RegisterClassExW(&bc);
    WNDCLASSEXW sc{sizeof(sc)};
    sc.lpfnWndProc = SelectProc;
    sc.hInstance = hInst;
    sc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    sc.lpszClassName = L"KRecSelect";
    RegisterClassExW(&sc);

    std::wstring title = std::wstring(kAppTitle) + (IsElevated() ? L"  (관리자)" : L"");
    HWND hwnd = CreateWindowExW(0, L"KRecMain", title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 520, 580, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    g_overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                                L"KRecOverlay", L"", WS_POPUP, 0, 0, 170, 30, nullptr, nullptr, hInst, nullptr);
    if (g_overlay) {
        SetLayeredWindowAttributes(g_overlay, 0, 215, LWA_ALPHA);
        if (!SetWindowDisplayAffinity(g_overlay, WDA_EXCLUDEFROMCAPTURE)) {
            // Older Windows can't hide it from the recording, so don't show it at all.
            DestroyWindow(g_overlay);
            g_overlay = nullptr;
        }
    }

    g_border = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
                               L"KRecBorder", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
    if (g_border) {
        SetLayeredWindowAttributes(g_border, kKey, 255, LWA_COLORKEY);
        if (!SetWindowDisplayAffinity(g_border, WDA_EXCLUDEFROMCAPTURE) &&
            GetEnvironmentVariableW(L"KREC_DEBUG_KEEPBORDER", nullptr, 0) == 0) {
            DestroyWindow(g_border);
            g_border = nullptr;
        }
    }
    bool noLayer = GetEnvironmentVariableW(L"KREC_DEBUG_NOLAYER", nullptr, 0) > 0;  // developer test hook
    g_sel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | (noLayer ? 0 : WS_EX_LAYERED), L"KRecSelect", L"", WS_POPUP,
                            0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
    if (g_sel && !noLayer) SetLayeredWindowAttributes(g_sel, kKey, 110, LWA_COLORKEY | LWA_ALPHA);

    if (g_border && GetEnvironmentVariableW(L"KREC_DEBUG_BORDERTEST", nullptr, 0) > 0) {
        // Developer test hook: drag the right edge of the idle border 100 px to the right.
        HWND hb = g_border;
        std::thread([hb] {
            Sleep(3000);
            RECT wr;
            GetWindowRect(hb, &wr);
            int y = (wr.top + wr.bottom) / 2, x = wr.right - 2;
            SetCursorPos(x, y);
            PostMessageW(hb, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x - wr.left, y - wr.top));
            Sleep(500);
            SetCursorPos(x + 100, y);
            PostMessageW(hb, WM_MOUSEMOVE, MK_LBUTTON, 0);
            Sleep(800);
            PostMessageW(hb, WM_LBUTTONUP, 0, 0);
        }).detach();
    }
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);  // keep sound capture from being starved by the recorded program
    TrayAdd();
    RegisterHotkeys();

    UpdateUI();
    UpdateEncStatus();
    if (g_ffmpegMissing) {
        SetStatus(L"ffmpeg.exe가 없어요. 같은 폴더의 ffmpeg_받기.bat을 먼저 실행해 주세요.");
    } else {
        SetStatus(L"그래픽카드를 확인하는 중...");
        DetectEncoders();
    }

    RestoreWindowPos();
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);
    if (!g_ffmpegMissing) RecoverLeftovers(hwnd, S.saveDir);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_job) CloseHandle(g_job);
    CloseHandle(mtx);
    if (g_relaunchAdmin) RelaunchElevated();
    CoUninitialize();
    return 0;
}
