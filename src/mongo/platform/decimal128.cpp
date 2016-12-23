/*    Copyright 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */


#include "mongo/platform/decimal128.h"
#include "mongo/platform/basic.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
// The Intel C library typedefs wchar_t, but it is a distinct fundamental type
// in C++, so we #define _WCHAR_T here to prevent the library from trying to typedef.
#define _WCHAR_T
#include <third_party/IntelRDFPMathLib20U1/LIBRARY/src/bid_conf.h>
#include <third_party/IntelRDFPMathLib20U1/LIBRARY/src/bid_functions.h>
#undef _WCHAR_T

#include "mongo/base/static_assert.h"
#include "mongo/config.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/stringutils.h"

namespace {
void validateInputString(mongo::StringData input, std::uint32_t* signalingFlags) {
    // Input must be of these forms:
    // * Valid decimal (standard or scientific notation):
    //      /[-+]?\d*(.\d+)?([e][+\-]?\d+)?/
    // * NaN: /[-+]?[[Nn][Aa][Nn]]/
    // * Infinity: /[+\-]?(inf|infinity)

    bool isSigned = input[0] == '-' || input[0] == '+';

    // Check for NaN and Infinity
    size_t start = (isSigned) ? 1 : 0;
    mongo::StringData noSign = input.substr(start);
    bool isNanOrInf = noSign == "nan" || noSign == "inf" || noSign == "infinity";
    if (isNanOrInf)
        return;

    // Input starting with non digit
    if (!std::isdigit(noSign[0])) {
        if (noSign[0] != '.') {
            *signalingFlags = mongo::Decimal128::SignalingFlag::kInvalid;
            return;
        } else if (noSign.size() == 1) {
            *signalingFlags = mongo::Decimal128::SignalingFlag::kInvalid;
            return;
        }
    }
    bool isZero = true;
    bool hasCoefficient = false;

    // Check coefficient, i.e. the part before the e
    int dotCount = 0;
    size_t i = 0;
    for (/*i = 0*/; i < noSign.size(); i++) {
        char c = noSign[i];
        if (c == '.') {
            dotCount++;
            if (dotCount > 1) {
                *signalingFlags = mongo::Decimal128::SignalingFlag::kInvalid;
                return;
            }
        } else if (!std::isdigit(c)) {
            break;
        } else {
            hasCoefficient = true;
            if (c != '0') {
                isZero = false;
            }
        }
    }

    if (isZero) {
        // Override inexact/overflow flag set by the intel library
        *signalingFlags = mongo::Decimal128::SignalingFlag::kNoFlag;
    }

    // Input is valid if we've parsed the entire string
    if (i == noSign.size()) {
        return;
    }

    // String with empty coefficient and non-empty exponent
    if (!hasCoefficient) {
        *signalingFlags = mongo::Decimal128::SignalingFlag::kInvalid;
        return;
    }

    // Check exponent
    mongo::StringData exponent = noSign.substr(i);

    if (exponent[0] != 'e' || exponent.size() < 2) {
        *signalingFlags = mongo::Decimal128::SignalingFlag::kInvalid;
        return;
    }
    if (exponent[1] == '-' || exponent[1] == '+') {
        exponent = exponent.substr(2);
        if (exponent.size() == 0) {
            *signalingFlags = mongo::Decimal128::SignalingFlag::kInvalid;
            return;
        }
    } else {
        exponent = exponent.substr(1);
    }

    for (size_t j = 0; j < exponent.size(); j++) {
        char c = exponent[j];
        if (!std::isdigit(c)) {
            *signalingFlags = mongo::Decimal128::SignalingFlag::kInvalid;
            return;
        }
    }
}
}  // namespace

namespace mongo {

namespace {
// Determine system's endian ordering in order to construct decimal 128 values directly
#if MONGO_CONFIG_BYTE_ORDER == 1234
const int kHigh64 = 1;
const int kLow64 = 0;
#else
const int kHigh64 = 0;
const int kLow64 = 1;
#endif

// The Intel library uses long long for BID_UINT128s parts, which on some
// systems is longer than a uint64_t.  We need to cast down, although there
// will not be data loss.
inline Decimal128::Value libraryTypeToValue(BID_UINT128 value) {
    return {static_cast<std::uint64_t>(value.w[kLow64]),
            static_cast<std::uint64_t>(value.w[kHigh64])};
}

/**
 * This helper function creates a library specific type for the
 * IntelRDFPMathLib20U1 library from Decimal128's _value
 */
BID_UINT128 decimal128ToLibraryType(Decimal128::Value value) {
    BID_UINT128 dec128;
    dec128.w[kLow64] = value.low64;
    dec128.w[kHigh64] = value.high64;
    return dec128;
}
}  // namespace

Decimal128::Decimal128(std::int32_t int32Value)
    : _value(libraryTypeToValue(bid128_from_int32(int32Value))) {}

Decimal128::Decimal128(std::int64_t int64Value)
    : _value(libraryTypeToValue(bid128_from_int64(int64Value))) {}

/**
 * Quantize a doubleValue argument to a Decimal128 with exactly 15 digits
 * of precision.
 *
 * To highlight the motivation for this function, consider doubleValue = 0.1.
 * The quantity 0.1 does not have an exact respresentation as a double.
 * The actual value stored in the 64-bit type is 0.1000000000000000055511...
 *
 * Although imprecise, the double type does guarantee a minimum of 15 digits
 * of decimal precision. When casting the double to a decimal type, we choose
 * to only appreciate the double's first 15 digits and round accordingly.
 *
 * To perform this operation, doubleValue is converted to a decimal and then quantized
 * with the appropriate quantum (Q) to yield exactly 15 digits of precision.
 * For example,
 *     doubleValue = 0.1
 *     dec128 = Decimal128(doubleValue)  <== 0.1000000000000000055511151231257827
 *     Q = 1E-15
 *     dec128.quantize(Q)
 *     ==> 0.100000000000000
 *
 * The value to quantize dec128 on (Q) is related to the base 10 exponent of the rounded
 * doubleValue,
 *     Q = 10 ** (floor(log10(doubleValue rounded to 15 decimal digits)) - 14)
 *
 *
 * ===============================================================================
 *
 * Convert a double's base 2 exponent to base 10 using integer arithmetic.
 *
 * Given doubleValue with exponent base2Exp, we would like to find base10Exp such that:
 *   (1) 10**base10Exp > |doubleValue rounded to 15 decimal digits|
 *   (2) 10**(base10Exp-1) <= |doubleValue rounded to 15 decimal digits|
 *
 * Given a double precision number of the form 2**E, we can compute base10Exp such that these
 * conditions hold for 2**E. However, because the absolute value of doubleValue maybe up to a
 * factor of two higher, the required base10Exp may be 1 higher. Exactly knowing in which case we
 * are would require knowing how the double value will round, so just try with the lowest
 * possible base10Exp, and retry if we need to increase the exponent by 1. It is important to first
 * try the lower exponent, as the other way around might unnecessarily lose a significant digit,
 * as in 0.9999999999999994 (15 nines) -> 1.00000000000000 (14 zeros) instead of 0.999999999999999
 * (15 nines).
 *
 *    +-------------+-------------------+----------------------+---------------------------+
 *    | doubleValue |      base2Exp     |  computed base10Exp  | Q                         |
 *    +-------------+-------------------+----------------------+---------------------------+
 *    | 100000      |                16 |                    4 | 10**(5 - 14) <= Retry     |
 *    | 500000      |                18 |                    5 | 10**(5 - 14)              |
 *    | 999999      |                19 |                    5 | 10**(5 - 14)              |
 *    | .00001      |               -17 |                   -6 | 10**(5 - 14) <= Retry     |
 *    | .00005      |               -15 |                   -5 | 10**(5 - 14)              |
 *    | .00009      |               -14 |                   -5 | 10**(5 - 14)              |
 *    +-------------+-------------------+----------------------+---------------------------+
 */
Decimal128::Decimal128(double doubleValue,
                       RoundingPrecision roundPrecision,
                       RoundingMode roundMode) {
    std::uint32_t throwAwayFlag = 0;
    Decimal128 convertedDoubleValue(
        libraryTypeToValue(binary64_to_bid128(doubleValue, roundMode, &throwAwayFlag)));

    // If the original number was zero, infinity, or NaN, there's no need to quantize
    if (doubleValue == 0.0 || std::isinf(doubleValue) || std::isnan(doubleValue) ||
        roundPrecision == kRoundTo34Digits) {
        *this = convertedDoubleValue;
        return;
    }

    // Get the base2 exponent from doubleValue.
    int base2Exp;
    frexp(doubleValue, &base2Exp);

    // As frexp normalizes doubleValue between 0.5 and 1.0 rather than 1.0 and 2.0, adjust.
    base2Exp--;


    // We will use base10Exp = base2Exp * 30103 / (100*1000) as lowerbound (using integer division).
    //
    // This formula is derived from the following, with base2Exp the binary exponent of doubleValue:
    //   (1) 10**(base2Exp * log10(2)) == 2**base2Exp
    //   (2) 0.30103 closely approximates log10(2)
    //
    // Exhaustive testing using Python shows :
    //     { base2Exp * 30103 / (100 * 1000) == math.floor(math.log10(2**base2Exp))
    //       for base2Exp in xrange(-1074, 1023) } == { True }
    int base10Exp = (base2Exp * 30103) / (100 * 1000);

    // As integer division truncates, rather than rounds down (as in Python), adjust accordingly.
    if (base2Exp < 0)
        base10Exp--;

    Decimal128 Q(0, base10Exp - 14 + Decimal128::kExponentBias, 0, 1);
    *this = convertedDoubleValue.quantize(Q, roundMode);

    // Check if the quantization was done correctly: _value stores exactly 15
    // decimal digits of precision (15 digits can fit into the low 64 bits of the decimal)
    uint64_t kSmallest15DigitInt = 1E14;     // A 1 with 14 zeros
    uint64_t kLargest15DigitInt = 1E15 - 1;  // 15 nines
    if (getCoefficientLow() > kLargest15DigitInt) {
        // If we didn't precisely get 15 digits of precision, the original base 10 exponent
        // guess was 1 off, so quantize once more with base10Exp + 1
        Q = Decimal128(0, base10Exp - 13 + Decimal128::kExponentBias, 0, 1);
        *this = convertedDoubleValue.quantize(Q, roundMode);
    }

    // The decimal must have exactly 15 digits of precision
    invariant(getCoefficientHigh() == 0);
    invariant(getCoefficientLow() >= kSmallest15DigitInt);
    invariant(getCoefficientLow() <= kLargest15DigitInt);
}

Decimal128::Decimal128(std::string stringValue, RoundingMode roundMode) {
    std::uint32_t throwAwayFlag = 0;
    *this = Decimal128(stringValue, &throwAwayFlag, roundMode);
}

Decimal128::Decimal128(std::string stringValue,
                       std::uint32_t* signalingFlags,
                       RoundingMode roundMode) {
    std::string lower = toAsciiLowerCase(stringValue);
    BID_UINT128 dec128;
    // The intel library function requires a char * while c_str() returns a const char*.
    // We're using const_cast here since the library function should not modify the input.
    dec128 = bid128_from_string(const_cast<char*>(lower.c_str()), roundMode, signalingFlags);
    validateInputString(StringData(lower), signalingFlags);
    _value = libraryTypeToValue(dec128);
}

Decimal128::Value Decimal128::getValue() const {
    return _value;
}

Decimal128 Decimal128::toAbs() const {
    BID_UINT128 dec128 = decimal128ToLibraryType(_value);
    dec128 = bid128_abs(dec128);
    return Decimal128(libraryTypeToValue(dec128));
}

std::int32_t Decimal128::toInt(RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return toInt(&throwAwayFlag, roundMode);
}

std::int32_t Decimal128::toInt(std::uint32_t* signalingFlags, RoundingMode roundMode) const {
    BID_UINT128 dec128 = decimal128ToLibraryType(_value);
    switch (roundMode) {
        case kRoundTiesToEven:
            return bid128_to_int32_rnint(dec128, signalingFlags);
        case kRoundTowardNegative:
            return bid128_to_int32_floor(dec128, signalingFlags);
        case kRoundTowardPositive:
            return bid128_to_int32_ceil(dec128, signalingFlags);
        case kRoundTowardZero:
            return bid128_to_int32_int(dec128, signalingFlags);
        case kRoundTiesToAway:
            return bid128_to_int32_rninta(dec128, signalingFlags);
        default:
            return bid128_to_int32_rnint(dec128, signalingFlags);
    }
}

int64_t Decimal128::toLong(RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return toLong(&throwAwayFlag, roundMode);
}

int64_t Decimal128::toLong(std::uint32_t* signalingFlags, RoundingMode roundMode) const {
    BID_UINT128 dec128 = decimal128ToLibraryType(_value);
    switch (roundMode) {
        case kRoundTiesToEven:
            return bid128_to_int64_rnint(dec128, signalingFlags);
        case kRoundTowardNegative:
            return bid128_to_int64_floor(dec128, signalingFlags);
        case kRoundTowardPositive:
            return bid128_to_int64_ceil(dec128, signalingFlags);
        case kRoundTowardZero:
            return bid128_to_int64_int(dec128, signalingFlags);
        case kRoundTiesToAway:
            return bid128_to_int64_rninta(dec128, signalingFlags);
        default:
            return bid128_to_int64_rnint(dec128, signalingFlags);
    }
}

std::int32_t Decimal128::toIntExact(RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return toIntExact(&throwAwayFlag, roundMode);
}

std::int32_t Decimal128::toIntExact(std::uint32_t* signalingFlags, RoundingMode roundMode) const {
    BID_UINT128 dec128 = decimal128ToLibraryType(_value);
    switch (roundMode) {
        case kRoundTiesToEven:
            return bid128_to_int32_xrnint(dec128, signalingFlags);
        case kRoundTowardNegative:
            return bid128_to_int32_xfloor(dec128, signalingFlags);
        case kRoundTowardPositive:
            return bid128_to_int32_xceil(dec128, signalingFlags);
        case kRoundTowardZero:
            return bid128_to_int32_xint(dec128, signalingFlags);
        case kRoundTiesToAway:
            return bid128_to_int32_xrninta(dec128, signalingFlags);
        default:
            return bid128_to_int32_xrnint(dec128, signalingFlags);
    }
}

std::int64_t Decimal128::toLongExact(RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return toLongExact(&throwAwayFlag, roundMode);
}

std::int64_t Decimal128::toLongExact(std::uint32_t* signalingFlags, RoundingMode roundMode) const {
    BID_UINT128 dec128 = decimal128ToLibraryType(_value);
    switch (roundMode) {
        case kRoundTiesToEven:
            return bid128_to_int64_xrnint(dec128, signalingFlags);
        case kRoundTowardNegative:
            return bid128_to_int64_xfloor(dec128, signalingFlags);
        case kRoundTowardPositive:
            return bid128_to_int64_xceil(dec128, signalingFlags);
        case kRoundTowardZero:
            return bid128_to_int64_xint(dec128, signalingFlags);
        case kRoundTiesToAway:
            return bid128_to_int64_xrninta(dec128, signalingFlags);
        default:
            return bid128_to_int64_xrnint(dec128, signalingFlags);
    }
}

double Decimal128::toDouble(RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return toDouble(&throwAwayFlag, roundMode);
}

double Decimal128::toDouble(std::uint32_t* signalingFlags, RoundingMode roundMode) const {
    BID_UINT128 dec128 = decimal128ToLibraryType(_value);
    return bid128_to_binary64(dec128, roundMode, signalingFlags);
}

std::string Decimal128::toString() const {
    // If the decimal is a variant of NaN (i.e. sNaN, -NaN, +NaN, etc...) or a variant of
    // Inf (i.e. +Inf, Inf, -Inf), return either NaN, Infinity, or -Infinity
    if (!isFinite()) {
        if (this->isEqual(kPositiveInfinity)) {
            return "Infinity";
        } else if (this->isEqual(kNegativeInfinity)) {
            return "-Infinity";
        }
        invariant(isNaN());
        return "NaN";
    }
    BID_UINT128 dec128 = decimal128ToLibraryType(_value);
    char decimalCharRepresentation[1 /* mantissa sign */ + 34 /* mantissa */ +
                                   1 /* scientific E */ + 1 /* exponent sign */ + 4 /* exponent */ +
                                   1 /* null terminator */];
    std::uint32_t idec_signaling_flags = 0;
    /**
     * Use the library's defined to_string method, which returns a string composed of a
     * sign ('+' or '-')
     * 1 to 34 decimal digits (no leading zeros)
     * the character 'E'
     * sign ('+' or '-')
     * 1 to 4 decimal digits (no leading zeros)
     * For example: +10522E-3
     */
    bid128_to_string(decimalCharRepresentation, dec128, &idec_signaling_flags);

    StringData dec128String(decimalCharRepresentation);

    int ePos = dec128String.find("E");

    // Calculate the precision and exponent of the number and output it in a readable manner
    int precision = 0;
    int exponent = 0;

    StringData exponentString = dec128String.substr(ePos);

    // Get the value of the exponent, start at 2 to ignore the E and the sign
    for (size_t i = 2; i < exponentString.size(); ++i) {
        exponent = exponent * 10 + (exponentString[i] - '0');
    }
    if (exponentString[1] == '-') {
        exponent *= -1;
    }
    // Get the total precision of the number, i.e. the length of the coefficient
    precision = dec128String.size() - exponentString.size() - 1 /* mantissa sign */;

    std::string result;
    // Initially result is set to equal just the sign of the dec128 string
    // For formatting, leave off the sign if it is positive
    if (dec128String[0] == '-')
        result = "-";

    StringData coefficient = dec128String.substr(1, precision);
    int adjustedExponent = exponent + precision - 1;

    if (exponent > 0 || adjustedExponent < -6) {
        result += _convertToScientificNotation(coefficient, adjustedExponent);
    } else {
        result += _convertToStandardDecimalNotation(coefficient, exponent);
    }

    return result;
}

std::string Decimal128::_convertToScientificNotation(StringData coefficient,
                                                     int adjustedExponent) const {
    int cLength = coefficient.size();
    std::string result;
    for (int i = 0; i < cLength; i++) {
        result += coefficient[i];
        if (i == 0 && cLength > 1) {
            result += '.';
        }
    }
    result += 'E';
    if (adjustedExponent > 0) {
        result += '+';
    }
    result += std::to_string(adjustedExponent);
    return result;
}

std::string Decimal128::_convertToStandardDecimalNotation(StringData coefficient,
                                                          int exponent) const {
    if (exponent == 0) {
        return coefficient.toString();
    } else {
        invariant(exponent < 0);
        std::string result;
        int precision = coefficient.size();
        // Absolute value of the exponent
        int significantDecimalDigits = -exponent;
        bool decimalAppended = false;

        // Pre-pend 0's before the coefficient as necessary
        for (int i = precision; i <= significantDecimalDigits; i++) {
            result += '0';
            if (i == precision) {
                result += '.';
                decimalAppended = true;
            }
        }

        // Copy over the digits in the coefficient
        for (int i = 0; i < precision; i++) {
            if (precision - i == significantDecimalDigits && !decimalAppended) {
                result += '.';
            }
            result += coefficient[i];
        }
        return result;
    }
}

bool Decimal128::isZero() const {
    return bid128_isZero(decimal128ToLibraryType(_value));
}

bool Decimal128::isNaN() const {
    return bid128_isNaN(decimal128ToLibraryType(_value));
}

bool Decimal128::isInfinite() const {
    return bid128_isInf(decimal128ToLibraryType(_value));
}

bool Decimal128::isFinite() const {
    return bid128_isFinite(decimal128ToLibraryType(_value));
}

bool Decimal128::isNegative() const {
    return bid128_isSigned(decimal128ToLibraryType(_value));
}

Decimal128 Decimal128::add(const Decimal128& other, RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return add(other, &throwAwayFlag, roundMode);
}

Decimal128 Decimal128::add(const Decimal128& other,
                           std::uint32_t* signalingFlags,
                           RoundingMode roundMode) const {
    BID_UINT128 current = decimal128ToLibraryType(_value);
    BID_UINT128 addend = decimal128ToLibraryType(other.getValue());
    current = bid128_add(current, addend, roundMode, signalingFlags);
    Decimal128::Value value = libraryTypeToValue(current);
    Decimal128 result(value);
    return result;
}

Decimal128 Decimal128::subtract(const Decimal128& other, RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return subtract(other, &throwAwayFlag, roundMode);
}

Decimal128 Decimal128::subtract(const Decimal128& other,
                                std::uint32_t* signalingFlags,
                                RoundingMode roundMode) const {
    BID_UINT128 current = decimal128ToLibraryType(_value);
    BID_UINT128 sub = decimal128ToLibraryType(other.getValue());
    current = bid128_sub(current, sub, roundMode, signalingFlags);
    Decimal128::Value value = libraryTypeToValue(current);
    Decimal128 result(value);
    return result;
}

Decimal128 Decimal128::multiply(const Decimal128& other, RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return multiply(other, &throwAwayFlag, roundMode);
}

Decimal128 Decimal128::multiply(const Decimal128& other,
                                std::uint32_t* signalingFlags,
                                RoundingMode roundMode) const {
    BID_UINT128 current = decimal128ToLibraryType(_value);
    BID_UINT128 factor = decimal128ToLibraryType(other.getValue());
    current = bid128_mul(current, factor, roundMode, signalingFlags);
    Decimal128::Value value = libraryTypeToValue(current);
    Decimal128 result(value);
    return result;
}

Decimal128 Decimal128::divide(const Decimal128& other, RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return divide(other, &throwAwayFlag, roundMode);
}

Decimal128 Decimal128::divide(const Decimal128& other,
                              std::uint32_t* signalingFlags,
                              RoundingMode roundMode) const {
    BID_UINT128 current = decimal128ToLibraryType(_value);
    BID_UINT128 divisor = decimal128ToLibraryType(other.getValue());
    current = bid128_div(current, divisor, roundMode, signalingFlags);
    Decimal128::Value value = libraryTypeToValue(current);
    Decimal128 result(value);
    return result;
}

Decimal128 Decimal128::exponential(RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return exponential(&throwAwayFlag);
}

Decimal128 Decimal128::exponential(std::uint32_t* signalingFlags, RoundingMode roundMode) const {
    BID_UINT128 current = decimal128ToLibraryType(_value);
    current = bid128_exp(current, roundMode, signalingFlags);
    return Decimal128{libraryTypeToValue(current)};
}

Decimal128 Decimal128::logarithm(RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return logarithm(&throwAwayFlag);
}

Decimal128 Decimal128::logarithm(std::uint32_t* signalingFlags, RoundingMode roundMode) const {
    BID_UINT128 current = decimal128ToLibraryType(_value);
    current = bid128_log(current, roundMode, signalingFlags);
    return Decimal128{libraryTypeToValue(current)};
}

Decimal128 Decimal128::logarithm(const Decimal128& other, RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    if (other.isEqual(Decimal128(2))) {
        BID_UINT128 current = decimal128ToLibraryType(_value);
        current = bid128_log2(current, roundMode, &throwAwayFlag);
        return Decimal128{libraryTypeToValue(current)};
    }
    if (other.isEqual(Decimal128(10))) {
        BID_UINT128 current = decimal128ToLibraryType(_value);
        current = bid128_log10(current, roundMode, &throwAwayFlag);
        return Decimal128{libraryTypeToValue(current)};
    }
    return logarithm(other, &throwAwayFlag);
}

Decimal128 Decimal128::logarithm(const Decimal128& other,
                                 std::uint32_t* signalingFlags,
                                 RoundingMode roundMode) const {
    return logarithm(signalingFlags, roundMode).divide(other);
}

Decimal128 Decimal128::modulo(const Decimal128& other) const {
    std::uint32_t throwAwayFlag = 0;
    return modulo(other, &throwAwayFlag);
}

Decimal128 Decimal128::modulo(const Decimal128& other, std::uint32_t* signalingFlags) const {
    BID_UINT128 current = decimal128ToLibraryType(_value);
    BID_UINT128 divisor = decimal128ToLibraryType(other.getValue());
    current = bid128_fmod(current, divisor, signalingFlags);
    return Decimal128{libraryTypeToValue(current)};
}

Decimal128 Decimal128::power(const Decimal128& other, RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return power(other, &throwAwayFlag, roundMode);
}

Decimal128 Decimal128::power(const Decimal128& other,
                             std::uint32_t* signalingFlags,
                             RoundingMode roundMode) const {
    BID_UINT128 base = decimal128ToLibraryType(_value);
    BID_UINT128 exp = decimal128ToLibraryType(other.getValue());


    BID_UINT128 result;
    if (this->isEqual(Decimal128(10)))
        result = bid128_exp10(exp, roundMode, signalingFlags);
    else if (this->isEqual(Decimal128(2)))
        result = bid128_exp2(exp, roundMode, signalingFlags);
    else
        result = bid128_pow(base, exp, roundMode, signalingFlags);
    return Decimal128{libraryTypeToValue(result)}.add(kLargestNegativeExponentZero);
}

Decimal128 Decimal128::quantize(const Decimal128& other, RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return quantize(other, &throwAwayFlag, roundMode);
}

Decimal128 Decimal128::quantize(const Decimal128& reference,
                                std::uint32_t* signalingFlags,
                                RoundingMode roundMode) const {
    BID_UINT128 current = decimal128ToLibraryType(_value);
    BID_UINT128 q = decimal128ToLibraryType(reference.getValue());
    BID_UINT128 quantizedResult = bid128_quantize(current, q, roundMode, signalingFlags);
    Decimal128::Value value = libraryTypeToValue(quantizedResult);
    Decimal128 result(value);
    return result;
}

Decimal128 Decimal128::squareRoot(RoundingMode roundMode) const {
    std::uint32_t throwAwayFlag = 0;
    return exponential(&throwAwayFlag);
}

Decimal128 Decimal128::squareRoot(std::uint32_t* signalingFlags, RoundingMode roundMode) const {
    BID_UINT128 current = decimal128ToLibraryType(_value);
    current = bid128_sqrt(current, roundMode, signalingFlags);
    return Decimal128{libraryTypeToValue(current)};
}

bool Decimal128::isEqual(const Decimal128& other) const {
    std::uint32_t throwAwayFlag = 0;
    BID_UINT128 current = decimal128ToLibraryType(_value);
    BID_UINT128 compare = decimal128ToLibraryType(other.getValue());
    return bid128_quiet_equal(current, compare, &throwAwayFlag);
}

bool Decimal128::isNotEqual(const Decimal128& other) const {
    std::uint32_t throwAwayFlag = 0;
    BID_UINT128 current = decimal128ToLibraryType(_value);
    BID_UINT128 compare = decimal128ToLibraryType(other.getValue());
    return bid128_quiet_not_equal(current, compare, &throwAwayFlag);
}

bool Decimal128::isGreater(const Decimal128& other) const {
    std::uint32_t throwAwayFlag = 0;
    BID_UINT128 current = decimal128ToLibraryType(_value);
    BID_UINT128 compare = decimal128ToLibraryType(other.getValue());
    return bid128_quiet_greater(current, compare, &throwAwayFlag);
}

bool Decimal128::isGreaterEqual(const Decimal128& other) const {
    std::uint32_t throwAwayFlag = 0;
    BID_UINT128 current = decimal128ToLibraryType(_value);
    BID_UINT128 compare = decimal128ToLibraryType(other.getValue());
    return bid128_quiet_greater_equal(current, compare, &throwAwayFlag);
}

bool Decimal128::isLess(const Decimal128& other) const {
    std::uint32_t throwAwayFlag = 0;
    BID_UINT128 current = decimal128ToLibraryType(_value);
    BID_UINT128 compare = decimal128ToLibraryType(other.getValue());
    return bid128_quiet_less(current, compare, &throwAwayFlag);
}

bool Decimal128::isLessEqual(const Decimal128& other) const {
    std::uint32_t throwAwayFlag = 0;
    BID_UINT128 current = decimal128ToLibraryType(_value);
    BID_UINT128 compare = decimal128ToLibraryType(other.getValue());
    return bid128_quiet_less_equal(current, compare, &throwAwayFlag);
}

/**
 * The following static const variables are used to mathematically produce
 * frequently needed Decimal128 constants.
 */

namespace {
// Get the representation of 1 with 17 zeros (half of decimal128's 34 digit precision)
const std::uint64_t t17 = 100ull * 1000 * 1000 * 1000 * 1000 * 1000;
// Get the low 64 bits of 34 consecutive decimal 9's
// t17 * 17 gives 1 with 34 0's, so subtract 1 to get all 9's
const std::uint64_t t34lo64 = t17 * t17 - 1;
// Mod t17 by 2^32 to get the low 32 bits of t17's binary representation
const std::uint64_t t17lo32 = t17 % (1ull << 32);
// Divide t17 by 2^32 to get the high 32 bits of t17's binary representation
const std::uint64_t t17hi32 = t17 >> 32;
// Multiply t17 by t17 and keep the high 64 bits by distributing the operation to
// t17hi32*t17hi32 + 2*t17hi32*t17lo32 + t17lo32*t17lo32 where the 2nd term
// is shifted right by 32 and the 3rd term by 64 (which effectively drops the 3rd term)
const std::uint64_t t34hi64 = t17hi32 * t17hi32 + (((t17hi32 * t17lo32) >> 31));
MONGO_STATIC_ASSERT(t34hi64 == 0x1ed09bead87c0);
MONGO_STATIC_ASSERT(t34lo64 == 0x378d8e63ffffffff);
}  // namespace

// (t34hi64 << 64) + t34lo64 == 1e34 - 1
const Decimal128 Decimal128::kLargestPositive(0, Decimal128::kMaxBiasedExponent, t34hi64, t34lo64);
// The smallest positive decimal is 1 with the largest negative exponent of 0 (biased)
const Decimal128 Decimal128::kSmallestPositive(0, 0, 0, 1);

// Add a sign bit to the largest and smallest positive to get their corresponding negatives
const Decimal128 Decimal128::kLargestNegative(1, Decimal128::kMaxBiasedExponent, t34hi64, t34lo64);
const Decimal128 Decimal128::kSmallestNegative(1, 0, 0, 1);

// Get the representation of 0 (0E0).
const Decimal128 Decimal128::kNormalizedZero(Decimal128::Value(
    {0, static_cast<uint64_t>(Decimal128::kExponentBias) << Decimal128::kExponentFieldPos}));

// Get the representation of 0 with the most negative exponent
const Decimal128 Decimal128::kLargestNegativeExponentZero(Decimal128::Value({0ull, 0ull}));

// Shift the format of the combination bits to the right position to get Inf and NaN
// +Inf = 0111 1000 ... ... = 0x78 ... ..., -Inf = 1111 1000 ... ... = 0xf8 ... ...
// +NaN = 0111 1100 ... ... = 0x7c ... ..., -NaN = 1111 1100 ... ... = 0xfc ... ...
const Decimal128 Decimal128::kPositiveInfinity(Decimal128::Value({0ull, 0x78ull << 56}));
const Decimal128 Decimal128::kNegativeInfinity(Decimal128::Value({0ull, 0xf8ull << 56}));
const Decimal128 Decimal128::kPositiveNaN(Decimal128::Value({0ull, 0x7cull << 56}));
const Decimal128 Decimal128::kNegativeNaN(Decimal128::Value({0ull, 0xfcull << 56}));

std::ostream& operator<<(std::ostream& stream, const Decimal128& value) {
    return stream << value.toString();
}

}  // namespace mongo
