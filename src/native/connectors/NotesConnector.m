#import "NotesConnector.h"

@implementation NotesConnector

+ (instancetype)sharedConnector {
    static NotesConnector *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[self alloc] init];
    });
    return shared;
}

- (NSString *)escapeForAppleScript:(NSString *)string {
    if (!string) return @"";
    NSString *s = [string stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
    s = [s stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
    return s;
}

- (NSArray<NSDictionary *> *)recentNotesWithLimit:(NSInteger)limit {
    if (limit <= 0) limit = 10;
    if (limit > 50) limit = 50;
    
    NSString *scriptSource = [NSString stringWithFormat:
        @"tell application \"Notes\"\n"
        @"  set totalNotes to count of notes\n"
        @"  set takeCount to %ld\n"
        @"  if totalNotes < takeCount then set takeCount to totalNotes\n"
        @"  set res to {}\n"
        @"  repeat with i from 1 to takeCount\n"
        @"    set n to note i\n"
        @"    set noteName to name of n\n"
        @"    set noteDate to (modification date of n as string)\n"
        @"    set noteBody to \"\"\n"
        @"    try\n"
        @"      set noteBody to (paragraph 1 of plaintext of n)\n"
        @"    end try\n"
        @"    set end of res to (noteName & \"<|||>\" & noteDate & \"<|||>\" & noteBody)\n"
        @"  end repeat\n"
        @"  return res\n"
        @"end tell", (long)limit];
        
    return [self executeFetchScript:scriptSource];
}

- (NSArray<NSDictionary *> *)searchNotesWithQuery:(NSString *)query limit:(NSInteger)limit {
    if (limit <= 0) limit = 10;
    NSString *cleanQuery = [self escapeForAppleScript:query.lowercaseString];
    
    NSString *scriptSource = [NSString stringWithFormat:
        @"tell application \"Notes\"\n"
        @"  set totalNotes to count of notes\n"
        @"  set matchLimit to %ld\n"
        @"  set res to {}\n"
        @"  set q to \"%@\"\n"
        @"  repeat with i from 1 to totalNotes\n"
        @"    set n to note i\n"
        @"    set nName to name of n\n"
        @"    set nText to \"\"\n"
        @"    try\n"
        @"      set nText to plaintext of n\n"
        @"    end try\n"
        @"    if (nName contains q) or (nText contains q) then\n"
        @"      set nDate to (modification date of n as string)\n"
        @"      set end of res to (nName & \"<|||>\" & nDate & \"<|||>\" & (paragraph 1 of nText))\n"
        @"      if (count of res) >= matchLimit then exit repeat\n"
        @"    end if\n"
        @"  end repeat\n"
        @"  return res\n"
        @"end tell", (long)limit, cleanQuery];
        
    return [self executeFetchScript:scriptSource];
}

- (NSArray<NSDictionary *> *)executeFetchScript:(NSString *)source {
    NSMutableArray *results = [NSMutableArray array];
    NSDictionary *errorDict = nil;
    NSAppleScript *script = [[NSAppleScript alloc] initWithSource:source];
    NSAppleEventDescriptor *eventDesc = [script executeAndReturnError:&errorDict];
    
    if (errorDict) {
        return @[@{
            @"error": [NSString stringWithFormat:@"Notes AppleScript error: %@", errorDict[NSAppleScriptErrorMessage] ?: @"Unknown error"]
        }];
    }
    
    NSInteger count = [eventDesc numberOfItems];
    for (NSInteger i = 1; i <= count; i++) {
        NSString *raw = [[eventDesc descriptorAtIndex:i] stringValue];
        if (raw) {
            NSArray *parts = [raw componentsSeparatedByString:@"<|||>"];
            NSString *title = parts.count > 0 ? parts[0] : @"Untitled";
            NSString *date = parts.count > 1 ? parts[1] : @"";
            NSString *preview = parts.count > 2 ? parts[2] : @"";
            [results addObject:@{
                @"title": title,
                @"date": date,
                @"preview": preview
            }];
        }
    }
    return results;
}

- (NSDictionary *)createNoteWithTitle:(NSString *)title body:(NSString *)body {
    NSString *cleanTitle = [self escapeForAppleScript:title];
    NSString *cleanBody = [self escapeForAppleScript:body];
    
    NSString *scriptSource = [NSString stringWithFormat:
        @"tell application \"Notes\"\n"
        @"  set newNote to make new note with properties {name:\"%@\", body:\"<h1>%@</h1><br><p>%@</p>\"}\n"
        @"  return name of newNote\n"
        @"end tell", cleanTitle, cleanTitle, cleanBody];
        
    NSDictionary *errorDict = nil;
    NSAppleScript *script = [[NSAppleScript alloc] initWithSource:scriptSource];
    NSAppleEventDescriptor *res = [script executeAndReturnError:&errorDict];
    
    if (errorDict) {
        return @{
            @"error": [NSString stringWithFormat:@"Failed to create note: %@", errorDict[NSAppleScriptErrorMessage] ?: @"Unknown error"]
        };
    }
    return @{
        @"success": @YES,
        @"title": [res stringValue] ?: title
    };
}

@end
