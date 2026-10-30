import { describe, it, expect } from "vitest"
import { ReplyReader, type ReplyResult } from "./reply"

const enc = new TextEncoder()

/** Feed `bytes` through a reader in pieces of `size`, and return everything the
 *  reader produced. `size` stands in for a Connection's buffer: the whole point
 *  of the convention is that NONE of these runs may differ. */
function readInChunks(bytes: Uint8Array, size: number): { streamed: string[]; end: ReplyResult } {
  const reader = new ReplyReader()
  const streamed: string[] = []
  for (let i = 0; i < bytes.length; i += size)
    streamed.push(...reader.push(bytes.subarray(i, Math.min(i + size, bytes.length))))
  return { streamed, end: reader.end() }
}

// Deliberately awkward: 1 splits every byte, 3 lands mid-record, and the large
// ones deliver whole replies at once. A reply reader that reads chunk
// boundaries as structure fails at some of these and passes at others, which is
// exactly how the old one looked correct.
const SIZES = [1, 2, 3, 5, 7, 13, 64, 512, 4096, 1_000_000]

describe("an ordinary single-record reply", () => {
  const reply = enc.encode('{"ok":true,"free":83404}')

  it.each(SIZES)("is one result and no records at chunk size %i", (size) => {
    const { streamed, end } = readInChunks(reply, size)
    expect(streamed).toEqual([])
    expect(end.records).toEqual([])
    expect(JSON.parse(end.result)).toEqual({ ok: true, free: 83404 })
    expect(end.body).toBeNull()
  })
})

describe("a large single-record reply", () => {
  // Longer than any reply window in the tree, so the transport MUST split it.
  // This is the case the old reader got wrong when onMessage was set: it parsed
  // each fragment as a record and rejected the reply.
  const big = { ok: true, lines: Array.from({ length: 400 }, (_, i) => `line ${i}`) }
  const reply = enc.encode(JSON.stringify(big))

  it.each(SIZES)("yields no spurious records at chunk size %i", (size) => {
    const { streamed, end } = readInChunks(reply, size)
    expect(streamed).toEqual([])
    expect(end.records).toEqual([])
    expect(JSON.parse(end.result)).toEqual(big)
  })

  it("is larger than the reply window it would be split by", () => {
    expect(reply.length).toBeGreaterThan(4096)
  })
})

describe("progress records followed by a result", () => {
  const reply = enc.encode('{"p":32768}\n{"p":65536}\n{"ok":true,"size":65536}')

  it.each(SIZES)("delivers both records and the result at chunk size %i", (size) => {
    const { streamed, end } = readInChunks(reply, size)
    expect(streamed).toEqual(['{"p":32768}', '{"p":65536}'])
    expect(end.records).toEqual(['{"p":32768}', '{"p":65536}'])
    expect(JSON.parse(end.result)).toEqual({ ok: true, size: 65536 })
    expect(end.body).toBeNull()
  })
})

describe("a progress record larger than the chunks carrying it", () => {
  // The case that cannot happen today only because progress records are ~15
  // bytes. Nothing promises that, so the reader must not depend on it.
  const fat = JSON.stringify({ p: 1, note: "x".repeat(9000) })
  const reply = enc.encode(`${fat}\n{"ok":true}`)

  it.each(SIZES)("is still one record at chunk size %i", (size) => {
    const { streamed, end } = readInChunks(reply, size)
    expect(streamed).toEqual([fat])
    expect(JSON.parse(end.result)).toEqual({ ok: true })
  })
})

describe("a header declaring a body", () => {
  const header = '{"ok":true,"size":6,"contentType":"application/octet-stream"}'
  const body = Uint8Array.from([0, 1, 2, 250, 251, 255])
  const reply = new Uint8Array([...enc.encode(header + "\n"), ...body])

  it.each(SIZES)("splits header from body at chunk size %i", (size) => {
    const { end } = readInChunks(reply, size)
    expect(end.header).toEqual({
      ok: true,
      size: 6,
      contentType: "application/octet-stream",
    })
    expect(Array.from(end.body ?? [])).toEqual(Array.from(body))
  })

  it.each(SIZES)("never treats body bytes as records at chunk size %i", (size) => {
    // The body here contains a newline (0x0a is not present above, so add one).
    const withNewline = Uint8Array.from([1, 0x0a, 2, 0x0a, 3])
    const r = new Uint8Array([...enc.encode(header + "\n"), ...withNewline])
    const { streamed, end } = readInChunks(r, size)
    expect(streamed).toEqual([header])          // the header, and nothing else
    expect(end.records).toEqual([])
    expect(Array.from(end.body ?? [])).toEqual(Array.from(withNewline))
  })
})

describe("a header with no body, ending in its separator", () => {
  // `web read`'s 404: one record, a newline, nothing after. The trailing empty
  // piece is not a record, and the result is the record that WAS completed.
  const reply = enc.encode('{"ok":true,"status":404}\n')

  it.each(SIZES)("reports the record as the result at chunk size %i", (size) => {
    const { end } = readInChunks(reply, size)
    expect(JSON.parse(end.result)).toEqual({ ok: true, status: 404 })
    expect(end.records).toEqual([])
    expect(end.body).toBeNull()
  })
})

describe("a refusal that would once have been sniffed", () => {
  // `partition read` of an unknown label. It has no contentType, so it is an
  // ordinary record -- no length heuristic, no startsWith('{"ok":false').
  const reply = enc.encode('{"ok":false,"error":"unknown partition"}')

  it.each(SIZES)("is an ordinary record at chunk size %i", (size) => {
    const { end } = readInChunks(reply, size)
    expect(end.body).toBeNull()
    expect(end.header).toBeNull()
    expect(JSON.parse(end.result)).toEqual({ ok: false, error: "unknown partition" })
  })
})

describe("chunk size is not observable", () => {
  it("gives byte-identical answers for every reply at every size", () => {
    const replies = [
      '{"ok":true}',
      '{"p":1}\n{"p":2}\n{"ok":true}',
      '{"ok":true,"status":404}\n',
      `{"ok":true,"contentType":"text/plain"}\n${"body\nwith\nnewlines"}`,
      JSON.stringify({ ok: true, blob: "z".repeat(20000) }),
    ]

    for (const text of replies) {
      const bytes = enc.encode(text)
      const reference = JSON.stringify(readInChunks(bytes, 1_000_000))
      for (const size of SIZES) {
        expect(JSON.stringify(readInChunks(bytes, size))).toEqual(reference)
      }
    }
  })
})
