#pragma once
#include "StruxProvider.h"
#include "AuthManager/AuthManager.h"
#include "CommandManager/CommandManager.h"
#include "ConsoleManager/ConsoleManager.h"
#include "NetworkManager/NetworkManager.h"
#include "RelayManager/RelayManager.h"
#include "TelemetryManager/TelemetryManager.h"
#include "SettingsManager/SettingsManager.h"
#include "SystemManager/SystemManager.h"
#include "TimeManager/TimeManager.h"
#include "PartitionManager/PartitionManager.h"
#include "WebServerManager/WebServerManager.h"

// The framework layer's context: owns every Strux manager and answers StruxProvider.
//
// Init() carries the ORDER, and that is the point of it. The order has real constraints
// in it — a manager registering a setting needs SettingsManager up first, Telemetry
// leaves down the Relay's pipe — and while it lived in main.cpp every fork owned a copy
// of it. A fork that pulls a new Strux manager now gets its position too, instead of
// having to be told.
//
// One constraint that used to be here is gone: Relay had to follow WebServer because it
// borrowed the WebServer's Authenticator. The credential authority is AuthManager's now,
// and a transport takes a reference to it whenever it likes.
class StruxContext : public StruxProvider
{
public:
    StruxContext() = default;
    ~StruxContext() = default;
    StruxContext(const StruxContext&) = delete;
    StruxContext& operator=(const StruxContext&) = delete;

    /// Bring the framework up. Call after the board and before the application: the
    /// application registers into these managers, so they must exist and be ready first.
    void Init()
    {
        consoleManager_.Init();
        settingsManager_.Init();
        authManager_.Init();
        systemManager_.Init();
        networkManager_.Init();
        timeManager_.Init();
        commandManager_.Init();
        partitionManager_.Init();
        webServerManager_.Init();
        relayManager_.Init();
        // After Relay: telemetry leaves the device down the relay pipe.
        telemetryManager_.Init();
    }

    AuthManager& getAuthManager() override { return authManager_; }
    CommandManager& getCommandManager() override { return commandManager_; }
    ConsoleManager& getConsoleManager() override { return consoleManager_; }
    NetworkManager& getNetworkManager() override { return networkManager_; }
    RelayManager& getRelayManager() override { return relayManager_; }
    TelemetryManager& getTelemetryManager() override { return telemetryManager_; }
    SettingsManager& getSettingsManager() override { return settingsManager_; }
    SystemManager& getSystemManager() override { return systemManager_; }
    TimeManager& getTimeManager() override { return timeManager_; }
    PartitionManager& getPartitionManager() override { return partitionManager_; }
    WebServerManager& getWebServerManager() override { return webServerManager_; }

private:
    ConsoleManager consoleManager_{*this};
    SettingsManager settingsManager_{*this};
    AuthManager authManager_{*this};
    SystemManager systemManager_{*this};
    NetworkManager networkManager_{*this};
    TimeManager timeManager_{*this};
    CommandManager commandManager_{*this};
    PartitionManager partitionManager_{*this};
    WebServerManager webServerManager_{*this};
    RelayManager relayManager_{*this};
    TelemetryManager telemetryManager_{*this};
};
