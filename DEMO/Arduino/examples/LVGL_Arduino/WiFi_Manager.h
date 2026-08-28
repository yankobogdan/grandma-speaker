#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

#define MAX_SAVED_NETWORKS 8

// Remembers several networks and picks whichever is in range, so the device
// follows you between locations without being reconfigured.
class WiFiManager {
private:
    String connectedSSID;
    bool isConnected;
    Preferences preferences;

public:
    WiFiManager();

    // Scan, then try saved networks that are actually in range, strongest first.
    bool connectToBestNetwork();

    // Quiet single-shot retry for the background watchdog - no UI updates.
    bool retryKnownNetworks();

    // Connect to one network and, on success, remember it for next time.
    bool connectAndSave(const String& ssid, const String& password);

    // Saved network list
    int  getSavedCount();
    String getSavedSSID(int index);
    bool isSaved(const String& ssid);
    void forgetNetwork(const String& ssid);

    // Status
    bool getConnectionStatus();
    String getConnectedSSID();
    String getIPAddress();

private:
    bool connectToNetwork(const char* ssid, const char* password);
    String getSavedPasswordFor(const String& ssid);
    void saveNetwork(const String& ssid, const String& password);
};
