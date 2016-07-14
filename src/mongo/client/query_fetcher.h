/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/client/fetcher.h"
#include "mongo/stdx/functional.h"

namespace mongo {

struct HostAndPort;
class NamespaceString;
class BSONObj;
class BSONObjBuilder;

/**
 * Follows the fetcher pattern for a find+getmore.
 * QueryFetcher will continue to call getmore until an error or
 * until the last batch of results.
 */
class QueryFetcher {
    MONGO_DISALLOW_COPYING(QueryFetcher);

public:
    using CallbackFn =
        stdx::function<void(const Fetcher::QueryResponseStatus&, Fetcher::NextAction*)>;

    QueryFetcher(executor::TaskExecutor* exec,
                 const HostAndPort& source,
                 const NamespaceString& nss,
                 const BSONObj& cmdBSON,
                 const QueryFetcher::CallbackFn& onBatchAvailable,
                 const BSONObj& metadata = rpc::makeEmptyMetadata());
    QueryFetcher(executor::TaskExecutor* exec,
                 const HostAndPort& source,
                 const NamespaceString& nss,
                 const BSONObj& cmdBSON,
                 const QueryFetcher::CallbackFn& onBatchAvailable,
                 const BSONObj& metadata,
                 Milliseconds timeout);
    virtual ~QueryFetcher() = default;

    bool isActive() const {
        return _fetcher.isActive();
    }
    Status schedule() {
        return _fetcher.schedule();
    }
    void cancel() {
        return _fetcher.cancel();
    }
    void wait() {
        if (_fetcher.isActive())
            _fetcher.wait();
    }
    std::string getDiagnosticString() const;

protected:
    void _onFetchCallback(const Fetcher::QueryResponseStatus& fetchResult,
                          Fetcher::NextAction* nextAction,
                          BSONObjBuilder* getMoreBob);

    /**
     * Called by _delegateCallback() to forward query results to '_work'.
     */
    void _onQueryResponse(const Fetcher::QueryResponseStatus& fetchResult,
                          Fetcher::NextAction* nextAction);

    virtual void _delegateCallback(const Fetcher::QueryResponseStatus& fetchResult,
                                   Fetcher::NextAction* nextAction);

private:
    executor::TaskExecutor* _exec;
    Fetcher _fetcher;
    const QueryFetcher::CallbackFn _work;
};

}  // namespace mongo
