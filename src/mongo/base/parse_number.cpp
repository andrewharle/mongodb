
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/parse_number.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/overflow_arithmetic.h"

namespace mongo {
namespace {

/**
 * Returns the value of the digit "c", with the same conversion behavior as strtol.
 *
 * Assumes "c" is an ASCII character or UTF-8 octet.
 */
uint8_t _digitValue(char c) {
    if (c >= '0' && c <= '9')
        return uint8_t(c - '0');
    if (c >= 'a' && c <= 'z')
        return uint8_t(c - 'a' + 10);
    if (c >= 'A' && c <= 'Z')
        return uint8_t(c - 'A' + 10);
    return 36;  // Illegal digit value for all supported bases.
}

/**
 * Assuming "stringValue" represents a parseable number, extracts the sign and returns a
 * substring with any sign characters stripped away.  "*isNegative" is set to true if the
 * number is negative, and false otherwise.
 */
inline StringData _extractSign(StringData stringValue, bool* isNegative) {
    if (stringValue.empty()) {
        *isNegative = false;
        return stringValue;
    }

    bool foundSignMarker;
    switch (stringValue[0]) {
        case '-':
            foundSignMarker = true;
            *isNegative = true;
            break;
        case '+':
            foundSignMarker = true;
            *isNegative = false;
            break;
        default:
            foundSignMarker = false;
            *isNegative = false;
            break;
    }

    if (foundSignMarker)
        return stringValue.substr(1);
    return stringValue;
}

/**
 * Assuming "stringValue" represents a parseable number, determines what base to use given
 * "inputBase".  Stores the correct base into "*outputBase".  Follows strtol rules.  If
 * "inputBase" is not 0, *outputBase is set to "inputBase".  Otherwise, if "stringValue" starts
 * with "0x" or "0X", sets outputBase to 16, or if it starts with 0, sets outputBase to 8.
 *
 * Returns stringValue, unless it sets *outputBase to 16, in which case it will strip off the
 * "0x" or "0X" prefix, if present.
 */
inline StringData _extractBase(StringData stringValue, int inputBase, int* outputBase) {
    const auto hexPrefixLower = "0x"_sd;
    const auto hexPrefixUpper = "0X"_sd;
    if (inputBase == 0) {
        if (stringValue.size() > 2 &&
            (stringValue.startsWith(hexPrefixLower) || stringValue.startsWith(hexPrefixUpper))) {
            *outputBase = 16;
            return stringValue.substr(2);
        }
        if (stringValue.size() > 1 && stringValue[0] == '0') {
            *outputBase = 8;
            return stringValue;
        }
        *outputBase = 10;
        return stringValue;
    } else {
        *outputBase = inputBase;
        if (inputBase == 16 &&
            (stringValue.startsWith(hexPrefixLower) || stringValue.startsWith(hexPrefixUpper))) {
            return stringValue.substr(2);
        }
        return stringValue;
    }
}

inline StatusWith<uint64_t> parseMagnitudeFromStringWithBase(uint64_t base,
                                                             StringData wholeString,
                                                             StringData magnitudeStr) {
    uint64_t n = 0;
    for (char digitChar : magnitudeStr) {
        const uint64_t digitValue = _digitValue(digitChar);
        if (digitValue >= base) {
            return Status(ErrorCodes::FailedToParse,
                          std::string("Bad digit \"") + digitChar + "\" while parsing " +
                              wholeString);
        }

        // This block is (n = (n * base) + digitValue) with overflow checking at each step.
        uint64_t multiplied;
        if (mongoUnsignedMultiplyOverflow64(n, base, &multiplied))
            return Status(ErrorCodes::FailedToParse, "Overflow");
        if (mongoUnsignedAddOverflow64(multiplied, digitValue, &n))
            return Status(ErrorCodes::FailedToParse, "Overflow");
    }
    return n;
}

}  // namespace

template <typename NumberType>
Status parseNumberFromStringWithBase(StringData wholeString, int base, NumberType* result) {
    MONGO_STATIC_ASSERT(sizeof(NumberType) <= sizeof(uint64_t));
    typedef ::std::numeric_limits<NumberType> limits;

    if (base == 1 || base < 0 || base > 36)
        return Status(ErrorCodes::BadValue, "Invalid base");

    // Separate the magnitude from modifiers such as sign and base prefixes such as "0x"
    bool isNegative = false;
    StringData magnitudeStr = _extractBase(_extractSign(wholeString, &isNegative), base, &base);
    if (isNegative && !limits::is_signed)
        return Status(ErrorCodes::FailedToParse, "Negative value");
    if (magnitudeStr.empty())
        return Status(ErrorCodes::FailedToParse, "No digits");

    auto status = parseMagnitudeFromStringWithBase(base, wholeString, magnitudeStr);
    if (!status.isOK())
        return status.getStatus();
    uint64_t magnitude = status.getValue();

    // The range of 2's complement integers is from -(max + 1) to +max.
    const uint64_t maxMagnitude = uint64_t(limits::max()) + (isNegative ? 1u : 0u);
    if (magnitude > maxMagnitude)
        return Status(ErrorCodes::FailedToParse, "Overflow");

#pragma warning(push)
// C4146: unary minus operator applied to unsigned type, result still unsigned
#pragma warning(disable : 4146)
    *result = NumberType(isNegative ? -magnitude : magnitude);
#pragma warning(pop)

    return Status::OK();
}

// Definition of the various supported implementations of parseNumberFromStringWithBase.

#define DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(NUMBER_TYPE) \
    template Status parseNumberFromStringWithBase<NUMBER_TYPE>(StringData, int, NUMBER_TYPE*);

DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(long)
DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(long long)
DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(unsigned long)
DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(unsigned long long)
DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(short)
DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(unsigned short)
DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(int)
DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(unsigned int)
DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(int8_t);
DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(uint8_t);
#undef DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE

#ifdef _WIN32

namespace {

/**
 * Converts ascii c-locale uppercase characters to lower case, leaves other char values
 * unchanged.
 */
char toLowerAscii(char c) {
    if (isascii(c) && isupper(c))
        return _tolower(c);
    return c;
}

}  // namespace

#endif  // defined(_WIN32)

template <>
Status parseNumberFromStringWithBase<double>(StringData stringValue, int base, double* result) {
    if (base != 0) {
        return Status(ErrorCodes::BadValue,
                      "Must pass 0 as base to parseNumberFromStringWithBase<double>.");
    }
    if (stringValue.empty())
        return Status(ErrorCodes::FailedToParse, "Empty string");

    if (isspace(stringValue[0]))
        return Status(ErrorCodes::FailedToParse, "Leading whitespace");

    std::string str = stringValue.toString();
    const char* cStr = str.c_str();
    char* endp;
    errno = 0;
    double d = strtod(cStr, &endp);
    int actualErrno = errno;
    if (endp != stringValue.size() + cStr) {
#ifdef _WIN32
        // The Windows libc implementation of strtod cannot parse +/-infinity or nan,
        // so handle that here.
        std::transform(str.begin(), str.end(), str.begin(), toLowerAscii);
        if (str == "nan"_sd) {
            *result = std::numeric_limits<double>::quiet_NaN();
            return Status::OK();
        } else if (str == "+infinity"_sd || str == "infinity"_sd) {
            *result = std::numeric_limits<double>::infinity();
            return Status::OK();
        } else if (str == "-infinity"_sd) {
            *result = -std::numeric_limits<double>::infinity();
            return Status::OK();
        }
#endif  // defined(_WIN32)

        return Status(ErrorCodes::FailedToParse, "Did not consume whole number.");
    }
    if (actualErrno == ERANGE)
        return Status(ErrorCodes::FailedToParse, "Out of range");
    *result = d;
    return Status::OK();
}

template <>
Status parseNumberFromStringWithBase<Decimal128>(StringData stringValue,
                                                 int base,
                                                 Decimal128* result) {
    if (base != 0) {
        return Status(ErrorCodes::BadValue,
                      "Must pass 0 as base to parseNumberFromStringWithBase<Decimal128>.");
    }

    if (stringValue.empty()) {
        return Status(ErrorCodes::FailedToParse, "Empty string");
    }

    std::uint32_t signalingFlags = 0;
    auto parsedDecimal = Decimal128(
        stringValue.toString(), &signalingFlags, Decimal128::RoundingMode::kRoundTowardZero);

    if (Decimal128::hasFlag(signalingFlags, Decimal128::SignalingFlag::kOverflow)) {
        return Status(ErrorCodes::FailedToParse,
                      "Conversion from string to decimal would overflow");
    } else if (Decimal128::hasFlag(signalingFlags, Decimal128::SignalingFlag::kUnderflow)) {
        return Status(ErrorCodes::FailedToParse,
                      "Conversion from string to decimal would underflow");
    } else if (signalingFlags != Decimal128::SignalingFlag::kNoFlag &&
               signalingFlags != Decimal128::SignalingFlag::kInexact) {  // Ignore precision loss.
        return Status(ErrorCodes::FailedToParse, "Failed to parse string to decimal");
    }

    *result = parsedDecimal;
    return Status::OK();
}
}  // namespace mongo
