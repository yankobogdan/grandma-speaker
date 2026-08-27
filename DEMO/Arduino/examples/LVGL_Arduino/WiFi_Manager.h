#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

#define MAX_PASSWORDS 10

class WiFiManager {
private:
    String passwords[MAX_PASSWORDS];
    int passwordCount;
    String connectedSSID;
    bool isConnected;
    Preferences preferences;
    
    // Load/Save preferences
    void loadPasswordsFromFlash();
    void savePasswordsToFlash();

public:
    WiFiManager();
    
    // Connection management
    bool connectToBestNetwork();
    bool connectToNetwork(const char* ssid, const char* password);
    
    // Password management
    void addPassword(const String& password);
    void clearPasswords();
    int getPasswordCount();
    void printStoredData();

    // User-provisioned network (entered on the touchscreen, stored in NVS so it
    // survives reboots and takes priority over the built-in password list).
    bool connectAndSave(const String& ssid, const String& password);
    String getSavedSSID();
    String getSavedPassword();

    // Silent single-shot retry of known credentials, for the background
    // watchdog - no scanning, no on-screen status churn.
    bool retryKnownNetworks();

    // Status getters
    bool getConnectionStatus();
    String getConnectedSSID();
    String getIPAddress();
};
