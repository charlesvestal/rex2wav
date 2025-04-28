#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include "REX.h"

using namespace REX;

// PPQ resolution (pulses per quarter‐note)
static constexpr double kPPQ = 15360.0;

// Clamp helper (C++11)
static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Write a mono/stereo 16-bit PCM WAV
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
    uint16_t audioFmt = 1;
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

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <input.rx2> <output_prefix>\n";
        return 1;
    }

    std::string inPath = argv[1];
    std::string prefix = argv[2];

    // 1) Init REX
    REXError err = REXInitializeDLL();
    if (err != kREXError_NoError) {
        std::cerr << "Failed to init REX DLL: " << err << "\n";
        return 1;
    }

    // 2) Read file into memory
    std::ifstream file(inPath, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Cannot open: " << inPath << "\n";
        REXUninitializeDLL();
        return 1;
    }
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);
    file.read(buffer.data(), size);

    // 3) Create REX object
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

    // 4) Query global info
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
    double tempoBPM = info.fTempo / 1000.0;

    std::cout << "Loaded " << sliceCount
              << " slices @ " << sampleRate
              << " Hz, " << channels << " channel(s)\n";

    // 5) Open report file
    std::ofstream report(prefix + "_info.txt");
    if (!report) {
        std::cerr << "❌ Cannot write report file\n";
    } else {
        // total loop length (seconds) = PPQlength * seconds-per-pulse
        double secPerPulse = 60.0 / (kPPQ * tempoBPM);
        double loopSeconds = info.fPPQLength * secPerPulse;

        report << "Total loop length: "
               << loopSeconds << " seconds\n\n";

        report << "Slice\tPPQ offset\tTime offset (s)\tDuration (s)\n";
    }

    // 6) Extract each slice
    for (int i = 0; i < sliceCount; ++i) {
        // get slice info
        REXSliceInfo sInfo;
        err = REXGetSliceInfo(rex, i, sizeof(sInfo), &sInfo);
        if (err != kREXError_NoError) {
            std::cerr << "REXGetSliceInfo("<<i<<") failed: "<<err<<"\n";
            continue;
        }

        int frames = sInfo.fSampleLength;
        double durationSec = double(frames) / sampleRate;
        double offsetSec   = sInfo.fPPQPos * (60.0 / (kPPQ * tempoBPM));

        // render slice
        std::vector<float> bufL(frames),
                            bufR(channels==2 ? frames : 0);
        float* outputs[2] = { bufL.data(),
                             channels==2 ? bufR.data() : nullptr };
        err = REXRenderSlice(rex, i, frames, outputs);
        if (err != kREXError_NoError) {
            std::cerr << "REXRenderSlice("<<i<<") failed: "<<err<<"\n";
            continue;
        }

        // convert to PCM
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

        // write WAV
        std::string wavName = prefix + "_slice_" + std::to_string(i) + ".wav";
        if (writeWav(wavName, pcm, sampleRate, channels))
            std::cout << "Wrote " << wavName << "\n";
        else
            std::cerr << "Failed writing " << wavName << "\n";

        // append to report
        if (report) {
            report << i << "\t"
                   << sInfo.fPPQPos << "\t\t"
                   << offsetSec << "\t\t"
                   << durationSec << "\n";
        }
    }

    // 7) Cleanup
    if (report) report.close();
    REXDelete(&rex);
    REXUninitializeDLL();
    return 0;
}