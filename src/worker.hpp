#pragma once

#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "command_queue.hpp"
#include "dispatcher.hpp"
#include "json_utils.hpp"
#include "operation_result.hpp"
#include "result_store.hpp"

class Worker
{
private:
    CommandQueue& commandQueue;
    Dispatcher& dispatcher;
    ResultStore& resultStore;
    std::thread workerThread;

    static std::string trim(const std::string& value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::optional<OperationResult> validate(Command& command)
    {
        std::map<std::string, std::optional<std::string>> fields;
        std::string parseError;
        JsonObjectParser parser(command.rawRequestBody);
        if (!parser.parse(fields, parseError))
        {
            return OperationResult::failure("MALFORMED_JSON", parseError);
        }

        const auto operationField = fields.find("operation");
        const auto cameraField = fields.find("camera_id");
        if (operationField != fields.end() && operationField->second)
        {
            command.operation = trim(*operationField->second);
        }
        if (cameraField != fields.end() && cameraField->second)
        {
            command.cameraId = trim(*cameraField->second);
        }
        resultStore.updateCommandDetails(
            command.requestId, command.cameraId, command.operation);

        if (operationField == fields.end())
        {
            return OperationResult::failure(
                "MISSING_FIELD", "Required field is operation");
        }
        if (!operationField->second)
        {
            return OperationResult::failure(
                "INVALID_FIELD_TYPE", "operation must be a JSON string");
        }
        if (command.operation.empty())
        {
            return OperationResult::failure(
                "INVALID_FIELD", "operation must not be empty");
        }

        if (command.operation == "GetProfiles"
            || command.operation == "GetPTZCapabilities")
        {
            return OperationResult::failure(
                "UNSUPPORTED_OPERATION",
                command.operation + " is recognized but not implemented");
        }
        if (command.operation != "GetDeviceInformation"
            && command.operation != "GetCurrentPairedDevices"
            && command.operation != "DiscoverDevices")
        {
            return OperationResult::failure(
                "UNKNOWN_OPERATION",
                "Unrecognized operation: " + command.operation);
        }

        if (command.operation == "GetCurrentPairedDevices")
        {
            if (cameraField != fields.end() && !cameraField->second)
            {
                return OperationResult::failure(
                    "INVALID_FIELD_TYPE", "camera_id must be a JSON string");
            }
            if (cameraField != fields.end() && command.cameraId.empty())
            {
                return OperationResult::failure(
                    "INVALID_FIELD", "camera_id must not be empty");
            }
            return std::nullopt;
        }

        if (command.operation == "DiscoverDevices")
        {
            if (cameraField != fields.end() && !cameraField->second)
            {
                return OperationResult::failure(
                    "INVALID_FIELD_TYPE", "camera_id must be a JSON string");
            }
            return std::nullopt;
        }

        if (cameraField == fields.end())
        {
            return OperationResult::failure(
                "MISSING_FIELD", "Required field is camera_id");
        }
        if (!cameraField->second)
        {
            return OperationResult::failure(
                "INVALID_FIELD_TYPE", "camera_id must be a JSON string");
        }
        if (command.cameraId.empty())
        {
            return OperationResult::failure(
                "INVALID_FIELD", "camera_id must not be empty");
        }

        return std::nullopt;
    }

    void process(Command command)
    {
        try
        {
            if (!resultStore.markProcessing(command.requestId))
            {
                std::cerr << "[Worker] Cannot transition request "
                          << command.requestId << " to PROCESSING" << std::endl;
                return;
            }

            const auto validationError = validate(command);
            if (validationError)
            {
                resultStore.complete(command.requestId, *validationError);
                return;
            }
            resultStore.complete(command.requestId, dispatcher.dispatch(command));
        }
        catch (const std::exception& error)
        {
            const std::string message = std::string("Internal processing error: ")
                + error.what();
            std::cerr << "[Worker] " << message << std::endl;
            resultStore.complete(
                command.requestId,
                OperationResult::failure("INTERNAL_ERROR", message));
        }
        catch (...)
        {
            const std::string message = "Internal processing error: unknown exception";
            std::cerr << "[Worker] " << message << std::endl;
            resultStore.complete(
                command.requestId,
                OperationResult::failure("INTERNAL_ERROR", message));
        }
    }

    void run()
    {
        while (true)
        {
            std::optional<Command> command = commandQueue.pop();
            if (!command)
            {
                return;
            }
            process(std::move(*command));
        }
    }

public:
    Worker(CommandQueue& queue, Dispatcher& dispatcher, ResultStore& results)
        : commandQueue(queue),
          dispatcher(dispatcher),
          resultStore(results)
    {
    }

    ~Worker()
    {
        stop();
        join();
    }

    void start()
    {
        if (workerThread.joinable())
        {
            throw std::logic_error("Worker has already been started");
        }
        workerThread = std::thread(&Worker::run, this);
    }

    void stop()
    {
        commandQueue.close();
    }

    void join()
    {
        if (workerThread.joinable())
        {
            workerThread.join();
        }
    }
};
