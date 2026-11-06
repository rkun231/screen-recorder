// 영상 편집기 (Editor.exe) — main window.
// Load videos → split → keep/discard each piece → save. Preview with live per-track volume.
#include "media.h"
#include "player.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#define APP_VER L"0.1.0"
#define APP_NAME L"영상 편집기"

// ------------------------------------------------------------------ ids & messages
enum {
    IDC_ADD = 100, IDC_CLEAR, IDC_TOPSTATUS, IDC_PREVIEW, IDC_LIST, IDC_UP, IDC_DOWN, IDC_MERGE, IDC_COPY,
    IDC_HDR_VOL, IDC_LBL_SYS, IDC_TB_SYS, IDC_LBL_MIC, IDC_TB_MIC, IDC_VOLNOTE,
    IDC_HDR_SEG, IDC_CHK_SEGVOL, IDC_LBL_SSYS, IDC_TB_SSYS, IDC_LBL_SMIC, IDC_TB_SMIC,
    IDC_PLAY, IDC_TIME, IDC_SPLIT, IDC_KEPTONLY, IDC_TLHINT, IDC_TIMELINE,
    IDC_LBL_SAVE, IDC_R_ONE, IDC_R_PER, IDC_R_FAST, IDC_R_PRECISE, IDC_CHK_TRACKS, IDC_SAVE, IDC_PROGRESS, IDC_STATUS,
};
enum {
    WM_APP_FRAME = WM_APP + 1, WM_APP_RUNEND, WM_APP_PROBED, WM_APP_EXPORT_PROG, WM_APP_EXPORT_DONE,
};
enum { TIMER_PLAY = 1, TIMER_TEST = 2 };

// ------------------------------------------------------------------ state
static HINSTANCE g_inst;
static HWND g_hwnd, g_list, g_preview, g_timeline, g_progress;
static HFONT g_font, g_fontBold, g_fontSmall;
static UINT g_dpi = 96;
static HBRUSH g_bgBrush;

static std::vector<MediaInfo> g_src;
static std::vector<Segment> g_seg;
static std::vector<double> g_seqStart;  // where each segment starts on the timeline (all segments, list order)
static Volumes g_global;
static CutMode g_cut = CutMode::Fast;
static SaveMode g_save = SaveMode::OneFile;
static bool g_keepTracks = true;
static bool g_keptOnly = false;

static double g_pos = 0;  // timeline seconds
static int g_sel = -1;
static bool g_playing = false;
static bool g_dragResume = false;
static bool g_dragging = false;
static std::unique_ptr<Player> g_player;

static std::vector<uint32_t> g_frame;
static int g_fw = 0, g_fh = 0;

// timeline view
static double g_tlPps = 0;   // pixels per second requested (0 = fit everything)
static double g_tlView = 0;  // time at the left edge

// list view flags
static bool g_listUpdating = false;
static bool g_allowEdit = false;
static bool g_editing = false;

// probing (adding files)
struct ProbeResult { bool ok; MediaInfo info; std::wstring err, path; };
static std::mutex g_probeMu;
static std::deque<std::wstring> g_probeQueue;
static bool g_probeRunning = false;
static int g_probePending = 0;
static std::vector<std::wstring> g_probeErrors;

// export
static std::atomic<bool> g_cancel{false};
static bool g_exporting = false;
static std::thread g_exportTh;
struct ExportResult { bool ok; std::wstring err; std::vector<std::wstring> written; bool cancelled; };

// test hooks (KEDIT_TEST=script file, KEDIT_TEST_LOG=log file)
static bool g_test = false;
static std::vector<std::wstring> g_testLines;
static size_t g_testIdx = 0;
static ULONGLONG g_testWaitUntil = 0;
static std::wstring g_testLog;
static std::wstring g_testSaveTo;

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }
static HWND Ctl(int id) { return GetDlgItem(g_hwnd, id); }
static void SetText(int id, const std::wstring& s) {
    wchar_t cur[512];
    GetWindowTextW(Ctl(id), cur, 512);
    if (s != cur) SetWindowTextW(Ctl(id), s.c_str());
}
static void TestLog(const std::wstring& s) {
    if (g_testLog.empty()) return;
    HANDLE h = CreateFileW(g_testLog.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string u = Narrow(s + L"\n");
    DWORD w;
    WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(h);
}
static void Status(const std::wstring& s) {
    SetText(IDC_STATUS, s);
    TestLog(L"[status] " + s);
}
static int Ask(const std::wstring& text, UINT flags, int testAnswer) {
    if (g_test) {
        TestLog(L"[ask] " + text + L" -> " + std::to_wstring(testAnswer));
        return testAnswer;
    }
    return MessageBoxW(g_hwnd, text.c_str(), APP_NAME, flags);
}

// ------------------------------------------------------------------ settings (UTF-16 INI)
static std::wstring IniPath() {
    std::wstring p = AppDataDir() + L"\\editor.ini";
    if (!FileExists(p)) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const wchar_t bom = 0xFEFF;
            DWORD w;
            WriteFile(h, &bom, 2, &w, nullptr);
            CloseHandle(h);
        }
    }
    return p;
}
static int IniInt(const wchar_t* k, int def) { return (int)GetPrivateProfileIntW(L"editor", k, def, IniPath().c_str()); }
static void IniSet(const wchar_t* k, int v) { WritePrivateProfileStringW(L"editor", k, std::to_wstring(v).c_str(), IniPath().c_str()); }
static void SaveOptions() {
    IniSet(L"precise", g_cut == CutMode::Precise);
    IniSet(L"perSegment", g_save == SaveMode::PerSegment);
    IniSet(L"keepTracks", g_keepTracks);
    IniSet(L"keptOnly", g_keptOnly);
}

// ------------------------------------------------------------------ sequence helpers
static double SegLen(int i) { return g_seg[i].end - g_seg[i].start; }
static double Total() { return g_seg.empty() ? 0 : g_seqStart.back() + SegLen((int)g_seg.size() - 1); }
static void RecalcSeq() {
    g_seqStart.resize(g_seg.size());
    double t = 0;
    for (size_t i = 0; i < g_seg.size(); i++) {
        g_seqStart[i] = t;
        t += SegLen((int)i);
    }
}
static int SegAt(double t) {
    for (size_t i = 0; i < g_seg.size(); i++)
        if (t < g_seqStart[i] + SegLen((int)i) - 1e-9) return (int)i;
    return (int)g_seg.size() - 1;
}
static bool AnyEditTracks() {
    for (auto& s : g_src) if (s.editTracks()) return true;
    return false;
}
static std::pair<float, float> SegVol(const Segment& s) {
    int sv = s.customVol ? s.sysVol : g_global.sys, mv = s.customVol ? s.micVol : g_global.mic;
    return {sv / 100.f, mv / 100.f};
}
static double CurPos() { return g_playing && g_player ? g_player->Position() : g_pos; }

static std::vector<PlayPiece> BuildPieces() {
    std::vector<PlayPiece> v;
    for (size_t i = 0; i < g_seg.size(); i++) {
        const Segment& s = g_seg[i];
        const MediaInfo& m = g_src[s.src];
        PlayPiece p;
        p.path = m.path;
        p.srcStart = s.start;
        p.srcEnd = s.end;
        p.seqStart = g_seqStart[i];
        p.audioTracks = m.audioTracks;
        p.fps = m.fps;
        p.width = m.width;
        p.height = m.height;
        auto vol = SegVol(s);
        p.sysVol = vol.first;
        p.micVol = vol.second;
        p.skip = g_keptOnly && !s.keep;
        v.push_back(p);
    }
    return v;
}
static void PushVolumes() {
    std::vector<std::pair<float, float>> v;
    for (auto& s : g_seg) v.push_back(SegVol(s));
    if (g_player) g_player->SetVolumes(v);
}

static std::wstring VolText(const Segment& s) {
    if (!s.customVol) return L"기본";
    if (g_src[s.src].editTracks())
        return L"컴 " + std::to_wstring(s.sysVol) + L"% · 마 " + std::to_wstring(s.micVol) + L"%";
    return L"소리 " + std::to_wstring(s.sysVol) + L"%";
}

// ------------------------------------------------------------------ UI refresh
static void UpdateTimeLabel() {
    SetText(IDC_TIME, PreciseTime(CurPos()) + L" / " + PreciseTime(Total()));
}
static void UpdatePlayButton() { SetText(IDC_PLAY, g_playing ? L"■ 정지" : L"▶ 재생"); }

static void UpdateTopStatus() {
    if (g_seg.empty()) {
        SetText(IDC_TOPSTATUS, g_probePending ? L"영상을 분석하는 중…" : L"영상 파일을 추가해 주세요.");
        return;
    }
    auto r = EffectiveRanges(g_src, g_seg, g_cut);
    double fin = 0;
    int kept = 0;
    for (size_t i = 0; i < g_seg.size(); i++)
        if (g_seg[i].keep) { fin += r[i].end - r[i].start; kept++; }
    std::wstring s = L"파일 " + std::to_wstring(g_src.size()) + L"개 · 구간 " + std::to_wstring(g_seg.size()) + L"개 (남김 " +
                     std::to_wstring(kept) + L") · 완성 길이 " + PreciseTime(fin);
    if (g_probePending) s += L"   (분석 중…)";
    SetText(IDC_TOPSTATUS, s);
}

static void SetRowTexts(int i, const std::vector<double>& outStart, const std::vector<EffRange>& r) {
    const Segment& s = g_seg[i];
    ListView_SetItemText(g_list, i, 0, (LPWSTR)s.name.c_str());
    std::wstring num = std::to_wstring(i + 1);
    std::wstring st = s.keep ? ChapterTime(outStart[i]) : L"—";
    std::wstring len = PreciseTime(s.keep ? r[i].end - r[i].start : s.length());
    std::wstring vol = VolText(s);
    ListView_SetItemText(g_list, i, 1, (LPWSTR)num.c_str());
    ListView_SetItemText(g_list, i, 2, (LPWSTR)st.c_str());
    ListView_SetItemText(g_list, i, 3, (LPWSTR)len.c_str());
    ListView_SetItemText(g_list, i, 4, (LPWSTR)vol.c_str());
}

static void UpdateListTexts() {
    auto r = EffectiveRanges(g_src, g_seg, g_cut);
    auto os = OutputStartTimes(g_src, g_seg, g_cut);
    g_listUpdating = true;
    for (int i = 0; i < (int)g_seg.size() && i < ListView_GetItemCount(g_list); i++) SetRowTexts(i, os, r);
    g_listUpdating = false;
}

static void SelectRow(int i) {
    g_listUpdating = true;
    int n = ListView_GetItemCount(g_list);
    for (int k = 0; k < n; k++) ListView_SetItemState(g_list, k, k == i ? (LVIS_SELECTED | LVIS_FOCUSED) : 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (i >= 0) ListView_EnsureVisible(g_list, i, FALSE);
    g_listUpdating = false;
    g_sel = i;
}

static void RefreshList() {
    g_listUpdating = true;
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    auto r = EffectiveRanges(g_src, g_seg, g_cut);
    auto os = OutputStartTimes(g_src, g_seg, g_cut);
    for (int i = 0; i < (int)g_seg.size(); i++) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (LPWSTR)g_seg[i].name.c_str();
        ListView_InsertItem(g_list, &it);
        ListView_SetCheckState(g_list, i, g_seg[i].keep);
        SetRowTexts(i, os, r);
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    g_listUpdating = false;
    if (g_sel >= (int)g_seg.size()) g_sel = (int)g_seg.size() - 1;
    SelectRow(g_sel);
    InvalidateRect(g_list, nullptr, TRUE);
}

static void SetTrack(int id, int v) { SendMessageW(Ctl(id), TBM_SETPOS, TRUE, v); }

static void UpdateVolPanel() {
    bool edit = AnyEditTracks();
    SetText(IDC_LBL_SYS, std::wstring(edit ? L"컴퓨터 소리  " : L"소리  ") + std::to_wstring(g_global.sys) + L"%");
    SetText(IDC_LBL_MIC, L"마이크  " + std::to_wstring(edit ? g_global.mic : 100) + L"%");
    SetTrack(IDC_TB_SYS, g_global.sys);
    SetTrack(IDC_TB_MIC, g_global.mic);
    bool any = !g_seg.empty();
    EnableWindow(Ctl(IDC_TB_SYS), any);
    EnableWindow(Ctl(IDC_LBL_SYS), any);
    EnableWindow(Ctl(IDC_TB_MIC), any && edit);
    EnableWindow(Ctl(IDC_LBL_MIC), any && edit);
    ShowWindow(Ctl(IDC_VOLNOTE), any && !edit ? SW_SHOWNA : SW_HIDE);

    bool has = g_sel >= 0 && g_sel < (int)g_seg.size();
    if (!has) {
        SetText(IDC_HDR_SEG, L"선택한 구간 없음");
        Button_SetCheck(Ctl(IDC_CHK_SEGVOL), BST_UNCHECKED);
        EnableWindow(Ctl(IDC_CHK_SEGVOL), FALSE);
        for (int id : {IDC_LBL_SSYS, IDC_TB_SSYS, IDC_LBL_SMIC, IDC_TB_SMIC}) EnableWindow(Ctl(id), FALSE);
        SetText(IDC_LBL_SSYS, L"컴퓨터 소리");
        SetText(IDC_LBL_SMIC, L"마이크");
    } else {
        const Segment& s = g_seg[g_sel];
        bool se = g_src[s.src].editTracks();
        SetText(IDC_HDR_SEG, L"선택한 구간 (" + std::to_wstring(g_sel + 1) + L"번)");
        EnableWindow(Ctl(IDC_CHK_SEGVOL), TRUE);
        Button_SetCheck(Ctl(IDC_CHK_SEGVOL), s.customVol ? BST_CHECKED : BST_UNCHECKED);
        int sv = s.customVol ? s.sysVol : g_global.sys, mv = s.customVol ? s.micVol : g_global.mic;
        SetText(IDC_LBL_SSYS, std::wstring(se ? L"컴퓨터 소리  " : L"소리  ") + std::to_wstring(sv) + L"%");
        SetText(IDC_LBL_SMIC, L"마이크  " + std::to_wstring(se ? mv : 100) + L"%");
        SetTrack(IDC_TB_SSYS, sv);
        SetTrack(IDC_TB_SMIC, mv);
        EnableWindow(Ctl(IDC_LBL_SSYS), s.customVol);
        EnableWindow(Ctl(IDC_TB_SSYS), s.customVol);
        EnableWindow(Ctl(IDC_LBL_SMIC), s.customVol && se);
        EnableWindow(Ctl(IDC_TB_SMIC), s.customVol && se);
    }
    EnableWindow(Ctl(IDC_CHK_TRACKS), edit);
}

static void UpdateButtons() {
    bool any = !g_seg.empty();
    bool has = g_sel >= 0 && g_sel < (int)g_seg.size();
    EnableWindow(Ctl(IDC_UP), has && g_sel > 0);
    EnableWindow(Ctl(IDC_DOWN), has && g_sel + 1 < (int)g_seg.size());
    bool canMerge = false;
    if (has && g_sel + 1 < (int)g_seg.size()) {
        const Segment &a = g_seg[g_sel], &b = g_seg[g_sel + 1];
        canMerge = a.src == b.src && std::fabs(a.end - b.start) < 1e-6;
    }
    EnableWindow(Ctl(IDC_MERGE), canMerge);
    EnableWindow(Ctl(IDC_COPY), any);
    EnableWindow(Ctl(IDC_PLAY), any);
    EnableWindow(Ctl(IDC_SPLIT), any);
    EnableWindow(Ctl(IDC_CLEAR), any);
    EnableWindow(Ctl(IDC_SAVE), any || g_exporting);
    SetText(IDC_SAVE, g_exporting ? L"취소" : L"저장하기");
    EnableWindow(Ctl(IDC_ADD), TRUE);
}

static void StartPlayAt(double t);

// Called after the segment list changed. structural = segments were added/removed/reordered.
static void Changed(bool structural) {
    RecalcSeq();
    if (structural) RefreshList();
    else UpdateListTexts();
    if (g_player) g_player->SetPieces(BuildPieces());
    UpdateTopStatus();
    UpdateVolPanel();
    UpdateButtons();
    UpdateTimeLabel();
    InvalidateRect(g_timeline, nullptr, FALSE);
    if (structural && g_playing) StartPlayAt(g_player->Position());
}

// ------------------------------------------------------------------ playback
static void ShowStillAt(double t) {
    if (g_seg.empty() || !g_player) return;
    g_player->ShowStill(SegAt(t), t);
}

static void FollowPlayhead();

static void StartPlayAt(double t) {
    if (g_seg.empty()) return;
    t = std::clamp(t, 0.0, Total());
    if (t >= Total() - 0.25) t = 0;  // at the very end: start over
    int i = SegAt(t);
    if (g_keptOnly && !g_seg[i].keep) {
        int j = -1;
        for (int k = i + 1; k < (int)g_seg.size(); k++) if (g_seg[k].keep) { j = k; break; }
        if (j < 0) for (int k = 0; k < (int)g_seg.size(); k++) if (g_seg[k].keep) { j = k; break; }
        if (j < 0) { Status(L"남긴 구간이 없어서 재생할 게 없어요."); return; }
        i = j;
        t = g_seqStart[j];
    }
    g_player->PlayFrom(i, t);
    g_playing = true;
    g_pos = t;
    SetTimer(g_hwnd, TIMER_PLAY, 30, nullptr);
    UpdatePlayButton();
    TestLog(L"[play] from " + Num(t, 3) + L" seg " + std::to_wstring(i + 1));
}

static void StopPlay() {
    if (!g_playing) return;
    g_pos = g_player->Position();
    g_player->Stop();
    g_playing = false;
    KillTimer(g_hwnd, TIMER_PLAY);
    UpdatePlayButton();
    UpdateTimeLabel();
    InvalidateRect(g_timeline, nullptr, FALSE);
    TestLog(L"[stop] at " + Num(g_pos, 3));
}

static void TogglePlay() {
    if (g_playing) StopPlay();
    else StartPlayAt(g_pos);
}

static void SeekTo(double t) {
    if (g_seg.empty()) return;
    t = std::clamp(t, 0.0, Total());
    g_pos = t;
    if (g_playing) {
        int i = SegAt(t);
        g_player->PlayFrom(i, t);
    } else {
        ShowStillAt(t);
    }
    FollowPlayhead();
    UpdateTimeLabel();
    InvalidateRect(g_timeline, nullptr, FALSE);
}

static void OnRunEnd(double t) {
    if (!g_playing || !g_player->RunDone()) return;  // stale message
    int next = -1;
    for (int i = 0; i < (int)g_seg.size(); i++) {
        if (g_seqStart[i] >= t - 0.005 && SegLen(i) > 0.005 && (!g_keptOnly || g_seg[i].keep)) { next = i; break; }
    }
    if (next < 0) {
        g_player->Stop();
        g_playing = false;
        KillTimer(g_hwnd, TIMER_PLAY);
        g_pos = std::min(t, Total());
        UpdatePlayButton();
        UpdateTimeLabel();
        InvalidateRect(g_timeline, nullptr, FALSE);
        TestLog(L"[ended] at " + Num(g_pos, 3));
        return;
    }
    TestLog(L"[runend] " + Num(t, 3) + L" -> seg " + std::to_wstring(next + 1));
    g_player->PlayFrom(next, g_seqStart[next]);
}

// ------------------------------------------------------------------ editing actions
static void DoSplit() {
    if (g_seg.empty()) return;
    double t = CurPos();
    int i = SegAt(t);
    double off = t - g_seqStart[i];
    if (off < 0.05 || SegLen(i) - off < 0.05) {
        Status(L"구간 경계와 너무 가까워서 나누지 않았어요.");
        return;
    }
    Segment b = g_seg[i];
    b.name.clear();
    g_seg[i].end = g_seg[i].start + off;
    b.start = g_seg[i].end;
    g_seg.insert(g_seg.begin() + i + 1, b);
    g_sel = i + 1;
    g_pos = t;
    Changed(true);
    Status(std::to_wstring(i + 1) + L"번 구간을 " + PreciseTime(t) + L" 에서 둘로 나눴어요.");
}

static void DoMerge() {
    int i = g_sel;
    if (i < 0 || i + 1 >= (int)g_seg.size()) return;
    Segment &a = g_seg[i], &b = g_seg[i + 1];
    if (a.src != b.src || std::fabs(a.end - b.start) > 1e-6) {
        Status(L"같은 영상에서 바로 이어지는 구간끼리만 합칠 수 있어요.");
        return;
    }
    a.end = b.end;  // the name stays the first piece's own (never filled in automatically)
    g_seg.erase(g_seg.begin() + i + 1);
    Changed(true);
    Status(std::to_wstring(i + 1) + L"번과 " + std::to_wstring(i + 2) + L"번 구간을 합쳤어요.");
}

static void DoMove(int dir) {
    int i = g_sel, j = i + dir;
    if (i < 0 || j < 0 || j >= (int)g_seg.size()) return;
    std::swap(g_seg[i], g_seg[j]);
    g_sel = j;
    RecalcSeq();
    g_pos = g_seqStart[j];
    Changed(true);
    if (!g_playing) ShowStillAt(g_pos);
}

static void DoClear() {
    if (g_seg.empty()) return;
    if (Ask(L"넣은 영상과 나눈 구간을 모두 지울까요?", MB_YESNO | MB_ICONQUESTION, IDYES) != IDYES) return;
    StopPlay();
    g_src.clear();
    g_seg.clear();
    g_sel = -1;
    g_pos = 0;
    g_tlPps = 0;
    g_tlView = 0;
    g_frame.clear();
    g_fw = g_fh = 0;
    InvalidateRect(g_preview, nullptr, FALSE);
    Changed(true);
    Status(L"모두 지웠어요.");
}

static std::wstring ChapterText(int* lines) {
    auto os = OutputStartTimes(g_src, g_seg, g_cut);
    std::wstring s;
    int n = 0;
    for (size_t i = 0; i < g_seg.size(); i++) {
        if (!g_seg[i].keep) continue;
        if (n) s += L"\r\n";
        s += ChapterTime(os[i]);
        if (!g_seg[i].name.empty()) s += L" " + g_seg[i].name;
        n++;
    }
    if (lines) *lines = n;
    return s;
}

static void DoCopy() {
    int n = 0;
    std::wstring s = ChapterText(&n);
    if (n == 0) { Status(L"남긴 구간이 없어서 복사할 게 없어요."); return; }
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        size_t bytes = (s.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            memcpy(GlobalLock(h), s.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        CloseClipboard();
    }
    TestLog(L"[copy]\n" + s + L"\n[/copy]");
    Status(std::to_wstring(n) + L"줄 복사했어요. 유튜브 설명란에 붙여 넣으면 돼요.");
}

// ------------------------------------------------------------------ adding files
static void ProbeWorker() {
    while (true) {
        std::wstring path;
        {
            std::lock_guard<std::mutex> lk(g_probeMu);
            if (g_probeQueue.empty()) { g_probeRunning = false; return; }
            path = g_probeQueue.front();
            g_probeQueue.pop_front();
        }
        auto* r = new ProbeResult;
        r->path = path;
        r->ok = ProbeMedia(path, r->info, r->err);
        if (!PostMessageW(g_hwnd, WM_APP_PROBED, 0, (LPARAM)r)) delete r;
    }
}

static void AddFiles(const std::vector<std::wstring>& files) {
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_probeMu);
        for (auto& f : files) {
            DWORD a = GetFileAttributesW(f.c_str());
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) continue;
            g_probeQueue.push_back(f);
            n++;
        }
        g_probePending += n;
        if (n && !g_probeRunning) {
            g_probeRunning = true;
            std::thread(ProbeWorker).detach();
        }
    }
    if (n) {
        Status(L"영상을 분석하는 중이에요… (" + std::to_wstring(n) + L"개)");
        UpdateTopStatus();
    }
}

static void OnProbed(ProbeResult* r) {
    g_probePending--;
    if (r->ok) {
        bool first = g_seg.empty();
        g_src.push_back(r->info);
        Segment s;
        s.src = (int)g_src.size() - 1;
        s.start = 0;
        s.end = r->info.duration;
        g_seg.push_back(s);
        if (first) { g_sel = 0; g_pos = 0; }
        g_tlPps = 0;  // fit the new length
        Changed(true);
        if (first && !g_playing) ShowStillAt(0);
        TestLog(L"[added] " + FileNameOf(r->path) + L" dur=" + Num(r->info.duration, 3) + L" tracks=" +
                std::to_wstring(r->info.audioTracks) + L" keyframes=" + std::to_wstring(r->info.keyframes.size()) + L" " +
                std::to_wstring(r->info.width) + L"x" + std::to_wstring(r->info.height) + L" fps=" + Num(r->info.fps, 2));
    } else {
        g_probeErrors.push_back(FileNameOf(r->path) + L": " + r->err);
        TestLog(L"[add failed] " + r->path + L" " + r->err);
    }
    delete r;
    if (g_probePending == 0) {
        if (!g_probeErrors.empty()) {
            std::wstring m = L"이 파일은 넣지 못했어요.\n\n";
            for (auto& e : g_probeErrors) m += e + L"\n";
            g_probeErrors.clear();
            Status(L"일부 파일을 넣지 못했어요.");
            Ask(m, MB_OK | MB_ICONWARNING, IDOK);
        } else {
            Status(L"영상을 넣었어요. 재생 위치에서 [여기서 나누기 (S)]로 구간을 나눌 수 있어요.");
        }
    }
    UpdateTopStatus();
}

static void OnAddButton() {
    std::vector<wchar_t> buf(65536, 0);
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = g_hwnd;
    of.lpstrFilter = L"영상 파일 (mp4, mkv, mov …)\0*.mp4;*.mkv;*.mov;*.avi;*.webm;*.flv;*.ts;*.m4v;*.wmv\0모든 파일\0*.*\0";
    of.lpstrFile = buf.data();
    of.nMaxFile = (DWORD)buf.size();
    of.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    std::wstring init = g_src.empty() ? L"" : DirOf(g_src.back().path);
    if (!init.empty()) of.lpstrInitialDir = init.c_str();
    if (!GetOpenFileNameW(&of)) return;
    std::vector<std::wstring> files;
    std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) files.push_back(first);
    else
        while (*p) {
            files.push_back(first + L"\\" + p);
            p += wcslen(p) + 1;
        }
    std::sort(files.begin(), files.end());
    AddFiles(files);
}

// ------------------------------------------------------------------ saving
static void OnSaveButton() {
    if (g_exporting) {
        g_cancel = true;
        Status(L"취소하는 중…");
        return;
    }
    if (g_seg.empty()) return;
    bool anyKept = false;
    for (auto& s : g_seg) anyKept |= s.keep;
    if (!anyKept) { Status(L"남긴 구간이 없어요. 구간을 하나 이상 체크해 주세요."); return; }
    StopPlay();
    if (g_cut == CutMode::Fast && g_save == SaveMode::OneFile) {  // separate files never need to match
        std::wstring why;
        if (!CanUseFast(g_src, g_seg, why)) {
            if (Ask(why + L"\n고속 모드로는 합칠 수 없어요. 정밀 모드로 바꿔서 저장할까요?\n(다시 인코딩해서 시간이 좀 더 걸려요)",
                    MB_YESNO | MB_ICONQUESTION, IDYES) != IDYES)
                return;
            g_cut = CutMode::Precise;
            CheckRadioButton(g_hwnd, IDC_R_FAST, IDC_R_PRECISE, IDC_R_PRECISE);
            SaveOptions();
            Changed(false);
        }
    }
    const MediaInfo& first = g_src[g_seg[0].src];
    std::wstring out;
    if (g_test) {
        out = g_testSaveTo;
    } else {
        std::vector<wchar_t> buf(4096, 0);
        std::wstring def = StemOf(first.path) + L"_편집.mp4";
        wcsncpy(buf.data(), def.c_str(), buf.size() - 1);
        std::wstring dir = DirOf(first.path);
        OPENFILENAMEW of{sizeof(of)};
        of.hwndOwner = g_hwnd;
        of.lpstrFilter = L"MP4 영상\0*.mp4\0";
        of.lpstrFile = buf.data();
        of.nMaxFile = (DWORD)buf.size();
        of.lpstrInitialDir = dir.c_str();
        of.lpstrDefExt = L"mp4";
        of.lpstrTitle = g_save == SaveMode::PerSegment ? L"저장할 이름 (구간마다 _01, _02 … 가 붙어요)" : L"저장할 이름";
        of.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&of)) return;
        out = buf.data();
    }
    ExportJob job;
    job.sources = g_src;
    job.segments = g_seg;
    job.global = g_global;
    job.cut = g_cut;
    job.save = g_save;
    job.keepTracks = g_keepTracks;
    job.outPath = out;
    job.quality = 1;
    if (FileExists(out) && g_save == SaveMode::OneFile) DeleteFileW(out.c_str());  // the dialog already asked
    g_cancel = false;
    g_exporting = true;
    UpdateButtons();
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    Status(g_cut == CutMode::Fast ? L"저장하는 중… (고속)" : L"저장하는 중… (정밀: 다시 인코딩해서 시간이 걸려요)");
    if (g_exportTh.joinable()) g_exportTh.join();
    HWND hwnd = g_hwnd;
    g_exportTh = std::thread([job, hwnd] {
        std::atomic<int> last{-1};
        auto* r = new ExportResult;
        r->ok = RunExport(
            job,
            [&](double p) {
                int v = (int)(p * 1000);
                if (v != last.exchange(v)) PostMessageW(hwnd, WM_APP_EXPORT_PROG, v, 0);
            },
            g_cancel, r->written, r->err);
        r->cancelled = g_cancel;
        PostMessageW(hwnd, WM_APP_EXPORT_DONE, 0, (LPARAM)r);
    });
}

static void OnExportDone(ExportResult* r) {
    if (g_exportTh.joinable()) g_exportTh.join();
    g_exporting = false;
    UpdateButtons();
    if (r->ok) {
        SendMessageW(g_progress, PBM_SETPOS, 1000, 0);
        std::wstring names;
        for (auto& w : r->written) names += L"\n" + FileNameOf(w);
        for (auto& w : r->written) TestLog(L"[written] " + w);
        Status(r->written.size() == 1 ? L"저장했어요: " + FileNameOf(r->written[0])
                                      : std::to_wstring(r->written.size()) + L"개 파일로 저장했어요.");
        if (Ask(L"저장했어요." + names + L"\n\n폴더를 열까요?", MB_YESNO | MB_ICONINFORMATION, IDNO) == IDYES) {
            std::wstring arg = L"/select,\"" + r->written[0] + L"\"";
            ShellExecuteW(g_hwnd, nullptr, L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        }
    } else if (r->cancelled) {
        SendMessageW(g_progress, PBM_SETPOS, 0, 0);
        Status(L"저장을 취소했어요.");
    } else {
        SendMessageW(g_progress, PBM_SETPOS, 0, 0);
        Status(L"저장하지 못했어요.");
        TestLog(L"[export error] " + r->err);
        Ask(r->err + L"\n\n자세한 기록: %APPDATA%\\녹화프로그램\\editor_ffmpeg.log", MB_OK | MB_ICONERROR, IDOK);
    }
    delete r;
}

// ------------------------------------------------------------------ timeline window
static const int kRuler = 20, kBottomPad = 6;
static int TlWidth() { RECT r; GetClientRect(g_timeline, &r); return r.right; }
static int TlMargin() { return S(8); }
static double TlPps() {
    double total = std::max(0.001, Total());
    double fit = (TlWidth() - 2 * TlMargin()) / total;
    return std::max(fit, g_tlPps);
}
static double TlVisible() { return (TlWidth() - 2 * TlMargin()) / TlPps(); }
static void ClampView() {
    double maxView = std::max(0.0, Total() - TlVisible());
    g_tlView = std::clamp(g_tlView, 0.0, maxView);
}
static int TlX(double t) { return TlMargin() + (int)std::lround((t - g_tlView) * TlPps()); }
static double TlTime(int x) { return g_tlView + (x - TlMargin()) / TlPps(); }

static void FollowPlayhead() {
    if (g_seg.empty()) return;
    ClampView();
    double t = CurPos(), vis = TlVisible();
    if (t < g_tlView || t > g_tlView + vis * 0.95) {
        g_tlView = t - vis * 0.1;
        ClampView();
    }
}

static COLORREF Mix(COLORREF a, COLORREF b, double k) {
    return RGB((int)(GetRValue(a) * (1 - k) + GetRValue(b) * k), (int)(GetGValue(a) * (1 - k) + GetGValue(b) * k),
               (int)(GetBValue(a) * (1 - k) + GetBValue(b) * k));
}

static void PaintTimeline(HDC dc, int W, int H) {
    RECT all{0, 0, W, H};
    HBRUSH bg = CreateSolidBrush(RGB(246, 247, 249));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_fontSmall);
    if (g_seg.empty()) {
        SetTextColor(dc, RGB(130, 130, 130));
        DrawTextW(dc, L"영상을 넣으면 여기에 타임라인이 보여요", -1, &all, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    ClampView();
    double pps = TlPps();
    int ruler = S(kRuler);
    int top = ruler + S(2), bot = H - S(kBottomPad);

    // ruler ticks
    static const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200};
    double step = steps[0];
    for (double s : steps) { step = s; if (s * pps >= S(70)) break; }
    HPEN tickPen = CreatePen(PS_SOLID, 1, RGB(170, 174, 182));
    HGDIOBJ oldPen = SelectObject(dc, tickPen);
    SetTextColor(dc, RGB(90, 94, 102));
    double t0 = std::floor(g_tlView / step) * step;
    for (double t = t0; t <= g_tlView + TlVisible() + step; t += step) {
        int x = TlX(t);
        if (x < -50 || x > W + 50) continue;
        MoveToEx(dc, x, ruler - S(6), nullptr);
        LineTo(dc, x, ruler);
        int x2 = TlX(t + step / 2);
        MoveToEx(dc, x2, ruler - S(3), nullptr);
        LineTo(dc, x2, ruler);
        std::wstring lab = step < 1 ? PreciseTime(t) : ChapterTime(t);
        RECT tr{x + S(3), 0, x + S(120), ruler - S(4)};
        DrawTextW(dc, lab.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
    MoveToEx(dc, 0, ruler, nullptr);
    LineTo(dc, W, ruler);

    // segments
    static const COLORREF pal[] = {RGB(74, 144, 217), RGB(76, 175, 120), RGB(156, 110, 210), RGB(230, 150, 60), RGB(60, 170, 180)};
    auto eff = EffectiveRanges(g_src, g_seg, g_cut);
    for (int i = 0; i < (int)g_seg.size(); i++) {
        const Segment& s = g_seg[i];
        int x0 = TlX(g_seqStart[i]), x1 = TlX(g_seqStart[i] + SegLen(i));
        if (x1 < 0 || x0 > W) continue;
        RECT r{x0, top, std::max(x0 + 1, x1), bot};
        COLORREF c = s.keep ? pal[s.src % 5] : RGB(200, 202, 206);
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &r, b);
        DeleteObject(b);
        if (!s.keep) {
            HBRUSH hb = CreateHatchBrush(HS_BDIAGONAL, RGB(150, 152, 158));
            SetBkColor(dc, c);
            SetBkMode(dc, TRANSPARENT);
            FillRect(dc, &r, hb);
            DeleteObject(hb);
        }
        // keyframe ticks (fast mode)
        if (g_cut == CutMode::Fast) {
            const MediaInfo& m = g_src[s.src];
            HPEN kp = CreatePen(PS_SOLID, 1, s.keep ? Mix(c, RGB(0, 0, 0), 0.45) : RGB(120, 120, 120));
            SelectObject(dc, kp);
            int lastX = -1000;
            auto it = std::lower_bound(m.keyframes.begin(), m.keyframes.end(), s.start - 1e-6);
            for (; it != m.keyframes.end() && *it < s.end; ++it) {
                int x = TlX(g_seqStart[i] + (*it - s.start));
                if (x < 0 || x > W || x - lastX < S(3)) continue;
                lastX = x;
                MoveToEx(dc, x, bot - S(6), nullptr);
                LineTo(dc, x, bot);
            }
            SelectObject(dc, tickPen);
            DeleteObject(kp);
        }
        // label
        if (x1 - x0 > S(18)) {
            std::wstring lab = std::to_wstring(i + 1);
            if (!s.name.empty()) lab += L"  " + s.name;
            RECT tr{std::max(x0, 0) + S(4), top + S(3), std::min(x1, W) - S(3), bot - S(8)};
            SetTextColor(dc, s.keep ? RGB(255, 255, 255) : RGB(80, 82, 88));
            SelectObject(dc, g_fontBold);
            DrawTextW(dc, lab.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (!s.keep && tr.bottom - tr.top > S(30)) {
                RECT t2 = tr;
                t2.top += S(18);
                SelectObject(dc, g_fontSmall);
                DrawTextW(dc, L"버림", -1, &t2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            }
            SelectObject(dc, g_fontSmall);
        }
        // boundary
        HPEN bp = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        SelectObject(dc, bp);
        MoveToEx(dc, x0, top, nullptr);
        LineTo(dc, x0, bot);
        SelectObject(dc, tickPen);
        DeleteObject(bp);
    }
    // selected outline
    if (g_sel >= 0 && g_sel < (int)g_seg.size()) {
        int x0 = TlX(g_seqStart[g_sel]), x1 = TlX(g_seqStart[g_sel] + SegLen(g_sel));
        HPEN sp = CreatePen(PS_SOLID, S(2), RGB(30, 34, 40));
        SelectObject(dc, sp);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, x0 + 1, top + 1, std::max(x0 + 3, x1), bot);
        SelectObject(dc, tickPen);
        DeleteObject(sp);
    }
    // fast mode: where the cut really happens (orange)
    if (g_cut == CutMode::Fast) {
        HPEN op = CreatePen(PS_SOLID, S(2), RGB(255, 140, 0));
        HBRUSH ob = CreateSolidBrush(RGB(255, 140, 0));
        SelectObject(dc, op);
        for (int i = 0; i < (int)g_seg.size(); i++) {
            double d = g_seg[i].start - eff[i].start;
            if (d < 1e-3) continue;
            bool relevant = g_seg[i].keep || (i > 0 && g_seg[i - 1].keep);
            if (!relevant) continue;
            int x = TlX(g_seqStart[i] - d), xb = TlX(g_seqStart[i]);
            if (xb < -20 || x > W + 20) continue;
            MoveToEx(dc, x, top, nullptr);
            LineTo(dc, x, bot);
            RECT band{x, bot - S(4), std::max(x + 1, xb), bot};
            FillRect(dc, &band, ob);
            POINT tri[3] = {{x - S(5), top}, {x + S(5), top}, {x, top + S(7)}};
            SelectObject(dc, ob);
            Polygon(dc, tri, 3);
        }
        SelectObject(dc, tickPen);
        DeleteObject(op);
        DeleteObject(ob);
    }
    // playhead
    {
        int x = TlX(CurPos());
        HPEN pp = CreatePen(PS_SOLID, S(2), RGB(220, 40, 40));
        HBRUSH pb = CreateSolidBrush(RGB(220, 40, 40));
        SelectObject(dc, pp);
        MoveToEx(dc, x, S(2), nullptr);
        LineTo(dc, x, H);
        SelectObject(dc, pb);
        POINT tri[3] = {{x - S(6), 0}, {x + S(6), 0}, {x, S(8)}};
        Polygon(dc, tri, 3);
        SelectObject(dc, tickPen);
        DeleteObject(pp);
        DeleteObject(pb);
    }
    SelectObject(dc, oldPen);
    DeleteObject(tickPen);
}

static void TimelineSeekFromMouse(int x) {
    double t = std::clamp(TlTime(x), 0.0, Total());
    int i = SegAt(t);
    if (i != g_sel) {
        SelectRow(i);
        UpdateVolPanel();
        UpdateButtons();
    }
    SeekTo(t);
}

static LRESULT CALLBACK TimelineProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT r;
            GetClientRect(h, &r);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
            HGDIOBJ ob = SelectObject(mem, bmp);
            PaintTimeline(mem, r.right, r.bottom);
            BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (g_seg.empty()) return 0;
            SetCapture(h);
            g_dragging = true;
            g_dragResume = g_playing;
            if (g_playing) StopPlay();
            TimelineSeekFromMouse(GET_X_LPARAM(l));
            return 0;
        case WM_MOUSEMOVE:
            if (g_dragging && GetCapture() == h) TimelineSeekFromMouse(GET_X_LPARAM(l));
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (g_dragging) {
                g_dragging = false;
                if (m == WM_LBUTTONUP) ReleaseCapture();
                if (g_dragResume) StartPlayAt(g_pos);
                g_dragResume = false;
            }
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            if (g_seg.empty()) return 0;
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(h, &pt);
            if (m == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(w) & MK_CONTROL)) {
                double tAt = TlTime(pt.x);
                double pps = TlPps() * std::pow(1.25, delta / 120.0);
                double fit = (TlWidth() - 2 * TlMargin()) / std::max(0.001, Total());
                g_tlPps = std::clamp(pps, fit, 400.0);
                if (g_tlPps <= fit * 1.0001) g_tlPps = 0;
                g_tlView = tAt - (pt.x - TlMargin()) / TlPps();
            } else {
                double d = (m == WM_MOUSEHWHEEL ? delta : -delta) / 120.0 * TlVisible() * 0.1;
                g_tlView += d;
            }
            ClampView();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------------ preview window
static LRESULT CALLBACK PreviewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT r;
            GetClientRect(h, &r);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
            HGDIOBJ ob = SelectObject(mem, bmp);
            FillRect(mem, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
            if (!g_frame.empty() && g_fw > 0 && g_fh > 0 && !g_seg.empty()) {
                double k = std::min((double)r.right / g_fw, (double)r.bottom / g_fh);
                int dw = (int)(g_fw * k), dh = (int)(g_fh * k);
                int dx = (r.right - dw) / 2, dy = (r.bottom - dh) / 2;
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = g_fw;
                bi.bmiHeader.biHeight = -g_fh;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(mem, HALFTONE);
                SetBrushOrgEx(mem, 0, 0, nullptr);
                StretchDIBits(mem, dx, dy, dw, dh, 0, 0, g_fw, g_fh, g_frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            } else {
                SetBkMode(mem, TRANSPARENT);
                SetTextColor(mem, RGB(200, 200, 200));
                SelectObject(mem, g_font);
                std::wstring t = g_probePending ? L"영상을 분석하는 중이에요…"
                                                : L"여기에 영상 파일을 끌어다 놓거나\n위의 [파일 추가]를 눌러 주세요.\n\n여러 개를 넣으면 순서대로 이어 붙여져요.";
                RECT tr = r;
                RECT calc = r;
                DrawTextW(mem, t.c_str(), -1, &calc, DT_CENTER | DT_CALCRECT | DT_WORDBREAK);
                tr.top = (r.bottom - (calc.bottom - calc.top)) / 2;
                DrawTextW(mem, t.c_str(), -1, &tr, DT_CENTER | DT_WORDBREAK);
            }
            BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (!g_seg.empty()) TogglePlay();
            return 0;
        case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------------ main window: controls & layout
static HWND MakeCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
}

static void MakeFonts() {
    if (g_font) DeleteObject(g_font);
    if (g_fontBold) DeleteObject(g_fontBold);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    auto mk = [](int pt, int weight) {
        return CreateFontW(-MulDiv(pt, (int)g_dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"맑은 고딕");
    };
    g_font = mk(9, FW_NORMAL);
    g_fontBold = mk(9, FW_BOLD);
    g_fontSmall = mk(8, FW_NORMAL);
}
static BOOL CALLBACK SetFontProc(HWND h, LPARAM) {
    int id = GetDlgCtrlID(h);
    HFONT f = (id == IDC_HDR_VOL || id == IDC_HDR_SEG) ? g_fontBold : (id == IDC_VOLNOTE || id == IDC_TLHINT) ? g_fontSmall : g_font;
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    return TRUE;
}

static void SetupListColumns() {
    struct { const wchar_t* t; int w; int fmt; } cols[] = {
        {L"이름 (더블클릭해서 입력)", 170, LVCFMT_LEFT}, {L"번호", 40, LVCFMT_RIGHT}, {L"시작", 56, LVCFMT_RIGHT},
        {L"길이", 72, LVCFMT_RIGHT}, {L"소리", 104, LVCFMT_LEFT}};
    while (ListView_DeleteColumn(g_list, 0)) {}
    for (int i = 0; i < 5; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.pszText = (LPWSTR)cols[i].t;
        c.cx = S(cols[i].w);
        c.fmt = cols[i].fmt;
        ListView_InsertColumn(g_list, i, &c);
    }
}

static void CreateControls() {
    MakeCtl(L"BUTTON", L"파일 추가", BS_PUSHBUTTON, IDC_ADD);
    MakeCtl(L"BUTTON", L"모두 지우기", BS_PUSHBUTTON, IDC_CLEAR);
    MakeCtl(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_TOPSTATUS);
    g_preview = MakeCtl(L"KEditPreview", L"", 0, IDC_PREVIEW);

    g_list = MakeCtl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_EDITLABELS | WS_TABSTOP, IDC_LIST,
                     WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SetupListColumns();
    MakeCtl(L"BUTTON", L"▲", BS_PUSHBUTTON, IDC_UP);
    MakeCtl(L"BUTTON", L"▼", BS_PUSHBUTTON, IDC_DOWN);
    MakeCtl(L"BUTTON", L"다음 구간과 합치기", BS_PUSHBUTTON, IDC_MERGE);
    MakeCtl(L"BUTTON", L"구간 목록 복사", BS_PUSHBUTTON, IDC_COPY);

    MakeCtl(L"STATIC", L"소리 크기 (전체 기본값)", SS_LEFT | SS_NOPREFIX, IDC_HDR_VOL);
    MakeCtl(L"STATIC", L"컴퓨터 소리", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, IDC_LBL_SYS);
    MakeCtl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | TBS_BOTH | WS_TABSTOP, IDC_TB_SYS);
    MakeCtl(L"STATIC", L"마이크", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, IDC_LBL_MIC);
    MakeCtl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | TBS_BOTH | WS_TABSTOP, IDC_TB_MIC);
    MakeCtl(L"STATIC", L"편집용 트랙이 없는 영상이라 소리 전체 크기만 조절할 수 있어요.", SS_LEFT | SS_NOPREFIX, IDC_VOLNOTE);
    MakeCtl(L"STATIC", L"선택한 구간", SS_LEFT | SS_NOPREFIX, IDC_HDR_SEG);
    MakeCtl(L"BUTTON", L"이 구간만 따로 조절", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CHK_SEGVOL);
    MakeCtl(L"STATIC", L"컴퓨터 소리", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, IDC_LBL_SSYS);
    MakeCtl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | TBS_BOTH | WS_TABSTOP, IDC_TB_SSYS);
    MakeCtl(L"STATIC", L"마이크", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, IDC_LBL_SMIC);
    MakeCtl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | TBS_BOTH | WS_TABSTOP, IDC_TB_SMIC);
    for (int id : {IDC_TB_SYS, IDC_TB_MIC, IDC_TB_SSYS, IDC_TB_SMIC}) {
        SendMessageW(Ctl(id), TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
        SendMessageW(Ctl(id), TBM_SETPAGESIZE, 0, 10);
        SendMessageW(Ctl(id), TBM_SETPOS, TRUE, 100);
    }

    MakeCtl(L"BUTTON", L"▶ 재생", BS_PUSHBUTTON, IDC_PLAY);
    MakeCtl(L"STATIC", L"0:00.00 / 0:00.00", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, IDC_TIME);
    MakeCtl(L"BUTTON", L"여기서 나누기 (S)", BS_PUSHBUTTON, IDC_SPLIT);
    MakeCtl(L"BUTTON", L"남긴 구간만 재생", BS_AUTOCHECKBOX, IDC_KEPTONLY);
    MakeCtl(L"STATIC", L"Space 재생 · 방향키 1초 · Shift+방향키 1프레임 · Ctrl+휠 확대", SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX, IDC_TLHINT);
    g_timeline = MakeCtl(L"KEditTimeline", L"", 0, IDC_TIMELINE, WS_EX_CLIENTEDGE);

    MakeCtl(L"STATIC", L"저장:", SS_LEFT | SS_CENTERIMAGE, IDC_LBL_SAVE);
    MakeCtl(L"BUTTON", L"하나의 파일로", BS_AUTORADIOBUTTON | WS_GROUP, IDC_R_ONE);
    MakeCtl(L"BUTTON", L"구간마다 따로", BS_AUTORADIOBUTTON, IDC_R_PER);
    MakeCtl(L"BUTTON", L"고속 (화질 그대로)", BS_AUTORADIOBUTTON | WS_GROUP, IDC_R_FAST);
    MakeCtl(L"BUTTON", L"정밀 (정확한 위치)", BS_AUTORADIOBUTTON, IDC_R_PRECISE);
    MakeCtl(L"BUTTON", L"편집용 트랙 유지", BS_AUTOCHECKBOX | WS_GROUP, IDC_CHK_TRACKS);
    MakeCtl(L"BUTTON", L"저장하기", BS_PUSHBUTTON, IDC_SAVE);
    g_progress = MakeCtl(PROGRESS_CLASSW, L"", 0, IDC_PROGRESS);
    SendMessageW(g_progress, PBM_SETRANGE32, 0, 1000);
    MakeCtl(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_STATUS);

    CheckRadioButton(g_hwnd, IDC_R_ONE, IDC_R_PER, g_save == SaveMode::OneFile ? IDC_R_ONE : IDC_R_PER);
    CheckRadioButton(g_hwnd, IDC_R_FAST, IDC_R_PRECISE, g_cut == CutMode::Fast ? IDC_R_FAST : IDC_R_PRECISE);
    Button_SetCheck(Ctl(IDC_CHK_TRACKS), g_keepTracks ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(Ctl(IDC_KEPTONLY), g_keptOnly ? BST_CHECKED : BST_UNCHECKED);
}

static void Place(int id, int x, int y, int w, int h) {
    HWND c = Ctl(id);
    if (c) MoveWindow(c, x, y, std::max(0, w), std::max(0, h), TRUE);
}

static void Layout() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(8), g = S(6), bh = S(28);
    // top
    Place(IDC_ADD, m, m, S(96), bh);
    Place(IDC_CLEAR, m + S(96) + g, m, S(96), bh);
    Place(IDC_TOPSTATUS, m + 2 * (S(96) + g) + S(6), m, W - (m + 2 * (S(96) + g) + S(6)) - m, bh);
    int y0 = m + bh + g;
    // bottom rows (from the bottom up)
    int yStatus = H - m - bh;
    int ySave = yStatus - g - S(26);
    int tlH = S(104);
    int yTl = ySave - g - tlH;
    int yCtl = yTl - g - bh;
    // status row
    Place(IDC_SAVE, m, yStatus, S(110), bh);
    Place(IDC_PROGRESS, m + S(110) + g, yStatus + S(5), S(220), bh - S(10));
    Place(IDC_STATUS, m + S(110) + g + S(220) + S(10), yStatus, W - (m + S(110) + g + S(220) + S(10)) - m, bh);
    // save options
    int x = m;
    Place(IDC_LBL_SAVE, x, ySave, S(40), S(26)); x += S(40);
    Place(IDC_R_ONE, x, ySave, S(120), S(26)); x += S(120);
    Place(IDC_R_PER, x, ySave, S(130), S(26)); x += S(130) + S(16);
    Place(IDC_R_FAST, x, ySave, S(150), S(26)); x += S(150);
    Place(IDC_R_PRECISE, x, ySave, S(160), S(26)); x += S(160) + S(16);
    Place(IDC_CHK_TRACKS, x, ySave, S(150), S(26));
    // timeline
    Place(IDC_TIMELINE, m, yTl, W - 2 * m, tlH);
    // playback controls
    x = m;
    Place(IDC_PLAY, x, yCtl, S(90), bh); x += S(90) + g;
    Place(IDC_TIME, x, yCtl, S(170), bh); x += S(170) + g;
    Place(IDC_SPLIT, x, yCtl, S(140), bh); x += S(140) + S(14);
    Place(IDC_KEPTONLY, x, yCtl, S(150), bh); x += S(150) + g;
    Place(IDC_TLHINT, x, yCtl, W - m - x, bh);
    // middle: preview | panel
    int panelW = S(430);
    int midH = yCtl - g - y0;
    int px = W - m - panelW;
    Place(IDC_PREVIEW, m, y0, px - g - m, midH);
    int rowH = S(26);
    int volH = rowH * 7 + S(8) + S(18);
    int btnY = y0 + midH - volH - g - bh;
    int listH = btnY - g - y0;
    Place(IDC_LIST, px, y0, panelW, listH);
    {   // the name column takes whatever the other columns leave
        RECT lr;
        GetClientRect(g_list, &lr);
        int others = 0;
        for (int c = 1; c < 5; c++) others += ListView_GetColumnWidth(g_list, c);
        int vs = GetSystemMetricsForDpi(SM_CXVSCROLL, g_dpi);
        ListView_SetColumnWidth(g_list, 0, std::max(S(80), (int)lr.right - others - vs - S(2)));
    }
    x = px;
    Place(IDC_UP, x, btnY, S(36), bh); x += S(36) + S(4);
    Place(IDC_DOWN, x, btnY, S(36), bh); x += S(36) + g;
    Place(IDC_MERGE, x, btnY, S(150), bh); x += S(150) + g;
    Place(IDC_COPY, x, btnY, px + panelW - x, bh);
    int y = btnY + bh + S(10);
    int lw = S(140);
    Place(IDC_HDR_VOL, px, y, panelW, rowH - S(4)); y += rowH - S(2);
    Place(IDC_LBL_SYS, px + S(8), y, lw, rowH); Place(IDC_TB_SYS, px + S(8) + lw, y, panelW - lw - S(8), rowH); y += rowH;
    Place(IDC_LBL_MIC, px + S(8), y, lw, rowH); Place(IDC_TB_MIC, px + S(8) + lw, y, panelW - lw - S(8), rowH); y += rowH;
    Place(IDC_VOLNOTE, px + S(8), y, panelW - S(8), S(18)); y += S(18) + S(6);
    Place(IDC_HDR_SEG, px, y, S(170), rowH - S(4));
    Place(IDC_CHK_SEGVOL, px + S(170), y - S(3), panelW - S(170), rowH); y += rowH - S(2);
    Place(IDC_LBL_SSYS, px + S(8), y, lw, rowH); Place(IDC_TB_SSYS, px + S(8) + lw, y, panelW - lw - S(8), rowH); y += rowH;
    Place(IDC_LBL_SMIC, px + S(8), y, lw, rowH); Place(IDC_TB_SMIC, px + S(8) + lw, y, panelW - lw - S(8), rowH);
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

// ------------------------------------------------------------------ keyboard
static bool HandleKey(WPARAM vk) {
    bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    if (ctrl) return false;
    switch (vk) {
        case VK_SPACE: if (!g_seg.empty()) TogglePlay(); return true;
        case 'S': if (!g_seg.empty()) DoSplit(); return true;
        case VK_HOME: SeekTo(0); return true;
        case VK_END:
            if (g_playing) StopPlay();
            SeekTo(Total());
            return true;
        case VK_LEFT:
        case VK_RIGHT: {
            if (g_seg.empty()) return true;
            double dir = vk == VK_LEFT ? -1 : 1;
            if (shift) {
                if (g_playing) StopPlay();
                double fps = g_src[g_seg[SegAt(g_pos)].src].fps;
                SeekTo(g_pos + dir / std::max(1.0, fps));
            } else {
                SeekTo(CurPos() + dir);
            }
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ volume sliders
static void OnSlider(HWND tb, int code) {
    int id = GetDlgCtrlID(tb);
    int v = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
    if (id == IDC_TB_SYS) g_global.sys = v;
    else if (id == IDC_TB_MIC) g_global.mic = v;
    else if ((id == IDC_TB_SSYS || id == IDC_TB_SMIC) && g_sel >= 0 && g_sel < (int)g_seg.size() && g_seg[g_sel].customVol) {
        (id == IDC_TB_SSYS ? g_seg[g_sel].sysVol : g_seg[g_sel].micVol) = v;
    } else
        return;
    PushVolumes();
    UpdateVolPanel();
    if (code != TB_THUMBTRACK) {
        if (g_player) g_player->SetPieces(BuildPieces());
        UpdateListTexts();
    }
}

static void OnSegVolCheck() {
    if (g_sel < 0 || g_sel >= (int)g_seg.size()) return;
    Segment& s = g_seg[g_sel];
    bool on = Button_GetCheck(Ctl(IDC_CHK_SEGVOL)) == BST_CHECKED;
    if (on && !s.customVol) {
        s.sysVol = g_global.sys;
        s.micVol = g_global.mic;
    }
    s.customVol = on;
    PushVolumes();
    Changed(false);
}

// ------------------------------------------------------------------ list notifications
static LRESULT OnListNotify(NMHDR* nh) {
    switch (nh->code) {
        case LVN_ITEMCHANGED: {
            auto* nv = (NMLISTVIEW*)nh;
            if (g_listUpdating || nv->iItem < 0 || nv->iItem >= (int)g_seg.size() || !(nv->uChanged & LVIF_STATE)) return 0;
            if ((nv->uNewState ^ nv->uOldState) & LVIS_STATEIMAGEMASK) {
                bool keep = ListView_GetCheckState(g_list, nv->iItem) != 0;
                if (keep != g_seg[nv->iItem].keep) {
                    g_seg[nv->iItem].keep = keep;
                    Changed(false);
                    if (g_playing && g_keptOnly) StartPlayAt(g_player->Position());
                    Status(std::to_wstring(nv->iItem + 1) + (keep ? L"번 구간을 남겨요." : L"번 구간을 버려요."));
                }
            }
            if ((nv->uNewState & LVIS_SELECTED) && !(nv->uOldState & LVIS_SELECTED)) {
                g_sel = nv->iItem;
                UpdateVolPanel();
                UpdateButtons();
                InvalidateRect(g_timeline, nullptr, FALSE);
            }
            return 0;
        }
        case NM_CLICK: {
            auto* ia = (NMITEMACTIVATE*)nh;
            if (ia->iItem < 0 || ia->iItem >= (int)g_seg.size()) return 0;
            LVHITTESTINFO hi{};
            hi.pt = ia->ptAction;
            ListView_SubItemHitTest(g_list, &hi);
            if (hi.flags & LVHT_ONITEMSTATEICON) return 0;  // checkbox click: don't jump
            SeekTo(g_seqStart[ia->iItem]);
            return 0;
        }
        case NM_DBLCLK: {
            auto* ia = (NMITEMACTIVATE*)nh;
            if (ia->iItem < 0 || ia->iItem >= (int)g_seg.size()) return 0;
            LVHITTESTINFO hi{};
            hi.pt = ia->ptAction;
            ListView_SubItemHitTest(g_list, &hi);
            if (hi.flags & LVHT_ONITEMSTATEICON) return 0;
            g_allowEdit = true;
            SetFocus(g_list);
            ListView_EditLabel(g_list, ia->iItem);
            g_allowEdit = false;
            return 0;
        }
        case LVN_BEGINLABELEDITW: {
            if (!g_allowEdit) return TRUE;  // only a double-click starts editing
            g_editing = true;
            HWND ed = ListView_GetEditControl(g_list);
            if (ed) SendMessageW(ed, EM_LIMITTEXT, 100, 0);
            return FALSE;
        }
        case LVN_ENDLABELEDITW: {
            auto* di = (NMLVDISPINFOW*)nh;
            g_editing = false;
            int i = di->item.iItem;
            if (di->item.pszText && i >= 0 && i < (int)g_seg.size()) {
                std::wstring t = di->item.pszText;
                while (!t.empty() && iswspace(t.back())) t.pop_back();
                while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
                g_seg[i].name = t;
                Changed(false);
                InvalidateRect(g_timeline, nullptr, FALSE);
                return FALSE;  // we set the text ourselves
            }
            return FALSE;
        }
    }
    return 0;
}

// ------------------------------------------------------------------ test script
static std::vector<std::wstring> SplitWords(const std::wstring& s, int maxParts) {
    std::vector<std::wstring> v;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == L' ') i++;
        if (i >= s.size()) break;
        if ((int)v.size() == maxParts - 1) { v.push_back(s.substr(i)); break; }
        size_t j = s.find(L' ', i);
        if (j == std::wstring::npos) j = s.size();
        v.push_back(s.substr(i, j - i));
        i = j;
    }
    return v;
}

static void DumpState() {
    auto r = EffectiveRanges(g_src, g_seg, g_cut);
    auto os = OutputStartTimes(g_src, g_seg, g_cut);
    TestLog(L"[dump] pos=" + Num(CurPos(), 3) + L" sel=" + std::to_wstring(g_sel + 1) + L" playing=" + std::to_wstring(g_playing) +
            L" cut=" + (g_cut == CutMode::Fast ? L"fast" : L"precise"));
    for (size_t i = 0; i < g_seg.size(); i++) {
        auto& s = g_seg[i];
        wchar_t lv[256] = L"";
        ListView_GetItemText(g_list, (int)i, 2, lv, 256);
        std::wstring lvs = lv;
        ListView_GetItemText(g_list, (int)i, 4, lv, 256);
        TestLog(L"  #" + std::to_wstring(i + 1) + L" src=" + std::to_wstring(s.src) + L" " + Num(s.start, 3) + L"-" + Num(s.end, 3) +
                L" eff=" + Num(r[i].start, 3) + L"-" + Num(r[i].end, 3) + L" keep=" + std::to_wstring(s.keep) + L" out=" + Num(os[i], 3) +
                L" name='" + s.name + L"' vol=" + VolText(s) + L" | list start='" + lvs + L"' vol='" + lv + L"' checked=" +
                std::to_wstring(ListView_GetCheckState(g_list, (int)i)));
    }
    wchar_t t[512];
    GetWindowTextW(Ctl(IDC_TOPSTATUS), t, 512);
    TestLog(std::wstring(L"  top: ") + t);
    GetWindowTextW(Ctl(IDC_TIME), t, 512);
    TestLog(std::wstring(L"  time: ") + t);
}

static void TestStep() {
    if (GetTickCount64() < g_testWaitUntil) return;
    if (g_probePending > 0 || g_exporting) return;
    if (g_testIdx >= g_testLines.size()) return;
    std::wstring line = g_testLines[g_testIdx++];
    if (line.empty() || line[0] == L'#') return;
    TestLog(L"> " + line);
    auto a = SplitWords(line, 4);
    const std::wstring& c = a[0];
    auto arg = [&](size_t i) { return i < a.size() ? a[i] : std::wstring(); };
    auto num = [&](size_t i) { return _wtof(arg(i).c_str()); };
    if (c == L"add") AddFiles({SplitWords(line, 2)[1]});
    else if (c == L"wait") g_testWaitUntil = GetTickCount64() + (ULONGLONG)num(1);
    else if (c == L"seek") SeekTo(num(1));
    else if (c == L"split") DoSplit();
    else if (c == L"keep") { ListView_SetCheckState(g_list, (int)num(1) - 1, num(2) != 0); }
    else if (c == L"name") {
        int i = (int)num(1) - 1;
        auto p = SplitWords(line, 3);
        if (i >= 0 && i < (int)g_seg.size()) { g_seg[i].name = p.size() > 2 ? p[2] : L""; Changed(false); }
    } else if (c == L"select") {
        int i = (int)num(1) - 1;
        ListView_SetItemState(g_list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    } else if (c == L"vol") {
        SetTrack(IDC_TB_SYS, (int)num(1)); OnSlider(Ctl(IDC_TB_SYS), TB_ENDTRACK);
        SetTrack(IDC_TB_MIC, (int)num(2)); OnSlider(Ctl(IDC_TB_MIC), TB_ENDTRACK);
    } else if (c == L"segvol") {
        Button_SetCheck(Ctl(IDC_CHK_SEGVOL), num(1) != 0 ? BST_CHECKED : BST_UNCHECKED);
        OnSegVolCheck();
        if (num(1) != 0) {
            SetTrack(IDC_TB_SSYS, (int)num(2)); OnSlider(Ctl(IDC_TB_SSYS), TB_ENDTRACK);
            SetTrack(IDC_TB_SMIC, (int)num(3)); OnSlider(Ctl(IDC_TB_SMIC), TB_ENDTRACK);
        }
    } else if (c == L"cut") SendMessageW(Ctl(arg(1) == L"fast" ? IDC_R_FAST : IDC_R_PRECISE), BM_CLICK, 0, 0);
    else if (c == L"savemode") SendMessageW(Ctl(arg(1) == L"one" ? IDC_R_ONE : IDC_R_PER), BM_CLICK, 0, 0);
    else if (c == L"tracks") { Button_SetCheck(Ctl(IDC_CHK_TRACKS), num(1) != 0); SendMessageW(g_hwnd, WM_COMMAND, MAKEWPARAM(IDC_CHK_TRACKS, BN_CLICKED), (LPARAM)Ctl(IDC_CHK_TRACKS)); }
    else if (c == L"keptonly") { Button_SetCheck(Ctl(IDC_KEPTONLY), num(1) != 0); SendMessageW(g_hwnd, WM_COMMAND, MAKEWPARAM(IDC_KEPTONLY, BN_CLICKED), (LPARAM)Ctl(IDC_KEPTONLY)); }
    else if (c == L"up") DoMove(-1);
    else if (c == L"down") DoMove(1);
    else if (c == L"merge") DoMerge();
    else if (c == L"save") {
        g_testSaveTo = SplitWords(line, 2)[1];
        SendMessageW(g_hwnd, WM_COMMAND, MAKEWPARAM(IDC_SAVE, BN_CLICKED), (LPARAM)Ctl(IDC_SAVE));
    } else if (c == L"copy") {
        DoCopy();
        if (OpenClipboard(g_hwnd)) {
            HANDLE h = GetClipboardData(CF_UNICODETEXT);
            if (h) { TestLog(L"[clipboard]\n" + std::wstring((wchar_t*)GlobalLock(h)) + L"\n[/clipboard]"); GlobalUnlock(h); }
            CloseClipboard();
        }
    } else if (c == L"play") StartPlayAt(g_pos);
    else if (c == L"stop") StopPlay();
    else if (c == L"key") { HandleKey((WPARAM)num(1)); }
    else if (c == L"dump") DumpState();
    else if (c == L"frame") TestLog(L"[frame] " + std::to_wstring(g_fw) + L"x" + std::to_wstring(g_fh) + L" px0=" +
                                    (g_frame.empty() ? std::wstring(L"none") : std::to_wstring(g_frame[g_frame.size() / 2] & 0xFFFFFF)));
    else if (c == L"clear") DoClear();
    else if (c == L"quit") PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    else TestLog(L"[unknown command]");
}

// ------------------------------------------------------------------ window procedure
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            g_hwnd = h;
            g_dpi = GetDpiForWindow(h);
            MakeFonts();
            CreateControls();
            EnumChildWindows(h, SetFontProc, 0);
            g_player = std::make_unique<Player>(h, WM_APP_FRAME, WM_APP_RUNEND);
            DragAcceptFiles(h, TRUE);
            // The recorder runs as administrator, so the editor may too: let Explorer's drag & drop through.
            ChangeWindowMessageFilterEx(h, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
            ChangeWindowMessageFilterEx(h, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
            ChangeWindowMessageFilterEx(h, 0x0049 /*WM_COPYGLOBALDATA*/, MSGFLT_ALLOW, nullptr);
            Changed(true);
            Status(L"영상을 넣고 → 나누고 → 남길 구간을 체크하고 → 저장하면 돼요. 구간 이름은 목록에서 더블클릭해서 입력해요.");
            return 0;
        }
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mi = (MINMAXINFO*)l;
            mi->ptMinTrackSize = {S(1000), S(640)};
            return 0;
        }
        case WM_DPICHANGED: {
            g_dpi = HIWORD(w);
            MakeFonts();
            EnumChildWindows(h, SetFontProc, 0);
            SetupListColumns();
            RefreshList();
            RECT* r = (RECT*)l;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)w;
            int id = GetDlgCtrlID((HWND)l);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            if (id == IDC_VOLNOTE || id == IDC_TLHINT) SetTextColor(dc, RGB(120, 120, 120));
            else if (id == IDC_HDR_VOL || id == IDC_HDR_SEG) SetTextColor(dc, RGB(40, 60, 110));
            return (LRESULT)g_bgBrush;
        }
        case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            switch (id) {
                case IDC_ADD: OnAddButton(); break;
                case IDC_CLEAR: DoClear(); break;
                case IDC_UP: DoMove(-1); break;
                case IDC_DOWN: DoMove(1); break;
                case IDC_MERGE: DoMerge(); break;
                case IDC_COPY: DoCopy(); break;
                case IDC_PLAY: TogglePlay(); break;
                case IDC_SPLIT: DoSplit(); break;
                case IDC_SAVE: OnSaveButton(); break;
                case IDC_CHK_SEGVOL: if (code == BN_CLICKED) OnSegVolCheck(); break;
                case IDC_KEPTONLY:
                    g_keptOnly = Button_GetCheck(Ctl(IDC_KEPTONLY)) == BST_CHECKED;
                    SaveOptions();
                    if (g_player) g_player->SetPieces(BuildPieces());
                    if (g_playing) StartPlayAt(g_player->Position());
                    break;
                case IDC_R_ONE:
                case IDC_R_PER:
                    g_save = Button_GetCheck(Ctl(IDC_R_PER)) == BST_CHECKED ? SaveMode::PerSegment : SaveMode::OneFile;
                    SaveOptions();
                    break;
                case IDC_R_FAST:
                case IDC_R_PRECISE: {
                    CutMode c = Button_GetCheck(Ctl(IDC_R_PRECISE)) == BST_CHECKED ? CutMode::Precise : CutMode::Fast;
                    if (c != g_cut) {
                        g_cut = c;
                        SaveOptions();
                        Changed(false);
                        Status(c == CutMode::Fast ? L"고속: 화질 그대로 빠르게 저장해요. 실제로 잘리는 위치는 타임라인의 주황색 표시예요."
                                                  : L"정밀: 나눈 위치 그대로 잘라요. 다시 인코딩해서 저장이 조금 오래 걸려요.");
                    }
                    break;
                }
                case IDC_CHK_TRACKS:
                    g_keepTracks = Button_GetCheck(Ctl(IDC_CHK_TRACKS)) == BST_CHECKED;
                    SaveOptions();
                    break;
            }
            // keep keyboard shortcuts working after clicking a button
            if (code == BN_CLICKED && id != IDC_LIST) SetFocus(h);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)l;
            if (nh->idFrom == IDC_LIST) return OnListNotify(nh);
            return 0;
        }
        case WM_HSCROLL:
            if (l) OnSlider((HWND)l, LOWORD(w));
            return 0;
        case WM_MOUSEWHEEL: {
            // route the wheel to the timeline when the pointer is over it
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            RECT r;
            GetWindowRect(g_timeline, &r);
            if (PtInRect(&r, pt)) return SendMessageW(g_timeline, m, w, l);
            break;
        }
        case WM_DROPFILES: {
            HDROP d = (HDROP)w;
            UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> files;
            for (UINT i = 0; i < n; i++) {
                wchar_t p[MAX_PATH * 4];
                DragQueryFileW(d, i, p, MAX_PATH * 4);
                files.push_back(p);
            }
            DragFinish(d);
            std::sort(files.begin(), files.end());
            AddFiles(files);
            return 0;
        }
        case WM_TIMER:
            if (w == TIMER_PLAY && g_playing) {
                FollowPlayhead();
                UpdateTimeLabel();
                InvalidateRect(g_timeline, nullptr, FALSE);
            } else if (w == TIMER_TEST) {
                TestStep();
            }
            return 0;
        case WM_APP_FRAME:
            if (g_player && g_player->CopyFrame(g_frame, g_fw, g_fh)) InvalidateRect(g_preview, nullptr, FALSE);
            return 0;
        case WM_APP_RUNEND: {
            double* t = (double*)l;
            double v = t ? *t : 0;
            delete t;
            OnRunEnd(v);
            return 0;
        }
        case WM_APP_PROBED: OnProbed((ProbeResult*)l); return 0;
        case WM_APP_EXPORT_PROG:
            SendMessageW(g_progress, PBM_SETPOS, w, 0);
            if (g_exporting) SetText(IDC_STATUS, L"저장하는 중… " + std::to_wstring((int)w / 10) + L"%");
            return 0;
        case WM_APP_EXPORT_DONE: OnExportDone((ExportResult*)l); return 0;
        case WM_CLOSE: {
            if (g_exporting) {
                if (Ask(L"아직 저장하는 중이에요. 취소하고 닫을까요?", MB_YESNO | MB_ICONQUESTION, IDYES) != IDYES) return 0;
                g_cancel = true;
                if (g_exportTh.joinable()) g_exportTh.join();
            }
            WINDOWPLACEMENT wp{sizeof(wp)};
            GetWindowPlacement(h, &wp);
            IniSet(L"x", wp.rcNormalPosition.left);
            IniSet(L"y", wp.rcNormalPosition.top);
            IniSet(L"w", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
            IniSet(L"h", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
            IniSet(L"max", wp.showCmd == SW_SHOWMAXIMIZED);
            SaveOptions();
            DestroyWindow(h);
            return 0;
        }
        case WM_DESTROY:
            if (g_player) g_player->Stop();
            g_player.reset();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}


// ------------------------------------------------------------------ entry
static std::wstring ReadUtf8File(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    std::string s((size_t)sz.QuadPart, '\0');
    DWORD r = 0;
    if (!s.empty()) ReadFile(h, &s[0], (DWORD)s.size(), &r, nullptr);
    CloseHandle(h);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF) s.erase(0, 3);
    return Widen(s);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    g_bgBrush = GetSysColorBrush(COLOR_WINDOW);

    g_cut = IniInt(L"precise", 0) ? CutMode::Precise : CutMode::Fast;
    g_save = IniInt(L"perSegment", 0) ? SaveMode::PerSegment : SaveMode::OneFile;
    g_keepTracks = IniInt(L"keepTracks", 1) != 0;
    g_keptOnly = IniInt(L"keptOnly", 0) != 0;

    wchar_t env[2048];
    if (GetEnvironmentVariableW(L"KEDIT_TEST", env, 2048)) {
        g_test = true;
        std::wstring script = ReadUtf8File(env);
        size_t i = 0;
        while (i <= script.size()) {
            size_t j = script.find(L'\n', i);
            if (j == std::wstring::npos) j = script.size();
            std::wstring ln = script.substr(i, j - i);
            while (!ln.empty() && (ln.back() == L'\r' || ln.back() == L' ')) ln.pop_back();
            g_testLines.push_back(ln);
            i = j + 1;
        }
        if (GetEnvironmentVariableW(L"KEDIT_TEST_LOG", env, 2048)) g_testLog = env;
        // test runs start from defaults
        g_cut = CutMode::Fast;
        g_save = SaveMode::OneFile;
        g_keepTracks = true;
        g_keptOnly = false;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"KEditMain";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    RegisterClassExW(&wc);
    WNDCLASSEXW pc{sizeof(pc)};
    pc.lpfnWndProc = PreviewProc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    pc.lpszClassName = L"KEditPreview";
    RegisterClassExW(&pc);
    WNDCLASSEXW tc{sizeof(tc)};
    tc.lpfnWndProc = TimelineProc;
    tc.hInstance = inst;
    tc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    tc.lpszClassName = L"KEditTimeline";
    RegisterClassExW(&tc);

    // window position: saved, or ~1280x820 centered (scaled for the primary monitor's DPI)
    UINT sysDpi = GetDpiForSystem();
    int ww = MulDiv(1280, sysDpi, 96), wh = MulDiv(820, sysDpi, 96);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    ww = std::min<int>(ww, wa.right - wa.left);
    wh = std::min<int>(wh, wa.bottom - wa.top);
    int wx = wa.left + (wa.right - wa.left - ww) / 2, wy = wa.top + (wa.bottom - wa.top - wh) / 2;
    int sx = IniInt(L"x", INT_MIN), sy = IniInt(L"y", INT_MIN), sw = IniInt(L"w", 0), sh = IniInt(L"h", 0);
    if (!g_test && sx != INT_MIN && sw > 200 && sh > 200) {
        RECT r{sx, sy, sx + sw, sy + sh};
        HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
        if (mon) { wx = sx; wy = sy; ww = sw; wh = sh; }
    }
    std::wstring title = std::wstring(APP_NAME) + L" v" + APP_VER;
    HWND h = CreateWindowExW(0, L"KEditMain", title.c_str(), WS_OVERLAPPEDWINDOW, wx, wy, ww, wh, nullptr, nullptr, inst, nullptr);
    ShowWindow(h, (!g_test && IniInt(L"max", 0)) ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(h);

    // files given on the command line (e.g. from the recorder)
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> files;
    for (int i = 1; i < argc; i++) files.push_back(argv[i]);
    LocalFree(argv);
    if (!files.empty()) AddFiles(files);
    if (g_test) SetTimer(h, TIMER_TEST, 50, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && !g_editing && (msg.hwnd == h || IsChild(h, msg.hwnd))) {
            if (HandleKey(msg.wParam)) continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
