#pragma once

#include "../goodies.h" // printStackTrace

namespace mongo {

    /** some self-testing of synchronization and attempts to catch race conditions.

        use something like:

        CodeBlock myBlock;

        void foo() { 
            CodeBlock::Within w(myBlock);
            ...
        }

        In _DEBUG builds, will (sometimes/maybe) fail if two threads are in the same code block at 
        the same time. Also detects and disallows recursion.
    */

#ifdef _WIN32
    typedef unsigned threadId_t;
#else
    typedef pthread_t threadId_t;
#endif


#if defined(_DEBUG)

    namespace race {

        class CodePoint { 
        public:
            string lastName;
            threadId_t lastTid;
            string file;
            CodePoint(string f) : lastTid(0), file(f) { }
        };
        class Check {
        public:
            Check(CodePoint& p) {
                threadId_t t = GetCurrentThreadId();
                if( p.lastTid == 0 ) {
                    p.lastTid = t;
                    p.lastName = getThreadName();
                }
                else if( t != p.lastTid ) { 
                    log() << "\n\n\n\n\nRACE? error assert\n  " << p.file << '\n' 
                        << "  " << p.lastName
                        << "  " << getThreadName() << "\n\n" << endl;
                    mongoAbort("racecheck");
                }
            };
        };

    }

#define RACECHECK
        // dm TODO - the right code for this file is in a different branch at the moment (merge)
        //#define RACECHECK 
        //static race::CodePoint __cp(__FILE__); 
        //race::Check __ck(__cp);

    class CodeBlock { 
        volatile int n;
        threadId_t tid;
        void fail() { 
            log() << "synchronization (race condition) failure" << endl;
            printStackTrace();
            ::abort();
        }
        void enter() { 
            if( ++n != 1 ) fail();
#if defined(_WIN32)
            tid = GetCurrentThreadId();
#endif
        }
        void leave() {
            if( --n != 0 ) fail();
        }
    public:
        CodeBlock() : n(0) { }

        class Within { 
            CodeBlock& _s;
        public:
            Within(CodeBlock& s) : _s(s) { _s.enter(); }
            ~Within() { _s.leave(); }
        };

        void assertWithin() {
            assert( n == 1 );
#if defined(_WIN32)
            assert( GetCurrentThreadId() == tid );
#endif
        }
    };
    
#else

#define RACECHECK

    class CodeBlock{ 
    public:
        class Within { 
        public:
            Within(CodeBlock&) { }
        };
        void assertWithin() { }
    };

#endif

} // namespace
