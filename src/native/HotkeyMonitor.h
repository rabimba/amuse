#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^HotkeyTriggerHandler)(void);

@interface HotkeyMonitor : NSObject

+ (instancetype)sharedMonitor;

/// Starts listening for double-tap Option hotkey.
- (void)startMonitoringWithHandler:(HotkeyTriggerHandler)handler;

/// Stops hotkey monitoring.
- (void)stopMonitoring;

@end

NS_ASSUME_NONNULL_END
