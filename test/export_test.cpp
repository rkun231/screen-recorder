// Test harness: probe + export (run under wine).
#include "../src/editor/media.h"
int wmain(int argc, wchar_t** argv) {
    // usage: export_test out_dir file1 [file2]
    std::vector<MediaInfo> src;
    for (int i = 2; i < argc; i++) {
        MediaInfo m; std::wstring err;
        if (!ProbeMedia(argv[i], m, err)) { wprintf(L"probe fail %ls\n", err.c_str()); return 1; }
        wprintf(L"%ls dur=%.3f %dx%d fps=%.2f codec=%ls audio=%d keyframes=%zu first=[", FileNameOf(m.path).c_str(), m.duration, m.width, m.height, m.fps, m.vcodec.c_str(), m.audioTracks, m.keyframes.size());
        for (size_t k = 0; k < m.keyframes.size() && k < 5; k++) wprintf(L"%.3f ", m.keyframes[k]);
        wprintf(L"]\n");
        src.push_back(m);
    }
    std::wstring outDir = argv[1];
    auto run = [&](const wchar_t* name, ExportJob job) {
        std::atomic<bool> cancel{false};
        std::vector<std::wstring> written; std::wstring err;
        double last = 0;
        bool ok = RunExport(job, [&](double p){ last = p; }, cancel, written, err);
        wprintf(L"[%ls] ok=%d progress=%.2f files=%zu %ls\n", name, ok, last, written.size(), ok ? L"" : err.c_str());
        for (auto& w : written) wprintf(L"   -> %ls\n", w.c_str());
    };
    ExportJob base; base.sources = src;
    // timeline: file0 split at 2.5 and 5.0 ; middle discarded ; file1 appended (if any)
    Segment a{0, 0, 2.5}, b{0, 2.5, 5.0}, c{0, 5.0, src[0].duration};
    b.keep = false; a.name = L"1팀"; c.name = L"2팀";
    c.customVol = true; c.sysVol = 50; c.micVol = 200;
    base.segments = {a, b, c};
    if (src.size() > 1) { Segment d{1, 0, src[1].duration}; d.name = L"3팀"; base.segments.push_back(d); }
    base.global.sys = 100; base.global.mic = 100;
    auto st = OutputStartTimes(src, base.segments, CutMode::Fast);
    for (size_t i = 0; i < st.size(); i++) if (st[i] >= 0) wprintf(L"chapter %ls %ls\n", ChapterTime(st[i]).c_str(), base.segments[i].name.c_str());
    std::wstring why; wprintf(L"canFast=%d %ls\n", CanUseFast(src, base.segments, why), why.c_str());
    ExportJob j1 = base; j1.cut = CutMode::Fast; j1.save = SaveMode::OneFile; j1.outPath = outDir + L"\\fast_one.mp4";
    if (src.size() == 1 || CanUseFast(src, base.segments, why)) run(L"fast one", j1);
    ExportJob j2 = base; j2.cut = CutMode::Fast; j2.save = SaveMode::PerSegment; j2.outPath = outDir + L"\\fast_split.mp4";
    run(L"fast split", j2);
    ExportJob j3 = base; j3.cut = CutMode::Precise; j3.save = SaveMode::OneFile; j3.outPath = outDir + L"\\precise_one.mp4";
    run(L"precise one", j3);
    ExportJob j4 = base; j4.cut = CutMode::Precise; j4.save = SaveMode::OneFile; j4.keepTracks = false; j4.outPath = outDir + L"\\precise_mix.mp4";
    run(L"precise mixonly", j4);
    return 0;
}
