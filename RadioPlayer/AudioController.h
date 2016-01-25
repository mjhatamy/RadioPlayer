//
//  AudioController.h
//  RadioPlayer
//
//  Created by Majid Hatami Aghdam on 1/24/16.
//  Copyright © 2016 Majid Hatami Aghdam. All rights reserved.
//

#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

#include "ffplayer.h"

#define kNumAQBufs 10
//#define kAudioBufferSeconds 9


typedef enum _AUDIO_STATE {
    AUDIO_STATE_READY           = 0,
    AUDIO_STATE_STOP            = 1,
    AUDIO_STATE_PLAYING         = 2,
    AUDIO_STATE_PAUSE           = 3,
    AUDIO_STATE_SEEKING         = 4
} AUDIO_STATE;

@interface AudioController : NSObject
{
    NSString *playingFilePath_;
    AudioStreamBasicDescription audioStreamBasicDesc_;
    AudioQueueRef audioQueue_;
    AudioQueueBufferRef audioQueueBuffer_[kNumAQBufs];
    BOOL started_, finished_;
    NSTimeInterval durationTime_, startedTime_;
    NSInteger state_;
    NSTimer *seekTimer_;
    NSLock *decodeLock_;
}

+ (AudioController*)sharedInstance;

- (BOOL)createAudioQueue:(AudioStreamBasicDescription *) asbd  bufSize:(int)bufSize  frameSize:(int)frameSize;
- (OSStatus)startQueue;
- (void)_startAudio;
- (void)_stopAudio;

- (void)audioQueueOutputCallback:(AudioQueueRef)inAQ inBuffer:(AudioQueueBufferRef)inBuffer;
- (void)audioQueueIsRunningCallback;

void audioQueueOutputCallback(void *inClientData, AudioQueueRef inAQ,
                              AudioQueueBufferRef inBuffer);
void audioQueueIsRunningCallback(void *inClientData, AudioQueueRef inAQ,
                                 AudioQueuePropertyID inID);
@end
