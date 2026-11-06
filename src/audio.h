#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>

struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

// Active playback devices (eRender) or microphones (eCapture).
std::vector<AudioDevice> EnumOutputDevices();
std::vector<AudioDevice> EnumInputDevices();

// Wall clock in microseconds since 1970-01-01 (same base as FFmpeg's av_gettime()).
long long WallClockUs();

// Records several sound sources at once and streams them to FFmpeg through named pipes,
// all as 48 kHz stereo float:
//   pipe 0: everything mixed (computer sound × sysVol + microphone × micVol)
//   pipe 1: computer sound only   (only when separate tracks are requested)
//   pipe 2: microphone only       (only when separate tracks are requested)
// Sample 0 of every pipe corresponds to wall-clock time `epochUs`, so the audio lines up
// with video whose timestamps are offset against the same epoch.
class AudioEngine {
public:
    static constexpr int kRate = 48000;

    struct Config {
        std::vector<std::wstring> outputs;  // playback devices to record ("" = Windows default)
        bool mic = false;
        std::wstring micId;                 // "" = Windows default microphone
        bool separateTracks = false;
        long long epochUs = 0;
        int offsetMs = 0;                   // + makes the sound earlier
    };

    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    // Opens all devices and creates the pipes. `pipeNames` gets one name per pipe to pass to FFmpeg.
    bool Start(const Config& cfg, std::vector<std::wstring>& pipeNames, std::wstring& err);
    void Stop();
    // One line describing sound problems seen so far (empty when everything was fine).
    std::string ProblemReport();
    // State of the pipes to FFmpeg (is FFmpeg still taking the sound?), for troubleshooting.
    std::string PipeReport();

    // Live controls (safe to change while recording).
    std::atomic<float> sysVol{1.0f};
    std::atomic<float> micVol{1.0f};
    std::atomic<bool> micOn{true};

    struct Source;  // one capture device
    struct Pipe;    // one output pipe

private:
    void MixerMain();
    std::vector<std::unique_ptr<Source>> sources_;
    std::vector<std::unique_ptr<Pipe>> pipes_;
    std::thread mixer_;
    HANDLE stop_ = nullptr;
    long long qpcEpoch100ns_ = 0;  // epoch expressed on the QPC clock, in 100 ns units
    long long offsetFrames_ = 0;
    bool separate_ = false;
    bool hasMic_ = false;
};
