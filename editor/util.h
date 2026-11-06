#pragma once
// Small shared helpers for the editor.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <string>
#include <vector>
#include <cstdio>

inline std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring s(buf, n);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? s : s.substr(0, p);
}
inline bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
// ffmpeg.exe next to the editor, or one folder up (편집기프로그램 is inside the recorder's folder).
inline std::wstring FfmpegPath() {
    static std::wstring p = [] {
        std::wstring d = ExeDir();
        std::wstring a = d + L"\\ffmpeg.exe";
        if (FileExists(a)) return a;
        size_t i = d.find_last_of(L"\\/");
        if (i != std::wstring::npos) {
            std::wstring b = d.substr(0, i) + L"\\ffmpeg.exe";
            if (FileExists(b)) return b;
        }
        return a;
    }();
    return p;
}
inline std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
inline std::wstring FileNameOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}
inline std::wstring DirOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"." : p.substr(0, i);
}
inline std::wstring StemOf(const std::wstring& p) {
    std::wstring f = FileNameOf(p);
    size_t i = f.find_last_of(L'.');
    return i == std::wstring::npos ? f : f.substr(0, i);
}
inline std::wstring AppDataDir() {
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) d = p;
    if (p) CoTaskMemFree(p);
    d += L"\\녹화프로그램";
    SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
    return d;
}
// "8:42" under an hour, "1:30:24" from an hour (YouTube chapter style).
inline std::wstring ChapterTime(double sec) {
    long long s = (long long)(sec + 0.0001);
    wchar_t b[32];
    if (s >= 3600) swprintf(b, 32, L"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else swprintf(b, 32, L"%lld:%02lld", s / 60, s % 60);
    return b;
}
// "1:23.45" style with hundredths, for the editor's time display.
inline std::wstring PreciseTime(double sec) {
    if (sec < 0) sec = 0;
    long long cs = (long long)(sec * 100 + 0.5);
    long long s = cs / 100;
    wchar_t b[40];
    if (s >= 3600) swprintf(b, 40, L"%lld:%02lld:%02lld.%02lld", s / 3600, (s / 60) % 60, s % 60, cs % 100);
    else swprintf(b, 40, L"%lld:%02lld.%02lld", s / 60, s % 60, cs % 100);
    return b;
}
inline std::wstring Num(double v, int prec = 3) {
    wchar_t b[64];
    swprintf(b, 64, L"%.*f", prec, v);
    return b;
}
inline std::wstring Quote(const std::wstring& p) { return L"\"" + p + L"\""; }

// Child processes die with the editor.
HANDLE EditorJob();

struct ChildProc {
    HANDLE h = nullptr;
    HANDLE out = nullptr;  // read end of stdout (if requested)
    HANDLE in = nullptr;   // write end of stdin
};
// Starts a hidden process. If captureOut, stdout is a pipe you read from `out`; stderr goes to `logPath`.
bool StartChild(const std::wstring& cmd, const std::wstring& logPath, bool captureOut, ChildProc& p);
void CloseChild(ChildProc& p);
// Runs a command and returns its stderr+stdout text (for probing).
std::string RunCapture(const std::wstring& cmd, DWORD timeoutMs, int* exitCode = nullptr);
