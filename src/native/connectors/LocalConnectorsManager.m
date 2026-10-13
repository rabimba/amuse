#import "LocalConnectorsManager.h"
#import "MessagesConnector.h"
#import "ContactsConnector.h"
#import "CalendarConnector.h"
#import "NotesConnector.h"

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
            
            NSDate *start = [df dateFromString:args[@"start_iso"]] ?: [NSDate date];
            NSDate *end = [df dateFromString:args[@"end_iso"]] ?: [start dateByAddingTimeInterval:3600];
            
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
                df.dateFormat = @"yyyy-MM-dd'T'HH:mm:ssZZZZZ";
                dueDate = [df dateFromString:args[@"due_iso"]];
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
        }
    ];
}

@end
