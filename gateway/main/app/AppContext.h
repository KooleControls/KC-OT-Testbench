#pragma once

#include "AppProvider.h"
#include "BoardContext.h"
#include "StruxProvider.h"
#include "SystemManager.h"
#include "NetworkManager.h"
#include "DeviceDoc.h"
#include "LedManager/LedManager.h"
#include "ModuleFirmwareManager/ModuleFirmwareManager.h"
#include "ModuleLinkManager/ModuleLinkManager.h"
#include "RoutingManager/RoutingManager.h"
#include "HvacSimManager/HvacSimManager.h"
#include "Rs485Manager/Rs485Manager.h"

// The application layer's context: owns this product's managers and answers AppProvider.
//
// This is the file a fork edits. Strux's own managers, their init order and their wiring
// all live one layer down in StruxContext, so pulling an improvement from the template
// does not touch anything here — which is the whole reason the layers were split.
//
// Adding an application manager means: create the class taking AppProvider&, add an
// accessor to AppProvider, add the member here, and call its Init() below. The framework
// does not need to be told it exists; the manager registers its own commands and
// settings into Strux from its Init().
class AppContext : public AppProvider
{
public:
    AppContext(BoardContext& board, StruxProvider& strux)
        : board_(board), strux_(strux) {}

    ~AppContext() = default;
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    /// Bring the application up. Called last: every manager here registers into the
    /// framework, so the framework has to be ready before any of this runs.
    void Init()
    {
        // What this product is, in the product's own words — registered like
        // everything else the application tells the framework about itself, and read
        // back by `system describe` and by the relay hello. See DeviceDoc.h.
        strux_.getSystemManager().SetDocumentation(
            DeviceDoc::DESCRIPTION, DeviceDoc::INSTRUCTIONS);

        // Ethernet: the board installed the driver, the framework runs the network on
        // it. Wired here because this is the one layer that sees both.
        strux_.getNetworkManager().AttachEthernet(board_.GetEthernet());

        ledManager_.Init();

        // Starts checking the PCB1246 on a task of its own; nothing here waits for it.
        moduleFirmwareManager_.Init();

        // Before the link: the link hands every frame a line receives to the router.
        routingManager_.Init();

        // The link to the module's firmware. Waits out the check above on its own:
        // the two share the module's UART under ModuleFirmwareManager's lock.
        moduleLinkManager_.Init();

        // The simulated HVAC units, before the bus they answer on asks for them.
        hvacSimManager_.Init();

        // The RS485 bus, where the units on Modbus RTU answer. Nothing else uses the port.
        rs485Manager_.Init();
    }

    StruxProvider& getStrux() override { return strux_; }
    BoardContext& getBoard() override { return board_; }
    LedManager& getLedManager() override { return ledManager_; }
    ModuleFirmwareManager& getModuleFirmwareManager() override { return moduleFirmwareManager_; }
    ModuleLinkManager& getModuleLinkManager() override { return moduleLinkManager_; }
    RoutingManager& getRoutingManager() override { return routingManager_; }
    HvacSimManager& getHvacSimManager() override { return hvacSimManager_; }
    Rs485Manager& getRs485Manager() override { return rs485Manager_; }

private:
    BoardContext& board_;
    StruxProvider& strux_;

    LedManager ledManager_{*this};
    ModuleFirmwareManager moduleFirmwareManager_{*this};
    ModuleLinkManager moduleLinkManager_{*this};
    RoutingManager routingManager_{*this};
    HvacSimManager hvacSimManager_{*this};
    Rs485Manager rs485Manager_{*this};
};
