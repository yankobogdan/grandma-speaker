#include "WiFi_Manager.h"
#include "GranVoice_Config.h"
#include <nvs_flash.h>

// Networks live in NVS as net0_ssid/net0_pass .. netN_ssid/netN_pass plus a
// count, so the device can follow the user between locations without being
// reflashed or reconfigured.
static const char* NVS_NS = "wifi";

WiFiManager::WiFiManager() {
    isConnected = false;
    connectedSSID = "";

    Serial.println("\n=== WiFi Manager Initializing ===");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.println("NVS partition was truncated - erasing and reinitializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        Serial.printf("NVS init failed: %s\n", esp_err_to_name(err));
    }

    // Seed any configured default that isn't stored yet. Idempotent, so adding
    // a network to the config later still takes effect on an already-provisioned
    // device instead of only mattering on a virgin NVS.
    if (strlen(GRANVOICE_DEFAULT_WIFI_SSID) > 0 && !isSaved(GRANVOICE_DEFAULT_WIFI_SSID)) {
        saveNetwork(GRANVOICE_DEFAULT_WIFI_SSID, GRANVOICE_DEFAULT_WIFI_PASS);
    }
    if (strlen(GRANVOICE_DEFAULT_WIFI2_SSID) > 0 && !isSaved(GRANVOICE_DEFAULT_WIFI2_SSID)) {
        saveNetwork(GRANVOICE_DEFAULT_WIFI2_SSID, GRANVOICE_DEFAULT_WIFI2_PASS);
    }

    Serial.printf("=== WiFi Manager Ready (%d saved network(s)) ===\n\n", getSavedCount());
}

int WiFiManager::getSavedCount() {
    preferences.begin(NVS_NS, true);
    int n = preferences.getInt("net_count", 0);
    preferences.end();
    if (n < 0) n = 0;
    if (n > MAX_SAVED_NETWORKS) n = MAX_SAVED_NETWORKS;
    return n;
}

String WiFiManager::getSavedSSID(int index) {
    if (index < 0 || index >= MAX_SAVED_NETWORKS) return "";
    preferences.begin(NVS_NS, true);
    String s = preferences.getString(("net" + String(index) + "_ssid").c_str(), "");
    preferences.end();
    return s;
}

String WiFiManager::getSavedPasswordFor(const String& ssid) {
    int n = getSavedCount();
    preferences.begin(NVS_NS, true);
    String pass = "";
    for (int i = 0; i < n; i++) {
        if (preferences.getString(("net" + String(i) + "_ssid").c_str(), "") == ssid) {
            pass = preferences.getString(("net" + String(i) + "_pass").c_str(), "");
            break;
        }
    }
    preferences.end();
    return pass;
}

bool WiFiManager::isSaved(const String& ssid) {
    int n = getSavedCount();
    for (int i = 0; i < n; i++) {
        if (getSavedSSID(i) == ssid) return true;
    }
    return false;
}

void WiFiManager::saveNetwork(const String& ssid, const String& password) {
    int n = getSavedCount();

    // Updating an existing entry keeps the list free of duplicates when a
    // password changes.
    int slot = -1;
    for (int i = 0; i < n; i++) {
        if (getSavedSSID(i) == ssid) { slot = i; break; }
    }
    if (slot < 0) {
        if (n < MAX_SAVED_NETWORKS) {
            slot = n;
            n++;
        } else {
            slot = 0; // full - overwrite the oldest
        }
    }

    preferences.begin(NVS_NS, false);
    preferences.putString(("net" + String(slot) + "_ssid").c_str(), ssid);
    preferences.putString(("net" + String(slot) + "_pass").c_str(), password);
    preferences.putInt("net_count", n);
    preferences.end();
    Serial.printf("Saved network '%s' in slot %d (%d total)\n", ssid.c_str(), slot, n);
}

void WiFiManager::forgetNetwork(const String& ssid) {
    int n = getSavedCount();
    String ssids[MAX_SAVED_NETWORKS], passes[MAX_SAVED_NETWORKS];
    int kept = 0;
    for (int i = 0; i < n; i++) {
        String s = getSavedSSID(i);
        if (s.length() == 0 || s == ssid) continue;
        ssids[kept] = s;
        passes[kept] = getSavedPasswordFor(s);
        kept++;
    }
    preferences.begin(NVS_NS, false);
    for (int i = 0; i < kept; i++) {
        preferences.putString(("net" + String(i) + "_ssid").c_str(), ssids[i]);
        preferences.putString(("net" + String(i) + "_pass").c_str(), passes[i]);
    }
    preferences.putInt("net_count", kept);
    preferences.end();
    Serial.printf("Forgot '%s' (%d remaining)\n", ssid.c_str(), kept);
}

bool WiFiManager::connectToNetwork(const char* ssid, const char* password) {
    // Disconnect first. Without this, switching networks while already
    // associated left WiFi.status() reporting WL_CONNECTED for the OLD network,
    // so a "successful" switch silently kept the previous connection.
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("    (disconnecting from %s first)\n", WiFi.SSID().c_str());
        WiFi.disconnect(false, true);
        unsigned long t0 = millis();
        while (WiFi.status() == WL_CONNECTED && millis() - t0 < 5000) delay(100);
    }

    WiFi.mode(WIFI_STA);
    // Let the radio settle before associating. Connecting immediately after a
    // scan fails often enough to matter (the vendor code had this delay; losing
    // it during the multi-network rewrite made the first boot attempt fail at
    // full signal, leaving the device offline until the 30s watchdog fired).
    delay(400);
    WiFi.begin(ssid, password);

    Serial.printf("    Connecting to '%s'", ssid);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        // Guard against associating with something other than what we asked for.
        if (WiFi.SSID() != String(ssid)) {
            Serial.printf("    Connected to '%s' but expected '%s' - treating as failure\n",
                          WiFi.SSID().c_str(), ssid);
            isConnected = false;
            return false;
        }
        isConnected = true;
        connectedSSID = ssid;
        Serial.printf("    CONNECTED to %s, IP %s, %d dBm\n",
                      ssid, WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return true;
    }

    Serial.printf("    FAILED (status %d)\n", (int)WiFi.status());
    isConnected = false;
    connectedSSID = "";
    WiFi.disconnect(false, true);
    delay(300);
    return false;
}

bool WiFiManager::connectAndSave(const String& ssid, const String& password) {
    if (!connectToNetwork(ssid.c_str(), password.c_str())) {
        Serial.println("Not saving - connection failed");
        return false;
    }
    saveNetwork(ssid, password);
    return true;
}

bool WiFiManager::connectToBestNetwork() {
    Serial.println("\n=== WiFi Manager Starting ===");
    WiFi.mode(WIFI_STA);

    int saved = getSavedCount();
    if (saved == 0) {
        Serial.println("No saved networks");
        return false;
    }

    // A stale cached scan is a common source of "no networks found", so the
    // previous results are dropped before scanning.
    WiFi.scanDelete();
    Serial.println("Scanning...");
    int n = WiFi.scanNetworks();
    if (n < 0) {
        Serial.printf("Scan failed (%d) - retrying once\n", n);
        WiFi.scanDelete();
        delay(500);
        n = WiFi.scanNetworks();
    }
    Serial.printf("Found %d networks\n", n);

    // Try saved networks that are actually in range, strongest first. scanNetworks
    // already returns results sorted by RSSI.
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0 || !isSaved(ssid)) continue;
        Serial.printf("  saved network in range: %s (%d dBm)\n", ssid.c_str(), WiFi.RSSI(i));
        String pass = getSavedPasswordFor(ssid);
        // Two attempts: the first association after a scan is unreliable even
        // with a strong signal, and a retry almost always succeeds.
        for (int attempt = 0; attempt < 2; attempt++) {
            if (attempt) Serial.println("    retrying once");
            if (connectToNetwork(ssid.c_str(), pass.c_str())) return true;
        }
    }

    Serial.println("=== No saved network could be joined ===");
    return false;
}

bool WiFiManager::retryKnownNetworks() {
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    int n = WiFi.scanNetworks();
    if (n <= 0) return false;

    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0 || !isSaved(ssid)) continue;
        if (connectToNetwork(ssid.c_str(), getSavedPasswordFor(ssid).c_str())) {
            Serial.printf("[WiFiRetry] reconnected to %s\n", ssid.c_str());
            return true;
        }
    }
    return false;
}

bool WiFiManager::getConnectionStatus() { return isConnected; }
String WiFiManager::getConnectedSSID()  { return connectedSSID; }
String WiFiManager::getIPAddress()      { return isConnected ? WiFi.localIP().toString() : String("Not connected"); }
