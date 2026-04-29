#pragma once

#include "app_types.h"

#include <string>
#include <unordered_map>
#include <vector>

void renderDashboardUI(
    const SensorData& sensorData,
    Filters& filters,
    const std::unordered_map<std::string, SignalHistory>& signalHistories,
    const std::vector<LocationPoint>& mapPoints);
