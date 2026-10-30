#pragma once

// How a reply is shaped, so that a caller can read one WITHOUT knowing which
// command it called.
//
// This file is the CONVENTION. What implements it is ReplyWriter: a record ends
// when its root scope closes, the separator is written before whatever follows, and
// ReplyObject::body declares a media type and hands back the stream. No handler
// writes a newline.
//
// ── The convention ───────────────────────────────────────────────────────────
//
// A reply is a sequence of JSON records separated by '\n'. The last record needs
// no separator, because FLAG_FINAL ends the reply.
//
//     ordinary command   {"ok":true,"free":83404}
//     progress + result  {"p":32768}\n{"p":65536}\n{"ok":true,"size":65536}
//
// If the FIRST record carries a "contentType" field, it is the only record and
// everything after its newline is an opaque body of that media type, read until
// FINAL:
//
//     header + body      {"ok":true,"status":200,"contentType":"text/html"}\n<bytes>
//
// The PRESENCE of contentType is what says there is a body. A reply that declares
// none is records and nothing else, which is what almost every command answers --
// so a caller that never heard of this reads those exactly as it always did.
//
// ── Why a newline, and not a flag or a length ────────────────────────────────
//
// Because a Connection chunk MEANS NOTHING at this layer, and must not be allowed
// to start meaning something. Channel emits a chunk when its reply buffer fills,
// and a handler's flush() emits one too; on the wire those are the same frame with
// the same flags, and they always will be. So a reader that treats "a chunk" as "a
// record" is reading the transport's buffer size as application structure -- it
// gets the right answer only while every record happens to be smaller than a
// window nobody promised it. Raise the window, lower it, put a relay in the middle
// that re-frames, tunnel the link over UART, and the same reply parses differently.
//
// A separator in the BYTES has none of that: it survives any chunking, any
// re-framing, and any number of hops, because it travels with the data instead of
// beside it. It is also already the convention twice over -- web read has always
// split its header from its body this way, and the telemetry stream packs several
// points into one frame the same way -- so this names what the tree was doing
// rather than adding a mechanism to it.
//
// It costs one byte per record and it cannot collide: ReplyWriter escapes '\n'
// inside strings and drops every control character below 0x20, so a record it
// produces can never contain a raw newline.
//
// The alternative, a FLAG_RECORD bit on flushed chunks, was not taken. It would
// have worked, but it puts application framing back in the transport header --
// every Connection implementation would then owe it, and #27's UART, BLE and
// ESP-NOW links would each have to carry a flag that a byte in the payload
// already carries for free.
namespace protocol
{
    /// What to declare for bytes with no better name: a flash image, an unknown
    /// blob. A caller sees "there is a body and it is not text", which is the
    /// distinction it actually needs.
    inline constexpr const char* CONTENT_TYPE_OCTETS = "application/octet-stream";
}
