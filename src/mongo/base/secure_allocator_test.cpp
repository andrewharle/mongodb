
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

#include "mongo/base/secure_allocator.h"

#include <array>

#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(SecureAllocator, SecureVector) {
    SecureAllocatorDefaultDomain::SecureVector<int> vec;

    vec->push_back(1);
    vec->push_back(2);

    ASSERT_EQUALS(1, (*vec)[0]);
    ASSERT_EQUALS(2, (*vec)[1]);

    vec->resize(2000, 3);
    ASSERT_EQUALS(3, (*vec)[2]);
}

TEST(SecureAllocator, SecureString) {
    SecureAllocatorDefaultDomain::SecureString str;

    str->resize(2000, 'x');
    ASSERT_EQUALS(0, str->compare(*SecureAllocatorDefaultDomain::SecureString(2000, 'x')));

    SecureAllocatorDefaultDomain::SecureString str2(str);
    ASSERT_NOT_EQUALS(&*str, &*str2);
    str2 = str;
    ASSERT_NOT_EQUALS(&*str, &*str2);

    auto strPtr = &*str;
    auto str2Ptr = &*str2;
    SecureAllocatorDefaultDomain::SecureString str3(std::move(str));
    ASSERT_EQUALS(strPtr, &*str3);
    str3 = std::move(str2);
    ASSERT_EQUALS(str2Ptr, &*str3);
}

// Verify that we can make a good number of secure objects.  Under the initial secure allocator
// design (page per object), you couldn't make more than 8-50 objects before running out of lockable
// pages.
TEST(SecureAllocator, ManySecureBytes) {
    std::array<SecureAllocatorDefaultDomain::SecureHandle<char>, 4096> chars;
    std::vector<SecureAllocatorDefaultDomain::SecureHandle<char>> e_chars(4096, 'e');
}

TEST(SecureAllocator, NonDefaultConstructibleWorks) {
    struct Foo {
        Foo(int) {}
        Foo() = delete;
    };

    SecureAllocatorDefaultDomain::SecureHandle<Foo> foo(10);
}

TEST(SecureAllocator, allocatorCanBeDisabled) {
    static size_t pegInvokationCountLast;
    static size_t pegInvokationCount;
    pegInvokationCountLast = 0;
    pegInvokationCount = 0;
    struct UnsecureAllocatorTrait {
        static bool peg() {
            pegInvokationCount++;

            return false;
        }
    };
    using UnsecureAllocatorDomain = SecureAllocatorDomain<UnsecureAllocatorTrait>;

    {
        std::vector<UnsecureAllocatorDomain::SecureHandle<char>> more_e_chars(4096, 'e');
        ASSERT_GT(pegInvokationCount, pegInvokationCountLast);
        pegInvokationCountLast = pegInvokationCount;

        UnsecureAllocatorDomain::SecureString str;
        ASSERT_GT(pegInvokationCount, pegInvokationCountLast);
        pegInvokationCountLast = pegInvokationCount;

        str->resize(2000, 'x');
        ASSERT_GT(pegInvokationCount, pegInvokationCountLast);
        pegInvokationCountLast = pegInvokationCount;

        ASSERT_EQUALS(0, str->compare(*UnsecureAllocatorDomain::SecureString(2000, 'x')));
        ASSERT_GT(pegInvokationCount, pegInvokationCountLast);
        pegInvokationCountLast = pegInvokationCount;
    }

    ASSERT_GT(pegInvokationCount, pegInvokationCountLast);
}

}  // namespace mongo
