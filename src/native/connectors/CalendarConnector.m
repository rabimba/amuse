#import "CalendarConnector.h"

@interface CalendarConnector ()
@property (nonatomic, strong) EKEventStore *eventStore;
@property (nonatomic, strong) NSDateFormatter *isoFormatter;
@end

@implementation CalendarConnector

+ (instancetype)sharedConnector {
    static CalendarConnector *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[self alloc] init];
    });
    return shared;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _eventStore = [[EKEventStore alloc] init];
        _isoFormatter = [[NSDateFormatter alloc] init];
        _isoFormatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
        _isoFormatter.dateFormat = @"yyyy-MM-dd'T'HH:mm:ssZZZZZ";
    }
    return self;
}

- (NSInteger)calendarAuthorizationStatus {
    return (NSInteger)[EKEventStore authorizationStatusForEntityType:EKEntityTypeEvent];
}

- (NSInteger)remindersAuthorizationStatus {
    return (NSInteger)[EKEventStore authorizationStatusForEntityType:EKEntityTypeReminder];
}

- (NSArray<NSDictionary *> *)getEventsDaysAhead:(NSInteger)days {
    if (days <= 0) days = 7;
    NSDate *start = [NSDate date];
    NSDate *end = [start dateByAddingTimeInterval:days * 86400];
    return [self getEventsFromDate:start toDate:end];
}

- (NSArray<NSDictionary *> *)getEventsFromDate:(NSDate *)startDate toDate:(NSDate *)endDate {
    NSInteger status = [self calendarAuthorizationStatus];
    if (status != 3 /* Authorized */) {
        return @[@{
            @"error": @"Calendar access not granted. Please enable Calendar in System Settings > Privacy & Security > Calendars.",
            @"authorization_status": @(status)
        }];
    }
    
    NSPredicate *predicate = [self.eventStore predicateForEventsWithStartDate:startDate endDate:endDate calendars:nil];
    NSArray<EKEvent *> *events = [self.eventStore eventsMatchingPredicate:predicate];
    
    NSMutableArray *results = [NSMutableArray array];
    for (EKEvent *event in events) {
        [results addObject:@{
            @"id": event.eventIdentifier ?: @"",
            @"title": event.title ?: @"Untitled Event",
            @"start": [self.isoFormatter stringFromDate:event.startDate] ?: @"",
            @"end": [self.isoFormatter stringFromDate:event.endDate] ?: @"",
            @"is_all_day": @(event.isAllDay),
            @"location": event.location ?: @"",
            @"notes": event.notes ?: @"",
            @"calendar": event.calendar.title ?: @""
        }];
    }
    
    return results;
}

- (NSDictionary *)createEventWithTitle:(NSString *)title
                             startDate:(NSDate *)startDate
                               endDate:(NSDate *)endDate
                                 notes:(nullable NSString *)notes
                              location:(nullable NSString *)location {
    NSInteger status = [self calendarAuthorizationStatus];
    if (status != 3) {
        return @{
            @"error": @"Calendar access not granted. Cannot create event.",
            @"authorization_status": @(status)
        };
    }
    
    EKEvent *event = [EKEvent eventWithEventStore:self.eventStore];
    event.title = title;
    event.startDate = startDate;
    event.endDate = endDate ?: [startDate dateByAddingTimeInterval:3600];
    event.calendar = [self.eventStore defaultCalendarForNewEvents];
    if (notes) event.notes = notes;
    if (location) event.location = location;
    
    NSError *error = nil;
    BOOL success = [self.eventStore saveEvent:event span:EKSpanThisEvent commit:YES error:&error];
    if (success) {
        return @{
            @"success": @YES,
            @"event_id": event.eventIdentifier ?: @"",
            @"title": event.title ?: @"",
            @"start": [self.isoFormatter stringFromDate:event.startDate] ?: @""
        };
    } else {
        return @{
            @"error": error ? error.localizedDescription : @"Unknown error creating event"
        };
    }
}

- (NSArray<NSDictionary *> *)getRemindersIncludeCompleted:(BOOL)includeCompleted {
    NSInteger status = [self remindersAuthorizationStatus];
    if (status != 3) {
        return @[@{
            @"error": @"Reminders access not granted. Please enable Reminders in System Settings > Privacy & Security > Reminders.",
            @"authorization_status": @(status)
        }];
    }
    
    dispatch_semaphore_t sema = dispatch_semaphore_create(0);
    __block NSArray<EKReminder *> *fetchedReminders = nil;
    
    NSPredicate *pred = includeCompleted
        ? [self.eventStore predicateForRemindersInCalendars:nil]
        : [self.eventStore predicateForIncompleteRemindersWithDueDateStarting:nil ending:nil calendars:nil];
        
    [self.eventStore fetchRemindersMatchingPredicate:pred completion:^(NSArray<EKReminder *> * _Nullable reminders) {
        fetchedReminders = reminders;
        dispatch_semaphore_signal(sema);
    }];
    
    dispatch_semaphore_wait(sema, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    
    NSMutableArray *results = [NSMutableArray array];
    for (EKReminder *rem in fetchedReminders) {
        NSString *dueStr = @"";
        if (rem.dueDateComponents) {
            NSDate *dueDate = [[NSCalendar currentCalendar] dateFromComponents:rem.dueDateComponents];
            if (dueDate) {
                dueStr = [self.isoFormatter stringFromDate:dueDate] ?: @"";
            }
        }
        
        [results addObject:@{
            @"id": rem.calendarItemIdentifier ?: @"",
            @"title": rem.title ?: @"",
            @"completed": @(rem.isCompleted),
            @"due_date": dueStr,
            @"notes": rem.notes ?: @"",
            @"calendar": rem.calendar.title ?: @""
        }];
    }
    
    return results;
}

- (NSDictionary *)createReminderWithTitle:(NSString *)title
                                  dueDate:(nullable NSDate *)dueDate
                                    notes:(nullable NSString *)notes {
    NSInteger status = [self remindersAuthorizationStatus];
    if (status != 3) {
        return @{
            @"error": @"Reminders access not granted. Cannot create reminder.",
            @"authorization_status": @(status)
        };
    }
    
    EKReminder *reminder = [EKReminder reminderWithEventStore:self.eventStore];
    reminder.title = title;
    reminder.calendar = [self.eventStore defaultCalendarForNewReminders];
    if (notes) reminder.notes = notes;
    if (dueDate) {
        reminder.dueDateComponents = [[NSCalendar currentCalendar] components:(NSCalendarUnitYear | NSCalendarUnitMonth | NSCalendarUnitDay | NSCalendarUnitHour | NSCalendarUnitMinute) fromDate:dueDate];
    }
    
    NSError *error = nil;
    BOOL success = [self.eventStore saveReminder:reminder commit:YES error:&error];
    if (success) {
        return @{
            @"success": @YES,
            @"reminder_id": reminder.calendarItemIdentifier ?: @"",
            @"title": reminder.title ?: @""
        };
    } else {
        return @{
            @"error": error ? error.localizedDescription : @"Unknown error creating reminder"
        };
    }
}

@end
