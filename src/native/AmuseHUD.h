#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^HUDMessageCallback)(NSString *type, id _Nullable payload);

@interface AmuseHUD : NSObject <WKScriptMessageHandler>

+ (instancetype)sharedHUD;

@property (nonatomic, copy, nullable) HUDMessageCallback messageCallback;

/// Configures and creates the floating HUD window.
- (void)setupWithHTMLPath:(NSString *)htmlPath;

- (void)showHUD;
- (void)hideHUD;
- (void)toggleHUD;

/// Updates state in the UI (e.g. "idle", "listening", "thinking", "speaking").
- (void)setStatus:(NSString *)status details:(nullable NSString *)details;

/// Adds or updates transcript text in the HUD.
- (void)appendTranscriptWithRole:(NSString *)role text:(NSString *)text;

/// Sends audio level or visualization update to the HUD.
- (void)updateAudioLevel:(float)level;

@end

NS_ASSUME_NONNULL_END
