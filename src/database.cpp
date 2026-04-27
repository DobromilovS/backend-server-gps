#include "database.h"

#include <iostream>
#include <string>

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
            "INSERT INTO cell_data (location_id, cell_type, ci, pci, tac, rsrp, rsrq, rssi, "
            "ta, lac, nci, ss_rsrp, ss_rsrq, ss_sinr) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14)";

        PGresult* cell_res = PQexecParams(
            con, cell_query.c_str(), 14, nullptr, cell_params, nullptr, nullptr, 0);
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
