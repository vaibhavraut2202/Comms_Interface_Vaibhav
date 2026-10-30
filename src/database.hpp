#pragma once

#include <sqlite3.h>

#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
            "ip_address TEXT NOT NULL"
            ")",
            nullptr,
            nullptr,
            &errorMessage);
        if (result != SQLITE_OK)
        {
            const std::string detail = errorMessage
                ? errorMessage
                : sqlite3_errmsg(connection);
            sqlite3_free(errorMessage);
            throw DatabaseError("Unable to initialize paired_devices table: "
                + detail);
        }
    }

    void upsertPairedDevice(const std::string& cameraId,
                            const std::string& ipAddress)
    {
        std::lock_guard<std::mutex> lock(mutex);
        Statement statement(connection,
            "INSERT INTO paired_devices (camera_id, ip_address) "
            "VALUES (?, ?) "
            "ON CONFLICT(camera_id) DO UPDATE SET "
            "ip_address = excluded.ip_address");
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
