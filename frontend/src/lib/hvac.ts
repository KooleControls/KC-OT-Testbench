// The HVAC types the testbench can simulate, as a person reads them: the gateway's
// HVAC list (Smarthome, HvacManager/Types/HvacType.h), each with its codes and its
// register map. The firmware's profiles (main/app/Simulators/Hvac) are what answers on
// the bus; this names what they answer, for the Simulators and RS485 pages. Each map
// is the one the gateway's driver for that type uses, so the two are kept in step by
// hand -- a change there is a change here.

import type { RegisterInfo, RegisterMap, Table } from "./modbus"

// ── Values ───────────────────────────────────────────────────────

const signed = (v: number) => (v >= 0x8000 ? v - 0x10000 : v)

/** °C x10, signed. */
const tenths = (v: number) => `${(signed(v) / 10).toFixed(1)} °C`
/** Whole °C, signed. */
const celsius = (v: number) => `${signed(v)} °C`
const onOff = (v: number) => (v === 0 ? "Off" : "On")
const yesNo = (v: number) => (v === 0 ? "No" : "Yes")
const lock = (v: number) => (v === 0 ? "Free" : "Locked")
const code = (v: number) => (v === 0 ? "none (OK)" : String(v))
const hex = (v: number) => "0x" + v.toString(16).toUpperCase().padStart(4, "0")

/** A value from a list of codes; one that is not in it shows as itself, with a ?. */
function named(names: Record<number, string>) {
  return (v: number) => names[v] ?? `${v}?`
}

const reg = (name: string, format: (v: number) => string = String): RegisterInfo => ({ name, format })

export interface Choice {
  value: number
  label: string
}

const choices = (names: Record<number, string>): Choice[] =>
  Object.entries(names).map(([v, label]) => ({ value: Number(v), label }))

export const DEMANDS = ["No demand", "Heating", "Cooling"] as const

// ── The CoolPlug (CoolPlug PRM rev 0.3, section 2.2) ─────────────

const COOLPLUG_MODES = { 0: "Cool", 1: "Heat", 2: "Auto", 3: "Dry", 4: "Heat aux", 5: "Fan" }
const COOLPLUG_FANS = { 0: "Low", 1: "Medium", 2: "High", 3: "Auto", 4: "Top", 5: "Very low" }
const COOLPLUG_SWINGS = { 0: "Vertical", 1: "30°", 2: "45°", 3: "60°", 4: "Horizontal", 5: "Auto", 6: "Off", 7: "No louvres" }
const LINE_ROLES = { 0: "Slave", 1: "Master", 2: "Auto" }

function lineState(v: number): string {
  const parts = [v & 0x01 ? "indoor connected" : "indoor NOT connected"]
  if (v & 0x04) parts.push("failure")
  if (v & 0x08) parts.push("boot mode")
  if (v & 0x10) parts.push("not configured")
  return parts.join(", ")
}

function compactStatus(v: number): string {
  const parts = [
    v & 0x01 ? "On" : "Off",
    named(COOLPLUG_MODES)((v >> 1) & 0x1f),
    `fan ${named(COOLPLUG_FANS)((v >> 6) & 0x0f)}`,
    `louvres ${named(COOLPLUG_SWINGS)((v >> 10) & 0x07)}`,
  ]
  if (v & 0x2000) parts.push("filter")
  if (v & 0x4000) parts.push("therm on")
  return parts.join(", ")
}

/** The CoolPlug's registers by their address ON THE WIRE: the PRM's table numbers minus one. */
const COOLPLUG_REGISTERS: Record<number, RegisterInfo> = {
  1: reg("Reset", (v) => (v === 1 ? "Enter boot mode" : v === 2 ? "Restart" : String(v))),
  29: reg("VAUT changeover threshold", tenths),
  30: reg("VAUT as auto mode", yesNo),
  31: reg("HVAC type index"),
  32: reg("Firmware version", (v) => `${Math.floor(v / 100)}.${Math.floor(v / 10) % 10}.${v % 10}`),
  33: reg("Serial number, low word", hex),
  34: reg("Serial number, high word", hex),
  35: reg("Modbus address"),
  36: reg("Master/slave on the HVAC line", named(LINE_ROLES)),
  37: reg("Indoor centralised address"),
  95: reg("Line state", lineState),
  96: reg("Compact indoor status", compactStatus),
  97: reg("Setpoint", tenths),
  98: reg("Room temperature", tenths),
  99: reg("Failure code", code),
  100: reg("Own room temperature", tenths),
  143: reg("Line state", lineState),
  144: reg("On/off", onOff),
  145: reg("Mode", named(COOLPLUG_MODES)),
  146: reg("Fan speed", named(COOLPLUG_FANS)),
  147: reg("Louvres", named(COOLPLUG_SWINGS)),
  148: reg("Filter sign", yesNo),
  149: reg("Setpoint", tenths),
  150: reg("Room temperature", tenths),
  151: reg("Failure code", code),
  152: reg("Own room temperature", tenths),
  153: reg("Operation status", named({ 0: "No demand", 1: "Heating", 2: "Cooling" })),
}

// ── The Hisense B544(E): status and control apart, with codes of their own ──

const HISENSE_CONTROL_MODES = { 0: "Fan", 1: "Heat", 2: "Cool", 3: "Dry", 4: "Auto" }
const HISENSE_STATUS_MODES = { 0: "Fan", 1: "Heat", 2: "Cool", 3: "Dry", 5: "Auto (cooling)", 6: "Auto (heating)", 7: "Auto (fan)" }
const HISENSE_FANS = { 0: "Auto", 1: "High", 2: "Low", 3: "Medium" }
const HISENSE_CONTROL_SWINGS = { 0: "Still", 1: "Up/down", 2: "Left/right", 3: "Both" }
const HISENSE_STATUS_SWINGS = { 0: "Still", 1: "Left/right", 2: "Up/down", 3: "Both" }

// ── The Intesis boxes and the Daikin ─────────────────────────────

const INTESIS_MODES = { 0: "Auto", 1: "Heat", 2: "Dry", 3: "Fan", 4: "Cool" }
const FOUR_FANS = { 0: "Low", 1: "Medium", 2: "High", 3: "Auto" }
const FOUR_SWINGS = { 0: "Off", 1: "Vertical", 2: "Horizontal", 3: "Auto" }
const DAIKIN_MODES = { 1: "Heat", 2: "Auto", 3: "Cool" }
const SAMSUNG_SWINGS = { 0: "Off", 1: "Swing" }

// ── The types ────────────────────────────────────────────────────

export interface HvacType {
  /** The gateway's number. */
  id: number
  /** As the gateway's HVAC list shows it. */
  code: string
  name: string
  /** How the gateway reaches it. */
  via: string
  /** Whether the testbench plays it, and if not, why not. */
  simulated: boolean
  why?: string
  /** What its remote sets, in its own codes: the ones its control registers take. */
  modes: Choice[]
  fans: Choice[]
  swings: Choice[]
  /** °C. */
  setpoint: { min: number; max: number; step: number }
  /** Whether it reports a room temperature, has a filter sign, lets the gateway lock
   *  its remote, and says whether it reaches its indoor unit. */
  room: boolean
  filter: boolean
  lock: boolean
  indoor: boolean
  registers: RegisterMap
  /** Where the register map is from, and what is guessed. */
  source: string
}

const NONE: Omit<HvacType, "id" | "code" | "name" | "via" | "simulated"> = {
  modes: [],
  fans: [],
  swings: [],
  setpoint: { min: 10, max: 35, step: 0.1 },
  room: false,
  filter: false,
  lock: false,
  indoor: false,
  registers: {},
  source: "",
}

/** Every type the gateway has, by its number, so a unit is set up as it is there. */
export const HVAC_TYPES: HvacType[] = [
  { ...NONE, id: 0, code: "NA", name: "none", via: "", simulated: false, why: "Not used, as in the gateway's list." },
  {
    ...NONE,
    id: 1,
    code: "OTH",
    name: "OpenTherm",
    via: "OpenTherm",
    simulated: false,
    why: "Not on Modbus: the Boiler simulator tab plays an OpenTherm boiler.",
  },
  {
    ...NONE,
    id: 2,
    code: "REL",
    name: "relay on/off",
    via: "a relay",
    simulated: false,
    why: "A relay contact on the gateway: nothing on a bus to answer.",
  },
  {
    id: 3,
    code: "SAM",
    name: "Samsung via Intesis",
    via: "Modbus RTU",
    simulated: true,
    modes: choices(INTESIS_MODES),
    fans: choices(FOUR_FANS),
    swings: choices(SAMSUNG_SWINGS),
    setpoint: { min: 10, max: 35, step: 0.1 },
    room: true,
    filter: false,
    lock: true,
    indoor: false,
    registers: {
      holding: {
        0: reg("On/off", onOff),
        1: reg("Mode", named(INTESIS_MODES)),
        2: reg("Fan speed", named(FOUR_FANS)),
        3: reg("Louvres", named(SAMSUNG_SWINGS)),
        4: reg("Setpoint", tenths),
        5: reg("Room temperature", tenths),
        11: reg("Error code", code),
        17: reg("Remote control", lock),
      },
    },
    source:
      "Holding registers 0-5, 11 and 17, as the gateway's driver (HvacSamsung.cpp) uses them; no manual was at hand. The setpoint range (10-35 °C) is a guess.",
  },
  {
    id: 4,
    code: "DAI",
    name: "Daikin",
    via: "Modbus RTU",
    simulated: true,
    modes: choices(DAIKIN_MODES),
    fans: choices(FOUR_FANS),
    swings: choices(FOUR_SWINGS),
    setpoint: { min: 10, max: 35, step: 0.1 },
    room: false,
    filter: false,
    lock: true,
    indoor: false,
    registers: {
      holding: {
        1: reg("Setpoint", tenths),
        2: reg("Fan speed", named(FOUR_FANS)),
        3: reg("Mode", named(DAIKIN_MODES)),
        4: reg("Louvres", named(FOUR_SWINGS)),
        5: reg("On/off", onOff),
        10: reg("Remote control (global update)", lock),
        121: reg("Failure", yesNo),
        122: reg("Failure code", code),
      },
    },
    source:
      "Holding registers 1-5, 10, 121 and 122, as the gateway's driver (HvacDaikin.cpp) uses them; no manual was at hand. It reports no room temperature. Other modes than heat, auto and cool, and the setpoint range (10-35 °C), are guesses.",
  },
  {
    ...NONE,
    id: 5,
    code: "CMN",
    name: "CoolMasterNet",
    via: "Modbus TCP",
    simulated: false,
    why: "Reached over the network, at unit 1 with a block of registers per indoor unit. Not simulated yet: the testbench has no Modbus TCP server.",
  },
  {
    ...NONE,
    id: 6,
    code: "CMA",
    name: "CoolMasterNet (Aedes)",
    via: "Modbus TCP",
    simulated: false,
    why: "As CMN: reached over the network, and not simulated yet.",
  },
  {
    id: 7,
    code: "MIT",
    name: "Mitsubishi via Intesis",
    via: "Modbus RTU",
    simulated: true,
    modes: choices(INTESIS_MODES),
    fans: choices(FOUR_FANS),
    swings: choices(FOUR_SWINGS),
    setpoint: { min: 10, max: 35, step: 0.1 },
    room: true,
    filter: false,
    lock: true,
    indoor: false,
    registers: {
      holding: {
        0: reg("On/off", onOff),
        1: reg("Mode", named(INTESIS_MODES)),
        2: reg("Fan speed", named(FOUR_FANS)),
        3: reg("Louvres", named(FOUR_SWINGS)),
        4: reg("Setpoint", tenths),
        5: reg("Room temperature", tenths),
        8: reg("Remote control", lock),
        10: reg("Failure", yesNo),
        11: reg("Failure code", code),
      },
    },
    source:
      "Holding registers 0-5, 8, 10 and 11 of an Intesis INMBSMHI001R000, as the gateway's driver (HvacMitsubishi.cpp) uses them; no manual was at hand. The setpoint range (10-35 °C) is a guess.",
  },
  {
    id: 8,
    code: "COP",
    name: "CoolPlug",
    via: "Modbus RTU",
    simulated: true,
    modes: choices(COOLPLUG_MODES),
    fans: choices(COOLPLUG_FANS),
    swings: choices(COOLPLUG_SWINGS),
    setpoint: { min: 10, max: 35, step: 0.1 },
    room: true,
    filter: true,
    lock: false,
    indoor: true,
    registers: { holding: COOLPLUG_REGISTERS },
    source:
      "The CoolPlug's own register map (PRM rev 0.3); the gateway reads 95, 97, 144-147, 150 and 151. A gateway can move the unit to another address by writing register 35 and restarting it (register 1); the new address is saved too.",
  },
  {
    id: 9,
    code: "HIS",
    name: "Hisense B544(E)",
    via: "Modbus RTU",
    simulated: true,
    modes: choices(HISENSE_CONTROL_MODES),
    fans: choices(HISENSE_FANS),
    swings: choices(HISENSE_CONTROL_SWINGS),
    setpoint: { min: 18, max: 32, step: 1 },
    room: true,
    filter: false,
    lock: false,
    indoor: false,
    registers: {
      discrete: { 0: reg("On/off", onOff) },
      input: {
        1: reg("Room temperature", celsius),
        2: reg("Setpoint", celsius),
        7: reg("Mode", named(HISENSE_STATUS_MODES)),
        8: reg("Fan speed", named(HISENSE_FANS)),
        9: reg("Louvres", named(HISENSE_STATUS_SWINGS)),
        12: reg("Failure code", code),
      },
      coils: { 0: reg("On/off", onOff) },
      holding: {
        0: reg("Setpoint", celsius),
        2: reg("Mode", named(HISENSE_CONTROL_MODES)),
        3: reg("Fan speed", named(HISENSE_FANS)),
        4: reg("Louvres", named(HISENSE_CONTROL_SWINGS)),
      },
    },
    source:
      "As the gateway's driver (HvacHisense.cpp) has it: the unit is read from discrete input 0 and input registers 1-12, and set through coil 0 and holding registers 0-4. In auto it reads which way it is going (5, 6, 7); up/down and left/right trade codes between writing and reading. Whole °C, setpoint 18-32. No manual was at hand; the coil and holding registers reading back is a guess. It has no remote lock.",
  },
]

export function hvacType(id: number | undefined): HvacType | undefined {
  return id === undefined ? undefined : HVAC_TYPES.find((t) => t.id === id)
}

/** As the gateway's HVAC list writes it: "8 - COP - CoolPlug via Modbus RTU". */
export function typeLabel(t: HvacType): string {
  if (t.id === 0) return "0 - none"
  const via = t.via.startsWith("Modbus") && !t.name.includes(" via ") ? ` via ${t.via}` : ""
  return `${t.id} - ${t.code} - ${t.name}${via}`
}

/** A point's table, from the one letter `hvac status` gives it. */
export const POINT_TABLES: Record<string, Table> = { c: "coils", d: "discrete", h: "holding", i: "input" }
