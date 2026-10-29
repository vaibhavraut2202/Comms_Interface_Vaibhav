# Comms Interface

A C++ communication layer for camera commands.

## HTTP API

The application exposes an HTTP API bound to loopback by default
(`127.0.0.1:8090`); the bind address is configurable. The API has no
authentication, so keep it on loopback or a trusted network.
`POST /command` accepts a JSON object with `camera_id` and `operation` string
fields. A valid request receives HTTP `202` with a request ID and `QUEUED`
status; camera work proceeds asynchronously through the command queue, worker,
dispatcher, and generated gSOAP `DeviceBindingProxy`.

```json
{"camera_id":"CAM_007","operation":"GetDeviceInformation"}
```

Poll `GET /result?id=<request_id>` for the current `QUEUED`, `PROCESSING`,
`SUCCESS`, or `FAILED` status. Successful results include device information;
failures include an error category and message. Only `CAM_007` is configured,
using the endpoint `http://172.16.10.23:8080/`. `GetDeviceInformation` is the
only supported operation. `GetProfiles` and `GetPTZCapabilities` are recognized
but explicitly unsupported.

The generated gSOAP call uses bounded three-second connect, send, and receive
timeouts. Its connection attempt is also the availability check, so the HTTP
handler does not perform a duplicate network probe. No external JSON or HTTP
library is required: the project uses a small strict JSON parser for the
request shape and Winsock for HTTP.

Run the API with optional configuration overrides:

```powershell
.\comms_interface.exe [--listen-address 127.0.0.1] [--port 8090] [--camera-endpoint "http://172.16.10.23:8080/"]
```

Build from the project root with MinGW-w64 and the installed gSOAP runtime:

```powershell
g++ -std=c++17 -I"generated" -I"C:\Users\vaibh\gsoap_2.8.135\gsoap-2.8\gsoap" -pthread src\main.cpp generated\soapC.cpp generated\soapDeviceBindingProxy.cpp "C:\Users\vaibh\gsoap_2.8.135\gsoap-2.8\gsoap\stdsoap2.cpp" -lws2_32 -o comms_interface.exe
```

## Development Environment

- C++17
- MinGW-w64
- gSOAP 2.8.135
