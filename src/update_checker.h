#ifndef UPDATE_CHECKER_H
#define UPDATE_CHECKER_H

#include <Arduino.h>
#include <mutex>
#include "config.h"

// Update check states
enum class UpdateCheckState {
    IDLE,
    CHECKING,
    DOWNLOADING,
    ERROR
};

// Update result info
struct UpdateInfo {
    bool available;
    char latestVersion[16];     // e.g., "1.7.0"
    char currentVersion[16];    // e.g., "1.6.0"
    char downloadUrl[196];      // GitHub release asset URL (firmware)
    char spiffsUrl[196];        // GitHub release asset URL (SPIFFS)
    char releaseUrl[128];       // GitHub releases page URL
    unsigned long lastCheckTime;// millis() of last successful check
    uint8_t downloadProgress;   // 0-100 for download progress
    char errorMessage[64];
};

class UpdateChecker {
public:
    void begin();
    void loop();

    // Check for updates (non-blocking trigger, actual check happens in loop)
    void checkForUpdates();

    // Get current state
    UpdateCheckState getState() const { return _state; }
    // Copy of the update info; _info is written by the check task and read
    // from the main loop and the async webserver task
    void getInfoSnapshot(UpdateInfo& out);
    bool isUpdateAvailable() const { return _info.available; }
    const char* getLatestVersion() const { return _info.latestVersion; }
    const char* getCurrentVersion() const { return _info.currentVersion; }
    const char* getReleaseUrl() const { return _info.releaseUrl; }
    const char* getErrorMessage() const { return _info.errorMessage; }
    uint8_t getDownloadProgress() const { return _info.downloadProgress; }
    unsigned long getLastCheckTime() const { return _info.lastCheckTime; }

    // Start OTA download from GitHub
    void startOTAUpdate();
    bool isDownloading() const { return _state == UpdateCheckState::DOWNLOADING; }

    // Callback for state changes (for MQTT publish trigger)
    typedef void (*UpdateStateCallback)();
    void onStateChange(UpdateStateCallback callback) { _stateCallback = callback; }

private:
    volatile UpdateCheckState _state = UpdateCheckState::IDLE;
    UpdateInfo _info;
    std::mutex _infoMutex;
    UpdateStateCallback _stateCallback = nullptr;

    unsigned long _lastAutoCheck = 0;
    bool _firstAutoCheckDone = false;
    unsigned long _bootTime = 0;  // Stored at begin() for overflow-safe timing
    volatile bool _checkRequested = false;
    volatile bool _otaRequested = false;

    // HTTP check runs in its own FreeRTOS task so the (up to 15s) TLS +
    // GitHub API call doesn't freeze the main loop (NFC, LED, captive portal)
    void startCheckTask();
    static void checkTask(void* arg);
    void performCheck();
    bool fetchGitHubRelease(UpdateInfo& info);
    bool parseReleaseJson(const char* json, size_t length, UpdateInfo& info);
    int compareVersions(const char* v1, const char* v2);
    void setErrorf(const char* format, ...);

    void performOTAUpdate();
    bool downloadAndInstall(const char* url, int updateType, const char* label);
};

extern UpdateChecker updateChecker;

#endif // UPDATE_CHECKER_H
