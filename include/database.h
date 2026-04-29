#pragma once

#include "app_types.h"

#include <libpq-fe.h>
#include <vector>

PGconn* connectToDatabase(const DbConfig& config);
bool ensureDatabaseConnection(PGconn*& con, const DbConfig& config);
bool insertMobileDataToDB(PGconn* con, const SensorData& data);
std::vector<LocationPoint> fetchRecentLocationPoints(PGconn* con, std::size_t limit);
