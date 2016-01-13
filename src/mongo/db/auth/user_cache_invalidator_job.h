/*    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/bson/oid.h"
#include "mongo/util/background.h"

#include <string>

namespace mongo {

    class AuthorizationManager;

    /**
     * Background job that runs only in mongos and periodically checks in with the config servers
     * to determine whether any authorization information has changed, and if so causes the
     * AuthorizationManager to throw out its in-memory cache of User objects (which contains the
     * users' credentials, roles, privileges, etc).
     */
    class UserCacheInvalidator : public BackgroundJob {
    public:
        explicit UserCacheInvalidator(AuthorizationManager* authzManager);

    protected:
        virtual std::string name() const;
        virtual void run();

    private:
        AuthorizationManager* _authzManager;
        OID _previousCacheGeneration;
    };

} // namespace mongo
