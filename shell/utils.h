// utils.h

#pragma once

#include "../scripting/engine.h"

namespace mongo {

    namespace shellUtils {

        extern std::string _dbConnect;
        extern std::string _dbAuth;

        void RecordMyLocation( const char *_argv0 );
        void installShellUtils( Scope& scope );

        // Scoped management of mongo program instances.  Simple implementation:
        // destructor kills all mongod instances created by the shell.
        struct MongoProgramScope {
            MongoProgramScope() {} // Avoid 'unused variable' warning.
            ~MongoProgramScope();
        };
        void KillMongoProgramInstances();
        
        void initScope( Scope &scope );
    }
}
