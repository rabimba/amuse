#import "LocalConnectorsManager.h"
#import "MessagesConnector.h"
#import "ContactsConnector.h"
#import "CalendarConnector.h"
#import "NotesConnector.h"
#import <sqlite3.h>

@implementation LocalConnectorsManager

+ (instancetype)sharedManager {
    static LocalConnectorsManager *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[self alloc] init];
    });
    return shared;
}

- (NSString *)jsonStringFromObject:(id)obj {
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:obj options:NSJSONWritingPrettyPrinted error:&error];
    if (error || !data) {
        return @"{\"error\":\"Failed to serialize response\"}";
    }
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

#pragma mark - Memory (SQLite, shared schema with the Linux build)

- (NSString *)amuseDataDir {
    NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@".amuse"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return dir;
}

- (sqlite3 *)openMemoryDB {
    NSString *dbPath = [[self amuseDataDir] stringByAppendingPathComponent:@"local_data.db"];
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath.UTF8String, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS memories (id INTEGER PRIMARY KEY AUTOINCREMENT, key TEXT UNIQUE, value TEXT, category TEXT, updated_at TEXT);",
                 NULL, NULL, NULL);
    return db;
}

- (NSString *)memoryTimestamp {
    NSDateFormatter *df = [[NSDateFormatter alloc] init];
    df.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    df.dateFormat = @"yyyy-MM-dd HH:mm:ss";
    return [df stringFromDate:[NSDate date]];
}

- (NSString *)saveMemoryKey:(NSString *)key value:(NSString *)value category:(NSString *)category {
    if (!key.length || !value.length) {
        return [self jsonStringFromObject:@{@"error": @"key and value are required"}];
    }
    sqlite3 *db = [self openMemoryDB];
    if (!db) return @"{\"error\":\"cannot open memory database\"}";
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO memories (key, value, category, updated_at) VALUES (?, ?, ?, ?) "
                      "ON CONFLICT(key) DO UPDATE SET value=excluded.value, category=excluded.category, updated_at=excluded.updated_at;";
    NSString *now = [self memoryTimestamp];
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key.UTF8String, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value.UTF8String, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, (category ?: @"preference").UTF8String, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, now.UTF8String, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return [self jsonStringFromObject:@{@"success": @YES, @"key": key, @"category": category ?: @"preference"}];
}

- (NSString *)recallMemoriesWithQuery:(NSString *)query {
    sqlite3 *db = [self openMemoryDB];
    if (!db) return @"{\"error\":\"cannot open memory database\"}";
    NSMutableArray *mems = [NSMutableArray array];
    sqlite3_stmt *stmt = NULL;
    BOOL hasQuery = query.length > 0;
    const char *sql = hasQuery
        ? "SELECT key, value, category, updated_at FROM memories WHERE key LIKE ? OR value LIKE ? ORDER BY id DESC LIMIT 20;"
        : "SELECT key, value, category, updated_at FROM memories ORDER BY id DESC LIMIT 20;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (hasQuery) {
            NSString *pat = [NSString stringWithFormat:@"%%%@%%", query];
            sqlite3_bind_text(stmt, 1, pat.UTF8String, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, pat.UTF8String, -1, SQLITE_TRANSIENT);
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *k = (const char *)sqlite3_column_text(stmt, 0);
            const char *v = (const char *)sqlite3_column_text(stmt, 1);
            const char *c = (const char *)sqlite3_column_text(stmt, 2);
            [mems addObject:@{
                @"key": k ? @(k) : @"",
                @"value": v ? @(v) : @"",
                @"category": c ? @(c) : @""
            }];
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return [self jsonStringFromObject:@{@"memories": mems, @"count": @(mems.count)}];
}

#pragma mark - Clock

- (NSString *)currentTimeAndEnvironment {
    NSDate *now = [NSDate date];
    NSMutableDictionary *zones = [NSMutableDictionary dictionary];
    NSDictionary *zoneMap = @{
        @"pacific_pt": @"America/Los_Angeles",
        @"mountain_mt": @"America/Denver",
        @"central_cst_cdt": @"America/Chicago",
        @"eastern_et": @"America/New_York",
        @"utc": @"UTC"
    };
    for (NSString *label in zoneMap) {
        NSDateFormatter *df = [[NSDateFormatter alloc] init];
        df.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
        df.dateFormat = @"EEE, MMM d yyyy, h:mm:ss a zzz";
        df.timeZone = [NSTimeZone timeZoneWithName:zoneMap[label]];
        zones[label] = [df stringFromDate:now];
    }
    NSDateFormatter *localFmt = [[NSDateFormatter alloc] init];
    localFmt.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    localFmt.dateFormat = @"EEE, MMM d yyyy, h:mm:ss a zzz";
    NSDateFormatter *isoFmt = [[NSDateFormatter alloc] init];
    isoFmt.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    isoFmt.dateFormat = @"yyyy-MM-dd'T'HH:mm:ssZZZZZ";

    return [self jsonStringFromObject:@{
        @"local_time": [localFmt stringFromDate:now],
        @"local_iso": [isoFmt stringFromDate:now],
        @"local_timezone": [[NSTimeZone localTimeZone] name],
        @"unix_epoch": @((long long)[now timeIntervalSince1970]),
        @"time_in_zones": zones,
        @"os": @"macOS",
        @"home_directory": NSHomeDirectory()
    }];
}

#pragma mark - Email (python connector; stdin JSON so secrets never hit argv)

- (NSString *)runEmailConnectorWithJson:(NSString *)argsJson {
    NSString *cwd = [[NSFileManager defaultManager] currentDirectoryPath];
    NSString *script = [cwd stringByAppendingPathComponent:@"scripts/email_connector.py"];
    if (![[NSFileManager defaultManager] fileExistsAtPath:script]) {
        return @"{\"success\":false,\"error\":\"email_connector.py not found — run from the repo root\"}";
    }
    NSArray *pythonCandidates = @[@"/usr/bin/python3", @"/opt/homebrew/bin/python3", @"/usr/local/bin/python3"];
    NSString *python = nil;
    for (NSString *c in pythonCandidates) {
        if ([[NSFileManager defaultManager] fileExistsAtPath:c]) { python = c; break; }
    }
    if (!python) return @"{\"success\":false,\"error\":\"python3 not found\"}";

    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:python];
    task.arguments = @[script, @"-"];
    task.currentDirectoryURL = [NSURL fileURLWithPath:cwd];
    NSPipe *inPipe = [NSPipe pipe];
    NSPipe *outPipe = [NSPipe pipe];
    task.standardInput = inPipe;
    task.standardOutput = outPipe;
    task.standardError = [NSFileHandle fileHandleWithStandardError];

    if (![task launchAndReturnError:nil]) {
        return @"{\"success\":false,\"error\":\"failed to launch email connector\"}";
    }
    [[inPipe fileHandleForWriting] writeData:[argsJson dataUsingEncoding:NSUTF8StringEncoding]];
    [[inPipe fileHandleForWriting] closeFile];

    // 45s watchdog: kill the connector if it hangs (e.g. dead IMAP server)
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(45 * NSEC_PER_SEC)),
                   dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        if (task.isRunning) [task terminate];
    });

    NSData *outData = [[outPipe fileHandleForReading] readDataToEndOfFile];
    [task waitUntilExit];
    NSString *out = [[[NSString alloc] initWithData:outData encoding:NSUTF8StringEncoding]
                     stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (!out.length || ![out hasPrefix:@"{"]) {
        return @"{\"success\":false,\"error\":\"Email connector returned invalid output\"}";
    }
    return out;
}

/// Paths the agent may attach/share: inside $HOME minus sensitive dirs.
- (BOOL)isPathAllowedForSharing:(NSString *)path {
    if (!path.length) return YES;
    NSString *p = [path stringByStandardizingPath];
    NSString *home = NSHomeDirectory();
    if (![p hasPrefix:[home stringByAppendingString:@"/"]]) return NO;
    NSString *rel = [p substringFromIndex:home.length];
    NSArray *deny = @[@"/.ssh", @"/.gnupg", @"/.aws", @"/.amuse", @"/.kube", @"/.docker",
                      @"/.local/share/keyrings", @"/Library"];
    for (NSString *d in deny) {
        if ([rel isEqualToString:d] || [rel hasPrefix:[d stringByAppendingString:@"/"]]) return NO;
    }
    return YES;
}

- (NSString *)emailActionWithArgs:(NSDictionary *)args {
    NSString *action = args[@"action"] ?: @"search";
    if ([action isEqualToString:@"send"]) {
        // Sending real email requires explicit per-call confirmation (matches Linux)
        if (![args[@"confirm_send"] boolValue]) {
            return [self jsonStringFromObject:@{
                @"success": @NO,
                @"status": @"needs_confirmation",
                @"message": @"This email has NOT been sent. Show the user this exact draft and ask for explicit confirmation. Then call email_action again with the same arguments plus confirm_send: true.",
                @"draft": @{
                    @"to": args[@"to"] ?: @"",
                    @"subject": args[@"subject"] ?: @"",
                    @"body": args[@"body"] ?: @"",
                    @"attachment_path": args[@"attachment_path"] ?: @""
                }
            }];
        }
        NSString *attachment = args[@"attachment_path"];
        if (attachment.length && ![self isPathAllowedForSharing:attachment]) {
            return [self jsonStringFromObject:@{
                @"success": @NO,
                @"error": @"attachment_blocked",
                @"message": @"Attachment must be inside the user's home directory and not in a sensitive location (~/.ssh, ~/.gnupg, ~/.aws, ~/.amuse, keyrings, ~/Library)."
            }];
        }
    }
    NSError *err = nil;
    NSData *jsonData = [NSJSONSerialization dataWithJSONObject:args options:0 error:&err];
    if (err || !jsonData) return @"{\"success\":false,\"error\":\"bad arguments\"}";
    return [self runEmailConnectorWithJson:[[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding]];
}

- (NSString *)connectEmailAccountWithArgs:(NSDictionary *)args {
    NSError *err = nil;
    NSMutableDictionary *payload = [args mutableCopy];
    payload[@"action"] = @"connect_account";
    NSData *jsonData = [NSJSONSerialization dataWithJSONObject:payload options:0 error:&err];
    if (err || !jsonData) return @"{\"success\":false,\"error\":\"bad arguments\"}";
    return [self runEmailConnectorWithJson:[[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding]];
}

- (NSString *)listConnectorsJson {
    // Honest macOS registry: everything listed is genuinely implemented here.
    NSArray *connectors = @[
        @{@"id": @"messages", @"name": @"iMessage / Messages", @"category": @"local", @"status": @"connected",
          @"description": @"Reads ~/Library/Messages/chat.db (needs Full Disk Access)"},
        @{@"id": @"contacts", @"name": @"Contacts", @"category": @"local", @"status": @"connected",
          @"description": @"macOS Contacts.framework search (permission required)"},
        @{@"id": @"calendar", @"name": @"Apple Calendar", @"category": @"local", @"status": @"connected",
          @"description": @"EventKit read/write (permission required)"},
        @{@"id": @"reminders", @"name": @"Apple Reminders", @"category": @"local", @"status": @"connected",
          @"description": @"EventKit reminders (permission required)"},
        @{@"id": @"notes", @"name": @"Apple Notes", @"category": @"local", @"status": @"connected",
          @"description": @"Notes via AppleScript (Automation consent required)"},
        @{@"id": @"email", @"name": @"Email (IMAP/SMTP + Thunderbird DB)", @"category": @"local", @"status": @"connected",
          @"description": @"Search hosted IMAP accounts and on-device Thunderbird; send via SMTP with confirmation"},
        @{@"id": @"active_memory", @"name": @"Active Memory", @"category": @"local", @"status": @"connected",
          @"description": @"Persistent memory store in ~/.amuse/local_data.db"},
        @{@"id": @"gmail_oauth", @"name": @"Gmail (OAuth API)", @"category": @"cloud", @"status": @"available",
          @"description": @"Not connected: OAuth connector planned; use connect_email_account with an app password today"},
        @{@"id": @"google_calendar", @"name": @"Google Calendar", @"category": @"cloud", @"status": @"available",
          @"description": @"Not connected: OAuth connector planned"},
        @{@"id": @"slack", @"name": @"Slack", @"category": @"cloud", @"status": @"available",
          @"description": @"Not connected: planned"},
        @{@"id": @"github", @"name": @"GitHub", @"category": @"cloud", @"status": @"available",
          @"description": @"Not connected: planned"},
        @{@"id": @"notion", @"name": @"Notion", @"category": @"cloud", @"status": @"available",
          @"description": @"Not connected: planned"}
    ];
    return [self jsonStringFromObject:@{@"connectors": connectors}];
}


- (NSString *)executeToolWithName:(NSString *)toolName arguments:(NSDictionary *)args {
    @try {
        if ([toolName isEqualToString:@"search_messages"]) {
            NSString *query = args[@"query"];
            NSString *sender = args[@"sender"];
            NSInteger limit = [args[@"limit"] integerValue];
            if (limit <= 0) limit = 15;
            NSArray *res = [[MessagesConnector sharedConnector] searchMessagesWithQuery:query sender:sender limit:limit];
            return [self jsonStringFromObject:@{@"messages": res}];
        }
        else if ([toolName isEqualToString:@"get_recent_messages"]) {
            NSInteger limit = [args[@"limit"] integerValue];
            if (limit <= 0) limit = 15;
            NSArray *res = [[MessagesConnector sharedConnector] recentMessagesWithLimit:limit];
            return [self jsonStringFromObject:@{@"messages": res}];
        }
        else if ([toolName isEqualToString:@"search_contacts"]) {
            NSString *name = args[@"name"];
            NSString *phoneOrEmail = args[@"phone_or_email"];
            if (name && name.length > 0) {
                NSArray *res = [[ContactsConnector sharedConnector] searchContactsWithName:name];
                return [self jsonStringFromObject:@{@"contacts": res}];
            } else if (phoneOrEmail && phoneOrEmail.length > 0) {
                NSArray *res = [[ContactsConnector sharedConnector] searchContactsWithPhoneOrEmail:phoneOrEmail];
                return [self jsonStringFromObject:@{@"contacts": res}];
            } else {
                NSArray *res = [[ContactsConnector sharedConnector] allContactsWithLimit:30];
                return [self jsonStringFromObject:@{@"contacts": res}];
            }
        }
        else if ([toolName isEqualToString:@"get_calendar_events"]) {
            NSInteger days = [args[@"days_ahead"] integerValue];
            if (days <= 0) days = 7;
            NSArray *res = [[CalendarConnector sharedConnector] getEventsDaysAhead:days];
            return [self jsonStringFromObject:@{@"events": res}];
        }
        else if ([toolName isEqualToString:@"create_calendar_event"]) {
            NSString *title = args[@"title"] ?: @"Untitled";
            NSString *notes = args[@"notes"];
            NSString *location = args[@"location"];
            
            NSDateFormatter *df = [[NSDateFormatter alloc] init];
            df.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
            df.dateFormat = @"yyyy-MM-dd'T'HH:mm:ssZZZZZ";
            
            NSDate *start = args[@"start_iso"] ? [df dateFromString:args[@"start_iso"]] : nil;
            if (!start) {
                return [self jsonStringFromObject:@{@"error": @"Invalid or missing start_iso. Use full ISO 8601 with timezone offset, e.g. 2026-10-15T09:00:00-07:00"}];
            }
            NSDate *end = args[@"end_iso"] ? [df dateFromString:args[@"end_iso"]] : nil;
            if (!end) end = [start dateByAddingTimeInterval:3600];
            
            NSDictionary *res = [[CalendarConnector sharedConnector] createEventWithTitle:title startDate:start endDate:end notes:notes location:location];
            return [self jsonStringFromObject:res];
        }
        else if ([toolName isEqualToString:@"get_reminders"]) {
            BOOL includeCompleted = [args[@"include_completed"] boolValue];
            NSArray *res = [[CalendarConnector sharedConnector] getRemindersIncludeCompleted:includeCompleted];
            return [self jsonStringFromObject:@{@"reminders": res}];
        }
        else if ([toolName isEqualToString:@"create_reminder"]) {
            NSString *title = args[@"title"] ?: @"Reminder";
            NSString *notes = args[@"notes"];
            NSDate *dueDate = nil;
            if (args[@"due_iso"]) {
                NSDateFormatter *df = [[NSDateFormatter alloc] init];
                df.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
                df.dateFormat = @"yyyy-MM-dd'T'HH:mm:ssZZZZZ";
                dueDate = [df dateFromString:args[@"due_iso"]];
                if (!dueDate) {
                    return [self jsonStringFromObject:@{@"error": @"Invalid due_iso. Use full ISO 8601 with timezone offset, e.g. 2026-10-15T09:00:00-07:00"}];
                }
            }
            NSDictionary *res = [[CalendarConnector sharedConnector] createReminderWithTitle:title dueDate:dueDate notes:notes];
            return [self jsonStringFromObject:res];
        }
        else if ([toolName isEqualToString:@"search_notes"]) {
            NSString *query = args[@"query"];
            NSInteger limit = [args[@"limit"] integerValue];
            if (limit <= 0) limit = 10;
            NSArray *res = nil;
            if (query && query.length > 0) {
                res = [[NotesConnector sharedConnector] searchNotesWithQuery:query limit:limit];
            } else {
                res = [[NotesConnector sharedConnector] recentNotesWithLimit:limit];
            }
            return [self jsonStringFromObject:@{@"notes": res}];
        }
        else if ([toolName isEqualToString:@"create_note"]) {
            NSString *title = args[@"title"] ?: @"New Note";
            NSString *body = args[@"body"] ?: @"";
            NSDictionary *res = [[NotesConnector sharedConnector] createNoteWithTitle:title body:body];
            return [self jsonStringFromObject:res];
        }
        else if ([toolName isEqualToString:@"get_current_time_and_environment"]) {
            return [self currentTimeAndEnvironment];
        }
        else if ([toolName isEqualToString:@"save_memory"]) {
            return [self saveMemoryKey:args[@"key"] value:args[@"value"] category:args[@"category"]];
        }
        else if ([toolName isEqualToString:@"recall_memories"]) {
            return [self recallMemoriesWithQuery:args[@"query"]];
        }
        else if ([toolName isEqualToString:@"email_action"]) {
            return [self emailActionWithArgs:args];
        }
        else if ([toolName isEqualToString:@"connect_email_account"]) {
            return [self connectEmailAccountWithArgs:args];
        }
        else if ([toolName isEqualToString:@"list_connectors"]) {
            return [self listConnectorsJson];
        }
        else {
            return [self jsonStringFromObject:@{@"error": [NSString stringWithFormat:@"Unknown tool: %@", toolName]}];
        }
    } @catch (NSException *e) {
        return [self jsonStringFromObject:@{@"error": [NSString stringWithFormat:@"Exception executing tool: %@", e.reason]}];
    }
}

- (NSArray<NSDictionary *> *)geminiToolDeclarations {
    return @[
        @{
            @"name": @"search_messages",
            @"description": @"Searches the user's iMessage database (~/Library/Messages/chat.db) for text content and/or sender telephone or email handle.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"query": @{ @"type": @"STRING", @"description": @"Search text keyword to find in messages" },
                    @"sender": @{ @"type": @"STRING", @"description": @"Optional phone number, email, or contact handle" },
                    @"limit": @{ @"type": @"INTEGER", @"description": @"Maximum number of messages to return (default 15)" }
                }
            }
        },
        @{
            @"name": @"get_recent_messages",
            @"description": @"Fetches the user's most recent incoming and outgoing iMessages.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"limit": @{ @"type": @"INTEGER", @"description": @"Maximum messages to fetch (default 15)" }
                }
            }
        },
        @{
            @"name": @"search_contacts",
            @"description": @"Searches macOS Contacts by name, phone number, or email to identify people, phone numbers, or addresses.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"name": @{ @"type": @"STRING", @"description": @"Contact name or partial name query" },
                    @"phone_or_email": @{ @"type": @"STRING", @"description": @"Phone number or email address to resolve to a contact" }
                }
            }
        },
        @{
            @"name": @"get_calendar_events",
            @"description": @"Retrieves the user's scheduled calendar events for the upcoming days from Apple Calendar.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"days_ahead": @{ @"type": @"INTEGER", @"description": @"Number of days ahead to search (default 7)" }
                }
            }
        },
        @{
            @"name": @"create_calendar_event",
            @"description": @"Creates a new event on the user's Apple Calendar.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"title": @{ @"type": @"STRING", @"description": @"Title of the event" },
                    @"start_iso": @{ @"type": @"STRING", @"description": @"Start time in ISO-8601 format (e.g. 2026-10-15T14:00:00-07:00)" },
                    @"end_iso": @{ @"type": @"STRING", @"description": @"End time in ISO-8601 format" },
                    @"location": @{ @"type": @"STRING", @"description": @"Optional location" },
                    @"notes": @{ @"type": @"STRING", @"description": @"Optional notes or description" }
                },
                @"required": @[@"title", @"start_iso"]
            }
        },
        @{
            @"name": @"get_reminders",
            @"description": @"Retrieves the user's to-do items from Apple Reminders.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"include_completed": @{ @"type": @"BOOLEAN", @"description": @"Whether to include completed tasks (default false)" }
                }
            }
        },
        @{
            @"name": @"create_reminder",
            @"description": @"Creates a new to-do reminder in Apple Reminders.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"title": @{ @"type": @"STRING", @"description": @"Reminder task text" },
                    @"due_iso": @{ @"type": @"STRING", @"description": @"Optional due date in ISO-8601 format" },
                    @"notes": @{ @"type": @"STRING", @"description": @"Optional notes" }
                },
                @"required": @[@"title"]
            }
        },
        @{
            @"name": @"search_notes",
            @"description": @"Searches the user's Apple Notes for notes matching a search phrase or retrieves recent notes.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"query": @{ @"type": @"STRING", @"description": @"Search keyword or title" },
                    @"limit": @{ @"type": @"INTEGER", @"description": @"Max notes to return (default 10)" }
                }
            }
        },
        @{
            @"name": @"create_note",
            @"description": @"Creates a new note in Apple Notes.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"title": @{ @"type": @"STRING", @"description": @"Title of the note" },
                    @"body": @{ @"type": @"STRING", @"description": @"Body text or content of the note" }
                },
                @"required": @[@"title", @"body"]
            }
        },
        @{
            @"name": @"get_current_time_and_environment",
            @"description": @"Returns the exact current local date, time, timezone, and times across US Pacific/Mountain/Central/Eastern and UTC. Call this whenever checking current time or timezone differences.",
            @"parameters": @{ @"type": @"OBJECT", @"properties": @{} }
        },
        @{
            @"name": @"save_memory",
            @"description": @"Saves a persistent fact, preference, or detail about the user to long-term memory.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"key": @{ @"type": @"STRING", @"description": @"Short camelCase key for the memory" },
                    @"value": @{ @"type": @"STRING", @"description": @"The fact or preference to remember" },
                    @"category": @{ @"type": @"STRING", @"description": @"personal | preference | work | project | health | finance | other" }
                },
                @"required": @[@"key", @"value"]
            }
        },
        @{
            @"name": @"recall_memories",
            @"description": @"Recalls long-term memories and facts about the user. Call at the start of a session or before personalized tasks.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"query": @{ @"type": @"STRING", @"description": @"Optional search keyword to filter memories" }
                }
            }
        },
        @{
            @"name": @"email_action",
            @"description": @"Searches, reads, drafts, or sends real emails via connected hosted IMAP/SMTP accounts (~/.amuse/email_accounts.json) and any on-device Thunderbird database. Actions: search, check, read_latest, list_accounts, draft, compose, send, open_composer. Sending requires confirm_send: true after showing the user the exact draft.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"action": @{ @"type": @"STRING", @"description": @"search | check | read_latest | list_accounts | draft | compose | send | open_composer" },
                    @"query": @{ @"type": @"STRING", @"description": @"Search keyword for search/check" },
                    @"to": @{ @"type": @"STRING", @"description": @"Recipient email (for draft/compose/send)" },
                    @"subject": @{ @"type": @"STRING", @"description": @"Subject (for draft/compose/send)" },
                    @"body": @{ @"type": @"STRING", @"description": @"Body (for draft/compose/send)" },
                    @"attachment_path": @{ @"type": @"STRING", @"description": @"Optional local file to attach (must be inside the user's home, not in sensitive dirs)" },
                    @"confirm_send": @{ @"type": @"BOOLEAN", @"description": @"Must be true to actually send; otherwise returns a draft preview for user confirmation" }
                },
                @"required": @[@"action"]
            }
        },
        @{
            @"name": @"connect_email_account",
            @"description": @"Connects a hosted IMAP/SMTP email account (Gmail with App Password, iCloud, Outlook, custom). The connection is verified with a live IMAP login before saving.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"email": @{ @"type": @"STRING", @"description": @"Email address" },
                    @"password": @{ @"type": @"STRING", @"description": @"Password or app-specific password" },
                    @"provider": @{ @"type": @"STRING", @"description": @"gmail | icloud | outlook | yahoo | fastmail | custom" },
                    @"imap_host": @{ @"type": @"STRING", @"description": @"Custom IMAP host (optional)" },
                    @"smtp_host": @{ @"type": @"STRING", @"description": @"Custom SMTP host (optional)" }
                },
                @"required": @[@"email", @"password"]
            }
        },
        @{
            @"name": @"list_connectors",
            @"description": @"Lists available local and cloud connectors and their real connection status.",
            @"parameters": @{
                @"type": @"OBJECT",
                @"properties": @{
                    @"category": @{ @"type": @"STRING", @"description": @"Optional filter: local | cloud | all" }
                }
            }
        }
    ];
}

@end
