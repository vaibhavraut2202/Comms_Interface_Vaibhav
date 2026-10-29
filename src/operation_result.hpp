#pragma once

#include <string>
#include <utility>

struct DeviceInformation
{
    std::string manufacturer;
    std::string model;
    std::string firmwareVersion;
    std::string serialNumber;
    std::string hardwareId;
};

struct OperationResult
{
    bool succeeded = false;
    DeviceInformation deviceInformation;
    std::string errorCategory;
    std::string errorMessage;

    static OperationResult success(DeviceInformation information)
    {
        OperationResult result;
        result.succeeded = true;
        result.deviceInformation = std::move(information);
        return result;
    }

    static OperationResult failure(std::string category, std::string message)
    {
        OperationResult result;
        result.errorCategory = std::move(category);
        result.errorMessage = std::move(message);
        return result;
    }
};
