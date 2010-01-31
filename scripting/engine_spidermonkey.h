// engine_spidermonkey.h

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

#pragma once

#include "engine.h"

// START inc hacking

#if defined( MOZJS )

#define MOZILLA_1_8_BRANCH

#include "mozjs/jsapi.h"
#include "mozjs/jsdate.h"
#include "mozjs/jsregexp.h"

#warning if you are using an ubuntu version of spider monkey, we recommend installing spider monkey from source

#elif defined( OLDJS )

#ifdef WIN32
#include "jstypes.h"
#undef JS_PUBLIC_API
#undef JS_PUBLIC_DATA
#define JS_PUBLIC_API(t)    t
#define JS_PUBLIC_DATA(t)   t
#endif

#include "jsapi.h"
#include "jsdate.h"
#include "jsregexp.h"

#else

#include "js/jsapi.h"
#include "js/jsobj.h"
#include "js/jsdate.h"
#include "js/jsregexp.h"

#endif

// END inc hacking

// -- SM 1.6 hacks ---
#ifndef JSCLASS_GLOBAL_FLAGS

#warning old version of spider monkey ( probably 1.6 ) you should upgrade to at least 1.7

#define JSCLASS_GLOBAL_FLAGS 0

JSBool JS_CStringsAreUTF8(){
    return false;
}

#define SM16

#endif
// -- END SM 1.6 hacks ---

#ifdef JSVAL_IS_TRACEABLE
#define SM18
#endif

#ifdef XULRUNNER
#define SM181
#endif

namespace mongo {

    class SMScope;
    class Convertor;
    
    extern JSClass bson_class;
    extern JSClass bson_ro_class;

    extern JSClass object_id_class;
    extern JSClass dbpointer_class;
    extern JSClass dbref_class;
    extern JSClass bindata_class;
    extern JSClass timestamp_class;
    extern JSClass minkey_class;
    extern JSClass maxkey_class;

    // internal things
    void dontDeleteScope( SMScope * s ){}
    void errorReporter( JSContext *cx, const char *message, JSErrorReport *report );
    extern boost::thread_specific_ptr<SMScope> currentScope;
    
    // bson
    JSBool resolveBSONField( JSContext *cx, JSObject *obj, jsval id, uintN flags, JSObject **objp );


    // mongo
    void initMongoJS( SMScope * scope , JSContext * cx , JSObject * global , bool local );
    bool appendSpecialDBObject( Convertor * c , BSONObjBuilder& b , const string& name , jsval val , JSObject * o );

#define JSVAL_IS_OID(v) ( JSVAL_IS_OBJECT( v ) && JS_InstanceOf( cx , JSVAL_TO_OBJECT( v ) , &object_id_class , 0 ) )
    
    bool isDate( JSContext * cx , JSObject * o );
    
}
