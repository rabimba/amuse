#import "ContactsConnector.h"

@interface ContactsConnector ()
@property (nonatomic, strong) CNContactStore *store;
@end

@implementation ContactsConnector

+ (instancetype)sharedConnector {
    static ContactsConnector *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[self alloc] init];
    });
    return shared;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _store = [[CNContactStore alloc] init];
    }
    return self;
}

- (NSInteger)authorizationStatus {
    return (NSInteger)[CNContactStore authorizationStatusForEntityType:CNEntityTypeContacts];
}

- (NSArray *)defaultKeysToFetch {
    return @[
        CNContactIdentifierKey,
        CNContactGivenNameKey,
        CNContactFamilyNameKey,
        CNContactOrganizationNameKey,
        CNContactPhoneNumbersKey,
        CNContactEmailAddressesKey,
        CNContactPostalAddressesKey
    ];
}

- (NSDictionary *)dictionaryFromContact:(CNContact *)contact {
    NSMutableDictionary *dict = [NSMutableDictionary dictionary];
    dict[@"id"] = contact.identifier ?: @"";
    dict[@"given_name"] = contact.givenName ?: @"";
    dict[@"family_name"] = contact.familyName ?: @"";
    NSString *combined = [NSString stringWithFormat:@"%@ %@", contact.givenName ?: @"", contact.familyName ?: @""];
    dict[@"full_name"] = [combined stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    dict[@"organization"] = contact.organizationName ?: @"";
    
    NSMutableArray *phones = [NSMutableArray array];
    for (CNLabeledValue<CNPhoneNumber *> *item in contact.phoneNumbers) {
        NSString *label = item.label ? [CNLabeledValue localizedStringForLabel:item.label] : @"Other";
        NSString *num = item.value.stringValue ?: @"";
        [phones addObject:@{@"label": label, @"number": num}];
    }
    dict[@"phone_numbers"] = phones;
    
    NSMutableArray *emails = [NSMutableArray array];
    for (CNLabeledValue<NSString *> *item in contact.emailAddresses) {
        NSString *label = item.label ? [CNLabeledValue localizedStringForLabel:item.label] : @"Other";
        NSString *addr = item.value ?: @"";
        [emails addObject:@{@"label": label, @"email": addr}];
    }
    dict[@"emails"] = emails;
    
    return dict;
}

- (NSArray<NSDictionary *> *)searchContactsWithName:(NSString *)nameQuery {
    NSInteger status = [self authorizationStatus];
    if (status != 3 /* Authorized */) {
        return @[@{
            @"error": @"Contacts permission not granted. Please allow Contacts access in System Settings > Privacy & Security > Contacts.",
            @"authorization_status": @(status)
        }];
    }
    
    NSMutableArray *results = [NSMutableArray array];
    NSPredicate *pred = [CNContact predicateForContactsMatchingName:nameQuery];
    NSError *error = nil;
    NSArray *contacts = [self.store unifiedContactsMatchingPredicate:pred keysToFetch:[self defaultKeysToFetch] error:&error];
    
    if (error) {
        return @[@{@"error": error.localizedDescription}];
    }
    
    for (CNContact *c in contacts) {
        [results addObject:[self dictionaryFromContact:c]];
    }
    return results;
}

- (NSArray<NSDictionary *> *)searchContactsWithPhoneOrEmail:(NSString *)query {
    NSInteger status = [self authorizationStatus];
    if (status != 3) {
        return @[@{
            @"error": @"Contacts permission not granted. Please allow Contacts access in System Settings > Privacy & Security > Contacts.",
            @"authorization_status": @(status)
        }];
    }
    
    NSMutableArray *results = [NSMutableArray array];
    CNContactFetchRequest *request = [[CNContactFetchRequest alloc] initWithKeysToFetch:[self defaultKeysToFetch]];
    NSError *error = nil;
    NSString *cleanQuery = [query.lowercaseString stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    
    [self.store enumerateContactsWithFetchRequest:request error:&error usingBlock:^(CNContact * _Nonnull contact, BOOL * _Nonnull stop) {
        BOOL match = NO;
        for (CNLabeledValue<CNPhoneNumber *> *item in contact.phoneNumbers) {
            NSString *cleanNum = [item.value.stringValue stringByReplacingOccurrencesOfString:@"[^0-9]" withString:@"" options:NSRegularExpressionSearch range:NSMakeRange(0, item.value.stringValue.length)];
            NSString *cleanQ = [cleanQuery stringByReplacingOccurrencesOfString:@"[^0-9]" withString:@"" options:NSRegularExpressionSearch range:NSMakeRange(0, cleanQuery.length)];
            if (cleanQ.length > 0 && [cleanNum containsString:cleanQ]) {
                match = YES;
                break;
            }
        }
        if (!match) {
            for (CNLabeledValue<NSString *> *item in contact.emailAddresses) {
                if ([item.value.lowercaseString containsString:cleanQuery]) {
                    match = YES;
                    break;
                }
            }
        }
        
        if (match) {
            [results addObject:[self dictionaryFromContact:contact]];
        }
    }];
    
    if (error) {
        return @[@{@"error": error.localizedDescription}];
    }
    return results;
}

- (NSArray<NSDictionary *> *)allContactsWithLimit:(NSInteger)limit {
    NSInteger status = [self authorizationStatus];
    if (status != 3) {
        return @[@{
            @"error": @"Contacts permission not granted. Please allow Contacts access in System Settings > Privacy & Security > Contacts.",
            @"authorization_status": @(status)
        }];
    }
    
    if (limit <= 0) limit = 50;
    NSMutableArray *results = [NSMutableArray array];
    CNContactFetchRequest *request = [[CNContactFetchRequest alloc] initWithKeysToFetch:[self defaultKeysToFetch]];
    NSError *error = nil;
    __block NSInteger count = 0;
    
    [self.store enumerateContactsWithFetchRequest:request error:&error usingBlock:^(CNContact * _Nonnull contact, BOOL * _Nonnull stop) {
        [results addObject:[self dictionaryFromContact:contact]];
        count++;
        if (count >= limit) {
            *stop = YES;
        }
    }];
    
    if (error) {
        return @[@{@"error": error.localizedDescription}];
    }
    return results;
}

@end
