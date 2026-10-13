#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface LocalConnectorsManager : NSObject

+ (instancetype)sharedManager;

/// Executes a tool call by name with JSON dictionary arguments, returning JSON data.
- (NSString *)executeToolWithName:(NSString *)toolName
                        arguments:(NSDictionary *)args;

/// Returns the array of tool declaration schemas for Gemini Function Calling.
- (NSArray<NSDictionary *> *)geminiToolDeclarations;

@end

NS_ASSUME_NONNULL_END
