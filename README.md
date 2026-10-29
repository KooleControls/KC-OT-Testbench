# KC-OT-Testbench

Test bench for OpenTherm on the KC1245 gateway and the PCB1246 OpenTherm plug-in board, built on the Strux framework. The goal is a rig that simulates both ends of the OpenTherm bus, a thermostat and a boiler, so OpenTherm devices and the gateway can be tested without real equipment. Work in progress. Lives at [KooleControls/KC-OT-Testbench](https://github.com/KooleControls/KC-OT-Testbench).

- `gateway/`: ESP32 firmware for the KC1245 gateway. A copy of [Strux](https://github.com/vanBassum/Strux) at commit `3c2e4ca`.
- `opentherm/`: LPC11U68 firmware for the PCB1246 (bare metal, no RTOS, Strux-style). Laid out like the gateway: the board, the Strux command/settings/system managers, and the OpenTherm modem with its link to the gateway.

Each folder holds its full source; nothing is shared between the two.

Decided so far:
- The LPC runs bare metal: one main loop, interrupts only set flags and fill buffers. No RTOS.
- The gateway and the PCB1246 talk over the UART using the Strux channel protocol instead of KC2.
- On the UART, each channel frame is COBS-framed with a CRC-16: `0x00 | COBS(frame | crc16) | 0x00`. A frame that fails the check is dropped and counted. The code is `lib/protocol/SerialFraming.h`, meant to be the same file on both sides.
- The PCB1246 is a modem and does not look inside a frame. Frames from either line go to the gateway on a stream the PCB1246 opens (`opentherm stream`); the gateway sends with `opentherm send side=<thermostat|boiler> frame=<uint32>`. Everything else is the gateway's.
- The gateway keeps the PCB1246's firmware up to date. A release builds `opentherm/` first and links its `.bin` into the gateway app, so one gateway OTA updates both boards. At boot, `ModuleFirmwareManager` resets the module into its ISP bootloader and compares the 32-byte image info at 0x300 (version, length, CRC-32, written by `tools/lpc_image_info.py`) with the image it carries. On a mismatch it flashes, with page 0 written last. `module status` / `module update`; the `module.update` setting turns the boot check off.
- The gateway's end of the modem link is `ModuleLinkManager`. It runs the same framing and handshake as the module, sends `opentherm send` requests, and records the module's frame stream, plus what it sent, in a ring of the last 256 frames. The web UI's OpenTherm page polls that ring with `module frames after=<seq>`, decodes each frame for display (`frontend/src/lib/opentherm.ts`), and can compose and send one. The module's UART is shared with `ModuleFirmwareManager` under its lock; after every ISP session the link starts over.
- How the thermostat and boiler simulation works is still open.
- The KC1245's RS485 port (MAX14830 UART 3, the chip driving the transceiver) carries Modbus RTU, 8N1 at `rs485.baud` (9600, a CoolPlug's default). The testbench is a slave there and never the master. `Rs485Manager` reads every frame on the bus, keeps the last 256 for the web UI's RS485 page (`rs485 frames`), and answers for the CoolPlugs it simulates; `rs485 send` puts a request on the bus by hand. A frame's end is found from its own length and CRC (`ModbusSplitter`), not from RTU's 3.5-character silence, which the MAX14830's FIFO hides.
- Up to ten CoolPlugs are simulated (`CoolPlugSimulator`), each at its own Modbus address, with the register map of the CoolPlug PRM rev 0.3, so a gateway configured for CoolPlugs cannot tell them from real ones. The indoor unit's state (power, mode, setpoint, fan, louvres, filter sign) lives in RAM and starts the same at every boot; the web UI plays its remote (`coolplug set`, `coolplug filter`). The simulation itself is settings, `cp1.*` to `cp10.*`: on the bus, address, room temperature, failure code, indoor unit connected, answering, reply delay. The room temperature reported is the set value ±0.1 to 0.5 °C, new every 10 s. What the PRM does not say (what unmapped registers and out-of-range values get, the HVAC type) is guessed, marked so in the code, and still to be checked against a real CoolPlug on the bus.

## Build

Gateway, board `pcb1245`, with ESP-IDF 6.0:

```sh
cd gateway
idf.py -DBOARD=pcb1245 set-target esp32
idf.py -DBOARD=pcb1245 build
```

OpenTherm board, board `pcb1246`, with the Arm GNU Toolchain (`arm-none-eabi-gcc`, on PATH or in its default Windows install folder), CMake 3.20+, Ninja and Python 3:

```sh
cd opentherm
cmake -S . -B build -G Ninja
cmake --build build
```

Output in `opentherm/build/`: `opentherm.elf`, `.hex`, `.bin` and `.map`, with the LPC boot checksum and the image info already patched in.

Build `opentherm/` before `gateway/`: the gateway build embeds `opentherm/build/opentherm.bin` when it exists, and warns and leaves the module alone when it does not (`-DPCB1246_FIRMWARE=<path>` points it elsewhere).

## Release

Push a tag `vX.Y.Z` (or `vX.Y.Z-rc.N` for a pre-release). `.github/workflows/release.yml` builds both boards, checks that both report the tag's version, and publishes the gateway app and factory images plus the PCB1246 `.bin`/`.hex`. Running the workflow by hand builds the same files as artifacts without releasing.
