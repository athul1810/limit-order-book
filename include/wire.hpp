#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "matching_engine.hpp"

namespace matching_engine {

// Binary wire protocol for submitting orders over a socket.
//
// Every integer is big-endian and every field is written out byte by byte.
// Structs are never memcpy'd onto the wire: their layout depends on the
// compiler's padding and the host's endianness, neither of which is part of a
// protocol two different builds have to agree on.
//
// Framing is length-prefixed because TCP is a byte stream, not a message
// stream. A single read() can deliver half a message, three messages, or two
// and a half; the length prefix is what lets a reader tell where one ends.
//
// Header, 12 bytes:
//   u32 payload_length   bytes following this header
//   u32 correlation_id   echoed back on the response, so a client can match
//                        replies to requests without assuming ordering
//   u8  message_type
//   u8  version
//   u16 reserved         must be zero
//
// payload_length is u32 rather than u16 deliberately: one market order sweeping
// a deep book can produce thousands of trades, and a 16-bit length would cap a
// response at about 2000 of them -- a limit that would only ever be discovered
// in production, on the largest and most interesting order of the day.
inline constexpr std::size_t kHeaderBytes = 12;
inline constexpr std::uint8_t kWireVersion = 1;

// Symbols are fixed-width on the wire so a request has a predictable shape.
inline constexpr std::size_t kSymbolBytes = 8;

// Bounds how much a single frame can make the server buffer. Without it, a
// client that claims a 4GB payload makes the server try to allocate it.
inline constexpr std::uint32_t kMaxPayloadBytes = 1u << 20;  // 1 MiB

enum class MessageType : std::uint8_t {
    AddSymbol = 1,
    LimitOrder = 2,
    MarketOrder = 3,
    ModifyOrder = 4,
    CancelOrder = 5,
    Response = 6,
    // Presents a credential on this connection. Only meaningful to
    // OrderServer, which intercepts it before it ever reaches applyRequest --
    // the engine itself has no notion of authentication. When the server
    // does not require authentication at all, this trivially succeeds; see
    // server.hpp.
    Authenticate = 7,
};

// One decoded client request. Fields not used by a given message type are left
// zeroed -- a flat struct rather than a variant because the set is small and
// fixed, and every consumer switches on `type` anyway.
struct Request {
    MessageType type = MessageType::AddSymbol;
    std::uint32_t correlation_id = 0;
    Symbol symbol;
    OrderId order_id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Quantity quantity = 0;
    ParticipantId participant = kNoParticipant;
    std::string token;  // Authenticate only: the raw credential, no encoding
};

struct Response {
    std::uint32_t correlation_id = 0;
    RejectReason reason = RejectReason::None;
    bool cancelled_by_self_trade = false;
    Quantity unfilled = 0;
    std::vector<Trade> trades;
};

// Appends a complete framed message to `out`. False if the request cannot be
// represented -- currently only an over-long symbol, which is rejected rather
// than truncated, since truncation would silently alias two instruments onto
// one book.
bool encodeRequest(const Request& request, std::vector<std::uint8_t>& out);
void encodeResponse(const Response& response, std::vector<std::uint8_t>& out);

// Decode a frame's payload; the header has already been read. False if the
// payload is the wrong size for its type or otherwise malformed.
bool decodeRequest(MessageType type, std::uint32_t correlation_id, const std::uint8_t* payload,
                   std::size_t length, Request& out);
bool decodeResponse(std::uint32_t correlation_id, const std::uint8_t* payload, std::size_t length,
                    Response& out);

// Reassembles frames from a TCP byte stream.
//
// Bytes arrive in whatever chunks the network hands over, so this buffers
// partial frames until they are complete and yields whole ones. It is the
// piece most worth testing directly: split a message anywhere and the result
// must be identical, which is exactly the property a live socket exercises
// only intermittently and only under load.
class FrameReader {
   public:
    void append(const std::uint8_t* data, std::size_t length);

    // Pops one complete frame. False when more bytes are needed, or when the
    // stream has failed -- check failed() to tell those apart.
    bool next(MessageType& type, std::uint32_t& correlation_id, std::vector<std::uint8_t>& payload);

    // A protocol violation: wrong version, or a payload over kMaxPayloadBytes.
    // Unrecoverable, because the framing is no longer trustworthy -- the only
    // correct response is to drop the connection.
    bool failed() const { return failed_; }
    const char* failureReason() const { return failure_reason_; }

    std::size_t buffered() const { return buffer_.size() - consumed_; }

   private:
    void compact();

    std::vector<std::uint8_t> buffer_;
    std::size_t consumed_ = 0;
    bool failed_ = false;
    const char* failure_reason_ = "";
};

// Applies a request to the engine and produces the response to send back.
// Pure dispatch with no I/O, so the whole server behaviour is testable without
// opening a socket.
Response applyRequest(const Request& request, MatchingEngine& engine);

}  // namespace matching_engine
