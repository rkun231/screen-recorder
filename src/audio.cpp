#define WIN32_LEAN_AND_MEAN
#include "audio.h"
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <mmreg.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <avrt.h>

namespace {
const PROPERTYKEY kFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// Sound threads must never wait behind the recorded program or FFmpeg: if they are late, sound goes missing.
// "Pro Audio" (MMCSS) is what audio programs use; fall back to plain time-critical priority.
void BoostAudioThread(bool critical) {
    DWORD task = 0;
    if (critical && AvSetMmThreadCharacteristicsW(L"Pro Audio", &task)) return;
    SetThreadPriority(GetCurrentThread(), critical ? THREAD_PRIORITY_TIME_CRITICAL : THREAD_PRIORITY_HIGHEST);
}

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

long long Qpc100ns() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (c.QuadPart / f.QuadPart) * 10000000LL + (c.QuadPart % f.QuadPart) * 10000000LL / f.QuadPart;
}

std::vector<AudioDevice> EnumDevices(EDataFlow flow) {
    std::vector<AudioDevice> out;
    IMMDeviceEnumerator* en = nullptr;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)))
        return out;
    if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            AudioDevice ad;
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id))) { ad.id = id; CoTaskMemFree(id); }
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) ad.name = v.pwszVal;
                PropVariantClear(&v);
                ps->Release();
            }
            if (ad.name.empty()) ad.name = L"(이름 없는 장치)";
            if (!ad.id.empty()) out.push_back(ad);
            d->Release();
        }
        col->Release();
    }
    en->Release();
    return out;
}

// Writes a whole buffer to an overlapped pipe; gives up when `stop` is signaled.
bool PipeWrite(HANDLE pipe, HANDLE ioEv, HANDLE stop, const void* data, size_t bytes) {
    const BYTE* p = (const BYTE*)data;
    while (bytes) {
        OVERLAPPED ov{};
        ov.hEvent = ioEv;
        ResetEvent(ioEv);
        DWORD chunk = bytes > (1u << 20) ? (1u << 20) : (DWORD)bytes;
        DWORD w = 0;
        if (!WriteFile(pipe, p, chunk, &w, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            HANDLE hs[2] = {ioEv, stop};
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &w, TRUE);
                return false;
            }
            if (!GetOverlappedResult(pipe, &ov, &w, FALSE)) return false;
        }
        p += w;
        bytes -= w;
    }
    return true;
}
}  // namespace

long long WallClockUs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)((u.QuadPart - 116444736000000000ULL) / 10ULL);
}

std::vector<AudioDevice> EnumOutputDevices() { return EnumDevices(eRender); }
std::vector<AudioDevice> EnumInputDevices() { return EnumDevices(eCapture); }

// ------------------------------------------------------------------ Source

struct AudioEngine::Source {
    static constexpr long long kRing = AudioEngine::kRate * 4;  // 4 s timeline buffer

    bool isMic = false;
    std::wstring id;
    long long qpcEpoch100ns = 0, offsetFrames = 0;
    HANDLE stop = nullptr;
    HANDLE ready = nullptr;
    std::thread th;
    bool ok = false;
    std::wstring err;

    std::mutex mu;
    std::vector<float> ring = std::vector<float>((size_t)kRing * 2, 0.0f);
    long long consumed = 0;  // mixer has taken everything before this frame
    long long validEnd = 0;  // end of the last written packet
    long long lastArrival = -1;  // timeline position ("now") when the last packet came in
    // Troubleshooting numbers (reported when the recording piece ends).
    long long lateFrames = 0, maxLateFrames = 0, packets = 0, resyncs = 0;
    long long maxLoopGap100ns = 0;

    // Fallback conversion when Windows can't hand us 48 kHz stereo float directly.
    bool convert = false;
    int inRate = kRate, inCh = 2, inBits = 32;
    bool inFloat = true;
    double resPos = 0;
    float prevL = 0, prevR = 0;
    std::vector<float> tmp, src;

    long long NowFrames() const { return (Qpc100ns() - qpcEpoch100ns) * AudioEngine::kRate / 10000000LL - offsetFrames; }
    void Write(long long idx, const float* data, long long frames) {
        std::lock_guard<std::mutex> lk(mu);
        packets++;
        lastArrival = NowFrames();
        long long from = std::max(idx, 0LL);  // sound from before the recording started doesn't count
        if (from < consumed && from < idx + frames) {
            lateFrames += std::min(idx + frames, consumed) - from;
            maxLateFrames = std::max(maxLateFrames, consumed - from);
        }
        for (long long i = 0; i < frames; i++) {
            long long f = idx + i;
            if (f < consumed || f >= consumed + kRing) continue;  // too late / too far ahead
            size_t s = (size_t)(f % kRing) * 2;
            ring[s] = data ? data[i * 2] : 0.0f;
            ring[s + 1] = data ? data[i * 2 + 1] : 0.0f;
        }
        validEnd = std::max(validEnd, idx + frames);
    }
    // Adds frames [pos, pos+n) into `out` and clears them so stale audio can never come back.
    void Snapshot(long long& end, long long& arrival) {
        std::lock_guard<std::mutex> lk(mu);
        end = validEnd;
        arrival = lastArrival;
    }
    void ReadAndClear(long long pos, long long n, float* out) {
        std::lock_guard<std::mutex> lk(mu);
        for (long long i = 0; i < n; i++) {
            size_t s = (size_t)((pos + i) % kRing) * 2;
            out[i * 2] += ring[s];
            out[i * 2 + 1] += ring[s + 1];
            ring[s] = ring[s + 1] = 0.0f;
        }
        consumed = pos + n;
    }

    float Sample(const BYTE* p, int ch) const {
        const BYTE* q = p + ch * (inBits / 8);
        if (inFloat) return inBits == 64 ? (float)*(const double*)q : *(const float*)q;
        switch (inBits) {
            case 16: return *(const int16_t*)q / 32768.0f;
            case 24: { int v = (q[0] << 8) | (q[1] << 16) | (q[2] << 24); return (v >> 8) / 8388608.0f; }
            default: return *(const int32_t*)q / 2147483648.0f;
        }
    }
    // Converts one packet to 48 kHz stereo float in `tmp`; returns the number of output frames.
    long long Convert(const BYTE* data, UINT32 frames, bool silent) {
        int bpf = inCh * inBits / 8;
        src.assign((size_t)(frames + 1) * 2, 0.0f);
        src[0] = prevL;
        src[1] = prevR;
        for (UINT32 i = 0; i < frames; i++) {
            float l = 0, r = 0;
            if (!silent) {
                const BYTE* p = data + (size_t)i * bpf;
                l = Sample(p, 0);
                r = inCh > 1 ? Sample(p, 1) : l;
            }
            src[(i + 1) * 2] = l;
            src[(i + 1) * 2 + 1] = r;
        }
        tmp.clear();
        if (inRate == kRate) {
            tmp.assign(src.begin() + 2, src.end());
        } else {
            double step = (double)inRate / kRate;
            while (resPos < frames) {  // interpolate between src[k] and src[k+1]; src[0] is last packet's end
                int k = (int)resPos;
                float f = (float)(resPos - k);
                tmp.push_back(src[k * 2] * (1 - f) + src[(k + 1) * 2] * f);
                tmp.push_back(src[k * 2 + 1] * (1 - f) + src[(k + 1) * 2 + 1] * f);
                resPos += step;
            }
            resPos -= frames;
        }
        if (frames) {
            prevL = src[frames * 2];
            prevR = src[frames * 2 + 1];
        }
        return (long long)tmp.size() / 2;
    }

    void Main();
};

void AudioEngine::Source::Main() {
    if (GetEnvironmentVariableW(L"KREC_DEBUG_FAKEAUDIO", nullptr, 0) > 0) {
        // Developer test hook: a sine tone instead of a real device (440 Hz computer sound, 880 Hz mic).
        ok = true;
        SetEvent(ready);
        double ph = 0, f = isMic ? 880.0 : 440.0;
        std::vector<float> buf;
        long long next = Qpc100ns();
        bool stallTest = GetEnvironmentVariableW(L"KREC_DEBUG_AUDIOSTALL", nullptr, 0) > 0;
        int loops = 0;
        while (WaitForSingleObject(stop, 10) == WAIT_TIMEOUT) {
            if (stallTest && ++loops % 50 == 0) Sleep(250);  // pretend the thread was starved for 0.25 s
            long long now = Qpc100ns();
            while (next + 100000 <= now) {  // 10 ms packets
                buf.resize(480 * 2);
                for (int i = 0; i < 480; i++) {
                    float v = 0.3f * (float)sin(ph);
                    ph += 2 * 3.14159265358979 * f / kRate;
                    buf[i * 2] = buf[i * 2 + 1] = v;
                }
                long long idx = (next - qpcEpoch100ns) * kRate / 10000000LL - offsetFrames;
                if (validEnd > 0 && std::llabs(idx - validEnd) <= 96) idx = validEnd;
                Write(idx, buf.data(), 480);
                next += 100000;
            }
        }
        return;
    }
    BoostAudioThread(true);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX* mix = nullptr;
    bool started = false;
    const wchar_t* what = isMic ? L"마이크" : L"출력 장치";

    auto fail = [&](const std::wstring& msg, HRESULT hr) {
        wchar_t code[32];
        swprintf(code, 32, L" (0x%08lX)", (unsigned long)hr);
        err = msg + code;
        ok = false;
        SetEvent(ready);
    };

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr)) { fail(L"오디오 시스템을 열 수 없어요", hr); goto done; }
    if (id.empty()) hr = en->GetDefaultAudioEndpoint(isMic ? eCapture : eRender, eConsole, &dev);
    else hr = en->GetDevice(id.c_str(), &dev);
    if (FAILED(hr)) { fail(std::wstring(L"선택한 ") + what + L"를 찾을 수 없어요. 연결돼 있는지 확인해 주세요", hr); goto done; }
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
    if (FAILED(hr)) { fail(std::wstring(what) + L"를 열 수 없어요", hr); goto done; }
    {
        // Ask Windows to hand us 48 kHz stereo float directly (it converts for us).
        WAVEFORMATEXTENSIBLE want{};
        want.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        want.Format.nChannels = 2;
        want.Format.nSamplesPerSec = kRate;
        want.Format.wBitsPerSample = 32;
        want.Format.nBlockAlign = 8;
        want.Format.nAvgBytesPerSec = kRate * 8;
        want.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        want.Samples.wValidBitsPerSample = 32;
        want.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        want.SubFormat = kSubtypeFloat;
        DWORD flags = (isMic ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK) | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10000000, 0, (WAVEFORMATEX*)&want, nullptr);
        if (FAILED(hr)) {
            // Fall back to the device's own format and convert ourselves.
            SafeRelease(ac);
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
            if (FAILED(hr)) { fail(std::wstring(what) + L"를 열 수 없어요", hr); goto done; }
            hr = ac->GetMixFormat(&mix);
            if (FAILED(hr)) { fail(std::wstring(what) + L" 형식을 읽을 수 없어요", hr); goto done; }
            inFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                      (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(((WAVEFORMATEXTENSIBLE*)mix)->SubFormat, kSubtypeFloat));
            inBits = mix->wBitsPerSample;
            inCh = mix->nChannels;
            inRate = (int)mix->nSamplesPerSec;
            if (!(inBits == 16 || inBits == 24 || inBits == 32 || (inFloat && inBits == 64)) || inCh < 1) {
                fail(std::wstring(L"지원하지 않는 ") + what + L" 형식이에요", E_FAIL);
                goto done;
            }
            hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, isMic ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, mix, nullptr);
            if (FAILED(hr)) { fail(std::wstring(what) + L" 녹음을 시작할 수 없어요", hr); goto done; }
            convert = true;
        }
    }
    hr = ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
    if (FAILED(hr)) { fail(std::wstring(what) + L" 녹음을 시작할 수 없어요", hr); goto done; }
    if (FAILED(ac->Start())) { fail(std::wstring(what) + L" 녹음을 시작할 수 없어요", E_FAIL); goto done; }
    started = true;
    ok = true;
    SetEvent(ready);

    {
        // Keep the sound continuous: device timestamps wobble by a few ms, and following them exactly
        // leaves tiny holes/overlaps (crackling). Only jump when the difference is real (silence, glitch).
        const long long snap = kRate * 30 / 1000;
        long long lastLoop = Qpc100ns();
        while (WaitForSingleObject(stop, 5) == WAIT_TIMEOUT) {
            long long nowq = Qpc100ns();
            maxLoopGap100ns = std::max(maxLoopGap100ns, nowq - lastLoop);
            lastLoop = nowq;
            UINT32 n = 0;
            if (FAILED(cap->GetNextPacketSize(&n))) break;  // device unplugged: just stop (silence)
            while (n > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD fl = 0;
                UINT64 devPos = 0, qpcPos = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &fl, &devPos, &qpcPos))) goto loopEnd;
                bool silent = (fl & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const float* out = nullptr;
                long long outFrames = frames;
                if (convert) {
                    outFrames = Convert(data, frames, silent);
                    out = silent ? nullptr : tmp.data();
                } else {
                    out = silent ? nullptr : (const float*)data;
                }
                // Where this packet belongs on the recording's timeline (sample 0 == epoch).
                long long idx;
                if (qpcPos) idx = ((long long)qpcPos - qpcEpoch100ns) * kRate / 10000000LL - offsetFrames;
                else idx = validEnd > 0 ? validEnd : ((Qpc100ns() - qpcEpoch100ns) * kRate / 10000000LL - offsetFrames - outFrames);
                bool disc = (fl & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
                if (validEnd > 0 && !disc && std::llabs(idx - validEnd) <= snap) idx = validEnd;  // continuous
                else if (validEnd > 0) resyncs++;
                Write(idx, out, outFrames);
                cap->ReleaseBuffer(frames);
                if (FAILED(cap->GetNextPacketSize(&n))) goto loopEnd;
            }
        }
    loopEnd:;
    }

done:
    if (WaitForSingleObject(ready, 0) != WAIT_OBJECT_0) SetEvent(ready);
    if (started && ac) ac->Stop();
    if (mix) CoTaskMemFree(mix);
    SafeRelease(cap);
    SafeRelease(ac);
    SafeRelease(dev);
    SafeRelease(en);
    CoUninitialize();
}

// ------------------------------------------------------------------ Pipe

struct AudioEngine::Pipe {
    std::wstring name;
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE stop = nullptr;
    HANDLE dataEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread th;
    std::mutex mu;
    std::vector<float> pending;
    std::atomic<long long> lastPush{0}, lastWriteDone{0}, written{0};  // for troubleshooting
    std::atomic<bool> writing{false};

    ~Pipe() { if (dataEv) CloseHandle(dataEv); }

    void Push(const float* d, size_t n) {
        {
            std::lock_guard<std::mutex> lk(mu);
            const size_t cap = (size_t)AudioEngine::kRate * 2 * 60;  // keep at most 60 s if FFmpeg isn't reading
            if (pending.size() + n > cap) pending.erase(pending.begin(), pending.begin() + std::min(pending.size(), n));
            pending.insert(pending.end(), d, d + n);
        }
        lastPush = Qpc100ns();
        SetEvent(dataEv);
    }

    void Main() {
        BoostAudioThread(false);
        HANDLE ioEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // Wait for FFmpeg to open this pipe.
        OVERLAPPED ov{};
        ov.hEvent = ioEv;
        bool connected = false;
        if (!ConnectNamedPipe(h, &ov)) {
            DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) connected = true;
            else if (e == ERROR_IO_PENDING) {
                HANDLE hs[2] = {ioEv, stop};
                if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0) connected = true;
                else {
                    CancelIoEx(h, &ov);
                    DWORD d;
                    GetOverlappedResult(h, &ov, &d, TRUE);
                }
            }
        } else connected = true;
        std::vector<float> chunk;
        while (connected) {
            HANDLE hs[2] = {dataEv, stop};
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0) break;
            {
                std::lock_guard<std::mutex> lk(mu);
                chunk.swap(pending);
                pending.clear();
            }
            if (!chunk.empty()) {
                writing = true;
                bool okw = PipeWrite(h, ioEv, stop, chunk.data(), chunk.size() * sizeof(float));
                writing = false;
                if (!okw) break;
                written += (long long)chunk.size();
                lastWriteDone = Qpc100ns();
            }
            chunk.clear();
        }
        CloseHandle(ioEv);
    }
};

// ------------------------------------------------------------------ Engine

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { Stop(); }

bool AudioEngine::Start(const Config& cfg, std::vector<std::wstring>& pipeNames, std::wstring& err) {
    static std::atomic<int> counter{0};
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    long long wall = WallClockUs(), q = Qpc100ns();
    qpcEpoch100ns_ = q - (wall - cfg.epochUs) * 10;
    offsetFrames_ = (long long)cfg.offsetMs * kRate / 1000;
    hasMic_ = cfg.mic;
    separate_ = cfg.separateTracks && cfg.mic && !cfg.outputs.empty();

    // Pipes first, so FFmpeg can connect as soon as it starts.
    int np = separate_ ? 3 : 1;
    int c = counter++;
    pipeNames.clear();
    for (int i = 0; i < np; i++) {
        auto p = std::make_unique<Pipe>();
        p->name = L"\\\\.\\pipe\\krec_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(c) + L"_" + std::to_wstring(i);
        p->stop = stop_;
        p->h = CreateNamedPipeW(p->name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 18, 1 << 18, 0, nullptr);  // small: our own queue holds the backlog
        if (p->h == INVALID_HANDLE_VALUE) {
            err = L"내부 통로(파이프)를 만들 수 없어요";
            Stop();
            return false;
        }
        pipeNames.push_back(p->name);
        pipes_.push_back(std::move(p));
    }
    for (auto& p : pipes_) p->th = std::thread(&Pipe::Main, p.get());

    auto addSource = [&](bool mic, const std::wstring& id) -> bool {
        auto s = std::make_unique<Source>();
        s->isMic = mic;
        s->id = id;
        s->qpcEpoch100ns = qpcEpoch100ns_;
        s->offsetFrames = offsetFrames_;
        s->stop = stop_;
        s->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        s->th = std::thread(&Source::Main, s.get());
        WaitForSingleObject(s->ready, INFINITE);
        bool ok = s->ok;
        if (!ok) err = s->err;
        sources_.push_back(std::move(s));
        return ok;
    };
    for (auto& id : cfg.outputs)
        if (!addSource(false, id)) { Stop(); return false; }
    if (cfg.mic && !addSource(true, cfg.micId)) { Stop(); return false; }

    mixer_ = std::thread(&AudioEngine::MixerMain, this);
    return true;
}

void AudioEngine::MixerMain() {
    BoostAudioThread(false);
    const long long delay = kRate / 10;  // mix 100 ms behind real time so every device's packets have arrived
    long long pos = 0;
    std::vector<float> sys, mic, mixed, t1, t2;
    while (WaitForSingleObject(stop_, 10) == WAIT_TIMEOUT) {
        long long now = (Qpc100ns() - qpcEpoch100ns_) * kRate / 10000000LL - offsetFrames_;
        long long target = now + offsetFrames_ - delay;
        // A device that is delivering sound but running late (busy computer) is waited for, up to 1 s,
        // instead of mixing silence in its place and throwing its sound away when it arrives.
        // (A device that sends nothing at all — nothing playing — is not waited for.)
        static const bool noWait = GetEnvironmentVariableW(L"KREC_DEBUG_NOWAIT", nullptr, 0) > 0;  // test hook: old behaviour
        for (auto& s : sources_) {
            if (noWait) break;
            long long end, arrival;
            s->Snapshot(end, arrival);
            if ((arrival >= 0 && now - arrival < kRate / 2) || (arrival < 0 && now < kRate)) target = std::min(target, end);
        }
        target = std::max(target, now + offsetFrames_ - kRate);
        while (target > pos) {
            long long n = std::min<long long>(target - pos, kRate / 2);
            size_t ns = (size_t)n * 2;
            sys.assign(ns, 0.0f);
            mic.assign(ns, 0.0f);
            for (auto& s : sources_) s->ReadAndClear(pos, n, s->isMic ? mic.data() : sys.data());
            float sv = sysVol.load(), mv = micOn.load() ? micVol.load() : 0.0f;
            mixed.resize(ns);
            for (size_t i = 0; i < ns; i++) mixed[i] = std::clamp(sys[i] * sv + mic[i] * mv, -1.0f, 1.0f);
            pipes_[0]->Push(mixed.data(), ns);
            if (separate_) {
                t1.resize(ns);
                t2.resize(ns);
                for (size_t i = 0; i < ns; i++) {
                    t1[i] = std::clamp(sys[i] * sv, -1.0f, 1.0f);
                    t2[i] = std::clamp(mic[i] * mv, -1.0f, 1.0f);
                }
                pipes_[1]->Push(t1.data(), ns);
                pipes_[2]->Push(t2.data(), ns);
            }
            pos += n;
        }
    }
}

std::string AudioEngine::PipeReport() {
    std::string r;
    long long now = Qpc100ns();
    for (size_t i = 0; i < pipes_.size(); i++) {
        auto& p = *pipes_[i];
        size_t pend;
        {
            std::lock_guard<std::mutex> lk(p.mu);
            pend = p.pending.size();
        }
        char b[256];
        snprintf(b, sizeof(b), "%spipe%zu: waiting-to-send %.1f s, last mixed %lld ms ago, last delivered %lld ms ago, %s, delivered %.1f s",
                 i ? "; " : "", i, pend / 2.0 / kRate, p.lastPush ? (now - p.lastPush) / 10000 : -1LL,
                 p.lastWriteDone ? (now - p.lastWriteDone) / 10000 : -1LL, p.writing ? "FFmpeg not taking sound" : "idle",
                 p.written / 2.0 / kRate);
        r += b;
    }
    return r;
}

std::string AudioEngine::ProblemReport() {
    std::string r;
    for (size_t i = 0; i < sources_.size(); i++) {
        auto& s = *sources_[i];
        std::lock_guard<std::mutex> lk(s.mu);
        long long lateMs = s.lateFrames * 1000 / kRate, maxLateMs = s.maxLateFrames * 1000 / kRate;
        long long gapMs = s.maxLoopGap100ns / 10000;
        if (lateMs < 20 && gapMs < 200 && s.resyncs < 20) continue;
        char b[256];
        snprintf(b, sizeof(b), "%s%s: lost(late) %lld ms (worst %lld ms late), longest capture pause %lld ms, jumps %lld, packets %lld",
                 r.empty() ? "" : "; ", s.isMic ? "mic" : ("output" + std::to_string(i)).c_str(), lateMs, maxLateMs, gapMs, s.resyncs, s.packets);
        r += b;
    }
    return r;
}

void AudioEngine::Stop() {
    if (stop_) SetEvent(stop_);
    if (mixer_.joinable()) mixer_.join();
    for (auto& s : sources_) {
        if (s->th.joinable()) s->th.join();
        if (s->ready) CloseHandle(s->ready);
    }
    sources_.clear();
    for (auto& p : pipes_) {
        if (p->th.joinable()) p->th.join();
        if (p->h != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(p->h);
            CloseHandle(p->h);
        }
    }
    pipes_.clear();
    if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
}
