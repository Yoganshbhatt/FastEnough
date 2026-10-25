// Toolchain smoke test: proves the Linux build compiles the portable subset
// (adb_discovery, qrcodegen) warning-clean and that its parsers behave.
#include <cassert>
#include <cstdio>

#include "adb_discovery.h"

int main() {
    // Matches `adb devices -l` output shape for a WiFi device. The address is
    // from the documentation range (RFC 5737); no real device data is used.
    const char* sample =
        "List of devices attached\n"
        "192.0.2.10:40007       device product:example model:Example_Phone device:example transport_id:1\n";
    const std::vector<DeviceInfo> devices = parseAdbDevices(sample);
    assert(devices.size() == 1);
    assert(devices[0].serial == "192.0.2.10:40007");
    assert(devices[0].state == "device");
    assert(isWirelessAdbSerial(devices[0].serial));
    assert(!isWirelessAdbSerial("AB1234CD"));

    AdbDeviceSnapshot snap;
    assert(snap.update(devices));   // first update always significant
    assert(!snap.update(devices));  // identical list: no change

    std::puts("afm_smoke: OK (adb_discovery parsers behave)");
    return 0;
}
