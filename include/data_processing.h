#pragma once

#include "app_types.h"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

void parse_cell_info(const nlohmann::json& j_cells, SensorData& sensorData);
void update_signal_histories(
    const SensorData& data,
    std::unordered_map<std::string, SignalHistory>& signalHistories,
    std::size_t maxHistoryPoints);

void from_json(const nlohmann::json& j, CellData& c);
void from_json(const nlohmann::json& j, SensorData& s);
