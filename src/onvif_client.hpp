#pragma once

#include <winsock2.h>

#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include "command.hpp"
#include "operation_result.hpp"
#include "../generated/soapDeviceBindingProxy.h"

class OnvifClient
{
public:
    OperationResult getDeviceInformation(const Command& command) const
    {
        std::cout << "[ONVIF Client] GetDeviceInformation()"
                  << " | Request: " << command.requestId
                  << " | Camera: " << command.cameraId
                  << " | Endpoint: " << command.endpoint
                  << std::endl;

        DeviceBindingProxy device;
        device.soap->connect_timeout = 3;
        device.soap->send_timeout = 3;
        device.soap->recv_timeout = 3;

        _tds__GetDeviceInformation request;
        _tds__GetDeviceInformationResponse response;
        request.soap_default(device.soap);
        response.soap_default(device.soap);

        const int result = device.GetDeviceInformation(
            command.endpoint.c_str(),
            nullptr,
            &request,
            response);

        if (result == SOAP_OK)
        {
            DeviceInformation information;
            information.manufacturer = response.Manufacturer;
            information.model = response.Model;
            information.firmwareVersion = response.FirmwareVersion;
            information.serialNumber = response.SerialNumber;
            information.hardwareId = response.HardwareId;
            std::cout << "[ONVIF Client] Device information received for "
                      << command.cameraId << std::endl;
            return OperationResult::success(std::move(information));
        }

        std::ostringstream fault;
        device.soap_stream_fault(fault);
        std::string category = "ONVIF_ERROR";
        std::string connectionDescription;
        if (result == SOAP_TCP_ERROR)
        {
            const std::string faultText = fault.str();
            if (device.soap->errnum == WSAETIMEDOUT
                || faultText.find("Timeout") != std::string::npos)
            {
                category = "CONNECTION_TIMEOUT";
                connectionDescription = "Connection to camera timed out";
            }
            else if (device.soap->errnum == WSAECONNREFUSED
                || faultText.find("Connection refused") != std::string::npos)
            {
                category = "CONNECTION_REFUSED";
                connectionDescription = "Connection to camera was refused";
            }
            else
            {
                category = "DEVICE_UNAVAILABLE";
                connectionDescription = "Camera endpoint is unavailable";
            }
        }
        else if (result == SOAP_FAULT || device.soap->fault != nullptr)
        {
            category = "SOAP_FAULT";
        }

        std::ostringstream message;
        if (!connectionDescription.empty())
        {
            message << connectionDescription;
            if (device.soap->errnum != 0)
            {
                message << " (Winsock error " << device.soap->errnum << ')';
            }
            message << " (gSOAP error " << result << ')';
        }
        else
        {
            message << "gSOAP error " << result;
        }
        const std::string faultText = fault.str();
        if (!faultText.empty())
        {
            message << ": " << faultText;
        }
        std::cerr << "[ONVIF Client] " << category << ": "
                  << message.str() << std::endl;
        return OperationResult::failure(category, message.str());
    }
};
