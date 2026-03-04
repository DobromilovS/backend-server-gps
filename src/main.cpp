#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <iostream>
#include <string>
#include <zmq.hpp>
#include <nlohmann/json.hpp>

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

struct SensorData {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    long long time = 0;
    bool updated = false;
} g_sensorData;

int main(int argc, char *argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "Ошибка инициализации SDL: " << SDL_GetError() << std::endl;
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "ZMQ Sensor Dashboard",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1024, 768,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.bind("tcp://*:7777");
    std::cout << ">>> ZMQ сервер запущен на порту 7777" << std::endl;

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
        auto recv_result = socket.recv(request, zmq::recv_flags::dontwait);
        
        if (recv_result) {
            std::string received_data(static_cast<char*>(request.data()), request.size());
            try {
                json j = json::parse(received_data);
                g_sensorData.latitude = j.value("_Latitude", 0.0);
                g_sensorData.longitude = j.value("_Longitude", 0.0);
                g_sensorData.altitude = j.value("_Altitude", 0.0);
                g_sensorData.time = j.value("_Time", 0LL);
                g_sensorData.updated = true;
                
                json command_json;
                command_json["f_lat"] = g_filters.sendLat;
                command_json["f_lon"] = g_filters.sendLon;
                command_json["f_alt"] = g_filters.sendAlt;

                std::string response = command_json.dump();
                zmq::message_t reply(response.data(), response.size());
                socket.send(reply, zmq::send_flags::none);
                
            } catch (const std::exception& e) {
                std::cerr << "Ошибка парсинга JSON: " << e.what() << std::endl;
                std::string error = "ERROR: Invalid JSON";
                zmq::message_t reply(error.data(), error.size());
                socket.send(reply, zmq::send_flags::none);
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        
        ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_None);

        {
            ImGui::Begin("Sensor Data");
            if (g_sensorData.updated) {
                ImGui::Text("Latitude:  %.8f", g_sensorData.latitude);
                ImGui::Text("Longitude: %.8f", g_sensorData.longitude);
                ImGui::Text("Altitude:  %.2f m", g_sensorData.altitude);
                ImGui::Text("Timestamp: %lld", g_sensorData.time);
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Status: Connection Active / Data Received");

                ImGui::Begin("Settings");
                ImGui::Checkbox("Send Latitude", &g_filters.sendLat);
                ImGui::Checkbox("Send Longitude", &g_filters.sendLon);
                ImGui::Checkbox("Send Altitude", &g_filters.sendAlt);
                ImGui::End();

            } else {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Waiting for ZMQ data on port 7777...");
            }
            ImGui::End();
        }

        ImGui::Render();
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);

        SDL_Delay(1); 
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