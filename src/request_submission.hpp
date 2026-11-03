#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "command.hpp"
#include "command_queue.hpp"
#include "json_utils.hpp"
#include "operation_result.hpp"
#include "result_store.hpp"

struct SubmissionResult
{
    bool accepted = false;
    bool duplicate = false;
    std::string requestId;
    std::string correlationId;
    std::string errorCategory;
    std::string errorMessage;
};

class RequestSubmissionService
{
private:
    struct ExistingMqttRequest
    {
        std::string requestId;
        std::string rawRequestBody;
    };

    CommandQueue& commandQueue;
    ResultStore& resultStore;
    std::mutex mutex;
    std::unordered_map<std::string, ExistingMqttRequest> mqttRequests;
    static std::atomic<std::uint64_t> nextRequestId;

    static std::string makeRequestId()
    {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return "req-" + std::to_string(now) + "-"
            + std::to_string(nextRequestId.fetch_add(1));
    }

    static bool isValidCorrelationId(const std::string& value)
    {
        if (value.empty() || value.size() > 128)
        {
            return false;
        }
        for (const unsigned char ch : value)
        {
            if (!((ch >= 'a' && ch <= 'z')
                  || (ch >= 'A' && ch <= 'Z')
                  || (ch >= '0' && ch <= '9')
                  || ch == '_' || ch == '-' || ch == '.' || ch == ':'))
            {
                return false;
            }
        }
        return true;
    }

    static std::optional<std::string> extractCorrelationId(
        const std::string& rawRequestBody)
    {
        std::map<std::string, std::optional<std::string>> fields;
        std::string parseError;
        JsonObjectParser parser(rawRequestBody);
        if (!parser.parse(fields, parseError))
        {
            return std::nullopt;
        }

        const auto field = fields.find("request_id");
        if (field == fields.end() || !field->second
            || !isValidCorrelationId(*field->second))
        {
            return std::nullopt;
        }
        return *field->second;
    }

    SubmissionResult submit(const std::string& rawRequestBody, bool mqtt)
    {
        std::lock_guard<std::mutex> lock(mutex);

        const std::optional<std::string> suppliedCorrelation = mqtt
            ? extractCorrelationId(rawRequestBody)
            : std::nullopt;
        if (suppliedCorrelation)
        {
            const auto existing = mqttRequests.find(*suppliedCorrelation);
            if (existing != mqttRequests.end())
            {
                SubmissionResult duplicate;
                duplicate.requestId = existing->second.requestId;
                duplicate.correlationId = *suppliedCorrelation;
                duplicate.duplicate = true;
                if (existing->second.rawRequestBody == rawRequestBody)
                {
                    duplicate.accepted = true;
                }
                else
                {
                    duplicate.errorCategory = "REQUEST_ID_CONFLICT";
                    duplicate.errorMessage =
                        "request_id is already associated with a different payload";
                }
                return duplicate;
            }
        }

        Command command;
        command.requestId = makeRequestId();
        command.rawRequestBody = rawRequestBody;
        if (mqtt)
        {
            command.mqttCorrelationId = suppliedCorrelation.value_or(command.requestId);
        }

        SubmissionResult submitted;
        submitted.requestId = command.requestId;
        submitted.correlationId = command.mqttCorrelationId.value_or(std::string{});

        try
        {
            resultStore.addQueued(command);
        }
        catch (const std::exception& error)
        {
            std::cerr << "[Submission] Unable to register request "
                      << command.requestId << ": " << error.what() << std::endl;
            submitted.errorCategory = "REQUEST_REGISTRATION_ERROR";
            submitted.errorMessage = error.what();
            return submitted;
        }

        if (suppliedCorrelation)
        {
            try
            {
                mqttRequests.emplace(*suppliedCorrelation,
                    ExistingMqttRequest{command.requestId, rawRequestBody});
            }
            catch (const std::exception& error)
            {
                std::cerr << "[Submission] Unable to track MQTT request "
                          << command.requestId << ": " << error.what()
                          << std::endl;
                resultStore.complete(command.requestId, OperationResult::failure(
                    "REQUEST_REGISTRATION_ERROR", error.what()));
                submitted.errorCategory = "REQUEST_REGISTRATION_ERROR";
                submitted.errorMessage = error.what();
                return submitted;
            }
        }

        try
        {
            if (!commandQueue.push(command))
            {
                if (suppliedCorrelation)
                {
                    mqttRequests.erase(*suppliedCorrelation);
                }
                resultStore.complete(command.requestId, OperationResult::failure(
                    "SERVER_SHUTDOWN", "Application is shutting down"));
                submitted.errorCategory = "SERVER_SHUTDOWN";
                submitted.errorMessage = "Application is no longer accepting commands";
                return submitted;
            }
        }
        catch (const std::exception& error)
        {
            std::cerr << "[Submission] Unable to enqueue request "
                      << command.requestId << ": " << error.what() << std::endl;
            if (suppliedCorrelation)
            {
                mqttRequests.erase(*suppliedCorrelation);
            }
            resultStore.complete(command.requestId, OperationResult::failure(
                "QUEUE_ERROR", "Unable to enqueue request"));
            submitted.errorCategory = "QUEUE_ERROR";
            submitted.errorMessage = "Unable to enqueue request";
            return submitted;
        }
        catch (...)
        {
            std::cerr << "[Submission] Unable to enqueue request "
                      << command.requestId << ": unknown exception" << std::endl;
            if (suppliedCorrelation)
            {
                mqttRequests.erase(*suppliedCorrelation);
            }
            resultStore.complete(command.requestId, OperationResult::failure(
                "QUEUE_ERROR", "Unable to enqueue request"));
            submitted.errorCategory = "QUEUE_ERROR";
            submitted.errorMessage = "Unknown exception while adding request to queue";
            return submitted;
        }

        submitted.accepted = true;
        return submitted;
    }

public:
    RequestSubmissionService(CommandQueue& queue, ResultStore& results)
        : commandQueue(queue), resultStore(results)
    {
    }

    SubmissionResult submitHttp(const std::string& rawRequestBody)
    {
        return submit(rawRequestBody, false);
    }

    SubmissionResult submitMqtt(const std::string& rawRequestBody)
    {
        return submit(rawRequestBody, true);
    }
};

inline std::atomic<std::uint64_t> RequestSubmissionService::nextRequestId{1};
