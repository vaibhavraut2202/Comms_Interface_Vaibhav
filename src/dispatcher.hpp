#pragma once

#include <exception>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "command.hpp"
#include "database.hpp"
#include "device_config.hpp"
#include "onvif_client.hpp"
#include "operation_result.hpp"
#include "ws_discovery.hpp"

class Dispatcher
{
private:
    static constexpr std::chrono::seconds enrichmentTimeBudget{60};
    static constexpr std::chrono::seconds maximumSoapCallDuration{9};

    Database& database;
    DeviceConfig deviceConfig;
    std::optional<std::string> discoverySoapTargetIp;
    OnvifClient onvifClient;
    WsDiscovery wsDiscovery;

public:
    static bool hasTimeForInformationAttempt(
        std::chrono::steady_clock::time_point deadline,
        std::chrono::steady_clock::time_point now)
    {
        return deadline - now >= maximumSoapCallDuration;
    }

    static bool shouldEnrichEndpoint(
        const std::string& endpoint,
        const std::optional<std::string>& targetIp)
    {
        return !targetIp || WsDiscovery::xAddrTargetsIpv4(endpoint, *targetIp);
    }

    static std::optional<std::string> resolveCameraEndpoint(
        Database& database,
        const DeviceConfig& deviceConfig,
        const std::string& cameraId,
        std::string& resolvedIpAddress,
        std::string& errorCategory,
        std::string& errorMessage)
    {
        if (cameraId.empty())
        {
            errorCategory = "INVALID_FIELD";
            errorMessage = "camera_id must not be empty";
            return std::nullopt;
        }

        const std::optional<std::string> ipAddress =
            database.getPairedDeviceIpAddress(cameraId);
        if (!ipAddress)
        {
            errorCategory = "CAMERA_NOT_PAIRED";
            errorMessage = "No paired-device IP mapping exists for camera "
                + cameraId;
            return std::nullopt;
        }

        std::string endpoint;
        if (!deviceConfig.resolveEndpoint(
                *ipAddress, endpoint, errorMessage))
        {
            errorCategory = "ENDPOINT_RESOLUTION_ERROR";
            return std::nullopt;
        }
        resolvedIpAddress = *ipAddress;
        errorCategory.clear();
        errorMessage.clear();
        return endpoint;
    }

    template <typename EnrichmentCall>
    static bool runEnrichmentIfAllowed(
        const std::string& endpoint,
        const std::optional<std::string>& targetIp,
        EnrichmentCall&& enrichmentCall)
    {
        if (!shouldEnrichEndpoint(endpoint, targetIp))
        {
            return false;
        }
        std::forward<EnrichmentCall>(enrichmentCall)();
        return true;
    }

    explicit Dispatcher(
        Database& deviceDatabase,
        DeviceConfig config = {},
        std::optional<std::string> discoverySoapTargetIp = std::nullopt)
        : database(deviceDatabase),
          deviceConfig(config),
          discoverySoapTargetIp(std::move(discoverySoapTargetIp))
    {
    }

    OperationResult dispatch(const Command& command) const
    {
        if (command.operation == "DiscoverDevices")
        {
            try
            {
                const std::vector<DiscoveredDevice> devices =
                    wsDiscovery.discover();
                for (const DiscoveredDevice& device : devices)
                {
                    database.upsertDiscoveredDevice(device);
                }

                DiscoverySummary summary;
                summary.discoveredCount = devices.size();
                const auto enrichmentDeadline =
                    std::chrono::steady_clock::now() + enrichmentTimeBudget;
                bool deadlineExpired = false;
                for (std::size_t deviceIndex = 0;
                     deviceIndex < devices.size();
                     ++deviceIndex)
                {
                    const DiscoveredDevice& device = devices[deviceIndex];
                    bool informationCollected = false;
                    if (!hasTimeForInformationAttempt(
                            enrichmentDeadline,
                            std::chrono::steady_clock::now()))
                    {
                        deadlineExpired = true;
                    }
                    std::vector<std::string> rejectionReasons;
                    const std::vector<std::string> endpoints = deadlineExpired
                        ? std::vector<std::string>()
                        : WsDiscovery::acceptableXAddrs(
                              device,
                              WsDiscovery::maximumXAddrAttemptsPerDevice,
                              rejectionReasons);
                    for (const std::string& reason : rejectionReasons)
                    {
                        std::cerr << "[Dispatcher] Rejected discovered XAddr "
                                  << "for " << device.endpointReference << ": "
                                  << reason << std::endl;
                    }
                    for (const std::string& endpoint : endpoints)
                    {
                        const auto now = std::chrono::steady_clock::now();
                        if (!hasTimeForInformationAttempt(
                                enrichmentDeadline, now))
                        {
                            deadlineExpired = true;
                            break;
                        }

                        Command deviceCommand = command;
                        deviceCommand.cameraId = device.endpointReference;
                        deviceCommand.operation = "GetDeviceInformation";
                        deviceCommand.endpoint = endpoint;

                        try
                        {
                            OperationResult information;
                            const bool enrichmentRan = runEnrichmentIfAllowed(
                                endpoint, discoverySoapTargetIp, [&]
                                {
                                    information = onvifClient
                                        .getDeviceInformation(deviceCommand);
                                });
                            if (!enrichmentRan)
                            {
                                std::cerr << "[Dispatcher] Skipping SOAP "
                                          << "enrichment for "
                                          << device.endpointReference
                                          << " because " << endpoint
                                          << " is outside the configured "
                                          << "target IP" << std::endl;
                                continue;
                            }
                            if (!information.succeeded)
                            {
                                continue;
                            }
                            database.updateDiscoveredDeviceInformation(
                                device.endpointReference,
                                information.deviceInformation);
                            ++summary.informationSuccessCount;
                            informationCollected = true;
                            break;
                        }
                        catch (const DatabaseError&)
                        {
                            throw;
                        }
                        catch (const std::exception& error)
                        {
                            std::cerr << "[Dispatcher] GetDeviceInformation "
                                      << "failed for " << device.endpointReference
                                      << " at " << endpoint << ": "
                                      << error.what() << std::endl;
                        }
                        catch (...)
                        {
                            std::cerr << "[Dispatcher] GetDeviceInformation "
                                      << "failed for "
                                      << device.endpointReference << " at "
                                      << endpoint << " with an unknown exception"
                                      << std::endl;
                        }
                    }
                    if (!informationCollected)
                    {
                        ++summary.informationFailureCount;
                    }
                    if (deadlineExpired)
                    {
                        for (std::size_t remaining = deviceIndex + 1;
                             remaining < devices.size();
                             ++remaining)
                        {
                            ++summary.informationFailureCount;
                        }
                        std::cerr << "[Dispatcher] Discovery information "
                                  << "enrichment reached its 60-second deadline"
                                  << std::endl;
                        break;
                    }
                }
                return OperationResult::discoverySuccess(summary);
            }
            catch (const DatabaseError& error)
            {
                return OperationResult::failure("DATABASE_ERROR", error.what());
            }
            catch (const std::exception& error)
            {
                return OperationResult::failure("DISCOVERY_ERROR", error.what());
            }
        }
        if (command.operation == "GetDeviceInformation")
        {
            std::string errorCategory;
            std::string errorMessage;
            std::string resolvedIpAddress;
            std::optional<std::string> endpoint;
            try
            {
                endpoint = resolveCameraEndpoint(
                    database, deviceConfig, command.cameraId,
                    resolvedIpAddress, errorCategory, errorMessage);
            }
            catch (const DatabaseError& error)
            {
                return OperationResult::failure("DATABASE_ERROR", error.what());
            }
            if (!endpoint)
            {
                return OperationResult::failure(
                    std::move(errorCategory), std::move(errorMessage));
            }

            Command cameraCommand = command;
            cameraCommand.endpoint = *endpoint;
            OperationResult result =
                onvifClient.getDeviceInformation(cameraCommand);
            if (!result.succeeded)
            {
                return result;
            }
            try
            {
                database.updatePairedDeviceInformation(
                    command.cameraId, resolvedIpAddress,
                    result.deviceInformation);
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
