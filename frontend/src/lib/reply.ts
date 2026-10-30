/** Reading a device reply, independently of how the transport chopped it up.
 *
 *  A reply is a sequence of JSON records separated by '\n'; the last needs no
 *  separator because FLAG_FINAL ends the reply. If the FIRST record carries a
 *  `contentType`, it is the only record and everything after its newline is an
 *  opaque body of that media type. See main/lib/protocol/ReplyBody.h — this is
 *  the reader for the convention that file writes down.
 *
 *  WHY THIS IS A SEPARATE, PURE MODULE: it is the one place where a mistake
 *  would make the answer depend on the transport's buffer size, so it is the one
 *  place worth testing at every fragmentation. It takes bytes and returns
 *  records; it knows nothing about sockets, sessions or commands.
 *
 *  What it replaces was the assumption that one transport chunk is one record.
 *  That happened to hold while every progress record was ~15 bytes and the reply
 *  window was 512 or 4096 — and it silently stopped being a property of anything
 *  the moment either number moved, or a relay re-framed in the middle. */

export interface ReplyResult {
  /** Records completed before the last one, in order. Progress and events. */
  records: string[]
  /** The record carrying the result. Empty only for a reply that was nothing
   *  but a header and a body. */
  result: string
  /** The first record, parsed, when it declared a body. Null otherwise. */
  header: Record<string, unknown> | null
  /** The declared body, or null when the reply declared none. */
  body: Uint8Array | null
}

const NEWLINE = 0x0a

export class ReplyReader {
  private pending = new Uint8Array(0)
  private complete: string[] = []
  private header: Record<string, unknown> | null = null
  private bodyParts: Uint8Array[] = []
  private bodyLength = 0
  private inBody = false
  private sawFirstRecord = false

  /** How many BODY bytes have arrived. Progress is measured against the body,
   *  not the reply, because the header is not part of what was asked for. */
  get received(): number {
    return this.bodyLength
  }

  /** True once the first record declared a contentType. */
  get hasBody(): boolean {
    return this.inBody
  }

  /** Feed one transport chunk. Returns the records COMPLETED by it — which may
   *  be none, one, or several, and has nothing to do with the chunk's size. */
  push(chunk: Uint8Array): string[] {
    if (this.inBody) {
      this.bodyParts.push(chunk)
      this.bodyLength += chunk.length
      return []
    }

    // A record may straddle any number of chunks, so the tail is carried over
    // rather than parsed. This concatenation is the whole cost of being
    // chunk-independent, and it is bounded by one record, not by the reply.
    const buf = new Uint8Array(this.pending.length + chunk.length)
    buf.set(this.pending, 0)
    buf.set(chunk, this.pending.length)

    const emitted: string[] = []
    let start = 0

    for (let i = 0; i < buf.length; i++) {
      if (buf[i] !== NEWLINE) continue

      const text = new TextDecoder().decode(buf.subarray(start, i))
      start = i + 1

      if (!this.sawFirstRecord) {
        this.sawFirstRecord = true
        const parsed = tryParseObject(text)
        if (parsed && typeof parsed.contentType === "string" && parsed.contentType) {
          // Everything from here on is body, including the rest of this chunk.
          this.header = parsed
          this.inBody = true
          const rest = buf.subarray(start)
          if (rest.length) {
            this.bodyParts.push(rest)
            this.bodyLength += rest.length
          }
          this.pending = new Uint8Array(0)
          return [text]
        }
      }

      this.complete.push(text)
      emitted.push(text)
    }

    this.pending = buf.subarray(start)
    return emitted
  }

  /** No more chunks. Whatever is left over is the final record, or the body's
   *  tail when one was declared. */
  end(): ReplyResult {
    if (this.inBody) {
      return {
        records: this.complete,
        result: "",
        header: this.header,
        body: concat(this.bodyParts, this.bodyLength),
      }
    }

    const trailing = new TextDecoder().decode(this.pending)
    this.pending = new Uint8Array(0)

    // A reply that ends WITH its separator has no trailing text, and then the
    // result is the last record it did complete. `web read`'s 404 is exactly
    // this shape — one record, a newline, and nothing after it.
    if (trailing.length === 0) {
      const records = this.complete.slice()
      const result = records.pop() ?? ""
      return { records, result, header: null, body: null }
    }

    return { records: this.complete, result: trailing, header: null, body: null }
  }
}

function tryParseObject(text: string): Record<string, unknown> | null {
  try {
    const v = JSON.parse(text)
    return v && typeof v === "object" && !Array.isArray(v)
      ? (v as Record<string, unknown>)
      : null
  } catch {
    return null
  }
}

function concat(parts: Uint8Array[], total: number): Uint8Array {
  const out = new Uint8Array(total)
  let off = 0
  for (const p of parts) {
    out.set(p, off)
    off += p.length
  }
  return out
}
