#include "database.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace {
const char* metricExpression(HeatmapCriterion criterion) {
    switch (criterion) {
        case HeatmapCriterion::RSRP:
            return "c.rsrp";
        case HeatmapCriterion::RSRQ:
            return "c.rsrq";
        case HeatmapCriterion::RSSI:
            return "c.rssi";
        case HeatmapCriterion::Altitude:
            return "l.altitude";
    }
    return "c.rsrp";
}
}  // namespace

PGconn* connectToDatabase(const DbConfig& config) {
    const std::string conn_str = config.getConnectionString();
    PGconn* con = PQconnectdb(conn_str.c_str());

    if (!con || PQstatus(con) != CONNECTION_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m: Не удалось подключиться к БД: "
                  << (con ? PQerrorMessage(con) : "PQconnectdb вернул nullptr") << "\n";
        if (con) {
            PQfinish(con);
        }
        return nullptr;
    }

    return con;
}

bool ensureDatabaseConnection(PGconn*& con, const DbConfig& config) {
    if (con && PQstatus(con) == CONNECTION_OK) {
        return true;
    }

    if (con) {
        PQfinish(con);
        con = nullptr;
    }

    con = connectToDatabase(config);
    return con != nullptr;
}

bool ensureDatabaseSchema(PGconn* con) {
    if (!con || PQstatus(con) != CONNECTION_OK) {
        return false;
    }

    const char* statements[] = {
        "ALTER TABLE cell_data ADD COLUMN IF NOT EXISTS earfcn INTEGER",
        "CREATE INDEX IF NOT EXISTS idx_cell_data_earfcn ON cell_data(earfcn)"};

    for (const char* statement : statements) {
        PGresult* res = PQexec(con, statement);
        if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
            std::cerr << "\033[31mОШИБКА\033[0m: Не удалось обновить схему БД: "
                      << (res ? PQresultErrorMessage(res) : "PQexec вернул nullptr") << "\n";
            if (res) {
                PQclear(res);
            }
            return false;
        }
        PQclear(res);
    }

    return true;
}

bool insertMobileDataToDB(PGconn* con, const SensorData& data) {
    if (!con || PQstatus(con) != CONNECTION_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m: Нет активного подключения к БД.\n";
        return false;
    }

    PGresult* tx_begin = PQexec(con, "BEGIN");
    if (PQresultStatus(tx_begin) != PGRES_COMMAND_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m: Не удалось начать транзакцию: "
                  << PQresultErrorMessage(tx_begin) << "\n";
        PQclear(tx_begin);
        return false;
    }
    PQclear(tx_begin);

    auto rollback = [con]() {
        PGresult* tx_rollback = PQexec(con, "ROLLBACK");
        if (tx_rollback) {
            PQclear(tx_rollback);
        }
    };

    const std::string lat_str = std::to_string(data.latitude);
    const std::string lon_str = std::to_string(data.longitude);
    const std::string alt_str = std::to_string(data.altitude);
    const std::string time_str = std::to_string(data.time);

    const char* location_params[] = {
        lat_str.c_str(),
        lon_str.c_str(),
        alt_str.c_str(),
        time_str.c_str()};

    const std::string location_query =
        "INSERT INTO location_data (latitude, longitude, altitude, timestamp) "
        "VALUES ($1, $2, $3, $4) RETURNING id";

    PGresult* location_res = PQexecParams(
        con, location_query.c_str(), 4, nullptr, location_params, nullptr, nullptr, 0);

    int location_id = 0;

    if (PQresultStatus(location_res) != PGRES_TUPLES_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m при вставке location_data: "
                  << PQresultErrorMessage(location_res) << "\n";
        PQclear(location_res);
        rollback();
        return false;
    }

    if (PQntuples(location_res) > 0) {
        location_id = std::stoi(PQgetvalue(location_res, 0, 0));
        std::cout << "Location data вставлена \033[32mУСПЕШНО!\033[0m ID: " << location_id
                  << "\n";
    }
    PQclear(location_res);

    for (const auto& cell : data.cells) {
        const std::string cell_type = cell.type;
        const std::string ci_str = (cell.type == "nr") ? "" : std::to_string(cell.ci);
        const std::string earfcn_str = cell.earfcn >= 0 ? std::to_string(cell.earfcn) : "";
        const std::string pci_str = std::to_string(cell.pci);
        const std::string tac_str = std::to_string(cell.tac);
        const std::string rsrp_str = std::to_string(cell.rsrp);
        const std::string rsrq_str = std::to_string(cell.rsrq);
        const std::string rssi_str = std::to_string(cell.rssi);
        const std::string ta_str = std::to_string(cell.ta);
        const std::string lac_str = std::to_string(cell.lac);
        const std::string nci_str = (cell.type == "nr") ? std::to_string(cell.nci) : "";
        const std::string ss_rsrp_str = std::to_string(cell.ss_rsrp);
        const std::string ss_rsrq_str = std::to_string(cell.ss_rsrq);
        const std::string ss_sinr_str = std::to_string(cell.ss_sinr);
        const std::string location_id_str = std::to_string(location_id);

        const char* cell_params[] = {
            location_id_str.c_str(),
            cell_type.c_str(),
            ci_str.empty() ? nullptr : ci_str.c_str(),
            earfcn_str.empty() ? nullptr : earfcn_str.c_str(),
            pci_str.c_str(),
            tac_str.c_str(),
            rsrp_str.c_str(),
            rsrq_str.c_str(),
            rssi_str.c_str(),
            ta_str.c_str(),
            lac_str.c_str(),
            nci_str.empty() ? nullptr : nci_str.c_str(),
            ss_rsrp_str.c_str(),
            ss_rsrq_str.c_str(),
            ss_sinr_str.c_str()};

        const std::string cell_query =
            "INSERT INTO cell_data (location_id, cell_type, ci, earfcn, pci, tac, rsrp, rsrq, rssi, "
            "ta, lac, nci, ss_rsrp, ss_rsrq, ss_sinr) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15)";

        PGresult* cell_res = PQexecParams(
            con, cell_query.c_str(), 15, nullptr, cell_params, nullptr, nullptr, 0);
        if (PQresultStatus(cell_res) != PGRES_COMMAND_OK) {
            std::cerr << "\033[31mОШИБКА\033[0m при вставке cell_data: "
                      << PQresultErrorMessage(cell_res) << "\n";
            PQclear(cell_res);
            rollback();
            return false;
        }

        std::cout << "Cell data (тип: " << cell.type << ") вставлена \033[32mУСПЕШНО!\033[0m\n";
        PQclear(cell_res);
    }

    PGresult* tx_commit = PQexec(con, "COMMIT");
    if (PQresultStatus(tx_commit) != PGRES_COMMAND_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m: Не удалось зафиксировать транзакцию: "
                  << PQresultErrorMessage(tx_commit) << "\n";
        PQclear(tx_commit);
        rollback();
        return false;
    }
    PQclear(tx_commit);

    return true;
}

std::vector<LocationPoint> fetchRecentLocationPoints(PGconn* con, std::size_t limit) {
    std::vector<LocationPoint> points;
    if (!con || PQstatus(con) != CONNECTION_OK) {
        return points;
    }

    PGresult* res = nullptr;
    if (limit == 0) {
        const std::string query =
            "SELECT latitude, longitude, timestamp "
            "FROM location_data "
            "WHERE latitude IS NOT NULL AND longitude IS NOT NULL "
            "ORDER BY timestamp DESC, id DESC";
        res = PQexec(con, query.c_str());
    } else {
        const std::string limit_str = std::to_string(limit);
        const char* params[] = {limit_str.c_str()};
        const std::string query =
            "SELECT latitude, longitude, timestamp "
            "FROM location_data "
            "WHERE latitude IS NOT NULL AND longitude IS NOT NULL "
            "ORDER BY timestamp DESC, id DESC "
            "LIMIT $1";
        res = PQexecParams(con, query.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
    }

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m: Не удалось прочитать точки карты: "
                  << (res ? PQresultErrorMessage(res) : "PQexecParams вернул nullptr") << "\n";
        if (res) {
            PQclear(res);
        }
        return points;
    }

    const int rows = PQntuples(res);
    points.reserve(static_cast<std::size_t>(rows));
    for (int i = 0; i < rows; ++i) {
        if (PQgetisnull(res, i, 0) || PQgetisnull(res, i, 1)) {
            continue;
        }

        LocationPoint point;
        point.latitude = std::stod(PQgetvalue(res, i, 0));
        point.longitude = std::stod(PQgetvalue(res, i, 1));
        point.timestamp = PQgetisnull(res, i, 2) ? 0 : std::stoll(PQgetvalue(res, i, 2));
        points.push_back(point);
    }

    PQclear(res);
    std::reverse(points.begin(), points.end());
    return points;
}

std::vector<int> fetchAvailableEarfcns(PGconn* con) {
    std::vector<int> earfcns;
    if (!con || PQstatus(con) != CONNECTION_OK) {
        return earfcns;
    }

    const std::string query =
        "SELECT DISTINCT earfcn "
        "FROM cell_data "
        "WHERE earfcn IS NOT NULL "
        "ORDER BY earfcn";

    PGresult* res = PQexec(con, query.c_str());
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m: Не удалось прочитать список EARFCN: "
                  << (res ? PQresultErrorMessage(res) : "PQexec вернул nullptr") << "\n";
        if (res) {
            PQclear(res);
        }
        return earfcns;
    }

    const int rows = PQntuples(res);
    earfcns.reserve(static_cast<std::size_t>(rows));
    for (int i = 0; i < rows; ++i) {
        if (!PQgetisnull(res, i, 0)) {
            earfcns.push_back(std::stoi(PQgetvalue(res, i, 0)));
        }
    }

    PQclear(res);
    return earfcns;
}

std::vector<HeatmapSample> fetchHeatmapSamples(
    PGconn* con,
    HeatmapCriterion criterion,
    int earfcn) {
    std::vector<HeatmapSample> samples;
    if (!con || PQstatus(con) != CONNECTION_OK) {
        return samples;
    }

    PGresult* res = nullptr;
    if (criterion == HeatmapCriterion::Altitude && earfcn < 0) {
        const std::string query =
            "SELECT latitude, longitude, altitude AS value "
            "FROM location_data "
            "WHERE latitude IS NOT NULL "
            "AND longitude IS NOT NULL "
            "AND altitude IS NOT NULL "
            "ORDER BY timestamp, id";
        res = PQexec(con, query.c_str());
    } else {
        const std::string metric = metricExpression(criterion);
        const std::string signal_filter =
            criterion == HeatmapCriterion::Altitude ? std::string{} : "AND " + metric + " < 0 ";
        const std::string earfcn_filter = earfcn >= 0 ? "AND c.earfcn = $1 " : "";
        const std::string query =
            "SELECT l.latitude, l.longitude, AVG(" + metric + ") AS value "
            "FROM location_data l "
            "JOIN cell_data c ON c.location_id = l.id "
            "WHERE l.latitude IS NOT NULL "
            "AND l.longitude IS NOT NULL "
            "AND " + metric + " IS NOT NULL "
            + signal_filter +
            earfcn_filter +
            "GROUP BY l.id, l.latitude, l.longitude "
            "ORDER BY MIN(l.timestamp), l.id";

        if (earfcn >= 0) {
            const std::string earfcn_str = std::to_string(earfcn);
            const char* params[] = {earfcn_str.c_str()};
            res = PQexecParams(con, query.c_str(), 1, nullptr, params, nullptr, nullptr, 0);
        } else {
            res = PQexec(con, query.c_str());
        }
    }

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m: Не удалось прочитать сэмплы тепловой карты: "
                  << (res ? PQresultErrorMessage(res) : "PQexec вернул nullptr") << "\n";
        if (res) {
            PQclear(res);
        }
        return samples;
    }

    const int rows = PQntuples(res);
    samples.reserve(static_cast<std::size_t>(rows));
    for (int i = 0; i < rows; ++i) {
        if (PQgetisnull(res, i, 0) || PQgetisnull(res, i, 1) || PQgetisnull(res, i, 2)) {
            continue;
        }

        HeatmapSample sample;
        sample.latitude = std::stod(PQgetvalue(res, i, 0));
        sample.longitude = std::stod(PQgetvalue(res, i, 1));
        sample.value = std::stod(PQgetvalue(res, i, 2));
        sample.earfcn = earfcn;
        samples.push_back(sample);
    }

    PQclear(res);
    return samples;
}
