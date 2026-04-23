#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <iostream>
#include <string>
#include <vector>
#include <zmq.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <cmath>
#include "implot.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "imgui.h"
#include <laserpants/dotenv/dotenv.h>
#include <libpq-fe.h>

#include <cstdlib>


using json = nlohmann::json;


struct DbConfig {
    std::string host;
    std::string port;
    std::string user;
    std::string pass;
    std::string name;

    std::string getConnectionString() const {
        return "host=" + host + " port=" + port + " user=" + user + 
               " password=" + pass + " dbname=" + name;
    }
};

struct Filters {
    bool sendLat = true;
    bool sendLon = true;
    bool sendAlt = true;
} g_filters;

struct CellData {
    std::string type;
    int ci = 0, pci = 0, tac = 0;
    int rsrp = 0, rsrq = 0, rssi = 0, ta = 0;
    int lac = 0;
    long long nci = 0;
    int ss_rsrp = 0, ss_rsrq = 0, ss_sinr = 0;
};

struct SensorData {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    long long time = 0;
    std::vector<CellData> cells;
    bool updated = false;
} g_sensorData;

struct SignalHistory {
    std::vector<double> timestamps;
    std::vector<double> values;
    int pci;
    std::string label;
};

struct MobileDataCollection {
    SensorData sensorData;
    std::unordered_map<std::string, SignalHistory> signalHistories;
    size_t maxHistoryPoints = 200;

    MobileDataCollection() = default;
    MobileDataCollection(const SensorData& sensor, 
                         const std::unordered_map<std::string, SignalHistory>& histories)
        : sensorData(sensor)
        , signalHistories(histories) {
    }
    
    void clear() {
        sensorData = SensorData{};
        signalHistories.clear();
    }

    bool isEmpty() const {
        return sensorData.cells.empty() && signalHistories.empty();
    }
};

std::unordered_map<std::string, SignalHistory> g_signalHistories;
const size_t MAX_HISTORY_POINTS = 200;

void parse_cell_info(const json& j_cells) {
    g_sensorData.cells.clear();
    for (auto& item : j_cells) {
        CellData c;
        c.type = item.value("type", "unknown");
        if (c.type == "lte") {
            c.ci = item.value("ci", 0);
            c.pci = item.value("pci", 0);
            c.tac = item.value("tac", 0);
            c.rsrp = item.value("rsrp", 0);
            c.rsrq = item.value("rsrq", 0);
            c.rssi = item.value("rssi", 0);
            c.ta = item.value("ta", 0);
        } else if (c.type == "gsm") {
            c.ci = item.value("ci", 0);
            c.lac = item.value("lac", 0);
            c.rssi = item.value("rssi", 0);
            c.ta = item.value("ta", 0);
        } else if (c.type == "nr") {
            c.nci = item.value("nci", 0LL);
            c.pci = item.value("pci", 0);
            c.tac = item.value("tac", 0);
            c.ss_rsrp = item.value("ssRsrp", 0);
            c.ss_rsrq = item.value("ssRsrq", 0);
            c.ss_sinr = item.value("ssSinr", 0);
        }
        g_sensorData.cells.push_back(c);
    }
}

void update_signal_histories(const SensorData& data) {
    double current_time = data.time / 1000.0;

    for (const auto& cell : data.cells) {
        std::string cell_id;
        double signal_value = 0.0;
        int pci = 0;

        if (cell.type == "lte") {
            cell_id = "lte_" + std::to_string(cell.ci);
            signal_value = cell.rsrp;
            pci = cell.pci;
        } else if (cell.type == "nr") {
            cell_id = "nr_" + std::to_string(cell.nci);
            signal_value = cell.ss_rsrp;
            pci = cell.pci;
        } else if (cell.type == "gsm") {
            cell_id = "gsm_" + std::to_string(cell.ci);
            signal_value = cell.rssi;
            pci = cell.ci;
        } else {
            continue;
        }

        auto it = g_signalHistories.find(cell_id);
        if (it == g_signalHistories.end()) {
            SignalHistory hist;
            hist.pci = pci;
            hist.label = cell.type + " CI:" + (cell.type=="nr"?std::to_string(cell.nci):std::to_string(cell.ci)) + " PCI:" + std::to_string(pci);
            g_signalHistories[cell_id] = hist;
            it = g_signalHistories.find(cell_id);
        }

        it->second.timestamps.push_back(current_time);
        it->second.values.push_back(signal_value);

        if (it->second.timestamps.size() > MAX_HISTORY_POINTS) {
            it->second.timestamps.erase(it->second.timestamps.begin());
            it->second.values.erase(it->second.values.begin());
        }
    }

    double now = current_time;
    for (auto it = g_signalHistories.begin(); it != g_signalHistories.end(); ) {
        if (!it->second.timestamps.empty() && (now - it->second.timestamps.back()) > 300.0) {
            it = g_signalHistories.erase(it);
        } else {
            ++it;
        }
    }
}

void from_json(const json& j, CellData& c) {
    c.type = j.value("type", "unknown");
    if (c.type == "lte") {
        c.ci   = j.value("ci", 0);
        c.pci  = j.value("pci", 0);
        c.tac  = j.value("tac", 0);
        c.rsrp = j.value("rsrp", 0);
    } else if (c.type == "nr") {
        c.nci  = j.value("nci", 0LL);
        c.ss_rsrp = j.value("ssRsrp", 0);
    }
}

void from_json(const json& j, SensorData& s) {
    if (j.contains("location")) {
        auto loc = j["location"];
        s.latitude  = loc.value("_Latitude", 0.0);
        s.longitude = loc.value("_Longitude", 0.0);
        s.altitude  = loc.value("_Altitude", 0.0);
    }
    s.time = j.value("time", 0LL);
    
    if (j.contains("cells")) {
        s.cells = j.at("cells").get<std::vector<CellData>>();
    }
}

PGconn* connectToDatabase(const DbConfig& config) {
    std::string conn_str = config.getConnectionString();
    PGconn* con = PQconnectdb(conn_str.c_str());

    if (!con || PQstatus(con) != CONNECTION_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m: Не удалось подключиться к БД: "
                  << (con ? PQerrorMessage(con) : "PQconnectdb вернул nullptr") << "\n";
        if (con) {
            PQfinish(con);
        }
        return nullptr;
    }

    return con;
}

bool ensureDatabaseConnection(PGconn*& con, const DbConfig& config) {
    if (con && PQstatus(con) == CONNECTION_OK) {
        return true;
    }

    if (con) {
        PQfinish(con);
        con = nullptr;
    }

    con = connectToDatabase(config);
    return con != nullptr;
}

bool insertMobileDataToDB(PGconn* con, const SensorData& data) {
    if (!con || PQstatus(con) != CONNECTION_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m: Нет активного подключения к БД.\n";
        return false;
    }

    PGresult* tx_begin = PQexec(con, "BEGIN");
    if (PQresultStatus(tx_begin) != PGRES_COMMAND_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m: Не удалось начать транзакцию: "
                  << PQresultErrorMessage(tx_begin) << "\n";
        PQclear(tx_begin);
        return false;
    }
    PQclear(tx_begin);

    auto rollback = [con]() {
        PGresult* tx_rollback = PQexec(con, "ROLLBACK");
        if (tx_rollback) {
            PQclear(tx_rollback);
        }
    };
    
    std::string lat_str = std::to_string(data.latitude);
    std::string lon_str = std::to_string(data.longitude);
    std::string alt_str = std::to_string(data.altitude);
    std::string time_str = std::to_string(data.time);
    
    const char* location_params[] = {
        lat_str.c_str(),
        lon_str.c_str(),
        alt_str.c_str(),
        time_str.c_str()
    };
    
    std::string location_query = "INSERT INTO location_data (latitude, longitude, altitude, timestamp) VALUES ($1, $2, $3, $4) RETURNING id";
    
    PGresult* location_res = PQexecParams(
        con,
        location_query.c_str(),
        4,
        NULL,
        location_params,
        NULL,
        NULL,
        0
    );
    
    int location_id = 0;
    
    if (PQresultStatus(location_res) != PGRES_TUPLES_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m при вставке location_data: " 
                  << PQresultErrorMessage(location_res) << "\n";
        PQclear(location_res);
        rollback();
        return false;
    } else {
        if (PQntuples(location_res) > 0) {
            location_id = std::stoi(PQgetvalue(location_res, 0, 0));
            std::cout << "Location data вставлена \033[32mУСПЕШНО!\033[0m ID: " << location_id << "\n";
        }
    }
    
    PQclear(location_res);
    for (const auto& cell : data.cells) {
        std::string cell_type = cell.type;
        std::string ci_str = (cell.type == "nr") ? "" : std::to_string(cell.ci);
        std::string pci_str = std::to_string(cell.pci);
        std::string tac_str = std::to_string(cell.tac);
        std::string rsrp_str = std::to_string(cell.rsrp);
        std::string rsrq_str = std::to_string(cell.rsrq);
        std::string rssi_str = std::to_string(cell.rssi);
        std::string ta_str = std::to_string(cell.ta);
        std::string lac_str = std::to_string(cell.lac);
        std::string nci_str = (cell.type == "nr") ? std::to_string(cell.nci) : "";
        std::string ss_rsrp_str = std::to_string(cell.ss_rsrp);
        std::string ss_rsrq_str = std::to_string(cell.ss_rsrq);
        std::string ss_sinr_str = std::to_string(cell.ss_sinr);
        std::string location_id_str = std::to_string(location_id);

        const char* cell_params[] = {
            location_id_str.c_str(),
            cell_type.c_str(),
            ci_str.empty() ? NULL : ci_str.c_str(),
            pci_str.c_str(),
            tac_str.c_str(),
            rsrp_str.c_str(),
            rsrq_str.c_str(),
            rssi_str.c_str(),
            ta_str.c_str(),
            lac_str.c_str(),
            nci_str.empty() ? NULL : nci_str.c_str(),
            ss_rsrp_str.c_str(),
            ss_rsrq_str.c_str(),
            ss_sinr_str.c_str()
        };

        std::string cell_query = "INSERT INTO cell_data (location_id, cell_type, ci, pci, tac, rsrp, rsrq, rssi, ta, lac, nci, ss_rsrp, ss_rsrq, ss_sinr) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14)";

        PGresult* cell_res = PQexecParams(
            con,
            cell_query.c_str(),
            14,
            NULL,
            cell_params,
            NULL,
            NULL,
            0
        );
        if (PQresultStatus(cell_res) != PGRES_COMMAND_OK) {
            std::cerr << "\033[31mОШИБКА\033[0m при вставке cell_data: " 
                      << PQresultErrorMessage(cell_res) << "\n";
            PQclear(cell_res);
            rollback();
            return false;
        } else {
            std::cout << "Cell data (тип: " << cell.type << ") вставлена \033[32mУСПЕШНО!\033[0m\n";
        }
        
        PQclear(cell_res);
    }

    PGresult* tx_commit = PQexec(con, "COMMIT");
    if (PQresultStatus(tx_commit) != PGRES_COMMAND_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m: Не удалось зафиксировать транзакцию: "
                  << PQresultErrorMessage(tx_commit) << "\n";
        PQclear(tx_commit);
        rollback();
        return false;
    }
    PQclear(tx_commit);
    return true;
}

int main(int argc, char *argv[]) {
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

    if (config.host.empty() || config.port.empty() || config.user.empty() || config.pass.empty() || config.name.empty()) {
        std::cerr << "Ошибка: Не все данные для БД указаны в .env!" << std::endl;
        return 1;
    }

    std::cout << "Подключаемся к: " << config.host << "..." << std::endl;
    PGconn* db_connection = connectToDatabase(config);
    if (!db_connection) {
        return 1;
    }

    const std::string output_dir = "./Location_save_data"; 
    std::filesystem::create_directories(output_dir);

    std::string log_file_path = output_dir + "/received_data.jsonl";

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return -1;

    SDL_Window* window = SDL_CreateWindow("ZMQ Remote Control Dashboard", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.bind("tcp://*:7777");

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
        }

        zmq::message_t request;
        if (socket.recv(request, zmq::recv_flags::dontwait)) {
            std::string received_data(static_cast<char*>(request.data()), request.size());
            
            std::ofstream log_file(log_file_path, std::ios::app);
            if (log_file.is_open()) {
                log_file << received_data << std::endl;
                log_file.close();
            }
            
            try {
                json j = json::parse(received_data);
                
                if (j.contains("location")) {
                    auto loc = j["location"];
                    g_sensorData.latitude = loc.value("_Latitude", 0.0);
                    g_sensorData.longitude = loc.value("_Longitude", 0.0);
                    g_sensorData.altitude = loc.value("_Altitude", 0.0);
                }
                g_sensorData.time = j.value("time", 0LL);

                if (j.contains("cells")) {
                    parse_cell_info(j["cells"]);
                    update_signal_histories(g_sensorData);
                }

                g_sensorData.updated = true;

                if (ensureDatabaseConnection(db_connection, config)) {
                    if (!insertMobileDataToDB(db_connection, g_sensorData)) {
                        std::cerr << "Пакет не записан в БД из-за ошибки транзакции.\n";
                    }
                } else {
                    std::cerr << "Пакет не записан: нет подключения к БД.\n";
                }
                
                json resp;
                resp["f_lat"] = g_filters.sendLat;
                resp["f_lon"] = g_filters.sendLon;
                resp["f_alt"] = g_filters.sendAlt;
                std::string s = resp.dump();
                socket.send(zmq::message_t(s.data(), s.size()), zmq::send_flags::none);

            } catch (...) {}
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_None);

        ImGui::Begin("Global Info");
        ImGui::Text("Timestamp: %lld", g_sensorData.time);
        ImGui::Separator();
        ImGui::Checkbox("Filter Latitude", &g_filters.sendLat);
        ImGui::Checkbox("Filter Longitude", &g_filters.sendLon);
        ImGui::Checkbox("Filter Altitude", &g_filters.sendAlt);
        ImGui::End();

        ImGui::Begin("GPS Data");
        ImGui::Text("Lat: %.6f", g_sensorData.latitude);
        ImGui::Text("Lon: %.6f", g_sensorData.longitude);
        ImGui::Text("Alt: %.2f m", g_sensorData.altitude);
        ImGui::End();

        if (ImPlot::BeginPlot("Signal Levels")) {
            ImPlot::SetupAxes("Time (s)", "Signal (dBm)");
            for (const auto& [key, hist] : g_signalHistories) {
                if (hist.timestamps.empty()) continue;
                ImPlot::PlotLine(hist.label.c_str(), hist.timestamps.data(), hist.values.data(), hist.timestamps.size());
            }
            ImPlot::EndPlot();
        }

        ImGui::Begin("Cellular Network Monitor");
        if (ImGui::BeginTable("CellTable", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("ID (CI/NCI)");
            ImGui::TableSetupColumn("PCI/BSIC");
            ImGui::TableSetupColumn("TAC/LAC");
            ImGui::TableSetupColumn("Primary Signal");
            ImGui::TableSetupColumn("Quality");
            ImGui::TableSetupColumn("TA");
            ImGui::TableHeadersRow();

            for (const auto& cell : g_sensorData.cells) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(cell.type.c_str());
                
                ImGui::TableSetColumnIndex(1);
                if (cell.type == "nr") ImGui::Text("%lld", cell.nci);
                else ImGui::Text("%d", cell.ci);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d", (cell.type == "gsm" ? cell.ci : cell.pci));

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d", (cell.type == "gsm" ? cell.lac : cell.tac));

                ImGui::TableSetColumnIndex(4);
                if (cell.type == "lte") ImGui::Text("RSRP: %d", cell.rsrp);
                else if (cell.type == "nr") ImGui::Text("SS-RSRP: %d", cell.ss_rsrp);
                else ImGui::Text("dBm: %d", cell.rssi);

                ImGui::TableSetColumnIndex(5);
                if (cell.type == "lte") ImGui::Text("RSRQ: %d", cell.rsrq);
                else if (cell.type == "nr") ImGui::Text("SINR: %d", cell.ss_sinr);
                else ImGui::Text("RSSI: %d", cell.rssi);

                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%d", cell.ta);
            }
            ImGui::EndTable();
        }
        ImGui::End();

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
