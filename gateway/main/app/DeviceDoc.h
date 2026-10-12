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
    "OpenTherm by simulating a thermostat and a boiler (work in progress).";

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
    "that crossed it, received and sent, from a ring of the last 256 - poll it with "
    "the last seq seen (the OpenTherm page in the web UI does exactly that). "
    "`module send side=boiler frame=0x80190000` puts one frame on a line; a frame "
    "is the 32 bits on the wire, parity included, and nothing checks or adds it. "
    "`module link` reports the serial link to the module, and `module exec "
    "line=\"opentherm status\"` runs one of the module's own commands.\n"
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
