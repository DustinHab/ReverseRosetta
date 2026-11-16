/* coreaudio.c - sound output through CoreAudio as SDL and many games do it: the default output AudioUnit with a render
 * callback (silence), started for one second. The i386 HAL talks to the system's coreaudiod and runs its own I/O thread
 * (real time on a Mac, time sharing under rosetta). Then an AudioQueue with three buffers of silence. Prints whether the
 * callbacks ran with sensible buffers; the output must match the native PowerPC build (under rosetta the HAL is the
 * system's, through ca.c). */
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreServices/CoreServices.h>
#include <stdio.h>
#include <unistd.h>

static volatile int g_calls, g_frames_ok = 1;

static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
                       AudioBufferList *data)
{
    UInt32 i;
    (void)ref; (void)ts; (void)bus;
    if (frames == 0 || frames > 8192) g_frames_ok = 0;
    for (i = 0; i < data->mNumberBuffers; i++) memset(data->mBuffers[i].mData, 0, data->mBuffers[i].mDataByteSize);
    *flags |= kAudioUnitRenderAction_OutputIsSilence;
    g_calls++;
    return noErr;
}

static volatile int g_qcalls;
static void queue_cb(void *ref, AudioQueueRef q, AudioQueueBufferRef b)
{
    (void)ref;
    memset(b->mAudioData, 0, b->mAudioDataBytesCapacity);
    b->mAudioDataByteSize = b->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(q, b, 0, 0);
    g_qcalls++;
}

int main(void)
{
    ComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    Component comp = FindNextComponent(0, &d);
    AudioUnit au = 0;
    AURenderCallbackStruct cb = { render, 0 };
    AudioStreamBasicDescription f;
    UInt32 sz = sizeof f;
    OSStatus e;
    if (!comp) { printf("no default output\n"); return 1; }
    {   /* the default output device and its name and UID (CFStrings) */
        AudioDeviceID dev = 0;
        AudioObjectPropertyAddress pa = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, 0 };
        UInt32 n = sizeof dev;
        CFStringRef s = 0;
        char name[256] = "", uid[256] = "";
        printf("default output %d", (int)AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, 0, &n, &dev));
        pa.mSelector = kAudioObjectPropertyName; n = sizeof s;
        if (!AudioObjectGetPropertyData(dev, &pa, 0, 0, &n, &s) && s) { CFStringGetCString(s, name, sizeof name, kCFStringEncodingUTF8); CFRelease(s); }
        pa.mSelector = kAudioDevicePropertyDeviceUID; n = sizeof s; s = 0;
        if (!AudioObjectGetPropertyData(dev, &pa, 0, 0, &n, &s) && s) { CFStringGetCString(s, uid, sizeof uid, kCFStringEncodingUTF8); CFRelease(s); }
        printf(": \"%s\", UID \"%s\"\n", name, uid);
    }
    e = OpenAComponent(comp, &au);
    printf("open %d\n", (int)e);
    e = AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, &sz);
    printf("format %d: %.0f Hz, %u channels, %u bits, flags %x, native byte order %s\n", (int)e, f.mSampleRate,
           (unsigned)f.mChannelsPerFrame, (unsigned)f.mBitsPerChannel, (unsigned)f.mFormatFlags & ~kAudioFormatFlagIsBigEndian,
           (f.mFormatFlags & kAudioFormatFlagIsBigEndian) == kAudioFormatFlagsNativeEndian ? "yes" : "no");
    printf("callback %d\n", (int)AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb));
    printf("initialize %d\n", (int)AudioUnitInitialize(au));
    printf("start %d\n", (int)AudioOutputUnitStart(au));
    sleep(1);
    printf("stop %d\n", (int)AudioOutputUnitStop(au));
    printf("render callbacks: %s, buffer sizes %s\n", g_calls > 10 ? "many" : g_calls > 0 ? "few" : "none",
           g_frames_ok ? "sensible" : "wrong");
    AudioUnitUninitialize(au);
    CloseComponent(au);
    {   /* AudioQueue (AudioToolbox): three buffers of silence, played for half a second */
        AudioStreamBasicDescription q;
        AudioQueueRef aq = 0;
        int k;
        memset(&q, 0, sizeof q);
        q.mSampleRate = 22050; q.mFormatID = kAudioFormatLinearPCM;
        q.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
        q.mBytesPerPacket = 4; q.mFramesPerPacket = 1; q.mBytesPerFrame = 4; q.mChannelsPerFrame = 2; q.mBitsPerChannel = 16;
        printf("queue %d\n", (int)AudioQueueNewOutput(&q, queue_cb, 0, 0, 0, 0, &aq));
        for (k = 0; k < 3; k++) {
            AudioQueueBufferRef b = 0;
            AudioQueueAllocateBuffer(aq, 4096, &b);
            queue_cb(0, aq, b);
        }
        printf("queue start %d\n", (int)AudioQueueStart(aq, 0));
        sleep(1);
        printf("queue stop %d\n", (int)AudioQueueStop(aq, 1));
        printf("queue callbacks: %s\n", g_qcalls > 6 ? "many" : g_qcalls > 3 ? "few" : "none");
        AudioQueueDispose(aq, 1);
    }
    return 0;
}
