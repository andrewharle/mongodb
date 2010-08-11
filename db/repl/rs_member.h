// @file rsmember.h
/*
 *    Copyright (C) 2010 10gen Inc.
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

/** replica set member */

#pragma once

namespace mongo {


    /*
        RS_STARTUP    serving still starting up, or still trying to initiate the set
        RS_PRIMARY    this server thinks it is primary
        RS_SECONDARY  this server thinks it is a secondary (slave mode)
        RS_RECOVERING recovering/resyncing; after recovery usually auto-transitions to secondary
        RS_FATAL      something bad has occurred and server is not completely offline with regard to the replica set.  fatal error.
        RS_STARTUP2   loaded config, still determining who is primary
    */
    struct MemberState { 
        enum MS { 
            RS_STARTUP,
            RS_PRIMARY,
            RS_SECONDARY,
            RS_RECOVERING,
            RS_FATAL,
            RS_STARTUP2,
            RS_UNKNOWN, /* remote node not yet reached */
            RS_ARBITER,
            RS_DOWN /* node not reachable for a report */
        } s;

        MemberState(MS ms = RS_UNKNOWN) : s(ms) { }
        explicit MemberState(int ms) : s((MS) ms) { }

        bool primary() const { return s == RS_PRIMARY; }
        bool secondary() const { return s == RS_SECONDARY; }
        bool recovering() const { return s == RS_RECOVERING; }
        bool startup2() const { return s == RS_STARTUP2; }
        bool fatal() const { return s == RS_FATAL; }

        bool operator==(const MemberState& r) const { return s == r.s; }
        bool operator!=(const MemberState& r) const { return s != r.s; }
    };

    /* this is supposed to be just basic information on a member, 
       and copy constructable. */
    class HeartbeatInfo { 
        unsigned _id;
    public:
        HeartbeatInfo() : _id(0xffffffff),skew(INT_MIN) { }
        HeartbeatInfo(unsigned id);
        bool up() const { return health > 0; }
        unsigned id() const { return _id; }
        MemberState hbstate;
        double health;
        time_t upSince;
        time_t lastHeartbeat;
        string lastHeartbeatMsg;
        OpTime opTime;
        int skew;

        /* true if changed in a way of interest to the repl set manager. */
        bool changed(const HeartbeatInfo& old) const;
    };

    inline HeartbeatInfo::HeartbeatInfo(unsigned id) : _id(id) { 
          health = -1.0;
          lastHeartbeat = upSince = 0; 
          skew = INT_MIN;
    }

    inline bool HeartbeatInfo::changed(const HeartbeatInfo& old) const { 
        return health != old.health ||
               hbstate != old.hbstate;
    }

}
