#pragma once
#include "util.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <functional>

// One segment of the edit sequence, as the player needs it.
struct PlayPiece {
    std::wstring path;
    double srcStart = 0, srcEnd = 0;  // seconds in the source
    double seqStart = 0;              // where it begins on the timeline
    int audioTracks = 0;              // >= 3: recorder editing tracks (a:1 computer, a:2 mic)
    double fps = 30;
    int width = 0, height = 0;
    float sysVol = 1, micVol = 1;     // live volumes for this piece
    bool skip = false;                // "남긴 구간만 재생": a run never continues into a skipped piece
    double seqEnd() const { return seqStart + (srcEnd - srcStart); }
};

// Preview player: FFmpeg decodes a small picture plus the separate sound tracks; we mix the
// tracks ourselves with the live volumes and play them through Windows audio.
// Messages posted to the notify window:
//   msgFrame      : a new picture is ready (wParam unused) — call CopyFrame
//   msgRunEnd     : playback reached the end of a continuous run; lParam = (LPARAM)(double*) seq time where it stopped
class Player {
public:
    Player(HWND notify, UINT msgFrame, UINT msgRunEnd);
    ~Player();

    void SetPieces(const std::vector<PlayPiece>& pieces);   // whole sequence (volumes included)
    void SetVolumes(const std::vector<std::pair<float, float>>& vols);  // per piece, live

    // Starts playing from piece `idx` at sequence time t (plays until the end of the contiguous run).
    void PlayFrom(int idx, double t);
    void Stop();
    // Shows a single still picture of piece `idx` at sequence time t (for seeking while paused).
    void ShowStill(int idx, double t);

    bool Playing() const { return playing_; }
    // True when the current run has reached its end (used to ignore stale msgRunEnd messages).
    bool RunDone() const;
    double Position() const;  // sequence seconds

    // Latest picture (BGRA). Returns false if none.
    bool CopyFrame(std::vector<uint32_t>& px, int& w, int& h);

    struct Session;
private:
    void StillWorker();
    HWND notify_;
    UINT msgFrame_, msgRunEnd_;
    mutable std::mutex mu_;
    std::vector<PlayPiece> pieces_;
    std::unique_ptr<Session> sess_;
    std::atomic<bool> playing_{false};
    std::atomic<double> pos_{0};

    // current picture
    std::mutex frameMu_;
    std::vector<uint32_t> frame_;
    int fw_ = 0, fh_ = 0;

    // still requests (coalesced)
    std::thread stillTh_;
    std::mutex stillMu_;
    HANDLE stillEv_ = nullptr;
    bool stillQuit_ = false;
    bool stillPending_ = false;
    PlayPiece stillPiece_;
    double stillT_ = 0;

    friend struct Session;
    void PublishFrame(const uint32_t* px, int w, int h);
};

// Preview picture size for a source (at most 960 wide, even numbers).
void PreviewSize(int srcW, int srcH, int& w, int& h);
