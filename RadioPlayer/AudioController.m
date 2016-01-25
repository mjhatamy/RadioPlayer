//
//  AudioController.m
//  RadioPlayer
//
//  Created by Majid Hatami Aghdam on 1/24/16.
//  Copyright © 2016 Majid Hatami Aghdam. All rights reserved.
//

#import "AudioController.h"

@implementation AudioController{
     struct FFGIXcallbacks callbacks;
}
#pragma mark Singleton Implementation
static AudioController *sharedObject;
+ (AudioController*)sharedInstance
{
    if (sharedObject == nil) {
        sharedObject = [[super allocWithZone:NULL] init];
    }
    return sharedObject;
}


-(instancetype)init{
    if(self == [super init]){
        AVAudioSession *audioSession = [AVAudioSession sharedInstance];
        [audioSession setCategory:AVAudioSessionCategoryPlayback error:nil];
        
        callbacks._FFPlayerFailed = &FFPlayerFailed;
        callbacks._StreamOpened = &FFStreamOpened;
        callbacks._StreamClosed = &FFStreamClosed;
        callbacks._LibraryFailed = &FFLibraryFailed;
        
        callbacks._OpenAudioPlayer = &FFOpenAudioPlayer;
        callbacks._StreamElapsedClock = &FFStreamElapsedClock;
        
        ffgix_init(callbacks);
        ffgix_run();
    }
    return self;
}

- (BOOL)createAudioQueue:(AudioStreamBasicDescription *) asbd  bufSize:(int)bufSize  frameSize:(int)frameSize{
    NSLog(@"Create Audio Queue called  sampleRate:%f", asbd->mSampleRate);
    state_ = AUDIO_STATE_READY;
    finished_ = NO;
    
    decodeLock_ = [[NSLock alloc] init];
    
    audioStreamBasicDesc_.mFormatID = asbd->mFormatID;
    audioStreamBasicDesc_.mFormatFlags = asbd->mFormatFlags;
    audioStreamBasicDesc_.mSampleRate = asbd->mSampleRate;
    audioStreamBasicDesc_.mChannelsPerFrame = asbd->mChannelsPerFrame;
    audioStreamBasicDesc_.mBitsPerChannel = asbd->mBitsPerChannel;
    audioStreamBasicDesc_.mFramesPerPacket = asbd->mFramesPerPacket;
    audioStreamBasicDesc_.mBytesPerFrame = asbd->mBytesPerFrame;
    audioStreamBasicDesc_.mBytesPerPacket = asbd->mBytesPerPacket;
    audioStreamBasicDesc_.mReserved = 0;
    
    
    OSStatus status = AudioQueueNewOutput(&audioStreamBasicDesc_, audioQueueOutputCallback, (__bridge void*)self, NULL, NULL, 0, &audioQueue_);
    if (status != noErr) {
        NSError *error = [NSError errorWithDomain:NSOSStatusErrorDomain code:status userInfo:nil];
        NSLog(@"Could not create new output.  %@", error.localizedDescription );
        return NO;
    }
    
    status = AudioQueueAddPropertyListener(audioQueue_, kAudioQueueProperty_IsRunning,
                                           audioQueueIsRunningCallback, (__bridge void*)self);
    if (status != noErr) {
        
        NSLog(@"Could not add propery listener. (kAudioQueueProperty_IsRunning)");
        return NO;
    }
    
    
    //    [ffmpegDecoder_ seekTime:10.0];
    
    for (NSInteger i = 0; i < kNumAQBufs; ++i) {
        status = AudioQueueAllocateBufferWithPacketDescriptions(audioQueue_, bufSize/8, bufSize/frameSize+1,
                                                                //audioStreamBasicDesc_.mSampleRate * kAudioBufferSeconds / 8,
                                                                //asbd->mSampleRate * kAudioBufferSeconds  / asbd->mBytesPerFrame + 1,
                                                                &audioQueueBuffer_[i]);
        if (status != noErr) {
            NSLog(@"Could not allocate buffer.");
            return NO;
        }
    }
    
    NSLog(@"Audio Queue Created Successfully");
    
    [self _startAudio];
    
    return YES;
}

- (void)audioQueueOutputCallback:(AudioQueueRef)inAQ inBuffer:(AudioQueueBufferRef)inBuffer {
    if (state_ == AUDIO_STATE_PLAYING) {
        // NSLog(@"called the queue");
        [self enqueueBuffer:inBuffer];
    }
}

- (void)audioQueueIsRunningCallback {
    UInt32 isRunning;
    UInt32 size = sizeof(isRunning);
    OSStatus status = AudioQueueGetProperty(audioQueue_, kAudioQueueProperty_IsRunning, &isRunning, &size);
    
    if (status == noErr && !isRunning && state_ == AUDIO_STATE_PLAYING) {
        state_ = AUDIO_STATE_STOP;
        
        if (finished_) {
        }
    }
}

void audioQueueOutputCallback(void *inClientData, AudioQueueRef inAQ,
                              AudioQueueBufferRef inBuffer) {
    
    AudioController *audioController = (__bridge AudioController*)inClientData;
    [audioController audioQueueOutputCallback:inAQ inBuffer:inBuffer];
}
void audioQueueIsRunningCallback(void *inClientData, AudioQueueRef inAQ,
                                 AudioQueuePropertyID inID) {
    
    AudioController *audioController = (__bridge AudioController*)inClientData;
    [audioController audioQueueIsRunningCallback];
}


- (void)removeAudioQueue {
    [self _stopAudio];
    started_ = NO;
    
    for (NSInteger i = 0; i < kNumAQBufs; ++i) {
        AudioQueueFreeBuffer(audioQueue_, audioQueueBuffer_[i]);
    }
    AudioQueueDispose(audioQueue_, YES);
}

- (OSStatus)startQueue {
    OSStatus status = noErr;
    
    if (!started_) {
        status = AudioQueueStart(audioQueue_, NULL);
        if (status == noErr) {
            started_ = YES;
        }
        else {
            NSLog(@"Could not start audio queue.");
        }
    }
    
    return status;
}

- (void)_startAudio {
    NSLog(@"ready to start audio");
    if (started_) {
        AudioQueueStart(audioQueue_, NULL);
    }
    else {
        
        //[self createAudioQueue] ;
        
        [self startQueue];
        
        
    }
    
    for (NSInteger i = 0; i < kNumAQBufs; ++i) {
        [self enqueueBuffer:audioQueueBuffer_[i]];
    }
    
    state_ = AUDIO_STATE_PLAYING;
}

- (void)_stopAudio{
    if (started_) {
        AudioQueueStop(audioQueue_, YES);
        startedTime_ = 0.0;
        state_ = AUDIO_STATE_STOP;
        finished_ = NO;
    }
}

- (OSStatus)enqueueBuffer:(AudioQueueBufferRef)buffer{
    OSStatus status = noErr;
    
    //UInt64 callbacktime = av_gettime_relative();
    ffgix_ios_audio_callback(buffer);
    
    [decodeLock_ lock];
    if (buffer->mPacketDescriptionCount > 0) {
        status = AudioQueueEnqueueBuffer(audioQueue_, buffer, 0, NULL);
        if (status != noErr) {
            NSLog(@"Could not enqueue buffer.");
        }
    }
    else {
        AudioQueueStop(audioQueue_, NO);
        finished_ = YES;
    }
    
    [decodeLock_ unlock];
    
    
    buffer->mAudioDataByteSize = 0;
    buffer->mPacketDescriptionCount = 0;
    
    return status;
}




-(void) OpenAudioPlayer:(AudioStreamBasicDescription *)asbd  bufSize:(int)bufSize  frameSize:(int)frameSize{
    [self createAudioQueue:asbd bufSize:bufSize frameSize:frameSize];
}


void FFPlayerFailed(const char *msg, const char *detail){
    NSLog(@"%s  -- %s", msg, detail);
}

void FFStreamOpened(){
    NSLog(@"Stream Opened");
}

void FFStreamClosed(){
    NSLog(@"Stream Closed");
}

void FFLibraryFailed(FFGixLibFailureReason reason){
    NSLog(@"Library Failed:%d", reason);
}

void FFOpenAudioPlayer(AudioStreamBasicDescription *asbd, int bufSize, int frameSize){
    NSLog(@"OpenAudioPlayer Called");
    [[AudioController sharedInstance] OpenAudioPlayer:asbd bufSize:bufSize frameSize:frameSize];
}

void FFStreamElapsedClock(float delay, float clock){
    //NSLog(@"Clock:%0.3f", clock);
}


@end
