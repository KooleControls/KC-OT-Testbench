// Modbus RTU frames on the RS485 bus, read for a person. Display only: the testbench
// passes frames on as hex, and nothing that acts on a frame reads this file.
//
// A frame (an ADU):
//
//   unit (1) | function code (1) | data (0-252) | CRC-16 (2, low byte first)
//
// What a register means depends on the device it is in, so the names come from a
// register map handed in (lib/hvac.ts has one per simulated HVAC type), by the address
// ON THE WIRE -- what a gateway's configuration uses too. Without one, a frame still
// reads, as plain numbers.

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

// ── Register maps ────────────────────────────────────────────────

/** Modbus' four tables: bits (coils, discrete inputs) and 16-bit registers. */
export type Table = "coils" | "discrete" | "holding" | "input"

/** How a table is called in a register's short name: "HR 145". */
export const TABLE_SHORT: Record<Table, string> = { coils: "Coil", discrete: "DI", holding: "HR", input: "IR" }

export interface RegisterInfo {
  name: string
  /** The value as the device means it. */
  format: (value: number) => string
}

/** One device's points, by table and wire address. */
export type RegisterMap = Partial<Record<Table, Record<number, RegisterInfo>>>

/** The table a function code reads or writes; undefined for one that reaches none. */
export function tableOf(fc: number): Table | undefined {
  switch (fc) {
    case 0x01:
    case 0x05:
    case 0x0f:
      return "coils"
    case 0x02:
      return "discrete"
    case 0x03:
    case 0x06:
    case 0x10:
    case 0x17:
      return "holding"
    case 0x04:
      return "input"
    default:
      return undefined
  }
}

export function registerName(map: RegisterMap | undefined, table: Table, address: number): string {
  return map?.[table]?.[address]?.name ?? `${TABLE_SHORT[table]} ${address}`
}

/** A point's value as the device means it, or as a number when the map has no word for it. */
export function formatPoint(map: RegisterMap | undefined, table: Table, address: number, value: number): string {
  const info = map?.[table]?.[address]
  return info ? info.format(value) : String(value)
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

/** `count` bits packed from `b[from]` on, the first in the low bit. */
function bits(b: readonly number[], from: number, count: number): number[] {
  const out: number[] = []
  for (let i = 0; i < count && from + (i >> 3) < b.length; i++) out.push((b[from + (i >> 3)] >> (i & 7)) & 1)
  return out
}

// ── Frames ───────────────────────────────────────────────────────

/** Which points a read asked for, so its reply can be read. */
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
  /** The first point it is about, and how many; absent when it is not about any. */
  start?: number
  count?: number
  /** "145 Mode", or "144-147" for a range. */
  registerText: string
  /** What it says: the values written or read, as the device means them. */
  valueText: string
  /** A read request, for its reply to be read with. */
  read?: ReadRequest
  /** Only part of the frame was recorded, so its CRC cannot be checked. */
  truncated: boolean
  crcOk: boolean
}

/**
 * Read one frame. `len` is its whole length as the testbench recorded it, which can be
 * more than the bytes it kept. A reply to a read says nothing about which points it
 * holds, so `request` is the read it answers, when known. `map` names the points of
 * the device at the frame's unit, when known.
 */
export function decodeFrame(
  bytes: readonly number[],
  kind: "request" | "reply",
  len: number,
  request?: ReadRequest,
  map?: RegisterMap,
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

  const table = tableOf(fc)
  const pointsText = (start: number, count: number) => {
    if (count !== 1) return `${start}-${start + count - 1}`
    const name = table && map?.[table]?.[start]?.name
    return name ? `${start} ${name}` : String(start)
  }
  const valuesText = (start: number, values: readonly number[]) => {
    if (!table) return values.join(", ")
    if (values.length === 1) return formatPoint(map, table, start, values[0])
    return values
      .map((v, i) => `${registerName(map, table, start + i)}: ${formatPoint(map, table, start + i, v)}`)
      .join(" | ")
  }

  const data = bytes.slice(2, truncated ? bytes.length : -2)
  switch (fc) {
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x04: {
      if (kind === "request") {
        const start = word(data, 0)
        const count = word(data, 2)
        return { ...base, start, count, registerText: pointsText(start, count), valueText: "", read: { start, count } }
      }
      const values: number[] = []
      if (fc <= 0x02) values.push(...bits(data, 1, request?.count ?? (data[0] ?? 0) * 8))
      else for (let i = 1; i + 1 < data.length; i += 2) values.push(word(data, i))
      if (!request) return { ...base, valueText: values.join(", ") }
      return {
        ...base,
        start: request.start,
        count: values.length,
        registerText: pointsText(request.start, values.length),
        valueText: valuesText(request.start, values),
      }
    }
    case 0x05: {
      // On is FF00, off 0000; anything else a device refuses.
      const start = word(data, 0)
      const value = word(data, 2)
      const text = value === 0xff00 ? valuesText(start, [1]) : value === 0 ? valuesText(start, [0]) : `0x${value.toString(16)}?`
      return { ...base, start, count: 1, registerText: pointsText(start, 1), valueText: text }
    }
    case 0x06: {
      const start = word(data, 0)
      return { ...base, start, count: 1, registerText: pointsText(start, 1), valueText: valuesText(start, [word(data, 2)]) }
    }
    case 0x0f:
    case 0x10: {
      const start = word(data, 0)
      const count = word(data, 2)
      if (kind === "reply") return { ...base, start, count, registerText: pointsText(start, count) }
      const values: number[] = []
      if (fc === 0x0f) values.push(...bits(data, 5, count))
      else for (let i = 5; i + 1 < data.length && values.length < count; i += 2) values.push(word(data, i))
      return { ...base, start, count, registerText: pointsText(start, count), valueText: valuesText(start, values) }
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
