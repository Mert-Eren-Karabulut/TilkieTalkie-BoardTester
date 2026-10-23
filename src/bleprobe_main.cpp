// Bare-minimum BLE controller probe (built ONLY with -DBLE_PROBE_BUILD): starts NimBLE with
// nothing else running, to tell a toolchain/sdkconfig problem from a runtime-state one.
#ifdef BLE_PROBE_BUILD

#include <Arduino.h>
#include <esp_heap_caps.h>
#include "nimble/nimble_port.h"

// Arduino core 3.3.7+ frees the BLE controller memory in initArduino() unless the sketch says
// Bluetooth is in use (only the core's own BLE library says so). This firmware drives BLE
// through the IDF provisioning manager, so without this hook the controller later initialises
// into memory that already belongs to the heap and the unit boot-loops on every fresh (erased)
// flash. Provisioned boots release the memory themselves once BLE is not needed.
extern "C" bool btInUse(void)
{
    return true;
}

void setup()
{
    Serial.begin(115200);
    delay(3000);
    Serial.printf("BLE PROBE: internal free=%u largest=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    esp_err_t r = nimble_port_init();
    Serial.printf("BLE PROBE: nimble_port_init -> %d (%s)\n", (int)r, esp_err_to_name(r));
}

void loop()
{
    delay(2000);
    Serial.println("BLE PROBE: alive");
}

#endif // BLE_PROBE_BUILD
