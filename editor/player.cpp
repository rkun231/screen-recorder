#include "player.h"
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <deque>
#include <condition_variable>
#include <algorithm>
#include <cmath>

namespace {
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const int kRate = 48000;

double QpcSeconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}

// Reads exactly n bytes (false on EOF/error).
bool ReadAll(HANDLE h, void* buf, DWORD n) {
    BYTE* p = (BYTE*)buf;
    while (n) {
        DWORD r = 0;
        if (!ReadFile(h, p, n, &r, nullptr) || r == 0) return false;
        p += r;
        n -= r;
    }
    return true;
}
}  // namespace

void PreviewSize(int srcW, int srcH, int& w, int& h) {
    if (srcW <= 0 || srcH <= 0) { w = 960; h = 540; return; }
    w = std::min(srcW, 960) & ~1;
    h = std::max(2, (int)((long long)srcH * w / srcW) & ~1);
}

// ------------------------------------------------------------------ session

struct Player::Session {
    Player* pl = nullptr;
    std::vector<PlayPiece> run;  // contiguous pieces of one source
    std::vector<int> runIdx;     // their indexes in the whole sequence
    double seqStart = 0, seqEnd = 0, srcStart = 0, srcEnd = 0;
    int tracks = 0;              // 0, 1 or 2 sound tracks we read (2 = computer + mic)
    int pw = 0, ph = 0;
    double outFps = 30;

    ChildProc proc;
    std::vector<HANDLE> pipes;
    std::vector<std::wstring> pipeNames;
    std::thread videoTh, audioTh[2], renderTh, presentTh;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    HANDLE stopEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    struct Frame { double pts; std::vector<uint32_t> px; };
    std::mutex vqMu;
    std::condition_variable vqCv;
    std::deque<Frame> vq;
    bool videoEof = false;
    bool haveFirstFrame = false;

    struct Ring { std::mutex mu; std::deque<float> d; bool eof = false; };
    Ring rings[2];
    long long consumedNoDev[2] = {0, 0};

    std::atomic<bool> clockStarted{false};
    std::atomic<double> clockBase{0};   // QPC seconds when the clock started (no-audio case)
    std::atomic<long long> playedFrames{0};
    std::atomic<bool> audioClock{false};

    ~Session() { if (stopEv) CloseHandle(stopEv); }

    double Clock() const {
        if (!clockStarted) return seqStart;
        if (audioClock) return seqStart + (double)playedFrames.load() / kRate;
        return seqStart + (QpcSeconds() - clockBase.load());
    }
    // Volumes at a sequence time (live from the player).
    void VolumesAt(double t, float& sv, float& mv) {
        std::lock_guard<std::mutex> lk(pl->mu_);
        sv = 1;
        mv = 1;
        for (size_t i = 0; i < runIdx.size(); i++) {
            if (runIdx[i] < 0 || runIdx[i] >= (int)pl->pieces_.size()) continue;
            const PlayPiece& p = pl->pieces_[runIdx[i]];
            if (t >= p.seqStart - 1e-6 && (t < p.seqEnd() || i + 1 == runIdx.size())) { sv = p.sysVol; mv = p.micVol; return; }
        }
    }

    bool Start();
    void Shutdown();
    void VideoMain();
    void AudioMain(int k);
    void RenderMain();
    void PresentMain();
};

bool Player::Session::Start() {
    const PlayPiece& f = run.front();
    seqStart = f.seqStart;
    srcStart = f.srcStart;
    srcEnd = run.back().srcEnd;
    seqEnd = seqStart + (srcEnd - srcStart);
    PreviewSize(f.width, f.height, pw, ph);
    outFps = std::min(60.0, f.fps > 1 ? f.fps : 30.0);
    tracks = f.audioTracks >= 3 ? 2 : (f.audioTracks >= 1 ? 1 : 0);

    std::wstring vf = L"fps=" + Num(outFps, 4) + L",scale=" + std::to_wstring(pw) + L":" + std::to_wstring(ph) +
                      L":flags=bilinear,format=bgra";
    std::wstring cmd = Quote(FfmpegPath()) + L" -v error -nostdin -y -ss " + Num(srcStart, 6) + L" -to " + Num(srcEnd, 6) +
                       L" -i " + Quote(f.path) + L" -map 0:v:0 -vf \"" + vf + L"\" -f rawvideo pipe:1";
    static std::atomic<int> counter{0};
    int c = counter++;
    for (int k = 0; k < tracks; k++) {
        std::wstring name = L"\\\\.\\pipe\\kedit_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(c) + L"_" + std::to_wstring(k);
        HANDLE h = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 20, 1 << 20, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        pipes.push_back(h);
        pipeNames.push_back(name);
        int stream = f.audioTracks >= 3 ? k + 1 : 0;  // a:1 computer, a:2 mic; or the only track
        cmd += L" -map 0:a:" + std::to_wstring(stream) + L" -f f32le -ar 48000 -ac 2 " + Quote(name);
    }
    for (int k = 0; k < tracks; k++) audioTh[k] = std::thread(&Session::AudioMain, this, k);
    if (!StartChild(cmd, AppDataDir() + L"\\editor_preview.log", true, proc)) {
        stop = true;
        SetEvent(stopEv);
        for (int k = 0; k < tracks; k++) if (audioTh[k].joinable()) audioTh[k].join();
        return false;
    }
    videoTh = std::thread(&Session::VideoMain, this);
    if (tracks) renderTh = std::thread(&Session::RenderMain, this);
    presentTh = std::thread(&Session::PresentMain, this);
    return true;
}

void Player::Session::Shutdown() {
    stop = true;
    SetEvent(stopEv);
    vqCv.notify_all();
    if (proc.h) TerminateProcess(proc.h, 0);
    if (videoTh.joinable()) videoTh.join();
    for (auto& t : audioTh) if (t.joinable()) t.join();
    if (renderTh.joinable()) renderTh.join();
    if (presentTh.joinable() && presentTh.get_id() != std::this_thread::get_id()) presentTh.join();
    for (HANDLE h : pipes) CloseHandle(h);
    pipes.clear();
    CloseChild(proc);
}

void Player::Session::VideoMain() {
    size_t bytes = (size_t)pw * ph * 4;
    long long n = 0;
    while (!stop) {
        Frame fr;
        fr.px.resize((size_t)pw * ph);
        if (!ReadAll(proc.out, fr.px.data(), (DWORD)bytes)) break;
        fr.pts = seqStart + n / outFps;
        n++;
        std::unique_lock<std::mutex> lk(vqMu);
        vqCv.wait(lk, [&] { return stop || vq.size() < 30; });
        if (stop) break;
        vq.push_back(std::move(fr));
        haveFirstFrame = true;
    }
    std::lock_guard<std::mutex> lk(vqMu);
    videoEof = true;
}

void Player::Session::AudioMain(int k) {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED ov{};
    ov.hEvent = ev;
    bool connected = ConnectNamedPipe(pipes[k], &ov) || GetLastError() == ERROR_PIPE_CONNECTED;
    if (!connected && GetLastError() == ERROR_IO_PENDING) {
        HANDLE hs[2] = {ev, stopEv};
        connected = WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0;
        if (!connected) { CancelIoEx(pipes[k], &ov); DWORD d; GetOverlappedResult(pipes[k], &ov, &d, TRUE); }
    }
    std::vector<float> buf(4096);
    while (connected && !stop) {
        {   // don't run too far ahead
            std::unique_lock<std::mutex> lk(rings[k].mu);
            if (rings[k].d.size() > (size_t)kRate * 2 * 4) { lk.unlock(); Sleep(10); continue; }
        }
        ResetEvent(ev);
        OVERLAPPED o{};
        o.hEvent = ev;
        DWORD r = 0;
        if (!ReadFile(pipes[k], buf.data(), (DWORD)(buf.size() * sizeof(float)), &r, &o)) {
            if (GetLastError() != ERROR_IO_PENDING) break;
            HANDLE hs[2] = {ev, stopEv};
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0) { CancelIoEx(pipes[k], &o); GetOverlappedResult(pipes[k], &o, &r, TRUE); break; }
            if (!GetOverlappedResult(pipes[k], &o, &r, FALSE)) break;
        }
        if (r == 0) break;
        std::lock_guard<std::mutex> lk(rings[k].mu);
        rings[k].d.insert(rings[k].d.end(), buf.begin(), buf.begin() + r / sizeof(float));
    }
    {
        std::lock_guard<std::mutex> lk(rings[k].mu);
        rings[k].eof = true;
    }
    CloseHandle(ev);
}

void Player::Session::RenderMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioRenderClient* rc = nullptr;
    IAudioClock* clk = nullptr;
    UINT32 bufFrames = 0;
    UINT64 clkFreq = 1;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
        SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) {
        WAVEFORMATEXTENSIBLE wf{};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = 2;
        wf.Format.nSamplesPerSec = kRate;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = 8;
        wf.Format.nAvgBytesPerSec = kRate * 8;
        wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wf.SubFormat = kSubtypeFloat;
        ok = SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      1000000, 0, (WAVEFORMATEX*)&wf, nullptr)) &&
             SUCCEEDED(ac->GetBufferSize(&bufFrames)) &&
             SUCCEEDED(ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc)) &&
             SUCCEEDED(ac->GetService(__uuidof(IAudioClock), (void**)&clk)) && SUCCEEDED(clk->GetFrequency(&clkFreq));
    }
    if (ok) audioClock = true;

    // Wait until there is some sound buffered (and a picture), then start.
    auto buffered = [&](int k) { std::lock_guard<std::mutex> lk(rings[k].mu); return rings[k].eof || rings[k].d.size() >= (size_t)kRate * 2 / 6; };
    while (!stop) {
        bool ready = true;
        for (int k = 0; k < tracks; k++) ready &= buffered(k);
        {
            std::lock_guard<std::mutex> lk(vqMu);
            ready &= haveFirstFrame || videoEof;
        }
        if (ready) break;
        Sleep(5);
    }
    if (ok && !stop) {
        ac->Start();
        clockStarted = true;
    } else if (!stop) {
        clockBase = QpcSeconds();  // no sound device: use the system clock
        clockStarted = true;
    }

    // No sound device: still consume the sound in step with the clock, so FFmpeg never waits on us.
    while (!stop && !ok && tracks) {
        long long due = (long long)((QpcSeconds() - clockBase.load()) * kRate) + kRate / 2;
        bool allEof = true;
        for (int k = 0; k < tracks; k++) {
            std::lock_guard<std::mutex> lk(rings[k].mu);
            long long drop = std::min<long long>((long long)rings[k].d.size() / 2, std::max<long long>(0, due - consumedNoDev[k]));
            rings[k].d.erase(rings[k].d.begin(), rings[k].d.begin() + drop * 2);
            consumedNoDev[k] += drop;
            allEof &= rings[k].eof && rings[k].d.empty();
        }
        if (allEof) break;
        if (WaitForSingleObject(stopEv, 10) == WAIT_OBJECT_0) break;
    }

    long long written = 0;
    while (!stop && ok) {
        UINT32 pad = 0;
        if (FAILED(ac->GetCurrentPadding(&pad))) break;
        UINT64 pos = 0, qpc = 0;
        if (SUCCEEDED(clk->GetPosition(&pos, &qpc))) playedFrames = (long long)((double)pos / clkFreq * kRate);
        UINT32 room = bufFrames - pad;
        // frames available in all rings
        size_t avail = SIZE_MAX;
        bool allEof = true;
        for (int k = 0; k < tracks; k++) {
            std::lock_guard<std::mutex> lk(rings[k].mu);
            avail = std::min(avail, rings[k].d.size() / 2);
            allEof &= rings[k].eof;
        }
        UINT32 n = (UINT32)std::min<size_t>(room, avail);
        if (n > 0) {
            BYTE* data = nullptr;
            if (SUCCEEDED(rc->GetBuffer(n, &data))) {
                float* out = (float*)data;
                float sv, mv;
                VolumesAt(seqStart + (double)written / kRate, sv, mv);
                if (tracks == 2) {
                    std::lock_guard<std::mutex> l0(rings[0].mu);
                    std::lock_guard<std::mutex> l1(rings[1].mu);
                    for (UINT32 i = 0; i < n * 2; i++) {
                        out[i] = std::clamp(rings[0].d[i] * sv + rings[1].d[i] * mv, -1.0f, 1.0f);
                    }
                    rings[0].d.erase(rings[0].d.begin(), rings[0].d.begin() + n * 2);
                    rings[1].d.erase(rings[1].d.begin(), rings[1].d.begin() + n * 2);
                } else {
                    std::lock_guard<std::mutex> l0(rings[0].mu);
                    for (UINT32 i = 0; i < n * 2; i++) out[i] = std::clamp(rings[0].d[i] * sv, -1.0f, 1.0f);
                    rings[0].d.erase(rings[0].d.begin(), rings[0].d.begin() + n * 2);
                }
                rc->ReleaseBuffer(n, 0);
                written += n;
            }
        } else if (allEof && avail == 0 && pad == 0) {
            break;  // everything played
        }
        if (WaitForSingleObject(stopEv, 5) == WAIT_OBJECT_0) break;
    }
    if (ok) ac->Stop();
    if (clk) clk->Release();
    if (rc) rc->Release();
    if (ac) ac->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    if (ok) {  // everything that reached the device has been played
        for (int k = 0; k < tracks; k++) {
            std::lock_guard<std::mutex> lk(rings[k].mu);
            rings[k].eof = true;
        }
    }
    audioClock = audioClock && !stop;
    CoUninitialize();
}

void Player::Session::PresentMain() {
    if (!tracks) {
        // No sound: start the clock when the first picture is there.
        while (!stop) {
            {
                std::lock_guard<std::mutex> lk(vqMu);
                if (haveFirstFrame || videoEof) break;
            }
            Sleep(3);
        }
        clockBase = QpcSeconds();
        clockStarted = true;
    }
    while (!stop) {
        Sleep(4);
        if (!clockStarted) continue;
        double t = Clock();
        pl->pos_ = std::min(t, seqEnd);
        Frame show;
        bool got = false, eof = false;
        {
            std::lock_guard<std::mutex> lk(vqMu);
            while (!vq.empty() && vq.front().pts <= t + 0.002) {
                show = std::move(vq.front());
                vq.pop_front();
                got = true;
            }
            eof = videoEof && vq.empty();
        }
        if (got) {
            vqCv.notify_all();
            pl->PublishFrame(show.px.data(), pw, ph);
            PostMessageW(pl->notify_, pl->msgFrame_, 0, 0);
        }
        bool soundDone = true;
        for (int k = 0; k < tracks; k++) {
            std::lock_guard<std::mutex> lk(rings[k].mu);
            soundDone &= rings[k].eof && rings[k].d.empty();
        }
        if (t >= seqEnd - 0.5 / outFps || (eof && soundDone)) {
            finished = true;
            PostMessageW(pl->notify_, pl->msgRunEnd_, 0, (LPARAM) new double(std::min(t, seqEnd)));
            return;
        }
    }
}

// ------------------------------------------------------------------ player

Player::Player(HWND notify, UINT msgFrame, UINT msgRunEnd) : notify_(notify), msgFrame_(msgFrame), msgRunEnd_(msgRunEnd) {
    stillEv_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stillTh_ = std::thread(&Player::StillWorker, this);
}

Player::~Player() {
    Stop();
    {
        std::lock_guard<std::mutex> lk(stillMu_);
        stillQuit_ = true;
    }
    SetEvent(stillEv_);
    if (stillTh_.joinable()) stillTh_.join();
    CloseHandle(stillEv_);
}

void Player::SetPieces(const std::vector<PlayPiece>& pieces) {
    std::lock_guard<std::mutex> lk(mu_);
    pieces_ = pieces;
}

void Player::SetVolumes(const std::vector<std::pair<float, float>>& vols) {
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < pieces_.size() && i < vols.size(); i++) {
        pieces_[i].sysVol = vols[i].first;
        pieces_[i].micVol = vols[i].second;
    }
}

void Player::PlayFrom(int idx, double t) {
    Stop();
    auto s = std::make_unique<Session>();
    s->pl = this;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (idx < 0 || idx >= (int)pieces_.size()) return;
        // Run = this piece + following pieces that continue the same source without a gap.
        PlayPiece first = pieces_[idx];
        double off = std::clamp(t - first.seqStart, 0.0, std::max(0.0, first.srcEnd - first.srcStart - 0.01));
        first.srcStart += off;
        first.seqStart += off;
        s->run.push_back(first);
        s->runIdx.push_back(idx);
        for (int i = idx + 1; i < (int)pieces_.size(); i++) {
            const PlayPiece& prev = pieces_[i - 1];
            const PlayPiece& cur = pieces_[i];
            if (cur.skip) break;
            if (cur.path != prev.path || std::fabs(cur.srcStart - prev.srcEnd) > 1e-6) break;
            s->run.push_back(cur);
            s->runIdx.push_back(i);
        }
    }
    pos_ = s->run.front().seqStart;
    if (!s->Start()) return;
    sess_ = std::move(s);
    playing_ = true;
}

void Player::Stop() {
    if (sess_) {
        sess_->Shutdown();
        sess_.reset();
    }
    playing_ = false;
}

double Player::Position() const { return pos_.load(); }

bool Player::RunDone() const { return !sess_ || sess_->finished.load(); }

void Player::PublishFrame(const uint32_t* px, int w, int h) {
    std::lock_guard<std::mutex> lk(frameMu_);
    frame_.assign(px, px + (size_t)w * h);
    fw_ = w;
    fh_ = h;
}

bool Player::CopyFrame(std::vector<uint32_t>& px, int& w, int& h) {
    std::lock_guard<std::mutex> lk(frameMu_);
    if (frame_.empty()) return false;
    px = frame_;
    w = fw_;
    h = fh_;
    return true;
}

void Player::ShowStill(int idx, double t) {
    std::lock_guard<std::mutex> lk(stillMu_);
    {
        std::lock_guard<std::mutex> l2(mu_);
        if (idx < 0 || idx >= (int)pieces_.size()) return;
        stillPiece_ = pieces_[idx];
    }
    stillT_ = std::clamp(t - stillPiece_.seqStart, 0.0, std::max(0.0, stillPiece_.srcEnd - stillPiece_.srcStart - 0.02)) + stillPiece_.srcStart;
    pos_ = t;
    stillPending_ = true;
    SetEvent(stillEv_);
}

void Player::StillWorker() {
    while (true) {
        WaitForSingleObject(stillEv_, INFINITE);
        PlayPiece p;
        double t;
        {
            std::lock_guard<std::mutex> lk(stillMu_);
            if (stillQuit_) return;
            if (!stillPending_) continue;
            stillPending_ = false;
            p = stillPiece_;
            t = stillT_;
        }
        int w, h;
        PreviewSize(p.width, p.height, w, h);
        std::wstring cmd = Quote(FfmpegPath()) + L" -v error -nostdin -ss " + Num(t, 6) + L" -i " + Quote(p.path) +
                           L" -map 0:v:0 -frames:v 1 -vf \"scale=" + std::to_wstring(w) + L":" + std::to_wstring(h) +
                           L":flags=bilinear,format=bgra\" -f rawvideo pipe:1";
        ChildProc cp;
        if (!StartChild(cmd, AppDataDir() + L"\\editor_still.log", true, cp)) continue;
        std::vector<uint32_t> px((size_t)w * h);
        bool ok = ReadAll(cp.out, px.data(), (DWORD)(px.size() * 4));
        WaitForSingleObject(cp.h, 3000);
        CloseChild(cp);
        if (ok) {
            PublishFrame(px.data(), w, h);
            PostMessageW(notify_, msgFrame_, 0, 0);
        }
    }
}
