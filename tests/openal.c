/* openal.c - OpenAL: device, context, listener, sources, buffers, queueing and playing (silently: listener gain 0),
 * values read back through every kind of getter. Built for i386 (translated: al.c bridges to the system's OpenAL)
 * and natively; the output must be the same. */
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define N 2205
static short pcm[N];

int main(void)
{
    ALCdevice *dev = alcOpenDevice(0);
    ALCcontext *ctx;
    ALCint attrs[] = { ALC_FREQUENCY, 22050, 0 }, major = 0, minor = 0;
    ALuint src[2] = { 0, 0 }, buf = 0, got = 0;
    ALfloat v3[3], o6[6] = { 0, 0, -1, 0, 1, 0 }, f = 0;
    ALint i, state = 0, done = 0, bits = 0, chans = 0, size = 0, freq = 0, looping = 0;
    if (!dev) { printf("no device\n"); return 1; }
    ctx = alcCreateContext(dev, attrs);
    printf("context %s, current %d\n", ctx ? "yes" : "no", alcMakeContextCurrent(ctx));
    alcGetIntegerv(dev, ALC_MAJOR_VERSION, 1, &major);
    alcGetIntegerv(dev, ALC_MINOR_VERSION, 1, &minor);
    printf("ALC %d.%d, AL version %s, device \"%s\"\n", major, minor, alGetString(AL_VERSION) ? "present" : "missing",
           alcGetString(dev, ALC_DEVICE_SPECIFIER));
    alListenerf(AL_GAIN, 0.0f);                             /* silent */
    alGetListenerf(AL_GAIN, &f);
    alListener3f(AL_POSITION, 1.0f, 2.5f, -3.0f);
    alGetListener3f(AL_POSITION, &v3[0], &v3[1], &v3[2]);
    printf("listener gain %g, position %g %g %g\n", f, v3[0], v3[1], v3[2]);
    alListenerfv(AL_ORIENTATION, o6);
    memset(o6, 0, sizeof o6);
    alGetListenerfv(AL_ORIENTATION, o6);
    printf("orientation %g %g %g %g %g %g\n", o6[0], o6[1], o6[2], o6[3], o6[4], o6[5]);
    alGenSources(2, src);
    printf("sources %s, %s\n", alIsSource(src[0]) ? "valid" : "invalid", src[0] != src[1] ? "different" : "the same");
    alSourcef(src[0], AL_PITCH, 1.5f);
    alGetSourcef(src[0], AL_PITCH, &f);
    alSource3f(src[0], AL_VELOCITY, -1.0f, 0.25f, 8.0f);
    alGetSourcefv(src[0], AL_VELOCITY, v3);
    alSourcei(src[0], AL_LOOPING, AL_TRUE);
    alGetSourcei(src[0], AL_LOOPING, &looping);
    printf("pitch %g, velocity %g %g %g, looping %d\n", f, v3[0], v3[1], v3[2], looping);
    for (i = 0; i < N; i++) pcm[i] = (short)(8000 * sin(i * 2 * 3.14159265 * 440 / 22050));
    alGenBuffers(1, &buf);
    alBufferData(buf, AL_FORMAT_MONO16, pcm, sizeof pcm, 22050);
    alGetBufferi(buf, AL_FREQUENCY, &freq);
    alGetBufferi(buf, AL_BITS, &bits);
    alGetBufferi(buf, AL_CHANNELS, &chans);
    alGetBufferi(buf, AL_SIZE, &size);
    printf("buffer %d Hz, %d bits, %d channels, %d bytes\n", freq, bits, chans, size);
    alSourceQueueBuffers(src[1], 1, &buf);
    alSourcePlay(src[1]);
    alGetSourcei(src[1], AL_SOURCE_STATE, &state);
    printf("playing: %s\n", state == AL_PLAYING ? "yes" : "no");
    for (i = 0; i < 50 && state == AL_PLAYING; i++) { usleep(20000); alGetSourcei(src[1], AL_SOURCE_STATE, &state); }
    alGetSourcei(src[1], AL_BUFFERS_PROCESSED, &done);
    alSourceUnqueueBuffers(src[1], 1, &got);
    printf("after the buffer: %s, %d processed, unqueued %s\n", state == AL_STOPPED ? "stopped" : "not stopped", done,
           got == buf ? "the buffer" : "something else");
    printf("doppler factor %g, error %x\n", alGetFloat(AL_DOPPLER_FACTOR), alGetError());
    {                                                       /* an extension only through alGetProcAddress, called */
        typedef ALvoid (*StaticFn)(ALint, ALenum, ALvoid *, ALsizei, ALsizei);
        StaticFn st = (StaticFn)alGetProcAddress("alBufferDataStatic");
        ALuint b2 = 0;
        ALint sz2 = 0, fr2 = 0;
        printf("alBufferDataStatic %s", st ? "found" : "missing");
        if (st) {
            alGenBuffers(1, &b2);
            st((ALint)b2, AL_FORMAT_MONO16, pcm, (ALsizei)(sizeof pcm / 2), 11025);
            alGetBufferi(b2, AL_SIZE, &sz2);
            alGetBufferi(b2, AL_FREQUENCY, &fr2);
            alDeleteBuffers(1, &b2);
            printf(", static buffer %d bytes at %d Hz, error %x", sz2, fr2, alGetError());
        }
        printf("\n");
    }
    alDeleteSources(2, src);
    alDeleteBuffers(1, &buf);
    printf("deleted: %s, error %x\n", alIsBuffer(buf) ? "no" : "yes", alGetError());
    alcMakeContextCurrent(0);
    alcDestroyContext(ctx);
    printf("closed %d\n", alcCloseDevice(dev));
    return 0;
}
