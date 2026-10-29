// Modbus RTU frames on the RS485 bus, read for a person. Display only: the testbench
// passes frames on as hex, and nothing that acts on a frame reads this file.
//
// A frame (an ADU):
//
//   unit (1) | function code (1) | data (0-252) | CRC-16 (2, low byte first)
//
// The registers are named from the CoolPlug's map (CoolPlug PRM rev 0.3, section 2.2),
// by their address ON THE WIRE: the PRM's table numbers minus one, which is also what
// a gateway's configuration uses.

export const FUNCTION_NAMES: Record<number, string> = {
  0x01: "Read coils",
  0x02: "Read discrete inputs",
  0x03: "Read holding registers",
  0x04: "Read input registers",
  0x05: "Write single coil",
  0x06: "Write single register",
  0x0f: "Write multiple coils",
  0x10: "Write multiple registers",
  0x17: "Read/write multiple registers",
  0x2b: "Read device identification",
}

export const EXCEPTION_NAMES: Record<number, string> = {
  0x01: "Illegal function",
  0x02: "Illegal data address",
  0x03: "Illegal data value",
  0x04: "Slave device failure",
  0x05: "Acknowledge",
  0x06: "Slave device busy",
  0x08: "Memory parity error",
  0x0a: "Gateway path unavailable",
  0x0b: "Gateway target failed",
}

// ── The CoolPlug's encodings ─────────────────────────────────────

export const MODES = ["Cool", "Heat", "Auto", "Dry", "Heat aux", "Fan"] as const
export const FAN_SPEEDS = ["Low", "Medium", "High", "Auto", "Top", "Very low"] as const
export const SWINGS = ["Vertical", "30°", "45°", "60°", "Horizontal", "Auto", "Off", "No louvres"] as const
export const DEMANDS = ["No demand", "Heating", "Cooling"] as const
export const LINE_ROLES = ["Slave", "Master", "Auto"] as const

/** How a register's 16 bits read. */
type Format =
  | "number"
  | "tenths"     // °C x10, signed
  | "onOff"
  | "yesNo"
  | "mode"
  | "fan"
  | "swing"
  | "demand"
  | "role"
  | "lineState" // bit fields
  | "compact"   // on/off, mode, fan, louvre, filter, therm on in one
  | "firmware"  // major * 100 + minor * 10 + subminor
  | "hex"
  | "reset"

interface RegisterInfo {
  name: string
  format: Format
}

export const COOLPLUG_REGISTERS: Record<number, RegisterInfo> = {
  1: { name: "Reset", format: "reset" },
  29: { name: "VAUT changeover threshold", format: "tenths" },
  30: { name: "VAUT as auto mode", format: "yesNo" },
  31: { name: "HVAC type index", format: "number" },
  32: { name: "Firmware version", format: "firmware" },
  33: { name: "Serial number, low word", format: "hex" },
  34: { name: "Serial number, high word", format: "hex" },
  35: { name: "Modbus address", format: "number" },
  36: { name: "Master/slave on the HVAC line", format: "role" },
  37: { name: "Indoor centralised address", format: "number" },
  95: { name: "Line state", format: "lineState" },
  96: { name: "Compact indoor status", format: "compact" },
  97: { name: "Setpoint", format: "tenths" },
  98: { name: "Room temperature", format: "tenths" },
  99: { name: "Failure code", format: "number" },
  100: { name: "Own room temperature", format: "tenths" },
  143: { name: "Line state", format: "lineState" },
  144: { name: "On/off", format: "onOff" },
  145: { name: "Mode", format: "mode" },
  146: { name: "Fan speed", format: "fan" },
  147: { name: "Louvres", format: "swing" },
  148: { name: "Filter sign", format: "yesNo" },
  149: { name: "Setpoint", format: "tenths" },
  150: { name: "Room temperature", format: "tenths" },
  151: { name: "Failure code", format: "number" },
  152: { name: "Own room temperature", format: "tenths" },
  153: { name: "Operation status", format: "demand" },
}

export function registerName(address: number): string {
  return COOLPLUG_REGISTERS[address]?.name ?? `Register ${address}`
}

function named(names: readonly string[], value: number): string {
  return names[value] ?? `${value}?`
}

/** A register's value as a CoolPlug means it. */
export function formatRegister(address: number, value: number): string {
  const info = COOLPLUG_REGISTERS[address]
  switch (info?.format) {
    case "tenths": {
      const signed = value >= 0x8000 ? value - 0x10000 : value
      return `${(signed / 10).toFixed(1)} °C`
    }
    case "onOff":
      return value === 0 ? "Off" : "On"
    case "yesNo":
      return value === 0 ? "No" : "Yes"
    case "mode":
      return named(MODES, value)
    case "fan":
      return named(FAN_SPEEDS, value)
    case "swing":
      return named(SWINGS, value)
    case "demand":
      return named(DEMANDS, value)
    case "role":
      return named(LINE_ROLES, value)
    case "lineState": {
      const parts = [value & 0x01 ? "indoor connected" : "indoor NOT connected"]
      if (value & 0x04) parts.push("failure")
      if (value & 0x08) parts.push("boot mode")
      if (value & 0x10) parts.push("not configured")
      return parts.join(", ")
    }
    case "compact": {
      const parts = [
        value & 0x01 ? "On" : "Off",
        named(MODES, (value >> 1) & 0x1f),
        `fan ${named(FAN_SPEEDS, (value >> 6) & 0x0f)}`,
        `louvres ${named(SWINGS, (value >> 10) & 0x07)}`,
      ]
      if (value & 0x2000) parts.push("filter")
      if (value & 0x4000) parts.push("therm on")
      return parts.join(", ")
    }
    case "firmware":
      return `${Math.floor(value / 100)}.${Math.floor(value / 10) % 10}.${value % 10}`
    case "hex":
      return "0x" + value.toString(16).toUpperCase().padStart(4, "0")
    case "reset":
      return value === 1 ? "Enter boot mode" : value === 2 ? "Restart" : String(value)
    default:
      return String(value)
  }
}

// ── Bytes ────────────────────────────────────────────────────────

/** Modbus' CRC-16: polynomial 0xA001, from 0xFFFF. On the wire low byte first. */
export function crc16(bytes: readonly number[]): number {
  let crc = 0xffff
  for (const b of bytes) {
    crc ^= b
    for (let i = 0; i < 8; i++) crc = crc & 1 ? (crc >>> 1) ^ 0xa001 : crc >>> 1
  }
  return crc
}

/** The bytes with their CRC after them. */
export function withCrc(bytes: readonly number[]): number[] {
  const crc = crc16(bytes)
  return [...bytes, crc & 0xff, crc >>> 8]
}

/** Hex bytes, two digits each, spaces between them allowed; null when it is not that. */
export function hexToBytes(text: string): number[] | null {
  const t = text.replace(/[\s:-]/g, "")
  if (t.length === 0 || t.length % 2 !== 0 || !/^[0-9a-f]+$/i.test(t)) return null
  const out: number[] = []
  for (let i = 0; i < t.length; i += 2) out.push(parseInt(t.slice(i, i + 2), 16))
  return out
}

export function bytesToHex(bytes: readonly number[]): string {
  return bytes.map((b) => b.toString(16).toUpperCase().padStart(2, "0")).join(" ")
}

const word = (b: readonly number[], i: number) => ((b[i] ?? 0) << 8) | (b[i + 1] ?? 0)

// ── Frames ───────────────────────────────────────────────────────

/** Which registers a read asked for, so its reply can be read. */
export interface ReadRequest {
  start: number
  count: number
}

export interface DecodedFrame {
  unit: number
  /** The function code without the exception bit. */
  fc: number
  functionName: string
  /** Set on an exception reply. */
  exception?: string
  /** The first register it is about, and how many; absent when it is not about any. */
  start?: number
  count?: number
  /** "145 Mode", or "144-147" for a range. */
  registerText: string
  /** What it says: the values written or read, as a CoolPlug means them. */
  valueText: string
  /** A read request, for its reply to be read with. */
  read?: ReadRequest
  /** Only part of the frame was recorded, so its CRC cannot be checked. */
  truncated: boolean
  crcOk: boolean
}

function registerText(start: number, count: number): string {
  return count === 1 ? `${start} ${registerName(start)}` : `${start}-${start + count - 1}`
}

function valuesText(start: number, values: readonly number[]): string {
  if (values.length === 1) return formatRegister(start, values[0])
  return values.map((v, i) => `${registerName(start + i)}: ${formatRegister(start + i, v)}`).join(" | ")
}

/**
 * Read one frame. `len` is its whole length as the testbench recorded it, which can be
 * more than the bytes it kept. A reply to a read says nothing about which registers it
 * holds, so `request` is the read it answers, when known.
 */
export function decodeFrame(
  bytes: readonly number[],
  kind: "request" | "reply",
  len: number,
  request?: ReadRequest,
): DecodedFrame {
  const truncated = bytes.length < len
  const n = bytes.length
  const crcOk = !truncated && n >= 4 && (bytes[n - 2] | (bytes[n - 1] << 8)) === crc16(bytes.slice(0, -2))
  const unit = bytes[0] ?? 0
  const raw = bytes[1] ?? 0
  const fc = raw & 0x7f
  const base: DecodedFrame = {
    unit,
    fc,
    functionName: FUNCTION_NAMES[fc] ?? `Function 0x${fc.toString(16).padStart(2, "0")}`,
    registerText: "",
    valueText: "",
    truncated,
    crcOk,
  }

  if (kind === "reply" && raw & 0x80) {
    const code = bytes[2] ?? 0
    return { ...base, exception: EXCEPTION_NAMES[code] ?? `Exception ${code}` }
  }

  const data = bytes.slice(2, truncated ? bytes.length : -2)
  switch (fc) {
    case 0x03:
    case 0x04: {
      if (kind === "request") {
        const start = word(data, 0)
        const count = word(data, 2)
        return { ...base, start, count, registerText: registerText(start, count), valueText: "", read: { start, count } }
      }
      const values: number[] = []
      for (let i = 1; i + 1 < data.length; i += 2) values.push(word(data, i))
      if (!request) return { ...base, valueText: values.join(", ") }
      return {
        ...base,
        start: request.start,
        count: values.length,
        registerText: registerText(request.start, values.length),
        valueText: valuesText(request.start, values),
      }
    }
    case 0x06: {
      const start = word(data, 0)
      return { ...base, start, count: 1, registerText: registerText(start, 1), valueText: formatRegister(start, word(data, 2)) }
    }
    case 0x10: {
      const start = word(data, 0)
      const count = word(data, 2)
      if (kind === "reply") return { ...base, start, count, registerText: registerText(start, count) }
      const values: number[] = []
      for (let i = 5; i + 1 < data.length && values.length < count; i += 2) values.push(word(data, i))
      return { ...base, start, count, registerText: registerText(start, count), valueText: valuesText(start, values) }
    }
    case 0x2b: {
      if (kind === "request") return { ...base, valueText: `code ${data[1] ?? "?"}, from object ${data[2] ?? "?"}` }
      // MEI, code, conformity, more, next, count, then id, length, text per object.
      const texts: string[] = []
      for (let i = 6, n = 0; n < (data[5] ?? 0) && i + 1 < data.length; n++) {
        const length = data[i + 1]
        texts.push(String.fromCharCode(...data.slice(i + 2, i + 2 + length)))
        i += 2 + length
      }
      return { ...base, valueText: texts.join(" | ") }
    }
    default:
      return base
  }
}
