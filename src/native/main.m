#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>
#import "LocalConnectorsManager.h"
#import "AudioEngine.h"
#import "HotkeyMonitor.h"
#import "AmuseHUD.h"

// Forward declaration
@interface AmuseAppController : NSObject <NSApplicationDelegate>
@end

@implementation AmuseAppController {
    NSPipe *_nodeStdinPipe;
    NSPipe *_nodeStdoutPipe;
    NSTask *_nodeTask;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    NSLog(@"[Amuse] Native controller launching...");
    
    NSString *bundleRoot = [[NSBundle mainBundle] bundlePath];
    NSString *htmlPath = [bundleRoot stringByAppendingPathComponent:@"Contents/Resources/web/index.html"];
    if (![[NSFileManager defaultManager] fileExistsAtPath:htmlPath]) {
        // Fallback for development run
        htmlPath = [[NSFileManager defaultManager] currentDirectoryPath];
        htmlPath = [htmlPath stringByAppendingPathComponent:@"src/web/index.html"];
    }
    
    // 1. Setup HUD window
    [[AmuseHUD sharedHUD] setupWithHTMLPath:htmlPath];
    
    // 2. Setup HUD Message Callback
    [AmuseHUD sharedHUD].messageCallback = ^(NSString * _Nonnull type, id  _Nullable payload) {
        if ([type isEqualToString:@"hideHUD"]) {
            [[AmuseHUD sharedHUD] hideHUD];
        } else if ([type isEqualToString:@"toggleVoice"]) {
            BOOL active = [payload[@"active"] boolValue];
            if (active) {
                [self startAudioCapture];
            } else {
                [self stopAudioCapture];
            }
        } else if ([type isEqualToString:@"sendTextMessage"]) {
            NSString *text = payload[@"text"];
            if (text) {
                [self sendToNode:@{
                    @"type": @"user_text_input",
                    @"text": text
                }];
            }
        }
    };
    
    // 3. Setup Double-Option Global Hotkey
    [[HotkeyMonitor sharedMonitor] startMonitoringWithHandler:^{
        NSLog(@"[Amuse] Double-tap Option detected! Toggling HUD.");
        [[AmuseHUD sharedHUD] toggleHUD];
    }];
    
    // 4. Launch Node.js Gemini Live Client background orchestrator
    [self launchNodeOrchestrator];
    
    // 5. Present HUD initially
    [[AmuseHUD sharedHUD] showHUD];
    [[AmuseHUD sharedHUD] setStatus:@"idle" details:@"Press ⌥⌥ (Double-tap Option) anytime to toggle."];
}

- (void)startAudioCapture {
    NSError *error = nil;
    BOOL ok = [[AudioEngine sharedEngine] startRecordingWithHandler:^(NSData * _Nonnull pcmData) {
        NSString *base64 = [pcmData base64EncodedStringWithOptions:0];
        [self sendToNode:@{
            @"type": @"audio_input_chunk",
            @"data": base64
        }];
        
        // Calculate basic RMS volume level for UI meter
        const int16_t *samples = (const int16_t *)pcmData.bytes;
        NSUInteger count = pcmData.length / sizeof(int16_t);
        double sum = 0;
        for (NSUInteger i = 0; i < count; i++) {
            sum += (double)samples[i] * (double)samples[i];
        }
        float rms = (count > 0) ? sqrt(sum / count) / 32768.0 : 0.0;
        float level = MIN(1.0, rms * 4.0); // Boosted for visibility
        dispatch_async(dispatch_get_main_queue(), ^{
            [[AmuseHUD sharedHUD] updateAudioLevel:level];
        });
    } error:&error];
    
    if (ok) {
        [[AmuseHUD sharedHUD] setStatus:@"listening" details:@"Microphone active. Streaming to Gemini..."];
    } else {
        NSLog(@"[Amuse] Failed to start microphone: %@", error);
        [[AmuseHUD sharedHUD] setStatus:@"idle" details:[NSString stringWithFormat:@"Microphone error: %@", error.localizedDescription]];
    }
}

- (void)stopAudioCapture {
    [[AudioEngine sharedEngine] stopRecording];
    [[AmuseHUD sharedHUD] setStatus:@"idle" details:@"Voice muted. Click mic or type below."];
    [self sendToNode:@{@"type": @"voice_stopped"}];
}

- (void)launchNodeOrchestrator {
    NSString *cwd = [[NSFileManager defaultManager] currentDirectoryPath];
    NSString *nodeScript = [cwd stringByAppendingPathComponent:@"src/js/index.mjs"];
    
    _nodeTask = [[NSTask alloc] init];
    
    // Look for node in standard paths
    NSString *nodePath = @"/usr/local/bin/node";
    NSString *nvmNode = [NSHomeDirectory() stringByAppendingPathComponent:@".nvm/versions/node/v24.21.0/bin/node"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:nvmNode]) {
        nodePath = nvmNode;
    } else if (![[NSFileManager defaultManager] fileExistsAtPath:nodePath]) {
        nodePath = @"/opt/homebrew/bin/node";
    }
    
    _nodeTask.executableURL = [NSURL fileURLWithPath:nodePath];
    _nodeTask.arguments = @[nodeScript];
    _nodeTask.currentDirectoryURL = [NSURL fileURLWithPath:cwd];
    
    _nodeStdinPipe = [NSPipe pipe];
    _nodeStdoutPipe = [NSPipe pipe];
    _nodeTask.standardInput = _nodeStdinPipe;
    _nodeTask.standardOutput = _nodeStdoutPipe;
    _nodeTask.standardError = [NSFileHandle fileHandleWithStandardError];
    
    NSFileHandle *readHandle = [_nodeStdoutPipe fileHandleForReading];
    readHandle.readabilityHandler = ^(NSFileHandle *handle) {
        NSData *data = [handle availableData];
        if (data.length == 0) return;
        
        NSString *raw = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        NSArray *lines = [raw componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
        for (NSString *line in lines) {
            if (line.length == 0) continue;
            [self handleNodeMessageLine:line];
        }
    };
    
    NSError *error = nil;
    if (![_nodeTask launchAndReturnError:&error]) {
        NSLog(@"[Amuse] Failed to launch Node orchestrator: %@", error);
    } else {
        NSLog(@"[Amuse] Node orchestrator launched (PID: %d)", _nodeTask.processIdentifier);
    }
}

- (void)sendToNode:(NSDictionary *)dict {
    NSError *err = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:dict options:0 error:&err];
    if (data) {
        NSMutableData *lineData = [NSMutableData dataWithData:data];
        [lineData appendBytes:"\n" length:1];
        @try {
            [[_nodeStdinPipe fileHandleForWriting] writeData:lineData];
        } @catch (NSException *ex) {
            NSLog(@"[Amuse] Error writing to Node: %@", ex);
        }
    }
}

- (void)handleNodeMessageLine:(NSString *)line {
    NSData *data = [line dataUsingEncoding:NSUTF8StringEncoding];
    if (!data) return;
    NSError *err = nil;
    NSDictionary *msg = [NSJSONSerialization JSONObjectWithData:data options:0 error:&err];
    if (![msg isKindOfClass:[NSDictionary class]]) return;
    
    NSString *type = msg[@"type"];
    
    dispatch_async(dispatch_get_main_queue(), ^{
        if ([type isEqualToString:@"audio_output_chunk"]) {
            NSString *b64 = msg[@"data"];
            if (b64) {
                NSData *pcm = [[NSData alloc] initWithBase64EncodedString:b64 options:0];
                [[AudioEngine sharedEngine] playPCMChunk:pcm sampleRate:24000.0];
                [[AmuseHUD sharedHUD] setStatus:@"speaking" details:@"Amuse is speaking..."];
            }
        }
        else if ([type isEqualToString:@"agent_status"]) {
            NSString *status = msg[@"status"] ?: @"idle";
            NSString *details = msg[@"details"] ?: @"";
            [[AmuseHUD sharedHUD] setStatus:status details:details];
        }
        else if ([type isEqualToString:@"transcript"]) {
            NSString *role = msg[@"role"] ?: @"agent";
            NSString *text = msg[@"text"] ?: @"";
            [[AmuseHUD sharedHUD] appendTranscriptWithRole:role text:text];
        }
        else if ([type isEqualToString:@"tool_call_request"]) {
            // Node asked native to execute a local connector tool
            NSString *callId = msg[@"callId"] ?: @"";
            NSString *toolName = msg[@"toolName"] ?: @"";
            NSDictionary *args = msg[@"arguments"] ?: @{};
            
            [[AmuseHUD sharedHUD] setStatus:@"thinking" details:[NSString stringWithFormat:@"Accessing %@", toolName]];
            
            // Execute on background queue
            dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                NSString *resJson = [[LocalConnectorsManager sharedManager] executeToolWithName:toolName arguments:args];
                
                dispatch_async(dispatch_get_main_queue(), ^{
                    [[AmuseHUD sharedHUD] appendTranscriptWithRole:@"tool" text:[NSString stringWithFormat:@"Executed %@", toolName]];
                });
                
                [self sendToNode:@{
                    @"type": @"tool_call_response",
                    @"callId": callId,
                    @"output": resJson
                }];
            });
        }
    });
}

@end

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        if (argc > 1) {
            NSString *cmd = [NSString stringWithUTF8String:argv[1]];
            
            if ([cmd isEqualToString:@"tools"]) {
                NSArray *tools = [[LocalConnectorsManager sharedManager] geminiToolDeclarations];
                NSData *data = [NSJSONSerialization dataWithJSONObject:tools options:NSJSONWritingPrettyPrinted error:nil];
                printf("%s\n", data ? [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] UTF8String] : "[]");
                return 0;
            }
            else if ([cmd isEqualToString:@"call"] && argc > 2) {
                NSString *toolName = [NSString stringWithUTF8String:argv[2]];
                NSDictionary *args = @{};
                if (argc > 3) {
                    NSData *argsData = [NSData dataWithBytes:argv[3] length:strlen(argv[3])];
                    args = [NSJSONSerialization JSONObjectWithData:argsData options:0 error:nil] ?: @{};
                }
                NSString *outJson = [[LocalConnectorsManager sharedManager] executeToolWithName:toolName arguments:args];
                printf("%s\n", [outJson UTF8String]);
                return 0;
            }
        }
        
        // Default mode: Launch full native GUI app with HUD and Hotkey monitor
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory]; // Menu bar / accessory app
        
        AmuseAppController *delegate = [[AmuseAppController alloc] init];
        [app setDelegate:delegate];
        [app run];
    }
    return 0;
}
