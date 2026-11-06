#pragma once
#include "util.h"
#include <functional>
#include <atomic>

// One input video file.
struct MediaInfo {
    std::wstring path;
    double duration = 0;
    int width = 0, height = 0;
    double fps = 30;
    std::wstring vcodec;
    int audioTracks = 0;                 // 3 = recorder's "editing tracks" (mix, computer, mic)
    std::vector<double> keyframes;       // seconds, sorted
    bool editTracks() const { return audioTracks >= 3; }
    double KeyframeAtOrBefore(double t) const;
};
bool ProbeMedia(const std::wstring& path, MediaInfo& out, std::wstring& err);

// A piece of the edit sequence (the timeline is the list of segments in order).
struct Segment {
    int src = 0;              // index into the sources
    double start = 0, end = 0;  // seconds in the source
    bool keep = true;
    std::wstring name;        // typed by the user; empty by default
    bool customVol = false;   // this segment has its own volumes
    int sysVol = 100, micVol = 100;  // used when customVol
    double length() const { return end - start; }
};

struct Volumes {
    int sys = 100, mic = 100;  // percent; for single-track files `sys` is the overall volume
};

enum class CutMode { Fast, Precise };
enum class SaveMode { OneFile, PerSegment };

struct ExportJob {
    std::vector<MediaInfo> sources;
    std::vector<Segment> segments;  // full sequence; only `keep` ones are exported
    Volumes global;
    CutMode cut = CutMode::Fast;
    SaveMode save = SaveMode::OneFile;
    bool keepTracks = true;          // keep the 3 editing tracks in the output (with volumes applied)
    std::wstring outPath;            // OneFile: the file; PerSegment: base path (numbers are added)
    int quality = 1;                 // precise mode: 0 보통, 1 높음, 2 최고
};

// Effective [start, end) used for a segment in fast mode: starts move back to a keyframe and the
// previous segment of the same source then ends there too, so nothing is doubled or lost.
struct EffRange { double start, end; };
std::vector<EffRange> EffectiveRanges(const std::vector<MediaInfo>& src, const std::vector<Segment>& seg, CutMode cut);

// Start time of each kept segment in the finished video (for the chapter list).
std::vector<double> OutputStartTimes(const std::vector<MediaInfo>& src, const std::vector<Segment>& seg, CutMode cut);

// Fast mode needs all sources to have matching video (codec/size); otherwise precise mode is required.
bool CanUseFast(const std::vector<MediaInfo>& src, const std::vector<Segment>& seg, std::wstring& why);

// Runs the export. progress(0..1) is called from the worker thread. Returns written files.
bool RunExport(const ExportJob& job, std::function<void(double)> progress, std::atomic<bool>& cancel,
               std::vector<std::wstring>& written, std::wstring& err);
