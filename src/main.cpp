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

using json = nlohmann::json;

struct Filters {
    bool sendLat = true;
    bool sendLon = true;
    bool sendAlt = true;
} g_filters;

struct CellData {
    std::string type;
    int ci = 0, pci = 0, tac = 0, mcc = 0, mnc = 0;
    int rsrp = 0, rsrq = 0, rssi = 0, rssnr = 0, ta = 0;
    int lac = 0, bsic = 0, arfcn = 0, band = 0;
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
            c.bsic = item.value("bsic", 0);
            c.arfcn = item.value("arfcn", 0);
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
            pci = cell.bsic;
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

int main(int argc, char *argv[]) {
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
                ImGui::Text("%d", (cell.type == "gsm" ? cell.bsic : cell.pci));

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
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}