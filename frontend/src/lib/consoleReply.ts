/** Reading a CONSOLE-codec reply, independently of how the transport chopped it up.
 *
 *  The console codec answers in YAML, so its records are nothing but newlines and
 *  the JSON codec's '\n' separator cannot divide them. Its separator is a line
 *  reading `---`, which is unambiguous for the same reason: every line the device
 *  writes inside a record is either indented or ends in ':', and a block scalar's
 *  lines are indented too, so a bare `---` can only be the divider.
 *
 *  See main/lib/protocol/YamlReplyWriter.h — this is the reader for the convention
 *  that file writes down, and it is the sibling of lib/reply.ts, which reads the
 *  other codec's. The two exist separately because a record separator belongs to a
 *  CODEC and not to the protocol: that is the thing the second codec taught us.
 */

export interface ConsoleReply {
  /** Every record, in order. Text as the device wrote it. */
  records: string[]
  /** The declared body, when the first record declared one. */
  body: Uint8Array | null
  /** The media type it declared, when there is a body. */
  contentType: string | null
  /** The content encoding it declared, when it declared one. */
  contentEncoding: string | null
}

const SEPARATOR = "\n---\n"

export class ConsoleReplyReader {
  private text = ""
  private pending = new Uint8Array(0)
  private records: string[] = []
  private bodyParts: Uint8Array[] = []
  private bodyLength = 0
  private inBody = false
  private contentType: string | null = null
  private contentEncoding: string | null = null

  /** How many BODY bytes have arrived, for the same progress the other reader
   *  reports. */
  get received(): number {
    return this.bodyLength
  }

  /** True once the first record declared a body. */
  get hasBody(): boolean {
    return this.inBody
  }

  /** Feed one transport chunk. Returns the records COMPLETED by it — which may be
   *  none, one, or several, and has nothing to do with the chunk's size. */
  push(chunk: Uint8Array): string[] {
    if (this.inBody) {
      this.bodyParts.push(chunk)
      this.bodyLength += chunk.length
      return []
    }

    // A separator may straddle any number of chunks, so the tail is carried over
    // as BYTES rather than as text: decoding a chunk that ends mid-character
    // would turn a split UTF-8 sequence into a replacement character.
    const buf = new Uint8Array(this.pending.length + chunk.length)
    buf.set(this.pending, 0)
    buf.set(chunk, this.pending.length)

    this.text += new TextDecoder().decode(buf)
    this.pending = new Uint8Array(0)

    const emitted: string[] = []
    let at: number
    while ((at = this.text.indexOf(SEPARATOR)) >= 0) {
      const record = this.text.slice(0, at)
      this.text = this.text.slice(at + SEPARATOR.length)
      this.records.push(record)
      emitted.push(record)

      if (this.records.length === 1) {
        // Only the FIRST record may declare a body, exactly as in the other
        // codec: a body belongs to the reply, not to a field of it.
        this.contentType = declared(record, "contentType")
        this.contentEncoding = declared(record, "contentEncoding")
        if (this.contentType !== null) {
          this.inBody = true
          const rest = new TextEncoder().encode(this.text)
          this.text = ""
          if (rest.length) {
            this.bodyParts.push(rest)
            this.bodyLength += rest.length
          }
          return emitted
        }
      }
    }

    return emitted
  }

  /** No more chunks. Whatever is left over is the final record. */
  end(): ConsoleReply {
    if (!this.inBody && this.text.length > 0) {
      this.records.push(this.text)
      this.text = ""
    }
    return {
      records: this.records,
      body: this.inBody ? concat(this.bodyParts, this.bodyLength) : null,
      contentType: this.contentType,
      contentEncoding: this.contentEncoding,
    }
  }
}

/** A top-level scalar field of a YAML record, or null. Top level only: a nested
 *  `contentType` is a field of something, not a declaration about the reply. */
function declared(record: string, key: string): string | null {
  for (const line of record.split("\n")) {
    if (!line.startsWith(`${key}: `)) continue
    const value = line.slice(key.length + 2).trim()
    return value.startsWith('"') ? value.slice(1, -1) : value
  }
  return null
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
