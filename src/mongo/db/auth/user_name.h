
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

#include <iosfwd>
#include <memory>
#include <string>


#include "mongo/base/clonable_ptr.h"
#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

/**
 * Representation of a name of a principal (authenticatable user) in a MongoDB system.
 *
 * Consists of a "user name" part, and a "database name" part.
 */
class UserName {
public:
    UserName() : _splitPoint(0) {}
    UserName(StringData user, StringData dbname);

    /**
     * Parses a string of the form "db.username" into a UserName object.
     */
    static StatusWith<UserName> parse(StringData userNameStr);

    /**
     * Gets the user part of a UserName.
     */
    StringData getUser() const {
        return StringData(_fullName).substr(0, _splitPoint);
    }

    /**
     * Gets the database name part of a UserName.
     */
    StringData getDB() const {
        return StringData(_fullName).substr(_splitPoint + 1);
    }

    /**
     * Gets the full unique name of a user as a string, formatted as "user@db".
     */
    const std::string& getFullName() const {
        return _fullName;
    }

    /**
     * Gets the full unambiguous unique name of a user as a string, formatted as "db.user"
     */
    std::string getUnambiguousName() const {
        return str::stream() << getDB() << "." << getUser();
    }

    /**
     * Stringifies the object, for logging/debugging.
     */
    std::string toString() const {
        return getFullName();
    }

    bool operator==(const UserName& rhs) const {
        return _splitPoint == rhs._splitPoint && getFullName() == rhs.getFullName();
    }

    bool operator!=(const UserName& rhs) const {
        return _splitPoint != rhs._splitPoint || getFullName() != rhs.getFullName();
    }

    bool operator<(const UserName& rhs) const {
        return getUser() < rhs.getUser() || (getUser() == rhs.getUser() && getDB() < rhs.getDB());
    }

private:
    std::string _fullName;  // The full name, stored as a string.  "user@db".
    size_t _splitPoint;     // The index of the "@" separating the user and db name parts.
};

std::ostream& operator<<(std::ostream& os, const UserName& name);

/**
 * Iterator over an unspecified container of UserName objects.
 */
class UserNameIterator {
public:
    class Impl {
    public:
        Impl() = default;
        virtual ~Impl(){};
        std::unique_ptr<Impl> clone() const {
            return std::unique_ptr<Impl>(doClone());
        }
        virtual bool more() const = 0;
        virtual const UserName& get() const = 0;

        virtual const UserName& next() = 0;

    private:
        virtual Impl* doClone() const = 0;
    };

    UserNameIterator() = default;
    explicit UserNameIterator(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

    bool more() const {
        return _impl.get() && _impl->more();
    }
    const UserName& get() const {
        return _impl->get();
    }

    const UserName& next() {
        return _impl->next();
    }

    const UserName& operator*() const {
        return get();
    }
    const UserName* operator->() const {
        return &get();
    }

private:
    clonable_ptr<Impl> _impl;
};


template <typename ContainerIterator>
class UserNameContainerIteratorImpl : public UserNameIterator::Impl {
public:
    UserNameContainerIteratorImpl(const ContainerIterator& begin, const ContainerIterator& end)
        : _curr(begin), _end(end) {}
    ~UserNameContainerIteratorImpl() override {}
    bool more() const override {
        return _curr != _end;
    }
    const UserName& next() override {
        return *(_curr++);
    }
    const UserName& get() const override {
        return *_curr;
    }
    UserNameIterator::Impl* doClone() const override {
        return new UserNameContainerIteratorImpl(*this);
    }

private:
    ContainerIterator _curr;
    ContainerIterator _end;
};

template <typename ContainerIterator>
UserNameIterator makeUserNameIterator(const ContainerIterator& begin,
                                      const ContainerIterator& end) {
    return UserNameIterator(
        std::make_unique<UserNameContainerIteratorImpl<ContainerIterator>>(begin, end));
}

template <typename Container>
UserNameIterator makeUserNameIteratorForContainer(const Container& container) {
    return makeUserNameIterator(container.begin(), container.end());
}

template <typename Container>
Container userNameIteratorToContainer(UserNameIterator it) {
    Container container;
    while (it.more()) {
        container.emplace_back(it.next());
    }
    return container;
}

}  // namespace mongo
