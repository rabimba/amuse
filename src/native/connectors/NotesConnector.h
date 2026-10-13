#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface NotesConnector : NSObject

+ (instancetype)sharedConnector;

- (NSArray<NSDictionary *> *)recentNotesWithLimit:(NSInteger)limit;

- (NSArray<NSDictionary *> *)searchNotesWithQuery:(NSString *)query limit:(NSInteger)limit;

- (NSDictionary *)createNoteWithTitle:(NSString *)title body:(NSString *)body;

@end

NS_ASSUME_NONNULL_END
