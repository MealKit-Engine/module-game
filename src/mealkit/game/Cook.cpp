#include "mealkit/game/Cook.h"

#include "mealkit/asset/Cooked.h"
#include "mealkit/render/Environment.h"
#include "mealkit/render/TextureCache.h"
#include "mealkit/core/Log.h"
#include "mealkit/game/ProjectSettings.h"
#include "mealkit/platform/Image.h"
#include "mealkit/platform/Pack.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string_view>
#include <thread>
#include <vector>

namespace mealkit::game {

#if MEALKIT_ASSET_IMPORT
namespace {

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
bool startsWith(std::string_view s, std::string_view prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// Files under a folder (any depth), as prefix + relative path with '/'
void walk(const std::string& dir, const std::string& prefix, std::vector<std::string>& out) {
    for (const std::string& f : listFiles(dir)) {
        if (f == ".gitkeep" || endsWith(f, ".autosave") || endsWith(f, ".tmp")) continue;
        out.push_back(prefix + f);
    }
    for (const std::string& d : listDirectories(dir)) walk(dir + "/" + d, prefix + d + "/", out);
}

// Binary kinds: not scanned for references
bool binary(std::string_view p) {
    for (const char* ext : {".glb", ".png", ".jpg", ".jpeg", ".wav", ".ogg", ".hdr", ".otf", ".ttf", ".mkprobe", ".bin", ".cube"})
        if (endsWith(p, ext)) return true;
    return false;
}

// Data the engine opens by fixed name or by folder, always packed (small text and fonts)
bool alwaysPacked(std::string_view p) {
    std::string_view local = p;
    if (startsWith(local, "modules/")) { // modules/<name>/<asset path>
        const size_t slash = local.find('/', 8);
        local = slash == std::string_view::npos ? std::string_view() : local.substr(slash + 1);
    }
    for (const char* ext : {".table", ".tuning", ".bindings", ".events", ".ui", ".fx", ".otf", ".ttf"})
        if (endsWith(local, ext)) return true;
    return startsWith(local, "strings/") || startsWith(local, "fonts/");
}

// Words of a text that could be asset paths
void words(std::string_view text, std::vector<std::string>& out) {
    const auto pathChar = [](unsigned char c) {
        return c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               c == '/' || c == '-' || c == '\\';
    };
    for (size_t i = 0; i < text.size();) {
        while (i < text.size() && !pathChar(static_cast<unsigned char>(text[i]))) ++i;
        const size_t start = i;
        while (i < text.size() && pathChar(static_cast<unsigned char>(text[i]))) ++i;
        if (i - start < 3) continue;
        std::string w(text.substr(start, i - start));
        std::replace(w.begin(), w.end(), '\\', '/');
        if (startsWith(w, "assets/")) w.erase(0, 7);
        if (w.find('.') != std::string::npos) out.push_back(std::move(w));
    }
}

// The space-separated entries of a project setting (folders have no dot, so not words())
std::vector<std::string> entries(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        const size_t space = text.find(' ');
        std::string w(text.substr(0, space));
        std::replace(w.begin(), w.end(), '\\', '/');
        while (!w.empty() && w.back() == '/') w.pop_back();
        if (!w.empty()) out.push_back(std::move(w));
        text = space == std::string_view::npos ? std::string_view() : text.substr(space + 1);
    }
    return out;
}

// A packFolders entry: a relative folder inside the project, not assets/ (that goes by reference) or build output
bool packableFolder(std::string_view folder) {
    if (folder.front() == '/' || folder.find(':') != std::string_view::npos) return false;
    const std::string_view first = folder.substr(0, folder.find('/'));
    if (first == "assets" || first == "build" || first == "export") return false;
    for (std::string_view rest = folder; !rest.empty();) {
        const size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if (part == "." || part == "..") return false;
        rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
    }
    return true;
}

// String literals in game source code ("ui/hud.ui")
void literals(std::string_view text, std::vector<std::string>& out) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') continue;
        const size_t end = text.find_first_of("\"\n", i + 1);
        if (end == std::string_view::npos) break;
        if (text[end] == '"') {
            std::string w(text.substr(i + 1, end - i - 1));
            if (startsWith(w, "assets/")) w.erase(0, 7);
            if (!w.empty()) out.push_back(std::move(w));
        }
        i = end;
    }
}

void sourceFiles(const std::string& dir, std::vector<std::string>& out) {
    if (!directoryExists(dir)) return;
    for (const std::string& f : listFiles(dir))
        if (endsWith(f, ".cpp") || endsWith(f, ".h")) out.push_back(dir + "/" + f);
    for (const std::string& d : listDirectories(dir)) sourceFiles(dir + "/" + d, out);
}

// ---- Browser cook (--cook-web) ----

u32 readU32(const std::string& b, size_t at) {
    return static_cast<u32>(static_cast<u8>(b[at])) | static_cast<u32>(static_cast<u8>(b[at + 1])) << 8 |
           static_cast<u32>(static_cast<u8>(b[at + 2])) << 16 | static_cast<u32>(static_cast<u8>(b[at + 3])) << 24;
}
u16 readU16(const std::string& b, size_t at) {
    return static_cast<u16>(static_cast<u8>(b[at]) | static_cast<u8>(b[at + 1]) << 8);
}
void putU32(std::string& out, u32 v) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}
void putU16(std::string& out, u16 v) {
    out += static_cast<char>(v & 0xFF);
    out += static_cast<char>(v >> 8);
}

// PCM or float WAV → interleaved samples in -1..1. false for other formats (ADPCM, µ-law: packed as they are)
bool decodeWav(const std::string& b, std::vector<float>& samples, u32& channels, u32& rate) {
    if (b.size() < 12 || b.compare(0, 4, "RIFF") != 0 || b.compare(8, 4, "WAVE") != 0) return false;
    u16 format = 0, bits = 0;
    channels = rate = 0;
    size_t dataAt = 0, dataSize = 0;
    for (size_t at = 12; at + 8 <= b.size();) {
        const u32 size = readU32(b, at + 4);
        const size_t body = at + 8;
        if (b.compare(at, 4, "fmt ") == 0 && size >= 16 && body + 16 <= b.size()) {
            format = readU16(b, body);
            channels = readU16(b, body + 2);
            rate = readU32(b, body + 4);
            bits = readU16(b, body + 14);
            if (format == 0xFFFE && size >= 26 && body + 26 <= b.size()) format = readU16(b, body + 24); // extensible: sub format
        } else if (b.compare(at, 4, "data") == 0) {
            dataAt = body;
            dataSize = std::min<size_t>(size, b.size() - body);
        }
        at = body + size + (size & 1);
    }
    if (!dataAt || !channels || channels > 2 || !rate) return false;
    const bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    if (!pcm && !(format == 3 && bits == 32)) return false;
    const size_t bytes = bits / 8, count = dataSize / bytes / channels * channels;
    samples.resize(count);
    const unsigned char* d = reinterpret_cast<const unsigned char*>(b.data() + dataAt);
    for (size_t i = 0; i < count; ++i, d += bytes) {
        float v = 0.0f;
        if (format == 3) std::memcpy(&v, d, 4);
        else if (bits == 8) v = (d[0] - 128) / 128.0f;
        else if (bits == 16) v = static_cast<i16>(d[0] | d[1] << 8) / 32768.0f;
        else if (bits == 24) v = static_cast<i32>(static_cast<u32>(d[0] << 8 | d[1] << 16 | d[2] << 24)) / 2147483648.0f;
        else v = static_cast<i32>(static_cast<u32>(d[0] | d[1] << 8 | d[2] << 16 | d[3] << 24)) / 2147483648.0f;
        samples[i] = v;
    }
    return true;
}

// Integer-ratio downsample with a windowed-sinc low-pass (cut-off a little under the new Nyquist)
std::vector<float> decimate(const std::vector<float>& in, u32 channels, u32 factor) {
    constexpr int kHalf = 24;
    const float cutoff = 0.45f / static_cast<float>(factor);
    float taps[2 * kHalf + 1];
    float sum = 0.0f;
    for (int k = -kHalf; k <= kHalf; ++k) {
        const float x = static_cast<float>(k);
        const float sinc = k == 0 ? 2.0f * cutoff : std::sin(6.2831853f * cutoff * x) / (3.14159265f * x);
        const float window = 0.42f + 0.5f * std::cos(3.14159265f * x / kHalf) + 0.08f * std::cos(6.2831853f * x / kHalf); // Blackman
        sum += taps[k + kHalf] = sinc * window;
    }
    for (float& t : taps) t /= sum;
    const size_t frames = in.size() / channels, outFrames = (frames + factor - 1) / factor;
    std::vector<float> out(outFrames * channels);
    for (size_t o = 0; o < outFrames; ++o)
        for (u32 c = 0; c < channels; ++c) {
            const i64 center = static_cast<i64>(o * factor);
            float acc = 0.0f;
            for (int k = -kHalf; k <= kHalf; ++k) {
                const i64 f = center + k;
                if (f >= 0 && f < static_cast<i64>(frames)) acc += taps[k + kHalf] * in[static_cast<size_t>(f) * channels + c];
            }
            out[o * channels + c] = acc;
        }
    return out;
}

// IMA ADPCM (WAVE_FORMAT_DVI_ADPCM 0x11), the layout miniaudio's WAV reader decodes: per block and channel a 4-byte header
// (first sample, step index), then 4-bit codes, low nibble first, in 4-byte groups per channel (8 samples) when stereo
constexpr int kImaSteps[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                               31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                               130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                               544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                               2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                               9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kImaIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct ImaState {
    int predictor = 0, index = 0;
    u8 encode(int sample) {
        int diff = sample - predictor, step = kImaSteps[index], vp = step >> 3;
        u8 code = 0;
        if (diff < 0) code = 8, diff = -diff;
        if (diff >= step) code |= 4, diff -= step, vp += step;
        step >>= 1;
        if (diff >= step) code |= 2, diff -= step, vp += step;
        step >>= 1;
        if (diff >= step) code |= 1, vp += step;
        predictor = std::clamp(predictor + ((code & 8) ? -vp : vp), -32768, 32767);
        index = std::clamp(index + kImaIndex[code & 7], 0, 88);
        return code;
    }
};

std::string encodeImaWav(const std::vector<float>& samples, u32 channels, u32 rate) {
    constexpr u32 kBlockAlignMono = 512; // bytes per channel and block
    const u32 blockAlign = kBlockAlignMono * channels;
    const u32 perBlock = (blockAlign - 4 * channels) * 8 / (4 * channels) + 1; // frames per block
    const size_t frames = samples.size() / channels;
    std::vector<i16> pcm(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) pcm[i] = static_cast<i16>(std::lround(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
    const auto at = [&](size_t frame, u32 c) { return frame < frames ? pcm[frame * channels + c] : 0; };
    std::string data;
    ImaState state[2];
    for (size_t start = 0; start < frames; start += perBlock) {
        const size_t inBlock = std::min<size_t>(perBlock, frames - start);
        for (u32 c = 0; c < channels; ++c) {
            state[c].predictor = at(start, c); // the header sample is the block's first frame
            putU16(data, static_cast<u16>(static_cast<i16>(state[c].predictor)));
            data += static_cast<char>(state[c].index);
            data += '\0';
        }
        const size_t coded = inBlock - 1, groups = (coded + 7) / 8; // 8 frames per 4-byte group and channel
        for (size_t g = 0; g < groups; ++g)
            for (u32 c = 0; c < channels; ++c)
                for (int k = 0; k < 8; k += 2) {
                    const size_t f = start + 1 + g * 8 + k;
                    const u8 lo = state[c].encode(at(f, c)), hi = state[c].encode(at(f + 1, c));
                    data += static_cast<char>(lo | hi << 4);
                }
    }
    std::string out = "RIFF";
    putU32(out, static_cast<u32>(4 + (8 + 20) + (8 + 4) + 8 + data.size() + (data.size() & 1)));
    out += "WAVEfmt ";
    putU32(out, 20);
    putU16(out, 0x11);
    putU16(out, static_cast<u16>(channels));
    putU32(out, rate);
    putU32(out, static_cast<u32>(static_cast<u64>(rate) * blockAlign / perBlock)); // bytes per second
    putU16(out, static_cast<u16>(blockAlign));
    putU16(out, 4);
    putU16(out, 2);
    putU16(out, static_cast<u16>(perBlock));
    out += "fact";
    putU32(out, 4);
    putU32(out, static_cast<u32>(frames));
    out += "data";
    putU32(out, static_cast<u32>(data.size()));
    out += data;
    if (data.size() & 1) out += '\0';
    return out;
}

// A WAV for the browser: IMA ADPCM, at most 24 kHz. false to keep the original (not plain PCM/float, or not smaller)
constexpr u32 kWebAudioRate = 24000;
bool webWav(const std::string& bytes, std::string& out) {
    std::vector<float> samples;
    u32 channels = 0, rate = 0;
    if (!decodeWav(bytes, samples, channels, rate) || samples.empty()) return false;
    const u32 factor = (rate + kWebAudioRate - 1) / kWebAudioRate;
    if (factor > 1) samples = decimate(samples, channels, factor);
    out = encodeImaWav(samples, channels, rate / factor);
    return out.size() < bytes.size();
}

// PNG "mkSz" chunk (ancillary, private, safe to copy): the picture's size before the browser cook halved it, as two big-endian
// u32 (width, height). render::SpriteTextures reports that size, so sprites sized from their texture keep their size on screen
u32 crc32(const unsigned char* p, size_t n, u32 crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}
void putBe32(std::string& out, u32 v) {
    for (int i = 3; i >= 0; --i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

// Half the pixels per side (2x2 average with alpha weighting, so transparent pixels' colour doesn't bleed into edges), written
// as PNG with the original size in mkSz. tempFile is a scratch path. false to keep the original
bool halfPng(const std::string& file, const std::string& tempFile, std::string& out) {
    Image src;
    if (!loadImage(file, 0, src) || src.width < 2 || src.height < 2) return false;
    Image dst;
    dst.width = (src.width + 1) / 2;
    dst.height = (src.height + 1) / 2;
    dst.rgba.resize(static_cast<size_t>(dst.width) * dst.height * 4);
    for (u32 y = 0; y < dst.height; ++y)
        for (u32 x = 0; x < dst.width; ++x) {
            u32 alpha = 0, n = 0, color[3] = {};
            for (u32 sy = y * 2; sy < std::min(y * 2 + 2, src.height); ++sy)
                for (u32 sx = x * 2; sx < std::min(x * 2 + 2, src.width); ++sx) {
                    const u8* p = &src.rgba[(static_cast<size_t>(sy) * src.width + sx) * 4];
                    for (int c = 0; c < 3; ++c) color[c] += p[c] * p[3];
                    alpha += p[3];
                    ++n;
                }
            u8* q = &dst.rgba[(static_cast<size_t>(y) * dst.width + x) * 4];
            for (int c = 0; c < 3; ++c) q[c] = static_cast<u8>(alpha ? (color[c] + alpha / 2) / alpha : 0);
            q[3] = static_cast<u8>((alpha + n / 2) / n);
        }
    std::string png;
    const bool written = savePng(tempFile, dst) && readTextFile(tempFile, png);
    std::remove(tempFile.c_str());
    if (!written || png.size() < 33 || png.compare(12, 4, "IHDR") != 0) return false;
    std::string chunk;
    putBe32(chunk, 8);
    chunk += "mkSz";
    putBe32(chunk, src.width);
    putBe32(chunk, src.height);
    putBe32(chunk, crc32(reinterpret_cast<const unsigned char*>(chunk.data() + 4), 12) ^ 0xFFFFFFFFu);
    out = png.substr(0, 33) + chunk + png.substr(33); // right after IHDR
    return true;
}

// Runs work(i) for i in [0, count) on all cores
template <class F>
void parallelFor(size_t count, F&& work) {
    std::atomic<size_t> next{0};
    const unsigned threads = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), 16));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < count;) work(i);
        });
    for (std::thread& t : pool) t.join();
}

} // namespace

bool cookGame(const std::string& outPack, bool web) {
    const std::string projectFile = findAsset(kProjectFileName);
    if (projectFile.empty()) {
        MEALKIT_WARN("Cook: %s not found", kProjectFileName);
        return false;
    }
    const std::string projectDir = parentDirectory(projectFile);
    ProjectFile project;
    std::string error;
    if (!loadProjectFile(projectFile, project, &error)) {
        MEALKIT_WARN("Cook: %s: %s", projectFile.c_str(), error.c_str());
        return false;
    }

    // Every asset (paths relative to assets, module assets as modules/<name>/...)
    std::vector<std::string> all;
    walk(projectDir + "/assets", "", all);
    for (const std::string& m : listDirectories(projectDir + "/modules"))
        if (directoryExists(projectDir + "/modules/" + m + "/assets")) walk(projectDir + "/modules/" + m + "/assets", "modules/" + m + "/", all);
    const std::set<std::string> exists(all.begin(), all.end());

    std::set<std::string> reached;
    std::deque<std::string> queue;
    const auto reach = [&](const std::string& p, const char* from) {
        if (exists.count(p)) {
            if (reached.insert(p).second) queue.push_back(p);
            return true;
        }
        bool folder = false; // packInclude folders
        for (const std::string& f : all)
            if (startsWith(f, p + "/")) {
                folder = true;
                if (reached.insert(f).second) queue.push_back(f);
            }
        if (!folder && from) MEALKIT_WARN("Cook: %s names %s, which is not under assets/", from, p.c_str());
        return folder;
    };
    for (const std::string& f : all)
        if (alwaysPacked(f)) reach(f, nullptr);
    reach(project.settings.scene, "the start scene");
    std::vector<std::string> listed;
    words(project.settings.scenes, listed);
    for (const std::string& s : listed) reach(s, "scenes");
    listed.clear();
    for (const std::string& w : entries(project.settings.packInclude)) reach(startsWith(w, "assets/") ? w.substr(7) : w, "packInclude");
    // packFolders files (paths relative to the project); assets their text names are reached like any other reference
    std::set<std::string> extra;
    for (const std::string& folder : entries(project.settings.packFolders)) {
        if (!packableFolder(folder) || !directoryExists(projectDir + "/" + folder)) {
            MEALKIT_WARN("Cook: packFolders names %s, which is not a project folder outside assets/, build/ and export/", folder.c_str());
            continue;
        }
        std::vector<std::string> files;
        walk(projectDir + "/" + folder, folder + "/", files);
        extra.insert(files.begin(), files.end());
    }
    for (const std::string& p : extra) {
        std::string text;
        if (binary(p) || !readTextFile(projectDir + "/" + p, text)) continue;
        std::vector<std::string> refs;
        words(text, refs);
        for (const std::string& r : refs) reach(r, nullptr);
    }
    while (!queue.empty()) {
        const std::string p = queue.front();
        queue.pop_front();
        if (binary(p) || endsWith(p, ".gltf")) continue;
        std::string text;
        if (!readTextFile(projectDir + "/" + assetFilePath(p), text)) continue;
        std::vector<std::string> refs;
        words(text, refs);
        for (const std::string& r : refs) reach(r, nullptr);
    }

    // Browser cook: WAVs as IMA ADPCM at 24 kHz, webHalfTextures PNGs at half size (all cores; the pack loop below takes the results)
    std::map<std::string, std::string> webBytes; // asset → bytes to pack instead of the file
    if (web) {
        std::vector<std::string> halves;
        for (const std::string& w : entries(project.settings.webHalfTextures)) halves.push_back(startsWith(w, "assets/") ? w.substr(7) : w);
        std::vector<std::string> jobs;
        for (const std::string& p : reached) {
            const bool half = endsWith(p, ".png") &&
                              std::any_of(halves.begin(), halves.end(), [&](const std::string& h) { return p == h || startsWith(p, h + "/"); });
            if (half || endsWith(p, ".wav")) jobs.push_back(p);
        }
        if (const std::string folder = parentDirectory(outPack); !folder.empty()) createDirectories(folder); // scratch PNGs go beside the pack
        std::vector<std::string> results(jobs.size());
        std::vector<char> done(jobs.size(), 0);
        std::vector<u64> sizes(jobs.size(), 0);
        parallelFor(jobs.size(), [&](size_t i) {
            const std::string file = projectDir + "/" + assetFilePath(jobs[i]);
            std::string bytes;
            if (!readTextFile(file, bytes)) return;
            sizes[i] = bytes.size();
            if (endsWith(jobs[i], ".wav")) done[i] = webWav(bytes, results[i]);
            else done[i] = halfPng(file, outPack + "." + std::to_string(i) + ".tmp.png", results[i]) && results[i].size() < bytes.size();
        });
        u64 before = 0, after = 0;
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (!done[i]) continue;
            before += sizes[i];
            after += results[i].size();
            webBytes[jobs[i]] = std::move(results[i]);
        }
        MEALKIT_INFO("Cook (web): %zu of %zu WAV/PNG files made smaller, %.1f MB -> %.1f MB", webBytes.size(), jobs.size(),
                     static_cast<double>(before) / (1024.0 * 1024.0), static_cast<double>(after) / (1024.0 * 1024.0));
    }

    // Browser chunks: chunk number per asset from webChunks (numbered by the order listed, then by subfolder name)
    std::map<std::string, u32> chunkOf; // asset → chunk
    std::map<std::string, u32> chunkIds; // chunk name → number
    if (web)
        for (std::string w : entries(project.settings.webChunks)) {
            if (startsWith(w, "assets/")) w = w.substr(7);
            const bool each = endsWith(w, "/*");
            if (each) w.resize(w.size() - 2);
            if (w.empty()) continue;
            for (const std::string& p : reached) {
                if (chunkOf.count(p) || !(each ? startsWith(p, w + "/") : p == w || startsWith(p, w + "/"))) continue;
                std::string name = fileName(w);
                if (each) {
                    const std::string rest = p.substr(w.size() + 1);
                    const size_t slash = rest.find('/');
                    if (slash == std::string::npos) continue; // a file directly in the folder stays in the pack
                    name = rest.substr(0, slash);
                }
                const auto id = chunkIds.emplace(name, static_cast<u32>(chunkIds.size() + 1)).first->second;
                chunkOf[p] = id;
            }
        }

    // Cook and pack
    PackWriter pack;
    u32 failed = 0;
    for (const std::string& p : reached) {
        const std::string packed = assetFilePath(p);
        const auto chunkIt = chunkOf.find(p);
        const u32 chunk = chunkIt == chunkOf.end() ? 0 : chunkIt->second;
        const std::string file = projectDir + "/" + packed;
        if (endsWith(p, ".glb") || endsWith(p, ".gltf")) {
            asset::CookedModel model;
            std::string modelError, cameraError;
            const bool isModel = asset::importModel(file, model, &render::loadCompressedTexture, modelError);
            if (isModel) pack.add(packed + asset::kCookedModelSuffix, asset::writeCookedModel(model), true, chunk);
            std::vector<asset::CameraClip> clips;
            const bool isCamera = asset::loadCameraClips(file, clips, cameraError);
            if (isCamera) pack.add(packed + asset::kCookedCameraSuffix, asset::writeCameraClips(clips), true, chunk);
            if (!isModel && !isCamera) {
                MEALKIT_WARN("Cook: %s: %s", p.c_str(), modelError.c_str());
                ++failed;
            }
            continue;
        }
        if (endsWith(p, ".hdr")) {
            render::EnvironmentMap env;
            std::string bytes;
            const std::string cache = render::loadEnvironment(file, env, error) ? render::environmentCacheFile(file) : std::string();
            if (cache.empty() || !readTextFile(cache, bytes)) {
                MEALKIT_WARN("Cook: %s: %s", p.c_str(), cache.empty() ? error.c_str() : "cannot read the baked cache");
                ++failed;
            } else pack.add(packed + asset::kCookedEnvironmentSuffix, bytes, false, chunk); // RGB9E5 texels barely compress
            continue;
        }
        if (const auto it = webBytes.find(p); it != webBytes.end()) {
            pack.add(packed, it->second, false, chunk); // PNG and ADPCM don't shrink further
            continue;
        }
        std::string bytes;
        if (!readTextFile(file, bytes)) {
            MEALKIT_WARN("Cook: cannot read %s", file.c_str());
            ++failed;
            continue;
        }
        const bool compressed = endsWith(p, ".png") || endsWith(p, ".jpg") || endsWith(p, ".jpeg") || endsWith(p, ".ogg");
        pack.add(packed, bytes, !compressed, chunk);
    }
    for (const std::string& p : extra) {
        std::string bytes;
        if (!readTextFile(projectDir + "/" + p, bytes)) {
            MEALKIT_WARN("Cook: cannot read %s", p.c_str());
            ++failed;
            continue;
        }
        const bool compressed = endsWith(p, ".png") || endsWith(p, ".jpg") || endsWith(p, ".jpeg") || endsWith(p, ".ogg");
        pack.add(p, bytes, !compressed);
    }
    // Project settings without the dev PC lines (like the old Export copy)
    {
        std::string text, kept;
        readTextFile(projectFile, text);
        for (size_t pos = 0; pos < text.size();) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            const std::string_view line = std::string_view(text).substr(pos, end - pos);
            const size_t start = line.find_first_not_of(" \t");
            const std::string_view key = start == std::string_view::npos ? std::string_view() : line.substr(start, line.find_first_of(" \t=", start) - start);
            if (key != "engine" && key != "engineVersion") (kept += line) += '\n';
            pos = end + 1;
        }
        pack.add(kProjectFileName, kept);
    }

    // Asset paths in code that weren't reached (opened by path string: add them to packInclude)
    std::vector<std::string> sources;
    for (const char* dir : {"source", "samples"}) sourceFiles(projectDir + "/" + dir, sources);
    for (const std::string& m : listDirectories(projectDir + "/modules")) sourceFiles(projectDir + "/modules/" + m + "/source", sources);
    std::set<std::string> warned;
    for (const std::string& s : sources) {
        std::string text;
        if (!readTextFile(s, text)) continue;
        std::vector<std::string> found;
        literals(text, found);
        for (const std::string& f : found)
            if (exists.count(f) && !reached.count(f) && warned.insert(f).second)
                MEALKIT_WARN("Cook: %s names assets/%s, which nothing else references - not packed (add it to packInclude in "
                             "Project Settings)", s.c_str(), f.c_str());
    }

    if (!pack.write(outPack, error)) {
        MEALKIT_WARN("Cook: %s", error.c_str());
        return false;
    }
    char hash[24];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(pack.contentHash()));
    std::string manifest = "# MealKit pack manifest (written by --cook)\ncontent " + std::string(hash) + "\nfiles " +
                           std::to_string(pack.count()) + "\n";
    for (const std::string& p : reached) manifest += "asset " + p + "\n";
    for (const std::string& p : extra) manifest += "file " + p + "\n";
    writeTextFile(outPack + ".txt", manifest);
    MEALKIT_INFO("Cook: %zu of %zu assets -> %s (%zu entries, %.1f MB before compression, content %s)%s", reached.size(), all.size(),
                 outPack.c_str(), pack.count(), static_cast<double>(pack.bytes()) / (1024.0 * 1024.0), hash,
                 failed ? " - some assets failed, see above" : "");
    if (!chunkIds.empty())
        MEALKIT_INFO("Cook (web): %zu of the assets in %zu chunk files beside the pack (webChunks)", chunkOf.size(), chunkIds.size());
    return true;
}

#else

bool cookGame(const std::string&, bool) {
    MEALKIT_WARN("--cook needs a development build (build.bat), shipping builds don't read source assets");
    return false;
}

#endif

} // namespace mealkit::game
