/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

namespace mongo {

class BSONObj;
class OperationContext;
class Status;
template <typename T>
class StatusWith;
namespace repl {
class ReadConcernArgs;
}


/**
 * Given the specified command and whether it supports read concern, returns an effective read
 * concern which should be used.
 */
StatusWith<repl::ReadConcernArgs> extractReadConcern(OperationContext* txn,
                                                     const BSONObj& cmdObj,
                                                     bool supportsReadConcern);

/**
 * Given the specified read concern arguments, performs checks that the read concern can actually be
 * satisfied given the current state of the server and if so calls into the replication subsystem to
 * perform the wait.
 */
Status waitForReadConcern(OperationContext* txn, const repl::ReadConcernArgs& readConcernArgs);

/*
 * Given a linearizable read command, confirm that
 * current primary is still the true primary of the replica set.
 */
Status waitForLinearizableReadConcern(OperationContext* txn);

}  // namespace mongo
