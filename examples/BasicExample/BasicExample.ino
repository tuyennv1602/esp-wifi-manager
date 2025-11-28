#include "wifi_manager.h"

void setup()
{
    Serial.begin(115200);
    delay(100);

    // Optional: set a custom AP name shown when device creates config hotspot
    setAPName("KS_Device");

    // Start wifi manager (will attempt stored networks or start AP)
    wifiManagerBegin();
}

void loop()
{
    // Must be called from your sketch loop()
    wifiManagerLoop();

    // Place your normal app code here
    delay(10);
}
