
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

#include "mongo/db/auth/user_name.h"

#include <algorithm>
#include <iostream>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"

namespace mongo {

UserName::UserName(StringData user, StringData dbname) {
    _fullName.resize(user.size() + dbname.size() + 1);
    std::string::iterator iter =
        std::copy(user.rawData(), user.rawData() + user.size(), _fullName.begin());
    *iter = '@';
    ++iter;
    iter = std::copy(dbname.rawData(), dbname.rawData() + dbname.size(), iter);
    dassert(iter == _fullName.end());
    _splitPoint = user.size();
}


StatusWith<UserName> UserName::parse(StringData userNameStr) {
    size_t splitPoint = userNameStr.find('.');

    if (splitPoint == std::string::npos) {
        return Status(ErrorCodes::BadValue,
                      "username must contain a '.' separated database.user pair");
    }

    StringData userDBPortion = userNameStr.substr(0, splitPoint);
    StringData userNamePortion = userNameStr.substr(splitPoint + 1);

    return UserName(userNamePortion, userDBPortion);
}

std::ostream& operator<<(std::ostream& os, const UserName& name) {
    return os << name.getFullName();
}

}  // namespace mongo
