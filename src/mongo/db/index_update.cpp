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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/db/index_update.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/btreebuilder.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/extsort.h"
#include "mongo/db/storage/index_details.h"
#include "mongo/db/index/btree_based_builder.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/pdfile_private.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/runner_yield_policy.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/structure/collection.h"
#include "mongo/util/processinfo.h"

namespace mongo {

    /**
     * Add the provided (obj, dl) pair to the provided index.
     */
    static void addKeysToIndex(const char *ns, NamespaceDetails *d, int idxNo, const BSONObj& obj,
                               const DiskLoc &recordLoc, bool dupsAllowed) {
        IndexDetails& id = d->idx(idxNo);
        auto_ptr<IndexDescriptor> desc(CatalogHack::getDescriptor(d, idxNo));
        auto_ptr<IndexAccessMethod> iam(CatalogHack::getIndex(desc.get()));
        InsertDeleteOptions options;
        options.logIfError = false;
        options.dupsAllowed = (!KeyPattern::isIdKeyPattern(id.keyPattern()) && !id.unique())
            || ignoreUniqueIndex(id);

        int64_t inserted;
        Status ret = iam->insert(obj, recordLoc, options, &inserted);
        if (Status::OK() != ret) {
            uasserted(ret.location(), ret.reason());
        }
    }

    //
    // Bulk index building
    //

    class BackgroundIndexBuildJob : public BackgroundOperation {

        unsigned long long addExistingToIndex(const char *ns, NamespaceDetails *d,
                                              IndexDetails& idx) {
            bool dupsAllowed = !idx.unique();
            bool dropDups = idx.dropDups();

            ProgressMeter& progress = cc().curop()->setMessage("bg index build",
                                                               "Background Index Build Progress",
                                                               d->numRecords());

            unsigned long long n = 0;
            unsigned long long numDropped = 0;

            auto_ptr<Runner> runner(InternalPlanner::collectionScan(ns));
            // We're not delegating yielding to the runner because we need to know when a yield
            // happens.
            RunnerYieldPolicy yieldPolicy;

            std::string idxName = idx.indexName();
            int idxNo = d->findIndexByName( idxName, true );
            verify( idxNo >= 0 );

            // After this yields in the loop, idx may point at a different index (if indexes get
            // flipped, see insert_makeIndex) or even an empty IndexDetails, so nothing below should
            // depend on idx. idxNo should be recalculated after each yield.

            BSONObj js;
            DiskLoc loc;
            while (Runner::RUNNER_ADVANCED == runner->getNext(&js, &loc)) {
                try {
                    if ( !dupsAllowed && dropDups ) {
                        LastError::Disabled led( lastError.get() );
                        addKeysToIndex(ns, d, idxNo, js, loc, dupsAllowed);
                    }
                    else {
                        addKeysToIndex(ns, d, idxNo, js, loc, dupsAllowed);
                    }
                }
                catch( AssertionException& e ) {
                    if( e.interrupted() ) {
                        killCurrentOp.checkForInterrupt();
                    }

                    // TODO: Does exception really imply dropDups exception?
                    if (dropDups) {
                        bool runnerEOF = runner->isEOF();
                        runner->saveState();
                        theDataFileMgr.deleteRecord(d, ns, loc.rec(), loc, false, true, true);
                        if (!runner->restoreState()) {
                            // Runner got killed somehow.  This probably shouldn't happen.
                            if (runnerEOF) {
                                // Quote: "We were already at the end.  Normal.
                                // TODO: Why is this normal?
                            }
                            else {
                                uasserted(12585, "cursor gone during bg index; dropDups");
                            }
                            break;
                        }
                        // We deleted a record, but we didn't actually yield the dblock.
                        // TODO: Why did the old code assume we yielded the lock?
                        numDropped++;
                    }
                    else {
                        log() << "background addExistingToIndex exception " << e.what() << endl;
                        throw;
                    }
                }

                n++;
                progress.hit();

                getDur().commitIfNeeded();
                if (yieldPolicy.shouldYield()) {
                    if (!yieldPolicy.yieldAndCheckIfOK(runner.get())) {
                        uasserted(12584, "cursor gone during bg index");
                        break;
                    }

                    progress.setTotalWhileRunning( d->numRecords() );
                    // Recalculate idxNo if we yielded
                    idxNo = d->findIndexByName( idxName, true );
                    verify( idxNo >= 0 );
                }
            }

            progress.finished();
            if ( dropDups )
                log() << "\t backgroundIndexBuild dupsToDrop: " << numDropped << endl;
            return n;
        }

        /* we do set a flag in the namespace for quick checking, but this is our authoritative info -
           that way on a crash/restart, we don't think we are still building one. */
        set<NamespaceDetails*> bgJobsInProgress;

        void prep(const char *ns, NamespaceDetails *d) {
            Lock::assertWriteLocked(ns);
            uassert( 13130 , "can't start bg index b/c in recursive lock (db.eval?)" , !Lock::nested() );
            bgJobsInProgress.insert(d);
        }
        void done(const char *ns) {
            Lock::assertWriteLocked(ns);

            // clear query optimizer cache
            Collection* collection = cc().database()->getCollection( ns );
            if ( collection )
                collection->infoCache()->addedIndex();
        }

    public:
        BackgroundIndexBuildJob(const char *ns) : BackgroundOperation(ns) { }

        unsigned long long go(string ns, NamespaceDetails *d, IndexDetails& idx) {

            // clear cached things since we are changing state
            // namely what fields are indexed
            Collection* collection = cc().database()->getCollection( ns );
            if ( collection )
                collection->infoCache()->addedIndex();

            unsigned long long n = 0;

            prep(ns.c_str(), d);
            try {
                idx.head.writing() = BtreeBasedBuilder::makeEmptyIndex(idx);
                n = addExistingToIndex(ns.c_str(), d, idx);
                // idx may point at an invalid index entry at this point
            }
            catch(...) {
                if( cc().database() && nsdetails(ns) == d ) {
                    done(ns.c_str());
                }
                else {
                    log() << "ERROR: db gone during bg index?" << endl;
                }
                throw;
            }
            done(ns.c_str());
            return n;
        }
    };

    // throws DBException
    void buildAnIndex(const std::string& ns,
                      NamespaceDetails* d,
                      IndexDetails& idx,
                      bool mayInterrupt) {

        BSONObj idxInfo = idx.info.obj();

        MONGO_TLOG(0) << "build index on: " << ns << " properties: " << idxInfo.jsonString() << endl;

        audit::logCreateIndex( currentClient.get(), &idxInfo, idx.indexName(), ns );

        Timer t;
        unsigned long long n;

        verify( Lock::isWriteLocked(ns) );

        if( inDBRepair || !idxInfo["background"].trueValue() ) {
            int idxNo = d->findIndexByName( idx.info.obj()["name"].valuestr(), true );
            verify( idxNo >= 0 );
            n = BtreeBasedBuilder::fastBuildIndex(ns.c_str(), d, idx, mayInterrupt, idxNo);
            verify( !idx.head.isNull() );
        }
        else {
            BackgroundIndexBuildJob j(ns.c_str());
            n = j.go(ns, d, idx);
        }
        MONGO_TLOG(0) << "build index done.  scanned " << n << " total records. " << t.millis() / 1000.0 << " secs" << endl;
    }

    extern BSONObj id_obj;  // { _id : 1 }

    void ensureHaveIdIndex(const char* ns, bool mayInterrupt) {
        NamespaceDetails *d = nsdetails(ns);
        if ( d == 0 || d->isSystemFlagSet(NamespaceDetails::Flag_HaveIdIndex) )
            return;

        d->setSystemFlag( NamespaceDetails::Flag_HaveIdIndex );

        {
            NamespaceDetails::IndexIterator i = d->ii();
            while( i.more() ) {
                if( i.next().isIdIndex() )
                    return;
            }
        }

        string system_indexes = cc().database()->name() + ".system.indexes";

        BSONObjBuilder b;
        b.append("name", "_id_");
        b.append("ns", ns);
        b.append("key", id_obj);
        BSONObj o = b.done();

        /* edge case: note the insert could fail if we have hit maxindexes already */
        theDataFileMgr.insert(system_indexes.c_str(), o.objdata(), o.objsize(), mayInterrupt, true);
    }

    static bool needToUpgradeMinorVersion(const string& newPluginName) {
        if (IndexNames::existedBefore24(newPluginName))
            return false;

        DataFileHeader* dfh = cc().database()->getFile(0)->getHeader();
        if (dfh->versionMinor == PDFILE_VERSION_MINOR_24_AND_NEWER)
            return false; // these checks have already been done

        fassert(16737, dfh->versionMinor == PDFILE_VERSION_MINOR_22_AND_OLDER);

        return true;
    }

    static void upgradeMinorVersionOrAssert(const string& newPluginName) {
        const string systemIndexes = cc().database()->name() + ".system.indexes";
        auto_ptr<Runner> runner(InternalPlanner::collectionScan(systemIndexes));
        BSONObj index;
        Runner::RunnerState state;
        while (Runner::RUNNER_ADVANCED == (state = runner->getNext(&index, NULL))) {
            const BSONObj key = index.getObjectField("key");
            const string plugin = IndexNames::findPluginName(key);
            if (IndexNames::existedBefore24(plugin))
                continue;

            const string errmsg = str::stream()
                << "Found pre-existing index " << index << " with invalid type '" << plugin << "'. "
                << "Disallowing creation of new index type '" << newPluginName << "'. See "
                << "http://dochub.mongodb.org/core/index-type-changes"
                ;

            error() << errmsg << endl;
            uasserted(16738, errmsg);
        }

        if (Runner::RUNNER_EOF != state) {
            warning() << "Internal error while reading collection " << systemIndexes << endl;
        }

        DataFileHeader* dfh = cc().database()->getFile(0)->getHeader();
        getDur().writingInt(dfh->versionMinor) = PDFILE_VERSION_MINOR_24_AND_NEWER;
    }

    bool prepareToBuildIndex(const BSONObj& io,
                             bool mayInterrupt,
                             bool god,
                             const string& sourceNS ) {

        BSONObj key = io.getObjectField("key");

        /* this is because we want key patterns like { _id : 1 } and { _id : <someobjid> } to
           all be treated as the same pattern.
        */
        if ( IndexDetails::isIdIndexPattern(key) ) {
            if( !god ) {
                ensureHaveIdIndex( sourceNS.c_str(), mayInterrupt );
                return false;
            }
        }
        else {
            /* is buildIndexes:false set for this replica set member?
               if so we don't build any indexes except _id
            */
            if( theReplSet && !theReplSet->buildIndexes() )
                return false;
        }

        string pluginName = IndexNames::findPluginName( key );
        if ( pluginName.size() ) {
            if (needToUpgradeMinorVersion(pluginName))
                upgradeMinorVersionOrAssert(pluginName);
        }

        return true;
    }
}  // namespace mongo

