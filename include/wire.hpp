#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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
    // Client -> server: start (or restart) receiving MarketData for a
    // symbol on this connection. Payload is just the symbol, the same shape
    // as AddSymbol. Rejected with a Response{UnknownSymbol} if the symbol
    // isn't registered; otherwise answered with one MarketData snapshot
    // (empty trades) and, from then on, a MarketData push after every
    // request that changes that symbol's book. Subscribing again while
    // already subscribed is a no-op that just resends the snapshot.
    Subscribe = 8,
    // Client -> server: stop receiving MarketData for a symbol on this
    // connection. Always succeeds, including when not currently subscribed
    // -- idempotent, like Subscribe.
    Unsubscribe = 9,
    // Server -> client only, like Response: never decoded as a client
    // request. Sent once as the reply to a successful Subscribe (with
    // `trades` empty, since nothing happened -- it's a snapshot, not an
    // event), and again, unsolicited, after every accepted LimitOrder,
    // MarketOrder, ModifyOrder or CancelOrder that touched a symbol this
    // connection is subscribed to. See MarketDataMessage below.
    MarketData = 10,
    // Client -> server: "catch me up on `symbol` since `since_sequence`,
    // without going through a fresh Subscribe." Payload is the symbol plus
    // an 8-byte since_sequence (see Request::since_sequence). Rejected with
    // a Response{UnknownSymbol} exactly like Subscribe; otherwise answered
    // with one MarketData message per event the server can still supply
    // starting at since_sequence + 1, each carrying this request's
    // correlation id -- or, if that much history is no longer retained (or
    // since_sequence is already caught up), a single MarketData snapshot
    // shaped exactly like a fresh Subscribe's, so a client that can't be
    // caught up incrementally still ends up in a known state. Independent of
    // subscription state: it neither requires nor creates a Subscribe, since
    // its entire purpose is recovering from a gap in a subscription a client
    // already has.
    ResyncMarketData = 11,
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
    // ResyncMarketData only: the last sequence this client is known to have
    // for `symbol`. 0 means "I have nothing yet", the same starting point a
    // fresh Subscribe implies.
    std::uint64_t since_sequence = 0;
};

struct Response {
    std::uint32_t correlation_id = 0;
    RejectReason reason = RejectReason::None;
    bool cancelled_by_self_trade = false;
    Quantity unfilled = 0;
    std::vector<Trade> trades;
};

// A push about one symbol's public book state: sent once as the answer to a
// successful Subscribe, and again, unsolicited, after every subsequent
// change to that symbol. `sequence` is a monotonic counter kept per symbol
// by OrderServer, incremented before each unsolicited push and reported
// as-is in a Subscribe snapshot -- so the snapshot's value is "what has
// already happened," and a subscriber that then sees anything other than
// sequence+1, +2, +3, ... next has missed one and should re-subscribe for a
// fresh snapshot rather than trust what it has.
//
// `best_bid`/`best_ask` mirror MatchingEngine::bestBid/bestAsk's own
// std::optional: unset means an empty side, not a sentinel price to
// misinterpret as real.
//
// `bid_levels`/`ask_levels` are aggregated depth, best price first, out to
// whatever depth the server is configured to publish (see OrderServer) --
// not necessarily the whole book. best_bid/best_ask are redundant with
// bid_levels[0]/ask_levels[0] when depth is at least 1, kept as their own
// fields rather than removed so a client that only ever wants top-of-book
// doesn't have to unpack an array to get it, and so an empty-book message
// (no levels at all) still has an unambiguous "empty" to check.
struct MarketDataMessage {
    std::uint32_t correlation_id = 0;  // 0 on an unsolicited push; nothing requested it
    Symbol symbol;
    std::uint64_t sequence = 0;
    std::optional<Price> best_bid;
    std::optional<Price> best_ask;
    std::vector<PriceLevel> bid_levels;
    std::vector<PriceLevel> ask_levels;
    std::vector<Trade> trades;  // empty on a Subscribe snapshot; may be empty on a push too
};

// Appends a complete framed message to `out`. False if the request cannot be
// represented -- currently only an over-long symbol, which is rejected rather
// than truncated, since truncation would silently alias two instruments onto
// one book.
bool encodeRequest(const Request& request, std::vector<std::uint8_t>& out);
void encodeResponse(const Response& response, std::vector<std::uint8_t>& out);
// False if `message.symbol` doesn't fit (see encodeRequest above); in
// practice this can't happen, since the symbol always came from one already
// validated the same way when a client's Subscribe request was decoded.
bool encodeMarketData(const MarketDataMessage& message, std::vector<std::uint8_t>& out);

// Decode a frame's payload; the header has already been read. False if the
// payload is the wrong size for its type or otherwise malformed.
bool decodeRequest(MessageType type, std::uint32_t correlation_id, const std::uint8_t* payload,
                   std::size_t length, Request& out);
bool decodeResponse(std::uint32_t correlation_id, const std::uint8_t* payload, std::size_t length,
                    Response& out);
bool decodeMarketData(std::uint32_t correlation_id, const std::uint8_t* payload, std::size_t length,
                      MarketDataMessage& out);

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
