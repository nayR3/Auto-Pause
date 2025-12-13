#include <Geode/Geode.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJEffectManager.hpp>
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <algorithm>
#include <geode.custom-keybinds/include/Keybinds.hpp>
// optional API
#include <geode.custom-keybinds/include/OptionalAPI.hpp>

using namespace keybinds;

using namespace geode::prelude;

enum class GameState {
    Menu,
    Level,
    Paused
};

static GameState lastState = GameState::Menu;
GameState state;
static bool g_paused = false;
static bool shouldMute= false;
static bool hasDied = false;
static bool deathHandled = false;
DWORD pid;
HWND hwnd;

bool inLevel() {
    return PlayLayer::get() != nullptr;
}

bool isAppRunning(const std::filesystem::path& targetExe) {
    std::string targetName = targetExe.filename().string(); // e.g., "firefox.exe"

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);

    if (Process32First(snapshot, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, targetName.c_str()) == 0) {
                CloseHandle(snapshot);
                return true; // found a running instance
            }
        } while (Process32Next(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return false;
}

DWORD getProcessPID(const std::filesystem::path& targetExe) {
    std::string targetName = targetExe.filename().string(); // e.g., "firefox.exe"

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);

    DWORD pid = 0;
    if (Process32First(snapshot, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, targetName.c_str()) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return pid; // 0 if not found
}

HWND findWindowByPID(DWORD pid) {
    HWND hwndFound = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        char className[256];
        GetClassNameA(hwnd, className, sizeof(className));
        if (IsWindowVisible(hwnd) && GetParent(hwnd) == nullptr && _stricmp(className, "MozillaWindowClass") == 0) {
            *reinterpret_cast<HWND*>(lParam) = hwnd;
            return FALSE; // stop
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&hwndFound));
    return hwndFound;
}

$on_mod(Loaded) {
    auto mediaPath = Mod::get()->getSettingValue<std::filesystem::path>("media-app");
    if (isAppRunning(mediaPath)) {
        log::info("App detected!");
        pid = getProcessPID(mediaPath);
        hwnd = findWindowByPID(pid);
    } else {
        log::info("App not detected!");
    }
}

$execute {
    BindManager::get()->registerBindable({
        // ID, should be prefixed with mod ID
        "pauseKey"_spr,
        // Name
        "Pause key",
        // Description, leave empty for none
        "Keybind for pause / mute / deafen.",
        // Default binds
        { Keybind::create(KEY_K, Modifier::None) },
        // Category; use slashes for specifying subcategories. See the
        // Category class for default categories
        "Auto Pause"
    });

    listenForSettingChanges("media-app", [](const std::filesystem::path& mediaPath) {
        if (isAppRunning(mediaPath)) {
            log::info("App detected!");
            pid = getProcessPID(mediaPath);
            hwnd = findWindowByPID(pid);
        } else {
            log::info("App not detected!");
        }
    });
}

void sendKey() {
    if (!pid || !hwnd) {
        log::warn("No media target available");
        return;
    }

    auto binds = BindManager::get()->getBindsFor("pauseKey"_spr);
    if (binds.empty()) return;

    // get raw pointer from Ref
    Bind* bindPtr = binds[0]; // Ref<T> implicitly converts to T*
    auto keybind = dynamic_cast<Keybind*>(bindPtr);
    if (!keybind) return;

    auto key = keybind->getKey();
    PostMessage(hwnd, WM_KEYDOWN, key, 0);
    PostMessage(hwnd, WM_KEYUP, key, 0);
}

void pause() { sendKey(); }

void unpause() { sendKey(); }

class $modify(PlayLayer) {

    void pauseGame(bool unfocused) {
        PlayLayer::pauseGame(unfocused); // original
        g_paused = true;
    }
    void updateProgressbar() {
        if (state == GameState::Level && !shouldMute && !this->m_isPracticeMode && !this->m_isTestMode) {
            float currentP = getCurrentPercent();
            double muteP = Mod::get()->getSettingValue<double>("muteP"); // read live
            if (currentP >= muteP && !hasDied) {
                shouldMute = true;
                log::info("Passed {}%", muteP);
                pause();
            }
        }
        PlayLayer::updateProgressbar();
    }
    void resetLevel() {
        deathHandled = false;
        hasDied = false;
        shouldMute = false;
        PlayLayer::resetLevel();
    }
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (!deathHandled) {
            double muteP = Mod::get()->getSettingValue<double>("muteP");
            if (getCurrentPercent() >= muteP) {
                deathHandled = true;
                hasDied = true;
                log::info("Player died past mute percent");
                unpause();
                shouldMute = false;
            }
        }
        PlayLayer::destroyPlayer(player, object);
    }
};

class $modify(GameManager) {

    void update(float dt) {
        GameManager::update(dt); // original

        // detect unpause
        if (g_paused && inLevel() && !PlayLayer::get()->m_isPaused) {
            g_paused = false;
        }

        if (!inLevel()) state = GameState::Menu;
        else if (g_paused) state = GameState::Paused;
        else state = GameState::Level;

        if (state != lastState) {
            lastState = state;
            if (state != GameState::Level && shouldMute){
                unpause();
                shouldMute = false;
            }
            switch (state) {
                case GameState::Menu:   log::info("In menu"); break;
                case GameState::Level:  log::info("In level (running)"); break;
                case GameState::Paused: log::info("In level (paused)"); break;
            }
        }
        // reset threshold flag when leaving level
        if (state != GameState::Level) {
            shouldMute = false;
        }
    }
};