#pragma once

// What one Strux manager may reach for. The framework layer's provider: implemented by
// StruxContext, handed to every manager at construction, and the only way a manager
// finds a peer.
//
// Note what is NOT here: the BoardContext. The framework does not touch hardware, and that is
// deliberate rather than incidental. Because there is no IBoard, a board only has to
// provide what the code above it actually calls — so if Strux called GetLed(), every
// board in every fork would owe an LED. Hardware belongs to the application, which has
// the BoardContext through AppProvider.
//
// Also not here: the application. Nothing in Strux may reach up. When the framework
// needs something from the application it is given it — the application registers a
// command, a setting, a telemetry point — and that inversion is what keeps the layers
// acyclic. A StruxManager that wanted AppProvider& would be a design error, not a
// missing accessor.

class AuthManager;
class CommandManager;
class ConsoleManager;
class NetworkManager;
class RelayManager;
class SettingsManager;
class SystemManager;
class TelemetryManager;
class TimeManager;
class PartitionManager;
class WebServerManager;

class StruxProvider
{
public:
    virtual AuthManager& getAuthManager() = 0;
    virtual CommandManager& getCommandManager() = 0;
    virtual ConsoleManager& getConsoleManager() = 0;
    virtual NetworkManager& getNetworkManager() = 0;
    virtual RelayManager& getRelayManager() = 0;
    virtual SettingsManager& getSettingsManager() = 0;
    virtual SystemManager& getSystemManager() = 0;
    virtual TelemetryManager& getTelemetryManager() = 0;
    virtual TimeManager& getTimeManager() = 0;
    virtual PartitionManager& getPartitionManager() = 0;
    virtual WebServerManager& getWebServerManager() = 0;
};
