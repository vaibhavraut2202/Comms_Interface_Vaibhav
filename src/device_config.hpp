#pragma once

#include <optional>
#include <string>
#include <unordered_map>

inline constexpr char configuredCameraIpAddress[] = "172.16.10.23";

struct DeviceConfig
{
    std::string cameraId;
    std::string endpoint;
};

class DeviceRegistry
{
private:
    std::unordered_map<std::string, DeviceConfig> devices;

public:
    static constexpr const char* defaultEndpoint = "http://172.16.10.23:8080/";

    explicit DeviceRegistry(const std::string& cameraEndpoint = defaultEndpoint)
    {
        devices.emplace("CAM_007", DeviceConfig{"CAM_007", cameraEndpoint});
    }

    std::optional<DeviceConfig> find(const std::string& cameraId) const
    {
        const auto device = devices.find(cameraId);
        if (device == devices.end())
        {
            return std::nullopt;
        }
        return device->second;
    }
};
