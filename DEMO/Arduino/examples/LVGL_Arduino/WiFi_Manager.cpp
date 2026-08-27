#include "WiFi_Manager.h"
#include "GranVoice_Config.h"
#include "LVGL_Example.h"
#include <nvs_flash.h>

WiFiManager::WiFiManager() {
    isConnected = false;
    connectedSSID = "";
    passwordCount = 0;
    
    Serial.println("\n=== WiFi Manager Initializing ===");
    
    // Initialize NVS (Non-Volatile Storage)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.println("NVS partition was truncated - erasing and reinitializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    
    if (err != ESP_OK) {
        Serial.printf("⚠️ NVS initialization failed: %s\n", esp_err_to_name(err));
    } else {
        Serial.println("✓ NVS initialized successfully");
    }
    
    // Load passwords from flash (Preferences/NVS)
    loadPasswordsFromFlash();
    
    // If no passwords stored, add defaults
    if (passwordCount == 0) {
        Serial.println("No passwords in flash - adding defaults");
        addPassword(GRANVOICE_DEFAULT_WIFI_PASS);
        savePasswordsToFlash();
        Serial.println("Default passwords saved to flash");
        
        // Verify the save worked
        Serial.println("\n=== Verifying Flash Write ===");
        loadPasswordsFromFlash();
        if (passwordCount > 0) {
            Serial.println("✓ Verification successful - passwords are in flash!");
        } else {
            Serial.println("⚠️ Verification failed - passwords NOT saved to flash!");
        }
    }
    
    Serial.println("=== WiFi Manager Ready ===\n");
}

void WiFiManager::loadPasswordsFromFlash() {
    if (!preferences.begin("wifi", true)) {  // Read-only mode
        Serial.println("⚠️ Failed to open Preferences in read mode");
        passwordCount = 0;
        return;
    }
    
    passwordCount = preferences.getInt("pass_count", 0);
    
    Serial.printf("Loading %d passwords from flash storage...\n", passwordCount);
    
    for (int i = 0; i < passwordCount && i < MAX_PASSWORDS; i++) {
        String key = "pass_" + String(i);
        passwords[i] = preferences.getString(key.c_str(), "");
        Serial.printf("  Password %d: [%s] (length: %d)\n", i+1, passwords[i].c_str(), passwords[i].length());
    }
    
    preferences.end();
    Serial.println("Passwords loaded from flash");
}

void WiFiManager::savePasswordsToFlash() {
    if (!preferences.begin("wifi", false)) {  // Read-write mode
        Serial.println("⚠️ Failed to open Preferences in write mode!");
        Serial.println("   Flash storage may not be initialized properly");
        return;
    }
    
    Serial.printf("Saving %d passwords to flash storage...\n", passwordCount);
    
    size_t written = preferences.putInt("pass_count", passwordCount);
    Serial.printf("  putInt returned: %d bytes\n", written);
    
    for (int i = 0; i < passwordCount; i++) {
        String key = "pass_" + String(i);
        size_t size = preferences.putString(key.c_str(), passwords[i]);
        Serial.printf("  Saved: %s = [%s] (%d bytes)\n", key.c_str(), passwords[i].c_str(), size);
    }
    
    preferences.end();
    Serial.println("✓ Passwords saved to flash successfully");
}

void WiFiManager::addPassword(const String& password) {
    if (passwordCount < MAX_PASSWORDS) {
        passwords[passwordCount] = password;
        passwordCount++;
        Serial.printf("Added password: [%s] (total: %d)\n", password.c_str(), passwordCount);
    } else {
        Serial.println("Cannot add password - maximum reached");
    }
}

void WiFiManager::clearPasswords() {
    preferences.begin("wifi", false);
    preferences.clear();
    preferences.end();
    
    passwordCount = 0;
    Serial.println("All passwords cleared from flash");
}

int WiFiManager::getPasswordCount() {
    return passwordCount;
}

void WiFiManager::printStoredData() {
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║   WiFi Flash Storage Contents         ║");
    Serial.println("╚════════════════════════════════════════╝");
    
    if (!preferences.begin("wifi", true)) {  // Read-only
        Serial.println("⚠️ ERROR: Cannot open Preferences!");
        Serial.println("   NVS may not be initialized");
        return;
    }
    
    int count = preferences.getInt("pass_count", 0);
    Serial.printf("Password Count: %d\n\n", count);
    
    if (count == 0) {
        Serial.println("❌ No passwords stored in flash");
        Serial.println("   (This is normal on first boot before passwords are saved)");
    } else {
        Serial.println("Stored Passwords:");
        for (int i = 0; i < count; i++) {
            String key = "pass_" + String(i);
            String pass = preferences.getString(key.c_str(), "");
            Serial.printf("  %d. [%s] (length: %d chars)\n", i+1, pass.c_str(), pass.length());
        }
    }
    
    preferences.end();
    
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║   Current Runtime Status              ║");
    Serial.println("╚════════════════════════════════════════╝");
    Serial.printf("Active Passwords in Memory: %d\n", passwordCount);
    Serial.printf("WiFi Connected: %s\n", isConnected ? "YES" : "NO");
    if (isConnected) {
        Serial.printf("Connected SSID: %s\n", connectedSSID.c_str());
        Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("Signal Strength: %d dBm\n", WiFi.RSSI());
    }
    Serial.println("════════════════════════════════════════\n");
}

String WiFiManager::getSavedSSID() {
    preferences.begin("wifi", true);
    String s = preferences.getString("user_ssid", "");
    preferences.end();
    return s;
}

String WiFiManager::getSavedPassword() {
    preferences.begin("wifi", true);
    String p = preferences.getString("user_pass", "");
    preferences.end();
    return p;
}

bool WiFiManager::connectAndSave(const String& ssid, const String& password) {
    Serial.printf("Trying user-provisioned network: %s\n", ssid.c_str());
    if (!connectToNetwork(ssid.c_str(), password.c_str())) {
        Serial.println("User-provisioned network failed - not saving");
        return false;
    }
    preferences.begin("wifi", false);
    preferences.putString("user_ssid", ssid);
    preferences.putString("user_pass", password);
    preferences.end();
    Serial.printf("Saved user network '%s' to flash\n", ssid.c_str());
    return true;
}

bool WiFiManager::retryKnownNetworks() {
    // Saved (user-provisioned) network first, then the built-in known one.
    String userSSID = getSavedSSID();
    if (userSSID.length() > 0) {
        if (connectToNetwork(userSSID.c_str(), getSavedPassword().c_str())) {
            Serial.printf("[WiFiRetry] reconnected to saved network: %s\n", userSSID.c_str());
            return true;
        }
    }
    const char* knownNetworks[] = {GRANVOICE_DEFAULT_WIFI_SSID};
    const int knownCount = sizeof(knownNetworks) / sizeof(knownNetworks[0]);
    for (int k = 0; k < knownCount; k++) {
        for (int pw = 0; pw < passwordCount; pw++) {
            if (connectToNetwork(knownNetworks[k], passwords[pw].c_str())) {
                Serial.printf("[WiFiRetry] reconnected to: %s\n", knownNetworks[k]);
                return true;
            }
        }
    }
    return false;
}

bool WiFiManager::connectToBestNetwork() {
    Serial.println("\n=== WiFi Manager Starting ===");

    // Force reset WiFi to clear any previous state
    Serial.println("Resetting WiFi...");
    WiFi.disconnect(true);  // Disconnect and erase credentials
    WiFi.mode(WIFI_OFF);
    delay(500);
    WiFi.mode(WIFI_STA);
    delay(500);

    // A network the user entered on the touchscreen wins over the built-in list.
    String userSSID = getSavedSSID();
    if (userSSID.length() > 0) {
        String userPass = getSavedPassword();
        Serial.printf("Trying saved user network: %s\n", userSSID.c_str());
        LVGL_WiFi_Display("Connecting...");
        if (connectToNetwork(userSSID.c_str(), userPass.c_str())) {
            Serial.printf("Connected to saved user network: %s\n", userSSID.c_str());
            LVGL_WiFi_Display(userSSID.c_str());
            return true;
        }
        Serial.println("Saved user network failed, falling back to scan");
    }

    // Only networks we have a real reason to try - the vendor demo's original
    // behaviour (every stored password against every visible AP) took minutes
    // and broadcast the password at every stranger's router nearby. Anything
    // else is handled by on-screen provisioning + the periodic retry below.
    Serial.println("Scanning for known networks...");
    LVGL_WiFi_Display("Scanning WiFi...");

    int n = WiFi.scanNetworks();
    if (n <= 0) {
        Serial.println("ERROR: No networks found");
        LVGL_WiFi_Display("No WiFi found");
        return false;
    }

    Serial.printf("\n=== Found %d networks ===\n", n);
    for (int i = 0; i < n; i++) {
        Serial.printf("  %d: %-20s (RSSI: %d dBm, Ch: %d, Enc: %d)\n",
                      i+1, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                      WiFi.channel(i), WiFi.encryptionType(i));
    }

    const char* knownNetworks[] = {GRANVOICE_DEFAULT_WIFI_SSID};
    const int knownCount = sizeof(knownNetworks) / sizeof(knownNetworks[0]);

    for (int k = 0; k < knownCount; k++) {
        bool inRange = false;
        for (int i = 0; i < n; i++) {
            if (WiFi.SSID(i).equalsIgnoreCase(knownNetworks[k])) { inRange = true; break; }
        }
        if (!inRange) {
            Serial.printf("Known network '%s' not in range\n", knownNetworks[k]);
            continue;
        }
        for (int pw = 0; pw < passwordCount; pw++) {
            char displayBuf[64];
            snprintf(displayBuf, sizeof(displayBuf), "%s...", knownNetworks[k]);
            LVGL_WiFi_Display(displayBuf);
            Serial.printf("Trying known network '%s' (password %d/%d)\n", knownNetworks[k], pw+1, passwordCount);
            if (connectToNetwork(knownNetworks[k], passwords[pw].c_str())) {
                Serial.println("\n*** SUCCESS! ***");
                Serial.printf("Connected to: %s\n", knownNetworks[k]);
                Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
                Serial.printf("Signal strength: %d dBm\n", WiFi.RSSI());
                Serial.println("=== WiFi Connected ===\n");
                LVGL_WiFi_Display(knownNetworks[k]);
                return true;
            }
        }
    }

    Serial.println("\n=== FAILED: Could not connect to any network ===\n");
    LVGL_WiFi_Display("WiFi failed");
    return false;
}

bool WiFiManager::connectToNetwork(const char* ssid, const char* password) {
    // Add a small delay before attempting connection
    delay(1000);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    Serial.print("    Connecting");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {  // Increased to 30 attempts (15 seconds)
        delay(500);
        Serial.print(".");
        attempts++;
        
        // Check if we're stuck
        if (attempts % 10 == 0) {
            wl_status_t currentStatus = WiFi.status();
            Serial.printf(" [Status: %d] ", currentStatus);
        }
    }
    Serial.println();
    
    wl_status_t status = WiFi.status();
    
    if (status == WL_CONNECTED) {
        isConnected = true;
        connectedSSID = ssid;
        Serial.printf("    Status: CONNECTED (took %d attempts)\n", attempts);
        return true;
    } else {
        isConnected = false;
        connectedSSID = "";
        
        // Print detailed failure reason
        Serial.printf("    Status: FAILED after %d attempts - ", attempts);
        switch (status) {
            case WL_NO_SSID_AVAIL:
                Serial.println("SSID not available");
                break;
            case WL_CONNECT_FAILED:
                Serial.println("Connection failed (likely wrong password)");
                break;
            case WL_CONNECTION_LOST:
                Serial.println("Connection lost");
                break;
            case WL_DISCONNECTED:
                Serial.println("Disconnected (wrong password or incompatible encryption)");
                break;
            case WL_IDLE_STATUS:
                Serial.println("Idle (timeout)");
                break;
            default:
                Serial.printf("Unknown status: %d\n", status);
                break;
        }
        
        // Force disconnect and wait before next attempt
        WiFi.disconnect(true);
        delay(500);
        return false;
    }
}

bool WiFiManager::getConnectionStatus() {
    return isConnected;
}

String WiFiManager::getConnectedSSID() {
    return connectedSSID;
}

String WiFiManager::getIPAddress() {
    if (isConnected) {
        return WiFi.localIP().toString();
    }
    return "Not connected";
}
