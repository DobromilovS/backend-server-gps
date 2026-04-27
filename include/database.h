#pragma once

#include "app_types.h"

#include <libpq-fe.h>

PGconn* connectToDatabase(const DbConfig& config);
bool ensureDatabaseConnection(PGconn*& con, const DbConfig& config);
bool insertMobileDataToDB(PGconn* con, const SensorData& data);
