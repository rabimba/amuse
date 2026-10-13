#import "MessagesConnector.h"
#import <sqlite3.h>

@implementation MessagesConnector

+ (instancetype)sharedConnector {
    static MessagesConnector *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[self alloc] init];
    });
    return shared;
}

- (NSString *)databasePath {
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Messages/chat.db"];
}

- (NSArray<NSDictionary *> *)recentMessagesWithLimit:(NSInteger)limit {
    return [self searchMessagesWithQuery:nil sender:nil limit:limit];
}

- (NSArray<NSDictionary *> *)searchMessagesWithQuery:(nullable NSString *)query
                                              sender:(nullable NSString *)sender
                                               limit:(NSInteger)limit {
    NSMutableArray *results = [NSMutableArray array];
    NSString *dbPath = [self databasePath];
    sqlite3 *db = NULL;
    
    if (sqlite3_open_v2([dbPath UTF8String], &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        return @[@{
            @"error": [NSString stringWithFormat:@"Unable to open chat.db: %s. Check Full Disk Access permissions.", sqlite3_errmsg(db)]
        }];
    }
    
    if (limit <= 0) limit = 20;
    if (limit > 100) limit = 100;
    
    NSMutableString *sql = [NSMutableString stringWithString:
        @"SELECT m.text, h.id, m.is_from_me, "
        @"datetime(m.date/1000000000 + strftime('%s', '2001-01-01'), 'unixepoch', 'localtime') as formatted_date, "
        @"m.guid "
        @"FROM message m "
        @"LEFT JOIN handle h ON m.handle_id = h.rowid "
        @"WHERE m.text IS NOT NULL AND length(m.text) > 0 "
    ];
    
    NSMutableArray *bindParams = [NSMutableArray array];
    
    if (query && query.length > 0) {
        [sql appendString:@"AND m.text LIKE ? "];
        [bindParams addObject:[NSString stringWithFormat:@"%%%@%%", query]];
    }
    
    if (sender && sender.length > 0) {
        [sql appendString:@"AND h.id LIKE ? "];
        [bindParams addObject:[NSString stringWithFormat:@"%%%@%%", sender]];
    }
    
    [sql appendFormat:@"ORDER BY m.date DESC LIMIT %ld;", (long)limit];
    
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, [sql UTF8String], -1, &stmt, NULL) == SQLITE_OK) {
        for (int i = 0; i < bindParams.count; i++) {
            sqlite3_bind_text(stmt, i + 1, [bindParams[i] UTF8String], -1, SQLITE_TRANSIENT);
        }
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *textRaw = (const char *)sqlite3_column_text(stmt, 0);
            const char *senderRaw = (const char *)sqlite3_column_text(stmt, 1);
            int isFromMe = sqlite3_column_int(stmt, 2);
            const char *dateRaw = (const char *)sqlite3_column_text(stmt, 3);
            const char *guidRaw = (const char *)sqlite3_column_text(stmt, 4);
            
            NSString *text = textRaw ? [NSString stringWithUTF8String:textRaw] : @"";
            NSString *senderStr = senderRaw ? [NSString stringWithUTF8String:senderRaw] : (isFromMe ? @"Me" : @"Unknown");
            NSString *dateStr = dateRaw ? [NSString stringWithUTF8String:dateRaw] : @"";
            NSString *guidStr = guidRaw ? [NSString stringWithUTF8String:guidRaw] : @"";
            
            [results addObject:@{
                @"text": text,
                @"sender": senderStr,
                @"is_from_me": @(isFromMe != 0),
                @"date": dateStr,
                @"guid": guidStr
            }];
        }
        sqlite3_finalize(stmt);
    } else {
        [results addObject:@{
            @"error": [NSString stringWithFormat:@"SQL query failed: %s", sqlite3_errmsg(db)]
        }];
    }
    
    sqlite3_close(db);
    return results;
}

@end
