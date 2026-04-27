#pragma once

#include "app_types.h"

#include <string>
#include <unordered_map>

void renderDashboardUI(
    const SensorData& sensorData,
    Filters& filters,
    const std::unordered_map<std::string, SignalHistory>& signalHistories);
