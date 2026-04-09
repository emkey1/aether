#import <Foundation/Foundation.h>
#include "AudioDevice.h"
#include "AudioOutput.h"
#include "fs/dyndev.h"
#include "fs/devices.h"
#include "fs/poll.h"
#include "kernel/fs.h"
#include "kernel/errno.h"
#include "util/sync.h"

#define AFMT_S16_LE_ 0x00000010

#define SNDCTL_DSP_RESET_      0x00005000
#define SNDCTL_DSP_SYNC_       0x00005001
#define SNDCTL_DSP_SPEED_      0xC0045002
#define SNDCTL_DSP_STEREO_     0xC0045003
#define SNDCTL_DSP_GETBLKSIZE_ 0xC0045004
#define SNDCTL_DSP_SETFMT_     0xC0045005
#define SNDCTL_DSP_CHANNELS_   0xC0045006
#define SNDCTL_DSP_POST_       0x00005008
#define SNDCTL_DSP_GETFMTS_    0x8004500B
#define SNDCTL_DSP_GETOSPACE_  0x8010500C
#define WAV_AUDIO_FORMAT_PCM_  1

struct guest_audio_buf_info_ {
    int fragments;
    int fragstotal;
    int fragsize;
    int bytes;
};

static const size_t kAudioRingCapacity = 256 * 1024;
static const size_t kAudioFragmentSize = 4096;
static const size_t kWAVHeaderLimit = 64 * 1024;

typedef NS_ENUM(NSUInteger, AudioInputKind) {
    AudioInputKindPCM,
    AudioInputKindUndecided,
    AudioInputKindWAVHeader,
    AudioInputKindWAVPayload,
};

typedef NS_ENUM(NSInteger, WavParseResult) {
    WavParseResultInvalid = -1,
    WavParseResultNeedMore = 0,
    WavParseResultReady = 1,
};

struct wav_parse_info {
    int sampleRate;
    int channels;
    int bitsPerSample;
    size_t dataOffset;
    size_t dataLength;
};

static uint16_t wav_read_le16(const uint8_t *bytes) {
    return (uint16_t) bytes[0] | ((uint16_t) bytes[1] << 8);
}

static uint32_t wav_read_le32(const uint8_t *bytes) {
    return (uint32_t) bytes[0]
         | ((uint32_t) bytes[1] << 8)
         | ((uint32_t) bytes[2] << 16)
         | ((uint32_t) bytes[3] << 24);
}

static WavParseResult wav_parse_header(const uint8_t *bytes, size_t length, struct wav_parse_info *info) {
    if (length < 12)
        return WavParseResultNeedMore;
    if (memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WAVE", 4) != 0)
        return WavParseResultInvalid;

    bool sawFormat = false;
    for (size_t offset = 12; offset + 8 <= length; ) {
        const uint8_t *chunk = bytes + offset;
        uint32_t chunkSize = wav_read_le32(chunk + 4);
        size_t dataOffset = offset + 8;
        size_t nextOffset = dataOffset + chunkSize + (chunkSize & 1u);
        if (nextOffset < dataOffset)
            return WavParseResultInvalid;

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (nextOffset > length)
                return WavParseResultNeedMore;
            if (chunkSize < 16)
                return WavParseResultInvalid;
            if (wav_read_le16(chunk + 8) != WAV_AUDIO_FORMAT_PCM_)
                return WavParseResultInvalid;
            info->channels = wav_read_le16(chunk + 10);
            info->sampleRate = (int) wav_read_le32(chunk + 12);
            info->bitsPerSample = wav_read_le16(chunk + 22);
            sawFormat = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!sawFormat)
                return WavParseResultInvalid;
            info->dataOffset = dataOffset;
            info->dataLength = chunkSize;
            return WavParseResultReady;
        } else if (nextOffset > length) {
            return WavParseResultNeedMore;
        }

        offset = nextOffset;
    }
    return WavParseResultNeedMore;
}

@interface AudioDeviceFile : NSObject <AudioOutputDataSource> {
    lock_t _lock;
    cond_t _spaceAvailable;
    cond_t _drained;
    cond_t _stateChanged;
    uint8_t *_buffer;
    size_t _capacity;
    size_t _readOffset;
    size_t _writeOffset;
    size_t _used;
    BOOL _started;
    BOOL _starting;
    BOOL _closing;
    BOOL _startFailed;
    NSMutableData *_inputPrefix;
    AudioInputKind _inputKind;
    size_t _wavBytesRemaining;
}

@property (nonatomic, assign) struct fd *guestFD;
@property (nonatomic, strong) AudioOutput *output;
@property (nonatomic) int sampleRate;
@property (nonatomic) int channels;
@property (nonatomic) int format;

- (ssize_t)writeBytes:(const void *)buf size:(size_t)size nonBlocking:(BOOL)nonBlocking;
- (int)pollMask;
- (int)reset;
- (int)sync;
- (int)configureFormat:(int *)format;
- (int)configureChannels:(int *)channels;
- (int)configureStereo:(int *)stereo;
- (int)configureSampleRate:(int *)sampleRate;
- (int)getBlockSize:(int *)blockSize;
- (int)getFormats:(int *)formats;
- (int)getOutputSpace:(struct guest_audio_buf_info_ *)info;
- (int)closeDevice;
- (ssize_t)enqueuePCMBytes:(const void *)buf size:(size_t)size nonBlocking:(BOOL)nonBlocking;

@end

@implementation AudioDeviceFile

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        lock_init(&_lock, "audio_dsp\0");
        cond_init(&_spaceAvailable);
        cond_init(&_drained);
        cond_init(&_stateChanged);
        _capacity = kAudioRingCapacity;
        _buffer = calloc(1, _capacity);
        if (_buffer == NULL)
            return nil;
        _inputPrefix = [NSMutableData new];
        _inputKind = AudioInputKindUndecided;
        _sampleRate = 48000;
        _channels = 2;
        _format = AFMT_S16_LE_;
    }
    return self;
}

- (void)dealloc {
    free(_buffer);
    cond_destroy(&_spaceAvailable);
    cond_destroy(&_drained);
    cond_destroy(&_stateChanged);
}

- (size_t)freeBytesLocked {
    return _capacity - _used;
}

- (size_t)copyIntoRingLocked:(const uint8_t *)src size:(size_t)size {
    size_t writable = [self freeBytesLocked];
    if (size > writable)
        size = writable;
    if (size == 0)
        return 0;

    size_t first = MIN(size, _capacity - _writeOffset);
    memcpy(_buffer + _writeOffset, src, first);
    _writeOffset = (_writeOffset + first) % _capacity;
    size_t remaining = size - first;
    if (remaining != 0) {
        memcpy(_buffer + _writeOffset, src + first, remaining);
        _writeOffset = (_writeOffset + remaining) % _capacity;
    }
    _used += size;
    return size;
}

- (size_t)consumeFromRingLocked:(void *)dst size:(size_t)size {
    if (size > _used)
        size = _used;
    if (size == 0)
        return 0;

    size_t first = MIN(size, _capacity - _readOffset);
    memcpy(dst, _buffer + _readOffset, first);
    _readOffset = (_readOffset + first) % _capacity;
    size_t remaining = size - first;
    if (remaining != 0) {
        memcpy((uint8_t *) dst + first, _buffer + _readOffset, remaining);
        _readOffset = (_readOffset + remaining) % _capacity;
    }
    _used -= size;
    return size;
}

- (void)discardInputPrefix:(size_t)size {
    if (size == 0)
        return;
    size_t remaining = _inputPrefix.length - size;
    if (remaining != 0)
        memmove(_inputPrefix.mutableBytes, (uint8_t *) _inputPrefix.mutableBytes + size, remaining);
    [_inputPrefix setLength:remaining];
}

- (int)ensureStarted {
    lock(&_lock, 0);
    while (_starting) {
        int err = wait_for(&_stateChanged, &_lock, NULL);
        if (err < 0) {
            unlock(&_lock);
            return err;
        }
    }
    if (_started) {
        unlock(&_lock);
        return 0;
    }
    if (_startFailed || _closing) {
        unlock(&_lock);
        return _EIO;
    }

    _starting = YES;
    int sampleRate = _sampleRate;
    int channels = _channels;
    unlock(&_lock);

    AudioOutput *output = [[AudioOutput alloc] initWithSampleRate:sampleRate
                                                         channels:channels
                                                       dataSource:self];
    BOOL started = [output start];

    lock(&_lock, 0);
    _starting = NO;
    if (started && !_closing) {
        self.output = output;
        _started = YES;
    } else {
        _startFailed = YES;
    }
    notify(&_stateChanged);
    unlock(&_lock);
    return started ? 0 : _EIO;
}

- (ssize_t)enqueuePCMBytes:(const void *)buf size:(size_t)size nonBlocking:(BOOL)nonBlocking {
    int err = [self ensureStarted];
    if (err < 0)
        return err;

    const uint8_t *cursor = buf;
    size_t total = 0;
    while (total < size) {
        lock(&_lock, 0);
        while ([self freeBytesLocked] == 0 && !_closing) {
            if (nonBlocking) {
                unlock(&_lock);
                return total > 0 ? (ssize_t) total : _EAGAIN;
            }
            err = wait_for(&_spaceAvailable, &_lock, NULL);
            if (err < 0) {
                unlock(&_lock);
                return total > 0 ? (ssize_t) total : err;
            }
        }
        if (_closing) {
            unlock(&_lock);
            return total > 0 ? (ssize_t) total : _EPIPE;
        }
        size_t copied = [self copyIntoRingLocked:cursor + total size:size - total];
        unlock(&_lock);
        total += copied;
    }
    return (ssize_t) total;
}

- (int)configureWAVWithInfo:(const struct wav_parse_info *)info {
    if (info->bitsPerSample != 16)
        return _EINVAL;
    if (info->channels < 1 || info->channels > 2)
        return _EINVAL;

    int format = AFMT_S16_LE_;
    int channels = info->channels;
    int sampleRate = info->sampleRate;

    int err = [self configureFormat:&format];
    if (err < 0)
        return err;
    err = [self configureChannels:&channels];
    if (err < 0)
        return err;
    err = [self configureSampleRate:&sampleRate];
    if (err < 0)
        return err;
    return 0;
}

- (ssize_t)processBufferedInputNonBlocking:(BOOL)nonBlocking {
    for (;;) {
        if (_inputKind == AudioInputKindUndecided) {
            if (_inputPrefix.length < 12)
                return 0;
            const uint8_t *bytes = _inputPrefix.bytes;
            if (memcmp(bytes, "RIFF", 4) == 0 && memcmp(bytes + 8, "WAVE", 4) == 0)
                _inputKind = AudioInputKindWAVHeader;
            else
                _inputKind = AudioInputKindPCM;
            continue;
        }

        if (_inputKind == AudioInputKindWAVHeader) {
            struct wav_parse_info info = {};
            size_t parseLength = MIN(_inputPrefix.length, kWAVHeaderLimit);
            WavParseResult result = wav_parse_header(_inputPrefix.bytes, parseLength, &info);
            if (result == WavParseResultNeedMore)
                return _inputPrefix.length > kWAVHeaderLimit ? _EINVAL : 0;
            if (result == WavParseResultInvalid)
                return _EINVAL;
            int err = [self configureWAVWithInfo:&info];
            if (err < 0)
                return err;
            [self discardInputPrefix:info.dataOffset];
            _inputKind = AudioInputKindWAVPayload;
            _wavBytesRemaining = info.dataLength;
            continue;
        }

        if (_inputKind == AudioInputKindPCM) {
            if (_inputPrefix.length == 0)
                return 0;
            ssize_t written = [self enqueuePCMBytes:_inputPrefix.bytes size:_inputPrefix.length nonBlocking:nonBlocking];
            if (written < 0)
                return written;
            [self discardInputPrefix:(size_t) written];
            return written;
        }

        if (_wavBytesRemaining == 0) {
            [_inputPrefix setLength:0];
            _inputKind = AudioInputKindUndecided;
            return 0;
        }

        size_t available = MIN(_inputPrefix.length, _wavBytesRemaining);
        if (available == 0)
            return 0;
        ssize_t written = [self enqueuePCMBytes:_inputPrefix.bytes size:available nonBlocking:nonBlocking];
        if (written < 0)
            return written;
        [self discardInputPrefix:(size_t) written];
        _wavBytesRemaining -= (size_t) written;
        if (_wavBytesRemaining == 0) {
            [_inputPrefix setLength:0];
            _inputKind = AudioInputKindUndecided;
        }
        return written;
    }
}

- (ssize_t)writeBytes:(const void *)buf size:(size_t)size nonBlocking:(BOOL)nonBlocking {
    if (size == 0)
        return 0;

    if (_inputKind == AudioInputKindPCM && _inputPrefix.length == 0)
        return [self enqueuePCMBytes:buf size:size nonBlocking:nonBlocking];

    if (_inputKind == AudioInputKindWAVPayload && _inputPrefix.length == 0) {
        size_t payloadSize = MIN(size, _wavBytesRemaining);
        ssize_t written = [self enqueuePCMBytes:buf size:payloadSize nonBlocking:nonBlocking];
        if (written < 0)
            return written;
        _wavBytesRemaining -= (size_t) written;
        if (_wavBytesRemaining == 0)
            _inputKind = AudioInputKindUndecided;
        if ((size_t) written < payloadSize)
            return written;
        return (ssize_t) size;
    }

    [_inputPrefix appendBytes:buf length:size];
    ssize_t result = [self processBufferedInputNonBlocking:nonBlocking];
    if (result < 0)
        return result;
    return (ssize_t) size;
}

- (int)pollMask {
    lock(&_lock, 0);
    int mask = _closing || _startFailed ? POLL_ERR : 0;
    if ([self freeBytesLocked] != 0)
        mask |= POLL_WRITE;
    unlock(&_lock);
    return mask;
}

- (int)reset {
    lock(&_lock, 0);
    _readOffset = 0;
    _writeOffset = 0;
    _used = 0;
    unlock(&_lock);
    [_inputPrefix setLength:0];
    _inputKind = AudioInputKindUndecided;
    _wavBytesRemaining = 0;
    notify(&_spaceAvailable);
    notify(&_drained);
    if (self.guestFD != NULL)
        poll_wakeup(self.guestFD, POLL_WRITE);
    return 0;
}

- (int)sync {
    lock(&_lock, 0);
    while (_used != 0 && !_closing) {
        int err = wait_for(&_drained, &_lock, NULL);
        if (err < 0) {
            unlock(&_lock);
            return err;
        }
    }
    unlock(&_lock);
    return 0;
}

- (int)configureFormat:(int *)format {
    if (*format != AFMT_S16_LE_ && *format != 0) {
        *format = AFMT_S16_LE_;
        return 0;
    }
    lock(&_lock, 0);
    _format = AFMT_S16_LE_;
    unlock(&_lock);
    *format = _format;
    return 0;
}

- (int)configureChannels:(int *)channels {
    int requested = *channels <= 1 ? 1 : 2;
    lock(&_lock, 0);
    if (!_started && !_starting)
        _channels = requested;
    requested = _channels;
    unlock(&_lock);
    *channels = requested;
    return 0;
}

- (int)configureStereo:(int *)stereo {
    int channels = *stereo ? 2 : 1;
    int err = [self configureChannels:&channels];
    *stereo = channels == 2;
    return err;
}

- (int)configureSampleRate:(int *)sampleRate {
    int requested = *sampleRate;
    if (requested < 8000)
        requested = 8000;
    if (requested > 48000)
        requested = 48000;
    lock(&_lock, 0);
    if (!_started && !_starting)
        _sampleRate = requested;
    requested = _sampleRate;
    unlock(&_lock);
    *sampleRate = requested;
    return 0;
}

- (int)getBlockSize:(int *)blockSize {
    *blockSize = (int) kAudioFragmentSize;
    return 0;
}

- (int)getFormats:(int *)formats {
    *formats = AFMT_S16_LE_;
    return 0;
}

- (int)getOutputSpace:(struct guest_audio_buf_info_ *)info {
    lock(&_lock, 0);
    size_t freeBytes = [self freeBytesLocked];
    unlock(&_lock);
    info->fragsize = (int) kAudioFragmentSize;
    info->fragstotal = (int) (_capacity / kAudioFragmentSize);
    info->fragments = (int) (freeBytes / kAudioFragmentSize);
    info->bytes = (int) freeBytes;
    return 0;
}

- (int)closeDevice {
    [self sync];

    lock(&_lock, 0);
    _closing = YES;
    AudioOutput *output = self.output;
    self.output = nil;
    _started = NO;
    unlock(&_lock);

    notify(&_spaceAvailable);
    notify(&_drained);
    notify(&_stateChanged);
    if (self.guestFD != NULL)
        poll_wakeup(self.guestFD, POLL_WRITE | POLL_ERR | POLL_HUP);
    [output stop];
    return 0;
}

- (size_t)audioOutput:(AudioOutput *)output renderIntoBuffer:(void *)buffer capacity:(size_t)capacity {
    (void) output;
    BOOL wakeWritable = NO;
    BOOL drained = NO;

    lock(&_lock, 0);
    size_t copied = [self consumeFromRingLocked:buffer size:capacity];
    wakeWritable = copied != 0;
    drained = _used == 0;
    unlock(&_lock);

    if (wakeWritable) {
        notify(&_spaceAvailable);
        if (self.guestFD != NULL)
            poll_wakeup(self.guestFD, POLL_WRITE);
    }
    if (drained)
        notify(&_drained);
    return copied;
}

@end

static int audio_open(int major, int minor, struct fd *fd) {
    (void) major;
    if (minor != DEV_DSP_MINOR)
        return _ENODEV;

    AudioDeviceFile *file = [AudioDeviceFile new];
    if (file == nil)
        return _ENOMEM;
    file.guestFD = fd;
    fd->data = (void *) CFBridgingRetain(file);
    return 0;
}

static int audio_close(struct fd *fd) {
    void *retainedFile = fd->data;
    AudioDeviceFile *file = (__bridge AudioDeviceFile *) retainedFile;
    fd->data = NULL;
    int err = [file closeDevice];
    CFBridgingRelease(retainedFile);
    return err;
}

static ssize_t audio_write(struct fd *fd, const void *buf, size_t bufsize) {
    AudioDeviceFile *file = (__bridge AudioDeviceFile *) fd->data;
    return [file writeBytes:buf size:bufsize nonBlocking:(fd->flags & O_NONBLOCK_) != 0];
}

static int audio_poll(struct fd *fd) {
    AudioDeviceFile *file = (__bridge AudioDeviceFile *) fd->data;
    return [file pollMask];
}

static ssize_t audio_ioctl_size(int cmd) {
    switch (cmd) {
        case SNDCTL_DSP_RESET_:
        case SNDCTL_DSP_SYNC_:
        case SNDCTL_DSP_POST_:
            return 0;
        case SNDCTL_DSP_SPEED_:
        case SNDCTL_DSP_STEREO_:
        case SNDCTL_DSP_GETBLKSIZE_:
        case SNDCTL_DSP_SETFMT_:
        case SNDCTL_DSP_CHANNELS_:
        case SNDCTL_DSP_GETFMTS_:
            return sizeof(int);
        case SNDCTL_DSP_GETOSPACE_:
            return sizeof(struct guest_audio_buf_info_);
    }
    return -1;
}

static int audio_ioctl(struct fd *fd, int cmd, void *arg) {
    AudioDeviceFile *file = (__bridge AudioDeviceFile *) fd->data;
    switch (cmd) {
        case SNDCTL_DSP_RESET_:
            return [file reset];
        case SNDCTL_DSP_SYNC_:
            return [file sync];
        case SNDCTL_DSP_SPEED_:
            return [file configureSampleRate:arg];
        case SNDCTL_DSP_STEREO_:
            return [file configureStereo:arg];
        case SNDCTL_DSP_GETBLKSIZE_:
            return [file getBlockSize:arg];
        case SNDCTL_DSP_SETFMT_:
            return [file configureFormat:arg];
        case SNDCTL_DSP_CHANNELS_:
            return [file configureChannels:arg];
        case SNDCTL_DSP_POST_:
            return 0;
        case SNDCTL_DSP_GETFMTS_:
            return [file getFormats:arg];
        case SNDCTL_DSP_GETOSPACE_:
            return [file getOutputSpace:arg];
        default:
            return _EINVAL;
    }
}

const struct dev_ops audio_dev = {
    .open = audio_open,
    .fd.write = audio_write,
    .fd.poll = audio_poll,
    .fd.ioctl_size = audio_ioctl_size,
    .fd.ioctl = audio_ioctl,
    .fd.close = audio_close,
};
