// queryutil.cpp

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

#include "btree.h"
#include "matcher.h"
#include "pdfile.h"
#include "queryoptimizer.h"
#include "../util/unittest.h"

namespace mongo {
    /** returns a string that when used as a matcher, would match a super set of regex()
        returns "" for complex regular expressions
        used to optimize queries in some simple regex cases that start with '^'

        if purePrefix != NULL, sets it to whether the regex can be converted to a range query
    */
    string simpleRegex(const char* regex, const char* flags, bool* purePrefix){
        string r = "";

        if (purePrefix) *purePrefix = false;

        bool extended = false;
        while (*flags){
            switch (*(flags++)){
                case 'm': // multiline
                    continue;
                case 'x': // extended
                    extended = true;
                    break;
                default:
                    return r; // cant use index
            }
        }

        if ( *(regex++) != '^' )
            return r;

        stringstream ss;

        while(*regex){
            char c = *(regex++);
            if ( c == '*' || c == '?' ){
                // These are the only two symbols that make the last char optional
                r = ss.str();
                r = r.substr( 0 , r.size() - 1 );
                return r; //breaking here fails with /^a?/
            } else if (c == '\\'){
                // slash followed by non-alphanumeric represents the following char
                c = *(regex++);
                if ((c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '0') ||
                    (c == '\0'))
                {
                    r = ss.str();
                    break;
                } else {
                    ss << c;
                }
            } else if (strchr("^$.[|()+{", c)){
                // list of "metacharacters" from man pcrepattern
                r = ss.str();
                break;
            } else if (extended && c == '#'){
                // comment
                r = ss.str();
                break;
            } else if (extended && isspace(c)){
                continue;
            } else {
                // self-matching char
                ss << c;
            }
        }

        if ( r.empty() && *regex == 0 ){
            r = ss.str();
            if (purePrefix) *purePrefix = !r.empty();
        }

        return r;
    }
    inline string simpleRegex(const BSONElement& e){
        switch(e.type()){
            case RegEx:
                return simpleRegex(e.regex(), e.regexFlags());
            case Object:{
                BSONObj o = e.embeddedObject();
                return simpleRegex(o["$regex"].valuestrsafe(), o["$options"].valuestrsafe());
            }
            default: assert(false); return ""; //return squashes compiler warning
        }
    }

    string simpleRegexEnd( string regex ) {
        ++regex[ regex.length() - 1 ];
        return regex;
    }    
    
    
    FieldRange::FieldRange( const BSONElement &e, bool isNot, bool optimize ) {
        // NOTE with $not, we could potentially form a complementary set of intervals.
        if ( !isNot && !e.eoo() && e.type() != RegEx && e.getGtLtOp() == BSONObj::opIN ) {
            set< BSONElement, element_lt > vals;
            vector< FieldRange > regexes;
            uassert( 12580 , "invalid query" , e.isABSONObj() );
            BSONObjIterator i( e.embeddedObject() );
            while( i.more() ) {
                BSONElement ie = i.next();
                if ( ie.type() == RegEx ) {
                    regexes.push_back( FieldRange( ie, false, optimize ) );
                } else {
                    vals.insert( ie );
                }
            }

            for( set< BSONElement, element_lt >::const_iterator i = vals.begin(); i != vals.end(); ++i )
                intervals_.push_back( FieldInterval(*i) );

            for( vector< FieldRange >::const_iterator i = regexes.begin(); i != regexes.end(); ++i )
                *this |= *i;
            
            return;
        }
        
        if ( e.type() == Array && e.getGtLtOp() == BSONObj::Equality ){
            
            intervals_.push_back( FieldInterval(e) );
            
            const BSONElement& temp = e.embeddedObject().firstElement();
            if ( ! temp.eoo() ){
                if ( temp < e )
                    intervals_.insert( intervals_.begin() , temp );
                else
                    intervals_.push_back( FieldInterval(temp) );
            }
            
            return;
        }

        intervals_.push_back( FieldInterval() );
        FieldInterval &initial = intervals_[ 0 ];
        BSONElement &lower = initial.lower_.bound_;
        bool &lowerInclusive = initial.lower_.inclusive_;
        BSONElement &upper = initial.upper_.bound_;
        bool &upperInclusive = initial.upper_.inclusive_;
        lower = minKey.firstElement();
        lowerInclusive = true;
        upper = maxKey.firstElement();
        upperInclusive = true;

        if ( e.eoo() )
            return;
        if ( e.type() == RegEx
             || (e.type() == Object && !e.embeddedObject()["$regex"].eoo())
           )
        {
            if ( !isNot ) { // no optimization for negated regex - we could consider creating 2 intervals comprising all nonmatching prefixes
                const string r = simpleRegex(e);
                if ( r.size() ) {
                    lower = addObj( BSON( "" << r ) ).firstElement();
                    upper = addObj( BSON( "" << simpleRegexEnd( r ) ) ).firstElement();
                    upperInclusive = false;
                } else {
                    BSONObjBuilder b1(32), b2(32);
                    b1.appendMinForType( "" , String );
                    lower = addObj( b1.obj() ).firstElement();

                    b2.appendMaxForType( "" , String );
                    upper = addObj( b2.obj() ).firstElement();
                    upperInclusive = false; //MaxForType String is an empty Object
                }

                // regex matches self - regex type > string type
                if (e.type() == RegEx){
                    BSONElement re = addObj( BSON( "" << e ) ).firstElement();
                    intervals_.push_back( FieldInterval(re) );
                } else {
                    BSONObj orig = e.embeddedObject();
                    BSONObjBuilder b;
                    b.appendRegex("", orig["$regex"].valuestrsafe(), orig["$options"].valuestrsafe());
                    BSONElement re = addObj( b.obj() ).firstElement();
                    intervals_.push_back( FieldInterval(re) );
                }

            }
            return;
        }
        int op = e.getGtLtOp();
        if ( isNot ) {
            switch( op ) {
                case BSONObj::Equality:
                case BSONObj::opALL:
                case BSONObj::opMOD: // NOTE for mod and type, we could consider having 1-2 intervals comprising the complementary types (multiple intervals already possible with $in)
                case BSONObj::opTYPE:
                    op = BSONObj::NE; // no bound calculation
                    break;
                case BSONObj::NE:
                    op = BSONObj::Equality;
                    break;
                case BSONObj::LT:
                    op = BSONObj::GTE;
                    break;
                case BSONObj::LTE:
                    op = BSONObj::GT;
                    break;
                case BSONObj::GT:
                    op = BSONObj::LTE;
                    break;
                case BSONObj::GTE:
                    op = BSONObj::LT;
                    break;
                default: // otherwise doesn't matter
                    break;
            }
        }
        switch( op ) {
        case BSONObj::Equality:
            lower = upper = e;
            break;
        case BSONObj::LT:
            upperInclusive = false;
        case BSONObj::LTE:
            upper = e;
            break;
        case BSONObj::GT:
            lowerInclusive = false;
        case BSONObj::GTE:
            lower = e;
            break;
        case BSONObj::opALL: {
            massert( 10370 ,  "$all requires array", e.type() == Array );
            BSONObjIterator i( e.embeddedObject() );
            bool bound = false;
            while ( i.more() ){
                BSONElement x = i.next();
                if ( x.type() == Object && x.embeddedObject().firstElement().getGtLtOp() == BSONObj::opELEM_MATCH ){
                    // taken care of elsewhere
                }
                else if ( x.type() != RegEx ) {
                    lower = upper = x;
                    bound = true;
                    break;
                }
            }
            if ( !bound ) { // if no good non regex bound found, try regex bounds
                BSONObjIterator i( e.embeddedObject() );
                while( i.more() ) {
                    BSONElement x = i.next();
                    if ( x.type() != RegEx )
                        continue;
                    string simple = simpleRegex( x.regex(), x.regexFlags() );
                    if ( !simple.empty() ) {
                        lower = addObj( BSON( "" << simple ) ).firstElement();
                        upper = addObj( BSON( "" << simpleRegexEnd( simple ) ) ).firstElement();
                        break;
                    }
                }
            }
            break;
        }
        case BSONObj::opMOD: {
            {
                BSONObjBuilder b;
                b.appendMinForType( "" , NumberDouble );
                lower = addObj( b.obj() ).firstElement();
            }
            {
                BSONObjBuilder b;
                b.appendMaxForType( "" , NumberDouble );
                upper = addObj( b.obj() ).firstElement();
            }            
            break;
        }
        case BSONObj::opTYPE: {
            BSONType t = (BSONType)e.numberInt();
            {
                BSONObjBuilder b;
                b.appendMinForType( "" , t );
                lower = addObj( b.obj() ).firstElement();
            }
            {
                BSONObjBuilder b;
                b.appendMaxForType( "" , t );
                upper = addObj( b.obj() ).firstElement();
            }
            
            break;
        }
        case BSONObj::opREGEX:
        case BSONObj::opOPTIONS:
            // do nothing
            break;
        case BSONObj::opELEM_MATCH: {
            log() << "warning: shouldn't get here?" << endl;
            break;
        }
        case BSONObj::opNEAR:
        case BSONObj::opWITHIN:
            _special = "2d";
            break;
        default:
            break;
        }
        
        if ( optimize ){
            if ( lower.type() != MinKey && upper.type() == MaxKey && lower.isSimpleType() ){ // TODO: get rid of isSimpleType
                BSONObjBuilder b;
                b.appendMaxForType( lower.fieldName() , lower.type() );
                upper = addObj( b.obj() ).firstElement();
            }
            else if ( lower.type() == MinKey && upper.type() != MaxKey && upper.isSimpleType() ){ // TODO: get rid of isSimpleType
                BSONObjBuilder b;
                b.appendMinForType( upper.fieldName() , upper.type() );
                lower = addObj( b.obj() ).firstElement();
            }
        }

    }

    // as called, these functions find the max/min of a bound in the
    // opposite direction, so inclusive bounds are considered less
    // superlative
    FieldBound maxFieldBound( const FieldBound &a, const FieldBound &b ) {
        int cmp = a.bound_.woCompare( b.bound_, false );
        if ( ( cmp == 0 && !b.inclusive_ ) || cmp < 0 )
            return b;
        return a;
    }

    FieldBound minFieldBound( const FieldBound &a, const FieldBound &b ) {
        int cmp = a.bound_.woCompare( b.bound_, false );
        if ( ( cmp == 0 && !b.inclusive_ ) || cmp > 0 )
            return b;
        return a;
    }

    bool fieldIntervalOverlap( const FieldInterval &one, const FieldInterval &two, FieldInterval &result ) {
        result.lower_ = maxFieldBound( one.lower_, two.lower_ );
        result.upper_ = minFieldBound( one.upper_, two.upper_ );
        return result.valid();
    }
    
	// NOTE Not yet tested for complex $or bounds, just for simple bounds generated by $in
    const FieldRange &FieldRange::operator&=( const FieldRange &other ) {
        vector< FieldInterval > newIntervals;
        vector< FieldInterval >::const_iterator i = intervals_.begin();
        vector< FieldInterval >::const_iterator j = other.intervals_.begin();
        while( i != intervals_.end() && j != other.intervals_.end() ) {
            FieldInterval overlap;
            if ( fieldIntervalOverlap( *i, *j, overlap ) )
                newIntervals.push_back( overlap );
            if ( i->upper_ == minFieldBound( i->upper_, j->upper_ ) )
                ++i;
            else
                ++j;      
        }
        intervals_ = newIntervals;
        for( vector< BSONObj >::const_iterator i = other.objData_.begin(); i != other.objData_.end(); ++i )
            objData_.push_back( *i );
        if ( _special.size() == 0 && other._special.size() )
            _special = other._special;
        return *this;
    }
    
    void handleInterval( const FieldInterval &lower, FieldBound &low, FieldBound &high, vector< FieldInterval > &newIntervals ) {
        if ( low.bound_.eoo() ) {
            low = lower.lower_; high = lower.upper_;
        } else {
            if ( high.bound_.woCompare( lower.lower_.bound_, false ) < 0 ) { // when equal but neither inclusive, just assume they overlap, since current btree scanning code just as efficient either way
                FieldInterval tmp;
                tmp.lower_ = low;
                tmp.upper_ = high;
                newIntervals.push_back( tmp );
                low = lower.lower_; high = lower.upper_;                    
            } else {
                high = lower.upper_;
            }
        }        
    }
    
    const FieldRange &FieldRange::operator|=( const FieldRange &other ) {
        vector< FieldInterval > newIntervals;
        FieldBound low;
        FieldBound high;
        vector< FieldInterval >::const_iterator i = intervals_.begin();
        vector< FieldInterval >::const_iterator j = other.intervals_.begin();
        while( i != intervals_.end() && j != other.intervals_.end() ) {
            int cmp = i->lower_.bound_.woCompare( j->lower_.bound_, false );
            if ( ( cmp == 0 && i->lower_.inclusive_ ) || cmp < 0 ) {
                handleInterval( *i, low, high, newIntervals );
                ++i;
            } else {
                handleInterval( *j, low, high, newIntervals );
                ++j;
            } 
        }
        while( i != intervals_.end() ) {
            handleInterval( *i, low, high, newIntervals );
            ++i;            
        }
        while( j != other.intervals_.end() ) {
            handleInterval( *j, low, high, newIntervals );
            ++j;            
        }
        FieldInterval tmp;
        tmp.lower_ = low;
        tmp.upper_ = high;
        newIntervals.push_back( tmp );        
        intervals_ = newIntervals;
        for( vector< BSONObj >::const_iterator i = other.objData_.begin(); i != other.objData_.end(); ++i )
            objData_.push_back( *i );
        if ( _special.size() == 0 && other._special.size() )
            _special = other._special;
        return *this;        
    }
    
    BSONObj FieldRange::addObj( const BSONObj &o ) {
        objData_.push_back( o );
        return o;
    }
    
    string FieldRangeSet::getSpecial() const {
        string s = "";
        for ( map<string,FieldRange>::iterator i=ranges_.begin(); i!=ranges_.end(); i++ ){
            if ( i->second.getSpecial().size() == 0 )
                continue;
            uassert( 13033 , "can't have 2 special fields" , s.size() == 0 );
            s = i->second.getSpecial();
        }
        return s;
    }

    void FieldRangeSet::processOpElement( const char *fieldName, const BSONElement &f, bool isNot, bool optimize ) {
        BSONElement g = f;
        int op2 = g.getGtLtOp();
        if ( op2 == BSONObj::opALL ) {
            BSONElement h = g;
            massert( 13050 ,  "$all requires array", h.type() == Array );
            BSONObjIterator i( h.embeddedObject() );
            if( i.more() ) {
                BSONElement x = i.next();
                if ( x.type() == Object && x.embeddedObject().firstElement().getGtLtOp() == BSONObj::opELEM_MATCH ) {
                    g = x.embeddedObject().firstElement();
                    op2 = g.getGtLtOp();
                }
            }
        }
        if ( op2 == BSONObj::opELEM_MATCH ) {
            BSONObjIterator k( g.embeddedObjectUserCheck() );
            while ( k.more() ){
                BSONElement h = k.next();
                StringBuilder buf(32);
                buf << fieldName << "." << h.fieldName();
                string fullname = buf.str();
                
                int op3 = getGtLtOp( h );
                if ( op3 == BSONObj::Equality ){
                    ranges_[ fullname ] &= FieldRange( h , isNot , optimize );
                }
                else {
                    BSONObjIterator l( h.embeddedObject() );
                    while ( l.more() ){
                        ranges_[ fullname ] &= FieldRange( l.next() , isNot , optimize );
                    }
                }
            }                        
        } else {
            ranges_[ fieldName ] &= FieldRange( f , isNot , optimize );
        }        
    }
    
    FieldRangeSet::FieldRangeSet( const char *ns, const BSONObj &query , bool optimize )
        : ns_( ns ), query_( query.getOwned() ) {
        BSONObjIterator i( query_ );
        
        while( i.more() ) {
            BSONElement e = i.next();
            // e could be x:1 or x:{$gt:1}

            if ( strcmp( e.fieldName(), "$where" ) == 0 )
                continue;

            bool equality = ( getGtLtOp( e ) == BSONObj::Equality );
            if ( equality && e.type() == Object ) {
                equality = ( strcmp( e.embeddedObject().firstElement().fieldName(), "$not" ) != 0 );
            }
            
            if ( equality || ( e.type() == Object && !e.embeddedObject()[ "$regex" ].eoo() ) ) {
                ranges_[ e.fieldName() ] &= FieldRange( e , false , optimize );
            }
            if ( !equality ) {
                BSONObjIterator j( e.embeddedObject() );
                while( j.more() ) {
                    BSONElement f = j.next();
                    if ( strcmp( f.fieldName(), "$not" ) == 0 ) {
                        switch( f.type() ) {
                            case Object: {
                                BSONObjIterator k( f.embeddedObject() );
                                while( k.more() ) {
                                    BSONElement g = k.next();
                                    uassert( 13034, "invalid use of $not", g.getGtLtOp() != BSONObj::Equality );
                                    processOpElement( e.fieldName(), g, true, optimize );
                                }
                                break;
                            }
                            case RegEx:
                                processOpElement( e.fieldName(), f, true, optimize );
                                break;
                            default:
                                uassert( 13041, "invalid use of $not", false );
                        }
                    } else {
                        processOpElement( e.fieldName(), f, false, optimize );
                    }
                }                
            }
        }
    }

    FieldRange *FieldRangeSet::trivialRange_ = 0;
    FieldRange &FieldRangeSet::trivialRange() {
        if ( trivialRange_ == 0 )
            trivialRange_ = new FieldRange();
        return *trivialRange_;
    }
    
    BSONObj FieldRangeSet::simplifiedQuery( const BSONObj &_fields ) const {
        BSONObj fields = _fields;
        if ( fields.isEmpty() ) {
            BSONObjBuilder b;
            for( map< string, FieldRange >::const_iterator i = ranges_.begin(); i != ranges_.end(); ++i ) {
                b.append( i->first.c_str(), 1 );
            }
            fields = b.obj();
        }
        BSONObjBuilder b;
        BSONObjIterator i( fields );
        while( i.more() ) {
            BSONElement e = i.next();
            const char *name = e.fieldName();
            const FieldRange &range = ranges_[ name ];
            assert( !range.empty() );
            if ( range.equality() )
                b.appendAs( range.min(), name );
            else if ( range.nontrivial() ) {
                BSONObjBuilder c;
                if ( range.min().type() != MinKey )
                    c.appendAs( range.min(), range.minInclusive() ? "$gte" : "$gt" );
                if ( range.max().type() != MaxKey )
                    c.appendAs( range.max(), range.maxInclusive() ? "$lte" : "$lt" );
                b.append( name, c.done() );                
            }
        }
        return b.obj();
    }
    
    QueryPattern FieldRangeSet::pattern( const BSONObj &sort ) const {
        QueryPattern qp;
        for( map< string, FieldRange >::const_iterator i = ranges_.begin(); i != ranges_.end(); ++i ) {
            assert( !i->second.empty() );
            if ( i->second.equality() ) {
                qp.fieldTypes_[ i->first ] = QueryPattern::Equality;
            } else if ( i->second.nontrivial() ) {
                bool upper = i->second.max().type() != MaxKey;
                bool lower = i->second.min().type() != MinKey;
                if ( upper && lower )
                    qp.fieldTypes_[ i->first ] = QueryPattern::UpperAndLowerBound;
                else if ( upper )
                    qp.fieldTypes_[ i->first ] = QueryPattern::UpperBound;
                else if ( lower )
                    qp.fieldTypes_[ i->first ] = QueryPattern::LowerBound;                    
            }
        }
        qp.setSort( sort );
        return qp;
    }
    
    BoundList FieldRangeSet::indexBounds( const BSONObj &keyPattern, int direction ) const {
        BSONObjBuilder equalityBuilder;
        typedef vector< pair< shared_ptr< BSONObjBuilder >, shared_ptr< BSONObjBuilder > > > BoundBuilders;
        BoundBuilders builders;
        BSONObjIterator i( keyPattern );
        while( i.more() ) {
            BSONElement e = i.next();
            const FieldRange &fr = range( e.fieldName() );
            int number = (int) e.number(); // returns 0.0 if not numeric
            bool forward = ( ( number >= 0 ? 1 : -1 ) * ( direction >= 0 ? 1 : -1 ) > 0 );
            if ( builders.empty() ) {
                if ( fr.equality() ) {
                    equalityBuilder.appendAs( fr.min(), "" );
                } else {
                    BSONObj equalityObj = equalityBuilder.done();
                    const vector< FieldInterval > &intervals = fr.intervals();
                    if ( forward ) {
                        for( vector< FieldInterval >::const_iterator j = intervals.begin(); j != intervals.end(); ++j ) {
                            builders.push_back( make_pair( shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ), shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ) ) );
                            builders.back().first->appendElements( equalityObj );
                            builders.back().second->appendElements( equalityObj );
                            builders.back().first->appendAs( j->lower_.bound_, "" );
                            builders.back().second->appendAs( j->upper_.bound_, "" );
                        }
                    } else {
                        for( vector< FieldInterval >::const_reverse_iterator j = intervals.rbegin(); j != intervals.rend(); ++j ) {
                            builders.push_back( make_pair( shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ), shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ) ) );
                            builders.back().first->appendElements( equalityObj );
                            builders.back().second->appendElements( equalityObj );
                            builders.back().first->appendAs( j->upper_.bound_, "" );
                            builders.back().second->appendAs( j->lower_.bound_, "" );
                        }                       
                    }
                }
            } else {
                for( BoundBuilders::const_iterator j = builders.begin(); j != builders.end(); ++j ) {
                    j->first->appendAs( forward ? fr.min() : fr.max(), "" );
                    j->second->appendAs( forward ? fr.max() : fr.min(), "" );
                }
            }
        }
        if ( builders.empty() ) {
            BSONObj equalityObj = equalityBuilder.done();
            assert( !equalityObj.isEmpty() );
            builders.push_back( make_pair( shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ), shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ) ) );
            builders.back().first->appendElements( equalityObj );
            builders.back().second->appendElements( equalityObj );            
        }
        BoundList ret;
        for( BoundBuilders::const_iterator i = builders.begin(); i != builders.end(); ++i )
            ret.push_back( make_pair( i->first->obj(), i->second->obj() ) );
        return ret;
    }

    ///////////////////
    // FieldMatcher //
    ///////////////////
    
    void FieldMatcher::add( const BSONObj& o ){
        massert( 10371 , "can only add to FieldMatcher once", _source.isEmpty());
        _source = o;

        BSONObjIterator i( o );
        int true_false = -1;
        while ( i.more() ){
            BSONElement e = i.next();
            add (e.fieldName(), e.trueValue());

            // validate input
            if (true_false == -1){
                true_false = e.trueValue();
                _include = !e.trueValue();
            }
            else{
                uassert( 10053 , "You cannot currently mix including and excluding fields. Contact us if this is an issue." , 
                         (bool)true_false == e.trueValue() );
            }
        }
    }

    void FieldMatcher::add(const string& field, bool include){
        if (field.empty()){ // this is the field the user referred to
            _include = include;
        } else {
            const size_t dot = field.find('.');
            const string subfield = field.substr(0,dot);
            const string rest = (dot == string::npos ? "" : field.substr(dot+1,string::npos)); 

            boost::shared_ptr<FieldMatcher>& fm = _fields[subfield];
            if (!fm)
                fm.reset(new FieldMatcher(!include));

            fm->add(rest, include);
        }
    }

    BSONObj FieldMatcher::getSpec() const{
        return _source;
    }

    //b will be the value part of an array-typed BSONElement
    void FieldMatcher::appendArray( BSONObjBuilder& b , const BSONObj& a ) const {
        int i=0;
        BSONObjIterator it(a);
        while (it.more()){
            BSONElement e = it.next();

            switch(e.type()){
                case Array:{
                    BSONObjBuilder subb;
                    appendArray(subb , e.embeddedObject());
                    b.appendArray(b.numStr(i++).c_str(), subb.obj());
                    break;
                }
                case Object:{
                    BSONObjBuilder subb;
                    BSONObjIterator jt(e.embeddedObject());
                    while (jt.more()){
                        append(subb , jt.next());
                    }
                    b.append(b.numStr(i++), subb.obj());
                    break;
                }
                default:
                    if (_include)
                        b.appendAs(e, b.numStr(i++).c_str());
            }
            

        }
    }

    void FieldMatcher::append( BSONObjBuilder& b , const BSONElement& e ) const {
        FieldMap::const_iterator field = _fields.find( e.fieldName() );
        
        if (field == _fields.end()){
            if (_include)
                b.append(e);
        } 
        else {
            FieldMatcher& subfm = *field->second;
            
            if (subfm._fields.empty() || !(e.type()==Object || e.type()==Array) ){
                if (subfm._include)
                    b.append(e);
            }
            else if (e.type() == Object){ 
                BSONObjBuilder subb;
                BSONObjIterator it(e.embeddedObject());
                while (it.more()){
                    subfm.append(subb, it.next());
                }
                b.append(e.fieldName(), subb.obj());

            } 
            else { //Array
                BSONObjBuilder subb;
                subfm.appendArray(subb, e.embeddedObject());
                b.appendArray(e.fieldName(), subb.obj());
            }
        }
    }
    
    struct SimpleRegexUnitTest : UnitTest {
        void run(){
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^foo");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "foo" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f?oo");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^fz?oo");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f", "");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f", "m");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "f" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f", "mi");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f \t\vo\n\ro  \\ \\# #comment", "mx");
                BSONObj o = b.done();
                assert( simpleRegex(o.firstElement()) == "foo #" );
            }
        }
    } simple_regex_unittest;
} // namespace mongo
