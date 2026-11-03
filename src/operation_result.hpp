#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct DeviceInformation
{
    std::string manufacturer;
    std::string model;
    std::string firmwareVersion;
    std::string serialNumber;
    std::string hardwareId;
};

struct PairedDevice
{
    std::string cameraId;
    std::string ipAddress;
};

struct DiscoveredDevice
{
    std::string endpointReference;
    std::vector<std::string> types;
    std::vector<std::string> scopes;
    std::vector<std::string> xAddrs;
    std::vector<std::string> responseSourceAddresses;
    std::uint32_t metadataVersion = 0;
};

struct DiscoverySummary
{
    std::size_t discoveredCount = 0;
    std::size_t informationSuccessCount = 0;
    std::size_t informationFailureCount = 0;
};

struct OperationResult
{
    bool succeeded = false;
    DeviceInformation deviceInformation;
    std::optional<std::vector<PairedDevice>> pairedDevices;
    std::optional<DiscoverySummary> discoverySummary;
    std::string errorCategory;
    std::string errorMessage;

    static OperationResult success(DeviceInformation information)
    {
        OperationResult result;
        result.succeeded = true;
        result.deviceInformation = std::move(information);
        return result;
    }

    static OperationResult pairedDevicesSuccess(std::vector<PairedDevice> devices)
    {
        OperationResult result;
        result.succeeded = true;
        result.pairedDevices = std::move(devices);
        return result;
    }

    static OperationResult discoverySuccess(DiscoverySummary summary)
    {
        OperationResult result;
        result.succeeded = true;
        result.discoverySummary = summary;
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
