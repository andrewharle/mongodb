// rs_config.h
// repl set configuration
//

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

#include "../../util/net/hostandport.h"
#include "../../util/concurrency/race.h"
#include "health.h"

namespace mongo {
    class Member;
    const string rsConfigNs = "local.system.replset";

    class ReplSetConfig {
        enum { EMPTYCONFIG = -2 };
        struct TagSubgroup;
    public:
        /**
         * This contacts the given host and tries to get a config from them.
         *
         * This sends a test heartbeat to the host and, if all goes well and the
         * host has a more recent config, fetches the config and loads it (see
         * from().
         *
         * If it's contacting itself, it skips the heartbeat (for obvious
         * reasons.) If something is misconfigured, throws an exception. If the
         * host couldn't be queried or is just blank, ok() will be false.
         */
        ReplSetConfig(const HostAndPort& h);

        ReplSetConfig(BSONObj cfg, bool force=false);

        bool ok() const { return _ok; }

        struct TagRule;

        struct MemberCfg {
            MemberCfg() : _id(-1), votes(1), priority(1.0), arbiterOnly(false), slaveDelay(0), hidden(false), buildIndexes(true) { }
            int _id;              /* ordinal */
            unsigned votes;       /* how many votes this node gets. default 1. */
            HostAndPort h;
            double priority;      /* 0 means can never be primary */
            bool arbiterOnly;
            int slaveDelay;       /* seconds.  int rather than unsigned for convenient to/front bson conversion. */
            bool hidden;          /* if set, don't advertise to drives in isMaster. for non-primaries (priority 0) */
            bool buildIndexes;    /* if false, do not create any non-_id indexes */
            map<string,string> tags;     /* tagging for data center, rack, etc. */
        private:
            set<TagSubgroup*> _groups; // the subgroups this member belongs to
        public:
            const set<TagSubgroup*>& groups() const { 
                return _groups;
            }
            set<TagSubgroup*>& groupsw() {
                return _groups;
            }
            void check() const;   /* check validity, assert if not. */
            BSONObj asBson() const;
            bool potentiallyHot() const { return !arbiterOnly && priority > 0; }
            void updateGroups(const OpTime& last) {
                for (set<TagSubgroup*>::iterator it = _groups.begin(); it != _groups.end(); it++) {
                    ((TagSubgroup*)(*it))->updateLast(last);
                }
            }
            bool operator==(const MemberCfg& r) const {
                if (!tags.empty() || !r.tags.empty()) {
                    if (tags.size() != r.tags.size()) {
                        return false;
                    }

                    // if they are the same size and not equal, at least one
                    // element in A must be different in B
                    for (map<string,string>::const_iterator lit = tags.begin(); lit != tags.end(); lit++) {
                        map<string,string>::const_iterator rit = r.tags.find((*lit).first);

                        if (rit == r.tags.end() || (*lit).second != (*rit).second) {
                            return false;
                        }
                    }
                }

                return _id==r._id && votes == r.votes && h == r.h && priority == r.priority &&
                       arbiterOnly == r.arbiterOnly && slaveDelay == r.slaveDelay && hidden == r.hidden &&
                       buildIndexes == buildIndexes;
            }
            bool operator!=(const MemberCfg& r) const { return !(*this == r); }
        };

        vector<MemberCfg> members;
        string _id;
        int version;
        HealthOptions ho;
        string md5;
        BSONObj getLastErrorDefaults;
        map<string,TagRule*> rules;

        list<HostAndPort> otherMemberHostnames() const; // except self

        /** @return true if could connect, and there is no cfg object there at all */
        bool empty() const { return version == EMPTYCONFIG; }

        string toString() const { return asBson().toString(); }

        /** validate the settings. does not call check() on each member, you have to do that separately. */
        void checkRsConfig() const;

        /** check if modification makes sense */
        static bool legalChange(const ReplSetConfig& old, const ReplSetConfig& n, string& errmsg);

        //static void receivedNewConfig(BSONObj);
        void saveConfigLocally(BSONObj comment); // to local db
        string saveConfigEverywhere(); // returns textual info on what happened

        /**
         * Update members' groups when the config changes but members stay the same.
         */
        void updateMembers(List1<Member> &dest);

        BSONObj asBson() const;

        bool _constructed;
    private:
        bool _ok;
        void from(BSONObj);
        void clear();

        struct TagClause;

        /**
         * This is a logical grouping of servers.  It is pointed to by a set of
         * servers with a certain tag.
         *
         * For example, suppose servers A, B, and C have the tag "dc" : "nyc". If we
         * have a rule {"dc" : 2}, then we want A _or_ B _or_ C to have the
         * write for one of the "dc" critiria to be fulfilled, so all three will
         * point to this subgroup. When one of their oplog-tailing cursors is
         * updated, this subgroup is updated.
         */
        struct TagSubgroup : boost::noncopyable {
            ~TagSubgroup(); // never called; not defined
            TagSubgroup(string nm) : name(nm) { }
            const string name;
            OpTime last;
            vector<TagClause*> clauses;

            // this probably won't actually point to valid members after the
            // subgroup is created, as initFromConfig() makes a copy of the
            // config
            set<MemberCfg*> m;

            void updateLast(const OpTime& op);

            //string toString() const;

            /**
             * If two tags have the same name, they should compare as equal so
             * that members don't have to update two identical groups on writes.
             */
            bool operator() (TagSubgroup& lhs, TagSubgroup& rhs) const {
                return lhs.name < rhs.name;
            }
        };

        /**
         * An argument in a rule.  For example, if we had the rule {dc : 2,
         * machines : 3}, "dc" : 2 and "machines" : 3 would be two TagClauses.
         *
         * Each tag clause has a set of associated subgroups.  For example, if
         * we had "dc" : 2, our subgroups might be "nyc", "sf", and "hk".
         */
        struct TagClause {
            OpTime last;
            map<string,TagSubgroup*> subgroups;
            TagRule *rule;
            string name;
            /**
             * If we have get a clause like {machines : 3} and this server is
             * tagged with "machines", then it's really {machines : 2}, as we
             * will always be up-to-date.  So, target would be 3 and
             * actualTarget would be 2, in that example.
             */
            int target;
            int actualTarget;

            void updateLast(const OpTime& op);
            string toString() const;
        };

        /**
         * Parses getLastErrorModes.
         */
        void parseRules(const BSONObj& modes);

        /**
         * Create a  hash containing every possible clause that could be used in a
         * rule and the servers related to that clause.
         *
         * For example, suppose we have the following servers:
         * A {"dc" : "ny", "ny" : "rk1"}
         * B {"dc" : "ny", "ny" : "rk1"}
         * C {"dc" : "ny", "ny" : "rk2"}
         * D {"dc" : "sf", "sf" : "rk1"}
         * E {"dc" : "sf", "sf" : "rk2"}
         *
         * This would give us the possible criteria:
         * "dc" -> {A, B, C},{D, E}
         * "ny" -> {A, B},{C}
         * "sf" -> {D},{E}
         */
        void _populateTagMap(map<string,TagClause> &tagMap);

    public:
        struct TagRule {
            vector<TagClause*> clauses;
            OpTime last;

            void updateLast(const OpTime& op);
            string toString() const;
        };
    };

}
