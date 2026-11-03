#pragma once

#include <charconv>
#include <string>

struct DeviceConfig
{
    unsigned short onvifPort = 8080;

    bool resolveEndpoint(
        const std::string& ipAddress,
        std::string& endpoint,
        std::string& error) const
    {
        unsigned int octets[4]{};
        std::size_t start = 0;
        for (std::size_t index = 0; index < 4; ++index)
        {
            const std::size_t end = index == 3
                ? ipAddress.size()
                : ipAddress.find('.', start);
            if (end == std::string::npos || end == start)
            {
                error = "Camera IP must be a numeric RFC1918 IPv4 address";
                return false;
            }

            const std::string part = ipAddress.substr(start, end - start);
            unsigned int value = 0;
            const auto parsed = std::from_chars(
                part.data(), part.data() + part.size(), value);
            if (parsed.ec != std::errc{}
                || parsed.ptr != part.data() + part.size()
                || value > 255
                || (part.size() > 1 && part.front() == '0'))
            {
                error = "Camera IP must be a numeric RFC1918 IPv4 address";
                return false;
            }
            octets[index] = value;
            start = end + 1;
        }

        if (start != ipAddress.size() + 1)
        {
            error = "Camera IP must be a numeric RFC1918 IPv4 address";
            return false;
        }

        const bool privateAddress = octets[0] == 10
            || (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31)
            || (octets[0] == 192 && octets[1] == 168);
        if (!privateAddress || onvifPort == 0)
        {
            error = !privateAddress
                ? "Camera IP must be an RFC1918 IPv4 address"
                : "ONVIF port must be between 1 and 65535";
            return false;
        }

        endpoint = "http://" + ipAddress + ":"
            + std::to_string(onvifPort) + "/";
        error.clear();
        return true;
    }
};
