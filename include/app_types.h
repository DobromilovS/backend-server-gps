#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

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
};

struct CellData {
    std::string type;
    int ci = 0;
    int pci = 0;
    int tac = 0;
    int rsrp = 0;
    int rsrq = 0;
    int rssi = 0;
    int ta = 0;
    int lac = 0;
    long long nci = 0;
    int ss_rsrp = 0;
    int ss_rsrq = 0;
    int ss_sinr = 0;
};

struct SensorData {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    long long time = 0;
    std::vector<CellData> cells;
    bool updated = false;
};

struct SignalHistory {
    std::vector<double> timestamps;
    std::vector<double> values;
    int pci = 0;
    std::string label;
};

struct MobileDataCollection {
    SensorData sensorData;
    std::unordered_map<std::string, SignalHistory> signalHistories;
    std::size_t maxHistoryPoints = 200;

    MobileDataCollection() = default;
    MobileDataCollection(
        const SensorData& sensor,
        const std::unordered_map<std::string, SignalHistory>& histories)
        : sensorData(sensor),
          signalHistories(histories) {}

    void clear() {
        sensorData = SensorData{};
        signalHistories.clear();
    }

    bool isEmpty() const {
        return sensorData.cells.empty() && signalHistories.empty();
    }
};
