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

// write a 16-bit PCM file (.wav or .aif)
static bool writeWav(const std::string& path,
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
    uint16_t audioFmt = 1; // PCM
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

// strip extension
static std::string stripExt(const std::string& s) {
    auto p = s.find_last_of('.');
    return p == std::string::npos ? s : s.substr(0, p);
}

// append 32-bit BE
static void push_u32_be(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back((v>>24)&0xFF);
    buf.push_back((v>>16)&0xFF);
    buf.push_back((v>>8)&0xFF);
    buf.push_back(v&0xFF);
}
// append 16-bit BE
static void push_u16_be(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back((v>>8)&0xFF);
    buf.push_back(v&0xFF);
}

// generate an Octatrack .ot file
static bool generateOT(const std::string& outDir,
                       const std::string& base,
                       int sampleRate,
                       int tempoBpm,
                       const std::vector<uint32_t>& starts,
                       const std::vector<uint32_t>& lengths)
{
    std::vector<uint8_t> d = {
        'F','O','R','M', 0,0,0,0,
        'D','P','S','1', 'S','M','P','A', 0,0,0,0,0, 0x02,0x00
    };
    uint32_t total = 0;
    for (auto l : lengths) total += l;
    uint32_t bars = uint32_t((double(tempoBpm)*total)/(double(sampleRate)*60.0) + 0.5)*25;
    uint32_t tempoParam = tempoBpm * 6 * 4;
    push_u32_be(d, tempoParam);
    push_u32_be(d, bars);
    push_u32_be(d, bars);
    push_u32_be(d, 0);
    push_u32_be(d, 0);
    push_u16_be(d, 48);
    d.push_back(255);
    push_u32_be(d, 0);
    push_u32_be(d, total);
    push_u32_be(d, 0);
    for (int i = 0; i < 64; ++i) {
        if (i < (int)lengths.size()) {
            push_u32_be(d, starts[i]);
            push_u32_be(d, starts[i] + lengths[i]);
            push_u32_be(d, lengths[i]);
        } else {
            push_u32_be(d, 0);
            push_u32_be(d, 0);
            push_u32_be(d, 0);
        }
    }
    push_u32_be(d, uint32_t(lengths.size()));
    uint16_t sum = 0;
    for (size_t i = 16; i < d.size(); ++i) sum += d[i];
    push_u16_be(d, sum);
    std::string path = outDir + "/" + base + ".ot";
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(d.data()), d.size());
    return out.good();
}

int main(int argc, char* argv[]) {
    bool single = false, octa = false;
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0] << " <input.rx2> [--single-file|--octa]\n";
        return 1;
    }
    std::string inPath = argv[1];
    if (argc == 3) {
        if (!std::strcmp(argv[2], "--single-file")) single = true;
        else if (!std::strcmp(argv[2], "--octa")) octa = true;
        else { std::cerr << "Unknown option " << argv[2] << "\n"; return 1; }
    }

    // derive base and make slices/
    auto p = inPath.find_last_of("/\\");
    std::string fname = p == std::string::npos ? inPath : inPath.substr(p + 1);
    std::string base = stripExt(fname);
    if (mkdir("slices", 0755) && errno != EEXIST) { perror("mkdir"); return 1; }

    // init REX
    if (REXInitializeDLL() != kREXError_NoError) { std::cerr << "Init failed\n"; return 1; }

    // load file
    std::ifstream fin(inPath, std::ios::binary | std::ios::ate);
    if (!fin) { std::cerr << "Cannot open " << inPath << "\n"; REXUninitializeDLL(); return 1; }
    auto sz = fin.tellg(); fin.seekg(0);
    std::vector<char> buf(sz);
    fin.read(buf.data(), sz);

    // create REX object
    REXHandle rex = nullptr;
    if (REXCreate(&rex, buf.data(), REX_int32_t(sz), nullptr, nullptr) != kREXError_NoError) {
        std::cerr << "Create failed\n"; REXUninitializeDLL(); return 1;
    }

    // get info
    REXInfo info;
    if (REXGetInfo(rex, sizeof(info), &info) != kREXError_NoError) {
        std::cerr << "GetInfo failed\n";
        REXDelete(&rex);
        REXUninitializeDLL();
        return 1;
    }
    int sampleRate = info.fSampleRate;
    int channels = info.fChannels;
    int sliceCount = info.fSliceCount;
    double tempo = info.fTempo / 1000.0;

    // gather starts & lengths
    std::vector<uint32_t> starts(sliceCount), lengths(sliceCount);
    uint32_t cumFrames = 0;
    for (int i = 0; i < sliceCount; ++i) {
        REXSliceInfo si;
        REXGetSliceInfo(rex, i, sizeof(si), &si);
        starts[i] = cumFrames;
        lengths[i] = uint32_t(si.fSampleLength);
        cumFrames += si.fSampleLength;
    }

    // always write CSV
    std::ofstream rep("slices/" + base + "_info.csv");
    rep << "Slice,StartSec,LengthSec,TotalSec\n";
    double totalSec = double(cumFrames) / sampleRate;
    double runningSec = 0.0;
    for (int i = 0; i < sliceCount; ++i) {
        double startSec = double(starts[i]) / sampleRate;
        double lenSec = double(lengths[i]) / sampleRate;
        rep << i << "," << startSec << "," << lenSec << "," << runningSec << "\n";
        runningSec += lenSec;
    }
    rep << "Loop,,," << totalSec << "\n";
    rep.close();

    // single-file: full loop
    if (single || octa) {
        int64_t totalF = cumFrames;
        std::vector<float> bufL(totalF), bufR(channels==2?totalF:0);
        float* outs[2] = {bufL.data(), channels==2?bufR.data():nullptr};
        REXStartPreview(rex);
        int64_t done = 0, chunk = 65536;
        while (done < totalF) {
            int toDo = int(std::min<int64_t>(chunk, totalF - done));
            REXRenderPreviewBatch(rex, toDo, outs);
            outs[0] += toDo;
            if (channels==2) outs[1] += toDo;
            done += toDo;
        }
        REXStopPreview(rex);
        std::vector<int16_t> pcm;
        pcm.reserve(totalF * channels);
        for (int64_t i = 0; i < totalF; ++i) {
            float l = clampf(bufL[i], -1.0f, 1.0f);
            pcm.push_back(int16_t(l * 32767));
            if (channels==2) {
                float r = clampf(bufR[i], -1.0f, 1.0f);
                pcm.push_back(int16_t(r * 32767));
            }
        }
        std::string full = "slices/" + base + "_full.aif";
        writeWav(full, pcm, sampleRate, channels);
        std::cout << "Wrote full loop: " << full << "\n";

        if (octa) {
            if (generateOT("slices", base, sampleRate, int(tempo), starts, lengths))
                std::cout << "Wrote Octatrack .ot: slices/" << base << ".ot\n";
            else
                std::cerr << "Failed .ot generation\n";
        }
    }

    // per-slice WAVs if neither flag
    if (!single && !octa) {
        for (int i = 0; i < sliceCount; ++i) {
            int frames = lengths[i];
            std::vector<float> bufL(frames), bufR(channels==2?frames:0);
            float* outs[2] = {bufL.data(), channels==2?bufR.data():nullptr};
            REXRenderSlice(rex, i, frames, outs);
            std::vector<int16_t> pcm;
            pcm.reserve(frames * channels);
            for (int j = 0; j < frames; ++j) {
                float l = clampf(bufL[j], -1.0f, 1.0f);
                pcm.push_back(int16_t(l * 32767));
                if (channels==2) {
                    float r = clampf(bufR[j], -1.0f, 1.0f);
                    pcm.push_back(int16_t(r * 32767));
                }
            }
            std::string outp = "slices/" + base + "_slice_" + std::to_string(i) + ".wav";
            writeWav(outp, pcm, sampleRate, channels);
            std::cout << "Wrote slice: " << outp << "\n";
        }
    }

    // cleanup
    REXDelete(&rex);
    REXUninitializeDLL();
    return 0;
}
