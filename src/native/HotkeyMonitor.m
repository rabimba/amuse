#import "HotkeyMonitor.h"

@interface HotkeyMonitor ()
@property (nonatomic, strong) id globalMonitor;
@property (nonatomic, strong) id localMonitor;
@property (nonatomic, copy, nullable) HotkeyTriggerHandler handler;
@property (nonatomic, assign) NSTimeInterval lastOptionPressTime;
@property (nonatomic, assign) BOOL wasOptionDown;
@end

@implementation HotkeyMonitor

+ (instancetype)sharedMonitor {
    static HotkeyMonitor *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[self alloc] init];
    });
    return shared;
}

- (void)startMonitoringWithHandler:(HotkeyTriggerHandler)handler {
    [self stopMonitoring];
    self.handler = handler;
    self.lastOptionPressTime = 0;
    self.wasOptionDown = NO;
    
    __weak typeof(self) weakSelf = self;
    
    void (^handleFlagsChanged)(NSEvent *) = ^(NSEvent *event) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        
        BOOL isOptionDown = (event.modifierFlags & NSEventModifierFlagOption) != 0;
        // Ignore other modifier keys like Command, Shift, Control
        BOOL otherModifiers = (event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagShift)) != 0;
        
        if (otherModifiers) {
            strongSelf.wasOptionDown = NO;
            strongSelf.lastOptionPressTime = 0;
            return;
        }
        
        NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
        
        // Key down transition
        if (isOptionDown && !strongSelf.wasOptionDown) {
            if (now - strongSelf.lastOptionPressTime < 0.35) {
                // Double tap detected!
                strongSelf.lastOptionPressTime = 0;
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (strongSelf.handler) {
                        strongSelf.handler();
                    }
                });
            } else {
                strongSelf.lastOptionPressTime = now;
            }
        }
        
        strongSelf.wasOptionDown = isOptionDown;
    };
    
    self.globalMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskFlagsChanged handler:handleFlagsChanged];
    self.localMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskFlagsChanged handler:^NSEvent * _Nullable(NSEvent * _Nonnull event) {
        handleFlagsChanged(event);
        return event;
    }];
}

- (void)stopMonitoring {
    if (self.globalMonitor) {
        [NSEvent removeMonitor:self.globalMonitor];
        self.globalMonitor = nil;
    }
    if (self.localMonitor) {
        [NSEvent removeMonitor:self.localMonitor];
        self.localMonitor = nil;
    }
    self.handler = nil;
}

@end
