#import <Foundation/Foundation.h>

#import "YapAbstractDatabase.h"
#import "YapAbstractDatabaseConnection.h"
#import "YapAbstractDatabaseTransaction.h"

#import "YapDatabaseConnectionState.h"
#import "YapSharedCache.h"

#import "sqlite3.h"

/**
 * Do we use a dedicated background thread/queue to run checkpoint operations?
 *
 * If YES (1), then auto-checkpoint is disabled on all connections.
 * A dedicated background connection runs checkpoint operations after transactions complete.
 *
 * If NO (0), then auto-checkpoint is enabled on all connections.
 * And the typical auto-checkpoint operations are run during commit operations of read-write transactions.
 *
 * If YES, then write operations will complete faster (but the WAL may grow faster).
 * If NO, then write operations will complete slower (but the WAL stays slim).
 *
 * A large size WAL seems to have some kind of negative performance during app launch.
**/
#define YAP_DATABASE_USE_CHECKPOINT_QUEUE 0

/**
 * Helper method to conditionally invoke sqlite3_finalize on a statement, and then set the ivar to NULL.
**/
NS_INLINE void sqlite_finalize_null(sqlite3_stmt **stmtPtr)
{
	if (*stmtPtr) {
		sqlite3_finalize(*stmtPtr);
		*stmtPtr = NULL;
	}
}


@interface YapAbstractDatabase () {
@private
	dispatch_queue_t snapshotQueue;
	dispatch_queue_t writeQueue;
#if YAP_DATABASE_USE_CHECKPOINT_QUEUE
	dispatch_queue_t checkpointQueue;
#endif
	
	NSMutableArray *connectionStates;
	NSMutableArray *changesets;
	NSTimeInterval lastWriteTimestamp;
	
#if YAP_DATABASE_USE_CHECKPOINT_QUEUE
	int walPendingPageCount;
	int32_t walCheckpointSchedule;
#endif
	
@protected
	
	sqlite3 *db;
	
	void *IsOnSnapshotQueueKey;
	
@public
	
	YapSharedCache *sharedObjectCache;
	YapSharedCache *sharedMetadataCache;
}

/**
 * Many of the methods below are only accessible from within the snapshotQueue.
 *
 * This queue is used to synchronize access to variables related to acquiring "snapshots"
 * of a particular state of the database.
**/
@property (nonatomic, readonly) dispatch_queue_t snapshotQueue;

/**
 * All read-write transactions must go through this serial queue.
 *
 * In sqlite there can only be a single writer at a time.
 * We enforce this externally to avoid busy errors and to help keep yap-level contructs synchronized.
**/
@property (nonatomic, readonly) dispatch_queue_t writeQueue;

/**
 * Required override hook.
 * Don't forget to invoke [super createTables].
**/
- (BOOL)createTables;

/**
 * Upgrade mechanism.
**/
- (BOOL)get_user_version:(int *)user_version_ptr;

/**
 * Optional override hook.
 * Don't forget to invoke [super prepare].
 * 
 * This method is run asynchronously on the snapshotQueue.
**/
- (void)prepare;

/**
 * Use the addConnection method from within newConnection.
 *
 * And when a connection is deallocated,
 * it should remove itself from the list of connections by calling removeConnection.
**/
- (void)addConnection:(YapAbstractDatabaseConnection *)connection;
- (void)removeConnection:(YapAbstractDatabaseConnection *)connection;

/**
 * REQUIRED OVERRIDE METHOD
 *
 * This method is used to generate the changeset block used with YapSharedCache & YapSharedCacheConnection.
 * The given changeset comes directly from a readwrite transaction.
 * 
 * The output block should return one of the following:
 * 
 *  0 if the changeset indicates the key/value pair was unchanged.
 * -1 if the changeset indicates the key/value pair was deleted.
 * +1 if the changeset indicates the key/value pair was modified.
**/
- (int (^)(id key))cacheChangesetBlockFromChanges:(NSDictionary *)changeset;

/**
 * This method is only accessible from within the snapshotQueue.
 * 
 * The lastWriteTimestamp represents the last time the database was modified by a read-write transaction.
 * This information isn persisted to the 'yap' database, and is separately held in memory.
 * It serves multiple purposes.
 * 
 * First is assists in validation of a connection's cache.
 * When a connection begins a new transaction, it may have items sitting in the cache.
 * However the connection doesn't know if the items are still valid because another connection may have made changes.
 * The cache is valid if the lastWriteTimestamp hasn't changed since the connection's last transaction.
 * Otherwise the entire cache should be invalidated / flushed.
 * 
 * The lastWriteTimestamp also assists in correcting for a rare race condition.
 * It order to minimize blocking we allow read-write transactions to commit outside the context
 * of the snapshotQueue. This is because the commit may be a time consuming operation, and we
 * don't want to block read-only transactions during this period. The race condition occurs if a read-only
 * transactions starts in the midst of a read-write commit, and the read-only transaction gets
 * a "yap-level" snapshot that's out of sync with the "sql-level" snapshot. This is easily correctable if caught.
 * Thus we maintain the lastWriteTimestamp in memory, and fetchable via a select query.
 * One represents the "yap-level" snapshot, and the other represents the "sql-level" snapshot.
 *
 * The timestamp comes from the [NSProcessInfo systemUptime]. Thus it can never decrease.
 * It is reset when the YapDatabase instance is initialized, and updated by each read-write transaction.
**/
- (NSTimeInterval)lastWriteTimestamp;

/**
 * This method is only accessible from within the snapshotQueue.
 * 
 * A transaction must update its state in accordance with the state transaction rules
**/
- (void)enumerateConnectionStates:(void (^)(YapDatabaseConnectionState *state))block;

/**
 * This method is only accessible from within the snapshotQueue.
 * 
 * Prior to starting the sqlite commit, the connection must report its changeset to the database.
 * The database will store the changeset, and provide it to other connections if needed (due to a race condition).
 * 
 * The following MUST be in the dictionary:
 *
 * - lastWriteTimestamp : NSNumber double with the changeset's timestamp
**/
- (void)notePendingChanges:(NSDictionary *)changeset fromConnection:(YapAbstractDatabaseConnection *)connection;

/**
 * This method is only accessible from within the snapshotQueue.
 * 
 * This method is used if a transaction finds itself in a race condition.
 * That is, the transaction started before it was able to process changesets from sibling connections.
 * 
 * It should fetch the changesets needed and then process them via [connection noteCommittedChanges:].
**/
- (NSArray *)pendingAndCommittedChangesSince:(NSTimeInterval)connectionTimestamp until:(NSTimeInterval)maxTimestamp;

/**
 * This method is only accessible from within the snapshotQueue.
 * 
 * Upon completion of a readwrite transaction, the connection must report its changeset to the database.
 * The database will then forward the changeset to all other connections.
 * 
 * The following MUST be in the dictionary:
 * 
 * - lastWriteTimestamp : NSNumber double with the changeset's timestamp
**/
- (void)noteCommittedChanges:(NSDictionary *)changeset fromConnection:(YapAbstractDatabaseConnection *)connection;

#if YAP_DATABASE_USE_CHECKPOINT_QUEUE

/**
 * All checkpointing is done on a low-priority background thread.
 * We checkpoint continuously to keep the WAL-index small.
**/
- (void)maybeRunCheckpointInBackground;
- (void)runCheckpointInBackground;

/**
 * Primarily for debugging.
**/
- (void)syncCheckpoint;

#endif

@end

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#pragma mark -
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

@interface YapAbstractDatabaseConnection () {
@private
	sqlite3_stmt *beginTransactionStatement;
	sqlite3_stmt *commitTransactionStatement;
	
	sqlite3_stmt *yapGetDataForKeyStatement; // Against "yap" database, for internal use
	sqlite3_stmt *yapSetDataForKeyStatement; // Against "yap" database, for internal use
	
@protected
	dispatch_queue_t connectionQueue;
	void *IsOnConnectionQueueKey;
	
	YapAbstractDatabase *database;
	
	NSTimeInterval cacheLastWriteTimestamp;
	
@public
	sqlite3 *db;
	
	YapSharedCacheConnection *objectCache;
	YapSharedCacheConnection *metadataCache;
	
	NSUInteger objectCacheLimit;          // Read-only by transaction. Use as consideration of whether to add to cache.
	NSUInteger metadataCacheLimit;        // Read-only by transaction. Use as consideration of whether to add to cache.
	
	BOOL hasMarkedSqlLevelSharedReadLock; // Read-only by transaction. Use as consideration of whether to invoke method.
}

- (id)initWithDatabase:(YapAbstractDatabase *)database;

@property (nonatomic, readonly) dispatch_queue_t connectionQueue;

- (void)_flushMemoryWithLevel:(int)level;

- (sqlite3_stmt *)beginTransactionStatement;
- (sqlite3_stmt *)commitTransactionStatement;

- (void)_readWithBlock:(void (^)(id))block;
- (void)_readWriteWithBlock:(void (^)(id))block;

- (void)_asyncReadWithBlock:(void (^)(id))block
            completionBlock:(dispatch_block_t)completionBlock
            completionQueue:(dispatch_queue_t)completionQueue;

- (void)_asyncReadWriteWithBlock:(void (^)(id))block
                 completionBlock:(dispatch_block_t)completionBlock
                 completionQueue:(dispatch_queue_t)completionQueue;

- (YapAbstractDatabaseTransaction *)newReadTransaction;
- (YapAbstractDatabaseTransaction *)newReadWriteTransaction;

- (void)preReadTransaction:(YapAbstractDatabaseTransaction *)transaction;
- (void)postReadTransaction:(YapAbstractDatabaseTransaction *)transaction;

- (void)preReadWriteTransaction:(YapAbstractDatabaseTransaction *)transaction;
- (void)postReadWriteTransaction:(YapAbstractDatabaseTransaction *)transaction;

- (void)markSqlLevelSharedReadLockAcquired;

- (NSMutableDictionary *)changeset;
- (void)noteCommittedChanges:(NSDictionary *)changeset;

@end

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#pragma mark -
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

@interface YapAbstractDatabaseTransaction () {
@protected
	
	__unsafe_unretained YapAbstractDatabaseConnection *abstractConnection;
}

- (id)initWithConnection:(YapAbstractDatabaseConnection *)connection;

- (void)beginTransaction;
- (void)commitTransaction;

@end
