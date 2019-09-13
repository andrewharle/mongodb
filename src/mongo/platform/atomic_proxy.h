
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

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "mongo/base/static_assert.h"
#include "mongo/config.h"

namespace mongo {

/**
* Provides a simple version of an atomic version of T
* that uses std::atomic<BaseWordT> as a backing type;
*/
template <typename T, typename BaseWordT>
class AtomicProxy {
    MONGO_STATIC_ASSERT_MSG(sizeof(T) == sizeof(BaseWordT),
                            "T and BaseWordT must have the same size");
    MONGO_STATIC_ASSERT_MSG(std::is_integral<BaseWordT>::value,
                            "BaseWordT must be an integral type");
#if MONGO_HAVE_STD_IS_TRIVIALLY_COPYABLE
    MONGO_STATIC_ASSERT_MSG(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
#endif
public:
    using value_type = T;
    using base_type = BaseWordT;

    explicit AtomicProxy(T value = 0) {
        store(value);
    }

    T operator=(T value) {
        store(value);
        return value;
    }

    operator T() const {
        return load();
    }

    T load(std::memory_order order = std::memory_order_seq_cst) const {
        const BaseWordT tempInteger = _value.load(order);
        T value;
        std::memcpy(&value, &tempInteger, sizeof(T));
        return value;
    }

    void store(const T value, std::memory_order order = std::memory_order_seq_cst) {
        BaseWordT tempInteger;
        std::memcpy(&tempInteger, &value, sizeof(T));
        _value.store(tempInteger, order);
    }

private:
    std::atomic<BaseWordT> _value;  // NOLINT
};

using AtomicDouble = AtomicProxy<double, std::uint64_t>;
}
