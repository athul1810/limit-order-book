#include "wire.hpp"

#include <cstring>

namespace matching_engine {

namespace {

// Two's complement is guaranteed from C++20 and universal in practice well
// before it; asserting it here means a hypothetical target where it doesn't
// hold fails to compile rather than silently mangling negative prices.
static_assert(static_cast<std::uint64_t>(static_cast<std::int64_t>(-1)) == ~std::uint64_t{0},
              "wire format assumes two's complement signed integers");

void putU8(std::vector<std::uint8_t>& out, std::uint8_t value) { out.push_back(value); }

void putU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void putU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void putI64(std::vector<std::uint8_t>& out, std::int64_t value) {
    putU64(out, static_cast<std::uint64_t>(value));
}

std::uint32_t getU32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::uint64_t getU64(const std::uint8_t* p) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | static_cast<std::uint64_t>(p[i]);
    return value;
}

std::int64_t getI64(const std::uint8_t* p) { return static_cast<std::int64_t>(getU64(p)); }

// Symbols travel null-padded in a fixed-width field.
bool putSymbol(std::vector<std::uint8_t>& out, const Symbol& symbol) {
    if (symbol.size() > kSymbolBytes) return false;
    for (std::size_t i = 0; i < kSymbolBytes; ++i) {
        out.push_back(i < symbol.size() ? static_cast<std::uint8_t>(symbol[i]) : 0);
    }
    return true;
}

Symbol getSymbol(const std::uint8_t* p) {
    std::size_t length = 0;
    while (length < kSymbolBytes && p[length] != 0) ++length;
    return Symbol(reinterpret_cast<const char*>(p), length);
}

void writeHeader(std::vector<std::uint8_t>& out, std::size_t header_at, MessageType type,
                 std::uint32_t correlation_id) {
    const std::uint32_t payload_length = static_cast<std::uint32_t>(out.size() - header_at - kHeaderBytes);
    std::vector<std::uint8_t> header;
    header.reserve(kHeaderBytes);
    putU32(header, payload_length);
    putU32(header, correlation_id);
    putU8(header, static_cast<std::uint8_t>(type));
    putU8(header, kWireVersion);
    putU16(header, 0);  // reserved
    std::memcpy(out.data() + header_at, header.data(), kHeaderBytes);
}

// Payload sizes, so a truncated or padded frame is rejected rather than read
// past its end.
constexpr std::size_t kAddSymbolBytes = kSymbolBytes;
constexpr std::size_t kLimitOrderBytes = kSymbolBytes + 8 + 1 + 8 + 8 + 8;
constexpr std::size_t kMarketOrderBytes = kSymbolBytes + 8 + 1 + 8 + 8;
constexpr std::size_t kModifyOrderBytes = kSymbolBytes + 8 + 8 + 8;
constexpr std::size_t kCancelOrderBytes = kSymbolBytes + 8;
// Subscribe and Unsubscribe carry only a symbol, the same shape as
// AddSymbol.
constexpr std::size_t kSubscribeBytes = kSymbolBytes;
constexpr std::size_t kUnsubscribeBytes = kSymbolBytes;
constexpr std::size_t kResponseFixedBytes = 1 + 1 + 4 + 8;
constexpr std::size_t kTradeBytes = 8 + 8 + 8 + 8;
// symbol + sequence + has_bid + bid_price + has_ask + ask_price + trade_count
constexpr std::size_t kMarketDataFixedBytes = kSymbolBytes + 8 + 1 + 8 + 1 + 8 + 4;

bool sideFromByte(std::uint8_t value, Side& out) {
    if (value == 0) {
        out = Side::Buy;
        return true;
    }
    if (value == 1) {
        out = Side::Sell;
        return true;
    }
    return false;
}

std::uint8_t sideToByte(Side side) { return side == Side::Buy ? 0 : 1; }

bool reasonFromByte(std::uint8_t value, RejectReason& out) {
    switch (value) {
        case 0: out = RejectReason::None; return true;
        case 1: out = RejectReason::DuplicateOrderId; return true;
        case 2: out = RejectReason::UnknownOrder; return true;
        case 3: out = RejectReason::InvalidQuantity; return true;
        case 4: out = RejectReason::UnknownSymbol; return true;
        case 5: out = RejectReason::NotAuthenticated; return true;
        case 6: out = RejectReason::AuthenticationFailed; return true;
        case 7: out = RejectReason::RateLimited; return true;
        default: return false;
    }
}

std::uint8_t reasonToByte(RejectReason reason) {
    switch (reason) {
        case RejectReason::None: return 0;
        case RejectReason::DuplicateOrderId: return 1;
        case RejectReason::UnknownOrder: return 2;
        case RejectReason::InvalidQuantity: return 3;
        case RejectReason::UnknownSymbol: return 4;
        case RejectReason::NotAuthenticated: return 5;
        case RejectReason::AuthenticationFailed: return 6;
        case RejectReason::RateLimited: return 7;
    }
    return 0;
}

}  // namespace

bool encodeRequest(const Request& request, std::vector<std::uint8_t>& out) {
    const std::size_t header_at = out.size();
    out.resize(header_at + kHeaderBytes);  // reserved; filled in once the length is known

    // Authenticate has no symbol prefix at all -- its payload is just the raw
    // token -- so it is handled before the common putSymbol() path every
    // other message type shares below.
    if (request.type == MessageType::Authenticate) {
        out.insert(out.end(), request.token.begin(), request.token.end());
        writeHeader(out, header_at, request.type, request.correlation_id);
        return true;
    }

    if (!putSymbol(out, request.symbol)) {
        out.resize(header_at);
        return false;
    }

    switch (request.type) {
        case MessageType::AddSymbol:
        case MessageType::Subscribe:
        case MessageType::Unsubscribe:
            // All three are just a symbol, and putSymbol() above already
            // wrote it.
            break;
        case MessageType::LimitOrder:
            putU64(out, request.order_id);
            putU8(out, sideToByte(request.side));
            putI64(out, request.price);
            putU64(out, request.quantity);
            putU64(out, request.participant);
            break;
        case MessageType::MarketOrder:
            putU64(out, request.order_id);
            putU8(out, sideToByte(request.side));
            putU64(out, request.quantity);
            putU64(out, request.participant);
            break;
        case MessageType::ModifyOrder:
            putU64(out, request.order_id);
            putI64(out, request.price);
            putU64(out, request.quantity);
            break;
        case MessageType::CancelOrder:
            putU64(out, request.order_id);
            break;
        case MessageType::Response:
        case MessageType::MarketData:
            out.resize(header_at);
            return false;  // not a client request
        case MessageType::Authenticate:
            // Unreachable: handled above, before putSymbol(). Kept as an
            // explicit case (rather than a default:) so adding a future
            // message type here trips -Wswitch instead of silently falling
            // through.
            out.resize(header_at);
            return false;
    }

    writeHeader(out, header_at, request.type, request.correlation_id);
    return true;
}

void encodeResponse(const Response& response, std::vector<std::uint8_t>& out) {
    const std::size_t header_at = out.size();
    out.resize(header_at + kHeaderBytes);

    putU8(out, reasonToByte(response.reason));
    putU8(out, response.cancelled_by_self_trade ? 1 : 0);
    putU32(out, static_cast<std::uint32_t>(response.trades.size()));
    putU64(out, response.unfilled);
    for (const Trade& trade : response.trades) {
        putU64(out, trade.buy_order_id);
        putU64(out, trade.sell_order_id);
        putI64(out, trade.price);
        putU64(out, trade.quantity);
    }

    writeHeader(out, header_at, MessageType::Response, response.correlation_id);
}

bool decodeRequest(MessageType type, std::uint32_t correlation_id, const std::uint8_t* payload,
                   std::size_t length, Request& out) {
    out = Request{};
    out.type = type;
    out.correlation_id = correlation_id;

    // Exact-length checks, not minimums: a frame carrying trailing bytes is a
    // sign the two ends disagree about the format, which is worth catching now
    // rather than misreading later.
    switch (type) {
        case MessageType::AddSymbol:
            if (length != kAddSymbolBytes) return false;
            out.symbol = getSymbol(payload);
            return !out.symbol.empty();
        case MessageType::LimitOrder: {
            if (length != kLimitOrderBytes) return false;
            out.symbol = getSymbol(payload);
            out.order_id = getU64(payload + 8);
            if (!sideFromByte(payload[16], out.side)) return false;
            out.price = getI64(payload + 17);
            out.quantity = getU64(payload + 25);
            out.participant = getU64(payload + 33);
            return !out.symbol.empty();
        }
        case MessageType::MarketOrder: {
            if (length != kMarketOrderBytes) return false;
            out.symbol = getSymbol(payload);
            out.order_id = getU64(payload + 8);
            if (!sideFromByte(payload[16], out.side)) return false;
            out.quantity = getU64(payload + 17);
            out.participant = getU64(payload + 25);
            return !out.symbol.empty();
        }
        case MessageType::ModifyOrder:
            if (length != kModifyOrderBytes) return false;
            out.symbol = getSymbol(payload);
            out.order_id = getU64(payload + 8);
            out.price = getI64(payload + 16);
            out.quantity = getU64(payload + 24);
            return !out.symbol.empty();
        case MessageType::CancelOrder:
            if (length != kCancelOrderBytes) return false;
            out.symbol = getSymbol(payload);
            out.order_id = getU64(payload + 8);
            return !out.symbol.empty();
        case MessageType::Authenticate:
            // The whole payload is the raw token; there is no symbol prefix
            // and no length field of its own, since the frame's own
            // payload_length already says exactly how many bytes there are.
            // The empty-string fallback avoids ever calling assign() with a
            // possibly-null `payload` at length 0.
            out.token.assign(length == 0 ? "" : reinterpret_cast<const char*>(payload), length);
            return true;
        case MessageType::Subscribe:
            if (length != kSubscribeBytes) return false;
            out.symbol = getSymbol(payload);
            return !out.symbol.empty();
        case MessageType::Unsubscribe:
            if (length != kUnsubscribeBytes) return false;
            out.symbol = getSymbol(payload);
            return !out.symbol.empty();
        case MessageType::Response:
        case MessageType::MarketData:
            return false;  // server -> client only, never a client request
    }
    return false;
}

bool decodeResponse(std::uint32_t correlation_id, const std::uint8_t* payload, std::size_t length,
                    Response& out) {
    out = Response{};
    out.correlation_id = correlation_id;
    if (length < kResponseFixedBytes) return false;
    if (!reasonFromByte(payload[0], out.reason)) return false;
    out.cancelled_by_self_trade = payload[1] != 0;
    const std::uint32_t trade_count = getU32(payload + 2);
    out.unfilled = getU64(payload + 6);

    if (length != kResponseFixedBytes + static_cast<std::size_t>(trade_count) * kTradeBytes) {
        return false;
    }
    out.trades.reserve(trade_count);
    const std::uint8_t* p = payload + kResponseFixedBytes;
    for (std::uint32_t i = 0; i < trade_count; ++i) {
        Trade trade{};
        trade.buy_order_id = getU64(p);
        trade.sell_order_id = getU64(p + 8);
        trade.price = getI64(p + 16);
        trade.quantity = getU64(p + 24);
        out.trades.push_back(trade);
        p += kTradeBytes;
    }
    return true;
}

bool encodeMarketData(const MarketDataMessage& message, std::vector<std::uint8_t>& out) {
    const std::size_t header_at = out.size();
    out.resize(header_at + kHeaderBytes);

    if (!putSymbol(out, message.symbol)) {
        out.resize(header_at);
        return false;
    }
    putU64(out, message.sequence);
    putU8(out, message.best_bid.has_value() ? 1 : 0);
    putI64(out, message.best_bid.value_or(0));
    putU8(out, message.best_ask.has_value() ? 1 : 0);
    putI64(out, message.best_ask.value_or(0));
    putU32(out, static_cast<std::uint32_t>(message.trades.size()));
    for (const Trade& trade : message.trades) {
        putU64(out, trade.buy_order_id);
        putU64(out, trade.sell_order_id);
        putI64(out, trade.price);
        putU64(out, trade.quantity);
    }

    writeHeader(out, header_at, MessageType::MarketData, message.correlation_id);
    return true;
}

bool decodeMarketData(std::uint32_t correlation_id, const std::uint8_t* payload, std::size_t length,
                      MarketDataMessage& out) {
    out = MarketDataMessage{};
    out.correlation_id = correlation_id;
    if (length < kMarketDataFixedBytes) return false;

    out.symbol = getSymbol(payload);
    if (out.symbol.empty()) return false;
    out.sequence = getU64(payload + 8);

    const bool has_bid = payload[16] != 0;
    const Price bid_price = getI64(payload + 17);
    if (has_bid) out.best_bid = bid_price;

    const bool has_ask = payload[25] != 0;
    const Price ask_price = getI64(payload + 26);
    if (has_ask) out.best_ask = ask_price;

    const std::uint32_t trade_count = getU32(payload + 34);
    if (length != kMarketDataFixedBytes + static_cast<std::size_t>(trade_count) * kTradeBytes) {
        return false;
    }
    out.trades.reserve(trade_count);
    const std::uint8_t* p = payload + kMarketDataFixedBytes;
    for (std::uint32_t i = 0; i < trade_count; ++i) {
        Trade trade{};
        trade.buy_order_id = getU64(p);
        trade.sell_order_id = getU64(p + 8);
        trade.price = getI64(p + 16);
        trade.quantity = getU64(p + 24);
        out.trades.push_back(trade);
        p += kTradeBytes;
    }
    return true;
}

void FrameReader::append(const std::uint8_t* data, std::size_t length) {
    buffer_.insert(buffer_.end(), data, data + length);
}

void FrameReader::compact() {
    // Drop bytes already handed out, so a long-lived connection doesn't grow a
    // buffer of everything it has ever received.
    if (consumed_ == 0) return;
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
    consumed_ = 0;
}

bool FrameReader::next(MessageType& type, std::uint32_t& correlation_id,
                       std::vector<std::uint8_t>& payload) {
    if (failed_) return false;
    if (buffered() < kHeaderBytes) {
        compact();
        return false;
    }

    const std::uint8_t* header = buffer_.data() + consumed_;
    const std::uint32_t payload_length = getU32(header);
    const std::uint32_t correlation = getU32(header + 4);
    const std::uint8_t raw_type = header[8];
    const std::uint8_t version = header[9];

    if (version != kWireVersion) {
        failed_ = true;
        failure_reason_ = "unsupported protocol version";
        return false;
    }
    if (payload_length > kMaxPayloadBytes) {
        failed_ = true;
        failure_reason_ = "payload exceeds maximum frame size";
        return false;
    }

    if (buffered() < kHeaderBytes + payload_length) {
        compact();
        return false;  // frame still in flight
    }

    const std::uint8_t* body = header + kHeaderBytes;
    payload.assign(body, body + payload_length);
    type = static_cast<MessageType>(raw_type);
    correlation_id = correlation;
    consumed_ += kHeaderBytes + payload_length;
    return true;
}

Response applyRequest(const Request& request, MatchingEngine& engine) {
    Response response;
    response.correlation_id = request.correlation_id;

    switch (request.type) {
        case MessageType::AddSymbol:
            // addSymbol has no SubmitResult; report a duplicate or malformed
            // symbol as a rejection so the client always gets a verdict.
            response.reason =
                engine.addSymbol(request.symbol) ? RejectReason::None : RejectReason::UnknownSymbol;
            return response;
        case MessageType::LimitOrder: {
            const SubmitResult result = engine.addLimitOrder(request.symbol, request.order_id,
                                                             request.side, request.price,
                                                             request.quantity, request.participant);
            response.reason = result.reason;
            response.unfilled = result.unfilled;
            response.cancelled_by_self_trade = result.cancelled_by_self_trade;
            response.trades = result.trades;
            return response;
        }
        case MessageType::MarketOrder: {
            const SubmitResult result = engine.addMarketOrder(
                request.symbol, request.order_id, request.side, request.quantity, request.participant);
            response.reason = result.reason;
            response.unfilled = result.unfilled;
            response.cancelled_by_self_trade = result.cancelled_by_self_trade;
            response.trades = result.trades;
            return response;
        }
        case MessageType::ModifyOrder: {
            const SubmitResult result =
                engine.modifyOrder(request.symbol, request.order_id, request.price, request.quantity);
            response.reason = result.reason;
            response.unfilled = result.unfilled;
            response.cancelled_by_self_trade = result.cancelled_by_self_trade;
            response.trades = result.trades;
            return response;
        }
        case MessageType::CancelOrder:
            response.reason = engine.cancelOrder(request.symbol, request.order_id)
                                  ? RejectReason::None
                                  : RejectReason::UnknownOrder;
            return response;
        case MessageType::Response:
            response.reason = RejectReason::UnknownOrder;
            return response;
        case MessageType::Authenticate:
        case MessageType::Subscribe:
        case MessageType::Unsubscribe:
        case MessageType::MarketData:
            // None of these ever actually reach here in the real server:
            // OrderServer intercepts Authenticate, Subscribe and Unsubscribe
            // before calling applyRequest, since the engine has no notion of
            // connections, credentials or subscriptions, and MarketData is
            // never sent by a client at all. Kept exhaustive and given the
            // same fallback as Response above, rather than a default:, so a
            // real future request type here trips -Wswitch instead of
            // silently reusing this branch.
            response.reason = RejectReason::UnknownOrder;
            return response;
    }
    return response;
}

}  // namespace matching_engine
