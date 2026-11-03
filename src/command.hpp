#pragma once

#include <optional>
#include <string>

struct Command
{
    std::string requestId;
    std::string rawRequestBody;
    std::optional<std::string> mqttCorrelationId;
    std::string cameraId;
    std::string operation;
    std::string endpoint;
};
