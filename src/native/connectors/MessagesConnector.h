#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface MessagesConnector : NSObject

+ (instancetype)sharedConnector;

/// Searches iMessage database for messages matching query text and/or sender handle.
- (NSArray<NSDictionary *> *)searchMessagesWithQuery:(nullable NSString *)query
                                              sender:(nullable NSString *)sender
                                               limit:(NSInteger)limit;

/// Fetches the most recent messages across all chats.
- (NSArray<NSDictionary *> *)recentMessagesWithLimit:(NSInteger)limit;

@end

NS_ASSUME_NONNULL_END
