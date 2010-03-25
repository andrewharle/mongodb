/* message

   todo: authenticate; encrypt?
*/

/*    Copyright 2009 10gen Inc.
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

#include "stdafx.h"
#include "message.h"
#include <time.h>
#include "../util/goodies.h"
#include "../util/background.h"
#include <fcntl.h>
#include <errno.h>
#include "../db/cmdline.h"

namespace mongo {

    bool objcheck = false;
    
// if you want trace output:
#define mmm(x)

#ifdef MSG_NOSIGNAL
    const int portSendFlags = MSG_NOSIGNAL;
    const int portRecvFlags = MSG_NOSIGNAL;
#else
    const int portSendFlags = 0;
    const int portRecvFlags = 0;
#endif

    /* listener ------------------------------------------------------------------- */

    bool Listener::init() {
        SockAddr me;
        if ( ip.empty() )
            me = SockAddr( port );
        else
            me = SockAddr( ip.c_str(), port );
        sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if ( sock == INVALID_SOCKET ) {
            log() << "ERROR: listen(): invalid socket? " << OUTPUT_ERRNO << endl;
            return false;
        }
        prebindOptions( sock );
        if ( ::bind(sock, (sockaddr *) &me.sa, me.addressSize) != 0 ) {
            log() << "listen(): bind() failed " << OUTPUT_ERRNO << " for port: " << port << endl;
            closesocket(sock);
            return false;
        }

        if ( ::listen(sock, 128) != 0 ) {
            log() << "listen(): listen() failed " << OUTPUT_ERRNO << endl;
            closesocket(sock);
            return false;
        }
        
        return true;
    }

    void Listener::listen() {
        static long connNumber = 0;
        SockAddr from;
        while ( ! inShutdown() ) {
            int s = accept(sock, (sockaddr *) &from.sa, &from.addressSize);
            if ( s < 0 ) {
                if ( errno == ECONNABORTED || errno == EBADF ) {
                    log() << "Listener on port " << port << " aborted" << endl;
                    return;
                }
                log() << "Listener: accept() returns " << s << " " << OUTPUT_ERRNO << endl;
                continue;
            }
            disableNagle(s);
            if ( ! cmdLine.quiet ) log() << "connection accepted from " << from.toString() << " #" << ++connNumber << endl;
            accepted( new MessagingPort(s, from) );
        }
    }

    /* messagingport -------------------------------------------------------------- */

    class PiggyBackData {
    public:
        PiggyBackData( MessagingPort * port ) {
            _port = port;
            _buf = new char[1300];
            _cur = _buf;
        }

        ~PiggyBackData() {
            flush();
            delete( _cur );
        }

        void append( Message& m ) {
            assert( m.data->len <= 1300 );

            if ( len() + m.data->len > 1300 )
                flush();

            memcpy( _cur , m.data , m.data->len );
            _cur += m.data->len;
        }

        int flush() {
            if ( _buf == _cur )
                return 0;

            int x = _port->send( _buf , len() );
            _cur = _buf;
            return x;
        }

        int len() {
            return _cur - _buf;
        }

    private:

        MessagingPort* _port;

        char * _buf;
        char * _cur;
    };

    class Ports { 
        set<MessagingPort*>& ports;
        mongo::mutex m;
    public:
        // we "new" this so it is still be around when other automatic global vars
        // are being destructed during termination.
        Ports() : ports( *(new set<MessagingPort*>()) ) {}
        void closeAll() { \
            scoped_lock bl(m);
            for ( set<MessagingPort*>::iterator i = ports.begin(); i != ports.end(); i++ )
                (*i)->shutdown();
        }
        void insert(MessagingPort* p) { 
            scoped_lock bl(m);
            ports.insert(p);
        }
        void erase(MessagingPort* p) { 
            scoped_lock bl(m);
            ports.erase(p);
        }
    } ports;



    void closeAllSockets() {
        ports.closeAll();
    }

    MessagingPort::MessagingPort(int _sock, SockAddr& _far) : sock(_sock), piggyBackData(0), farEnd(_far) {
        ports.insert(this);
    }

    MessagingPort::MessagingPort() {
        ports.insert(this);
        sock = -1;
        piggyBackData = 0;
    }

    void MessagingPort::shutdown() {
        if ( sock >= 0 ) {
            closesocket(sock);
            sock = -1;
        }
    }

    MessagingPort::~MessagingPort() {
        if ( piggyBackData )
            delete( piggyBackData );
        shutdown();
        ports.erase(this);
    }

    class ConnectBG : public BackgroundJob {
    public:
        int sock;
        int res;
        SockAddr farEnd;
        void run() {
            res = ::connect(sock, (sockaddr *) &farEnd.sa, farEnd.addressSize);
        }
    };

    bool MessagingPort::connect(SockAddr& _far)
    {
        farEnd = _far;

        sock = socket(AF_INET, SOCK_STREAM, 0);
        if ( sock == INVALID_SOCKET ) {
            log() << "ERROR: connect(): invalid socket? " << OUTPUT_ERRNO << endl;
            return false;
        }

#if 0
        long fl = fcntl(sock, F_GETFL, 0);
        assert( fl >= 0 );
        fl |= O_NONBLOCK;
        fcntl(sock, F_SETFL, fl);

        int res = ::connect(sock, (sockaddr *) &farEnd.sa, farEnd.addressSize);
        if ( res ) {
            if ( errno == EINPROGRESS )
                closesocket(sock);
            sock = -1;
            return false;
        }

#endif

        ConnectBG bg;
        bg.sock = sock;
        bg.farEnd = farEnd;
        bg.go();

        // int res = ::connect(sock, (sockaddr *) &farEnd.sa, farEnd.addressSize);
        if ( bg.wait(5000) ) {
            if ( bg.res ) {
                closesocket(sock);
                sock = -1;
                return false;
            }
        }
        else {
            // time out the connect
            closesocket(sock);
            sock = -1;
            bg.wait(); // so bg stays in scope until bg thread terminates
            return false;
        }

        disableNagle(sock);

#ifdef SO_NOSIGPIPE
        // osx
        const int one = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(int));
#endif

        return true;
    }

    bool MessagingPort::recv(Message& m) {
again:
        mmm( out() << "*  recv() sock:" << this->sock << endl; )
        int len = -1;

        char *lenbuf = (char *) &len;
        int lft = 4;
        while ( 1 ) {
            int x = recv( lenbuf, lft );
            if ( x == 0 ) {
                DEV out() << "MessagingPort recv() conn closed? " << farEnd.toString() << endl;
                m.reset();
                return false;
            }
            if ( x < 0 ) {
                log() << "MessagingPort recv() " << OUTPUT_ERRNO << " " << farEnd.toString()<<endl;
                m.reset();
                return false;
            }
            lft -= x;
            if ( lft == 0 )
                break;
            lenbuf += x;
            log() << "MessagingPort recv() got " << x << " bytes wanted 4, lft=" << lft << endl;
            assert( lft > 0 );
        }

        if ( len < 0 || len > 16000000 ) {
            if ( len == -1 ) {
                // Endian check from the database, after connecting, to see what mode server is running in.
                unsigned foo = 0x10203040;
                int x = send( (char *) &foo, 4 );
                if ( x <= 0 ) {
                    log() << "MessagingPort endian send() " << OUTPUT_ERRNO << ' ' << farEnd.toString() << endl;
                    return false;
                }
                goto again;
            }

            if ( len == 542393671 ){
                // an http GET
                log() << "looks like you're trying to access db over http on native driver port.  please add 1000 for webserver" << endl;
                string msg = "You are trying to access MongoDB on the native driver port. For http diagnostic access, add 1000 to the port number\n";
                stringstream ss;
                ss << "HTTP/1.0 200 OK\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: " << msg.size() << "\r\n\r\n" << msg;
                string s = ss.str();
                send( s.c_str(), s.size() );
                return false;
            }
            log() << "bad recv() len: " << len << '\n';
            return false;
        }

        int z = (len+1023)&0xfffffc00;
        assert(z>=len);
        MsgData *md = (MsgData *) malloc(z);
        md->len = len;

        if ( len <= 0 ) {
            out() << "got a length of " << len << ", something is wrong" << endl;
            return false;
        }

        char *p = (char *) &md->id;
        int left = len -4;
        while ( 1 ) {
            int x = recv( p, left );
            if ( x == 0 ) {
                DEV out() << "MessagingPort::recv(): conn closed? " << farEnd.toString() << endl;
                m.reset();
                return false;
            }
            if ( x < 0 ) {
                log() << "MessagingPort recv() " << OUTPUT_ERRNO << ' ' << farEnd.toString() << endl;
                m.reset();
                return false;
            }
            left -= x;
            p += x;
            if ( left <= 0 )
                break;
        }

        m.setData(md, true);
        return true;
    }

    void MessagingPort::reply(Message& received, Message& response) {
        say(/*received.from, */response, received.data->id);
    }

    void MessagingPort::reply(Message& received, Message& response, MSGID responseTo) {
        say(/*received.from, */response, responseTo);
    }

    bool MessagingPort::call(Message& toSend, Message& response) {
        mmm( out() << "*call()" << endl; )
        MSGID old = toSend.data->id;
        say(/*to,*/ toSend);
        while ( 1 ) {
            bool ok = recv(response);
            if ( !ok )
                return false;
            //out() << "got response: " << response.data->responseTo << endl;
            if ( response.data->responseTo == toSend.data->id )
                break;
            out() << "********************" << endl;
            out() << "ERROR: MessagingPort::call() wrong id got:" << (unsigned)response.data->responseTo << " expect:" << (unsigned)toSend.data->id << endl;
            out() << "  toSend op: " << toSend.data->operation() << " old id:" << (unsigned)old << endl;
            out() << "  response msgid:" << (unsigned)response.data->id << endl;
            out() << "  response len:  " << (unsigned)response.data->len << endl;
            out() << "  response op:  " << response.data->operation() << endl;
            out() << "  farEnd: " << farEnd << endl;
            assert(false);
            response.reset();
        }
        mmm( out() << "*call() end" << endl; )
        return true;
    }

    void MessagingPort::say(Message& toSend, int responseTo) {
        assert( toSend.data );
        mmm( out() << "*  say() sock:" << this->sock << " thr:" << GetCurrentThreadId() << endl; )
        toSend.data->id = nextMessageId();
        toSend.data->responseTo = responseTo;

        int x = -100;

        if ( piggyBackData && piggyBackData->len() ) {
            mmm( out() << "*     have piggy back" << endl; )
            if ( ( piggyBackData->len() + toSend.data->len ) > 1300 ) {
                // won't fit in a packet - so just send it off
                piggyBackData->flush();
            }
            else {
                piggyBackData->append( toSend );
                x = piggyBackData->flush();
            }
        }

        if ( x == -100 )
            x = send( (char*)toSend.data, toSend.data->len );
        
        if ( x <= 0 ) {
            log() << "MessagingPort say send() " << OUTPUT_ERRNO << ' ' << farEnd.toString() << endl;
            throw SocketException();
        }

    }

    int MessagingPort::send( const char * data , const int len ){
        return ::send( sock , data , len , portSendFlags );
    }
    
    int MessagingPort::recv( char * buf , int max ){
        return ::recv( sock , buf , max , portRecvFlags );
    }

    void MessagingPort::piggyBack( Message& toSend , int responseTo ) {

        if ( toSend.data->len > 1300 ) {
            // not worth saving because its almost an entire packet
            say( toSend );
            return;
        }

        // we're going to be storing this, so need to set it up
        toSend.data->id = nextMessageId();
        toSend.data->responseTo = responseTo;

        if ( ! piggyBackData )
            piggyBackData = new PiggyBackData( this );

        piggyBackData->append( toSend );
    }

    unsigned MessagingPort::remotePort(){
        return farEnd.getPort();
    }

    MSGID NextMsgId;
    bool usingClientIds = 0;
    ThreadLocalValue<int> clientId;

    struct MsgStart {
        MsgStart() {
            NextMsgId = (((unsigned) time(0)) << 16) ^ curTimeMillis();
            assert(MsgDataHeaderSize == 16);
        }
    } msgstart;
    
    MSGID nextMessageId(){
        MSGID msgid = NextMsgId++;
        
        if ( usingClientIds ){
            msgid = msgid & 0xFFFF;
            msgid = msgid | clientId.get();
        }

        return msgid;
    }

    bool doesOpGetAResponse( int op ){
        return op == dbQuery || op == dbGetMore;
    }
    
    void setClientId( int id ){
        usingClientIds = true;
        id = id & 0xFFFF0000;
        massert( 10445 ,  "invalid id" , id );
        clientId.set( id );
    }
    
    int getClientId(){
        return clientId.get();
    }
    
} // namespace mongo
