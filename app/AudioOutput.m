#import "AudioOutput.h"
#import <AudioToolbox/AudioQueue.h>

static const UInt32 kAudioOutputBufferCount = 3;
static const UInt32 kAudioOutputBufferBytes = 4096;

@interface AudioOutput () {
    AudioQueueRef _queue;
    AudioQueueBufferRef _buffers[kAudioOutputBufferCount];
    BOOL _running;
}

@property (nonatomic, weak) id<AudioOutputDataSource> dataSource;
@property (nonatomic) int sampleRate;
@property (nonatomic) int channels;

@end

static void AudioOutputCallback(void *userData, AudioQueueRef queue, AudioQueueBufferRef buffer);

@implementation AudioOutput

- (instancetype)initWithSampleRate:(int)sampleRate
                          channels:(int)channels
                        dataSource:(id<AudioOutputDataSource>)dataSource {
    self = [super init];
    if (self != nil) {
        _sampleRate = sampleRate;
        _channels = channels;
        _dataSource = dataSource;
    }
    return self;
}

- (void)dealloc {
    [self stop];
}

- (BOOL)start {
    if (_running)
        return YES;

    AudioStreamBasicDescription format = {
        .mSampleRate = self.sampleRate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mFramesPerPacket = 1,
        .mChannelsPerFrame = (UInt32) self.channels,
        .mBitsPerChannel = 16,
        .mBytesPerFrame = (UInt32) (self.channels * sizeof(int16_t)),
        .mBytesPerPacket = (UInt32) (self.channels * sizeof(int16_t)),
    };

    OSStatus status = AudioQueueNewOutput(&format, AudioOutputCallback, (__bridge void *) self,
                                          NULL, NULL, 0, &_queue);
    if (status != noErr)
        return NO;

    _running = YES;
    for (UInt32 index = 0; index < kAudioOutputBufferCount; index++) {
        status = AudioQueueAllocateBuffer(_queue, kAudioOutputBufferBytes, &_buffers[index]);
        if (status != noErr)
            break;
        [self enqueueBuffer:_buffers[index]];
    }
    if (status != noErr) {
        [self stop];
        return NO;
    }

    status = AudioQueueStart(_queue, NULL);
    if (status != noErr) {
        [self stop];
        return NO;
    }
    return YES;
}

- (void)stop {
    if (!_running && _queue == NULL)
        return;

    _running = NO;
    if (_queue != NULL) {
        AudioQueueStop(_queue, true);
        AudioQueueDispose(_queue, true);
        _queue = NULL;
    }
    for (UInt32 index = 0; index < kAudioOutputBufferCount; index++)
        _buffers[index] = NULL;
}

- (void)enqueueBuffer:(AudioQueueBufferRef)buffer {
    if (!_running || buffer == NULL)
        return;

    memset(buffer->mAudioData, 0, buffer->mAudioDataBytesCapacity);
    @autoreleasepool {
        [self.dataSource audioOutput:self
                    renderIntoBuffer:buffer->mAudioData
                            capacity:buffer->mAudioDataBytesCapacity];
    }
    buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(_queue, buffer, 0, NULL);
}

@end

static void AudioOutputCallback(void *userData, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    (void) queue;
    AudioOutput *output = (__bridge AudioOutput *) userData;
    [output enqueueBuffer:buffer];
}
