// SPDX-License-Identifier: BSD-2-Clause-Patent
// FajitaAudio.driver: the CoreAudio HAL plug-in through which iOS's haptics reach the OnePlus 6T's vibration motor.
//
// iOS 16 on the iPhone X plays every haptic (UIKit feedback, keyboard taps, CoreHaptics patterns, notification and ringer
// vibrations) as a one-channel waveform on an audio device whose UID is "Actuator", through mediaserverd's HAL and the virtual
// audio device (/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin, which knows the iPhone's devices by their UIDs and builds its routes
// from them; MobileGestalt's DeviceSupportsClosedLoopHaptics, /product/haptics closed-loop in the device tree, makes iOS use it). On
// a real iPhone the device comes from the AOP's audio plug-in; here this plug-in presents it, plus the two companions the virtual
// device will not build routes without (the design HackiOS found on its redwood port, written anew for this board):
//   "Speaker"   stereo output (played into nowhere: no audio hardware is driven yet) and a silent stereo input
//   "Hawking"   one silent input
//   "Actuator"  one output channel; each I/O buffer becomes a level (FajitaHapticPolicy.h) written to /dev/fajita-haptic, where
//               the kernel (package/FajitaHaptics.cpp, fajita-haptics=3) drives the PMI8998 LRA by it
// The kernel mounts this bundle over /Library/Audio/Plug-Ins/HAL (fajita-haptics=3, ios16/patches/xnu-8796/
// zzy-native-gpu-addon-mount.patch); scripts/fajita/build-fajita-audio.sh builds and signs it, build.sh puts it in the add-on.
//
// The AudioServerPlugIn interface is the one macOS documents (CoreAudio/AudioServerPlugIn.h); the iOS SDK has no header for it, so
// the parts used are declared below. Diagnostics go to the kernel log through /dev/fajita-haptic (a write starting with 0xffff).
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreAudioTypes/CoreAudioTypes.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../FajitaHapticPolicy.h"

// ---- the AudioServerPlugIn interface ----
typedef UInt32 AudioObjectID;
typedef struct {UInt32 mSelector, mScope, mElement;} AudioObjectPropertyAddress;
typedef struct {AudioStreamBasicDescription mFormat; AudioValueRange mSampleRateRange;} AudioStreamRangedDescription;
typedef struct {UInt32 mClientID; pid_t mProcessID; Boolean mIsNativeEndian; CFStringRef mBundleID;} AudioServerPlugInClientInfo;
typedef struct {
    UInt64 mIOCycleCounter;
    UInt32 mNominalIOBufferFrameSize;
    AudioTimeStamp mCurrentTime, mInputTime, mOutputTime;
    Float64 mMasterHostTicksPerFrame, mDeviceHostTicksPerFrame;
} AudioServerPlugInIOCycleInfo;
typedef struct AudioServerPlugInHostInterface AudioServerPlugInHostInterface;
typedef const AudioServerPlugInHostInterface *AudioServerPlugInHostRef;
struct AudioServerPlugInHostInterface {
    OSStatus (*PropertiesChanged)(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress *);
    OSStatus (*CopyFromStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef *);
    OSStatus (*WriteToStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef);
    OSStatus (*DeleteFromStorage)(AudioServerPlugInHostRef, CFStringRef);
    OSStatus (*RequestDeviceConfigurationChange)(AudioServerPlugInHostRef, AudioObjectID, UInt64, void *);
};
typedef struct AudioServerPlugInDriverInterface AudioServerPlugInDriverInterface;
typedef AudioServerPlugInDriverInterface **AudioServerPlugInDriverRef;
struct AudioServerPlugInDriverInterface {
    void *_reserved;
    HRESULT (*QueryInterface)(void *, REFIID, LPVOID *);
    ULONG (*AddRef)(void *);
    ULONG (*Release)(void *);
    OSStatus (*Initialize)(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
    OSStatus (*CreateDevice)(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
    OSStatus (*DestroyDevice)(AudioServerPlugInDriverRef, AudioObjectID);
    OSStatus (*AddDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
    OSStatus (*RemoveDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
    OSStatus (*PerformDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
    OSStatus (*AbortDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
    Boolean (*HasProperty)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
    OSStatus (*IsPropertySettable)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
    OSStatus (*GetPropertyDataSize)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *,
                                    UInt32 *);
    OSStatus (*GetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *,
                                UInt32, UInt32 *, void *);
    OSStatus (*SetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *,
                                UInt32, const void *);
    OSStatus (*StartIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
    OSStatus (*StopIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
    OSStatus (*GetZeroTimeStamp)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
    OSStatus (*WillDoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
    OSStatus (*BeginIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
    OSStatus (*DoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32,
                              const AudioServerPlugInIOCycleInfo *, void *, void *);
    OSStatus (*EndIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
};
// 443ABAB8-E7B3-491A-B985-BEB9187030DB, EEA5773D-CC43-49F1-8E00-8F96E7D23B17
#define TYPE_UUID CFUUIDGetConstantUUIDWithBytes(NULL, 0x44, 0x3A, 0xBA, 0xB8, 0xE7, 0xB3, 0x49, 0x1A, 0xB9, 0x85, 0xBE, 0xB9, 0x18, 0x70, 0x30, 0xDB)
#define DRIVER_UUID CFUUIDGetConstantUUIDWithBytes(NULL, 0xEE, 0xA5, 0x77, 0x3D, 0xCC, 0x43, 0x49, 0xF1, 0x8E, 0x00, 0x8F, 0x96, 0xE7, 0xD2, 0x3B, 0x17)

#define FCC(a, b, c, d) ((UInt32)(a) << 24 | (UInt32)(b) << 16 | (UInt32)(c) << 8 | (UInt32)(d))
enum {
    ErrUnknownProperty = FCC('w', 'h', 'o', '?'), ErrBadSize = FCC('!', 's', 'i', 'z'), ErrIllegal = FCC('n', 'o', 'p', 'e'),
    ErrBadObject = FCC('!', 'o', 'b', 'j'), ErrUnsupported = FCC('u', 'n', 'o', 'p'), ErrFormat = FCC('!', 'd', 'a', 't'),
    ScopeGlobal = FCC('g', 'l', 'o', 'b'), ScopeInput = FCC('i', 'n', 'p', 't'), ScopeOutput = FCC('o', 'u', 't', 'p'),
    ClassObject = FCC('a', 'o', 'b', 'j'), ClassPlugIn = FCC('a', 'p', 'l', 'g'), ClassDevice = FCC('a', 'd', 'e', 'v'),
    ClassStream = FCC('a', 's', 't', 'r'),
    PBaseClass = FCC('b', 'c', 'l', 's'), PClass = FCC('c', 'l', 'a', 's'), POwner = FCC('s', 't', 'd', 'v'), PName = FCC('l', 'n', 'a', 'm'),
    PManufacturer = FCC('l', 'm', 'a', 'k'), POwned = FCC('o', 'w', 'n', 'd'), PControls = FCC('c', 't', 'r', 'l'),
    PBoxList = FCC('b', 'o', 'x', '#'), PUIDToBox = FCC('u', 'i', 'd', 'b'), PDeviceList = FCC('d', 'e', 'v', '#'),
    PUIDToDevice = FCC('u', 'i', 'd', 'd'), PResourceBundle = FCC('r', 's', 'r', 'c'),
    PDeviceUID = FCC('u', 'i', 'd', ' '), PModelUID = FCC('m', 'u', 'i', 'd'), PTransport = FCC('t', 'r', 'a', 'n'),
    PRelated = FCC('a', 'k', 'i', 'n'), PClockDomain = FCC('c', 'l', 'k', 'd'), PAlive = FCC('l', 'i', 'v', 'n'),
    PRunning = FCC('g', 'o', 'i', 'n'), PCanDefault = FCC('d', 'f', 'l', 't'), PCanDefaultSystem = FCC('s', 'f', 'l', 't'),
    PLatency = FCC('l', 't', 'n', 'c'), PStreams = FCC('s', 't', 'm', '#'), PSafety = FCC('s', 'a', 'f', 't'),
    PRate = FCC('n', 's', 'r', 't'), PRates = FCC('n', 's', 'r', '#'), PHidden = FCC('h', 'i', 'd', 'n'),
    PStereo = FCC('d', 'c', 'h', '2'), PPeriod = FCC('r', 'i', 'n', 'g'),
    SActive = FCC('s', 'a', 'c', 't'), SDirection = FCC('s', 'd', 'i', 'r'), STerminal = FCC('t', 'e', 'r', 'm'),
    SStart = FCC('s', 'c', 'h', 'n'), SVirtual = FCC('s', 'f', 'm', 't'), SVirtuals = FCC('s', 'f', 'm', 'a'),
    SPhysical = FCC('p', 'f', 't', ' '), SPhysicals = FCC('p', 'f', 't', 'a'),
    TermSpeaker = FCC('s', 'p', 'k', 'r'), TermMic = FCC('m', 'i', 'c', 'r'), TransportBuiltIn = FCC('b', 'l', 't', 'n'),
    OpRead = FCC('r', 'e', 'a', 'd'), OpWriteMix = FCC('r', 'i', 't', 'e'),
};

// ---- the devices ----
#define RATE 48000.0
#define PERIOD 16384u   // frames between the zero time stamps the device reports
enum {
    OPlugIn = 1, OSpeaker, OSpeakerOut, OSpeakerIn, OHawking, OHawkingIn, OActuator, OActuatorOut, OLast = OActuatorOut
};
typedef struct {
    AudioObjectID object, output, input;
    UInt32 outChannels, inChannels;
    CFStringRef uid;
    unsigned running;
    UInt64 anchor, periods;
} Device;
static Device devices[] = {
    {OSpeaker, OSpeakerOut, OSpeakerIn, 2, 2, CFSTR("Speaker"), 0, 0, 0},
    {OHawking, 0, OHawkingIn, 0, 1, CFSTR("Hawking"), 0, 0, 0},
    {OActuator, OActuatorOut, 0, 1, 0, CFSTR("Actuator"), 0, 0, 0},
};
#define NDEVICES (sizeof devices / sizeof devices[0])

static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static Float64 gTicksPerFrame, gNsPerTick;
static bool gInactive[OLast + 1];
static int gMotor = -1;          // /dev/fajita-haptic; -2: not there (the kernel runs without fajita-haptics=3)
static fhp_sender gSender;
static UInt64 gActuatorFrames, gActuatorLoud;

static void say(const char *fmt, ...) {
    char line[200] = {(char)0xff, (char)0xff};
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line + 2, sizeof line - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 3) n = (int)sizeof line - 3;
    if (gMotor == -1) gMotor = open("/dev/fajita-haptic", O_WRONLY | O_NONBLOCK);
    if (gMotor >= 0) (void)write(gMotor, line, (size_t)n + 3);   // the NUL too: the kernel prints it as a string
}

static Device *deviceOf(AudioObjectID o) {
    for (unsigned i = 0; i < NDEVICES; i++)
        if (o && (o == devices[i].object || o == devices[i].output || o == devices[i].input)) return &devices[i];
    return NULL;
}

static AudioStreamBasicDescription formatOf(AudioObjectID stream) {
    const Device *d = deviceOf(stream);
    UInt32 ch = stream == d->output ? d->outChannels : d->inChannels;
    AudioStreamBasicDescription f = {
        .mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 2 * ch, .mFramesPerPacket = 1, .mBytesPerFrame = 2 * ch, .mChannelsPerFrame = ch, .mBitsPerChannel = 16,
    };
    return f;
}

// A property value: up to a few words, a number, a string, a format.
typedef struct {
    UInt32 size;
    bool known, string;
    union {
        UInt32 w[8];
        Float64 f64;
        CFStringRef s;
        AudioValueRange range;
        AudioStreamBasicDescription format;
        AudioStreamRangedDescription ranged;
    } u;
} Value;
#define WORD(x) (v.u.w[0] = (x), v.size = 4, v.known = true)
#define ADD(x) (v.u.w[v.size / 4] = (x), v.size += 4, v.known = true)
#define LISTED() (v.known = true)
#define STR(x) (v.u.s = (x), v.string = true, v.size = sizeof(CFStringRef), v.known = true)

static Value property(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q) {
    Value v;
    memset(&v, 0, sizeof v);
    const Device *d = deviceOf(o);
    if (o != OPlugIn && !d) return v;
    const UInt32 sel = a->mSelector, scope = a->mScope;
    switch (sel) {
    case PBaseClass: WORD(ClassObject); return v;
    case PClass: WORD(o == OPlugIn ? ClassPlugIn : o == d->object ? ClassDevice : ClassStream); return v;
    case POwner: WORD(o == OPlugIn ? 0 : o == d->object ? OPlugIn : d->object); return v;
    }
    if (o == OPlugIn) {
        switch (sel) {
        case PManufacturer: STR(CFSTR("fajita")); break;
        case POwned: case PDeviceList: LISTED(); for (unsigned i = 0; i < NDEVICES; i++) ADD(devices[i].object); break;
        case PBoxList: LISTED(); break;
        case PUIDToBox: WORD(0); break;
        case PUIDToDevice:
            WORD(0);
            for (unsigned i = 0; i < NDEVICES; i++)
                if (qsize == sizeof(CFStringRef) && q && *(const CFStringRef *)q && CFEqual(*(const CFStringRef *)q, devices[i].uid))
                    WORD(devices[i].object);
            break;
        case PResourceBundle: STR(CFSTR("")); break;
        }
        return v;
    }
    if (o == d->object) {
        switch (sel) {
        case PManufacturer: STR(CFSTR("fajita")); break;
        case PName: case PDeviceUID: case PModelUID: STR(d->uid); break;
        case PTransport: WORD(TransportBuiltIn); break;
        case PRelated: WORD(d->object); break;
        case PClockDomain: case PLatency: case PSafety: case PHidden: WORD(0); break;
        case PAlive: WORD(1); break;
        case PCanDefault: case PCanDefaultSystem: WORD(d->output != 0); break;
        case PRunning: WORD(d->running != 0); break;
        case POwned: case PStreams:
            LISTED();
            if (scope != ScopeInput && d->output) ADD(d->output);
            if (scope != ScopeOutput && d->input) ADD(d->input);
            break;
        case PControls: LISTED(); break;
        case PRate: v.u.f64 = RATE; v.size = sizeof(Float64); v.known = true; break;
        case PRates: v.u.range.mMinimum = v.u.range.mMaximum = RATE; v.size = sizeof(AudioValueRange); v.known = true; break;
        case PStereo: v.u.w[0] = 1; v.u.w[1] = d->outChannels > 1 ? 2 : 1; v.size = 8; v.known = true; break;
        case PPeriod: WORD(PERIOD); break;
        }
        return v;
    }
    switch (sel) {   // a stream
    case SActive: WORD(!gInactive[o]); break;
    case SDirection: WORD(o == d->input); break;
    case STerminal: WORD(o == d->input ? TermMic : TermSpeaker); break;
    case SStart: WORD(1); break;
    case PLatency: WORD(0); break;
    case SVirtual: case SPhysical: v.u.format = formatOf(o); v.size = sizeof(AudioStreamBasicDescription); v.known = true; break;
    case SVirtuals: case SPhysicals:
        v.u.ranged.mFormat = formatOf(o);
        v.u.ranged.mSampleRateRange.mMinimum = v.u.ranged.mSampleRateRange.mMaximum = RATE;
        v.size = sizeof(AudioStreamRangedDescription); v.known = true;
        break;
    }
    return v;
}

// What the HAL or the virtual audio device asks that is not answered, the first times (what a refused device may lack).
static void unknown(AudioObjectID o, const AudioObjectPropertyAddress *a) {
    static UInt32 told[48][3];
    static unsigned n, dropped;
    for (unsigned i = 0; i < n; i++)
        if (told[i][0] == o && told[i][1] == a->mSelector && told[i][2] == a->mScope) return;
    if (n == 48) {if (!dropped++) say("more unknown properties than the 48 told (counting no further)"); return;}
    told[n][0] = o; told[n][1] = a->mSelector; told[n][2] = a->mScope; n++;
    const UInt32 s = a->mSelector, c = a->mScope;
    say("asked object %u for '%c%c%c%c' scope '%c%c%c%c' element %u: not there", (unsigned)o, (int)(s >> 24), (int)(s >> 16 & 255),
        (int)(s >> 8 & 255), (int)(s & 255), (int)(c >> 24), (int)(c >> 16 & 255), (int)(c >> 8 & 255), (int)(c & 255), (unsigned)a->mElement);
}

static HRESULT queryInterface(void *driver, REFIID uuid, LPVOID *out) {
    CFUUIDRef r = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    Boolean ok = CFEqual(r, IUnknownUUID) || CFEqual(r, DRIVER_UUID);
    CFRelease(r);
    if (!out || !ok) return E_NOINTERFACE;
    *out = driver;
    return S_OK;
}
static ULONG addRef(void *driver) {(void)driver; return 1;}
static ULONG release(void *driver) {(void)driver; return 1;}

static OSStatus initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host) {
    (void)driver;
    gHost = host;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    gTicksPerFrame = 1e9 * tb.denom / tb.numer / RATE;
    gNsPerTick = (Float64)tb.numer / tb.denom;
    say("plug-in loaded in pid %d: Speaker, Hawking, Actuator (/dev/fajita-haptic %s)", (int)getpid(), gMotor >= 0 ? "open" : "missing");
    return 0;
}
static OSStatus createDevice(AudioServerPlugInDriverRef dr, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *o) {
    (void)dr; (void)desc; (void)ci; (void)o;
    return ErrUnsupported;
}
static OSStatus destroyDevice(AudioServerPlugInDriverRef dr, AudioObjectID o) {(void)dr; (void)o; return ErrUnsupported;}
static OSStatus deviceClient(AudioServerPlugInDriverRef dr, AudioObjectID o, const AudioServerPlugInClientInfo *ci) {(void)dr; (void)o; (void)ci; return 0;}
static OSStatus configChange(AudioServerPlugInDriverRef dr, AudioObjectID o, UInt64 action, void *info) {(void)dr; (void)o; (void)action; (void)info; return 0;}

static Boolean hasProperty(AudioServerPlugInDriverRef dr, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a) {
    (void)dr; (void)pid;
    if (!a) return false;
    Value v = property(o, a, 0, NULL);
    if (!v.known) unknown(o, a);
    return v.known;
}
static OSStatus isSettable(AudioServerPlugInDriverRef dr, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    if (!a || !out || !hasProperty(dr, o, pid, a)) return ErrUnknownProperty;
    const Device *d = deviceOf(o);
    if (o == OPlugIn) *out = false;
    else if (o == d->object) *out = a->mSelector == PRate;
    else *out = a->mSelector == SActive || a->mSelector == SVirtual || a->mSelector == SPhysical;
    return 0;
}
static OSStatus getSize(AudioServerPlugInDriverRef dr, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                        const void *q, UInt32 *size) {
    if (!a || !size || !hasProperty(dr, o, pid, a)) return o < OPlugIn || o > OLast ? ErrBadObject : ErrUnknownProperty;
    *size = property(o, a, qs, q).size;
    return 0;
}
static OSStatus getData(AudioServerPlugInDriverRef dr, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                        const void *q, UInt32 room, UInt32 *size, void *data) {
    if (!a || !size || !data || !hasProperty(dr, o, pid, a)) return o < OPlugIn || o > OLast ? ErrBadObject : ErrUnknownProperty;
    pthread_mutex_lock(&gLock);
    Value v = property(o, a, qs, q);
    pthread_mutex_unlock(&gLock);
    if (v.size > room) {
        // lists may be asked for in part, nothing else
        if (a->mSelector != POwned && a->mSelector != PStreams && a->mSelector != PDeviceList && a->mSelector != PControls) return ErrBadSize;
        v.size = room - room % 4;
    }
    memcpy(data, &v.u, v.size);
    if (v.string && v.size) CFRetain(v.u.s);
    *size = v.size;
    return 0;
}
static OSStatus setData(AudioServerPlugInDriverRef dr, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                        const void *q, UInt32 size, const void *data) {
    (void)qs; (void)q;
    Boolean settable = false;
    if (!a || !data || isSettable(dr, o, pid, a, &settable)) return ErrUnknownProperty;
    if (!settable) return ErrIllegal;
    if (a->mSelector == SActive) {
        if (size != 4) return ErrBadSize;
        pthread_mutex_lock(&gLock);
        gInactive[o] = *(const UInt32 *)data == 0;
        pthread_mutex_unlock(&gLock);
        return 0;
    }
    if (a->mSelector == PRate) return size == sizeof(Float64) && *(const Float64 *)data == RATE ? 0 : ErrFormat;
    if (size != sizeof(AudioStreamBasicDescription)) return ErrBadSize;
    const AudioStreamBasicDescription *w = data, f = formatOf(o);
    return w->mSampleRate == f.mSampleRate && w->mFormatID == f.mFormatID && w->mChannelsPerFrame == f.mChannelsPerFrame &&
           w->mBitsPerChannel == f.mBitsPerChannel && (w->mFormatFlags & kAudioFormatFlagIsSignedInteger) ? 0 : ErrFormat;
}

static OSStatus startIO(AudioServerPlugInDriverRef dr, AudioObjectID o, UInt32 client) {
    (void)dr; (void)client;
    Device *d = deviceOf(o);
    if (!d || o != d->object) return ErrBadObject;
    pthread_mutex_lock(&gLock);
    if (d->running++ == 0) {
        d->anchor = mach_absolute_time();
        d->periods = 0;
        if (d->object == OActuator) say("the Actuator starts (haptics on)");
    }
    pthread_mutex_unlock(&gLock);
    return 0;
}
static OSStatus stopIO(AudioServerPlugInDriverRef dr, AudioObjectID o, UInt32 client) {
    (void)dr; (void)client;
    Device *d = deviceOf(o);
    if (!d || o != d->object) return ErrBadObject;
    pthread_mutex_lock(&gLock);
    if (d->running && --d->running == 0 && d->object == OActuator) {
        say("the Actuator stops: %llu frames, %llu of them above silence, %u levels sent", gActuatorFrames, gActuatorLoud, gSender.sends);
        if (gSender.sent && gMotor >= 0) {uint16_t z = 0; (void)write(gMotor, &z, 2);}
        gSender.sent = 0;
    }
    pthread_mutex_unlock(&gLock);
    return 0;
}
static OSStatus zeroTimeStamp(AudioServerPlugInDriverRef dr, AudioObjectID o, UInt32 client, Float64 *sample, UInt64 *hostTime, UInt64 *seed) {
    (void)dr; (void)client;
    Device *d = deviceOf(o);
    if (!d) return ErrBadObject;
    const Float64 tpp = gTicksPerFrame * PERIOD;
    const UInt64 now = mach_absolute_time();
    pthread_mutex_lock(&gLock);
    while (now >= d->anchor + (UInt64)((Float64)(d->periods + 1) * tpp)) d->periods++;
    *sample = (Float64)d->periods * PERIOD;
    *hostTime = d->anchor + (UInt64)((Float64)d->periods * tpp);
    *seed = 1;
    pthread_mutex_unlock(&gLock);
    return 0;
}
static OSStatus willDo(AudioServerPlugInDriverRef dr, AudioObjectID o, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace) {
    (void)dr; (void)client;
    const Device *d = deviceOf(o);
    if (!d || !will || !inPlace) return ErrBadObject;
    *will = (op == OpWriteMix && d->output) || (op == OpRead && d->input);
    *inPlace = true;
    return 0;
}
static OSStatus beginEnd(AudioServerPlugInDriverRef dr, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *c) {
    (void)dr; (void)o; (void)client; (void)op; (void)frames; (void)c;
    return 0;
}

// The Actuator's samples -> a level -> /dev/fajita-haptic (when the sender says so).
static void actuator(const int16_t *s, UInt32 frames) {
    const unsigned level = fhp_level(s, frames);
    gActuatorFrames += frames;
    if (level) gActuatorLoud += frames;
    const uint64_t ms = (uint64_t)((Float64)mach_absolute_time() * gNsPerTick / 1e6);
    if (!fhp_should_send(&gSender, level, ms)) return;
    if (gMotor == -1) gMotor = open("/dev/fajita-haptic", O_WRONLY | O_NONBLOCK);
    if (gMotor == -1) gMotor = -2;
    if (gMotor >= 0) {uint16_t l = (uint16_t)level; (void)write(gMotor, &l, 2);}
    static unsigned firsts;
    if (level && firsts < 8) {firsts++; say("the Actuator plays: %u frames at level %u", (unsigned)frames, level);}
}

static OSStatus doIO(AudioServerPlugInDriverRef dr, AudioObjectID o, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 frames,
                     const AudioServerPlugInIOCycleInfo *c, void *buf, void *secondary) {
    (void)dr; (void)o; (void)client; (void)c; (void)secondary;
    const Device *d = deviceOf(stream);
    if (!d) return ErrBadObject;
    if (op == OpRead) memset(buf, 0, (size_t)frames * 2 * d->inChannels);   // the inputs are silent
    else if (op == OpWriteMix && stream == OActuatorOut) actuator(buf, frames);
    return 0;                                                                 // the Speaker plays into nowhere
}

static AudioServerPlugInDriverInterface gInterface = {
    NULL, queryInterface, addRef, release, initialize, createDevice, destroyDevice, deviceClient, deviceClient, configChange,
    configChange, hasProperty, isSettable, getSize, getData, setData, startIO, stopIO, zeroTimeStamp, willDo, beginEnd, doIO, beginEnd,
};
static AudioServerPlugInDriverInterface *gDriver = &gInterface;

// The factory named in Info.plist.
void *FajitaAudio_Create(CFAllocatorRef allocator, CFUUIDRef type) {
    (void)allocator;
    return CFEqual(type, TYPE_UUID) ? &gDriver : NULL;
}
