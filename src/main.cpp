#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include "REX.h"

using namespace REX;

// Simple clamp for floats (C++11)
static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Write a mono or stereo 16-bit PCM WAV
bool writeWav(const std::string& path,
              const std::vector<int16_t>& pcm,
              int sampleRate,
              int channels)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    uint32_t dataBytes   = uint32_t(pcm.size()) * sizeof(int16_t);
    uint32_t chunkSize   = 36 + dataBytes;
    uint16_t bitsPerSamp = 16;
    uint16_t blockAlign  = channels * bitsPerSamp/8;
    uint32_t byteRate    = sampleRate * blockAlign;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&chunkSize), 4);
    out.write("WAVE", 4);

    out.write("fmt ", 4);
    uint32_t fmtSize = 16;
    uint16_t audioFmt = 1;  // PCM
    out.write(reinterpret_cast<const char*>(&fmtSize),    4);
    out.write(reinterpret_cast<const char*>(&audioFmt),   2);
    out.write(reinterpret_cast<const char*>(&channels),   2);
    out.write(reinterpret_cast<const char*>(&sampleRate), 4);
    out.write(reinterpret_cast<const char*>(&byteRate),   4);
    out.write(reinterpret_cast<const char*>(&blockAlign), 2);
    out.write(reinterpret_cast<const char*>(&bitsPerSamp),2);

    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataBytes), 4);
    out.write(reinterpret_cast<const char*>(pcm.data()), dataBytes);

    return out.good();
}

static std::string stripExtension(const std::string& name) {
    auto pos = name.find_last_of('.');
    return pos == std::string::npos ? name
                                    : name.substr(0, pos);
}

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <input.rx2>\n";
        return 1;
    }

    std::string inPath = argv[1];
    // Derive basename
    auto slash = inPath.find_last_of("/\\");
    std::string filename = (slash == std::string::npos ? inPath : inPath.substr(slash + 1));
    std::string prefix = stripExtension(filename);

    // Create slices/ directory
    const char* dir = "slices";
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        std::perror("mkdir slices");
        return 1;
    }

    // Initialize REX framework
    REXError err = REXInitializeDLL();
    if (err != kREXError_NoError) {
        std::cerr << "Failed to init REX DLL: " << err << "\n";
        return 1;
    }

    // Load file into memory
    std::ifstream file(inPath, std::ios::binary|std::ios::ate);
    if (!file) {
        std::cerr << "Cannot open: " << inPath << "\n";
        REXUninitializeDLL();
        return 1;
    }
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);
    file.read(buffer.data(), size);

    // Create REX object
    REXHandle rex = nullptr;
    err = REXCreate(&rex,
                    buffer.data(),
                    (REX_int32_t)size,
                    nullptr,
                    nullptr);
    if (err != kREXError_NoError) {
        std::cerr << "REXCreate failed: " << err << "\n";
        REXUninitializeDLL();
        return 1;
    }

    // Get global info
    REXInfo info;
    err = REXGetInfo(rex, sizeof(info), &info);
    if (err != kREXError_NoError) {
        std::cerr << "REXGetInfo failed: " << err << "\n";
        REXDelete(&rex);
        REXUninitializeDLL();
        return 1;
    }

    int sampleRate = info.fSampleRate;
    int channels   = info.fChannels;
    int sliceCount = info.fSliceCount;
    double tempo   = info.fTempo / 1000.0; // BPM
    const double kPPQ = 15360.0;
    double secPerPulse = 60.0 / (kPPQ * tempo);
    double loopSec = info.fPPQLength * secPerPulse;

    std::cout << "Loaded " << sliceCount
              << " slices @ " << sampleRate
              << " Hz, " << channels << " channel(s)\n";

    // Open CSV report
    std::string reportPath = std::string(dir) + "/" + prefix + "_info.csv";
    std::ofstream report(reportPath);
    if (!report) {
        std::cerr << "Cannot write report: " << reportPath << "\n";
    } else {
        report << "Slice,Duration,Total\n";
    }

    double cumulative = 0.0;
    // Extract each slice
    for (int i = 0; i < sliceCount; ++i) {
        // Get slice info
        REXSliceInfo sInfo;
        err = REXGetSliceInfo(rex, i, sizeof(sInfo), &sInfo);
        if (err != kREXError_NoError) {
            std::cerr << "REXGetSliceInfo(" << i << ") failed: " << err << "\n";
            continue;
        }
        int frames = sInfo.fSampleLength;
        double duration = double(frames) / sampleRate;
        double offset   = cumulative;

        // Render slice
        std::vector<float> bufL(frames), bufR(channels==2 ? frames : 0);
        float* outputs[2] = { bufL.data(), channels==2 ? bufR.data() : nullptr };
        err = REXRenderSlice(rex, i, frames, outputs);
        if (err != kREXError_NoError) {
            std::cerr << "REXRenderSlice(" << i << ") failed: " << err << "\n";
            continue;
        }

        // Convert to PCM
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

        // Write WAV
        std::string wavPath = std::string(dir) + "/" + prefix + "_slice_" + std::to_string(i) + ".wav";
        if (writeWav(wavPath, pcm, sampleRate, channels))
            std::cout << "Wrote " << wavPath << "\n";
        else
            std::cerr << "Failed writing " << wavPath << "\n";

        // Append CSV row: slice, offset, duration
        if (report) {
            report << i << "," << offset << "," << duration << "\n";
        }
        cumulative += duration;
    }

    // Final total line
    if (report) {
        report << "Loop,," << loopSec << "\n";
        report.close();
    }

    // Cleanup
    REXDelete(&rex);
    REXUninitializeDLL();
    return 0;
}