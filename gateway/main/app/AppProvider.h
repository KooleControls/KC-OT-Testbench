#pragma once

// What one application manager may reach for: its peers, the framework beneath it, and
// the board beneath that. Implemented by AppContext and handed to every app
// manager at construction — the same shape as StruxProvider one layer down, so a manager
// still takes exactly one reference and finds everything through it.
//
// The two extra accessors are what make this the top layer: getStrux() reaches down to
// the framework (register a command, read a setting, take a telemetry point) and
// getBoard() reaches past it to the hardware. Neither points back up, and nothing in
// Strux or on the BoardContext can see this interface at all.

class BoardContext;
class StruxProvider;
class LedManager;
class ModuleFirmwareManager;
class ModuleLinkManager;
class RoutingManager;
class Rs485Manager;

class AppProvider
{
public:
    /// The framework layer. Everything Strux offers is behind this one call.
    virtual StruxProvider& getStrux() = 0;

    /// The hardware. Only the application layer has this — see StruxProvider.h for why.
    virtual BoardContext& getBoard() = 0;

    // ── This application's own managers ──
    virtual LedManager& getLedManager() = 0;
    virtual ModuleFirmwareManager& getModuleFirmwareManager() = 0;
    virtual ModuleLinkManager& getModuleLinkManager() = 0;
    virtual RoutingManager& getRoutingManager() = 0;
    virtual Rs485Manager& getRs485Manager() = 0;
};
