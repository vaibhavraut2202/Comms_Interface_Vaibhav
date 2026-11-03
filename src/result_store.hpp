#pragma once

#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
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
    std::optional<std::string> mqttCorrelationId;
    std::string cameraId;
    std::string operation;
    RequestStatus status = RequestStatus::queued;
    std::optional<DeviceInformation> deviceInformation;
    std::optional<std::vector<PairedDevice>> pairedDevices;
    std::optional<DiscoverySummary> discoverySummary;
    std::string errorCategory;
    std::string errorMessage;
    std::chrono::system_clock::time_point createdAt;
    std::optional<std::chrono::system_clock::time_point> completedAt;
};

class ResultStore
{
private:
    using CompletionCallback = std::function<void(const CommandRecord&)>;

    mutable std::mutex mutex;
    std::unordered_map<std::string, CommandRecord> records;
    CompletionCallback completionCallback;

public:
    void addQueued(const Command& command)
    {
        CommandRecord record;
        record.requestId = command.requestId;
        record.mqttCorrelationId = command.mqttCorrelationId;
        record.cameraId = command.cameraId;
        record.operation = command.operation;
        record.createdAt = std::chrono::system_clock::now();

        std::lock_guard<std::mutex> lock(mutex);
        if (!records.emplace(record.requestId, std::move(record)).second)
        {
            throw std::logic_error("A request with this ID already exists");
        }
    }

    void setCompletionCallback(CompletionCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex);
        completionCallback = std::move(callback);
    }

    void updateCommandDetails(const std::string& requestId,
                              const std::string& cameraId,
                              const std::string& operation)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto record = records.find(requestId);
        if (record != records.end())
        {
            record->second.cameraId = cameraId;
            record->second.operation = operation;
        }
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
        CommandRecord completedRecord;
        CompletionCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto record = records.find(requestId);
            if (record == records.end()
                || record->second.status == RequestStatus::success
                || record->second.status == RequestStatus::failed)
            {
                return;
            }

            record->second.status = result.succeeded
                ? RequestStatus::success
                : RequestStatus::failed;
            if (result.succeeded)
            {
                if (result.discoverySummary)
                {
                    record->second.discoverySummary =
                        std::move(result.discoverySummary);
                }
                else
                {
                    record->second.deviceInformation =
                        std::move(result.deviceInformation);
                    record->second.pairedDevices =
                        std::move(result.pairedDevices);
                }
            }
            else
            {
                record->second.errorCategory = std::move(result.errorCategory);
                record->second.errorMessage = std::move(result.errorMessage);
            }
            record->second.completedAt = std::chrono::system_clock::now();
            if (record->second.mqttCorrelationId)
            {
                completedRecord = record->second;
                callback = completionCallback;
            }
        }

        if (callback)
        {
            try
            {
                callback(completedRecord);
            }
            catch (const std::exception& error)
            {
                std::cerr << "[ResultStore] Completion notification failed for "
                          << requestId << ": " << error.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "[ResultStore] Completion notification failed for "
                          << requestId << ": unknown exception" << std::endl;
            }
        }
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
