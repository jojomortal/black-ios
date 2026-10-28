#include "hd_textures.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>

#include "common/options.h"
#include "core/log.h"
#include "platform.h"

namespace fs = std::filesystem;

namespace hd {
namespace {

struct Entry {
    uint64_t key;
    uint32_t w, h, fmt;
    std::string file;
};

std::vector<Entry> g_entries;
std::unordered_multimap<uint64_t, int> g_by_key;
std::unordered_map<std::string, fs::path> g_files;  // pack file name (lower case) -> path
bool g_map_tried = false, g_pack_tried = false;

struct Stats {
    // distinct textures seen by format (index: 0 P8, 1 DXT1, 2 DXT3, 3 DXT5), and how many of them the map knows
    uint64_t seen[4] = {}, hit[4] = {};
    uint64_t skipped = 0;  // not eligible: linear, cube, 3D, depth, under 16 px, other formats
    std::set<uint64_t> keys_hit;  // distinct map keys seen
    std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t>> done;
    double hash_ms = 0;
    uint64_t loads = 0, load_bytes = 0, load_failed = 0;
    double load_ms = 0;
    std::chrono::steady_clock::time_point logged = std::chrono::steady_clock::now();
} g_st;

int Slot(uint32_t fmt) { return fmt == 0x0B ? 0 : fmt == 0x0C ? 1 : fmt == 0x0E ? 2 : fmt == 0x0F ? 3 : -1; }

// the map's key: FNV-1a 64 over the top level's bytes as 64-bit words
uint64_t Fnv(const uint8_t* m, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull, word;
    for (size_t i = 0; i < n; i += 8) {
        std::memcpy(&word, m + i, 8);
        h = (h ^ word) * 0x100000001b3ull;
    }
    return h;
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

void Report() {
    uint64_t seen = 0, hit = 0;
    for (int i = 0; i < 4; i++) { seen += g_st.seen[i]; hit += g_st.hit[i]; }
    LOGI("hd: %llu textures keyed, %llu in the map (%.0f%%), %llu distinct map keys, %llu uploads not eligible; "
         "P8 %llu/%llu DXT1 %llu/%llu DXT3 %llu/%llu DXT5 %llu/%llu; hashing %.1f ms",
         (unsigned long long)seen, (unsigned long long)hit, seen ? 100.0 * hit / seen : 0.0,
         (unsigned long long)g_st.keys_hit.size(), (unsigned long long)g_st.skipped,
         (unsigned long long)g_st.hit[0], (unsigned long long)g_st.seen[0], (unsigned long long)g_st.hit[1],
         (unsigned long long)g_st.seen[1], (unsigned long long)g_st.hit[2], (unsigned long long)g_st.seen[2],
         (unsigned long long)g_st.hit[3], (unsigned long long)g_st.seen[3], g_st.hash_ms);
    if (g_st.loads || g_st.load_failed)
        LOGI("hd: %llu pack files loaded (%llu failed), %.1f MB of image data, %.1f ms reading and preparing",
             (unsigned long long)g_st.loads, (unsigned long long)g_st.load_failed, g_st.load_bytes / 1048576.0,
             g_st.load_ms);
}

// BLACK_HD_MAP, else the map shipped beside the executable (the app bundle's resources on iOS)
void LoadMap() {
    if (g_map_tried) return;
    g_map_tried = true;
    std::string path;
    if (const char* env = std::getenv("BLACK_HD_MAP")) path = env;
    if (path.empty()) path = platform::AssetPath("textures/black-hd-reimagined.map").string();
    std::ifstream in(path);
    if (!in) {
        if (std::getenv("BLACK_HD_MAP")) LOGE("hd: cannot open the map %s", path.c_str());
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string key, fmt;
        Entry e{};
        if (!(ls >> key >> e.w >> e.h >> fmt >> e.file)) continue;
        e.key = std::strtoull(key.c_str(), nullptr, 16);
        e.fmt = uint32_t(std::strtoul(fmt.c_str(), nullptr, 16));
        g_by_key.emplace(e.key, int(g_entries.size()));
        g_entries.push_back(e);
    }
    LOGI("hd: map %s: %zu textures", path.c_str(), g_entries.size());
}

// the pack: BLACK_HD_PACK, else an "hd" folder in the game's (Documents/Black/hd on iOS); every .dds and .png under
// it by name, so the PCSX2 layout (SLUS-21376/replacements) and a flat folder both work
void FindPack() {
    if (g_pack_tried) return;
    g_pack_tried = true;
    std::vector<fs::path> roots;
    if (const char* env = std::getenv("BLACK_HD_PACK")) roots.push_back(env);
    else
        for (const fs::path& p : platform::GameFolders()) roots.push_back(p / "hd");
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::string ext = Lower(it->path().extension().string());
            if (ext == ".dds" || ext == ".png") g_files[Lower(it->path().filename().string())] = it->path();
        }
        if (!g_files.empty()) {
            LOGI("hd: pack %s: %zu files", root.string().c_str(), g_files.size());
            return;
        }
    }
    LOGI("hd: no pack folder (BLACK_HD_PACK, or hd/ in the game folder), Textures HD does nothing");
}

bool ReadFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return false;
    std::streamsize n = in.tellg();
    if (n <= 0 || n > (1ll << 30)) return false;
    out.resize(size_t(n));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(out.data()), n);
    return bool(in);
}

uint32_t Rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

size_t BlockBytes(Image::Format f) { return f == Image::BC1 ? 8 : 16; }

size_t LevelBytes(Image::Format f, uint32_t w, uint32_t h) {
    if (f == Image::BGRA8) return size_t(w) * h * 4;
    return size_t((w + 3) / 4) * ((h + 3) / 4) * BlockBytes(f);
}

// true when the biggest 32x32 cell average of an alpha plane stays under 130: the PS2's alpha, 128 for opaque
bool Ps2Alpha(const uint8_t* alpha, uint32_t w, uint32_t h, size_t stride, size_t step) {
    float top = 0;
    for (uint32_t cy = 0; cy < 32; cy++)
        for (uint32_t cx = 0; cx < 32; cx++) {
            uint32_t x0 = cx * w / 32, x1 = std::max((cx + 1) * w / 32, x0 + 1), y0 = cy * h / 32,
                     y1 = std::max((cy + 1) * h / 32, y0 + 1);
            double sum = 0;
            uint32_t n = 0;
            for (uint32_t y = y0; y < y1 && y < h; y++)
                for (uint32_t x = x0; x < x1 && x < w; x++, n++) sum += alpha[y * stride + x * step];
            if (n) top = std::max(top, float(sum / n));
        }
    return top <= 130.0f;
}

// a BC3 level's alpha plane, one byte a texel, to look at it
void Bc3Alpha(const uint8_t* s, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    out.assign(size_t(w) * h, 0);
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++, s += 16) {
            unsigned a0 = s[0], a1 = s[1], pal[8];
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++) bits |= uint64_t(s[2 + i]) << (8 * i);
            pal[0] = a0;
            pal[1] = a1;
            if (a0 > a1) for (unsigned i = 1; i < 7; i++) pal[1 + i] = ((7 - i) * a0 + i * a1) / 7;
            else {
                for (unsigned i = 1; i < 5; i++) pal[1 + i] = ((5 - i) * a0 + i * a1) / 5;
                pal[6] = 0;
                pal[7] = 255;
            }
            for (unsigned i = 0; i < 16; i++, bits >>= 3) {
                uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x < w && y < h) out[size_t(y) * w + x] = uint8_t(pal[bits & 7]);
            }
        }
}

// BC3 alpha on the PS2's scale to the full range: both end points doubled
void Bc3DoubleAlpha(uint8_t* s, size_t bytes) {
    for (size_t i = 0; i + 16 <= bytes; i += 16) {
        unsigned a0 = s[i], a1 = s[i + 1];
        unsigned b0 = std::min(a0 * 2, 255u), b1 = std::min(a1 * 2, 255u);
        s[i] = uint8_t(b0);
        s[i + 1] = uint8_t(b1);
        if (a0 > a1 && b0 == b1) std::memset(s + i + 2, 0, 6);  // both opaque now: every texel the first
    }
}

// one level of BGRA to the next: 2x2 box
void Downsample(const uint8_t* src, uint32_t w, uint32_t h, uint8_t* dst) {
    uint32_t dw = std::max(w / 2, 1u), dh = std::max(h / 2, 1u);
    for (uint32_t y = 0; y < dh; y++)
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t x0 = std::min(x * 2, w - 1), y0 = std::min(y * 2, h - 1), x1 = std::min(x0 + 1, w - 1),
                     y1 = std::min(y0 + 1, h - 1);
            for (int k = 0; k < 4; k++) {
                unsigned s = src[(y0 * w + x0) * 4 + k] + src[(y0 * w + x1) * 4 + k] + src[(y1 * w + x0) * 4 + k] +
                             src[(y1 * w + x1) * 4 + k];
                dst[(y * dw + x) * 4 + k] = uint8_t((s + 2) / 4);
            }
        }
}

// one level of BC1/2/3 to BGRA, w x h texels (only for files that have to be scaled down, so it is not fast)
void DecodeBc(Image::Format f, const uint8_t* s, uint32_t w, uint32_t h, uint8_t* out) {
    auto expand = [](unsigned c, unsigned bits) { return bits == 5 ? (c << 3 | c >> 2) : (c << 2 | c >> 4); };
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++, s += BlockBytes(f)) {
            const uint8_t* cb = f == Image::BC1 ? s : s + 8;
            unsigned c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8;
            unsigned col[4][4];  // b g r a
            auto set = [&](int i, unsigned r, unsigned g, unsigned b, unsigned a) {
                col[i][0] = b, col[i][1] = g, col[i][2] = r, col[i][3] = a;
            };
            unsigned r0 = expand(c0 >> 11, 5), g0 = expand((c0 >> 5) & 63, 6), b0 = expand(c0 & 31, 5);
            unsigned r1 = expand(c1 >> 11, 5), g1 = expand((c1 >> 5) & 63, 6), b1 = expand(c1 & 31, 5);
            set(0, r0, g0, b0, 255);
            set(1, r1, g1, b1, 255);
            if (c0 > c1 || f != Image::BC1) {
                set(2, (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
                set(3, (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
            } else {
                set(2, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
                set(3, 0, 0, 0, 0);
            }
            uint32_t idx = cb[4] | cb[5] << 8 | cb[6] << 16 | uint32_t(cb[7]) << 24;
            unsigned alpha[16];
            for (int i = 0; i < 16; i++) alpha[i] = 255;
            if (f == Image::BC2) {
                for (int i = 0; i < 16; i++) alpha[i] = ((s[i / 2] >> (4 * (i & 1))) & 15) * 17;
            } else if (f == Image::BC3) {
                unsigned a0 = s[0], a1 = s[1], pal[8];
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= uint64_t(s[2 + i]) << (8 * i);
                pal[0] = a0;
                pal[1] = a1;
                if (a0 > a1) for (unsigned i = 1; i < 7; i++) pal[1 + i] = ((7 - i) * a0 + i * a1) / 7;
                else {
                    for (unsigned i = 1; i < 5; i++) pal[1 + i] = ((5 - i) * a0 + i * a1) / 5;
                    pal[6] = 0;
                    pal[7] = 255;
                }
                for (int i = 0; i < 16; i++, bits >>= 3) alpha[i] = pal[bits & 7];
            }
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x >= w || y >= h) continue;
                const unsigned* c = col[(idx >> (2 * i)) & 3];
                uint8_t* o = out + (size_t(y) * w + x) * 4;
                o[0] = uint8_t(c[0]);
                o[1] = uint8_t(c[1]);
                o[2] = uint8_t(c[2]);
                o[3] = f == Image::BC1 ? uint8_t(c[3]) : uint8_t(alpha[i]);
            }
        }
}

// BGRA texels of the top level, brought down to max_side and given the chain of levels below
void FillBgraChain(std::vector<uint8_t> px, uint32_t w, uint32_t h, uint32_t max_side, Image& out) {
    std::vector<uint8_t> next;
    while (max_side && std::max(w, h) > max_side) {
        next.resize(size_t(std::max(w / 2, 1u)) * std::max(h / 2, 1u) * 4);
        Downsample(px.data(), w, h, next.data());
        px.swap(next);
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
    out.format = Image::BGRA8;
    out.width = w;
    out.height = h;
    out.levels = 0;
    out.offset.clear();
    out.data.clear();
    for (;;) {
        out.offset.push_back(out.data.size());
        out.data.insert(out.data.end(), px.begin(), px.end());
        out.levels++;
        if (w == 1 && h == 1) break;
        next.resize(size_t(std::max(w / 2, 1u)) * std::max(h / 2, 1u) * 4);
        Downsample(px.data(), w, h, next.data());
        px.swap(next);
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
}

bool LoadDds(const std::vector<uint8_t>& file, uint32_t max_side, Image& out) {
    if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4)) return false;
    const uint8_t* hd = file.data();
    uint32_t h = Rd32(hd + 12), w = Rd32(hd + 16), levels = Rd32(hd + 28), fourcc = Rd32(hd + 84);
    Image::Format f;
    if (fourcc == 0x31545844u) f = Image::BC1;
    else if (fourcc == 0x33545844u) f = Image::BC2;
    else if (fourcc == 0x35545844u) f = Image::BC3;
    else return false;
    if (!w || !h || w > 16384 || h > 16384) return false;
    levels = std::clamp(levels, 1u, 16u);
    std::vector<size_t> at(levels), len(levels);
    size_t pos = 128;
    for (uint32_t i = 0; i < levels; i++) {
        at[i] = pos;
        len[i] = LevelBytes(f, std::max(w >> i, 1u), std::max(h >> i, 1u));
        pos += len[i];
    }
    while (levels > 1 && at[levels - 1] + len[levels - 1] > file.size()) levels--;
    if (at[0] + len[0] > file.size()) return false;
    bool ps2 = false;
    if (f == Image::BC3) {
        // looked at on a level of 64 px or more at its smaller side, as small as the file has
        uint32_t l = 0;
        while (l + 1 < levels && (w >> (l + 1)) >= 64 && (h >> (l + 1)) >= 64) l++;
        std::vector<uint8_t> alpha;
        uint32_t lw = std::max(w >> l, 1u), lh = std::max(h >> l, 1u);
        Bc3Alpha(file.data() + at[l], lw, lh, alpha);
        ps2 = Ps2Alpha(alpha.data(), lw, lh, lw, 1);
    }
    // the biggest levels dropped while they pass max_side; a file with one level has nothing smaller to use
    uint32_t skip = 0;
    while (max_side && skip + 1 < levels && (std::max(w >> skip, h >> skip) > max_side)) skip++;
    if (max_side && std::max(w >> skip, h >> skip) > max_side) {
        // a file without the smaller levels: decoded, scaled down to the cap and given its own chain
        std::vector<uint8_t> top(file.begin() + at[skip], file.begin() + at[skip] + len[skip]);
        if (ps2) Bc3DoubleAlpha(top.data(), top.size());
        uint32_t tw = std::max(w >> skip, 1u), th = std::max(h >> skip, 1u);
        std::vector<uint8_t> px(size_t(tw) * th * 4);
        DecodeBc(f, top.data(), tw, th, px.data());
        out.file_width = w;
        out.file_height = h;
        FillBgraChain(std::move(px), tw, th, max_side, out);
        return true;
    }
    out.format = f;
    out.file_width = w;
    out.file_height = h;
    out.width = std::max(w >> skip, 1u);
    out.height = std::max(h >> skip, 1u);
    out.levels = levels - skip;
    out.data.clear();
    for (uint32_t i = skip; i < levels; i++) {
        out.offset.push_back(out.data.size());
        out.data.insert(out.data.end(), file.begin() + at[i], file.begin() + at[i] + len[i]);
    }
    if (ps2) Bc3DoubleAlpha(out.data.data(), out.data.size());  // only BC3 levels of 16-byte blocks all through
    return true;
}

bool LoadPng(const fs::path& path, uint32_t max_side, Image& out) {
    SDL_Surface* png = SDL_LoadPNG(path.string().c_str());
    SDL_Surface* s = png ? SDL_ConvertSurface(png, SDL_PIXELFORMAT_ARGB8888) : nullptr;  // B G R A in memory
    SDL_DestroySurface(png);
    if (!s) return false;
    uint32_t w = uint32_t(s->w), h = uint32_t(s->h);
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++)
        std::memcpy(&px[size_t(y) * w * 4], static_cast<const uint8_t*>(s->pixels) + size_t(y) * s->pitch, size_t(w) * 4);
    SDL_DestroySurface(s);
    if (Ps2Alpha(px.data() + 3, w, h, size_t(w) * 4, 4))
        for (size_t i = 3; i < px.size(); i += 4) px[i] = uint8_t(std::min(px[i] * 2u, 255u));
    out.file_width = w;
    out.file_height = h;
    FillBgraChain(std::move(px), w, h, max_side, out);
    return true;
}

// BLACK_HD_SELFTEST=<max side>: every map entry loaded once at the first texture, to see that none fail and what
// they cost; not for play
void SelfTest() {
    const char* e = std::getenv("BLACK_HD_SELFTEST");
    if (!e || g_files.empty()) return;
    uint32_t max_side = uint32_t(std::strtoul(e, nullptr, 10)), ok = 0, bad = 0;
    uint64_t bytes = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < g_entries.size(); i++) {
        Image im;
        if (Load(int(i), max_side, im)) {
            ok++;
            bytes += im.data.size();
        } else bad++;
    }
    LOGI("hd: self test, max side %u: %u loaded, %u failed, %.0f MB, %.1f s", max_side, ok, bad, bytes / 1048576.0,
         std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

}  // namespace

uint32_t State() {
    // BLACK_TEXTURES=hd|original overrides the setting for a run (the options menu then does nothing to it)
    static const char* env = std::getenv("BLACK_TEXTURES");
    if (env && *env ? std::strcmp(env, "hd") != 0 : game::Opt().textures != 1) return 0;
    LoadMap();
    FindPack();
    static bool tested = (SelfTest(), true);
    (void)tested;
    return !g_entries.empty() && !g_files.empty();
}

int Find(const gpu::TextureDesc& d, const gpu::FormatInfo& fi, const uint8_t* guest) {
    LoadMap();
    if (g_entries.empty()) return -1;
    int slot = Slot(d.format);
    if (slot < 0 || d.linear || d.cube || d.depth > 1 || fi.depth || d.width < 16 || d.height < 16 || !guest) {
        g_st.skipped++;
        return -1;
    }
    size_t top = slot == 0 ? size_t(d.width) * d.height
                           : size_t(d.width / 4) * (d.height / 4) * (slot == 1 ? 8 : 16);
    if (!top || (top & 7)) {
        g_st.skipped++;
        return -1;
    }
    auto t0 = std::chrono::steady_clock::now();
    uint64_t key = Fnv(guest, top);
    g_st.hash_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    int found = -1;
    auto r = g_by_key.equal_range(key);
    for (auto it = r.first; it != r.second; ++it) {
        const Entry& e = g_entries[size_t(it->second)];
        if (e.w == d.width && e.h == d.height && e.fmt == d.format) found = it->second;
    }
    if (g_st.done.insert({key, d.width, d.height, d.format}).second) {
        g_st.seen[slot]++;
        if (found >= 0) {
            g_st.hit[slot]++;
            g_st.keys_hit.insert(key);
        } else {
            LOGI("hd: miss %016llx %ux%u fmt %02x levels %u", (unsigned long long)key, d.width, d.height, d.format,
                 d.levels);
        }
    }
    auto now = std::chrono::steady_clock::now();
    if (now - g_st.logged > std::chrono::seconds(5)) {
        g_st.logged = now;
        Report();
    }
    return State() ? found : -1;
}

bool Load(int entry, uint32_t max_side, Image& out) {
    if (entry < 0 || size_t(entry) >= g_entries.size()) return false;
    auto t0 = std::chrono::steady_clock::now();
    const std::string& name = g_entries[size_t(entry)].file;
    out = Image{};
    bool ok = false;
    auto it = g_files.find(Lower(name));
    if (it != g_files.end()) {
        if (Lower(it->second.extension().string()) == ".png") ok = LoadPng(it->second, max_side, out);
        else {
            std::vector<uint8_t> file;
            ok = ReadFile(it->second, file) && LoadDds(file, max_side, out);
        }
    }
    g_st.load_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ok) {
        g_st.loads++;
        g_st.load_bytes += out.data.size();
    } else {
        if (g_st.load_failed++ < 8) LOGE("hd: cannot use %s", name.c_str());
    }
    return ok;
}

}  // namespace hd
