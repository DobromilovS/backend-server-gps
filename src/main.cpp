#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <laserpants/dotenv/dotenv.h>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "app_types.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "data_processing.h"
#include "database.h"
#include "imgui.h"
#include "implot.h"
#include "ui.h"

using json = nlohmann::json;

namespace {
constexpr std::size_t kMaxHistoryPoints = 200;
constexpr std::size_t kDefaultMapPointsLimit = 0;  // 0 = load all points from DB
constexpr auto kMapRefreshInterval = std::chrono::seconds(2);
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    auto exe_dir = std::filesystem::canonical("/proc/self/exe").parent_path().string();
    std::string env_path = exe_dir + "/../.env";
    if (!std::filesystem::exists(env_path)) {
        env_path = ".env";
    }
    dotenv::init(env_path.c_str());

    auto get_env = [](const char* key) {
        const char* val = std::getenv(key);
        return val ? std::string(val) : "";
    };

    DbConfig config;
    config.host = get_env("DB_HOST");
    config.port = get_env("DB_PORT");
    config.user = get_env("DB_USER");
    config.pass = get_env("DB_PASS");
    config.name = get_env("DB_NAME");
    std::size_t map_points_limit = kDefaultMapPointsLimit;
    const std::string map_points_limit_env = get_env("MAP_POINTS_LIMIT");
    if (!map_points_limit_env.empty()) {
        try {
            map_points_limit = static_cast<std::size_t>(std::stoull(map_points_limit_env));
        } catch (...) {
            std::cerr << "Предупреждение: MAP_POINTS_LIMIT некорректен, используем значение по умолчанию.\n";
            map_points_limit = kDefaultMapPointsLimit;
        }
    }

    if (config.host.empty() || config.port.empty() || config.user.empty() || config.pass.empty() ||
        config.name.empty()) {
        std::cerr << "Ошибка: Не все данные для БД указаны в .env!\n";
        return 1;
    }

    std::cout << "Подключаемся к: " << config.host << "...\n";
    PGconn* db_connection = connectToDatabase(config);
    if (!db_connection) {
        return 1;
    }

    SensorData sensorData;
    Filters filters;
    std::unordered_map<std::string, SignalHistory> signalHistories;
    std::vector<LocationPoint> mapPoints;
    auto last_map_refresh = std::chrono::steady_clock::now() - kMapRefreshInterval;

    const std::string output_dir = "./Location_save_data";
    std::filesystem::create_directories(output_dir);
    const std::string log_file_path = output_dir + "/received_data.jsonl";

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "ZMQ Remote Control Dashboard",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1280,
        800,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);

    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    std::string zmq_bind_endpoint = get_env("ZMQ_BIND_ENDPOINT");
    if (zmq_bind_endpoint.empty()) {
        zmq_bind_endpoint = "tcp://*:7777";
    }

    try {
        socket.bind(zmq_bind_endpoint);
    } catch (const zmq::error_t& e) {
        std::cerr << "Ошибка bind для ZMQ endpoint '" << zmq_bind_endpoint << "': " << e.what()
                  << "\n";
        if (e.num() == EADDRINUSE) {
            std::cerr << "Порт уже занят. Останови старый процесс или задай другой endpoint в .env:\n"
                      << "ZMQ_BIND_ENDPOINT=tcp://*:7778\n";
        }
        return 1;
    }

    auto refresh_map_points = [&]() {
        if (!ensureDatabaseConnection(db_connection, config)) {
            return;
        }
        mapPoints = fetchRecentLocationPoints(db_connection, map_points_limit);
        last_map_refresh = std::chrono::steady_clock::now();
    };

    refresh_map_points();

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }

        zmq::message_t request;
        bool need_refresh_map = false;
        if (socket.recv(request, zmq::recv_flags::dontwait)) {
            std::string received_data(static_cast<char*>(request.data()), request.size());

            std::ofstream log_file(log_file_path, std::ios::app);
            if (log_file.is_open()) {
                log_file << received_data << '\n';
                log_file.close();
            }

            try {
                json j = json::parse(received_data);

                if (j.contains("location")) {
                    auto loc = j["location"];
                    sensorData.latitude = loc.value("_Latitude", 0.0);
                    sensorData.longitude = loc.value("_Longitude", 0.0);
                    sensorData.altitude = loc.value("_Altitude", 0.0);
                }
                sensorData.time = j.value("time", 0LL);

                if (j.contains("cells")) {
                    parse_cell_info(j["cells"], sensorData);
                    update_signal_histories(sensorData, signalHistories, kMaxHistoryPoints);
                }

                sensorData.updated = true;

                if (ensureDatabaseConnection(db_connection, config)) {
                    if (!insertMobileDataToDB(db_connection, sensorData)) {
                        std::cerr << "Пакет не записан в БД из-за ошибки транзакции.\n";
                    } else {
                        need_refresh_map = true;
                    }
                } else {
                    std::cerr << "Пакет не записан: нет подключения к БД.\n";
                }

                json resp;
                resp["f_lat"] = filters.sendLat;
                resp["f_lon"] = filters.sendLon;
                resp["f_alt"] = filters.sendAlt;
                std::string s = resp.dump();
                socket.send(zmq::message_t(s.data(), s.size()), zmq::send_flags::none);
            } catch (...) {
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (need_refresh_map || now - last_map_refresh >= kMapRefreshInterval) {
            refresh_map_points();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        renderDashboardUI(sensorData, filters, signalHistories, mapPoints);

        ImGui::Render();
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    if (db_connection) {
        PQfinish(db_connection);
    }
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
