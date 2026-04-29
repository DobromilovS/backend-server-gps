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

void renderMapWindow(const std::vector<LocationPoint>& mapPoints) {
    ImGui::Begin("OSM Mercator Map");

    TileManager& tiles = TileManager::instance();
    ImGui::TextUnformatted(tiles.statusText().c_str());
    ImGui::Text("Points from DB: %zu", mapPoints.size());

    if (!tiles.isAvailable()) {
        ImGui::TextUnformatted("Map disabled: tile fetcher is unavailable.");
        ImGui::End();
        return;
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
                const GLuint gpuId = tiles.getTextureOrQueue(currentZoom, x, y);
                if (gpuId == 0) {
                    continue;
                }

                const std::string tileId = makeTileId(currentZoom, x, y);
                const ImPlotPoint minPoint{TileXToMercatorX(x, currentZoom), TileYToMercatorY(y + 1, currentZoom)};
                const ImPlotPoint maxPoint{TileXToMercatorX(x + 1, currentZoom), TileYToMercatorY(y, currentZoom)};
                ImPlot::PlotImage(
                    ("##tile_" + tileId).c_str(),
                    (ImTextureID)(uintptr_t)gpuId,
                    minPoint,
                    maxPoint);
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

    ImGui::End();
}
}  // namespace

void renderDashboardUI(
    const SensorData& sensorData,
    Filters& filters,
    const std::unordered_map<std::string, SignalHistory>& signalHistories,
    const std::vector<LocationPoint>& mapPoints) {
    ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_None);

    ImGui::Begin("Global Info");
    ImGui::Text("Timestamp: %lld", sensorData.time);
    ImGui::Separator();
    ImGui::Checkbox("Filter Latitude", &filters.sendLat);
    ImGui::Checkbox("Filter Longitude", &filters.sendLon);
    ImGui::Checkbox("Filter Altitude", &filters.sendAlt);
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
            7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("ID (CI/NCI)");
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
            ImGui::Text("%d", (cell.type == "gsm" ? cell.ci : cell.pci));

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", (cell.type == "gsm" ? cell.lac : cell.tac));

            ImGui::TableSetColumnIndex(4);
            if (cell.type == "lte") {
                ImGui::Text("RSRP: %d", cell.rsrp);
            } else if (cell.type == "nr") {
                ImGui::Text("SS-RSRP: %d", cell.ss_rsrp);
            } else {
                ImGui::Text("dBm: %d", cell.rssi);
            }

            ImGui::TableSetColumnIndex(5);
            if (cell.type == "lte") {
                ImGui::Text("RSRQ: %d", cell.rsrq);
            } else if (cell.type == "nr") {
                ImGui::Text("SINR: %d", cell.ss_sinr);
            } else {
                ImGui::Text("RSSI: %d", cell.rssi);
            }

            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%d", cell.ta);
        }
        ImGui::EndTable();
    }
    ImGui::End();

    renderMapWindow(mapPoints);
}
