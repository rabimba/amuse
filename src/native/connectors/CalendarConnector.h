#import <Foundation/Foundation.h>
#import <EventKit/EventKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface CalendarConnector : NSObject

+ (instancetype)sharedConnector;

- (NSInteger)calendarAuthorizationStatus;
- (NSInteger)remindersAuthorizationStatus;

// Calendar
- (NSArray<NSDictionary *> *)getEventsDaysAhead:(NSInteger)days;
- (NSArray<NSDictionary *> *)getEventsFromDate:(NSDate *)startDate toDate:(NSDate *)endDate;
- (NSDictionary *)createEventWithTitle:(NSString *)title
                             startDate:(NSDate *)startDate
                               endDate:(NSDate *)endDate
                                 notes:(nullable NSString *)notes
                              location:(nullable NSString *)location;

// Reminders
- (NSArray<NSDictionary *> *)getRemindersIncludeCompleted:(BOOL)includeCompleted;
- (NSDictionary *)createReminderWithTitle:(NSString *)title
                                  dueDate:(nullable NSDate *)dueDate
                                    notes:(nullable NSString *)notes;

@end

NS_ASSUME_NONNULL_END
