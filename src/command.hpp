#pragma once

#include <string>

struct Command
{
    std::string requestId;
    std::string cameraId;
    std::string operation;
    std::string endpoint;
};
