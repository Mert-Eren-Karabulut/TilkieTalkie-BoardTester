#include "ConfigManager.h"
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <vector>

static const char *TAG = "cfg";

namespace {
struct CfgLock {
    SemaphoreHandle_t m;
    explicit CfgLock(SemaphoreHandle_t h) : m(h) { if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY); }
    ~CfgLock() { if (m) xSemaphoreGiveRecursive(m); }
};

// Factory namespace keys (PROTOCOL-V2 §1). "tested" and "bringup" are reserved for the
// factory test record and the bring-up result (written by later phases).
const char *FK_SECRET = "secret";
const char *FK_POP = "pop";
const char *FK_SERIAL = "serial";
const char *FK_ENROLLED = "enrolled";

String g_nvsRepairNote; // set by the constructor (static init), taken once by main

// ---- NVS repair without losing the factory identity ---------------------------------------
// nvs_flash_init() answers ESP_ERR_NVS_NO_FREE_PAGES when the partition has no empty page
// left. The usual remedy (and the Arduino core's, inside initArduino()) is to erase the whole
// partition, which would also destroy the factory secret and pop: a unit whose label no longer
// matches its secret needs a re-enrollment at the factory. Instead the partition is mounted
// read-only (NVS needs no free page then), every entry is copied to PSRAM, the partition is
// erased, and the entries are written back, most valuable namespace first. A namespace that no
// longer fits is erased as a whole (never half-restored) and named in the note.
struct SavedEntry {
    char ns[NVS_NS_NAME_MAX_SIZE];
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
    uint64_t num;      // integer types
    uint8_t *data;     // strings (with the NUL) and blobs, PSRAM
    size_t len;
};

int namespacePriority(const char *ns)
{
    static const char *order[] = {"factory", "config", "nvs.net80211", "settings", "faults", "requestmgr"};
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
        if (strcmp(ns, order[i]) == 0) return (int)i;
    }
    return 100;
}

bool readEntry(const char *part, const nvs_entry_info_t &info, SavedEntry &out)
{
    memset(&out, 0, sizeof(out));
    strlcpy(out.ns, info.namespace_name, sizeof(out.ns));
    strlcpy(out.key, info.key, sizeof(out.key));
    out.type = info.type;
    nvs_handle_t h;
    if (nvs_open_from_partition(part, info.namespace_name, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t r = ESP_FAIL;
    switch (info.type) {
    case NVS_TYPE_U8: { uint8_t v; r = nvs_get_u8(h, info.key, &v); out.num = v; break; }
    case NVS_TYPE_I8: { int8_t v; r = nvs_get_i8(h, info.key, &v); out.num = (uint64_t)(int64_t)v; break; }
    case NVS_TYPE_U16: { uint16_t v; r = nvs_get_u16(h, info.key, &v); out.num = v; break; }
    case NVS_TYPE_I16: { int16_t v; r = nvs_get_i16(h, info.key, &v); out.num = (uint64_t)(int64_t)v; break; }
    case NVS_TYPE_U32: { uint32_t v; r = nvs_get_u32(h, info.key, &v); out.num = v; break; }
    case NVS_TYPE_I32: { int32_t v; r = nvs_get_i32(h, info.key, &v); out.num = (uint64_t)(int64_t)v; break; }
    case NVS_TYPE_U64: { uint64_t v; r = nvs_get_u64(h, info.key, &v); out.num = v; break; }
    case NVS_TYPE_I64: { int64_t v; r = nvs_get_i64(h, info.key, &v); out.num = (uint64_t)v; break; }
    case NVS_TYPE_STR:
    case NVS_TYPE_BLOB: {
        size_t len = 0;
        r = (info.type == NVS_TYPE_STR) ? nvs_get_str(h, info.key, nullptr, &len) : nvs_get_blob(h, info.key, nullptr, &len);
        if (r != ESP_OK) break;
        out.data = static_cast<uint8_t *>(heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!out.data) out.data = static_cast<uint8_t *>(malloc(len ? len : 1));
        if (!out.data) { r = ESP_ERR_NO_MEM; break; }
        out.len = len;
        r = (info.type == NVS_TYPE_STR) ? nvs_get_str(h, info.key, (char *)out.data, &len)
                                        : nvs_get_blob(h, info.key, out.data, &len);
        break;
    }
    default:
        break;
    }
    nvs_close(h);
    if (r != ESP_OK) {
        free(out.data);
        out.data = nullptr;
        return false;
    }
    return true;
}

esp_err_t writeEntry(nvs_handle_t h, const SavedEntry &e)
{
    switch (e.type) {
    case NVS_TYPE_U8: return nvs_set_u8(h, e.key, (uint8_t)e.num);
    case NVS_TYPE_I8: return nvs_set_i8(h, e.key, (int8_t)(int64_t)e.num);
    case NVS_TYPE_U16: return nvs_set_u16(h, e.key, (uint16_t)e.num);
    case NVS_TYPE_I16: return nvs_set_i16(h, e.key, (int16_t)(int64_t)e.num);
    case NVS_TYPE_U32: return nvs_set_u32(h, e.key, (uint32_t)e.num);
    case NVS_TYPE_I32: return nvs_set_i32(h, e.key, (int32_t)(int64_t)e.num);
    case NVS_TYPE_U64: return nvs_set_u64(h, e.key, e.num);
    case NVS_TYPE_I64: return nvs_set_i64(h, e.key, (int64_t)e.num);
    case NVS_TYPE_STR: return nvs_set_str(h, e.key, (const char *)e.data);
    case NVS_TYPE_BLOB: return nvs_set_blob(h, e.key, e.data, e.len);
    default: return ESP_ERR_NOT_SUPPORTED;
    }
}

// Runs at static-init time (before Serial and Faults): results go to ESP_LOG and the note.
esp_err_t repairFullNvs()
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NVS_DEFAULT_PART_NAME);
    if (!part) return ESP_ERR_NOT_FOUND;
    // NVS keeps the pointer for as long as the partition is mounted: static storage.
    static esp_partition_t readOnlyView;
    readOnlyView = *part;
    readOnlyView.readonly = true;

    std::vector<SavedEntry> saved;
    int unreadable = 0;
    if (nvs_flash_init_partition_ptr(&readOnlyView) == ESP_OK) {
        nvs_iterator_t it = nullptr;
        esp_err_t r = nvs_entry_find(part->label, nullptr, NVS_TYPE_ANY, &it);
        while (r == ESP_OK) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            SavedEntry e;
            if (readEntry(part->label, info, e)) saved.push_back(e); else unreadable++;
            r = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        nvs_flash_deinit_partition(part->label);
    } else {
        ESP_LOGE(TAG, "NVS: read-only mount failed too, nothing can be kept");
    }

    esp_err_t err = nvs_flash_erase_partition(part->label);
    if (err == ESP_OK) err = nvs_flash_init();
    if (err != ESP_OK) {
        for (auto &e : saved) free(e.data);
        return err;
    }

    std::stable_sort(saved.begin(), saved.end(), [](const SavedEntry &a, const SavedEntry &b) {
        return namespacePriority(a.ns) < namespacePriority(b.ns);
    });
    std::vector<String> dropped;
    size_t restored = 0;
    for (size_t i = 0; i < saved.size();) {
        // One namespace at a time: all of it or nothing.
        size_t j = i;
        while (j < saved.size() && strcmp(saved[j].ns, saved[i].ns) == 0) j++;
        nvs_handle_t h;
        bool ok = nvs_open(saved[i].ns, NVS_READWRITE, &h) == ESP_OK;
        if (ok) {
            for (size_t k = i; k < j && ok; ++k) ok = writeEntry(h, saved[k]) == ESP_OK;
            if (!ok) nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        if (ok) restored += j - i; else dropped.push_back(saved[i].ns);
        i = j;
    }
    for (auto &e : saved) free(e.data);

    g_nvsRepairNote = "nvs full, reformatted";
    if (unreadable) g_nvsRepairNote += " lost=" + String(unreadable);
    for (const String &ns : dropped) g_nvsRepairNote += " -" + ns;
    ESP_LOGW(TAG, "NVS had no free page: reformatted, %u entr%s restored, %d unreadable, %u namespace(s) dropped",
             (unsigned)restored, restored == 1 ? "y" : "ies", unreadable, (unsigned)dropped.size());
    return ESP_OK;
}
}

// Static member definitions
ConfigManager* ConfigManager::instance = nullptr;
const char* ConfigManager::NAMESPACE = "config";
const char* ConfigManager::SETTINGS_NAMESPACE = "settings";
const char* ConfigManager::WIFI_SSID_KEY = "ssid";        // Shortened key
const char* ConfigManager::WIFI_PASSWORD_KEY = "pass";     // Shortened key
const char* ConfigManager::DEVICE_NAME_KEY = "device";     // Shortened key
const char* ConfigManager::PROVISIONING_PIN_KEY = "pin";   // Shortened key
const char* ConfigManager::JWT_TOKEN_KEY = "jwt";         // Shortened key
const char* ConfigManager::TOKEN_DAYS_KEY = "tok_days";
const char* ConfigManager::FACTORY_NAMESPACE = "factory";

ConfigManager::ConfigManager() {
    // Initialize NVS. This runs during static construction, before initArduino(): when it
    // succeeds here, the Arduino core's own "erase the whole partition" fallback never runs.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // Full partition: keep what can be kept (factory identity first), see repairFullNvs().
        err = repairFullNvs();
    } else if (err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Written by a newer NVS format (firmware downgrade): this firmware cannot read any of
        // it, and initArduino() would erase it anyway. Erase it here so the event is reported.
        ESP_LOGE(TAG, "NVS written by a newer format - erasing (factory identity lost)");
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
        g_nvsRepairNote = "nvs newer format, erased";
    }
    if (err != ESP_OK) {
        // No abort: a boot loop helps nobody. Preferences fail below and openFailed reports it.
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }
    lock = xSemaphoreCreateRecursiveMutex();
    
    // Now initialize preferences. A failure is NOT answered by erasing the whole NVS (that
    // would also drop the WiFi credentials, the fault ring and the OTA state): it is logged,
    // reported as CONFIG_CORRUPT once Faults is up, and begin() is retried on the next access.
    opened = preferences.begin(NAMESPACE, false);
    if (!opened) {
        ESP_LOGE(TAG, "ERROR: Failed to initialize preferences!");
        openFailed = true;
    }
}

bool ConfigManager::ensureOpen() {
    if (!opened) {
        opened = preferences.begin(NAMESPACE, false);
    }
    return opened;
}

String ConfigManager::takeNvsRepairNote() {
    CfgLock g(lock);
    String note = g_nvsRepairNote;
    g_nvsRepairNote = String();
    return note;
}

bool ConfigManager::takeOpenFailure() {
    CfgLock g(lock);
    const bool failed = openFailed;
    openFailed = false;
    return failed;
}

ConfigManager::~ConfigManager() {
    preferences.end();
}

ConfigManager& ConfigManager::getInstance() {
    if (instance == nullptr) {
        instance = new ConfigManager();
    }
    return *instance;
}

String ConfigManager::getWiFiSSID() {
    CfgLock g(lock);
    if (!ensureOpen() || !preferences.isKey(WIFI_SSID_KEY)) {
        return "";
    }
    return preferences.getString(WIFI_SSID_KEY, "");
}

String ConfigManager::getWiFiPassword() {
    CfgLock g(lock);
    if (!ensureOpen() || !preferences.isKey(WIFI_PASSWORD_KEY)) {
        return "";
    }
    return preferences.getString(WIFI_PASSWORD_KEY, "");
}

void ConfigManager::setWiFiCredentials(const String& ssid, const String& password) {
    // An empty password is an open network (PROTOCOL-V2 §6): stored and mirrored like any other.
    if (ssid.length() == 0) {
        return;
    }
    CfgLock g(lock);
    if (!ensureOpen()) {
        ESP_LOGE(TAG, "ERROR: Failed to store WiFi credentials");
        return;
    }
    // Overwrite in place (no remove first: a reset between remove and put used to lose the
    // credentials), and only when they changed (flash wear).
    if (preferences.isKey(WIFI_SSID_KEY) && preferences.isKey(WIFI_PASSWORD_KEY) &&
        preferences.getString(WIFI_SSID_KEY, "") == ssid && preferences.getString(WIFI_PASSWORD_KEY, "") == password) {
        return;
    }
    size_t ssidResult = preferences.putString(WIFI_SSID_KEY, ssid);
    size_t passResult = preferences.putString(WIFI_PASSWORD_KEY, password);
    
    // putString("") stores an empty string and reports 0 bytes: only a non-empty password can fail that way.
    if (ssidResult == 0 || (passResult == 0 && password.length() > 0)) {
        ESP_LOGE(TAG, "ERROR: Failed to store WiFi credentials");
    }
}

bool ConfigManager::hasWiFiCredentials() {
    CfgLock g(lock);
    // Check if keys exist before trying to read them to avoid NVS errors
    if (!ensureOpen() || !preferences.isKey(WIFI_SSID_KEY) || !preferences.isKey(WIFI_PASSWORD_KEY)) {
        return false;
    }
    
    // Only read if keys exist
    String ssid = preferences.getString(WIFI_SSID_KEY, "");
    String password = preferences.getString(WIFI_PASSWORD_KEY, "");
    
    (void)password; // open networks have an empty password
    return ssid.length() > 0;
}

void ConfigManager::clearWiFiCredentials() {
    CfgLock g(lock);
    if (!ensureOpen()) return;
    preferences.remove(WIFI_SSID_KEY);
    preferences.remove(WIFI_PASSWORD_KEY);
}

// Getters never write: the default is derived again on every read until something is set.
String ConfigManager::getDeviceName() {
    String deviceName;
    {
        CfgLock g(lock);
        if (ensureOpen() && preferences.isKey(DEVICE_NAME_KEY)) {
            deviceName = preferences.getString(DEVICE_NAME_KEY, "");
        }
    }
    if (deviceName.length() == 0) {
        deviceName = String(F("TilkieTalkie_")) + String(ESP.getEfuseMac());
    }
    return deviceName;
}

void ConfigManager::setDeviceName(const String& name) {
    CfgLock g(lock);
    if (ensureOpen()) preferences.putString(DEVICE_NAME_KEY, name);
}

String ConfigManager::getProvisioningPin() {
    String pin;
    {
        CfgLock g(lock);
        if (ensureOpen() && preferences.isKey(PROVISIONING_PIN_KEY)) {
            pin = preferences.getString(PROVISIONING_PIN_KEY, "");
        }
    }
    if (pin.length() == 0) {
        pin = F("abcd1234");
    }
    return pin;
}

void ConfigManager::setProvisioningPin(const String& pin) {
    CfgLock g(lock);
    if (ensureOpen()) preferences.putString(PROVISIONING_PIN_KEY, pin);
}


void ConfigManager::storeCurrentWiFiCredentials() {
    if (WiFi.isConnected()) {
        setWiFiCredentials(WiFi.SSID(), WiFi.psk());
    }
}

void ConfigManager::resetAll() {
    CfgLock g(lock);
    if (ensureOpen()) preferences.clear();
}

void ConfigManager::printAllSettings() {
    CfgLock g(lock);
    ESP_LOGI(TAG, "=== Configuration Settings ===");
    ESP_LOGI(TAG, "WiFi SSID: %s", getWiFiSSID().c_str());
    ESP_LOGI(TAG, "WiFi Password: %s", getWiFiPassword().length() > 0 ? "***" : "Not set");
    ESP_LOGI(TAG, "Device Name: %s", getDeviceName().c_str());
#ifdef TT_BENCH
    ESP_LOGI(TAG, "Provisioning PIN (legacy): %s", getProvisioningPin().c_str());
#endif
    ESP_LOGI(TAG, "Factory identity: %s (serial %s)", isEnrolled() ? "enrolled" : "not enrolled",
             factorySerial().length() ? factorySerial().c_str() : "-");
    ESP_LOGI(TAG, "JWT Token: %s (%u day(s) old)", getJWTToken().length() > 0 ? "Set" : "Not set", (unsigned)getTokenDays());
    ESP_LOGI(TAG, "Config Valid: %s", isValid() ? "Yes" : "No");
    ESP_LOGI(TAG, "Free NVS Space: %u bytes", (unsigned)getFreeSpace());
    
    // Settings namespace info
    Preferences settingsPrefs;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, true)) {
        ESP_LOGI(TAG, "Settings Namespace Entries: %u", (unsigned)settingsPrefs.freeEntries());
        settingsPrefs.end();
    }
    
    // Essential NVS statistics only
    nvs_stats_t nvs_stats;
    if (nvs_get_stats(NULL, &nvs_stats) == ESP_OK) {
        ESP_LOGI(TAG, "NVS Stats: %u used, %u free", (unsigned)nvs_stats.used_entries, (unsigned)nvs_stats.free_entries);
    }
    
    ESP_LOGI(TAG, "==============================");
}

bool ConfigManager::isValid() {
    // Check if the configuration is in a valid state
    // At minimum, device name and provisioning pin should be set
    return getDeviceName().length() > 0 && getProvisioningPin().length() > 0;
}

void ConfigManager::commit() {
    // Intentionally empty: Preferences::put* commits to flash before returning.
}

size_t ConfigManager::getFreeSpace() {
    CfgLock g(lock);
    return ensureOpen() ? preferences.freeEntries() : 0;
}

void ConfigManager::factoryReset() {
    ESP_LOGI(TAG, "=== FACTORY RESET ===");
    ESP_LOGW(TAG, "WARNING: This will erase ALL stored data!");
    CfgLock g(lock);
    
    // Close preferences first
    preferences.end();
    opened = false;
    
    // Erase entire NVS flash
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ERROR: Failed to erase NVS flash");
        ESP_LOGE(TAG, "Error code: %d", (int)err);
    } else {
        ESP_LOGI(TAG, "SUCCESS: NVS flash erased");
    }
    
    // Reinitialize NVS
    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ERROR: Failed to reinitialize NVS");
        ESP_LOGE(TAG, "Error code: %d", (int)err);
    } else {
        ESP_LOGI(TAG, "SUCCESS: NVS reinitialized");
    }
    
    // Reopen preferences
    opened = preferences.begin(NAMESPACE, false);
    if (opened) {
        ESP_LOGI(TAG, "SUCCESS: Preferences reopened");
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to reopen preferences");
    }
    
    ESP_LOGI(TAG, "=== FACTORY RESET COMPLETE ===");
    ESP_LOGI(TAG, "Device will restart in 3 seconds...");
    delay(3000);
    ESP.restart();
}

String ConfigManager::getJWTToken() {
    CfgLock g(lock);
    if (!ensureOpen() || !preferences.isKey(JWT_TOKEN_KEY)) {
        return String();
    }
    return preferences.getString(JWT_TOKEN_KEY, "");
}

void ConfigManager::setJWTToken(const String &token) {
    CfgLock g(lock);
    if (!ensureOpen()) return;
    if (preferences.isKey(JWT_TOKEN_KEY) && preferences.getString(JWT_TOKEN_KEY, "") == token) {
        return; // unchanged: no flash write
    }
    preferences.putString(JWT_TOKEN_KEY, token);
    if (preferences.isKey(TOKEN_DAYS_KEY)) {
        preferences.remove(TOKEN_DAYS_KEY); // a new token starts at day 0
    }
}

void ConfigManager::clearJWTToken() {
    CfgLock g(lock);
    if (!ensureOpen()) return;
    if (preferences.isKey(JWT_TOKEN_KEY)) preferences.remove(JWT_TOKEN_KEY);
    if (preferences.isKey(TOKEN_DAYS_KEY)) preferences.remove(TOKEN_DAYS_KEY);
}

uint16_t ConfigManager::getTokenDays() {
    CfgLock g(lock);
    if (!ensureOpen() || !preferences.isKey(TOKEN_DAYS_KEY)) return 0;
    return preferences.getUShort(TOKEN_DAYS_KEY, 0);
}

void ConfigManager::setTokenDays(uint16_t days) {
    CfgLock g(lock);
    if (ensureOpen()) preferences.putUShort(TOKEN_DAYS_KEY, days);
}

// ---- Factory namespace ----------------------------------------------------------------------

String ConfigManager::factoryString(const char *key) {
    Preferences fp;
    String value;
    if (fp.begin(FACTORY_NAMESPACE, true)) { // read-only open fails quietly when the namespace does not exist yet
        if (fp.isKey(key)) value = fp.getString(key, "");
        fp.end();
    }
    return value;
}

bool ConfigManager::hasFactorySecret() {
    CfgLock g(lock);
    return factoryString(FK_SECRET).length() > 0;
}

String ConfigManager::factorySecret() {
    CfgLock g(lock);
    return factoryString(FK_SECRET); // never logged
}

String ConfigManager::factoryPop() {
    CfgLock g(lock);
    return factoryString(FK_POP);
}

String ConfigManager::factorySerial() {
    CfgLock g(lock);
    return factoryString(FK_SERIAL);
}

bool ConfigManager::isEnrolled() {
    CfgLock g(lock);
    Preferences fp;
    bool enrolled = false;
    if (fp.begin(FACTORY_NAMESPACE, true)) {
        enrolled = fp.isKey(FK_ENROLLED) && fp.getUChar(FK_ENROLLED, 0) != 0;
        fp.end();
    }
    return enrolled;
}

bool ConfigManager::setFactory(const String &secret, const String &pop, const String &serial) {
    CfgLock g(lock);
    Preferences fp;
    if (!fp.begin(FACTORY_NAMESPACE, false)) {
        ESP_LOGE(TAG, "factory namespace cannot be opened");
        return false;
    }
    bool ok = fp.putString(FK_SECRET, secret) == secret.length() && fp.putString(FK_POP, pop) == pop.length();
    if (ok) {
        if (serial.length() > 0) {
            ok = fp.putString(FK_SERIAL, serial) == serial.length();
        } else if (fp.isKey(FK_SERIAL)) {
            fp.remove(FK_SERIAL);
        }
    }
    // "enrolled" last: a reset in the middle leaves a unit that is visibly not enrolled.
    ok = ok && fp.putUChar(FK_ENROLLED, 1) == 1;
    fp.end();
    ESP_LOGI(TAG, "factory identity %s", ok ? "stored" : "could not be stored");
    return ok;
}

void ConfigManager::clearFactory() {
    CfgLock g(lock);
    Preferences fp;
    if (fp.begin(FACTORY_NAMESPACE, false)) {
        fp.clear(); // secret, pop, serial, enrolled and the reserved tested / bringup records
        fp.end();
    }
    ESP_LOGW(TAG, "factory identity erased");
}

// Simple settings storage methods
void ConfigManager::storeInt(const String& keyname, int value) {
    CfgLock g(lock);
    Preferences settingsPrefs;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, false)) {
        size_t result = settingsPrefs.putInt(keyname.c_str(), value);
        if (result == 0) {
            ESP_LOGE(TAG, "ERROR: Failed to store int setting: %s", keyname.c_str());
        }
        settingsPrefs.end();
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to open settings namespace for storing int");
    }
}

void ConfigManager::storeString(const String& keyname, const String& value) {
    CfgLock g(lock);
    Preferences settingsPrefs;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, false)) {
        size_t result = settingsPrefs.putString(keyname.c_str(), value);
        if (result == 0) {
            ESP_LOGE(TAG, "ERROR: Failed to store string setting: %s", keyname.c_str());
        }
        settingsPrefs.end();
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to open settings namespace for storing string");
    }
}

int ConfigManager::getInt(const String& keyname, int defaultValue) {
    CfgLock g(lock);
    Preferences settingsPrefs;
    int value = defaultValue;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, true)) {  // Read-only mode
        if (settingsPrefs.isKey(keyname.c_str())) {
            value = settingsPrefs.getInt(keyname.c_str(), defaultValue);
        }
        settingsPrefs.end();
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to open settings namespace for reading int");
    }
    return value;
}

String ConfigManager::getString(const String& keyname, const String& defaultValue) {
    CfgLock g(lock);
    Preferences settingsPrefs;
    String value = defaultValue;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, true)) {  // Read-only mode
        if (settingsPrefs.isKey(keyname.c_str())) {
            value = settingsPrefs.getString(keyname.c_str(), defaultValue);
        }
        settingsPrefs.end();
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to open settings namespace for reading string");
    }
    return value;
}

void ConfigManager::deleteSetting(const String& keyname) {
    CfgLock g(lock);
    Preferences settingsPrefs;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, false)) {
        // Deleting a key that was never written is normal (OTA bookkeeping clears all its keys);
        // Preferences::remove() logs an error for that, so check first.
        if (settingsPrefs.isKey(keyname.c_str()) && !settingsPrefs.remove(keyname.c_str())) {
            ESP_LOGW(TAG, "Failed to delete setting: %s", keyname.c_str());
        }
        settingsPrefs.end();
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to open settings namespace for deleting setting");
    }
}

void ConfigManager::deleteAllSettings() {
    CfgLock g(lock);
    Preferences settingsPrefs;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, false)) {
        bool result = settingsPrefs.clear();
        if (result) {
            ESP_LOGI(TAG, "SUCCESS: All settings cleared");
        } else {
            ESP_LOGE(TAG, "ERROR: Failed to clear all settings");
        }
        settingsPrefs.end();
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to open settings namespace for clearing all settings");
    }
}
