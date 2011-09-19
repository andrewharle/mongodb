// stringutils.h

/*    Copyright 2010 10gen Inc.
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

namespace mongo {

    // see also mongoutils/str.h - perhaps move these there?
    // see also text.h

    void splitStringDelim( const string& str , vector<string>* res , char delim );

    void joinStringDelim( const vector<string>& strs , string* res , char delim );

    inline string tolowerString( const string& input ) {
        string::size_type sz = input.size();

        boost::scoped_array<char> line(new char[sz+1]);
        char * copy = line.get();

        for ( string::size_type i=0; i<sz; i++ ) {
            char c = input[i];
            copy[i] = (char)tolower( (int)c );
        }
        copy[sz] = 0;
        return string(copy);
    }

    /**
     * Non numeric characters are compared lexicographically; numeric substrings
     * are compared numerically; dots separate ordered comparable subunits.
     * For convenience, character 255 is greater than anything else.
     */
    inline int lexNumCmp( const char *s1, const char *s2 ) {
        //cout << "START : " << s1 << "\t" << s2 << endl;

        bool startWord = true;
        
        while( *s1 && *s2 ) {

            bool d1 = ( *s1 == '.' );
            bool d2 = ( *s2 == '.' );
            if ( d1 && !d2 )
             	return -1;
            if ( d2 && !d1 )
             	return 1;
            if ( d1 && d2 ) {
             	++s1; ++s2;
                startWord = true;
                continue;
            }
            
            bool p1 = ( *s1 == (char)255 );
            bool p2 = ( *s2 == (char)255 );
            //cout << "\t\t " << p1 << "\t" << p2 << endl;
            if ( p1 && !p2 )
                return 1;
            if ( p2 && !p1 )
                return -1;

            bool n1 = isNumber( *s1 );
            bool n2 = isNumber( *s2 );

            if ( n1 && n2 ) {
                // get rid of leading 0s
                if ( startWord ) {
                    while ( *s1 == '0' ) s1++;
                    while ( *s2 == '0' ) s2++;
                }

                char * e1 = (char*)s1;
                char * e2 = (char*)s2;

                // find length
                // if end of string, will break immediately ('\0')
                while ( isNumber (*e1) ) e1++;
                while ( isNumber (*e2) ) e2++;

                int len1 = (int)(e1-s1);
                int len2 = (int)(e2-s2);

                int result;
                // if one is longer than the other, return
                if ( len1 > len2 ) {
                    return 1;
                }
                else if ( len2 > len1 ) {
                    return -1;
                }
                // if the lengths are equal, just strcmp
                else if ( (result = strncmp(s1, s2, len1)) != 0 ) {
                    return result;
                }

                // otherwise, the numbers are equal
                s1 = e1;
                s2 = e2;
                startWord = false;
                continue;
            }
            
            if ( n1 )
                return 1;

            if ( n2 )
                return -1;

            if ( *s1 > *s2 )
                return 1;

            if ( *s2 > *s1 )
                return -1;
            
            s1++; s2++;
            startWord = false;
        }

        if ( *s1 )
            return 1;
        if ( *s2 )
            return -1;
        return 0;
    }

} // namespace mongo
