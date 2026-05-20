#include "ui.h"

#include <GL/glew.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "implot.h"

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxMercatorLatitude = 85.05112878;
constexpr int kMinZoom = 1;
constexpr int kMaxZoom = 19;
constexpr int kTileSizePx = 256;

double clampLatitude(double lat) {
    return std::clamp(lat, -kMaxMercatorLatitude, kMaxMercatorLatitude);
}

double latitudeToMercatorY(double latitude_deg) {
    const double lat_rad = clampLatitude(latitude_deg) * kPi / 180.0;
    const double mercator_rad = std::log(std::tan(kPi / 4.0 + lat_rad / 2.0));
    return mercator_rad * 180.0 / kPi;
}

double MercatorXToTileX(double mercatorX, int zoom) {
    return (0.5 + mercatorX / 360.0) * static_cast<double>(1 << zoom);
}

double MercatorYToTileY(double mercatorY, int zoom) {
    return (0.5 - mercatorY / 360.0) * static_cast<double>(1 << zoom);
}

double TileXToMercatorX(int tileX, int zoom) {
    return (tileX / static_cast<double>(1 << zoom) - 0.5) * 360.0;
}

double TileYToMercatorY(int tileY, int zoom) {
    return (0.5 - tileY / static_cast<double>(1 << zoom)) * 360.0;
}

double TileXToMercatorX(double tileX, int zoom) {
    return (tileX / static_cast<double>(1 << zoom) - 0.5) * 360.0;
}

double TileYToMercatorY(double tileY, int zoom) {
    return (0.5 - tileY / static_cast<double>(1 << zoom)) * 360.0;
}

double mercatorYToLatitude(double mercatorY) {
    const double mercator_rad = mercatorY * kPi / 180.0;
    const double lat_rad = 2.0 * std::atan(std::exp(mercator_rad)) - kPi / 2.0;
    return lat_rad * 180.0 / kPi;
}

int chooseZoomByPlotWidth(double lon_span_deg) {
    if (lon_span_deg <= 0.0) {
        return kMaxZoom;
    }
    const double zoom_estimate = std::floor(std::log2(360.0 / lon_span_deg)) + 2.0;
    return std::clamp(static_cast<int>(zoom_estimate), kMinZoom, kMaxZoom);
}

std::string makeTileId(int zoom, int x, int y) {
    return std::to_string(zoom) + "/" + std::to_string(x) + "/" + std::to_string(y);
}

std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

const char* heatmapCriterionKey(HeatmapCriterion criterion) {
    switch (criterion) {
        case HeatmapCriterion::RSRP:
            return "rsrp";
        case HeatmapCriterion::RSRQ:
            return "rsrq";
        case HeatmapCriterion::RSSI:
            return "rssi";
        case HeatmapCriterion::Altitude:
            return "altitude";
    }
    return "rsrp";
}

double distanceMeters(double lat1, double lon1, double lat2, double lon2) {
    constexpr double earthRadiusMeters = 6371000.0;
    const double lat1Rad = lat1 * kPi / 180.0;
    const double lat2Rad = lat2 * kPi / 180.0;
    const double dLat = (lat2 - lat1) * kPi / 180.0;
    const double dLon = (lon2 - lon1) * kPi / 180.0;
    const double a = std::sin(dLat / 2.0) * std::sin(dLat / 2.0) +
                     std::cos(lat1Rad) * std::cos(lat2Rad) *
                         std::sin(dLon / 2.0) * std::sin(dLon / 2.0);
    return earthRadiusMeters * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

std::filesystem::path buildOutputRoot() {
    const std::filesystem::path cwd = std::filesystem::current_path();
    if (cwd.filename() == "build") {
        return cwd;
    }
    return cwd / "build";
}

struct TileJob {
    std::string id;
    int zoom = 0;
    int x = 0;
    int y = 0;
};

struct TextureData {
    GLuint id = 0;
    bool isLoading = false;
    std::vector<std::uint8_t> rgbaBlob;
    int width = 0;
    int height = 0;
    std::chrono::steady_clock::time_point retryAfter = std::chrono::steady_clock::time_point::min();
};

class TileManager {
   public:
    static TileManager& instance() {
        static TileManager manager;
        return manager;
    }

    ~TileManager() {
        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            stopRequested_ = true;
        }
        jobCv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool isAvailable() const {
        return fetcherAvailable_;
    }

    std::string statusText() const {
        return statusText_;
    }

    void onZoomChanged(int newZoom) {
        if (newZoom == activeZoom_) {
            return;
        }
        activeZoom_ = newZoom;

        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            std::queue<TileJob> empty;
            std::swap(jobQueue_, empty);
        }

        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            for (auto& [id, tex] : tileCache_) {
                (void)id;
                tex.isLoading = false;
            }
        }
    }

    GLuint getTextureOrQueue(int zoom, int x, int y) {
        const int maxTile = (1 << zoom) - 1;
        if (x < 0 || x > maxTile || y < 0 || y > maxTile) {
            return 0;
        }

        const TileJob job{makeTileId(zoom, x, y), zoom, x, y};
        bool shouldEnqueue = false;

        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            auto& tex = tileCache_[job.id];
            if (!tex.rgbaBlob.empty() && tex.id == 0) {
                uploadRgbaToGpu(tex);
            }

            if (tex.id != 0) {
                return tex.id;
            }

            const auto now = std::chrono::steady_clock::now();
            if (!tex.isLoading && now >= tex.retryAfter) {
                tex.isLoading = true;
                shouldEnqueue = true;
            }
        }

        if (shouldEnqueue) {
            std::lock_guard<std::mutex> lock(jobMutex_);
            jobQueue_.push(job);
            jobCv_.notify_one();
        }

        return 0;
    }

    std::size_t pendingJobs() const {
        std::lock_guard<std::mutex> lock(jobMutex_);
        return jobQueue_.size();
    }

    std::size_t cachedTiles() const {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        return tileCache_.size();
    }

   private:
    TileManager() {
        if (std::system("curl --version >/dev/null 2>&1") == 0 &&
            std::system("ffmpeg -version >/dev/null 2>&1") == 0) {
            fetcherAvailable_ = true;
            statusText_ = "Tile fetcher: C++ worker (curl + ffmpeg)";
        } else {
            fetcherAvailable_ = false;
            statusText_ = "curl/ffmpeg not found: OSM tiles disabled";
        }

        cacheDir_ = std::filesystem::current_path() / "Location_save_data" / "osm_cache";
        std::error_code ec;
        std::filesystem::create_directories(cacheDir_, ec);
        if (ec) {
            statusText_ = "Failed to create cache dir: " + cacheDir_.string();
        }

        worker_ = std::thread(&TileManager::workerLoop, this);
    }

   public:
    static void uploadRgbaToGpu(TextureData& tex) {
        if (tex.rgbaBlob.empty() || tex.width <= 0 || tex.height <= 0) {
            return;
        }

        if (tex.id == 0) {
            glGenTextures(1, &tex.id);
        }

        glBindTexture(GL_TEXTURE_2D, tex.id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            tex.width,
            tex.height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            tex.rgbaBlob.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        tex.rgbaBlob.clear();
        tex.rgbaBlob.shrink_to_fit();
    }

   private:
    bool fetchTileData(const TileJob& job, std::vector<std::uint8_t>& rgba, int& width, int& height) {
        if (!fetcherAvailable_) {
            return false;
        }

        const std::filesystem::path tileDir = cacheDir_ / std::to_string(job.zoom) / std::to_string(job.x);
        const std::filesystem::path pngPath = tileDir / (std::to_string(job.y) + ".png");
        const std::filesystem::path rgbaPath = tileDir / (std::to_string(job.y) + ".rgba");

        width = kTileSizePx;
        height = kTileSizePx;
        const std::size_t expectedSize =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;

        std::error_code ec;
        std::filesystem::create_directories(tileDir, ec);
        if (ec) {
            return false;
        }

        if (!std::filesystem::exists(pngPath)) {
            const std::string url = "https://tile.openstreetmap.org/" + std::to_string(job.zoom) +
                                    "/" + std::to_string(job.x) + "/" + std::to_string(job.y) +
                                    ".png";
            const std::string curlCmd =
                "curl -fsSL --retry 2 --connect-timeout 5 -A "
                "'backend-server-gps/1.0 (tile loader)' -o " +
                shellQuote(pngPath.string()) + " " + shellQuote(url) + " >/dev/null 2>&1";
            if (std::system(curlCmd.c_str()) != 0) {
                return false;
            }
        }

        if (!std::filesystem::exists(rgbaPath) || std::filesystem::file_size(rgbaPath, ec) != expectedSize ||
            ec) {
            const std::string ffmpegCmd = "ffmpeg -v error -y -i " + shellQuote(pngPath.string()) +
                                          " -f rawvideo -pix_fmt rgba " +
                                          shellQuote(rgbaPath.string()) + " >/dev/null 2>&1";
            if (std::system(ffmpegCmd.c_str()) != 0) {
                return false;
            }
        }

        std::ifstream raw(rgbaPath, std::ios::binary);
        if (!raw.is_open()) {
            return false;
        }

        raw.seekg(0, std::ios::end);
        const std::streamsize size = raw.tellg();
        raw.seekg(0, std::ios::beg);
        if (size <= 0) {
            return false;
        }

        rgba.resize(static_cast<std::size_t>(size));
        if (!raw.read(reinterpret_cast<char*>(rgba.data()), size)) {
            return false;
        }

        if (rgba.size() != expectedSize) {
            rgba.clear();
            return false;
        }

        return true;
    }

    void workerLoop() {
        while (true) {
            TileJob job;
            {
                std::unique_lock<std::mutex> lock(jobMutex_);
                jobCv_.wait(lock, [this]() { return stopRequested_ || !jobQueue_.empty(); });
                if (stopRequested_) {
                    return;
                }

                job = jobQueue_.front();
                jobQueue_.pop();
            }

            std::vector<std::uint8_t> rgba;
            int width = 0;
            int height = 0;
            const bool ok = fetchTileData(job, rgba, width, height);

            std::lock_guard<std::mutex> lock(cacheMutex_);
            auto it = tileCache_.find(job.id);
            if (it == tileCache_.end()) {
                continue;
            }

            auto& tex = it->second;
            tex.isLoading = false;
            if (ok) {
                tex.width = width;
                tex.height = height;
                tex.rgbaBlob = std::move(rgba);
                tex.retryAfter = std::chrono::steady_clock::time_point::min();
            } else {
                tex.retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
        }
    }

    mutable std::mutex cacheMutex_;
    mutable std::mutex jobMutex_;
    std::condition_variable jobCv_;
    std::map<std::string, TextureData> tileCache_;
    std::queue<TileJob> jobQueue_;
    std::thread worker_;

    bool stopRequested_ = false;
    bool fetcherAvailable_ = false;
    int activeZoom_ = -1;
    std::string statusText_;
    std::filesystem::path cacheDir_;
};

struct HeatmapTileJob {
    std::string id;
    int zoom = 0;
    int x = 0;
    int y = 0;
    std::uint64_t sourceVersion = 0;
};

struct HeatmapSource {
    HeatmapSettings settings;
    std::vector<HeatmapSample> samples;
    double minValue = 0.0;
    double maxValue = 1.0;
    std::string cacheNamespace;
    std::uint64_t version = 0;
    bool valid = false;
};

std::uint64_t heatmapSamplesFingerprint(const std::vector<HeatmapSample>& samples) {
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };

    mix(static_cast<std::uint64_t>(samples.size()));
    if (samples.empty()) {
        return hash;
    }

    for (const auto& sample : samples) {
        mix(static_cast<std::uint64_t>(std::llround(sample.latitude * 1000000.0)));
        mix(static_cast<std::uint64_t>(std::llround(sample.longitude * 1000000.0)));
        mix(static_cast<std::uint64_t>(std::llround(sample.value * 1000.0)));
    }
    return hash;
}

std::uint64_t heatmapSourceFingerprint(
    const HeatmapSettings& settings,
    const std::vector<HeatmapSample>& samples) {
    std::uint64_t hash = heatmapSamplesFingerprint(samples);
    hash ^= static_cast<std::uint64_t>(settings.selectedEarfcn + 1000003) + 0x9e3779b97f4a7c15ULL +
            (hash << 6) + (hash >> 2);
    hash ^= static_cast<std::uint64_t>(settings.criterion) + 0x9e3779b97f4a7c15ULL +
            (hash << 6) + (hash >> 2);
    hash ^= static_cast<std::uint64_t>(std::llround(settings.radiusMeters * 100.0)) +
            0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    hash ^= static_cast<std::uint64_t>(settings.enabled) + 0x9e3779b97f4a7c15ULL +
            (hash << 6) + (hash >> 2);
    return hash;
}

void heatmapValueRange(
    HeatmapCriterion criterion,
    const std::vector<HeatmapSample>& samples,
    double& minValue,
    double& maxValue) {
    switch (criterion) {
        case HeatmapCriterion::RSRP:
            minValue = -120.0;
            maxValue = -60.0;
            return;
        case HeatmapCriterion::RSRQ:
            minValue = -20.0;
            maxValue = -3.0;
            return;
        case HeatmapCriterion::RSSI:
            minValue = -110.0;
            maxValue = -50.0;
            return;
        case HeatmapCriterion::Altitude:
            break;
    }

    if (samples.empty()) {
        minValue = 0.0;
        maxValue = 1.0;
        return;
    }

    minValue = samples.front().value;
    maxValue = samples.front().value;
    for (const auto& sample : samples) {
        minValue = std::min(minValue, sample.value);
        maxValue = std::max(maxValue, sample.value);
    }
    if (std::abs(maxValue - minValue) < 0.001) {
        minValue -= 1.0;
        maxValue += 1.0;
    }
}

void heatmapColor(double normalized, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    const double t = std::clamp(normalized, 0.0, 1.0);
    const double scaled = t * 4.0;
    const int band = std::min(3, static_cast<int>(scaled));
    const double local = scaled - band;

    const std::uint8_t colors[5][3] = {
        {49, 54, 149},
        {69, 117, 180},
        {116, 173, 209},
        {253, 174, 97},
        {215, 48, 39},
    };

    r = static_cast<std::uint8_t>(colors[band][0] + (colors[band + 1][0] - colors[band][0]) * local);
    g = static_cast<std::uint8_t>(colors[band][1] + (colors[band + 1][1] - colors[band][1]) * local);
    b = static_cast<std::uint8_t>(colors[band][2] + (colors[band + 1][2] - colors[band][2]) * local);
}

class HeatmapTileManager {
   public:
    static HeatmapTileManager& instance() {
        static HeatmapTileManager manager;
        return manager;
    }

    ~HeatmapTileManager() {
        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            stopRequested_ = true;
        }
        jobCv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void setSource(const HeatmapSettings& settings, const std::vector<HeatmapSample>& samples) {
        const bool valid = settings.enabled && !samples.empty() && ffmpegAvailable_;
        const std::uint64_t newFingerprint = valid ? heatmapSourceFingerprint(settings, samples) : 0;

        std::lock_guard<std::mutex> sourceLock(sourceMutex_);
        if (sourceFingerprint_ == newFingerprint) {
            return;
        }

        sourceFingerprint_ = newFingerprint;
        ++source_.version;
        source_.settings = settings;
        source_.samples = samples;
        source_.valid = valid;
        heatmapValueRange(settings.criterion, samples, source_.minValue, source_.maxValue);

        const int radius = static_cast<int>(std::llround(settings.radiusMeters));
        const std::string earfcnPart = settings.selectedEarfcn >= 0
                                           ? "earfcn_" + std::to_string(settings.selectedEarfcn)
                                           : "earfcn_all";
        source_.cacheNamespace = std::string("heatmap/") + heatmapCriterionKey(settings.criterion) +
                                 "/" + earfcnPart + "/r_" + std::to_string(radius);

        {
            std::lock_guard<std::mutex> jobLock(jobMutex_);
            std::queue<HeatmapTileJob> empty;
            std::swap(jobQueue_, empty);
        }

        {
            std::lock_guard<std::mutex> cacheLock(cacheMutex_);
            tileCache_.clear();
        }
    }

    bool isAvailable() const {
        return ffmpegAvailable_;
    }

    std::string statusText() const {
        if (!ffmpegAvailable_) {
            return "Heatmap disabled: ffmpeg not found";
        }

        std::lock_guard<std::mutex> lock(sourceMutex_);
        if (!source_.valid) {
            return "Heatmap: no samples for selected criterion";
        }
        return "Heatmap: " + source_.cacheNamespace;
    }

    GLuint getTextureOrQueue(int zoom, int x, int y) {
        std::uint64_t sourceVersion = 0;
        {
            std::lock_guard<std::mutex> lock(sourceMutex_);
            if (!source_.valid) {
                return 0;
            }
            sourceVersion = source_.version;
        }

        const int maxTile = (1 << zoom) - 1;
        if (x < 0 || x > maxTile || y < 0 || y > maxTile) {
            return 0;
        }

        const HeatmapTileJob job{makeTileId(zoom, x, y), zoom, x, y, sourceVersion};
        bool shouldEnqueue = false;

        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            auto& tex = tileCache_[job.id];
            if (!tex.rgbaBlob.empty() && tex.id == 0) {
                TileManager::uploadRgbaToGpu(tex);
            }

            if (tex.id != 0) {
                return tex.id;
            }

            const auto now = std::chrono::steady_clock::now();
            if (!tex.isLoading && now >= tex.retryAfter) {
                tex.isLoading = true;
                shouldEnqueue = true;
            }
        }

        if (shouldEnqueue) {
            std::lock_guard<std::mutex> lock(jobMutex_);
            jobQueue_.push(job);
            jobCv_.notify_one();
        }

        return 0;
    }

    std::size_t pendingJobs() const {
        std::lock_guard<std::mutex> lock(jobMutex_);
        return jobQueue_.size();
    }

    std::size_t cachedTiles() const {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        return tileCache_.size();
    }

   private:
    HeatmapTileManager() {
        ffmpegAvailable_ = std::system("ffmpeg -version >/dev/null 2>&1") == 0;
        outputRoot_ = buildOutputRoot();
        worker_ = std::thread(&HeatmapTileManager::workerLoop, this);
    }

    bool computeTileRgba(const HeatmapSource& source, const HeatmapTileJob& job, std::vector<std::uint8_t>& rgba) {
        const double radiusMeters = std::clamp(source.settings.radiusMeters, 10.0, 40.0);
        const double tileLonMin = TileXToMercatorX(job.x, job.zoom);
        const double tileLonMax = TileXToMercatorX(job.x + 1, job.zoom);
        const double tileLatTop = mercatorYToLatitude(TileYToMercatorY(job.y, job.zoom));
        const double tileLatBottom = mercatorYToLatitude(TileYToMercatorY(job.y + 1, job.zoom));
        const double tileLatMin = std::min(tileLatBottom, tileLatTop);
        const double tileLatMax = std::max(tileLatBottom, tileLatTop);
        const double centerLat = (tileLatMin + tileLatMax) / 2.0;
        const double latPad = radiusMeters / 111320.0;
        const double lonPad = radiusMeters /
                              std::max(1.0, 111320.0 * std::cos(centerLat * kPi / 180.0));

        std::vector<HeatmapSample> candidates;
        candidates.reserve(source.samples.size());
        for (const auto& sample : source.samples) {
            if (sample.latitude >= tileLatMin - latPad && sample.latitude <= tileLatMax + latPad &&
                sample.longitude >= tileLonMin - lonPad && sample.longitude <= tileLonMax + lonPad) {
                candidates.push_back(sample);
            }
        }

        rgba.assign(static_cast<std::size_t>(kTileSizePx) * kTileSizePx * 4, 0);
        if (candidates.empty()) {
            return true;
        }

        const double range = source.maxValue - source.minValue;
        for (int py = 0; py < kTileSizePx; ++py) {
            const double tileY = job.y + (static_cast<double>(py) + 0.5) / kTileSizePx;
            const double lat = mercatorYToLatitude(TileYToMercatorY(tileY, job.zoom));
            for (int px = 0; px < kTileSizePx; ++px) {
                const double tileX = job.x + (static_cast<double>(px) + 0.5) / kTileSizePx;
                const double lon = TileXToMercatorX(tileX, job.zoom);

                double weightedSum = 0.0;
                double weightSum = 0.0;
                bool exactSample = false;
                double exactValue = 0.0;

                for (const auto& sample : candidates) {
                    const double distance = distanceMeters(lat, lon, sample.latitude, sample.longitude);
                    if (distance > radiusMeters) {
                        continue;
                    }

                    if (distance < 0.5) {
                        exactSample = true;
                        exactValue = sample.value;
                        break;
                    }

                    const double weight = 1.0 / (distance * distance + 1.0);
                    weightedSum += sample.value * weight;
                    weightSum += weight;
                }

                if (!exactSample && weightSum <= 0.0) {
                    continue;
                }

                const double value = exactSample ? exactValue : weightedSum / weightSum;
                const double normalized = range > 0.0 ? (value - source.minValue) / range : 0.5;

                std::uint8_t r = 0;
                std::uint8_t g = 0;
                std::uint8_t b = 0;
                heatmapColor(normalized, r, g, b);

                const std::size_t offset = (static_cast<std::size_t>(py) * kTileSizePx + px) * 4;
                rgba[offset] = r;
                rgba[offset + 1] = g;
                rgba[offset + 2] = b;
                rgba[offset + 3] = 175;
            }
        }

        return true;
    }

    bool saveTilePng(const HeatmapSource& source, const HeatmapTileJob& job, const std::vector<std::uint8_t>& rgba) {
        const std::filesystem::path tileDir =
            outputRoot_ / source.cacheNamespace / std::to_string(job.zoom) / std::to_string(job.x);
        const std::filesystem::path pngPath = tileDir / (std::to_string(job.y) + ".png");
        const std::filesystem::path rawPath = tileDir / (std::to_string(job.y) + ".rgba.tmp");

        std::error_code ec;
        std::filesystem::create_directories(tileDir, ec);
        if (ec) {
            return false;
        }

        {
            std::ofstream raw(rawPath, std::ios::binary);
            if (!raw.is_open()) {
                return false;
            }
            raw.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
            if (!raw.good()) {
                return false;
            }
        }

        const std::string ffmpegCmd =
            "ffmpeg -v error -y -f rawvideo -pix_fmt rgba -s 256x256 -i " +
            shellQuote(rawPath.string()) + " " + shellQuote(pngPath.string()) + " >/dev/null 2>&1";
        const bool ok = std::system(ffmpegCmd.c_str()) == 0;
        std::filesystem::remove(rawPath, ec);
        return ok;
    }

    void workerLoop() {
        while (true) {
            HeatmapTileJob job;
            {
                std::unique_lock<std::mutex> lock(jobMutex_);
                jobCv_.wait(lock, [this]() { return stopRequested_ || !jobQueue_.empty(); });
                if (stopRequested_) {
                    return;
                }

                job = jobQueue_.front();
                jobQueue_.pop();
            }

            HeatmapSource sourceSnapshot;
            {
                std::lock_guard<std::mutex> lock(sourceMutex_);
                if (!source_.valid || source_.version != job.sourceVersion) {
                    continue;
                }
                sourceSnapshot = source_;
            }

            std::vector<std::uint8_t> rgba;
            const bool ok = computeTileRgba(sourceSnapshot, job, rgba) &&
                            saveTilePng(sourceSnapshot, job, rgba);

            std::lock_guard<std::mutex> lock(cacheMutex_);
            auto it = tileCache_.find(job.id);
            if (it == tileCache_.end()) {
                continue;
            }

            auto& tex = it->second;
            tex.isLoading = false;
            if (ok) {
                tex.width = kTileSizePx;
                tex.height = kTileSizePx;
                tex.rgbaBlob = std::move(rgba);
                tex.retryAfter = std::chrono::steady_clock::time_point::min();
            } else {
                tex.retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
        }
    }

    mutable std::mutex cacheMutex_;
    mutable std::mutex jobMutex_;
    mutable std::mutex sourceMutex_;
    std::condition_variable jobCv_;
    std::map<std::string, TextureData> tileCache_;
    std::queue<HeatmapTileJob> jobQueue_;
    std::thread worker_;

    bool stopRequested_ = false;
    bool ffmpegAvailable_ = false;
    std::filesystem::path outputRoot_;
    HeatmapSource source_;
    std::uint64_t sourceFingerprint_ = 0;
};

void renderMapWindow(
    const std::vector<LocationPoint>& mapPoints,
    const HeatmapSettings& heatmapSettings,
    const std::vector<HeatmapSample>& heatmapSamples) {
    ImGui::Begin("OSM Mercator Map");

    TileManager& tiles = TileManager::instance();
    HeatmapTileManager& heatmap = HeatmapTileManager::instance();
    heatmap.setSource(heatmapSettings, heatmapSamples);

    ImGui::TextUnformatted(tiles.statusText().c_str());
    if (heatmapSettings.enabled) {
        ImGui::TextUnformatted(heatmap.statusText().c_str());
    }
    ImGui::Text("Points from DB: %zu", mapPoints.size());

    const bool drawOsmTiles = tiles.isAvailable();
    if (!drawOsmTiles) {
        ImGui::TextUnformatted("OSM tiles disabled: tile fetcher is unavailable.");
    }

    static int lastZoom = -1;
    static int minX = 0;
    static int maxX = 0;
    static int minY = 0;
    static int maxY = 0;
    static int centerX = 0;
    static int centerY = 0;
    static int currentZoom = kMinZoom;
    static bool forceSquareView = true;

    ImGui::Checkbox("Square Mercator View", &forceSquareView);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    float plotSide = std::min(avail.x, std::max(320.0f, avail.y - 120.0f));
    if (plotSide < 320.0f) {
        plotSide = std::max(320.0f, std::min(avail.x, 520.0f));
    }

    ImPlotFlags plotFlags = ImPlotFlags_NoLegend;
    if (forceSquareView) {
        plotFlags |= ImPlotFlags_Equal;
    }

    if (ImPlot::BeginPlot("##OSMPlot", ImVec2(plotSide, plotSide), plotFlags)) {
        ImPlot::SetupAxes("Longitude", "Mercator Y");
        ImPlot::SetupAxisLimits(ImAxis_X1, -180.0, 180.0, ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -180.0, 180.0, ImPlotCond_Once);

        const ImPlotRect limits = ImPlot::GetPlotLimits();
        currentZoom = chooseZoomByPlotWidth(limits.X.Max - limits.X.Min);
        if (currentZoom != lastZoom) {
            tiles.onZoomChanged(currentZoom);
            lastZoom = currentZoom;
        }

        minX = static_cast<int>(std::floor(MercatorXToTileX(limits.X.Min, currentZoom)));
        minY = static_cast<int>(std::floor(MercatorYToTileY(limits.Y.Max, currentZoom)));
        maxX = static_cast<int>(std::floor(MercatorXToTileX(limits.X.Max, currentZoom)));
        maxY = static_cast<int>(std::floor(MercatorYToTileY(limits.Y.Min, currentZoom)));

        const int maxTileCount = (1 << currentZoom) - 1;
        minX = std::max(0, minX);
        maxX = std::min(maxTileCount, maxX);
        minY = std::max(0, minY);
        maxY = std::min(maxTileCount, maxY);

        centerX = (minX + maxX) / 2;
        centerY = (minY + maxY) / 2;

        for (int x = minX; x <= maxX; ++x) {
            for (int y = minY; y <= maxY; ++y) {
                const std::string tileId = makeTileId(currentZoom, x, y);
                const ImPlotPoint minPoint{TileXToMercatorX(x, currentZoom), TileYToMercatorY(y + 1, currentZoom)};
                const ImPlotPoint maxPoint{TileXToMercatorX(x + 1, currentZoom), TileYToMercatorY(y, currentZoom)};

                if (drawOsmTiles) {
                    const GLuint gpuId = tiles.getTextureOrQueue(currentZoom, x, y);
                    if (gpuId != 0) {
                        ImPlot::PlotImage(
                            ("##tile_" + tileId).c_str(),
                            (ImTextureID)(uintptr_t)gpuId,
                            minPoint,
                            maxPoint);
                    }
                }

                if (heatmapSettings.enabled && heatmap.isAvailable()) {
                    const GLuint heatmapGpuId = heatmap.getTextureOrQueue(currentZoom, x, y);
                    if (heatmapGpuId != 0) {
                        ImPlot::PlotImage(
                            ("##heatmap_" + tileId).c_str(),
                            (ImTextureID)(uintptr_t)heatmapGpuId,
                            minPoint,
                            maxPoint);
                    }
                }
            }
        }

        if (!mapPoints.empty()) {
            std::vector<double> xs;
            std::vector<double> ys;
            xs.reserve(mapPoints.size());
            ys.reserve(mapPoints.size());

            for (const auto& point : mapPoints) {
                xs.push_back(std::clamp(point.longitude, -180.0, 180.0));
                ys.push_back(latitudeToMercatorY(point.latitude));
            }

            ImPlot::PlotLine("Route", xs.data(), ys.data(), static_cast<int>(xs.size()));
            ImPlot::PlotScatter("Points", xs.data(), ys.data(), static_cast<int>(xs.size()));
        }

        ImPlot::EndPlot();
    }

    ImGui::Separator();
    ImGui::Text("Zoom: %d", currentZoom);
    ImGui::Text("Visible tiles: x=%d..%d y=%d..%d", minX, maxX, minY, maxY);
    ImGui::Text(
        "Corners: TL(%d,%d) TR(%d,%d) BL(%d,%d) BR(%d,%d)",
        minX,
        minY,
        maxX,
        minY,
        minX,
        maxY,
        maxX,
        maxY);
    ImGui::Text("Center tile: (%d,%d)", centerX, centerY);
    ImGui::Text("Tile queue: %zu | Cache size: %zu", tiles.pendingJobs(), tiles.cachedTiles());
    if (heatmapSettings.enabled) {
        ImGui::Text(
            "Heatmap queue: %zu | Cache size: %zu",
            heatmap.pendingJobs(),
            heatmap.cachedTiles());
    }

    ImGui::End();
}
}  // namespace

void renderDashboardUI(
    const SensorData& sensorData,
    Filters& filters,
    HeatmapSettings& heatmapSettings,
    const std::unordered_map<std::string, SignalHistory>& signalHistories,
    const std::vector<LocationPoint>& mapPoints,
    const std::vector<int>& availableEarfcns,
    const std::vector<HeatmapSample>& heatmapSamples) {
    ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_None);

    const HeatmapCriterion criterionBeforeControls = heatmapSettings.criterion;
    const int earfcnBeforeControls = heatmapSettings.selectedEarfcn;

    ImGui::Begin("Global Info");
    ImGui::Text("Timestamp: %lld", sensorData.time);
    ImGui::Separator();
    ImGui::Checkbox("Filter Latitude", &filters.sendLat);
    ImGui::Checkbox("Filter Longitude", &filters.sendLon);
    ImGui::Checkbox("Filter Altitude", &filters.sendAlt);
    ImGui::Separator();

    ImGui::Checkbox("Show Heatmap", &heatmapSettings.enabled);

    const char* criteria[] = {"RSRP", "RSRQ", "RSSI", "Altitude"};
    int criterionIndex = static_cast<int>(heatmapSettings.criterion);
    if (ImGui::Combo("Heatmap Criterion", &criterionIndex, criteria, IM_ARRAYSIZE(criteria))) {
        heatmapSettings.criterion = static_cast<HeatmapCriterion>(criterionIndex);
    }

    if (availableEarfcns.empty()) {
        heatmapSettings.selectedEarfcn = -1;
        ImGui::TextUnformatted("EARFCN: All");
    } else {
        std::string selectedLabel = "All";
        if (heatmapSettings.selectedEarfcn >= 0) {
            selectedLabel = std::to_string(heatmapSettings.selectedEarfcn);
        }

        if (ImGui::BeginCombo("EARFCN", selectedLabel.c_str())) {
            const bool allSelected = heatmapSettings.selectedEarfcn < 0;
            if (ImGui::Selectable("All", allSelected)) {
                heatmapSettings.selectedEarfcn = -1;
            }
            if (allSelected) {
                ImGui::SetItemDefaultFocus();
            }

            for (int i = 0; i < static_cast<int>(availableEarfcns.size()); ++i) {
                const bool selected = availableEarfcns[i] == heatmapSettings.selectedEarfcn;
                const std::string label = std::to_string(availableEarfcns[i]);
                if (ImGui::Selectable(label.c_str(), selected)) {
                    heatmapSettings.selectedEarfcn = availableEarfcns[i];
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    int radiusMeters = static_cast<int>(std::llround(heatmapSettings.radiusMeters));
    radiusMeters = std::clamp(radiusMeters, 10, 40);
    if (ImGui::SliderInt("Interpolation Radius (m)", &radiusMeters, 10, 40)) {
        heatmapSettings.radiusMeters = static_cast<double>(radiusMeters);
    }
    ImGui::Text("Heatmap samples: %zu", heatmapSamples.size());
    ImGui::End();

    ImGui::Begin("GPS Data");
    ImGui::Text("Lat: %.6f", sensorData.latitude);
    ImGui::Text("Lon: %.6f", sensorData.longitude);
    ImGui::Text("Alt: %.2f m", sensorData.altitude);
    ImGui::End();

    if (ImPlot::BeginPlot("Signal Levels")) {
        ImPlot::SetupAxes("Time (s)", "Signal (dBm)");
        for (const auto& [key, hist] : signalHistories) {
            (void)key;
            if (hist.timestamps.empty()) {
                continue;
            }
            ImPlot::PlotLine(
                hist.label.c_str(),
                hist.timestamps.data(),
                hist.values.data(),
                static_cast<int>(hist.timestamps.size()));
        }
        ImPlot::EndPlot();
    }

    ImGui::Begin("Cellular Network Monitor");
    if (ImGui::BeginTable(
            "CellTable",
            8,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("ID (CI/NCI)");
        ImGui::TableSetupColumn("EARFCN");
        ImGui::TableSetupColumn("PCI/BSIC");
        ImGui::TableSetupColumn("TAC/LAC");
        ImGui::TableSetupColumn("Primary Signal");
        ImGui::TableSetupColumn("Quality");
        ImGui::TableSetupColumn("TA");
        ImGui::TableHeadersRow();

        for (const auto& cell : sensorData.cells) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(cell.type.c_str());

            ImGui::TableSetColumnIndex(1);
            if (cell.type == "nr") {
                ImGui::Text("%lld", cell.nci);
            } else {
                ImGui::Text("%d", cell.ci);
            }

            ImGui::TableSetColumnIndex(2);
            if (cell.earfcn >= 0) {
                ImGui::Text("%d", cell.earfcn);
            } else {
                ImGui::TextUnformatted("-");
            }

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", (cell.type == "gsm" ? cell.ci : cell.pci));

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%d", (cell.type == "gsm" ? cell.lac : cell.tac));

            ImGui::TableSetColumnIndex(5);
            if (cell.type == "lte") {
                ImGui::Text("RSRP: %d", cell.rsrp);
            } else if (cell.type == "nr") {
                ImGui::Text("SS-RSRP: %d", cell.ss_rsrp);
            } else {
                ImGui::Text("dBm: %d", cell.rssi);
            }

            ImGui::TableSetColumnIndex(6);
            if (cell.type == "lte") {
                ImGui::Text("RSRQ: %d", cell.rsrq);
            } else if (cell.type == "nr") {
                ImGui::Text("SINR: %d", cell.ss_sinr);
            } else {
                ImGui::Text("RSSI: %d", cell.rssi);
            }

            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%d", cell.ta);
        }
        ImGui::EndTable();
    }
    ImGui::End();

    static const std::vector<HeatmapSample> emptyHeatmapSamples;
    const bool heatmapNeedsNewDbSamples =
        criterionBeforeControls != heatmapSettings.criterion ||
        earfcnBeforeControls != heatmapSettings.selectedEarfcn;
    renderMapWindow(
        mapPoints,
        heatmapSettings,
        heatmapNeedsNewDbSamples ? emptyHeatmapSamples : heatmapSamples);
}
