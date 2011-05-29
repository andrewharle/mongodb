// @file dur_recover.cpp crash recovery via the journal

/**
*    Copyright (C) 2009 10gen Inc.
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

#include "pch.h"

#include "dur.h"
#include "dur_recover.h"
#include "dur_journal.h"
#include "dur_journalformat.h"
#include "durop.h"
#include "namespace.h"
#include "../util/mongoutils/str.h"
#include "../util/bufreader.h"
#include "pdfile.h"
#include "database.h"
#include "db.h"
#include "../util/unittest.h"
#include "cmdline.h"
#include "curop.h"
#include "mongommf.h"

#include <sys/stat.h>
#include <fcntl.h>

using namespace mongoutils;

namespace mongo {

    namespace dur {

        struct ParsedJournalEntry { /*copyable*/
            ParsedJournalEntry() : e(0) { }

            // relative path of database for the operation.
            // might be a pointer into mmaped Journal file
            const char *dbName;

            // thse are pointers into the memory mapped journal file
            const JEntry *e;  // local db sentinel is already parsed out here into dbName

            // if not one of the two simple JEntry's above, this is the operation:
            shared_ptr<DurOp> op;
        };

        void removeJournalFiles();
        path getJournalDir();

        /** get journal filenames, in order. throws if unexpected content found */
        static void getFiles(path dir, vector<path>& files) {
            map<unsigned,path> m;
            for ( filesystem::directory_iterator i( dir );
                    i != filesystem::directory_iterator();
                    ++i ) {
                filesystem::path filepath = *i;
                string fileName = filesystem::path(*i).leaf();
                if( str::startsWith(fileName, "j._") ) {
                    unsigned u = str::toUnsigned( str::after(fileName, '_') );
                    if( m.count(u) ) {
                        uasserted(13531, str::stream() << "unexpected files in journal directory " << dir.string() << " : " << fileName);
                    }
                    m.insert( pair<unsigned,path>(u,filepath) );
                }
            }
            for( map<unsigned,path>::iterator i = m.begin(); i != m.end(); ++i ) {
                if( i != m.begin() && m.count(i->first - 1) == 0 ) {
                    uasserted(13532,
                    str::stream() << "unexpected file in journal directory " << dir.string()
                      << " : " << filesystem::path(i->second).leaf() << " : can't find its preceeding file");
                }
                files.push_back(i->second);
            }
        }

        /** read through the memory mapped data of a journal file (journal/j._<n> file)
            throws
        */
        class JournalSectionIterator : boost::noncopyable {
        public:
            JournalSectionIterator(const void *p, unsigned len, bool doDurOps)
                : _br(p, len)
                , _sectHead(static_cast<const JSectHeader*>(_br.skip(sizeof(JSectHeader))))
                , _lastDbName(NULL)
                , _doDurOps(doDurOps)
            {}

            bool atEof() const { return _br.atEof(); }

            unsigned long long seqNumber() const { return _sectHead->seqNumber; }

            /** get the next entry from the log.  this function parses and combines JDbContext and JEntry's.
             *  @return true if got an entry.  false at successful end of section (and no entry returned).
             *  throws on premature end of section.
             */
            bool next(ParsedJournalEntry& e) {
                unsigned lenOrOpCode;
                _br.read(lenOrOpCode);

                if (lenOrOpCode > JEntry::OpCode_Min) {
                    switch( lenOrOpCode ) {

                    case JEntry::OpCode_Footer: {
                        if (_doDurOps) {
                            const char* pos = (const char*) _br.pos();
                            pos -= sizeof(lenOrOpCode); // rewind to include OpCode
                            const JSectFooter& footer = *(const JSectFooter*)pos;
                            int len = pos - (char*)_sectHead;
                            if (!footer.checkHash(_sectHead, len)) {
                                massert(13594, str::stream() << "Journal checksum doesn't match. recorded: "
                                        << toHex(footer.hash, sizeof(footer.hash))
                                        << " actual: " << md5simpledigest(_sectHead, len)
                                        , false);
                            }
                        }
                        return false; // false return value denotes end of section
                    }

                    case JEntry::OpCode_FileCreated:
                    case JEntry::OpCode_DropDb: {
                        e.dbName = 0;
                        boost::shared_ptr<DurOp> op = DurOp::read(lenOrOpCode, _br);
                        if (_doDurOps) {
                            e.op = op;
                        }
                        return true;
                    }

                    case JEntry::OpCode_DbContext: {
                        _lastDbName = (const char*) _br.pos();
                        const unsigned limit = std::min((unsigned)Namespace::MaxNsLen, _br.remaining());
                        const unsigned len = strnlen(_lastDbName, limit);
                        massert(13533, "problem processing journal file during recovery", _lastDbName[len] == '\0');
                        _br.skip(len+1); // skip '\0' too
                        _br.read(lenOrOpCode);
                    }
                    // fall through as a basic operation always follows jdbcontext, and we don't have anything to return yet

                    default:
                        // fall through
                        ;
                    }
                }

                // JEntry - a basic write
                assert( lenOrOpCode && lenOrOpCode < JEntry::OpCode_Min );
                _br.rewind(4);
                e.e = (JEntry *) _br.skip(sizeof(JEntry));
                e.dbName = e.e->isLocalDbContext() ? "local" : _lastDbName;
                assert( e.e->len == lenOrOpCode );
                _br.skip(e.e->len);
                return true;
            }
        private:
            BufReader _br;
            const JSectHeader* _sectHead;
            const char *_lastDbName; // pointer into mmaped journal file
            const bool _doDurOps;
        };

        static string fileName(const char* dbName, int fileNo) {
            stringstream ss;
            ss << dbName << '.';
            assert( fileNo >= 0 );
            if( fileNo == JEntry::DotNsSuffix )
                ss << "ns";
            else
                ss << fileNo;

            // relative name -> full path name
            path full(dbpath);
            full /= ss.str();
            return full.string();
        }

        RecoveryJob::~RecoveryJob() {
            DESTRUCTOR_GUARD(
                if( !_mmfs.empty() )
                    close();
            )
        }

        void RecoveryJob::close() {
            scoped_lock lk(_mx);
            _close();
        }

        void RecoveryJob::_close() {
            MongoFile::flushAll(true);
            _mmfs.clear();
        }

        void RecoveryJob::write(const ParsedJournalEntry& entry) {
            const string fn = fileName(entry.dbName, entry.e->getFileNo());
            MongoFile* file;
            {
                MongoFileFinder finder; // must release lock before creating new MongoMMF
                file = finder.findByPath(fn);
            }

            MongoMMF* mmf;
            if (file) {
                assert(file->isMongoMMF());
                mmf = (MongoMMF*)file;
            }
            else {
                assert(_recovering);
                boost::shared_ptr<MongoMMF> sp (new MongoMMF);
                assert(sp->open(fn, false));
                _mmfs.push_back(sp);
                mmf = sp.get();
            }

            if ((entry.e->ofs + entry.e->len) <= mmf->length()) {
                void* dest = (char*)mmf->view_write() + entry.e->ofs;
                memcpy(dest, entry.e->srcData(), entry.e->len);
            }
            else {
                massert(13622, "Trying to write past end of file in WRITETODATAFILES", _recovering);
            }
        }

        void RecoveryJob::applyEntry(const ParsedJournalEntry& entry, bool apply, bool dump) {
            if( entry.e ) {
                if( dump ) {
                    stringstream ss;
                    ss << "  BASICWRITE " << setw(20) << entry.dbName << '.';
                    if( entry.e->isNsSuffix() )
                        ss << "ns";
                    else
                        ss << setw(2) << entry.e->getFileNo();
                    ss << ' ' << setw(6) << entry.e->len << ' ' << /*hex << setw(8) << (size_t) fqe.srcData << dec <<*/
                       "  " << hexdump(entry.e->srcData(), entry.e->len);
                    log() << ss.str() << endl;
                }
                if( apply ) {
                    write(entry);
                }
            }
            else if(entry.op) {
                // a DurOp subclass operation
                if( dump ) {
                    log() << "  OP " << entry.op->toString() << endl;
                }
                if( apply ) {
                    if( entry.op->needFilesClosed() ) {
                        _close(); // locked in processSection
                    }
                    entry.op->replay();
                }
            }
        }

        void RecoveryJob::applyEntries(const vector<ParsedJournalEntry> &entries) {
            bool apply = (cmdLine.durOptions & CmdLine::DurScanOnly) == 0;
            bool dump = cmdLine.durOptions & CmdLine::DurDumpJournal;
            if( dump )
                log() << "BEGIN section" << endl;

            for( vector<ParsedJournalEntry>::const_iterator i = entries.begin(); i != entries.end(); ++i ) {
                applyEntry(*i, apply, dump);
            }

            if( dump )
                log() << "END section" << endl;
        }

        void RecoveryJob::processSection(const void *p, unsigned len) {
            scoped_lock lk(_mx);

            vector<ParsedJournalEntry> entries;
            JournalSectionIterator i(p, len, _recovering);

            //DEV log() << "recovery processSection seq:" << i.seqNumber() << endl;
            if( _recovering && _lastDataSyncedFromLastRun > i.seqNumber() + ExtraKeepTimeMs ) {
                if( i.seqNumber() != _lastSeqMentionedInConsoleLog ) {
                    log() << "recover skipping application of section seq:" << i.seqNumber() << " < lsn:" << _lastDataSyncedFromLastRun << endl;
                    _lastSeqMentionedInConsoleLog = i.seqNumber();
                }
                return;
            }

            // first read all entries to make sure this section is valid
            ParsedJournalEntry e;
            while( i.next(e) ) {
                entries.push_back(e);
            }

            // got all the entries for one group commit.  apply them:
            applyEntries(entries);
        }

        /** apply a specific journal file, that is already mmap'd
            @param p start of the memory mapped file
            @return true if this is detected to be the last file (ends abruptly)
        */
        bool RecoveryJob::processFileBuffer(const void *p, unsigned len) {
            try {
                unsigned long long fileId;
                BufReader br(p,len);

                {
                    // read file header
                    JHeader h;
                    br.read(h);
                    if( !h.versionOk() ) {
                        log() << "journal file version number mismatch. recover with old version of mongod, terminate cleanly, then upgrade." << endl;
                        uasserted(13536, str::stream() << "journal version number mismatch " << h._version);
                    }
                    uassert(13537, "journal header invalid", h.valid());
                    fileId = h.fileId;
                    if(cmdLine.durOptions & CmdLine::DurDumpJournal) { 
                        log() << "JHeader::fileId=" << fileId << endl;
                    }
                }

                // read sections
                while ( !br.atEof() ) {
                    JSectHeader h;
                    br.peek(h);
                    if( h.fileId != fileId ) {
                        if( debug || (cmdLine.durOptions & CmdLine::DurDumpJournal) ) {
                            log() << "Ending processFileBuffer at differing fileId want:" << fileId << " got:" << h.fileId << endl;
                            log() << "  sect len:" << h.len << " seqnum:" << h.seqNumber << endl;
                        }
                        return true;
                    }
                    processSection(br.skip(h.len), h.len);

                    // ctrl c check
                    killCurrentOp.checkForInterrupt(false);
                }
            }
            catch( BufReader::eof& ) {
                if( cmdLine.durOptions & CmdLine::DurDumpJournal )
                    log() << "ABRUPT END" << endl;
                return true; // abrupt end
            }

            return false; // non-abrupt end
        }

        /** apply a specific journal file */
        bool RecoveryJob::processFile(path journalfile) {
            log() << "recover " << journalfile.string() << endl;
            MemoryMappedFile f;
            void *p = f.mapWithOptions(journalfile.string().c_str(), MongoFile::READONLY | MongoFile::SEQUENTIAL);
            massert(13544, str::stream() << "recover error couldn't open " << journalfile.string(), p);
            return processFileBuffer(p, (unsigned) f.length());
        }

        /** @param files all the j._0 style files we need to apply for recovery */
        void RecoveryJob::go(vector<path>& files) {
            log() << "recover begin" << endl;
            _recovering = true;

            // load the last sequence number synced to the datafiles on disk before the last crash
            _lastDataSyncedFromLastRun = journalReadLSN();
            log() << "recover lsn: " << _lastDataSyncedFromLastRun << endl;

            for( unsigned i = 0; i != files.size(); ++i ) {
	      /*bool abruptEnd = */processFile(files[i]);
                /*if( abruptEnd && i+1 < files.size() ) {
                    log() << "recover error: abrupt end to file " << files[i].string() << ", yet it isn't the last journal file" << endl;
                    close();
                    uasserted(13535, "recover abrupt journal file end");
                }*/
            }

            close();

            if( cmdLine.durOptions & CmdLine::DurScanOnly ) {
                uasserted(13545, str::stream() << "--durOptions " << (int) CmdLine::DurScanOnly << " (scan only) specified");
            }

            log() << "recover cleaning up" << endl;
            removeJournalFiles();
            log() << "recover done" << endl;
            okToCleanUp = true;
            _recovering = false;
        }

        void _recover() {
            assert( cmdLine.dur );

            filesystem::path p = getJournalDir();
            if( !exists(p) ) {
                log() << "directory " << p.string() << " does not exist, there will be no recovery startup step" << endl;
                okToCleanUp = true;
                return;
            }

            vector<path> journalFiles;
            getFiles(p, journalFiles);

            if( journalFiles.empty() ) {
                log() << "recover : no journal files present, no recovery needed" << endl;
                okToCleanUp = true;
                return;
            }

            RecoveryJob::get().go(journalFiles);
        }

        extern mutex groupCommitMutex;

        /** recover from a crash
            called during startup
            throws on error
        */
        void recover() {
            // we use a lock so that exitCleanly will wait for us
            // to finish (or at least to notice what is up and stop)
            writelock lk;

            // this is so the mutexdebugger doesn't get confused.  we are actually single threaded 
            // at this point in the program so it wouldn't have been a true problem (I think)
            scoped_lock lk2(groupCommitMutex);

            _recover(); // throws on interruption
        }

        struct BufReaderY { int a,b; };
        class BufReaderUnitTest : public UnitTest {
        public:
            void run() {
                BufReader r((void*) "abcdabcdabcd", 12);
                char x;
                BufReaderY y;
                r.read(x); //cout << x; // a
                assert( x == 'a' );
                r.read(y);
                r.read(x);
                assert( x == 'b' );
            }
        } brunittest;

        // can't free at termination because order of destruction of global vars is arbitrary
        RecoveryJob &RecoveryJob::_instance = *(new RecoveryJob());

    } // namespace dur

} // namespace mongo

