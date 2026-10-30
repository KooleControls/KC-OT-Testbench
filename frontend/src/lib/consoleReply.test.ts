import { describe, expect, it } from "vitest"

import { ConsoleReplyReader } from "./consoleReply"

// The same discipline as reply.test.ts, and for the same reason: this is the one
// place where a mistake would make the answer depend on the transport's buffer
// size, so it is the one place worth feeding at every fragmentation.

const bytes = (s: string) => new TextEncoder().encode(s)

/** Feed `text` in chunks of `size`, which is what the transport does to it. */
function read(text: string, size: number) {
  const reader = new ConsoleReplyReader()
  const all = bytes(text)
  const emitted: string[] = []
  for (let at = 0; at < all.length; at += size)
    emitted.push(...reader.push(all.subarray(at, Math.min(at + size, all.length))))
  return { emitted, reply: reader.end() }
}

describe("ConsoleReplyReader", () => {
  it("reads a single record", () => {
    const { reply } = read("ok: true\npong: true", 4096)
    expect(reply.records).toEqual(["ok: true\npong: true"])
    expect(reply.body).toBeNull()
  })

  it("does not mistake a record's own newlines for a separator", () => {
    // The whole reason this codec has a separator of its own.
    const { reply } = read("partitions:\n  - label: nvs\n    size: 1", 4096)
    expect(reply.records).toHaveLength(1)
  })

  it("splits records on the separator", () => {
    const { emitted, reply } = read("p: 32768\n---\np: 65536\n---\nok: true", 4096)
    expect(emitted).toEqual(["p: 32768", "p: 65536"])
    expect(reply.records).toEqual(["p: 32768", "p: 65536", "ok: true"])
  })

  it("gives the same answer at every chunk size", () => {
    const text = "p: 32768\n---\np: 65536\n---\nok: true\nsize: 65536"
    for (const size of [1, 2, 3, 5, 7, 13, 1024]) {
      const { reply } = read(text, size)
      expect(reply.records, `chunked by ${size}`).toEqual([
        "p: 32768",
        "p: 65536",
        "ok: true\nsize: 65536",
      ])
    }
  })

  it("takes everything after a declared body's separator as bytes", () => {
    const { reply } = read(
      'ok: true\nstatus: 200\ncontentType: text/html\n---\n<!doctype html>',
      4096,
    )
    expect(reply.records).toEqual(["ok: true\nstatus: 200\ncontentType: text/html"])
    expect(reply.contentType).toBe("text/html")
    expect(new TextDecoder().decode(reply.body!)).toBe("<!doctype html>")
  })

  it("reads a declared body the same way however it was chunked", () => {
    const text = "ok: true\ncontentType: application/octet-stream\n---\nabcdefghij"
    for (const size of [1, 3, 8, 1024]) {
      const { reply } = read(text, size)
      expect(new TextDecoder().decode(reply.body!), `chunked by ${size}`).toBe(
        "abcdefghij",
      )
    }
  })

  it("carries a quoted content type", () => {
    const { reply } = read('ok: true\ncontentType: "text/plain"\n---\nhi', 4096)
    expect(reply.contentType).toBe("text/plain")
  })

  it("reports a declared content encoding", () => {
    const { reply } = read(
      "ok: true\ncontentEncoding: gzip\ncontentType: text/html\n---\nx",
      4096,
    )
    expect(reply.contentEncoding).toBe("gzip")
  })

  it("ignores a contentType that is not a record's own field", () => {
    // Nested, so it is a field OF something rather than a declaration about the
    // reply — and a reply whose body was decided by a nested key would be at the
    // mercy of whatever a handler happened to call a field.
    const { reply } = read("ok: true\nfile:\n  contentType: text/html", 4096)
    expect(reply.contentType).toBeNull()
    expect(reply.body).toBeNull()
  })

  it("survives a reply that ends with its separator", () => {
    const { reply } = read("ok: true\nstatus: 404\n---\n", 4096)
    expect(reply.records).toEqual(["ok: true\nstatus: 404"])
  })
})
