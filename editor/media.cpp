#include "media.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

// ------------------------------------------------------------------ processes

HANDLE EditorJob() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (j) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof(li));
        }
        return j;
    }();
    return job;
}

bool StartChild(const std::wstring& cmd, const std::wstring& logPath, bool captureOut, ChildProc& p) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE hLog = INVALID_HANDLE_VALUE;
    if (!logPath.empty())
        hLog = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, 0, nullptr);
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    CreatePipe(&inR, &inW, &sa, 0);
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    if (captureOut) {
        CreatePipe(&outR, &outW, &sa, 1 << 20);
        SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = captureOut ? outW : (hLog != INVALID_HANDLE_VALUE ? hLog : nullptr);
    si.hStdError = hLog != INVALID_HANDLE_VALUE ? hLog : nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    std::wstring dir = ExeDir();
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             dir.c_str(), &si, &pi);
    CloseHandle(inR);
    if (outW) CloseHandle(outW);
    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
    if (!ok) {
        CloseHandle(inW);
        if (outR) CloseHandle(outR);
        return false;
    }
    if (EditorJob()) AssignProcessToJobObject(EditorJob(), pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    p.h = pi.hProcess;
    p.in = inW;
    p.out = outR;
    return true;
}

void CloseChild(ChildProc& p) {
    if (p.in) { CloseHandle(p.in); p.in = nullptr; }
    if (p.out) { CloseHandle(p.out); p.out = nullptr; }
    if (p.h) { CloseHandle(p.h); p.h = nullptr; }
}

std::string RunCapture(const std::wstring& cmd, DWORD timeoutMs, int* exitCode) {
    // stdout and stderr both into one pipe.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE r = nullptr, w = nullptr;
    CreatePipe(&r, &w, &sa, 1 << 20);
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = w;
    si.hStdError = w;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    std::string out;
    std::wstring dir = ExeDir();
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(r);
        CloseHandle(w);
        if (exitCode) *exitCode = -1;
        return out;
    }
    if (EditorJob()) AssignProcessToJobObject(EditorJob(), pi.hProcess);
    CloseHandle(w);
    ULONGLONG t0 = GetTickCount64();
    char tmp[65536];
    DWORD n;
    while (ReadFile(r, tmp, sizeof(tmp), &n, nullptr) && n > 0) {
        out.append(tmp, n);
        if (GetTickCount64() - t0 > timeoutMs) { TerminateProcess(pi.hProcess, 1); break; }
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exitCode) *exitCode = (int)code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(r);
    return out;
}

// ------------------------------------------------------------------ probing

double MediaInfo::KeyframeAtOrBefore(double t) const {
    if (keyframes.empty()) return t;
    auto it = std::upper_bound(keyframes.begin(), keyframes.end(), t + 1e-6);
    if (it == keyframes.begin()) return keyframes.front() <= t + 1e-6 ? keyframes.front() : 0.0;
    return *(it - 1);
}

bool ProbeMedia(const std::wstring& path, MediaInfo& m, std::wstring& err) {
    m = MediaInfo{};
    m.path = path;
    if (!FileExists(FfmpegPath())) { err = L"ffmpeg.exe를 찾지 못했어요. 녹화 프로그램 폴더에 ffmpeg.exe가 있어야 해요."; return false; }
    std::string info = RunCapture(Quote(FfmpegPath()) + L" -hide_banner -i " + Quote(path), 30000);
    // Duration: 00:00:24.88
    size_t d = info.find("Duration: ");
    if (d == std::string::npos) { err = L"영상 파일을 읽을 수 없어요."; return false; }
    int hh = 0, mm = 0;
    double ss = 0;
    if (sscanf(info.c_str() + d + 10, "%d:%d:%lf", &hh, &mm, &ss) == 3) m.duration = hh * 3600 + mm * 60 + ss;
    size_t pos = 0;
    bool haveVideo = false;
    while ((pos = info.find("Stream #", pos)) != std::string::npos) {
        size_t eol = info.find('\n', pos);
        std::string line = info.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? info.size() : eol;
        if (line.find(": Video: ") != std::string::npos && !haveVideo) {
            haveVideo = true;
            size_t v = line.find(": Video: ") + 9;
            size_t sp = line.find_first_of(" ,", v);
            m.vcodec = Widen(line.substr(v, sp - v));
            // first "WxH" made of digits
            for (size_t i = 0; i + 3 < line.size(); i++) {
                int w = 0, h = 0;
                if (isdigit((unsigned char)line[i]) && (i == 0 || !isdigit((unsigned char)line[i - 1])) &&
                    sscanf(line.c_str() + i, "%dx%d", &w, &h) == 2 && w > 15 && h > 15) {
                    m.width = w;
                    m.height = h;
                    break;
                }
            }
            size_t f = line.find(" fps");
            size_t tb = line.find(" tbr");
            size_t use = tb != std::string::npos ? tb : f;
            if (use != std::string::npos) {
                size_t b = line.rfind(' ', use - 1);
                double fps = atof(line.c_str() + b + 1);
                if (line.compare(use - 1, 1, "k") == 0) fps *= 1000;
                if (fps > 1 && fps < 1000) m.fps = fps;
            }
        } else if (line.find(": Audio: ") != std::string::npos) {
            m.audioTracks++;
        }
    }
    if (!haveVideo || m.duration <= 0) { err = L"영상이 들어 있는 파일이 아니에요."; return false; }

    // Keyframe positions (packet copy, no decoding: fast).
    std::string crc = RunCapture(Quote(FfmpegPath()) + L" -v error -i " + Quote(path) + L" -map 0:v:0 -c copy -f framecrc -", 120000);
    long long tbn = 1, tbd = 1;
    size_t t = crc.find("#tb 0: ");
    if (t != std::string::npos) sscanf(crc.c_str() + t + 7, "%lld/%lld", &tbn, &tbd);
    pos = 0;
    while (pos < crc.size()) {
        size_t eol = crc.find('\n', pos);
        if (eol == std::string::npos) eol = crc.size();
        if (crc[pos] == '0') {  // packet line of stream 0 ("#" lines are headers)
            const char* ln = crc.c_str() + pos;
            std::string line(ln, eol - pos);
            if (line.find("F=") == std::string::npos) {  // keyframes have no flags field
                long long dts = 0, pts = 0;
                int si = 0;
                if (sscanf(ln, "%d, %lld, %lld", &si, &dts, &pts) == 3 && tbd) m.keyframes.push_back((double)pts * tbn / tbd);
            }
        }
        pos = eol + 1;
    }
    std::sort(m.keyframes.begin(), m.keyframes.end());
    return true;
}

// ------------------------------------------------------------------ ranges & chapter times

std::vector<EffRange> EffectiveRanges(const std::vector<MediaInfo>& src, const std::vector<Segment>& seg, CutMode cut) {
    std::vector<EffRange> r(seg.size());
    for (size_t i = 0; i < seg.size(); i++) {
        r[i] = {seg[i].start, seg[i].end};
        if (cut == CutMode::Fast) r[i].start = src[seg[i].src].KeyframeAtOrBefore(seg[i].start);
    }
    if (cut == CutMode::Fast) {
        for (size_t i = 0; i + 1 < seg.size(); i++) {
            const Segment &a = seg[i], &b = seg[i + 1];
            if (a.src == b.src && std::fabs(a.end - b.start) < 1e-6) r[i].end = r[i + 1].start;
        }
        for (auto& x : r) x.end = std::max(x.end, x.start);
    }
    return r;
}

std::vector<double> OutputStartTimes(const std::vector<MediaInfo>& src, const std::vector<Segment>& seg, CutMode cut) {
    auto r = EffectiveRanges(src, seg, cut);
    std::vector<double> out(seg.size(), -1);
    double t = 0;
    for (size_t i = 0; i < seg.size(); i++) {
        if (!seg[i].keep) continue;
        out[i] = t;
        t += r[i].end - r[i].start;
    }
    return out;
}

bool CanUseFast(const std::vector<MediaInfo>& src, const std::vector<Segment>& seg, std::wstring& why) {
    const MediaInfo* first = nullptr;
    for (auto& s : seg) {
        if (!s.keep) continue;
        const MediaInfo& m = src[s.src];
        if (!first) { first = &m; continue; }
        if (m.vcodec != first->vcodec || m.width != first->width || m.height != first->height) {
            why = L"합치는 영상들의 해상도나 형식이 서로 달라요.";
            return false;
        }
        if (m.audioTracks != first->audioTracks) {
            why = L"합치는 영상들의 소리 트랙 수가 서로 달라요.";
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------ export

namespace {

double Vol(int pct) { return std::max(0, pct) / 100.0; }

void SegVolumes(const ExportJob& job, const Segment& s, double& sv, double& mv) {
    sv = Vol(s.customVol ? s.sysVol : job.global.sys);
    mv = Vol(s.customVol ? s.micVol : job.global.mic);
}

// How many audio tracks the output gets: 0, 1 (mix) or 3 (mix, computer, mic).
int OutputTracks(const ExportJob& job, const std::vector<const Segment*>& kept) {
    bool anyAudio = false, anyEdit = false;
    for (auto* s : kept) {
        anyAudio |= job.sources[s->src].audioTracks > 0;
        anyEdit |= job.sources[s->src].editTracks();
    }
    if (!anyAudio) return 0;
    return (anyEdit && job.keepTracks) ? 3 : 1;
}

// Audio filter chain for input #in (already trimmed), producing labels [a<k>] (+ [s<k>] [m<k>] when tracks == 3).
std::wstring AudioChain(const ExportJob& job, const Segment& s, int in, int k, int tracks, double dur) {
    const MediaInfo& m = job.sources[s.src];
    double sv, mv;
    SegVolumes(job, s, sv, mv);
    std::wstring K = std::to_wstring(k), I = std::to_wstring(in);
    const std::wstring fmt = L"aformat=sample_rates=48000:channel_layouts=stereo";
    std::wstring f;
    if (m.audioTracks == 0) {
        std::wstring sil = L"anullsrc=r=48000:cl=stereo,atrim=duration=" + Num(dur, 6);
        f += sil + L"[a" + K + L"];";
        if (tracks == 3) f += sil + L"[s" + K + L"];" + sil + L"[m" + K + L"];";
    } else if (m.editTracks()) {
        f += L"[" + I + L":a:1]asetpts=PTS-STARTPTS," + fmt + L",volume=" + Num(sv) + L"[sx" + K + L"];";
        f += L"[" + I + L":a:2]asetpts=PTS-STARTPTS," + fmt + L",volume=" + Num(mv) + L"[mx" + K + L"];";
        if (tracks == 3) {
            f += L"[sx" + K + L"]asplit=2[sa" + K + L"][s" + K + L"];";
            f += L"[mx" + K + L"]asplit=2[ma" + K + L"][m" + K + L"];";
            f += L"[sa" + K + L"][ma" + K + L"]amix=inputs=2:normalize=0:duration=longest[a" + K + L"];";
        } else {
            f += L"[sx" + K + L"][mx" + K + L"]amix=inputs=2:normalize=0:duration=longest[a" + K + L"];";
        }
    } else {
        f += L"[" + I + L":a:0]asetpts=PTS-STARTPTS," + fmt + L",volume=" + Num(sv);
        if (tracks == 3) {
            f += L",asplit=2[a" + K + L"][s" + K + L"];";
            f += L"anullsrc=r=48000:cl=stereo,atrim=duration=" + Num(dur, 6) + L"[m" + K + L"];";
        } else {
            f += L"[a" + K + L"];";
        }
    }
    return f;
}

std::wstring TrackMeta(int tracks) {
    if (tracks != 3) return L"";
    return L" -metadata:s:a:0 title=\"전체\" -metadata:s:a:1 title=\"컴퓨터 소리\" -metadata:s:a:2 title=\"마이크\""
           L" -metadata:s:a:0 handler_name=\"전체\" -metadata:s:a:1 handler_name=\"컴퓨터 소리\" -metadata:s:a:2 handler_name=\"마이크\""
           L" -disposition:a:0 default -disposition:a:1 0 -disposition:a:2 0";
}

// Runs ffmpeg with "-progress pipe:1"; reports seconds done through onTime.
bool RunWithProgress(const std::wstring& cmd, std::function<void(double)> onTime, std::atomic<bool>& cancel, std::wstring& err) {
    std::wstring log = AppDataDir() + L"\\editor_ffmpeg.log";
    {
        HANDLE h = CreateFileW((AppDataDir() + L"\\editor_last_command.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            std::string c = Narrow(cmd);
            DWORD w;
            WriteFile(h, c.data(), (DWORD)c.size(), &w, nullptr);
            CloseHandle(h);
        }
    }
    ChildProc p;
    if (!StartChild(cmd, log, true, p)) { err = L"ffmpeg.exe를 실행할 수 없어요."; return false; }
    std::string acc;
    char buf[4096];
    DWORD n;
    while (true) {
        DWORD avail = 0;
        if (!PeekNamedPipe(p.out, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0) {
            if (cancel) { TerminateProcess(p.h, 1); break; }
            if (WaitForSingleObject(p.h, 50) == WAIT_OBJECT_0) {
                // drain the rest
                while (PeekNamedPipe(p.out, nullptr, 0, nullptr, &avail, nullptr) && avail && ReadFile(p.out, buf, sizeof(buf), &n, nullptr) && n)
                    acc.append(buf, n);
                break;
            }
            continue;
        }
        if (!ReadFile(p.out, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        acc.append(buf, n);
        size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            if (line.compare(0, 12, "out_time_us=") == 0 || line.compare(0, 12, "out_time_ms=") == 0) {
                long long us = atoll(line.c_str() + 12);
                if (us > 0 && onTime) onTime(us / 1e6);
            }
        }
    }
    WaitForSingleObject(p.h, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(p.h, &code);
    CloseChild(p);
    if (cancel) { err = L"취소했어요."; return false; }
    if (code != 0) {
        // last lines of the log
        HANDLE h = CreateFileW(log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        std::string t;
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            GetFileSizeEx(h, &sz);
            LARGE_INTEGER off;
            off.QuadPart = std::max<long long>(0, sz.QuadPart - 1500);
            SetFilePointerEx(h, off, nullptr, FILE_BEGIN);
            t.resize((size_t)(sz.QuadPart - off.QuadPart));
            DWORD rd = 0;
            if (!t.empty()) ReadFile(h, &t[0], (DWORD)t.size(), &rd, nullptr);
            t.resize(rd);
            CloseHandle(h);
        }
        err = L"저장하지 못했어요.\n" + Widen(t);
        return false;
    }
    return true;
}

std::wstring SafeName(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) r += (wcschr(L"\\/:*?\"<>|", c) || c < 32) ? L'_' : c;
    while (!r.empty() && (r.back() == L' ' || r.back() == L'.')) r.pop_back();
    return r.substr(0, 60);
}

bool NvencWorks() {
    static int ok = -1;
    if (ok < 0) {
        int code = 1;
        RunCapture(Quote(FfmpegPath()) + L" -hide_banner -loglevel error -f lavfi -i color=c=black:s=640x360:r=30 -frames:v 5 -c:v h264_nvenc -f null -",
                   20000, &code);
        ok = code == 0 ? 1 : 0;
    }
    return ok == 1;
}

std::wstring VideoEncoder(int quality) {
    static const int nvq[3] = {27, 22, 18}, x264q[3] = {26, 21, 18};
    int q = std::clamp(quality, 0, 2);
    if (NvencWorks())
        return L" -c:v h264_nvenc -preset p5 -tune hq -rc vbr -cq " + std::to_wstring(nvq[q]) + L" -b:v 0 -profile:v high -pix_fmt yuv420p";
    return L" -c:v libx264 -preset veryfast -crf " + std::to_wstring(x264q[q]) + L" -pix_fmt yuv420p";
}

// Fast mode, one segment → one file (video copied, audio copied or re-made for volumes).
std::wstring FastSegmentCmd(const ExportJob& job, const Segment& s, const EffRange& r, int tracks, const std::wstring& out) {
    const MediaInfo& m = job.sources[s.src];
    double sv, mv;
    SegVolumes(job, s, sv, mv);
    std::wstring c = Quote(FfmpegPath()) + L" -hide_banner -y -v error -progress pipe:1 -nostats";
    // With stream copy FFmpeg starts at the keyframe before the requested time; asking for a moment just
    // after our keyframe makes sure it picks that keyframe (asking exactly for it can land one keyframe earlier).
    double seek = r.start > 0 ? r.start + 1.5 / std::max(1.0, m.fps) : 0.0;
    c += L" -ss " + Num(seek, 6) + L" -to " + Num(r.end, 6) + L" -i " + Quote(m.path);
    c += L" -map 0:v:0 -c:v copy";
    bool untouched = std::fabs(sv - 1) < 1e-9 && (!m.editTracks() || std::fabs(mv - 1) < 1e-9);
    if (tracks > 0 && m.audioTracks > 0) {
        if (untouched && ((tracks == 3 && m.editTracks()) || (tracks == 1 && !m.editTracks()))) {
            c += L" -map 0:a -c:a copy";  // nothing to change: keep the original sound as is
        } else if (untouched && tracks == 1 && m.editTracks()) {
            c += L" -map 0:a:0 -c:a copy";  // the recorder's mix track
        } else {
            c += L" -filter_complex \"" + AudioChain(job, s, 0, 0, tracks, r.end - r.start);
            if (c.back() == L';') c.pop_back();
            c += L"\" -map \"[a0]\"";
            if (tracks == 3) c += L" -map \"[s0]\" -map \"[m0]\"";
            c += L" -c:a aac -aac_coder twoloop -b:a 192k";
        }
    }
    c += TrackMeta(tracks) + L" -avoid_negative_ts make_zero " + Quote(out);
    return c;
}

// Precise mode: re-encode the given segments into one file.
std::wstring PreciseCmd(const ExportJob& job, const std::vector<const Segment*>& segs, int tracks, const std::wstring& out) {
    const MediaInfo& first = job.sources[segs[0]->src];
    int W = first.width & ~1, H = first.height & ~1;
    double F = first.fps > 0 ? first.fps : 30;
    std::wstring c = Quote(FfmpegPath()) + L" -hide_banner -y -v error -progress pipe:1 -nostats";
    for (auto* s : segs)
        c += L" -ss " + Num(s->start, 6) + L" -to " + Num(s->end, 6) + L" -i " + Quote(job.sources[s->src].path);
    std::wstring g, cat;
    for (size_t k = 0; k < segs.size(); k++) {
        std::wstring K = std::to_wstring(k);
        g += L"[" + K + L":v:0]setpts=PTS-STARTPTS,scale=" + std::to_wstring(W) + L":" + std::to_wstring(H) +
             L":force_original_aspect_ratio=decrease,pad=" + std::to_wstring(W) + L":" + std::to_wstring(H) +
             L":(ow-iw)/2:(oh-ih)/2,setsar=1,fps=" + Num(F, 3) + L",format=yuv420p[v" + K + L"];";
        if (tracks > 0) g += AudioChain(job, *segs[k], (int)k, (int)k, tracks, segs[k]->length());
        cat += L"[v" + K + L"]";
        if (tracks > 0) cat += L"[a" + K + L"]";
        if (tracks == 3) cat += L"[s" + K + L"][m" + K + L"]";
    }
    g += cat + L"concat=n=" + std::to_wstring(segs.size()) + L":v=1:a=" + std::to_wstring(tracks) + L"[v]";
    if (tracks > 0) g += L"[a]";
    if (tracks == 3) g += L"[s][m]";
    c += L" -filter_complex \"" + g + L"\" -map \"[v]\"";
    if (tracks > 0) c += L" -map \"[a]\"";
    if (tracks == 3) c += L" -map \"[s]\" -map \"[m]\"";
    c += VideoEncoder(job.quality);
    if (tracks > 0) c += L" -c:a aac -aac_coder twoloop -b:a 192k";
    c += TrackMeta(tracks) + L" -movflags +faststart " + Quote(out);
    return c;
}

std::wstring UniqueOut(const std::wstring& path) {
    if (!FileExists(path)) return path;
    std::wstring dir = DirOf(path), stem = StemOf(path);
    for (int i = 2;; i++) {
        std::wstring p = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L").mp4";
        if (!FileExists(p)) return p;
    }
}

}  // namespace

bool RunExport(const ExportJob& job, std::function<void(double)> progress, std::atomic<bool>& cancel,
               std::vector<std::wstring>& written, std::wstring& err) {
    std::vector<const Segment*> kept;
    std::vector<EffRange> ranges = EffectiveRanges(job.sources, job.segments, job.cut);
    std::vector<EffRange> keptR;
    for (size_t i = 0; i < job.segments.size(); i++)
        if (job.segments[i].keep && ranges[i].end - ranges[i].start > 0.01) {
            kept.push_back(&job.segments[i]);
            keptR.push_back(ranges[i]);
        }
    if (kept.empty()) { err = L"남긴 구간이 없어요. 구간을 하나 이상 체크해 주세요."; return false; }
    int tracks = OutputTracks(job, kept);
    double total = 0;
    for (auto& r : keptR) total += r.end - r.start;
    double doneBefore = 0;
    auto report = [&](double t) { if (progress) progress(std::min(1.0, (doneBefore + t) / std::max(0.001, total))); };

    std::wstring tmpDir = AppDataDir() + L"\\editor_tmp";
    CreateDirectoryW(tmpDir.c_str(), nullptr);

    // Output names.
    std::vector<std::wstring> outs;
    if (job.save == SaveMode::OneFile) {
        outs.push_back(UniqueOut(job.outPath));
    } else {
        std::wstring dir = DirOf(job.outPath), stem = StemOf(job.outPath);
        for (size_t k = 0; k < kept.size(); k++) {
            wchar_t num[16];
            swprintf(num, 16, L"_%02zu", k + 1);
            std::wstring nm = SafeName(kept[k]->name);
            outs.push_back(UniqueOut(dir + L"\\" + stem + num + (nm.empty() ? L"" : L"_" + nm) + L".mp4"));
        }
    }

    if (job.cut == CutMode::Fast) {
        std::vector<std::wstring> parts;
        for (size_t k = 0; k < kept.size(); k++) {
            std::wstring out = job.save == SaveMode::PerSegment ? outs[k]
                               : (kept.size() == 1 ? outs[0] : tmpDir + L"\\part" + std::to_wstring(k) + L".mp4");
            if (!RunWithProgress(FastSegmentCmd(job, *kept[k], keptR[k], tracks, out), report, cancel, err)) {
                for (auto& p : parts) DeleteFileW(p.c_str());
                DeleteFileW(out.c_str());
                return false;
            }
            doneBefore += keptR[k].end - keptR[k].start;
            parts.push_back(out);
        }
        if (job.save == SaveMode::OneFile && kept.size() > 1) {
            std::string list;
            for (auto& p : parts) {
                std::string u = Narrow(p);
                std::string esc;
                for (char ch : u) { if (ch == '\'') esc += "'\\''"; else esc += ch; }
                list += "file '" + esc + "'\n";
            }
            std::wstring listPath = tmpDir + L"\\list.txt";
            HANDLE h = CreateFileW(listPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD w;
            WriteFile(h, list.data(), (DWORD)list.size(), &w, nullptr);
            CloseHandle(h);
            std::wstring c = Quote(FfmpegPath()) + L" -hide_banner -y -v error -progress pipe:1 -nostats -f concat -safe 0 -i " +
                             Quote(listPath) + L" -map 0 -c copy" + TrackMeta(tracks) + L" -movflags +faststart " + Quote(outs[0]);
            bool ok = RunWithProgress(c, nullptr, cancel, err);
            for (auto& p : parts) DeleteFileW(p.c_str());
            DeleteFileW(listPath.c_str());
            if (!ok) { DeleteFileW(outs[0].c_str()); return false; }
        }
        written = outs;
        if (progress) progress(1.0);
        return true;
    }

    // Precise
    if (job.save == SaveMode::OneFile) {
        if (!RunWithProgress(PreciseCmd(job, kept, tracks, outs[0]), report, cancel, err)) {
            DeleteFileW(outs[0].c_str());
            return false;
        }
    } else {
        for (size_t k = 0; k < kept.size(); k++) {
            if (!RunWithProgress(PreciseCmd(job, {kept[k]}, tracks, outs[k]), report, cancel, err)) {
                DeleteFileW(outs[k].c_str());
                return false;
            }
            doneBefore += kept[k]->length();
        }
    }
    written = outs;
    if (progress) progress(1.0);
    return true;
}
