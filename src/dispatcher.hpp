#pragma once

#include "command.hpp"
#include "database.hpp"
#include "device_config.hpp"
#include "onvif_client.hpp"
#include "operation_result.hpp"

class Dispatcher
{
private:
    Database& database;
    OnvifClient onvifClient;

public:
    explicit Dispatcher(Database& deviceDatabase)
        : database(deviceDatabase)
    {
    }

    OperationResult dispatch(const Command& command) const
    {
        if (command.operation == "GetDeviceInformation")
        {
            OperationResult result = onvifClient.getDeviceInformation(command);
            if (!result.succeeded)
            {
                return result;
            }
            try
            {
                database.upsertPairedDevice(
                    command.cameraId, configuredCameraIpAddress);
            }
            catch (const DatabaseError& error)
            {
                return OperationResult::failure("DATABASE_ERROR", error.what());
            }
            return result;
        }
        if (command.operation == "GetCurrentPairedDevices")
        {
            try
            {
                return OperationResult::pairedDevicesSuccess(
                    database.getPairedDevices());
            }
            catch (const DatabaseError& error)
            {
                return OperationResult::failure("DATABASE_ERROR", error.what());
            }
        }
        if (command.operation == "GetProfiles"
            || command.operation == "GetPTZCapabilities")
        {
            return OperationResult::failure(
                "UNSUPPORTED_OPERATION",
                command.operation + " is recognized but not implemented");
        }
        return OperationResult::failure(
            "UNKNOWN_OPERATION",
            "Unrecognized operation: " + command.operation);
    }
};
