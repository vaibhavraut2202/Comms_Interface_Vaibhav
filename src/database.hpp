#pragma once

#include <sqlite3.h>

#include <array>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "json_utils.hpp"
#include "operation_result.hpp"

class DatabaseError : public std::runtime_error
{
public:
    explicit DatabaseError(std::string message)
        : std::runtime_error(std::move(message))
    {
    }
};

class Database
{
private:
    sqlite3* connection = nullptr;
    std::mutex mutex;

    static std::string sqliteMessage(sqlite3* database, const std::string& action)
    {
        return action + ": "
            + (database ? sqlite3_errmsg(database) : "SQLite connection is unavailable");
    }

    static void checkResult(sqlite3* database, int result, const std::string& action)
    {
        if (result != SQLITE_OK)
        {
            throw DatabaseError(sqliteMessage(database, action));
        }
    }

    class Statement
    {
    private:
        sqlite3* connection;
        sqlite3_stmt* statement = nullptr;

    public:
        Statement(sqlite3* database, const char* sql)
            : connection(database)
        {
            const int result = sqlite3_prepare_v2(
                connection, sql, -1, &statement, nullptr);
            if (result != SQLITE_OK)
            {
                const std::string message = sqliteMessage(
                    connection, "Unable to prepare database statement");
                if (statement)
                {
                    sqlite3_finalize(statement);
                    statement = nullptr;
                }
                throw DatabaseError(message);
            }
        }

        ~Statement()
        {
            if (statement)
            {
                sqlite3_finalize(statement);
            }
        }

        Statement(const Statement&) = delete;
        Statement& operator=(const Statement&) = delete;

        sqlite3_stmt* get() const
        {
            return statement;
        }

        void finalize()
        {
            const int result = sqlite3_finalize(statement);
            statement = nullptr;
            checkResult(connection, result, "Unable to finalize database statement");
        }
    };

    static void bindText(sqlite3* database, sqlite3_stmt* statement, int index,
                         const std::string& value)
    {
        if (value.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max()))
        {
            throw DatabaseError("Database text value exceeds SQLite's size limit");
        }

        checkResult(database,
            sqlite3_bind_text(statement, index, value.data(),
                static_cast<int>(value.size()), SQLITE_TRANSIENT),
            "Unable to bind database text value");
    }

    static std::string serializeStringList(
        const std::vector<std::string>& values)
    {
        std::string json = "[";
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            if (index != 0)
            {
                json += ',';
            }
            json += '"';
            json += jsonEscape(values[index]);
            json += '"';
        }
        json += ']';
        return json;
    }

    bool hasPairedDeviceColumn(const std::string& columnName)
    {
        Statement statement(connection, "PRAGMA table_info(paired_devices)");
        int result = SQLITE_OK;
        while ((result = sqlite3_step(statement.get())) == SQLITE_ROW)
        {
            const auto* name = reinterpret_cast<const char*>(
                sqlite3_column_text(statement.get(), 1));
            if (name && columnName == name)
            {
                statement.finalize();
                return true;
            }
        }
        if (result != SQLITE_DONE)
        {
            throw DatabaseError(sqliteMessage(
                connection, "Unable to inspect paired_devices schema"));
        }
        statement.finalize();
        return false;
    }

    void addPairedDeviceInformationColumns()
    {
        const std::array<const char*, 6> columns{
            "manufacturer",
            "model",
            "firmware_version",
            "serial_number",
            "hardware_id",
            "discovered_endpoint_reference"
        };
        for (const char* column : columns)
        {
            if (hasPairedDeviceColumn(column))
            {
                continue;
            }
            const std::string sql = "ALTER TABLE paired_devices ADD COLUMN "
                + std::string(column) + " TEXT";
            char* errorMessage = nullptr;
            const int result = sqlite3_exec(
                connection, sql.c_str(), nullptr, nullptr, &errorMessage);
            if (result != SQLITE_OK)
            {
                const std::string detail = errorMessage
                    ? errorMessage
                    : sqlite3_errmsg(connection);
                sqlite3_free(errorMessage);
                throw DatabaseError(
                    "Unable to add paired-device information column "
                    + std::string(column) + ": " + detail);
            }
        }
    }

public:
    explicit Database(const std::string& path = "comms.db")
    {
        const int result = sqlite3_open_v2(
            path.c_str(),
            &connection,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr);
        if (result != SQLITE_OK)
        {
            const std::string message = sqliteMessage(
                connection, "Unable to open database " + path);
            if (connection)
            {
                sqlite3_close(connection);
                connection = nullptr;
            }
            throw DatabaseError(message);
        }
    }

    ~Database()
    {
        if (connection)
        {
            const int result = sqlite3_close(connection);
            if (result != SQLITE_OK)
            {
                std::cerr << "[Database] " << sqliteMessage(
                    connection, "Unable to close database") << std::endl;
                const int deferredResult = sqlite3_close_v2(connection);
                if (deferredResult != SQLITE_OK)
                {
                    std::cerr << "[Database] " << sqliteMessage(
                        connection, "Unable to defer database close") << std::endl;
                }
            }
        }
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void initialize()
    {
        std::lock_guard<std::mutex> lock(mutex);
        char* errorMessage = nullptr;
        const int result = sqlite3_exec(
            connection,
            "CREATE TABLE IF NOT EXISTS paired_devices ("
            "camera_id TEXT PRIMARY KEY, "
            "ip_address TEXT NOT NULL, "
            "manufacturer TEXT, "
            "model TEXT, "
            "firmware_version TEXT, "
            "serial_number TEXT, "
            "hardware_id TEXT, "
            "discovered_endpoint_reference TEXT"
            ");"
            "CREATE TABLE IF NOT EXISTS discovered_devices ("
            "endpoint_reference TEXT PRIMARY KEY, "
            "types_json TEXT NOT NULL, "
            "scopes_json TEXT NOT NULL, "
            "xaddrs_json TEXT NOT NULL, "
            "metadata_version INTEGER NOT NULL, "
            "manufacturer TEXT, "
            "model TEXT, "
            "firmware_version TEXT, "
            "serial_number TEXT, "
            "hardware_id TEXT"
            ");",
            nullptr,
            nullptr,
            &errorMessage);
        if (result != SQLITE_OK)
        {
            const std::string detail = errorMessage
                ? errorMessage
                : sqlite3_errmsg(connection);
            sqlite3_free(errorMessage);
            throw DatabaseError("Unable to initialize database schema: "
                + detail);
        }
        addPairedDeviceInformationColumns();
    }

    void upsertPairedDevice(const std::string& cameraId,
                            const std::string& ipAddress)
    {
        std::lock_guard<std::mutex> lock(mutex);
        Statement statement(connection,
            "INSERT INTO paired_devices (camera_id, ip_address) "
            "VALUES (?, ?) "
            "ON CONFLICT(camera_id) DO UPDATE SET "
            "ip_address = excluded.ip_address, "
            "discovered_endpoint_reference = CASE "
            "WHEN paired_devices.ip_address != excluded.ip_address "
            "THEN NULL ELSE paired_devices.discovered_endpoint_reference END");
        bindText(connection, statement.get(), 1, cameraId);
        bindText(connection, statement.get(), 2, ipAddress);

        const int result = sqlite3_step(statement.get());
        if (result != SQLITE_DONE)
        {
            throw DatabaseError(sqliteMessage(
                connection, "Unable to save paired device"));
        }
        statement.finalize();
    }

    void associatePairedDevice(
        const std::string& cameraId,
        const std::string& ipAddress,
        const std::string& endpointReference)
    {
        if (cameraId.empty() || ipAddress.empty() || endpointReference.empty())
        {
            throw DatabaseError(
                "Camera ID, IP address, and EndpointReference are required");
        }

        std::lock_guard<std::mutex> lock(mutex);
        Statement discovered(connection,
            "SELECT 1 FROM discovered_devices WHERE endpoint_reference = ?");
        bindText(connection, discovered.get(), 1, endpointReference);
        const int discoveryResult = sqlite3_step(discovered.get());
        if (discoveryResult == SQLITE_DONE)
        {
            discovered.finalize();
            throw DatabaseError(
                "Cannot pair a device that has not been discovered");
        }
        if (discoveryResult != SQLITE_ROW)
        {
            throw DatabaseError(sqliteMessage(
                connection, "Unable to verify discovered device"));
        }
        discovered.finalize();

        Statement paired(connection,
            "UPDATE paired_devices SET ip_address = ?, "
            "discovered_endpoint_reference = ? WHERE camera_id = ?");
        bindText(connection, paired.get(), 1, ipAddress);
        bindText(connection, paired.get(), 2, endpointReference);
        bindText(connection, paired.get(), 3, cameraId);
        const int result = sqlite3_step(paired.get());
        if (result != SQLITE_DONE)
        {
            throw DatabaseError(sqliteMessage(
                connection, "Unable to associate discovered device"));
        }
        if (sqlite3_changes(connection) != 1)
        {
            throw DatabaseError(
                "Cannot pair a device to an unknown camera ID");
        }
        paired.finalize();
    }

    std::optional<std::string> getPairedDeviceEndpointReference(
        const std::string& cameraId)
    {
        std::lock_guard<std::mutex> lock(mutex);
        Statement statement(connection,
            "SELECT discovered_endpoint_reference FROM paired_devices "
            "WHERE camera_id = ?");
        bindText(connection, statement.get(), 1, cameraId);
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE)
        {
            statement.finalize();
            return std::nullopt;
        }
        if (result != SQLITE_ROW)
        {
            throw DatabaseError(sqliteMessage(
                connection, "Unable to retrieve paired-device association"));
        }
        if (sqlite3_column_type(statement.get(), 0) == SQLITE_NULL)
        {
            statement.finalize();
            return std::nullopt;
        }
        const auto* reference = reinterpret_cast<const char*>(
            sqlite3_column_text(statement.get(), 0));
        if (!reference)
        {
            throw DatabaseError(
                "Paired-device EndpointReference contains an invalid value");
        }
        const std::string value(reference, static_cast<std::size_t>(
            sqlite3_column_bytes(statement.get(), 0)));
        statement.finalize();
        return value;
    }

    std::optional<std::string> getPairedDeviceIpAddress(
        const std::string& cameraId)
    {
        std::lock_guard<std::mutex> lock(mutex);
        Statement statement(connection,
            "SELECT ip_address FROM paired_devices WHERE camera_id = ?");
        bindText(connection, statement.get(), 1, cameraId);

        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE)
        {
            statement.finalize();
            return std::nullopt;
        }
        if (result != SQLITE_ROW)
        {
            throw DatabaseError(sqliteMessage(
                connection, "Unable to retrieve paired-device IP address"));
        }
        const auto* ipAddress = reinterpret_cast<const char*>(
            sqlite3_column_text(statement.get(), 0));
        if (!ipAddress)
        {
            throw DatabaseError("Paired-device IP address is NULL");
        }
        const std::string value(ipAddress, static_cast<std::size_t>(
            sqlite3_column_bytes(statement.get(), 0)));
        statement.finalize();
        return value;
    }

    void updatePairedDeviceInformation(
        const std::string& cameraId,
        const std::string& ipAddress,
        const DeviceInformation& information)
    {
        std::lock_guard<std::mutex> lock(mutex);
        Statement statement(connection,
            "UPDATE paired_devices SET manufacturer = ?, model = ?, "
            "firmware_version = ?, serial_number = ?, hardware_id = ? "
            "WHERE camera_id = ? AND ip_address = ?");
        bindText(connection, statement.get(), 1, information.manufacturer);
        bindText(connection, statement.get(), 2, information.model);
        bindText(connection, statement.get(), 3, information.firmwareVersion);
        bindText(connection, statement.get(), 4, information.serialNumber);
        bindText(connection, statement.get(), 5, information.hardwareId);
        bindText(connection, statement.get(), 6, cameraId);
        bindText(connection, statement.get(), 7, ipAddress);

        const int result = sqlite3_step(statement.get());
        if (result != SQLITE_DONE)
        {
            throw DatabaseError(sqliteMessage(
                connection, "Unable to update paired-device information"));
        }
        if (sqlite3_changes(connection) != 1)
        {
            throw DatabaseError(
                "Paired-device mapping changed before information was saved");
        }
        statement.finalize();
    }

    std::optional<DeviceInformation> getPairedDeviceInformation(
        const std::string& cameraId)
    {
        std::lock_guard<std::mutex> lock(mutex);
        Statement statement(connection,
            "SELECT manufacturer, model, firmware_version, serial_number, "
            "hardware_id FROM paired_devices WHERE camera_id = ?");
        bindText(connection, statement.get(), 1, cameraId);

        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE)
        {
            statement.finalize();
            return std::nullopt;
        }
        if (result != SQLITE_ROW)
        {
            throw DatabaseError(sqliteMessage(
                connection, "Unable to retrieve paired-device information"));
        }

        bool hasInformation = false;
        std::string values[5];
        for (int column = 0; column < 5; ++column)
        {
            if (sqlite3_column_type(statement.get(), column) != SQLITE_NULL)
            {
                hasInformation = true;
                const auto* value = reinterpret_cast<const char*>(
                    sqlite3_column_text(statement.get(), column));
                if (value)
                {
                    values[column] = std::string(value,
                        static_cast<std::size_t>(sqlite3_column_bytes(
                            statement.get(), column)));
                }
            }
        }
        statement.finalize();
        if (!hasInformation)
        {
            return std::nullopt;
        }
        return DeviceInformation{
            std::move(values[0]),
            std::move(values[1]),
            std::move(values[2]),
            std::move(values[3]),
            std::move(values[4])
        };
    }

    void upsertDiscoveredDevice(const DiscoveredDevice& device)
    {
        if (device.endpointReference.empty())
        {
            throw DatabaseError("Discovered device has an empty EndpointReference");
        }
        std::lock_guard<std::mutex> lock(mutex);
        Statement statement(connection,
            "INSERT INTO discovered_devices (endpoint_reference, types_json, "
            "scopes_json, xaddrs_json, metadata_version) VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(endpoint_reference) DO UPDATE SET "
            "types_json = excluded.types_json, "
            "scopes_json = excluded.scopes_json, "
            "xaddrs_json = excluded.xaddrs_json, "
            "metadata_version = excluded.metadata_version, "
            "manufacturer = NULL, model = NULL, firmware_version = NULL, "
            "serial_number = NULL, hardware_id = NULL");
        bindText(connection, statement.get(), 1, device.endpointReference);
        bindText(connection, statement.get(), 2, serializeStringList(device.types));
        bindText(connection, statement.get(), 3, serializeStringList(device.scopes));
        bindText(connection, statement.get(), 4, serializeStringList(device.xAddrs));
        checkResult(connection,
            sqlite3_bind_int64(statement.get(), 5,
                static_cast<sqlite3_int64>(device.metadataVersion)),
            "Unable to bind discovered-device metadata version");

        const int result = sqlite3_step(statement.get());
        if (result != SQLITE_DONE)
        {
            throw DatabaseError(sqliteMessage(
                connection, "Unable to save discovered device"));
        }
        statement.finalize();
    }

    void updateDiscoveredDeviceInformation(
        const std::string& endpointReference,
        const DeviceInformation& information)
    {
        std::lock_guard<std::mutex> lock(mutex);
        Statement statement(connection,
            "UPDATE discovered_devices SET manufacturer = ?, model = ?, "
            "firmware_version = ?, serial_number = ?, hardware_id = ? "
            "WHERE endpoint_reference = ?");
        bindText(connection, statement.get(), 1, information.manufacturer);
        bindText(connection, statement.get(), 2, information.model);
        bindText(connection, statement.get(), 3, information.firmwareVersion);
        bindText(connection, statement.get(), 4, information.serialNumber);
        bindText(connection, statement.get(), 5, information.hardwareId);
        bindText(connection, statement.get(), 6, endpointReference);

        const int result = sqlite3_step(statement.get());
        if (result != SQLITE_DONE)
        {
            throw DatabaseError(sqliteMessage(
                connection, "Unable to update discovered-device information"));
        }
        if (sqlite3_changes(connection) != 1)
        {
            throw DatabaseError(
                "Unable to update information for an unknown discovered device");
        }
        statement.finalize();
    }

    std::vector<PairedDevice> getPairedDevices()
    {
        std::lock_guard<std::mutex> lock(mutex);
        Statement statement(connection,
            "SELECT camera_id, ip_address FROM paired_devices ORDER BY camera_id");
        std::vector<PairedDevice> devices;
        int result = SQLITE_OK;
        while ((result = sqlite3_step(statement.get())) == SQLITE_ROW)
        {
            const auto* cameraId = reinterpret_cast<const char*>(
                sqlite3_column_text(statement.get(), 0));
            const auto* ipAddress = reinterpret_cast<const char*>(
                sqlite3_column_text(statement.get(), 1));
            if (!cameraId || !ipAddress)
            {
                throw DatabaseError("Paired-device row contains a NULL value");
            }

            devices.push_back(PairedDevice{
                std::string(cameraId, static_cast<std::size_t>(
                    sqlite3_column_bytes(statement.get(), 0))),
                std::string(ipAddress, static_cast<std::size_t>(
                    sqlite3_column_bytes(statement.get(), 1)))
            });
        }
        if (result != SQLITE_DONE)
        {
            throw DatabaseError(sqliteMessage(
                connection, "Unable to retrieve paired devices"));
        }
        statement.finalize();
        return devices;
    }
};
