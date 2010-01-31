// matcher.h

/* Matcher is our boolean expression evaluator for "where" clauses */

/**
*    Copyright (C) 2008 10gen Inc.
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
*/

#pragma once

#include "jsobj.h"
#include <pcrecpp.h>

namespace mongo {

    class CoveredIndexMatcher;
    class Matcher;

    class RegexMatcher {
    public:
        const char *fieldName;
        pcrecpp::RE *re;
        RegexMatcher() {
            re = 0;
        }
        ~RegexMatcher() {
            delete re;
        }
    };
    
    struct element_lt
    {
        bool operator()(const BSONElement& l, const BSONElement& r) const
        {
            int x = (int) l.canonicalType() - (int) r.canonicalType();
            if ( x < 0 ) return true;
            else if ( x > 0 ) return false;
            return compareElementValues(l,r) < 0;
        }
    };

    
    class ElementMatcher {
    public:
    
        ElementMatcher() {
        }
        
        ElementMatcher( BSONElement _e , int _op );
        
        ElementMatcher( BSONElement _e , int _op , const BSONObj& array ) : toMatch( _e ) , compareOp( _op ) {
            
            myset.reset( new set<BSONElement,element_lt>() );
            
            BSONObjIterator i( array );
            while ( i.more() ) {
                BSONElement ie = i.next();
                myset->insert(ie);
            }
        }
        
        ~ElementMatcher();

        BSONElement toMatch;
        int compareOp;
        shared_ptr< set<BSONElement,element_lt> > myset;
        
        // these are for specific operators
        int mod;
        int modm;
        BSONType type;

        shared_ptr<Matcher> subMatcher;
    };

// SQL where clause equivalent
    class Where;
    class DiskLoc;

    /* Match BSON objects against a query pattern.

       e.g.
           db.foo.find( { a : 3 } );

       { a : 3 } is the pattern object.  See wiki documentation for full info.

       GT/LT:
         { a : { $gt : 3 } }
       Not equal:
         { a : { $ne : 3 } }

       TODO: we should rewrite the matcher to be more an AST style.
    */
    class Matcher : boost::noncopyable {
        int matchesDotted(
            const char *fieldName,
            const BSONElement& toMatch, const BSONObj& obj,
            int compareOp, const ElementMatcher& bm, bool isArr = false);

        int matchesNe(
            const char *fieldName,
            const BSONElement &toMatch, const BSONObj &obj,
            const ElementMatcher&bm);
        
    public:
        static int opDirection(int op) {
            return op <= BSONObj::LTE ? -1 : 1;
        }

        // Only specify constrainIndexKey if matches() will be called with
        // index keys having empty string field names.
        Matcher(const BSONObj &pattern, const BSONObj &constrainIndexKey = BSONObj());

        ~Matcher();

        bool matches(const BSONObj& j);
        
        bool keyMatch() const { return !all && !haveSize && !hasArray; }

        bool atomic() const { return _atomic; }

    private:
        void addBasic(const BSONElement &e, int c) {
            // TODO May want to selectively ignore these element types based on op type.
            if ( e.type() == MinKey || e.type() == MaxKey )
                return;
            basics.push_back( ElementMatcher( e , c ) );
        }

        int valuesMatch(const BSONElement& l, const BSONElement& r, int op, const ElementMatcher& bm);

        Where *where;                    // set if query uses $where
        BSONObj jsobj;                  // the query pattern.  e.g., { name: "joe" }
        BSONObj constrainIndexKey_;
        vector<ElementMatcher> basics;
//        int n;                           // # of basicmatcher items
        bool haveSize;
        bool all;
        bool hasArray;

        /* $atomic - if true, a multi document operation (some removes, updates)
                     should be done atomically.  in that case, we do not yield - 
                     i.e. we stay locked the whole time.
                     http://www.mongodb.org/display/DOCS/Removing[
        */
        bool _atomic;

        RegexMatcher regexs[4];
        int nRegex;

        // so we delete the mem when we're done:
        vector< shared_ptr< BSONObjBuilder > > _builders;

        friend class CoveredIndexMatcher;
    };
    
    // If match succeeds on index key, then attempt to match full document.
    class CoveredIndexMatcher : boost::noncopyable {
    public:
        CoveredIndexMatcher(const BSONObj &pattern, const BSONObj &indexKeyPattern);
        bool matches(const BSONObj &o){ return _docMatcher.matches( o ); }
        bool matches(const BSONObj &key, const DiskLoc &recLoc);
        bool needRecord(){ return _needRecord; }

        Matcher& docMatcher() { return _docMatcher; }
    private:
        Matcher _keyMatcher;
        Matcher _docMatcher;
        bool _needRecord;
    };
    
} // namespace mongo
