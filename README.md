# Comms Interface

A C++ communication layer for camera commands.

## HTTP API

The application exposes an HTTP API bound to loopback by default
(`127.0.0.1:8090`); the bind address is configurable. The API has no
authentication, so keep it on loopback or a trusted network.
`POST /command` accepts JSON requests. A valid request receives HTTP `202` with
a request ID and `QUEUED` status; work proceeds asynchronously through the
command queue, worker, dispatcher, and result store.

```json
{"camera_id":"CAM_007","operation":"GetDeviceInformation"}
```

`GetDeviceInformation` communicates with the configured camera at
`http://172.16.10.23:8080/`. After a successful ONVIF response, the application
inserts or updates that camera in `comms.db` with the temporary configured IP
address `172.16.10.23`. The IP address is not read from the ONVIF response. A
database write failure makes the result `FAILED` with a `DATABASE_ERROR`;
ONVIF failures do not create or update a paired-device record.

To list paired devices without contacting a camera, send:

```json
{"operation":"GetCurrentPairedDevices"}
```

`camera_id` is not required for this operation. Poll
`GET /result?id=<request_id>` for the current `QUEUED`, `PROCESSING`,
`SUCCESS`, or `FAILED` status. Device-information results keep their existing
response format. A successful paired-device result contains a `data` array of
`camera_id` and `ip_address` objects; when there are no records, it is `[]`.
Failures include an error category and message. Only `CAM_007` is configured
for ONVIF. `GetProfiles` and `GetPTZCapabilities` remain recognized but
explicitly unsupported.

The application creates `comms.db` in its current working directory at startup.
The database contains a `paired_devices` table, and its records persist across
restarts.

The generated gSOAP call uses bounded three-second connect, send, and receive
timeouts. Its connection attempt is also the availability check, so the HTTP
handler does not perform a duplicate network probe. No external JSON or HTTP
library is required: the project uses a small strict JSON parser for the
request shape and Winsock for HTTP.

Run the API with optional configuration overrides:

```powershell
.\comms_interface.exe [--listen-address 127.0.0.1] [--port 8090] [--camera-endpoint "http://172.16.10.23:8080/"]
```

Build from the project root in Windows PowerShell with MinGW-w64 and the
installed gSOAP 2.8.135 runtime. SQLite 3.54.1's official amalgamation source
is included in `third_party\sqlite`; compile the C source and then link the
application so no SQLite DLL or MinGW SQLite import library is required.

```powershell
gcc -std=c11 -O2 -c third_party\sqlite\sqlite3.c -o third_party\sqlite\sqlite3.o
g++ -std=c++17 -I"third_party\sqlite" -I"generated" -I"C:\Users\vaibh\gsoap_2.8.135\gsoap-2.8\gsoap" -pthread src\main.cpp generated\soapC.cpp generated\soapDeviceBindingProxy.cpp "C:\Users\vaibh\gsoap_2.8.135\gsoap-2.8\gsoap\stdsoap2.cpp" third_party\sqlite\sqlite3.o -lws2_32 -o comms_interface.exe
```

To exercise both operations, start `.\comms_interface.exe` in one PowerShell
window. In a second PowerShell window, from the project directory, post each
request and poll its result:

```powershell
$baseUrl = "http://127.0.0.1:8090"

function Wait-ForCommandResult([string]$requestId) {
    do {
        $result = Invoke-RestMethod -Uri "$baseUrl/result?id=$requestId"
        if ($result.status -eq "QUEUED" -or $result.status -eq "PROCESSING") {
            Start-Sleep -Milliseconds 250
        }
    } while ($result.status -eq "QUEUED" -or $result.status -eq "PROCESSING")
    $result
}

$deviceRequest = Invoke-RestMethod -Method Post `
    -Uri "$baseUrl/command" `
    -ContentType "application/json" `
    -Body '{"camera_id":"CAM_007","operation":"GetDeviceInformation"}'
Wait-ForCommandResult $deviceRequest.request_id | ConvertTo-Json -Depth 8

$pairedRequest = Invoke-RestMethod -Method Post `
    -Uri "$baseUrl/command" `
    -ContentType "application/json" `
    -Body '{"operation":"GetCurrentPairedDevices"}'
Wait-ForCommandResult $pairedRequest.request_id | ConvertTo-Json -Depth 8
```

The first request requires the configured camera to be reachable and stores its
configured IP after a successful ONVIF response. The second request requires no
camera and reads the database directly. Its successful `data` value is an array
of `camera_id` and `ip_address` objects (or `[]` if no devices are stored).

## Development Environment

- C++17
- MinGW-w64
- gSOAP 2.8.135
- SQLite 3.54.1 amalgamation (`sqlite3.c` and `sqlite3.h`)
