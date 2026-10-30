// OpenTherm frames, read for a person. Display only: the modem and the testbench pass
// frames through as 32-bit numbers, and nothing that acts on a frame reads this file.
//
// A frame (OpenTherm Protocol 2.2):
//
//   bit 31      parity: makes the number of 1 bits in the frame even
//   bits 30-28  message type
//   bits 27-24  spare, 0
//   bits 23-16  data id
//   bits 15-0   data value: HB (15-8) and LB (7-0)

export const MSG_TYPES = [
  "Read-Data",
  "Write-Data",
  "Invalid-Data",
  "Reserved",
  "Read-Ack",
  "Write-Ack",
  "Data-Invalid",
  "Unknown-DataId",
] as const

export type MsgType = (typeof MSG_TYPES)[number]

/** Sent by the master (thermostat) for types 0-3, by the slave (boiler) for 4-7. */
export function isFromMaster(msgType: number): boolean {
  return msgType < 4
}

/** How a data id's 16-bit value is laid out. */
type ValueType =
  | "f88"        // signed fixed point, 8.8
  | "u16"
  | "s16"
  | "u8u8"       // two unsigned bytes
  | "s8s8"       // two signed bytes
  | "flag8u8"    // HB flags, LB a number
  | "flag8flag8" // flags in both bytes
  | "status"     // id 0: master flags in HB, slave flags in LB

interface DataIdInfo {
  name: string
  type: ValueType
  unit?: string
}

export const DATA_IDS: Record<number, DataIdInfo> = {
  0: { name: "Status", type: "status" },
  1: { name: "Control setpoint", type: "f88", unit: "°C" },
  2: { name: "Master config", type: "flag8u8" },
  3: { name: "Slave config", type: "flag8u8" },
  4: { name: "Command", type: "u8u8" },
  5: { name: "Fault flags / OEM code", type: "flag8u8" },
  6: { name: "Remote parameter flags", type: "flag8flag8" },
  7: { name: "Cooling control", type: "f88", unit: "%" },
  8: { name: "Control setpoint CH2", type: "f88", unit: "°C" },
  9: { name: "Remote override room setpoint", type: "f88", unit: "°C" },
  10: { name: "TSP count", type: "u8u8" },
  11: { name: "TSP index / value", type: "u8u8" },
  12: { name: "Fault buffer size", type: "u8u8" },
  13: { name: "Fault buffer entry", type: "u8u8" },
  14: { name: "Max rel. modulation", type: "f88", unit: "%" },
  15: { name: "Max capacity / min modulation", type: "u8u8" },
  16: { name: "Room setpoint", type: "f88", unit: "°C" },
  17: { name: "Rel. modulation level", type: "f88", unit: "%" },
  18: { name: "CH water pressure", type: "f88", unit: "bar" },
  19: { name: "DHW flow rate", type: "f88", unit: "l/min" },
  20: { name: "Day / time", type: "u8u8" },
  21: { name: "Date", type: "u8u8" },
  22: { name: "Year", type: "u16" },
  23: { name: "Room setpoint CH2", type: "f88", unit: "°C" },
  24: { name: "Room temperature", type: "f88", unit: "°C" },
  25: { name: "Boiler water temp.", type: "f88", unit: "°C" },
  26: { name: "DHW temperature", type: "f88", unit: "°C" },
  27: { name: "Outside temperature", type: "f88", unit: "°C" },
  28: { name: "Return water temp.", type: "f88", unit: "°C" },
  29: { name: "Solar storage temp.", type: "f88", unit: "°C" },
  30: { name: "Solar collector temp.", type: "f88", unit: "°C" },
  31: { name: "Flow temperature CH2", type: "f88", unit: "°C" },
  32: { name: "DHW2 temperature", type: "f88", unit: "°C" },
  33: { name: "Exhaust temperature", type: "s16", unit: "°C" },
  48: { name: "DHW setpoint bounds", type: "s8s8", unit: "°C" },
  49: { name: "Max CH setpoint bounds", type: "s8s8", unit: "°C" },
  50: { name: "OTC heat curve bounds", type: "s8s8" },
  56: { name: "DHW setpoint", type: "f88", unit: "°C" },
  57: { name: "Max CH water setpoint", type: "f88", unit: "°C" },
  58: { name: "OTC heat curve ratio", type: "f88" },
  100: { name: "Remote override function", type: "flag8u8" },
  115: { name: "OEM diagnostic code", type: "u16" },
  116: { name: "Burner starts", type: "u16" },
  117: { name: "CH pump starts", type: "u16" },
  118: { name: "DHW pump/valve starts", type: "u16" },
  119: { name: "DHW burner starts", type: "u16" },
  120: { name: "Burner hours", type: "u16", unit: "h" },
  121: { name: "CH pump hours", type: "u16", unit: "h" },
  122: { name: "DHW pump/valve hours", type: "u16", unit: "h" },
  123: { name: "DHW burner hours", type: "u16", unit: "h" },
  124: { name: "OpenTherm version master", type: "f88" },
  125: { name: "OpenTherm version slave", type: "f88" },
  126: { name: "Master product version", type: "u8u8" },
  127: { name: "Slave product version", type: "u8u8" },
}

const MASTER_STATUS = ["CH", "DHW", "Cooling", "OTC", "CH2"]
const SLAVE_STATUS = ["Fault", "CH", "DHW", "Flame", "Cooling", "CH2", "Diagnostic"]

export interface DecodedFrame {
  raw: number
  hex: string
  parityOk: boolean
  msgType: number
  msgTypeName: MsgType
  fromMaster: boolean
  dataId: number
  dataIdName: string
  value: number
  /** The value as its data id defines it, or as two bytes when the id is unknown. */
  valueText: string
}

function popcount(n: number): number {
  let c = 0
  for (let v = n >>> 0; v; v >>>= 1) c += v & 1
  return c
}

const s8 = (b: number) => (b & 0x80 ? b - 0x100 : b)
const s16 = (w: number) => (w & 0x8000 ? w - 0x10000 : w)
const bits8 = (b: number) => b.toString(2).padStart(8, "0")

function flags(byte: number, names: string[]): string {
  const on = names.filter((_, i) => byte & (1 << i))
  return on.length ? on.join(", ") : "none"
}

function formatValue(info: DataIdInfo | undefined, value: number, fromMaster: boolean): string {
  const hb = value >> 8
  const lb = value & 0xff
  const unit = info?.unit ? ` ${info.unit}` : ""
  switch (info?.type) {
    case "f88": {
      const v = s16(value) / 256
      return `${Number.isInteger(v) ? v.toFixed(0) : v.toFixed(2)}${unit}`
    }
    case "u16":
      return `${value}${unit}`
    case "s16":
      return `${s16(value)}${unit}`
    case "s8s8":
      return `${s8(hb)} / ${s8(lb)}${unit}`
    case "flag8u8":
      return `${bits8(hb)} / ${lb}`
    case "flag8flag8":
      return `${bits8(hb)} / ${bits8(lb)}`
    case "status":
      // The master's request carries its own flags; the slave's reply echoes them and
      // adds its own, which is the half a person is looking for.
      return fromMaster
        ? `master: ${flags(hb, MASTER_STATUS)}`
        : `master: ${flags(hb, MASTER_STATUS)} | slave: ${flags(lb, SLAVE_STATUS)}`
    case "u8u8":
    default:
      return `${hb} / ${lb}`
  }
}

export function decodeFrame(raw: number): DecodedFrame {
  const frame = raw >>> 0
  const msgType = (frame >>> 28) & 0x7
  const dataId = (frame >>> 16) & 0xff
  const value = frame & 0xffff
  const info = DATA_IDS[dataId]
  const fromMaster = isFromMaster(msgType)
  return {
    raw: frame,
    hex: "0x" + frame.toString(16).toUpperCase().padStart(8, "0"),
    parityOk: popcount(frame) % 2 === 0,
    msgType,
    msgTypeName: MSG_TYPES[msgType],
    fromMaster,
    dataId,
    dataIdName: info?.name ?? `Data id ${dataId}`,
    value,
    valueText: formatValue(info, value, fromMaster),
  }
}

/** A frame from its parts, with the parity bit set so it is valid on the line. */
export function encodeFrame(msgType: number, dataId: number, value: number): number {
  const body =
    (((msgType & 0x7) << 28) | ((dataId & 0xff) << 16) | (value & 0xffff)) >>> 0
  return popcount(body) % 2 === 0 ? body : (body | 0x80000000) >>> 0
}
