#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <algorithm>    // std::min
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include "REX.h"

using namespace REX;

// clamp helper
static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// write a mono or stereo 16-bit PCM WAV
bool writeWav(const std::string& path,
              const std::vector<int16_t>& pcm,
              int sampleRate,
              int channels)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    uint32_t dataBytes   = uint32_t(pcm.size()) * sizeof(int16_t);
    uint32_t chunkSize   = 36 + dataBytes;
    uint16_t bitsPerSamp = 16;                         // 16 bits/sample
    uint16_t blockAlign  = channels * bitsPerSamp/8;
    uint32_t byteRate    = sampleRate * blockAlign;

    // RIFF header
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&chunkSize), 4);
    out.write("WAVE", 4);

    // fmt subchunk
    out.write("fmt ", 4);
    uint32_t fmtSize = 16;
    uint16_t audioFmt = 1; // PCM
    out.write(reinterpret_cast<const char*>(&fmtSize),    4);
    out.write(reinterpret_cast<const char*>(&audioFmt),   2);
    out.write(reinterpret_cast<const char*>(&channels),   2);
    out.write(reinterpret_cast<const char*>(&sampleRate), 4);
    out.write(reinterpret_cast<const char*>(&byteRate),   4);
    out.write(reinterpret_cast<const char*>(&blockAlign), 2);
    out.write(reinterpret_cast<const char*>(&bitsPerSamp),2);

    // data subchunk
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataBytes), 4);
    out.write(reinterpret_cast<const char*>(pcm.data()), dataBytes);

    return out.good();
}

// strip extension
static std::string stripExt(const std::string& s) {
    auto p = s.find_last_of('.');
    return p == std::string::npos ? s : s.substr(0, p);
}

int main(int argc, char* argv[])
{
    // --- Parse args ---
    if (argc < 2 || argc > 3 ||
        (argc == 3 && std::strcmp(argv[2], "--single-file") != 0)) {
        std::cerr << "Usage: " << argv[0] << " <input.rx2> [--single-file]\n";
        return 1;
    }
    bool single = (argc == 3);
    std::string inPath = argv[1];

    // --- Derive base name and make output dir ---
    auto slash = inPath.find_last_of("/\\");
    std::string fname = (slash == std::string::npos ? inPath : inPath.substr(slash+1));
    std::string base  = stripExt(fname);
    if (mkdir("slices", 0755) && errno != EEXIST) {
        std::perror("mkdir slices");
        return 1;
    }

    // --- Init REX ---
    if (REXInitializeDLL() != kREXError_NoError) {
        std::cerr << "REXInitializeDLL failed\n";
        return 1;
    }

    // --- Load file into memory ---
    std::ifstream fin(inPath, std::ios::binary|std::ios::ate);
    if (!fin) {
        std::cerr << "Cannot open " << inPath << "\n";
        REXUninitializeDLL();
        return 1;
    }
    auto sz = fin.tellg();
    fin.seekg(0);
    std::vector<char> buf(sz);
    fin.read(buf.data(), sz);

    // --- Create REX object ---
    REXHandle rex = nullptr;
    if (REXCreate(&rex, buf.data(), REX_int32_t(sz), nullptr, nullptr) != kREXError_NoError) {
        std::cerr << "REXCreate failed\n";
        REXUninitializeDLL();
        return 1;
    }

    // --- Get global info ---
    REXInfo info;
    if (REXGetInfo(rex, sizeof(info), &info) != kREXError_NoError) {
        std::cerr << "REXGetInfo failed\n";
        REXDelete(&rex);
        REXUninitializeDLL();
        return 1;
    }

    int    sampleRate = info.fSampleRate;
    int    channels   = info.fChannels;
    int    sliceCount = info.fSliceCount;
    double tempo      = info.fTempo / 1000.0;      // BPM
    const double kPPQ = 15360.0;
    double secPerPulse= 60.0 / (kPPQ * tempo);
    double loopSec    = info.fPPQLength * secPerPulse;

    std::cout << "Loaded " << sliceCount
              << " slices @ " << sampleRate
              << " Hz, " << channels << " channel(s)\n";

    // --- Prepare CSV report ---
    std::string csvPath = "slices/" + base + "_info.csv";
    std::ofstream report(csvPath);
    if (!report) {
        std::cerr << "Cannot write report: " << csvPath << "\n";
    } else {
        report << "Slice,Duration,Total\n";
    }

    // --- Gather slice durations ---
    std::vector<double> durations(sliceCount);
    for (int i = 0; i < sliceCount; ++i) {
        REXSliceInfo si;
        REXGetSliceInfo(rex, i, sizeof(si), &si);
        durations[i] = double(si.fSampleLength) / sampleRate;
    }

    // --- Write slice lines to CSV ---
    double cumulative = 0.0;
    if (report) {
        for (int i = 0; i < sliceCount; ++i) {
            report << i << "," << durations[i] << "," << cumulative << "\n";
            cumulative += durations[i];
        }
    }

    // --- Single-file mode? render entire loop via Preview API ---
    if (single) {
        if (REXStartPreview(rex) != kREXError_NoError) {
            std::cerr << "REXStartPreview failed\n";
            // write Loop line, clean up, exit
            if (report) report << "Loop,," << loopSec << "\n";
            report.close();
            REXDelete(&rex);
            REXUninitializeDLL();
            return 1;
        }

        int64_t totalFrames = int64_t(loopSec * sampleRate + 0.5);
        std::vector<float> bufL(totalFrames),
                            bufR(channels==2 ? totalFrames : 0);
        float* outputs[2] = { bufL.data(), channels==2 ? bufR.data() : nullptr };

        const int chunk = 65536;
        int64_t done = 0;
        while (done < totalFrames) {
            int toDo = int(std::min<int64_t>(chunk, totalFrames - done));
            if (REXRenderPreviewBatch(rex, toDo, outputs) != kREXError_NoError) {
                std::cerr << "Preview render error\n";
                break;
            }
            outputs[0] += toDo;
            if (channels==2) outputs[1] += toDo;
            done += toDo;
        }
        REXStopPreview(rex);

        // Convert to PCM
        std::vector<int16_t> pcm;
        pcm.reserve(totalFrames * channels);
        for (int64_t i = 0; i < totalFrames; ++i) {
            float l = clampf(bufL[i], -1.0f, 1.0f);
            pcm.push_back(int16_t(l * 32767));
            if (channels == 2) {
                float r = clampf(bufR[i], -1.0f, 1.0f);
                pcm.push_back(int16_t(r * 32767));
            }
        }

        // Write full-loop WAV
        std::string outFull = "slices/" + base + "_full.wav";
        if (writeWav(outFull, pcm, sampleRate, channels))
            std::cout << "Wrote full loop: " << outFull << "\n";
        else
            std::cerr << "Failed writing " << outFull << "\n";

        if (report) report << "Loop,," << loopSec << "\n";
        report.close();
        REXDelete(&rex);
        REXUninitializeDLL();
        return 0;
    }

    // --- Per-slice mode: render each slice individually ---
    double cursor = 0.0;
    for (int i = 0; i < sliceCount; ++i) {
        int frames = int(durations[i] * sampleRate);
        std::vector<float> bufL(frames),
                            bufR(channels==2 ? frames : 0);
        float* outputs[2] = { bufL.data(), channels==2 ? bufR.data() : nullptr };

        if (REXRenderSlice(rex, i, frames, outputs) != kREXError_NoError) {
            std::cerr << "Slice " << i << " render failed\n";
            cursor += durations[i];
            continue;
        }

        // to PCM
        std::vector<int16_t> pcm;
        pcm.reserve(frames * channels);
        for (int j = 0; j < frames; ++j) {
            float l = clampf(bufL[j], -1.0f, 1.0f);
            pcm.push_back(int16_t(l * 32767));
            if (channels == 2) {
                float r = clampf(bufR[j], -1.0f, 1.0f);
                pcm.push_back(int16_t(r * 32767));
            }
        }

        std::string outSlice = "slices/" + base + "_slice_" + std::to_string(i) + ".wav";
        if (writeWav(outSlice, pcm, sampleRate, channels))
            std::cout << "Wrote " << outSlice << "\n";
        else
            std::cerr << "Failed writing " << outSlice << "\n";

        cursor += durations[i];
    }

    // final Loop line in CSV
    if (report) {
        report << "Loop,," << loopSec << "\n";
        report.close();
    }

    // cleanup
    REXDelete(&rex);
    REXUninitializeDLL();
    return 0;
}