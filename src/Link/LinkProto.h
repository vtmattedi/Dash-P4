#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>

// DashLink wire format, panel side (pc/PROTOCOL.md is the specification; pc/dashlink/protocol.py is the PC side).
// Pure code, no sockets and no ESP-IDF beyond mbedTLS for the HMAC, so the host tests cover it.
//
//   frame = u32 length (little endian) | type (one ASCII letter) | payload (a JSON object, may be empty)
//   `length` counts the type byte and the payload; 1..kMaxFrame.

namespace LinkProto
{
constexpr uint16_t kPort = 7340;
constexpr size_t kMaxFrame = 16 * 1024;
constexpr int kVersion = 0;

constexpr char kChallenge = 'C';
constexpr char kHello = 'H';
constexpr char kWelcome = 'W';
constexpr char kError = 'E';
constexpr char kPing = 'P';
constexpr char kPong = 'O';
constexpr char kState = 'S';
constexpr char kAction = 'A';
constexpr char kResult = 'R';

/// One complete frame, ready to send.
std::string encodeFrame(char type, const std::string &json = std::string());

/// Cuts a TCP byte stream into frames: feed it whatever recv() returned, then call next() until it says NeedMore.
class FrameParser
{
public:
    enum class Result
    {
        NeedMore,
        Frame,
        Error // a frame length outside 1..kMaxFrame; the stream cannot be resynchronised, close the connection
    };

    void feed(const uint8_t *data, size_t length);
    Result next(char &type, std::string &json);
    void reset();

private:
    std::string m_buffer;
    size_t m_start = 0;
    bool m_failed = false;
};

/// HMAC-SHA256, raw 32 bytes.
std::string hmacSha256(const std::string &key, const std::string &message);
std::string toHex(const std::string &bytes);

/// hex(HMAC-SHA256(key = token, message = nonce + "|" + name)), the proof in the PC's HELLO.
std::string proof(const std::string &token, const std::string &nonce, const std::string &name);

/// Constant-time comparison of the PC's proof with the expected one.
bool verifyProof(const std::string &token, const std::string &nonce, const std::string &name, const std::string &given);
} // namespace LinkProto
