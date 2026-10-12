// Singleton backend service — all communication over a single WebSocket.

import { DEV_HOST } from "@/config"
import type { Master, Slave } from "./routing"
import { ReplyReader } from "./reply"
import { ConsoleReplyReader, type ConsoleReply } from "./consoleReply"

const TOKEN_KEY = "device.token"

// ── Types ────────────────────────────────────────────────────────

type BroadcastHandler = (msg: Record<string, unknown>) => void

interface PendingRequest {
  resolve: (data: unknown) => void
  reject: (err: Error) => void
  timer: ReturnType<typeof setTimeout>
  timeoutMs: number
  // Turns transport chunks back into reply records. It, and not this file, is
  // what knows the reply format — and WHICH one it knows is the codec's business,
  // because a record separator belongs to a codec rather than to the protocol.
  // See lib/reply.ts (JSON) and lib/consoleReply.ts (console).
  reader: ReplyReader | ConsoleReplyReader
  // Called for each record completed before the last one. A record is delimited
  // by a newline in the BYTES, so this fires the same way whatever size the
  // transport framed in.
  onMessage?: (msg: Record<string, unknown>) => void
  // Called for each completed record as its TEXT, before onMessage parses it. A
  // caller that has no type for the reply — the command console — still wants a
  // record that is not JSON, which onMessage drops.
  onRecord?: (text: string) => void
  // The reply is in the CONSOLE codec, so its records are divided by `---` and
  // not by a newline, and it resolves with that reader's result.
  wantsConsole?: boolean
  // The caller expects a declared body (a header record with contentType, then
  // bytes). Resolves with { header, body } instead of the parsed record.
  wantsBody?: boolean
  // Cumulative BODY bytes, for progress.
  onData?: (received: number) => void
}

/** A reply that declared a body: the header record, and the bytes after it. */
export interface BodyReply<H = Record<string, unknown>> {
  header: H
  body: Uint8Array
}

/** Per-call options for `send`. A strict subset of what `awaitReply` takes: the
 *  binary-reply machinery stays private, because `send<T>` resolves with parsed
 *  JSON and a caller asking for bytes wants `readFile`, not a flag.
 *
 *  `M` is the progress record's shape. It defaults to `Record<string, unknown>`
 *  so `send<Result>(...)` is unchanged, and a caller who knows the shape can
 *  write `send<Result, Progress>(...)` instead of narrowing in the callback. */
export interface SendOptions<M = Record<string, unknown>> {
  /** Bounds SILENCE, not total transfer time - each chunk restarts it. Default
   *  10 s, which is short for anything that writes flash. */
  timeoutMs?: number

  /** Called for each reply RECORD completed before the last one.
   *
   *  A record is delimited by a newline in the bytes, so this fires the same way
   *  whatever size the transport framed in, and an ordinary reply -- which is a
   *  single record of any length -- produces none of them. Passing this is
   *  therefore safe for any command: at worst nothing arrives.
   *
   *  That was NOT true when this was first exposed. It used to hand over every
   *  non-final transport chunk, so a reply merely longer than the reply window
   *  was delivered as fragments and then failed to parse. See lib/reply.ts. */
  onMessage?: (msg: M) => void
}

/** Thrown when an upload was cancelled from the UI. A distinct type because a
 *  cancel is not a failure and must not be reported as one. */
export class UploadCancelled extends Error {
  constructor() {
    super("Upload cancelled")
    this.name = "UploadCancelled"
  }
}

/** What a login attempt came to. `error` is set only when the device gave a
 *  reason of its own - a rate limit today - so its absence means an ordinary
 *  wrong password. */
export type LoginOutcome =
  | { ok: true }
  | { ok: false; error?: string; retryAfter?: number }

export type ConnectionStatus = "connected" | "connecting" | "disconnected"
type StatusHandler = (status: ConnectionStatus) => void
type AuthHandler = (authenticated: boolean) => void

// ── Service ──────────────────────────────────────────────────────

// The browser is a PEER, not a client. It handshakes, owns half the channel-id
// space, opens channels from it, and accepts channels the device opens at it.
//
// The device is still single-in-flight, so opens are SERIALIZED through a FIFO
// queue (see `enqueue`): callers fire concurrently, one channel is active on the
// wire at a time, and the next starts on the previous reply's FINAL. That is
// policy, not protocol - the wire would carry several at once.
const FLAG_FINAL = 0x01
const FLAG_RESET = 0x02
const FLAG_OPEN = 0x04
const FLAG_CONTROL = 0x08

const PROTOCOL_VERSION = 1
const MAX_HANDSHAKE_ATTEMPTS = 3

// Two contiguous halves; the handshake decides which is ours. Nothing is
// reserved - channel 0 is an ordinary id, and CONTROL frames merely write it and
// have it ignored.
const LOW_BASE = 0x0000
const LOW_LIMIT = 0x8000
const HIGH_BASE = 0x8000
const HIGH_LIMIT = 0x10000

// The device UI runs in two places, and its WebSocket follows the page:
//   • served by the device            → ws://<device>/ws
//   • served through the relay server → ws://<server>/devices/<id>/ws
//
// Resolving "ws" against the page's own directory covers both, which keeps this
// file ignorant that a relay exists — no device id is parsed here. `pnpm dev`
// still talks to DEV_HOST.
function resolveWsUrl(): string {
  const proto = location.protocol === "https:" ? "wss:" : "ws:"
  if (import.meta.env.DEV) return `${proto}//${DEV_HOST}/ws`

  // Drop the document name so "/devices/x/" and "/devices/x/index.html" both
  // resolve to "/devices/x/ws".
  const dir = location.pathname.replace(/[^/]*$/, "")
  return `${proto}//${location.host}${dir}ws`
}

class BackendService {
  private ws: WebSocket | null = null
  private pending = new Map<number, PendingRequest>()
  private broadcastHandlers = new Set<BroadcastHandler>()
  private statusHandlers = new Set<StatusHandler>()
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private heartbeatTimer: ReturnType<typeof setInterval> | null = null
  private connecting: Promise<void> | null = null
  // Tail of the open-serialization queue. Each enqueued task runs only after the
  // previous one has fully settled (its reply's FINAL received, or it failed).
  private queue: Promise<unknown> = Promise.resolve()

  // ── Connection state (reset on every socket) ──────────────────────────────
  private ready = false
  private readyWaiters: Array<{ resolve: () => void; reject: (e: Error) => void }> = []
  private nonce = 0n
  private handshakeAttempts = 0
  private lowHalf = false
  private nextChannel = 0
  // Channels the DEVICE opened at us, by id, with the name from its envelope.
  private theirChannels = new Map<number, string>()
  private _status: ConnectionStatus = "disconnected"
  private token: string | null = sessionStorage.getItem(TOKEN_KEY)
  private authHandlers = new Set<AuthHandler>()
  private _authenticated = false
  private _authResolved = false

  get status(): ConnectionStatus {
    return this._status
  }

  get authenticated(): boolean {
    return this._authenticated
  }

  // True once the first handshake has settled either way (authed or
  // needs-login). Lets a late-subscribing hook skip its "checking" state
  // instead of waiting on the timeout fallback.
  get authResolved(): boolean {
    return this._authResolved
  }

  get hasToken(): boolean {
    return this.token !== null
  }

  onAuthChange(fn: AuthHandler): () => void {
    this.authHandlers.add(fn)
    return () => {
      this.authHandlers.delete(fn)
    }
  }

  private setAuthenticated(auth: boolean) {
    this._authenticated = auth
    this._authResolved = true
    this.authHandlers.forEach((fn) => fn(auth))
  }

  private setStatus(s: ConnectionStatus) {
    if (s !== this._status) {
      this._status = s
      this.statusHandlers.forEach((fn) => fn(s))
    }
  }

  onStatusChange(fn: StatusHandler): () => void {
    this.statusHandlers.add(fn)
    return () => {
      this.statusHandlers.delete(fn)
    }
  }

  connect() {
    this.ensureConnected().catch(() => {})
  }

  // "Connected" means the channel layer is up, not that the socket opened. A
  // command sent between those two points would be dropped by the device, which
  // refuses channel traffic before READY.
  private ensureConnected(): Promise<void> {
    if (this.ws?.readyState === WebSocket.OPEN && this.ready) return Promise.resolve()
    if (this.connecting) return this.connecting
    return this.doConnect()
  }

  private doConnect(): Promise<void> {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
    if (this.connecting) return this.connecting

    this.setStatus("connecting")

    const p = new Promise<void>((resolve, reject) => {
      const ws = new WebSocket(resolveWsUrl())
      ws.binaryType = "arraybuffer"
      let opened = false

      ws.onopen = () => {
        opened = true
        this.ws = ws
        // Ours goes out unprompted; the device's is already on its way. Nothing
        // else may be sent until both have landed, which is why resolve() waits
        // on whenReady() rather than on the socket.
        this.resetConnectionState()
        this.sendHandshake()
        this.whenReady().then(
          () => {
            this.setStatus("connected")
            void this.doAuth()   // in-band auth, once the channel layer is up
            resolve()
          },
          (err) => {
            reject(err instanceof Error ? err : new Error("handshake failed"))
          },
        )
      }

      ws.onmessage = (ev) => {
        if (ev.data instanceof ArrayBuffer) {
          this.onBinaryChunk(ev.data)
          return
        }
        if (typeof Blob !== "undefined" && ev.data instanceof Blob) {
          ev.data.arrayBuffer().then((buf) => this.onBinaryChunk(buf))
        }
      }

      ws.onclose = () => {
        this.ws = null
        this.stopHeartbeat()
        this.setStatus("disconnected")
        this.failHandshakeWaiters(new Error("WebSocket closed"))
        this.resetConnectionState()
        // Keep `authenticated` as-is across a brief drop — the reconnect's
        // auth{key} either resumes silently or fails (then doHandshake shows login).
        for (const [, req] of this.pending) {
          clearTimeout(req.timer)
          req.reject(new Error("WebSocket closed"))
        }
        this.pending.clear()
        if (!opened) reject(new Error("Connection failed"))
        this.reconnectTimer = setTimeout(() => {
          this.doConnect().catch(() => {})
        }, 2000)
      }

      ws.onerror = () => ws.close()
    })

    this.connecting = p
    p.catch(() => {}).then(() => {
      if (this.connecting === p) this.connecting = null
    })
    return p
  }

  // Auth, in band, once the channel layer is READY: `auth hello` says whether a
  // password is set; if so, resume with the stored key or fall back to the login
  // page. Runs on every (re)connect. Named apart from the CONNECTION handshake
  // above, which is a different thing that used to share the word.
  private async doAuth() {
    try {
      const info = await this.send<{ authRequired: boolean }>("auth hello")
      if (!info.authRequired) {
        this.setAuthenticated(true)
        this.startHeartbeat()
        return
      }
      if (this.token) {
        const res = await this.send<{ ok: boolean }>("auth resume", { key: this.token })
        if (res.ok) {
          this.setAuthenticated(true)
          this.startHeartbeat()
          return
        }
        this.token = null
        sessionStorage.removeItem(TOKEN_KEY)
      }
      this.setAuthenticated(false)   // needs login
    } catch {
      /* socket died mid-handshake — onclose handles the reconnect */
    }
  }

  private startHeartbeat() {
    this.stopHeartbeat()
    this.heartbeatTimer = setInterval(() => {
      if (this.ws?.readyState !== WebSocket.OPEN) return
      this.send("system ping").catch(() => {
        this.setStatus("disconnected")
        this.ws?.close()
      })
    }, 15000)
  }

  private stopHeartbeat() {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer)
      this.heartbeatTimer = null
    }
  }

  // Run `task` after every previously-enqueued task has settled — the
  // open-serialization queue. A task's failure never stalls the queue.
  //
  // A task queued from INSIDE a running task deadlocks: the inner one chains
  // onto a queue whose tail is the outer one, so it waits for the task that is
  // waiting for it. There is no timeout on that and no error — it simply never
  // happens, which is what made an upload sit at 0% forever with the device
  // idle. A multi-command sequence that must own the socket (see
  // `uploadPartition`) therefore calls `sendUnqueued` for its inner steps: it
  // already holds the slot.
  private enqueue<T>(task: () => Promise<T>): Promise<T> {
    const run = this.queue.then(task, task)
    this.queue = run.then(
      () => {},
      () => {},
    )
    return run
  }

  /** Next free id in our half, skipping anything still in flight. */
  private allocSession(): number {
    const base = this.lowHalf ? LOW_BASE : HIGH_BASE
    const limit = this.lowHalf ? LOW_LIMIT : HIGH_LIMIT
    for (let i = 0; i < limit - base; i++) {
      const id = this.nextChannel
      this.nextChannel = id + 1 >= limit ? base : id + 1
      if (!this.pending.has(id) && !this.theirChannels.has(id)) return id
    }
    throw new Error("no free channel ids")
  }

  private drawNonce(): bigint {
    const a = new BigUint64Array(1)
    crypto.getRandomValues(a)
    return a[0]
  }

  /** Our half of the handshake, sent unprompted the moment the socket opens. */
  private sendHandshake(mixIn = 0n) {
    this.nonce = (this.drawNonce() ^ mixIn) & 0xffffffffffffffffn
    this.handshakeAttempts++
    const body = new Uint8Array(9)
    body[0] = PROTOCOL_VERSION
    let v = this.nonce
    for (let i = 0; i < 8; i++) {
      body[1 + i] = Number(v & 0xffn)
      v >>= 8n
    }
    // Channel 0 by convention, ignored on receipt.
    this.sendChunk(0, FLAG_CONTROL, body)
  }

  private onControl(body: Uint8Array) {
    if (body.length < 9) return

    if (this.ready) {
      // The device restarted underneath us. Drop what we thought we knew and
      // handshake again rather than talking about channels it has forgotten.
      this.resetConnectionState()
      this.sendHandshake()
    }

    const version = body[0]
    if (version !== PROTOCOL_VERSION) {
      this.failHandshake(
        new Error(`device speaks protocol ${version}, this UI speaks ${PROTOCOL_VERSION}`),
      )
      return
    }

    let peer = 0n
    for (let i = 7; i >= 0; i--) peer = (peer << 8n) | BigInt(body[1 + i])

    if (peer === this.nonce) {
      if (this.handshakeAttempts >= MAX_HANDSHAKE_ATTEMPTS) {
        this.failHandshake(new Error("handshake nonce collision"))
        return
      }
      // Mix the peer's nonce in so two identical draws diverge.
      this.sendHandshake(peer)
      return
    }

    this.lowHalf = this.nonce > peer
    this.nextChannel = this.lowHalf ? LOW_BASE : HIGH_BASE
    this.ready = true
    for (const w of this.readyWaiters) w.resolve()
    this.readyWaiters = []
  }

  private failHandshake(err: Error) {
    this.failHandshakeWaiters(err)
    this.ws?.close()
  }

  private failHandshakeWaiters(err: Error) {
    for (const w of this.readyWaiters) w.reject(err)
    this.readyWaiters = []
  }

  private resetConnectionState() {
    this.ready = false
    this.nonce = 0n
    this.handshakeAttempts = 0
    this.lowHalf = false
    this.nextChannel = 0
    this.theirChannels.clear()
  }

  /** Resolves once the handshake has settled. Nothing may be sent before it. */
  private whenReady(): Promise<void> {
    if (this.ready) return Promise.resolve()
    return new Promise((resolve, reject) => this.readyWaiters.push({ resolve, reject }))
  }

  // Send one session chunk: [session:u16 LE | flags | payload].
  private sendChunk(session: number, flags: number, payload: Uint8Array) {
    const frame = new Uint8Array(3 + payload.length)
    frame[0] = session & 0xff
    frame[1] = (session >> 8) & 0xff
    frame[2] = flags
    frame.set(payload, 3)
    this.ws!.send(frame)
  }

  // Register a pending reply for `session`; resolves when its FINAL chunk
  // arrives (reassembled in onBinaryChunk), rejects on REJECT/timeout/close.
  // For a streamed reply the timeout is idle-based: onBinaryChunk bumps it on
  // each chunk (see bumpTimer), so it bounds silence, not total transfer time.
  private awaitReply<T>(
    session: number,
    opts: {
      timeoutMs?: number
      onMessage?: (msg: Record<string, unknown>) => void
      onRecord?: (text: string) => void
      wantsConsole?: boolean
      wantsBody?: boolean
      onData?: (received: number) => void
    } = {},
  ): Promise<T> {
    // `?? 10000` and not a default parameter, because callers forward an
    // optional straight through and undefined has to mean "unset", not "zero".
    const timeoutMs = opts.timeoutMs ?? 10000
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(session)
        reject(new Error("Request timeout"))
      }, timeoutMs)
      this.pending.set(session, {
        resolve: resolve as (data: unknown) => void,
        reject,
        timer,
        timeoutMs,
        reader: opts.wantsConsole ? new ConsoleReplyReader() : new ReplyReader(),
        wantsConsole: opts.wantsConsole,
        onMessage: opts.onMessage,
        onRecord: opts.onRecord,
        wantsBody: opts.wantsBody,
        onData: opts.onData,
      })
    })
  }

  // Restart a pending reply's idle timeout — called on each streamed chunk so a
  // large-but-steady transfer isn't killed by the total-duration cap.
  private bumpTimer(session: number) {
    const req = this.pending.get(session)
    if (!req) return
    clearTimeout(req.timer)
    req.timer = setTimeout(() => {
      this.pending.delete(session)
      req.reject(new Error("Request timeout"))
    }, req.timeoutMs)
  }

  async send<T, M = Record<string, unknown>>(
    type: string,
    params: Record<string, unknown> = {},
    opts: SendOptions<M> = {},
  ): Promise<T> {
    return this.enqueue(() => this.sendUnqueued<T, M>(type, params, opts))
  }

  /** One command, WITHOUT taking a queue slot. Only for a caller that already
   *  holds one — see the deadlock note on `enqueue`. */
  private async sendUnqueued<T, M = Record<string, unknown>>(
    type: string,
    params: Record<string, unknown> = {},
    opts: SendOptions<M> = {},
  ): Promise<T> {
    await this.ensureConnected()
    const session = this.allocSession()
    const reply = this.awaitReply<T>(session, {
      timeoutMs: opts.timeoutMs,
      onMessage: opts.onMessage as ((msg: Record<string, unknown>) => void) | undefined,
    })
    // Request = one FINAL session chunk: the command JSON + '\n' (the device
    // splits the header line from any body; these commands have no body).
    const body = new TextEncoder().encode(JSON.stringify({ type, ...params }) + "\n")
    // OPEN|FINAL: the whole request in one frame, and our direction closed with it.
    this.sendChunk(session, FLAG_OPEN | FLAG_FINAL, body)
    return reply
  }

  // Reassemble a reply from its channel frames. Each frame is
  // [channel:u16 LE | flags | payload]; FLAG_FINAL ends a direction, FLAG_RESET
  // terminates the channel and its payload is the reason.
  private onBinaryChunk(data: ArrayBuffer) {
    const view = new Uint8Array(data)
    if (view.length < 3) return
    const session = view[0] | (view[1] << 8)
    const flags = view[2]

    // Connection-level. The channel id is written as 0 and means nothing here.
    if (flags & FLAG_CONTROL) {
      this.onControl(view.subarray(3))
      return
    }

    // A channel the DEVICE opened at us. Nothing is reserved any more: it names
    // itself in the envelope on its OPEN frame, exactly as a command does, and
    // what used to be "session 0 is logs" is now "this channel said it is logs".
    if (flags & FLAG_OPEN && !this.pending.has(session)) {
      let name = ""
      try {
        name = String(JSON.parse(new TextDecoder().decode(view.subarray(3)).trim()).type ?? "")
      } catch {
        /* unreadable envelope: refuse below */
      }
      if (name === "log stream") {
        this.theirChannels.set(session, name)
      } else {
        // Silence is acceptance, so a refusal has to be said. RESET is it.
        this.sendChunk(session, FLAG_RESET, new TextEncoder().encode("unknown stream"))
      }
      return
    }

    const theirs = this.theirChannels.get(session)
    if (theirs !== undefined) {
      if (flags & FLAG_RESET) {
        this.theirChannels.delete(session)
        return
      }
      if (theirs === "log stream") {
        try {
          const msg = JSON.parse(new TextDecoder().decode(view.subarray(3)))
          this.broadcastHandlers.forEach((fn) => fn(msg))
        } catch {
          /* malformed record: ignore */
        }
      }
      return
    }

    // No matching request: a reply to something that already timed out, or
    // residue from a channel that has closed. Dropping it is the whole handling.
    const req = this.pending.get(session)
    if (!req) return
    if (flags & FLAG_RESET) {
      this.pending.delete(session)
      clearTimeout(req.timer)
      req.reject(new Error(new TextDecoder().decode(view.subarray(3)) || "rejected"))
      return
    }

    // ONE path for every reply shape, because the wire has one: records
    // separated by newlines, optionally followed by a declared body. The reader
    // owns that; this only decides what to do with what it hands back.
    //
    // What is gone with the two branches that used to be here is the assumption
    // underneath both of them -- that a transport chunk is a unit of meaning. It
    // never was. A chunk is emitted when Channel's reply buffer fills, and also
    // when a handler flushes a record, and those are the same frame with the
    // same flags. See lib/reply.ts and main/lib/protocol/ReplyBody.h.
    const records = req.reader.push(view.subarray(3))
    this.bumpTimer(session)

    for (const text of records) {
      req.onRecord?.(text)
      if (!req.onMessage) continue
      try {
        req.onMessage(JSON.parse(text))
      } catch {
        /* a record that is not JSON is not a progress message; ignore it */
      }
    }
    if (req.onData && req.reader.hasBody) req.onData(req.reader.received)

    if (!(flags & FLAG_FINAL)) return

    this.pending.delete(session)
    clearTimeout(req.timer)

    if (req.wantsConsole) {
      req.resolve((req.reader as ConsoleReplyReader).end())
      return
    }

    const reply = (req.reader as ReplyReader).end()

    if (req.wantsBody) {
      if (!reply.body || !reply.header) {
        // The command answered without declaring a body. That is a refusal
        // record -- an unknown partition, a closed door -- and it is now
        // readable AS a refusal instead of being guessed at by length and
        // prefix, which is what this used to do.
        const refusal = tryParse(reply.result) ?? tryParse(reply.records.at(-1) ?? "")
        const why = typeof refusal?.error === "string" ? refusal.error : "no body in reply"
        req.reject(new Error(why))
        return
      }
      req.onData?.(reply.body.length)
      req.resolve({ header: reply.header, body: reply.body })
      return
    }

    try {
      req.resolve(reply.result.length ? JSON.parse(reply.result) : {})
    } catch (e) {
      req.reject(e instanceof Error ? e : new Error("bad reply"))
    }
  }

  subscribe(fn: BroadcastHandler): () => void {
    this.broadcastHandlers.add(fn)
    this.ensureConnected().catch(() => {})
    return () => {
      this.broadcastHandlers.delete(fn)
    }
  }

  // ── API methods ──────────────────────────────────────────────

  async getInfo(): Promise<DeviceInfo> {
    return this.send<DeviceInfo>("system info")
  }

  async getLogs(): Promise<LogsResponse> {
    return this.send<LogsResponse>("log list")
  }

  async getUpdateStatus(): Promise<UpdateStatus> {
    return this.send<UpdateStatus>("partition status")
  }

  /** The device's whole command registry, in one reply.
   *
   *  This is the device describing ITSELF — names, descriptions and argument
   *  declarations — so nothing here is a list this file maintains. The command
   *  console is built entirely out of what this returns. */
  async describeCommands(): Promise<CommandRegistry> {
    return this.send<CommandRegistry>("help", {}, { timeoutMs: 20000 })
  }

  /** One command in the CONSOLE codec: the line as typed, verbatim.
   *
   *  Nothing is translated on the way out — `led set enabled=true` reaches the
   *  device as those bytes, and the firmware's ConsoleEnvelope is what reads
   *  them. The reply comes back as YAML records, which is why this has a reader
   *  of its own: `send` divides records by newline, and that is the JSON codec's
   *  separator rather than the protocol's. */
  async runConsole(
    line: string,
    opts: { timeoutMs?: number; onRecord?: (text: string) => void } = {},
  ): Promise<ConsoleReply> {
    return this.enqueue(async () => {
      await this.ensureConnected()
      const session = this.allocSession()
      const reply = this.awaitReply<ConsoleReply>(session, {
        timeoutMs: opts.timeoutMs ?? 30000,
        onRecord: opts.onRecord,
        wantsConsole: true,
      })
      this.sendChunk(session, FLAG_OPEN | FLAG_FINAL, new TextEncoder().encode(`${line}
`))
      return reply
    })
  }

  async getSettings(): Promise<SettingsResponse> {
    return this.send<SettingsResponse>("settings list")
  }

  async setSetting(key: string, value: string): Promise<{ ok: boolean }> {
    return this.send("settings set", { key, value })
  }

  async saveSettings(): Promise<{ ok: boolean }> {
    return this.send("settings save")
  }

  async wifiScan(): Promise<WifiScanResponse> {
    return this.send<WifiScanResponse>("wifi scan")
  }

  async getPartitions(): Promise<PartitionsResponse> {
    return this.send<PartitionsResponse>("partition list")
  }

  async reboot(): Promise<{ ok: boolean }> {
    return this.send("system reboot")
  }

  async getLed(): Promise<LedState> {
    return this.send<LedState>("led get")
  }

  // ── The PCB1246 OpenTherm module ───────────────────────────────

  /** The frames that crossed the module after `after` (a seq), oldest first. */
  async getModuleFrames(after: number): Promise<ModuleFramesReply> {
    return this.send<ModuleFramesReply>("module frames", { after })
  }

  /** The testbench's serial link to the module. */
  async getModuleLink(): Promise<ModuleLinkState> {
    return this.send<ModuleLinkState>("module link")
  }

  /** Put one frame on one of the module's lines. */
  async sendModuleFrame(side: OpenThermSide, frame: number): Promise<{ ok: boolean; error?: string }> {
    return this.send<{ ok: boolean; error?: string }>("module send", { side, frame })
  }

  /** Which boiler each thermostat is connected to, and the forwarding counters. */
  async getRouting(): Promise<RoutingState> {
    return this.send<RoutingState>("route get")
  }

  /** Connect a thermostat to a boiler, or leave it open; whoever had that boiler is
   *  left open. The device saves it. */
  async setRoute(master: Master, slave: Slave | "open"): Promise<RoutingState> {
    return this.send<RoutingState>("route set", { master, slave })
  }

  /** Turns the link indication on or off. Omitted fields are left alone by the
   *  device, so `{}` is a no-op that still reports the current state. */
  async setLed(params: { enabled?: boolean }): Promise<LedState> {
    return this.send<LedState>("led set", params)
  }

  /** Throws on connection failure. On success stores the session key and marks
   *  the connection authenticated.
   *
   *  A refusal is not always a wrong password: the device rate-limits failed
   *  logins and says so, and reporting that as "invalid password" is how
   *  somebody spends an hour retyping a password that was right. */
  async login(password: string): Promise<LoginOutcome> {
    await this.ensureConnected()
    const res = await this.send<{ ok: boolean; key?: string; error?: string; retryAfter?: number }>(
      "auth login",
      { password },
    )
    if (!res.ok) {
      return res.error
        ? { ok: false, error: res.error, retryAfter: res.retryAfter }
        : { ok: false }
    }
    this.token = res.key ?? null
    if (this.token) sessionStorage.setItem(TOKEN_KEY, this.token)
    this.setAuthenticated(true)
    this.startHeartbeat()
    return { ok: true }
  }

  /** One command whose REQUEST has a body: an envelope chunk (not FINAL), then
   *  `body` streamed on the same session in window-sized pieces, then one reply.
   *
   *  The generic form of what `uploadPartition` does by hand, kept generic because
   *  `partition write` is not the only command shaped like this. What is NOT here is
   *  the erase-write-activate sequence: that is partition policy and belongs to
   *  whoever knows about partitions, which is the page above, not this transport.
   *
   *  Runs through the open queue, so nothing else touches the socket mid-upload — the
   *  device would REJECT an interleaved session id. */
  async uploadSession<T>(
    type: string,
    params: Record<string, unknown> | undefined,
    body: Blob,
    onProgress?: (fraction: number) => void,
  ): Promise<T> {
    return this.enqueue(async () => {
      await this.ensureConnected()

      const session = this.allocSession()
      const total = body.size

      // Progress is DEVICE-driven: the handler streams {"p":<bytesWritten>} as it
      // works, and those are mapped to a fraction. Client-side "bytes sent" cannot
      // see the device's write position — the OS buffers the socket — so it would
      // race to 1 while the write is still in flight.
      const reply = this.awaitReply<T>(session, {
        timeoutMs: 120000,
        onMessage: (msg: Record<string, unknown>) => {
          if (total && typeof msg.p === "number")
            onProgress?.(Math.min(1, msg.p / total))
        },
      })

      const envelope = new TextEncoder().encode(
        JSON.stringify({ type, ...(params ?? {}) }) + "\n",
      )
      // OPEN, not FINAL: the body follows on the same channel.
      this.sendChunk(session, FLAG_OPEN, envelope)

      // Kept under the smallest inbound window on the path — the device's own, or
      // the relay's when there is one in between. It is a CONVENTION, not a
      // contract: each hop owns its own framing and neither tells us its number.
      // Overshooting now costs this one channel a RESET rather than the socket,
      // which is what makes picking conservatively here good enough.
      const CHUNK = 4096
      let sent = 0
      while (sent < total) {
        const end = Math.min(sent + CHUNK, total)
        const slice = new Uint8Array(await body.slice(sent, end).arrayBuffer())
        const isLast = end >= total
        await this.drainBuffer()
        this.sendChunk(session, isLast ? FLAG_FINAL : 0, slice)
        sent = end
      }
      // A zero-length body still needs a FINAL to close the request direction.
      if (total === 0) this.sendChunk(session, FLAG_FINAL, new Uint8Array(0))

      const result = await reply
      onProgress?.(1)
      return result
    })
  }

  /** Upload a .bin as one streamed `writePartition` session: an envelope chunk
   *  ({"type":"writePartition","partition":...}\n) followed by body chunks, the
   *  last carrying FLAG_FINAL. The device drains it straight to flash and replies
   *  once, at end-of-stream. Runs through the open queue, so nothing else touches
   *  the socket mid-upload (the device would REJECT an interleaved session id). */
  async uploadPartition(
    partition: string,
    file: File,
    onProgress?: (percent: number) => void,
    signal?: AbortSignal,
  ): Promise<UploadResult> {
    return this.enqueue(async () => {
      await this.ensureConnected()

      // sendUnqueued, not send: this task already holds the queue slot, and
      // re-entering enqueue from here waits for this task to finish. See the
      // note on `enqueue`.
      //
      // Erase first: `partition write` never erases, and flash bits only clear on
      // erase, so writing over stale content would produce an image that fails
      // validation at activate.
      const cleared = await this.sendUnqueued<{ ok: boolean; error?: string }>("partition clear", { partition })
      if (!cleared.ok) throw new Error(cleared.error ?? "partition clear failed")

      const session = this.allocSession()
      const total = file.size

      // Progress is DEVICE-driven: the handler streams {"p":<bytesWritten>}
      // messages as it flashes, and we map those to a percentage. Client-side
      // "bytes sent" can't see the device's write position (the OS buffers the
      // socket), so it would race to 100% while the write is still in flight.
      const reply = this.awaitReply<{ ok: boolean; size?: number; error?: string }>(session, {
        timeoutMs: 120000,
        onMessage: (msg) => {
          if (total && typeof msg.p === "number") {
            onProgress?.(Math.min(100, Math.round((msg.p / total) * 100)))
          }
        },
      })

      // Envelope chunk (not FINAL — the body follows on the same session id).
      // The route is TWO words: ReadCommandRoute splits "<category> <command>" and
      // rejects a single-word type outright, so "writePartition" never reached the
      // handler — every upload from this page was refused before it started.
      const envelope = new TextEncoder().encode(JSON.stringify({ type: "partition write", partition }) + "\n")
      // OPEN, not FINAL: the body follows on the same channel.
      this.sendChunk(session, FLAG_OPEN, envelope)

      // Body chunks, kept under the smallest inbound window on the path. See the
      // note on the other CHUNK above: a convention, not a negotiated limit.
      const CHUNK = 4096
      let sent = 0
      while (sent < total) {
        if (signal?.aborted) {
          // Stopping mid-body cannot just stop: the device is blocked reading
          // this channel and would wait for a FINAL that is never coming, and a
          // FINAL sent early would be read as a COMPLETE upload - a truncated
          // image that passes for a whole one.
          //
          // This used to drop the whole socket, because there was no way to say
          // "this one is over" without saying "this one finished". RESET is that
          // way: the device's read fails, `partition write` reports it, the boot
          // slot was never touched, and every other channel on the connection is
          // untouched too.
          this.sendChunk(session, FLAG_RESET, new TextEncoder().encode("cancelled"))
          throw new UploadCancelled()
        }
        const end = Math.min(sent + CHUNK, total)
        const slice = new Uint8Array(await file.slice(sent, end).arrayBuffer())
        const isLast = end >= total
        await this.drainBuffer()
        this.sendChunk(session, isLast ? FLAG_FINAL : 0, slice)
        sent = end
      }
      // A zero-length file still needs a FINAL to close the request direction.
      if (total === 0) this.sendChunk(session, FLAG_FINAL, new Uint8Array(0))

      const res = await reply
      if (!res.ok) throw new Error(res.error ?? "partition write failed")

      // NOTHING is activated here. An upload writes bytes; choosing what boots is
      // `partition activate`, which the Firmware page offers as its own button.
      // Uploading used to switch the boot slot as its last step, which meant the
      // only way to stage an image was to commit to it.
      onProgress?.(100)
      return { ok: true, size: res.size ?? sent }
    })
  }

  /** Choose which app partition boots. `restart` reboots into it immediately;
   *  without it the switch waits for the next restart. This is the only call
   *  that changes the boot slot — `reboot()` never does. */
  async activatePartition(partition: string, restart = false): Promise<void> {
    const res = await this.send<{ ok: boolean; error?: string }>("partition activate", {
      partition,
      restart,
    })
    if (!res.ok) throw new Error(res.error ?? "partition activate failed")
  }

  // Backpressure: don't let the browser-side WS buffer outrun the socket.
  private async drainBuffer(limit = 64 * 1024) {
    while (this.ws && this.ws.bufferedAmount > limit) {
      await new Promise((r) => setTimeout(r, 20))
    }
  }

  /** One command whose reply is a HEADER RECORD AND A BODY: the header names the
   *  media type, the bytes follow it, and this returns both.
   *
   *  Generic on purpose -- it names no command and no media type. That is the
   *  whole point of #32: a caller that discovered a command through `help
   *  describe` can perform it and read the answer without a second, private
   *  agreement about which commands return bytes. The relay's MCP surface reads
   *  replies by exactly this rule already (api/Mcp/DeviceReply.cs).
   *
   *  A command that refuses answers with an ordinary record and no body, and
   *  that arrives here as a rejection carrying the device's own `error` -- not
   *  as a tiny file. It used to be found by testing whether a short reply began
   *  with {"ok":false, which a partition image is perfectly entitled to contain.
   *
   *  Runs through the open queue, so it owns the socket until it finishes. */
  async downloadSessionWithHeader<H = Record<string, unknown>>(
    type: string,
    params: Record<string, unknown> | undefined,
    total?: number,
    onProgress?: (fraction: number) => void,
  ): Promise<BodyReply<H>> {
    const reply = await this.enqueue(async () => {
      await this.ensureConnected()
      const session = this.allocSession()
      const pending = this.awaitReply<BodyReply<H>>(session, {
        timeoutMs: 120000,
        wantsBody: true,
        onData: (received) => {
          if (total) onProgress?.(Math.min(1, received / total))
        },
      })
      const body = new TextEncoder().encode(
        JSON.stringify({ type, ...(params ?? {}) }) + "\n",
      )
      this.sendChunk(session, FLAG_OPEN | FLAG_FINAL, body)
      return pending
    })

    // The device streams a whole partition, so a short body means a mid-stream
    // flash or socket failure produced a truncated image followed by a FINAL --
    // which would otherwise be saved as a corrupt file that looks fine. The
    // header now states the size, so this no longer depends on the CALLER
    // knowing it.
    const declared =
      typeof (reply.header as Record<string, unknown>).size === "number"
        ? ((reply.header as Record<string, unknown>).size as number)
        : total
    if (declared !== undefined && reply.body.length !== declared)
      throw new Error(`incomplete download: got ${reply.body.length} of ${declared} bytes`)

    onProgress?.(1)
    return reply
  }

  /** Download a partition image and save it as <label>.bin.
   *
   *  Everything about reading the reply now lives in downloadSessionWithHeader;
   *  what is left here is the one thing that is genuinely this method's own --
   *  that a browser saves a file by clicking an anchor. */
  async downloadPartitionFile(
    label: string,
    expectedSize?: number,
    onProgress?: (percent: number) => void,
  ): Promise<void> {
    const { body } = await this.downloadSessionWithHeader(
      "partition read",
      { partition: label },
      expectedSize,
      (fraction) => onProgress?.(Math.round(fraction * 100)),
    )

    const url = URL.createObjectURL(new Blob([body as BlobPart]))
    const a = document.createElement("a")
    a.href = url
    a.download = `${label}.bin`
    a.click()
    URL.revokeObjectURL(url)
    onProgress?.(100)
  }

}

function tryParse(text: string): Record<string, unknown> | null {
  try {
    const v = JSON.parse(text)
    return v && typeof v === "object" ? (v as Record<string, unknown>) : null
  } catch {
    return null
  }
}

const instance = new BackendService()
instance.connect()
export const backend = instance

// ── Types ────────────────────────────────────────────────────────

export interface DeviceInfo {
  name: string
  project: string
  firmware: string
  idf: string
  date: string
  time: string
  chip: string
  cpu: string
  ip: string
  heapFree: number
  heapMin: number
  deviceTime: string
}

// ── The PCB1246 OpenTherm module (ModuleLinkManager) ─────────────────────────

export type OpenThermSide = "thermostat" | "boiler"

/** `route get` / `route set`: each thermostat's boiler, "open" for none. */
export interface RoutingState {
  ok: boolean
  error?: string
  thermostat: Slave | "open"
  thermostatSim: Slave | "open"
  counters: { forwarded: number; queueFull: number; busy: number; failed: number; toSimulator: number }
}

/** One frame that crossed the module. */
export interface ModuleFrame {
  /** The testbench's numbering: from 1, gap-free, restarting with the testbench. */
  seq: number
  side: OpenThermSide
  /** rx: off the line, from the thermostat or boiler. tx: put on the line by the testbench. */
  dir: "rx" | "tx"
  /** The 32 bits on the line, parity included. */
  frame: number
  /** The testbench's clock (ms since boot) when it recorded the frame. */
  ms: number
  /** The module's clock when it collected the frame; rx only. */
  moduleMs?: number
  /** A received frame the testbench forwarded: the line it went out on. */
  to?: OpenThermSide
}

export interface ModuleFramesReply {
  /** The newest seq the testbench holds. Below the cursor asked with: it restarted. */
  latest: number
  /** Frames after the cursor that had already left the testbench's ring. */
  missed: number
  /** Whether the link to the module is up right now. */
  link: boolean
  frames: ModuleFrame[]
}

export interface ModuleLinkState {
  phase: "handshake" | "ready" | "failed" | "paused"
  stream: boolean
  framesRecorded: number
  counters: Record<string, number>
}

// ── The LED, this template's worked example ──────────────────────────────────
//
// A product built from Strux deletes this pair along with LedManager and puts its
// own feature's commands here — the demo is the only thing in this file that is not
// framework.

export interface LedState {
  /** Whether the LED is showing the relay link at all. */
  enabled: boolean
  /** The relay pipe's state — what the LED is indicating. */
  connected: boolean
  /** The pin, which is `enabled && connected`. */
  on: boolean
}

export interface UpdateStatus {
  firmware: string
  running: string
  nextSlot: string
}

export interface UploadResult {
  ok: boolean
  size: number
}

export type SettingType = "string" | "int32" | "uint32" | "float" | "bool"

export const NUMERIC_SETTING_TYPES: SettingType[] = ["int32", "uint32", "float"]

export interface SettingEntry {
  key: string
  label: string
  type: SettingType
  value: string | number | boolean
}

export interface SettingsResponse {
  settings: SettingEntry[]
}

export interface WifiNetwork {
  ssid: string
  rssi: number
  channel: number
  secure: boolean
  /// The auth mode by name ("WPA2", "WPA2/WPA3", "open"). `secure` is the padlock;
  /// this is the detail that tells a refusal from a distance. Optional because a
  /// device on older firmware does not send it.
  auth?: string
}

export interface WifiScanResponse {
  ok: boolean
  networks: WifiNetwork[]
}

export interface LogsResponse {
  lines: string[]
}

export interface Partition {
  label: string
  type: string
  subtype: string
  offset: number
  size: number
  running: boolean
  nextOta: boolean
  uploadable: boolean
  version: string
}

export interface PartitionsResponse {
  partitions: Partition[]
}

// ── Command self-description (`help`) ────────────────────────────
//
// The shape the device answers with, transcribed and nothing more. No command
// name appears anywhere below, deliberately: everything the console knows about
// a command it learned at runtime from this reply.

/** The argument types `ArgTypeName` reports. */
export type CommandArgType = "string" | "uint32" | "int32" | "float" | "bool"

/** One declared argument of one command. */
export interface CommandArgDesc {
  name: string
  type: CommandArgType
  required: boolean
  /** Strings only: the length at which the device refuses a value. */
  maxLength?: number
  /** Absent when the command did not describe it — which is not the same as an
   *  empty description, and is worth showing differently. */
  description?: string
}

export interface CommandDesc {
  /** The command exactly as the wire carries it: `partition write`. */
  name: string
  description?: string
  arguments: CommandArgDesc[]
}

/** Every command the device has, flat. The registry says nothing about grouping,
 *  because grouping is a question about a command rather than part of it. */
export interface CommandRegistry {
  ok: boolean
  commands: CommandDesc[]
}
