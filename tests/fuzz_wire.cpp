// Fuzz target for the wire protocol's parsing path.
//
// FrameReader and decodeRequest are the only code here that reads bytes chosen
// by someone else: anyone who can open a TCP connection to the server feeds
// them directly. The hand-written tests cover the malformed inputs I thought
// to imagine, which is exactly the wrong basis for deciding a parser is safe.
//
// Two entry points, because Apple clang ships no libFuzzer runtime:
//   - LLVMFuzzerTestOneInput, used by -fsanitize=fuzzer in CI.
//   - a standalone main (-DSTANDALONE_FUZZ_DRIVER) that replays a corpus of
//     generated and mutated inputs, so the harness can be run under
//     ASan/UBSan on any toolchain.
//
// The target must be deterministic and must not exit on bad input: rejecting
// a malformed frame is correct behaviour, and only a crash, a hang, or a
// sanitizer report is a finding.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "matching_engine.hpp"
#include "wire.hpp"

using namespace matching_engine;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FrameReader reader;
    reader.append(data, size);

    // A fresh engine per input keeps runs independent, so a crash reproduces
    // from the single input that caused it rather than from a history.
    MatchingEngine engine;
    engine.addSymbol("AAPL");
    engine.addSymbol("MSFT");

    MessageType type;
    std::uint32_t correlation_id = 0;
    std::vector<std::uint8_t> payload;

    // Bound the loop independently of the reader: a decode bug that failed to
    // consume input would otherwise spin here rather than being reported.
    int frames = 0;
    while (frames++ < 1000 && reader.next(type, correlation_id, payload)) {
        Request request;
        if (decodeRequest(type, correlation_id, payload.data(), payload.size(), request)) {
            // Exercise the engine too: a decoded request is about to be acted
            // on for real, so the interesting question is whether anything it
            // can express breaks the book.
            const Response response = applyRequest(request, engine);

            // And that responses re-encode and decode cleanly, which is what a
            // client on the other end depends on.
            std::vector<std::uint8_t> encoded;
            encodeResponse(response, encoded);
            if (encoded.size() > kHeaderBytes) {
                Response decoded;
                decodeResponse(response.correlation_id, encoded.data() + kHeaderBytes,
                               encoded.size() - kHeaderBytes, decoded);
            }
        }

        // The same payload read as a response, since a malicious peer can send
        // any message type in either direction.
        Response as_response;
        decodeResponse(correlation_id, payload.data(), payload.size(), as_response);
    }

    return 0;
}

#ifdef STANDALONE_FUZZ_DRIVER

#include <cstdio>
#include <random>

namespace {

// A valid frame, so mutation starts from something that parses rather than
// from noise that is rejected at the first byte.
std::vector<std::uint8_t> seedFrame(MessageType type) {
    Request request;
    request.type = type;
    request.correlation_id = 7;
    request.symbol = "AAPL";
    request.order_id = 42;
    request.side = Side::Buy;
    request.price = 100'50;
    request.quantity = 9;
    request.participant = 3;
    std::vector<std::uint8_t> bytes;
    encodeRequest(request, bytes);
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    const int iterations = (argc > 1) ? std::atoi(argv[1]) : 20000;
    std::mt19937 rng(12345);  // fixed seed: a failure here reproduces exactly

    const std::vector<MessageType> types = {MessageType::AddSymbol,   MessageType::LimitOrder,
                                            MessageType::MarketOrder, MessageType::ModifyOrder,
                                            MessageType::CancelOrder, MessageType::Response};

    std::vector<std::vector<std::uint8_t>> corpus;
    for (MessageType type : types) corpus.push_back(seedFrame(type));
    // Several frames back to back, which is how they actually arrive.
    std::vector<std::uint8_t> batched;
    for (const auto& frame : corpus) batched.insert(batched.end(), frame.begin(), frame.end());
    corpus.push_back(batched);
    corpus.push_back({});  // empty input
    corpus.push_back(std::vector<std::uint8_t>(kHeaderBytes - 1, 0xAB));  // partial header

    for (int i = 0; i < iterations; ++i) {
        std::vector<std::uint8_t> input = corpus[rng() % corpus.size()];

        // Mutate: flip bytes, truncate, or extend. Truncation matters most --
        // it is what a torn frame on a real socket looks like.
        const int mutation = static_cast<int>(rng() % 4);
        if (mutation == 0 && !input.empty()) {
            input[rng() % input.size()] = static_cast<std::uint8_t>(rng() % 256);
        } else if (mutation == 1 && !input.empty()) {
            input.resize(rng() % input.size());
        } else if (mutation == 2) {
            for (int n = static_cast<int>(rng() % 32); n > 0; --n) {
                input.push_back(static_cast<std::uint8_t>(rng() % 256));
            }
        } else if (!input.empty()) {
            for (int n = static_cast<int>(rng() % 4) + 1; n > 0; --n) {
                input[rng() % input.size()] = static_cast<std::uint8_t>(rng() % 256);
            }
        }

        LLVMFuzzerTestOneInput(input.data(), input.size());
    }

    std::printf("standalone fuzz driver: %d inputs, no crash\n", iterations);
    return 0;
}

#endif  // STANDALONE_FUZZ_DRIVER
