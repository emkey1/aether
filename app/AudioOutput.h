#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@class AudioOutput;

@protocol AudioOutputDataSource <NSObject>
- (size_t)audioOutput:(AudioOutput *)output renderIntoBuffer:(void *)buffer capacity:(size_t)capacity;
@end

@interface AudioOutput : NSObject

- (instancetype)initWithSampleRate:(int)sampleRate
                          channels:(int)channels
                        dataSource:(id<AudioOutputDataSource>)dataSource;
- (BOOL)start;
- (void)stop;

@end

NS_ASSUME_NONNULL_END
