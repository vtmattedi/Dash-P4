#include "LinkProto.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "mbedtls/md.h"
#endif

namespace LinkProto
{
std::string encodeFrame(char type, const std::string &json)
{
    const uint32_t length = static_cast<uint32_t>(1 + json.size());
    std::string out;
    out.reserve(4 + length);
    out.push_back(static_cast<char>(length & 0xFF));
    out.push_back(static_cast<char>((length >> 8) & 0xFF));
    out.push_back(static_cast<char>((length >> 16) & 0xFF));
    out.push_back(static_cast<char>((length >> 24) & 0xFF));
    out.push_back(type);
    out += json;
    return out;
}

void FrameParser::feed(const uint8_t *data, size_t length)
{
    if (m_failed)
    {
        return;
    }
    if (m_start > 0 && m_start >= m_buffer.size())
    {
        m_buffer.clear();
        m_start = 0;
    }
    else if (m_start > 4096)
    {
        m_buffer.erase(0, m_start);
        m_start = 0;
    }
    m_buffer.append(reinterpret_cast<const char *>(data), length);
}

FrameParser::Result FrameParser::next(char &type, std::string &json)
{
    if (m_failed)
    {
        return Result::Error;
    }
    const size_t available = m_buffer.size() - m_start;
    if (available < 4)
    {
        return Result::NeedMore;
    }
    const uint8_t *p = reinterpret_cast<const uint8_t *>(m_buffer.data()) + m_start;
    const uint32_t length = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                            (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    if (length < 1 || length > kMaxFrame)
    {
        m_failed = true;
        return Result::Error;
    }
    if (available < 4 + static_cast<size_t>(length))
    {
        return Result::NeedMore;
    }
    type = static_cast<char>(p[4]);
    json.assign(reinterpret_cast<const char *>(p) + 5, length - 1);
    m_start += 4 + length;
    return Result::Frame;
}

void FrameParser::reset()
{
    m_buffer.clear();
    m_start = 0;
    m_failed = false;
}

// ------------------------------------------------------------------------------------------------ HMAC-SHA256

#ifdef ESP_PLATFORM
std::string hmacSha256(const std::string &key, const std::string &message)
{
    unsigned char out[32] = {0};
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr ||
        mbedtls_md_hmac(info, reinterpret_cast<const unsigned char *>(key.data()), key.size(),
                        reinterpret_cast<const unsigned char *>(message.data()), message.size(), out) != 0)
    {
        return std::string(); // an empty result never equals a proof, so a failure refuses the PC
    }
    return std::string(reinterpret_cast<const char *>(out), 32);
}
#else
// The host tests have no mbedTLS: a plain FIPS 180-4 SHA-256 and RFC 2104 HMAC, checked against RFC 4231 in the tests.
namespace
{
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block[64];
    size_t blockLength = 0;
    uint64_t total = 0;

    void compress(const uint8_t *b)
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
        {
            w[i] = (static_cast<uint32_t>(b[i * 4]) << 24) | (static_cast<uint32_t>(b[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(b[i * 4 + 2]) << 8) | static_cast<uint32_t>(b[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & bb) ^ (a & c) ^ (bb & c);
            const uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = bb;
            bb = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += bb;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    void update(const uint8_t *data, size_t length)
    {
        total += length;
        while (length > 0)
        {
            const size_t take = (64 - blockLength < length) ? 64 - blockLength : length;
            memcpy(block + blockLength, data, take);
            blockLength += take;
            data += take;
            length -= take;
            if (blockLength == 64)
            {
                compress(block);
                blockLength = 0;
            }
        }
    }

    std::string finish()
    {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (blockLength != 56)
        {
            update(&zero, 1);
        }
        uint8_t lengthBytes[8];
        for (int i = 0; i < 8; ++i)
        {
            lengthBytes[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        }
        update(lengthBytes, 8);
        std::string out(32, '\0');
        for (int i = 0; i < 8; ++i)
        {
            out[i * 4] = static_cast<char>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<char>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<char>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<char>(h[i]);
        }
        return out;
    }
};

std::string sha256(const std::string &data)
{
    Sha256 s;
    s.update(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    return s.finish();
}
} // namespace

std::string hmacSha256(const std::string &keyIn, const std::string &message)
{
    std::string key = keyIn.size() > 64 ? sha256(keyIn) : keyIn;
    key.resize(64, '\0');
    std::string inner(64, '\0'), outer(64, '\0');
    for (size_t i = 0; i < 64; ++i)
    {
        inner[i] = static_cast<char>(key[i] ^ 0x36);
        outer[i] = static_cast<char>(key[i] ^ 0x5c);
    }
    return sha256(outer + sha256(inner + message));
}
#endif

std::string toHex(const std::string &bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes)
    {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0F]);
    }
    return out;
}

std::string proof(const std::string &token, const std::string &nonce, const std::string &name)
{
    return toHex(hmacSha256(token, nonce + "|" + name));
}

bool verifyProof(const std::string &token, const std::string &nonce, const std::string &name, const std::string &given)
{
    const std::string expected = proof(token, nonce, name);
    if (expected.size() != 64 || given.size() != expected.size())
    {
        return false;
    }
    unsigned char difference = 0;
    for (size_t i = 0; i < expected.size(); ++i)
    {
        difference |= static_cast<unsigned char>(expected[i] ^ given[i]);
    }
    return difference == 0;
}
} // namespace LinkProto
