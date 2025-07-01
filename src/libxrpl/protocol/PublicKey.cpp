//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/basics/contract.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/detail/secp256k1.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/tokens.h>

#include <boost/multiprecision/fwd.hpp>
#include <boost/multiprecision/number.hpp>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>

#include <ed25519.h>
#include <secp256k1.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <ostream>
#include <string>

namespace ripple {

std::ostream&
operator<<(std::ostream& os, PublicKey const& pk)
{
    os << strHex(pk);
    return os;
}

template <>
std::optional<PublicKey>
parseBase58(TokenType type, std::string const& s)
{
    auto const result = decodeBase58Token(s, type);
    auto const pks = makeSlice(result);
    if (!publicKeyType(pks))
        return std::nullopt;
    return PublicKey(pks);
}

//------------------------------------------------------------------------------

// Parse a length-prefixed number
//  Format: 0x02 <length-byte> <number>
static std::optional<Slice>
sigPart(Slice& buf)
{
    if (buf.size() < 3 || buf[0] != 0x02)
        return std::nullopt;
    auto const len = buf[1];
    buf += 2;
    if (len > buf.size() || len < 1 || len > 33)
        return std::nullopt;
    // Can't be negative
    if ((buf[0] & 0x80) != 0)
        return std::nullopt;
    if (buf[0] == 0)
    {
        // Can't be zero
        if (len == 1)
            return std::nullopt;
        // Can't be padded
        if ((buf[1] & 0x80) == 0)
            return std::nullopt;
    }
    std::optional<Slice> number = Slice(buf.data(), len);
    buf += len;
    return number;
}

static std::string
sliceToHex(Slice const& slice)
{
    std::string s;
    if (slice[0] & 0x80)
    {
        s.reserve(2 * (slice.size() + 2));
        s = "0x00";
    }
    else
    {
        s.reserve(2 * (slice.size() + 1));
        s = "0x";
    }
    for (int i = 0; i < slice.size(); ++i)
    {
        s += "0123456789ABCDEF"[((slice[i] & 0xf0) >> 4)];
        s += "0123456789ABCDEF"[((slice[i] & 0x0f) >> 0)];
    }
    return s;
}

/** Determine whether a signature is canonical.
    Canonical signatures are important to protect against signature morphing
    attacks.
    @param vSig the signature data
    @param sigLen the length of the signature
    @param strict_param whether to enforce strictly canonical semantics

    @note For more details please see:
    https://xrpl.org/transaction-malleability.html
    https://bitcointalk.org/index.php?topic=8392.msg127623#msg127623
    https://github.com/sipa/bitcoin/commit/58bc86e37fda1aec270bccb3df6c20fbd2a6591c
*/
std::optional<ECDSACanonicality>
ecdsaCanonicality(Slice const& sig)
{
    using uint264 =
        boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
            264,
            264,
            boost::multiprecision::signed_magnitude,
            boost::multiprecision::unchecked,
            void>>;

    static uint264 const G(
        "0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141");

    // The format of a signature should be:
    // <30> <len> [ <02> <lenR> <R> ] [ <02> <lenS> <S> ]
    if ((sig.size() < 8) || (sig.size() > 72))
        return std::nullopt;
    if ((sig[0] != 0x30) || (sig[1] != (sig.size() - 2)))
        return std::nullopt;
    Slice p = sig + 2;
    auto r = sigPart(p);
    auto s = sigPart(p);
    if (!r || !s || !p.empty())
        return std::nullopt;

    uint264 R(sliceToHex(*r));
    if (R >= G)
        return std::nullopt;

    uint264 S(sliceToHex(*s));
    if (S >= G)
        return std::nullopt;

    // (R,S) and (R,G-S) are canonical,
    // but is fully canonical when S <= G-S
    auto const Sp = G - S;
    if (S > Sp)
        return ECDSACanonicality::canonical;
    return ECDSACanonicality::fullyCanonical;
}

static bool
ed25519Canonical(Slice const& sig)
{
    if (sig.size() != 64)
        return false;
    // Big-endian Order, the Ed25519 subgroup order
    std::uint8_t const Order[] = {
        0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0xDE, 0xF9, 0xDE, 0xA2, 0xF7,
        0x9C, 0xD6, 0x58, 0x12, 0x63, 0x1A, 0x5C, 0xF5, 0xD3, 0xED,
    };
    // Take the second half of signature
    // and byte-reverse it to big-endian.
    auto const le = sig.data() + 32;
    std::uint8_t S[32];
    std::reverse_copy(le, le + 32, S);
    // Must be less than Order
    return std::lexicographical_compare(S, S + 32, Order, Order + 32);
}

//------------------------------------------------------------------------------

PublicKey::PublicKey(Slice const& slice)
{
    if (slice.size() < size_)
        LogicError(
            "PublicKey::PublicKey - Input slice cannot be an undersized "
            "buffer");

    if (!publicKeyType(slice))
        LogicError("PublicKey::PublicKey invalid type");
    size_ = slice.size();
    std::memcpy(buf_, slice.data(), size_);
}

PublicKey::PublicKey(PublicKey const& other) : size_(other.size_)
{
    if (size_)
        std::memcpy(buf_, other.buf_, size_);
}

PublicKey&
PublicKey::operator=(PublicKey const& other)
{
    if (this != &other)
    {
        size_ = other.size_;
        if (size_)
            std::memcpy(buf_, other.buf_, size_);
    }

    return *this;
}

//------------------------------------------------------------------------------

std::optional<KeyType>
publicKeyType(Slice const& slice)
{
    if (slice.size() == 33)
    {
        if (slice[0] == 0xED)
            return KeyType::ed25519;

        if (slice[0] == 0x02 || slice[0] == 0x03)
            return KeyType::secp256k1;
    }

    if (slice.size() == 65)
    {
        return KeyType::p256;
    }

    return std::nullopt;
}

bool
verifyDigest(
    PublicKey const& publicKey,
    uint256 const& digest,
    Slice const& sig,
    bool mustBeFullyCanonical) noexcept
{
    if (publicKeyType(publicKey) != KeyType::secp256k1)
        LogicError("sign: secp256k1 required for digest signing");
    auto const canonicality = ecdsaCanonicality(sig);
    if (!canonicality)
        return false;
    if (mustBeFullyCanonical &&
        (*canonicality != ECDSACanonicality::fullyCanonical))
        return false;

    secp256k1_pubkey pubkey_imp;
    if (secp256k1_ec_pubkey_parse(
            secp256k1Context(),
            &pubkey_imp,
            reinterpret_cast<unsigned char const*>(publicKey.data()),
            publicKey.size()) != 1)
        return false;

    secp256k1_ecdsa_signature sig_imp;
    if (secp256k1_ecdsa_signature_parse_der(
            secp256k1Context(),
            &sig_imp,
            reinterpret_cast<unsigned char const*>(sig.data()),
            sig.size()) != 1)
        return false;
    if (*canonicality != ECDSACanonicality::fullyCanonical)
    {
        secp256k1_ecdsa_signature sig_norm;
        if (secp256k1_ecdsa_signature_normalize(
                secp256k1Context(), &sig_norm, &sig_imp) != 1)
            return false;
        return secp256k1_ecdsa_verify(
                   secp256k1Context(),
                   &sig_norm,
                   reinterpret_cast<unsigned char const*>(digest.data()),
                   &pubkey_imp) == 1;
    }
    return secp256k1_ecdsa_verify(
               secp256k1Context(),
               &sig_imp,
               reinterpret_cast<unsigned char const*>(digest.data()),
               &pubkey_imp) == 1;
}

struct ECDSASignature
{
    std::array<uint8_t, 32> r;
    std::array<uint8_t, 32> s;
};

std::optional<ECDSASignature>
parseDERSignature(Slice const& derSig) noexcept
{
    if (derSig.size() < 8)
        return std::nullopt;

    uint8_t const* data = derSig.data();
    size_t offset = 0;

    // Check sequence tag
    if (data[offset++] != 0x30)
        return std::nullopt;

    // Skip total length
    offset++;

    // Parse R
    if (data[offset++] != 0x02)
        return std::nullopt;
    uint8_t rLen = data[offset++];
    if (offset + rLen >= derSig.size())
        return std::nullopt;

    ECDSASignature result{};

    // Copy R, handling leading zeros
    int rStart = (rLen > 32 && data[offset] == 0x00) ? 1 : 0;
    int rCopyLen = std::min(32, static_cast<int>(rLen - rStart));
    std::memcpy(
        result.r.data() + (32 - rCopyLen), data + offset + rStart, rCopyLen);
    offset += rLen;

    // Parse S
    if (data[offset++] != 0x02)
        return std::nullopt;
    uint8_t sLen = data[offset++];
    if (offset + sLen > derSig.size())
        return std::nullopt;

    // Copy S, handling leading zeros
    int sStart = (sLen > 32 && data[offset] == 0x00) ? 1 : 0;
    int sCopyLen = std::min(32, static_cast<int>(sLen - sStart));
    std::memcpy(
        result.s.data() + (32 - sCopyLen), data + offset + sStart, sCopyLen);

    return result;
}

bool
verifyP256ECDSA(
    uint8_t const* hash,
    size_t hashLen,
    uint8_t const* r,
    size_t rLen,
    uint8_t const* s,
    size_t sLen,
    uint8_t const* x,
    size_t xLen,
    uint8_t const* y,
    size_t yLen) noexcept
{
    if (hashLen != 32 || rLen > 32 || sLen > 32 || xLen > 32 || yLen > 32)
        return false;

    // Create curve object
    EC_GROUP* group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    if (!group)
        return false;

    // Set group to EC_KEY
    EC_KEY* key = EC_KEY_new();
    if (!key)
    {
        EC_GROUP_free(group);
        return false;
    }
    EC_KEY_set_group(key, group);

    // Restore public key point from coordinates
    EC_POINT* point = EC_POINT_new(group);
    BIGNUM* bn_x = BN_bin2bn(x, xLen, nullptr);
    BIGNUM* bn_y = BN_bin2bn(y, yLen, nullptr);

    bool success = false;
    if (point && bn_x && bn_y &&
        EC_POINT_set_affine_coordinates_GFp(
            group, point, bn_x, bn_y, nullptr) == 1 &&
        EC_KEY_set_public_key(key, point) == 1)
    {
        // Pack r/s into ECDSA_SIG structure
        ECDSA_SIG* sig = ECDSA_SIG_new();
        BIGNUM* bn_r = BN_bin2bn(r, rLen, nullptr);
        BIGNUM* bn_s = BN_bin2bn(s, sLen, nullptr);

        if (sig && bn_r && bn_s && ECDSA_SIG_set0(sig, bn_r, bn_s) == 1)
        {
            // Verify (ECDSA_SIG_set0 takes ownership of bn_r, bn_s)
            int verified = ECDSA_do_verify(hash, hashLen, sig, key);
            success = (verified == 1);
            bn_r = nullptr;  // ownership transferred
            bn_s = nullptr;  // ownership transferred
        }

        ECDSA_SIG_free(sig);
        if (bn_r)
            BN_free(bn_r);
        if (bn_s)
            BN_free(bn_s);
    }

    EC_POINT_free(point);
    BN_free(bn_x);
    BN_free(bn_y);
    EC_KEY_free(key);
    EC_GROUP_free(group);

    return success;
}

bool
verify(
    PublicKey const& publicKey,
    Slice const& m,
    Slice const& sig,
    bool mustBeFullyCanonical) noexcept
{
    if (auto const type = publicKeyType(publicKey))
    {
        if (*type == KeyType::secp256k1)
        {
            return verifyDigest(
                publicKey, sha512Half(m), sig, mustBeFullyCanonical);
        }
        else if (*type == KeyType::ed25519)
        {
            if (!ed25519Canonical(sig))
                return false;

            // We internally prefix Ed25519 keys with a 0xED
            // byte to distinguish them from secp256k1 keys
            // so when verifying the signature, we need to
            // first strip that prefix.
            return ed25519_sign_open(
                       m.data(), m.size(), publicKey.data() + 1, sig.data()) ==
                0;
        }
        else if (*type == KeyType::p256)
        {
            // Parse DER signature to extract r and s values
            auto parsedSig = parseDERSignature(sig);
            if (!parsedSig)
                return false;

            // Hash the message with SHA-256 (P-256 uses ECDSA-SHA256)
            auto hash = sha256(m);

            // We internally prefix P-256 keys with a prefix byte
            // so strip it to get the raw public key coordinates
            if (publicKey.size() != 65)  // 1 prefix + 32-byte x + 32-byte y
                return false;

            // Extract x and y coordinates (skip prefix byte)
            uint8_t const* x_coord = publicKey.data() + 1;
            uint8_t const* y_coord = publicKey.data() + 33;

            return verifyP256ECDSA(
                hash.data(),
                hash.size(),
                parsedSig->r.data(),
                32,  // r component
                parsedSig->s.data(),
                32,  // s component
                x_coord,
                32,  // x coordinate
                y_coord,
                32);  // y coordinate
        }
    }
    return false;
}

NodeID
calcNodeID(PublicKey const& pk)
{
    static_assert(NodeID::bytes == sizeof(ripesha_hasher::result_type));

    ripesha_hasher h;
    h(pk.data(), pk.size());
    return NodeID{static_cast<ripesha_hasher::result_type>(h)};
}

}  // namespace ripple
