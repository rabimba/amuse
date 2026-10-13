#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^AudioChunkHandler)(NSData *pcmData);

@interface AudioEngine : NSObject

+ (instancetype)sharedEngine;

@property (nonatomic, readonly) BOOL isRecording;
@property (nonatomic, readonly) BOOL isPlaying;

/// Starts recording microphone audio, converting to 16kHz 1-channel 16-bit PCM.
- (BOOL)startRecordingWithHandler:(AudioChunkHandler)handler error:(NSError **)outError;

/// Stops audio capture.
- (void)stopRecording;

/// Plays raw linear PCM audio chunks (defaults to 24000Hz 1-channel 16-bit PCM for Gemini Live output).
- (void)playPCMChunk:(NSData *)pcmData sampleRate:(double)rate;

/// Stops all current and queued audio playback.
- (void)stopPlayback;

@end

NS_ASSUME_NONNULL_END
