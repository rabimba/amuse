#import <Foundation/Foundation.h>
#import <Contacts/Contacts.h>

NS_ASSUME_NONNULL_BEGIN

@interface ContactsConnector : NSObject

+ (instancetype)sharedConnector;

- (NSInteger)authorizationStatus;

- (NSArray<NSDictionary *> *)searchContactsWithName:(NSString *)nameQuery;

- (NSArray<NSDictionary *> *)searchContactsWithPhoneOrEmail:(NSString *)query;

- (NSArray<NSDictionary *> *)allContactsWithLimit:(NSInteger)limit;

@end

NS_ASSUME_NONNULL_END
