#import "AudioEngine.h"

@interface AudioEngine ()
@property (nonatomic, strong) AVAudioEngine *recordEngine;
@property (nonatomic, strong) AVAudioConverter *recordConverter;
@property (nonatomic, strong) AVAudioFormat *targetInputFormat; // 16kHz, 1ch, 16bit PCM
@property (nonatomic, copy, nullable) AudioChunkHandler chunkHandler;

@property (nonatomic, strong) AVAudioEngine *playbackEngine;
@property (nonatomic, strong) AVAudioPlayerNode *playerNode;
@property (nonatomic, strong) AVAudioFormat *playbackFormat; // 24kHz, 1ch, 16bit PCM
@property (nonatomic, assign) BOOL isRecording;
@property (nonatomic, assign) BOOL isPlaying;
@end

@implementation AudioEngine

+ (instancetype)sharedEngine {
    static AudioEngine *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[self alloc] init];
    });
    return shared;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        // Target recording format: 16kHz, 1 channel, 16-bit linear PCM
        AudioStreamBasicDescription asbd;
        memset(&asbd, 0, sizeof(asbd));
        asbd.mSampleRate = 16000.0;
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        asbd.mBytesPerPacket = 2;
        asbd.mFramesPerPacket = 1;
        asbd.mBytesPerFrame = 2;
        asbd.mChannelsPerFrame = 1;
        asbd.mBitsPerChannel = 16;
        _targetInputFormat = [[AVAudioFormat alloc] initWithStreamDescription:&asbd];
        
        // Target playback format: 24kHz, 1 channel, 16-bit linear PCM (Gemini Live default)
        AudioStreamBasicDescription playAsbd;
        memset(&playAsbd, 0, sizeof(playAsbd));
        playAsbd.mSampleRate = 24000.0;
        playAsbd.mFormatID = kAudioFormatLinearPCM;
        playAsbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        playAsbd.mBytesPerPacket = 2;
        playAsbd.mFramesPerPacket = 1;
        playAsbd.mBytesPerFrame = 2;
        playAsbd.mChannelsPerFrame = 1;
        playAsbd.mBitsPerChannel = 16;
        _playbackFormat = [[AVAudioFormat alloc] initWithStreamDescription:&playAsbd];
        
        [self setupPlaybackEngine];
    }
    return self;
}

- (void)setupPlaybackEngine {
    _playbackEngine = [[AVAudioEngine alloc] init];
    _playerNode = [[AVAudioPlayerNode alloc] init];
    [_playbackEngine attachNode:_playerNode];
    
    // Connect player to main mixer
    [_playbackEngine connect:_playerNode to:_playbackEngine.mainMixerNode format:_playbackFormat];
    
    NSError *error = nil;
    if (![_playbackEngine startAndReturnError:&error]) {
        NSLog(@"[AmuseAudio] Playback engine start error: %@", error);
    }
}

- (BOOL)startRecordingWithHandler:(AudioChunkHandler)handler error:(NSError **)outError {
    if (self.isRecording) {
        [self stopRecording];
    }
    
    self.chunkHandler = handler;
    _recordEngine = [[AVAudioEngine alloc] init];
    AVAudioInputNode *inputNode = _recordEngine.inputNode;
    AVAudioFormat *hardwareFormat = [inputNode inputFormatForBus:0];
    
    _recordConverter = [[AVAudioConverter alloc] initFromFormat:hardwareFormat toFormat:_targetInputFormat];
    if (!_recordConverter) {
        if (outError) {
            *outError = [NSError errorWithDomain:@"AmuseAudio" code:1 userInfo:@{NSLocalizedDescriptionKey: @"Failed to create audio converter"}];
        }
        return NO;
    }
    
    __weak typeof(self) weakSelf = self;
    [inputNode installTapOnBus:0 bufferSize:2048 format:hardwareFormat block:^(AVAudioPCMBuffer * _Nonnull buffer, AVAudioTime * _Nonnull when) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf || !strongSelf.chunkHandler) return;
        
        // Convert to 16kHz 1ch PCM
        AVAudioFrameCount outputFrameCapacity = (AVAudioFrameCount)((buffer.frameLength * strongSelf.targetInputFormat.sampleRate) / hardwareFormat.sampleRate + 1);
        AVAudioPCMBuffer *outputBuffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:strongSelf.targetInputFormat frameCapacity:outputFrameCapacity];
        
        NSError *convError = nil;
        AVAudioConverterOutputStatus status = [strongSelf.recordConverter convertToBuffer:outputBuffer error:&convError withInputFromBlock:^AVAudioBuffer * _Nullable(AVAudioPacketCount inNumberOfPackets, AVAudioConverterInputStatus * _Nonnull outStatus) {
            *outStatus = AVAudioConverterInputStatus_HaveData;
            return buffer;
        }];
        
        if (status == AVAudioConverterOutputStatus_HaveData && outputBuffer.frameLength > 0) {
            int16_t *channelData = (int16_t *)outputBuffer.int16ChannelData[0];
            NSUInteger byteLength = outputBuffer.frameLength * sizeof(int16_t);
            NSData *pcmData = [NSData dataWithBytes:channelData length:byteLength];
            strongSelf.chunkHandler(pcmData);
        }
    }];
    
    NSError *startError = nil;
    if (![_recordEngine startAndReturnError:&startError]) {
        if (outError) *outError = startError;
        return NO;
    }
    
    self.isRecording = YES;
    return YES;
}

- (void)stopRecording {
    if (!self.isRecording) return;
    [_recordEngine.inputNode removeTapOnBus:0];
    [_recordEngine stop];
    _recordEngine = nil;
    _recordConverter = nil;
    self.chunkHandler = nil;
    self.isRecording = NO;
}

- (void)playPCMChunk:(NSData *)pcmData sampleRate:(double)rate {
    if (pcmData.length == 0) return;
    
    if (!self.playbackEngine.isRunning) {
        NSError *err = nil;
        [self.playbackEngine startAndReturnError:&err];
    }
    
    if (!self.playerNode.isPlaying) {
        [self.playerNode play];
    }
    
    NSUInteger frameCount = pcmData.length / sizeof(int16_t);
    AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:self.playbackFormat frameCapacity:(AVAudioFrameCount)frameCount];
    buffer.frameLength = (AVAudioFrameCount)frameCount;
    
    int16_t *targetChannel = buffer.int16ChannelData[0];
    memcpy(targetChannel, pcmData.bytes, pcmData.length);
    
    self.isPlaying = YES;
    __weak typeof(self) weakSelf = self;
    [self.playerNode scheduleBuffer:buffer completionHandler:^{
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (strongSelf && !strongSelf.playerNode.isPlaying) {
            strongSelf.isPlaying = NO;
        }
    }];
}

- (void)stopPlayback {
    [self.playerNode stop];
    self.isPlaying = NO;
}

@end
