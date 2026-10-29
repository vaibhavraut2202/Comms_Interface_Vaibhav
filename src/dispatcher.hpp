#pragma once

#include "command.hpp"
#include "onvif_client.hpp"
#include "operation_result.hpp"

class Dispatcher
{
private:
    OnvifClient onvifClient;

public:
    OperationResult dispatch(const Command& command) const
    {
        if (command.operation == "GetDeviceInformation")
        {
            return onvifClient.getDeviceInformation(command);
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
