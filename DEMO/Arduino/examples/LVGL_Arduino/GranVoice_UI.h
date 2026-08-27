#pragma once

void GranVoice_UI_Init(void); // builds the talk screen, wires GeminiLive callbacks, starts the state machine
void GranVoice_UI_ShowWifiSetup(void); // jump straight to network picking (used when WiFi won't connect)
