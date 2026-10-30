#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "command.hpp"
#include "operation_result.hpp"

enum class RequestStatus
{
    queued,
    processing,
    success,
    failed
};

inline const char* statusName(RequestStatus status)
{
    switch (status)
    {
    case RequestStatus::queued:
        return "QUEUED";
    case RequestStatus::processing:
        return "PROCESSING";
    case RequestStatus::success:
        return "SUCCESS";
    case RequestStatus::failed:
        return "FAILED";
    }
    return "FAILED";
}

struct CommandRecord
{
    std::string requestId;
    std::string cameraId;
    std::string operation;
    RequestStatus status = RequestStatus::queued;
    std::optional<DeviceInformation> deviceInformation;
    std::optional<std::vector<PairedDevice>> pairedDevices;
    std::string errorCategory;
    std::string errorMessage;
    std::chrono::system_clock::time_point createdAt;
    std::optional<std::chrono::system_clock::time_point> completedAt;
};

class ResultStore
{
private:
    mutable std::mutex mutex;
    std::unordered_map<std::string, CommandRecord> records;

public:
    void addQueued(const Command& command)
    {
        CommandRecord record;
        record.requestId = command.requestId;
        record.cameraId = command.cameraId;
        record.operation = command.operation;
        record.createdAt = std::chrono::system_clock::now();

        std::lock_guard<std::mutex> lock(mutex);
        records.emplace(record.requestId, std::move(record));
    }

    bool markProcessing(const std::string& requestId)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto record = records.find(requestId);
        if (record == records.end() || record->second.status != RequestStatus::queued)
        {
            return false;
        }
        record->second.status = RequestStatus::processing;
        return true;
    }

    void complete(const std::string& requestId, OperationResult result)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto record = records.find(requestId);
        if (record == records.end())
        {
            return;
        }

        record->second.status = result.succeeded
            ? RequestStatus::success
            : RequestStatus::failed;
        if (result.succeeded)
        {
            record->second.deviceInformation = std::move(result.deviceInformation);
            record->second.pairedDevices = std::move(result.pairedDevices);
        }
        else
        {
            record->second.errorCategory = std::move(result.errorCategory);
            record->second.errorMessage = std::move(result.errorMessage);
        }
        record->second.completedAt = std::chrono::system_clock::now();
    }

    std::optional<CommandRecord> find(const std::string& requestId) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto record = records.find(requestId);
        if (record == records.end())
        {
            return std::nullopt;
        }
        return record->second;
    }
};
