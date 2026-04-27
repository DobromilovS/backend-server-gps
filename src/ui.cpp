#include "ui.h"

#include "implot.h"
#include "imgui.h"

void renderDashboardUI(
    const SensorData& sensorData,
    Filters& filters,
    const std::unordered_map<std::string, SignalHistory>& signalHistories) {
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
}
