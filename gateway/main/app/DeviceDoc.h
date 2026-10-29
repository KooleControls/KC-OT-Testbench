#pragma once

// ──────────────────────────────────────────────────────────────
// What this product is, and how it is meant to be driven — in the product's own
// words, in the application layer, beside the managers the words are about.
//
// It lives here and not in the relay for one reason: whatever answers "how do I use
// this device" has to be the firmware that is actually running. A copy kept
// server-side is a copy that describes last month's build, and the one caller who
// cannot notice is the one most likely to act on it.
//
// Registered into the framework from AppContext::Init(), like every other thing the
// application tells Strux about itself (see SystemManager::SetDocumentation). Strux
// never reaches up for it.
//
// Two strings, on purpose:
//
//   DESCRIPTION   one line. It rides in the relay hello, so it is in a device list
//                 before anything asks the device a question. Sentence, not paragraph.
//
//   INSTRUCTIONS  free-form, as long as it needs to be, served only when asked for
//                 (`system describe`). Deliberately prose and NOT a schema: every
//                 command already declares its own name, arguments, types and
//                 required/optional state, and this is for the part that never fits
//                 in a declaration — which commands belong together, what order they
//                 go in, what the units are, what will not work and why.
//
// A fork replaces both when it replaces LedManager with a real feature.
// ──────────────────────────────────────────────────────────────
namespace DeviceDoc {

inline constexpr const char* DESCRIPTION =
    "KC-OT-Testbench - a KC1245 with a PCB1246 OpenTherm module, for testing "
    "OpenTherm by simulating a thermostat and a boiler, and Modbus gateways by "
    "simulating HVAC units on RS485 (work in progress).";

inline constexpr const char* INSTRUCTIONS =
    "This is the KC-OT-Testbench firmware, built on the Strux framework. It "
    "drives a PCB1246 OpenTherm module and is meant to simulate a thermostat and a "
    "boiler for testing OpenTherm; that simulation is still in progress. It also "
    "keeps the Strux demonstration: an LED that is lit while the device is connected "
    "to its relay server and dark when it is not.\n"
    "\n"
    "Discovering what it can do\n"
    "  Every command declares itself. `help` returns every command this firmware "
    "has, with its description and each of its arguments - name, type, whether it "
    "is required, and what it means - in one reply. Nothing about a command is "
    "written down twice, so what it returns is always this exact build.\n"
    "\n"
    "The demonstration feature\n"
    "  `led get` reports three separate facts: whether the indicator is enabled, "
    "whether the relay link is up, and whether the LED is lit at this instant. "
    "`led set enabled=false` keeps the board dark without affecting the link. The "
    "setting is persisted, so it survives a reboot.\n"
    "\n"
    "The PCB1246 OpenTherm module\n"
    "  This firmware carries the module's firmware and keeps the module running it. "
    "At every boot it resets the module into its bootloader, compares, and flashes "
    "it when it runs anything else - another build, the old firmware, or nothing. "
    "`module status` says what it found, with progress while flashing (16 pages "
    "of 4 KB take tens of seconds). `module update` runs the check again; "
    "`module update force=true` flashes even when it matches. The module's "
    "OpenTherm lines are down while it is in the bootloader. Set `module.update` "
    "false to leave the module alone at boot, e.g. while it runs a build flashed "
    "over SWD.\n"
    "\n"
    "OpenTherm traffic\n"
    "  The module is a modem: it passes frames between its two lines and this "
    "testbench, and decodes nothing. `module frames after=<seq>` returns the frames "
    "on the bench, each once with who sent it (from) and who it went to (to), from a "
    "ring of the last 256 - poll it with the last seq seen (the web UI's Frames page "
    "does exactly that). "
    "`module send side=boiler frame=0x80190000` puts one frame on a line; a frame "
    "is the 32 bits on the wire, parity included, and nothing checks or adds it. "
    "`module link` reports the serial link to the module, and `module exec "
    "line=\"opentherm status\"` runs one of the module's own commands.\n"
    "\n"
    "Routing and simulators\n"
    "  Four ends: the real thermostat and boiler on the module's lines, and a "
    "thermostat and a boiler simulator on this testbench. Each thermostat is "
    "connected to at most one boiler and the other way round: `route get` shows it, "
    "`route set master=thermostat slave=boilerSim` changes it (slave=open leaves it "
    "unconnected; saved). A frame goes from each end to its partner. A simulator "
    "runs while it is connected: the thermostat simulator asks once per interval, "
    "the boiler simulator answers every request. Their behaviour is the settings "
    "tsim.* and bsim.*, read with every frame; `sim status` shows what each has heard "
    "from the other end.\n"
    "\n"
    "The RS485 bus and the HVAC simulators\n"
    "  The board's RS485 port carries Modbus RTU, 8N1 at `rs485.baud` (9600, a "
    "CoolPlug's default). The testbench is a slave there and never the master. "
    "`rs485 frames after=<seq>` returns every frame on the bus - requests, replies and "
    "noise, as hex, with who sent it - from a ring of the last 256; the web UI's RS485 "
    "page decodes them. `rs485 send frame=\"01 03 00 61 00 01\"` puts a request on the "
    "bus with the CRC added. Ten simulated HVAC units, 1 to 10, can answer there, set up "
    "as the gateway's HVAC list is: each a type, by the gateway's numbers, and a Modbus "
    "address. Simulated on Modbus RTU: 3 SAM (Samsung via Intesis), 4 DAI (Daikin), "
    "7 MIT (Mitsubishi via Intesis), 8 COP (CoolPlug) and 9 HIS (Hisense B544E), each "
    "with the register map the gateway's driver for it uses. 5 CMN and 6 CMA "
    "(CoolMasterNet) are reached over Modbus TCP, which is not served yet; 0 leaves the "
    "unit unused. `hvac status` reports them, with the registers the gateway reads as "
    "they read now; `hvac set unit=1 power=true mode=1 setpoint=215` changes an indoor "
    "unit as its own remote would (codes are the type's own, setpoint in tenths of a "
    "degree), and `hvac filter unit=1 on=true` raises a CoolPlug's filter sign. The "
    "type, the address, whether a unit answers, its room temperature and the faults to "
    "simulate are the settings hvac1.* to hvac10.*, read again every second.\n"
    "\n"
    "Settings\n"
    "  `settings list` enumerates every setting on the device with its type and "
    "current value; `settings set` changes one in RAM and `settings save` commits it "
    "to NVS. A change is not durable until it is saved. Keys are dotted "
    "(`relay.url`, `led.enabled`) and are at most 15 characters, which is a storage "
    "limit and not a style.\n"
    "\n"
    "Things worth knowing before driving this device\n"
    "  * `system reboot` answers first and then restarts about half a second later, "
    "so every connection drops and anything in flight is lost.\n"
    "  * The `partition` commands write flash. `partition write` and "
    "`partition clear` change what the device boots; they exist for firmware "
    "updates and are not a general storage API.\n"
    "  * `web read` serves the device's own web UI files. It is how the relay "
    "renders this device's page; it is not a filesystem.\n"
    "  * Two codecs carry a command, and the device picks by the request line: one "
    "starting with '{' is JSON and is answered in JSON; anything else is read as a "
    "console line (`led set enabled=true`) and is answered in YAML. Nothing is "
    "negotiated and both reach the same commands.\n"
    "  * A reply is one record unless the command says otherwise. `web read` and "
    "`partition read` are the exceptions: they write a header record declaring a "
    "contentType and then raw bytes.\n"
    "  * Numbers in arguments are unsigned 32-bit. There are no floating-point "
    "arguments anywhere in the protocol.";

} // namespace DeviceDoc
