#import "AmuseHUD.h"

@interface AmuseHUD ()
@property (nonatomic, strong) NSPanel *panel;
@property (nonatomic, strong) WKWebView *webView;
@property (nonatomic, assign) BOOL isVisible;
@end

@implementation AmuseHUD

+ (instancetype)sharedHUD {
    static AmuseHUD *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[self alloc] init];
    });
    return shared;
}

- (void)setupWithHTMLPath:(NSString *)htmlPath {
    NSScreen *screen = [NSScreen mainScreen];
    NSRect screenRect = screen ? screen.visibleFrame : NSMakeRect(0, 0, 1440, 900);
    
    CGFloat width = 460;
    CGFloat height = 580;
    CGFloat x = NSMidX(screenRect) - (width / 2.0);
    CGFloat y = NSMidY(screenRect) - (height / 2.0) + 60; // Slightly above center
    NSRect windowRect = NSMakeRect(x, y, width, height);
    
    _panel = [[NSPanel alloc] initWithContentRect:windowRect
                                        styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel)
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
                                            
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setOpaque:NO];
    [_panel setBackgroundColor:[NSColor clearColor]];
    [_panel setHasShadow:YES];
    [_panel setHidesOnDeactivate:NO];
    [_panel setCollectionBehavior:(NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary)];
    
    // Liquid glass visual effect container
    NSVisualEffectView *effectView = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    effectView.material = NSVisualEffectMaterialHUDWindow;
    effectView.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    effectView.state = NSVisualEffectStateActive;
    effectView.wantsLayer = YES;
    effectView.layer.cornerRadius = 24.0;
    effectView.layer.masksToBounds = YES;
    effectView.layer.borderWidth = 1.0;
    effectView.layer.borderColor = [NSColor colorWithWhite:1.0 alpha:0.18].CGColor;
    
    // WebKit Configuration
    WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];
    WKUserContentController *controller = [[WKUserContentController alloc] init];
    [controller addScriptMessageHandler:self name:@"amuseBridge"];
    config.userContentController = controller;
    [config.preferences setValue:@YES forKey:@"developerExtrasEnabled"];
    
    _webView = [[WKWebView alloc] initWithFrame:effectView.bounds configuration:config];
    _webView.autoresizingMask = (NSViewWidthSizable | NSViewHeightSizable);
    [_webView setValue:@NO forKey:@"drawsBackground"];
    
    [effectView addSubview:_webView];
    [_panel setContentView:effectView];
    
    NSURL *fileURL = [NSURL fileURLWithPath:htmlPath];
    NSURL *dirURL = [fileURL URLByDeletingLastPathComponent];
    [_webView loadFileURL:fileURL allowingReadAccessToURL:dirURL];
}

- (void)showHUD {
    if (!_panel) return;
    [_panel makeKeyAndOrderFront:nil];
    self.isVisible = YES;
    [self evalJS:@"window.onHUDVisibilityChanged && window.onHUDVisibilityChanged(true)"];
}

- (void)hideHUD {
    if (!_panel) return;
    [_panel orderOut:nil];
    self.isVisible = NO;
    [self evalJS:@"window.onHUDVisibilityChanged && window.onHUDVisibilityChanged(false)"];
}

- (void)toggleHUD {
    if (self.isVisible) {
        [self hideHUD];
    } else {
        [self showHUD];
    }
}

- (void)setStatus:(NSString *)status details:(nullable NSString *)details {
    NSString *safeStatus = [status stringByReplacingOccurrencesOfString:@"'" withString:@"\\'"];
    NSString *safeDetails = details ? [details stringByReplacingOccurrencesOfString:@"'" withString:@"\\'"] : @"";
    NSString *js = [NSString stringWithFormat:@"window.setAgentStatus && window.setAgentStatus('%@', '%@');", safeStatus, safeDetails];
    [self evalJS:js];
}

- (void)appendTranscriptWithRole:(NSString *)role text:(NSString *)text {
    NSData *jsonData = [NSJSONSerialization dataWithJSONObject:@{@"role": role ?: @"", @"text": text ?: @""} options:0 error:nil];
    if (!jsonData) return;
    NSString *jsonStr = [[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding];
    NSString *js = [NSString stringWithFormat:@"window.appendTranscriptMessage && window.appendTranscriptMessage(%@);", jsonStr];
    [self evalJS:js];
}

- (void)updateAudioLevel:(float)level {
    NSString *js = [NSString stringWithFormat:@"window.updateAudioLevel && window.updateAudioLevel(%f);", level];
    [self evalJS:js];
}

- (void)evalJS:(NSString *)js {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.webView evaluateJavaScript:js completionHandler:nil];
    });
}

#pragma mark - WKScriptMessageHandler

- (void)userContentController:(WKUserContentController *)userContentController didReceiveScriptMessage:(WKScriptMessage *)message {
    if ([message.name isEqualToString:@"amuseBridge"]) {
        NSDictionary *body = message.body;
        if ([body isKindOfClass:[NSDictionary class]]) {
            NSString *type = body[@"type"];
            id payload = body[@"payload"];
            if (self.messageCallback) {
                self.messageCallback(type, payload);
            }
        }
    }
}

@end
