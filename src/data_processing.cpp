#include "data_processing.h"

#include <string>
#include <vector>

using json = nlohmann::json;

void parse_cell_info(const json& j_cells, SensorData& sensorData) {
    sensorData.cells.clear();
    for (auto& item : j_cells) {
        CellData c;
        c.type = item.value("type", "unknown");
        c.earfcn = item.value("earfcn", item.value("EARFCN", item.value("arfcn", -1)));
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
        sensorData.cells.push_back(c);
    }
}

void update_signal_histories(
    const SensorData& data,
    std::unordered_map<std::string, SignalHistory>& signalHistories,
    std::size_t maxHistoryPoints) {
    const double current_time = data.time / 1000.0;

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

        auto it = signalHistories.find(cell_id);
        if (it == signalHistories.end()) {
            SignalHistory hist;
            hist.pci = pci;
            hist.label = cell.type + " CI:" +
                         (cell.type == "nr" ? std::to_string(cell.nci) : std::to_string(cell.ci)) +
                         " PCI:" + std::to_string(pci);
            signalHistories[cell_id] = hist;
            it = signalHistories.find(cell_id);
        }

        it->second.timestamps.push_back(current_time);
        it->second.values.push_back(signal_value);

        if (it->second.timestamps.size() > maxHistoryPoints) {
            it->second.timestamps.erase(it->second.timestamps.begin());
            it->second.values.erase(it->second.values.begin());
        }
    }

    const double now = current_time;
    for (auto it = signalHistories.begin(); it != signalHistories.end();) {
        if (!it->second.timestamps.empty() && (now - it->second.timestamps.back()) > 300.0) {
            it = signalHistories.erase(it);
        } else {
            ++it;
        }
    }
}

void from_json(const json& j, CellData& c) {
    c.type = j.value("type", "unknown");
    c.earfcn = j.value("earfcn", j.value("EARFCN", j.value("arfcn", -1)));
    if (c.type == "lte") {
        c.ci = j.value("ci", 0);
        c.pci = j.value("pci", 0);
        c.tac = j.value("tac", 0);
        c.rsrp = j.value("rsrp", 0);
    } else if (c.type == "nr") {
        c.nci = j.value("nci", 0LL);
        c.ss_rsrp = j.value("ssRsrp", 0);
    }
}

void from_json(const json& j, SensorData& s) {
    if (j.contains("location")) {
        const auto& loc = j["location"];
        s.latitude = loc.value("_Latitude", 0.0);
        s.longitude = loc.value("_Longitude", 0.0);
        s.altitude = loc.value("_Altitude", 0.0);
    }
    s.time = j.value("time", 0LL);

    if (j.contains("cells")) {
        s.cells = j.at("cells").get<std::vector<CellData>>();
    }
}
