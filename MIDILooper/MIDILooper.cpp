/*
 * MIDILooper for Expert Sleepers disting NT.
 *
 * Stage 1 (confirmed stable on hardware): skeleton — full parameter/page set (Routing:
 * RecClock bus + 4 MIDI destination toggles; Looper: Record/RecLength/Live!), step()
 * reads RecClock and tracks step position, midiMessage() passes every message straight
 * through to whichever destinations are toggled on.
 *
 * Stage 2 (confirmed stable on hardware): adds the DRAM-backed event buffer and capture-only
 * recording. Record on/off is edge-detected inside step() (comparing against a DTC-cached
 * previous value, no parameterChanged()); while recording, midiMessage() also appends the
 * event to the buffer with its sample-accurate offset from the current RecClock step.
 *
 * Stage 3 (this version): proper mode state machine (Live/Recording/Overdub/Playback) and
 * playback scheduling — each RecClock step, recorded events belonging to that step are armed
 * into a small pending pool and fired at their sample-accurate offset. Recording auto-advances
 * to Overdub after one full lap (RecLength bars); turning Record off stops capture and moves
 * to Playback (still looping); turning Record on again from Playback resumes into Overdub.
 * No masking yet (so a live note and its recorded twin can currently double up in Playback —
 * that's the next stage), no Live! reset, no all-notes-off safety yet.
 *
 * Internal-feedback fix (this version): kNT_destinationInternal is a same-unit software bus
 * with no per-message source info — anything the looper sends via Internal comes right back
 * into its own midiMessage(), which would otherwise re-send it via Internal again, forever.
 * Fixed with a content/echo channel partition (kChannelRemaps): the 8 channels actually used
 * (1,2,3,4,5,6,10,13) each map to their own dedicated echo channel drawn from the remaining
 * 8 (7,8,9,11,12,14,15,16) — content and echo sets are fully disjoint across all 16 channels,
 * so there's never any ambiguity between new input and our own echo. Any channel not in the
 * table is withheld from Internal entirely (never sent there at all). Incoming messages on any
 * of the 8 echo channels are unconditionally dropped before any other processing. Breakout/
 * USB/Select Bus are unaffected — they always get every channel, unrestricted and unshifted.
 *
 * Overdub replace-in-place (this version): capturing a note/CC that already has an entry at
 * the same step (same channel + note/CC number) overwrites that entry instead of appending a
 * new one — Note On/Off share one identity per note number regardless of which half of the
 * lifecycle is being captured. This lets repeated overdub layers re-record the same step
 * indefinitely without growing the buffer; only genuinely new (never-before-recorded-at-this-
 * step) identities consume a fresh slot, so the fixed-size buffer only fills from real growth
 * in distinct recorded content, not from repeatedly touching the same notes/CCs.
 *
 * Explicit note termination (this version): CC 123 "All Notes Off" only silences notes on
 * receivers that specifically implement that Channel Mode Message — not guaranteed, and
 * apparently not honored by at least one real receiver in testing. Instead, every Note On/Off
 * actually sent (live pass-through or recorded playback firing) is tracked in a per-channel
 * bitset; dropping the loop (Live!) or stopping capture (Record off) now sends an explicit
 * Note Off for every note still tracked as active, in addition to (not instead of) the CC 123
 * broadcast, then clears the tracking.
 *
 * Dedicated Sync input (this version): a new "Sync" bus-select parameter on the Routing page
 * now times the *start* of a fresh recording — Record 0->1 from Live no longer starts capture
 * immediately, it arms (recordArmed) and waits for the next Sync rising edge, which resets
 * loopStep to 0 and begins capture right at that edge. RecClock's own role — the 1/16 bar/step
 * clock driving loopStep advancement, the Recording->Overdub auto-transition, and playback
 * scheduling — is completely unchanged and independent of Sync. Resuming into Overdub from
 * Playback (Record 0->1 while an existing loop is already playing) still starts immediately,
 * since that's continuing an already-aligned loop, not starting a fresh one.
 *
 * LED feedback (this version): kLedNoteRecord (83) sustained on ch13 while Recording or
 * Overdub; kLedNoteLoop (84) sustained on ch13 while Overdub or Playback — so Recording alone
 * shows only the record note, Overdub shows both, Playback alone shows only the loop note, and
 * Live shows neither. Sent only on actual mode changes (updateLedNotes(), checked once per
 * step() regardless of RecClock connectivity), hardcoded straight to the DIN output
 * (kNT_destinationBreakout) — this deliberately bypasses the configurable destination toggles.
 *
 * RecLength +/- control (this version): ch13 note89/90 Note On step RecLength by +1/-1 index
 * (clamped to the enum's range) via NT_setParameterFromAudio(), so the change shows on the
 * module's own display. Note Off is ignored. Intercepted and consumed in midiMessage() like
 * the LED notes' channel — never recorded or passed through.
 *
 * Record button protection (this version): ch13 note83 (kLedNoteRecord) is also intercepted
 * on input — both Note On and Note Off — purely to stop the button's own press/release from
 * leaking into the recording, since it's the same pad whose LED we drive as output. No
 * function is wired to it (yet); this only guards recording integrity.
 *
 * Quantizer / Quant2Clk (this version): new Quantizer page, one parameter. Off = exactly the
 * prior behavior. On: every note is processed (captured if Recording/Overdub, passed through
 * always) via the shared processNote() helper, decided by comparing samplesSinceEdge against
 * half of lastStepDuration (the previous RecClock inter-edge interval, the best available
 * estimate of the current one) — closer to the edge that already happened processes
 * immediately with offset forced to 0 (that edge is in the past, so "on it" just means "now");
 * closer to the upcoming edge holds the note in quantHold[] until step()'s RecClock handler
 * releases it, also at offset 0, exactly when that edge arrives. A live note is therefore never
 * heard except in sync with a clock edge, and captured timing always lands exactly on a step.
 * Switching Off flushes anything currently held immediately (real elapsed offset, not 0) rather
 * than stranding it waiting for an edge that will no longer release it.
 *
 * Enum RecLength + independent active/recorded length (this version): RecLength is now an enum
 * (1/4, 1/2, 1, 2, 4, 8, 16 bars -> kRecLengthSteps 4/8/16/32/64/128/256) instead of a raw bar
 * count, displayed by name via kNT_unitEnum. The loop now tracks two lengths separately:
 * activeLenSteps (what loopStep wraps against and what gates playback arming right now) and
 * recordedLenSteps (how much has actually been captured, locked in the moment Recording
 * completes one full lap and auto-transitions to Overdub). During Live/Recording, active
 * directly tracks the RecLength request. Once something is recorded, shrinking the request
 * takes effect immediately — active drops right away, discarding from the end by simply no
 * longer reaching those step positions — but growing only takes effect immediately up to
 * recordedLenSteps; a request beyond that just stays pending in v[] for the next fresh take.
 * Critically, shrinking never deletes anything from loopBuffer — events beyond the shrunk
 * active length just stop matching loopStep in the arming scan, so growing back up (without a
 * fresh recording clearing the buffer first) makes them audible again automatically.
 *
 * Style and architecture modelled directly on two official examples:
 * Disting NT Examples/distingNT_API/examples/nt_midi_router.cpp (parameter/page helper
 * style, direct v[] reads with no parameterChanged(), 2-vs-3-byte MIDI send distinction)
 * and .../examples/midilooper.cpp (DTC memory for frequently-touched state, a minimal
 * construct() that never calls parameterChanged() or NT_setParameterFromAudio(), and a
 * plain flat DRAM cast with no manual pointer-arithmetic carving into sub-regions).
 */

#include <distingnt/api.h>
#include <cstring>
#include <new>

namespace {

constexpr _NT_parameter makeParameter(const char* name, int16_t min, int16_t max, int16_t def,
                                       uint8_t unit, char const * const * enumStrings = nullptr) {
    return { name, min, max, def, unit, 0, enumStrings };
}

constexpr _NT_parameterPage makePage(const char* name, uint8_t numParams, const uint8_t* params) {
    return { name, numParams, 0, { 0, 0 }, params };
}

static char const * const kOffOnStrings[] = { "Off", "On" };

// RecLength is bar-fraction/multiple names, not a raw integer — kRecLengthSteps[i] is the
// step count (16 steps/bar) for the same index i in kRecLengthStrings. Index-aligned; keep
// both arrays and kNumRecLengthValues in sync if either changes.
static char const * const kRecLengthStrings[] = { "1/4", "1/2", "1", "2", "4", "8", "16" };
constexpr uint16_t kRecLengthSteps[] =         {   4,     8,    16,  32,  64, 128, 256  };
constexpr int kNumRecLengthValues = 7;
static_assert(ARRAY_SIZE(kRecLengthStrings) == kNumRecLengthValues, "RecLength string table mismatch");
static_assert(ARRAY_SIZE(kRecLengthSteps) == kNumRecLengthValues, "RecLength step table mismatch");

enum {
    // Routing page
    kParamRecClock,
    kParamSync,
    kParamDestBreakout,
    kParamDestUSB,
    kParamDestSelectBus,
    kParamDestInternal,

    // Looper page
    kParamRecord,
    kParamRecLength,
    kParamLive,

    // Quantizer page
    kParamQuant2Clk,

    kNumParams,
};

static const _NT_parameter kParameters[] = {
    NT_PARAMETER_CV_INPUT( "RecClock", 0, 0 )                                  // kParamRecClock
    NT_PARAMETER_CV_INPUT( "Sync", 0, 0 )                                       // kParamSync
    makeParameter("Breakout",   0, 1, 1, kNT_unitEnum, kOffOnStrings),          // kParamDestBreakout
    makeParameter("USB",        0, 1, 0, kNT_unitEnum, kOffOnStrings),          // kParamDestUSB
    makeParameter("Select Bus", 0, 1, 0, kNT_unitEnum, kOffOnStrings),          // kParamDestSelectBus
    makeParameter("Internal",   0, 1, 0, kNT_unitEnum, kOffOnStrings),          // kParamDestInternal
    makeParameter("Record",     0, 1, 0, kNT_unitEnum, kOffOnStrings),          // kParamRecord
    makeParameter("RecLength",  0, 6, 4, kNT_unitEnum, kRecLengthStrings),      // kParamRecLength
    makeParameter("Live!",      0, 1, 0, kNT_unitEnum, kOffOnStrings),          // kParamLive
    makeParameter("Quant2Clk",  0, 1, 0, kNT_unitEnum, kOffOnStrings),          // kParamQuant2Clk
};

static_assert(ARRAY_SIZE(kParameters) == kNumParams, "parameter table mismatch");

static const uint8_t kRoutingPage[] = {
    kParamRecClock, kParamSync, kParamDestBreakout, kParamDestUSB, kParamDestSelectBus, kParamDestInternal
};

static const uint8_t kLooperPage[] = {
    kParamRecord, kParamRecLength, kParamLive
};

static const uint8_t kQuantizerPage[] = {
    kParamQuant2Clk
};

static const _NT_parameterPage kPages[] = {
    makePage("Routing",    static_cast<uint8_t>(ARRAY_SIZE(kRoutingPage)),   kRoutingPage),
    makePage("Looper",     static_cast<uint8_t>(ARRAY_SIZE(kLooperPage)),    kLooperPage),
    makePage("Quantizer",  static_cast<uint8_t>(ARRAY_SIZE(kQuantizerPage)), kQuantizerPage),
};

static const _NT_parameterPages kParameterPages = {
    ARRAY_SIZE(kPages),
    kPages,
};

// One recorded MIDI event. uint32_t first, then four uint8_t: sizeof == 8, no padding.
struct LoopEvent {
    uint32_t offset;   // samples elapsed since this step's RecClock rising edge, at capture time
    uint8_t  step;     // loop step index this event belongs to (0 .. activeLenSteps-1, max 255)
    uint8_t  status;   // raw MIDI status byte, channel already OR'd in
    uint8_t  data1;
    uint8_t  data2;
};

constexpr int kLoopBufferSize  = 2048;  // max recorded MIDI events
constexpr int kLoopPendingPool = 16;    // concurrent scheduled-but-not-yet-fired playback events
constexpr int kQuantHoldPool   = 16;    // concurrent notes held awaiting the next RecClock edge

enum { kModeLive = 0, kModeRecording = 1, kModeOverdub = 2, kModePlayback = 3 };

// LED feedback notes, hardcoded to the DIN output (kNT_destinationBreakout) regardless of the
// configurable destination toggles — see updateLedNotes(). Assumed typo-corrected from "note
// 33" to "note 83" for the Recording case, to stay consistent with the Overdub case explicitly
// naming both 83 and 84.
constexpr uint8_t kLedChannel    = 12;   // MIDI ch13
constexpr uint8_t kLedNoteRecord = 83;   // sustained while Recording or Overdub
constexpr uint8_t kLedNoteLoop   = 84;   // sustained while Overdub or Playback

// RecLength +/- control notes, same channel as the LED feedback (ch13). Note On only —
// intercepted and consumed in midiMessage(), never captured or passed through. Each Note On
// steps RecLength by 1, clamped to its declared [1,8] range, via NT_setParameterFromAudio() so
// the change is visible on the module's own display, not just internal state.
constexpr uint8_t kRecLengthIncNote = 89;
constexpr uint8_t kRecLengthDecNote = 90;

// One armed, not-yet-fired scheduled playback event. sizeof == 8, no padding.
struct LoopPendingEvent {
    uint32_t countdown;  // samples remaining until fire (0 = fire this frame)
    uint8_t  status;
    uint8_t  data1;
    uint8_t  data2;
    bool     active;     // true = slot in use
};

// A live note held (when Quant2Clk is On) awaiting the next RecClock rising edge, at which
// point it's processed (captured if recording, and passed through) — see processNote() and
// the Quant2Clk header comment. Released by edge, not by sample countdown, so no countdown
// field is needed here (unlike LoopPendingEvent).
struct QuantHoldNote {
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    bool    active;
};

// Frequently-touched state, in DTC (fastest memory) rather than the SRAM instance struct —
// following midilooper.cpp's pattern rather than embedding this directly in the algorithm.
struct MIDILooper_DTC {
    uint8_t  loopStep;              // 0 .. (activeLenSteps - 1)
    float    lastRecClock;          // previous RecClock sample, for rising-edge detection
    uint32_t samplesSinceEdge;      // reset to 0 on each RecClock rising edge, ++ every sample

    // The currently active loop length (what loopStep wraps against and what gates playback
    // arming), versus what has actually been recorded — see the header comment for the
    // shrink-immediately / grow-only-up-to-what's-recorded rule these two implement together.
    uint16_t activeLenSteps;        // currently active loop length, in steps
    uint16_t recordedLenSteps;      // steps actually recorded so far; 0 = not yet established
                                     // (still on the first Recording pass, or nothing recorded)

    float    lastSync;              // previous Sync sample, for rising-edge detection
    bool     recordArmed;           // Record went 0->1 from Live; waiting for next Sync edge

    uint8_t  mode;                  // kModeLive / kModeRecording / kModeOverdub / kModePlayback
    uint8_t  lastLedMode;            // mode as of the last LED update — see updateLedNotes()
    int16_t  lastRecordValue;       // previous Record param value, for edge detection in step()
    int16_t  lastLiveValue;         // previous Live! param value, for edge detection in step()
    int16_t  lastQuant2ClkValue;    // previous Quant2Clk param value, for edge detection in step()
    uint16_t loopEventCount;        // 0..kLoopBufferSize; also the append-write cursor

    LoopPendingEvent pending[kLoopPendingPool];

    // Quant2Clk state. lastStepDuration is the previous RecClock inter-edge interval's measured
    // length in samples (updated every rising edge, right before samplesSinceEdge resets) — the
    // estimate used to decide whether an incoming note is closer to the edge that just happened
    // (process now) or the upcoming one (hold). quantHold[] are notes currently held for the
    // latter case. See processNote() and the header comment for the full mechanism.
    uint32_t lastStepDuration;
    QuantHoldNote quantHold[kQuantHoldPool];

    // Tracks which (channel, note) pairs currently have an unterminated Note On outstanding —
    // from live pass-through or recorded playback — so a precise Note Off can be sent for each
    // one on demand. [channel][note>>3], bit (note&7). Not relying on CC 123 "All Notes Off"
    // alone, since not every receiver implements that Channel Mode Message.
    uint8_t activeNotes[16][16];
};

struct MIDILooperAlgorithm : public _NT_algorithm {
    MIDILooperAlgorithm(MIDILooper_DTC* dtc_, LoopEvent* loopBuffer_)
        : dtc(dtc_), loopBuffer(loopBuffer_) {
        parameters       = nullptr;
        parameterPages   = nullptr;
        vIncludingCommon = nullptr;
        v                = nullptr;
    }

    MIDILooper_DTC* dtc;
    LoopEvent*      loopBuffer;    // DRAM: recorded event ring, kLoopBufferSize entries
};

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t*) {
    req.numParameters = ARRAY_SIZE(kParameters);
    req.sram = sizeof(MIDILooperAlgorithm);
    req.dram = kLoopBufferSize * sizeof(LoopEvent);
    req.dtc  = sizeof(MIDILooper_DTC);
    req.itc  = 0;
}

// Content/echo channel partition for Internal. Every channel actually used (content) maps to
// its own dedicated echo channel that nothing else ever uses — the two sets are fully disjoint
// across all 16 MIDI channels, so a content channel can never collide with an echo channel and
// there's no ambiguity between "genuinely new input" and "our own Internal echo coming back".
// Values are 0-indexed (MIDI channel N = index N-1). Any channel not listed here is never sent
// to Internal at all (withheld, same as before) and is never filtered on input either.
struct ChannelRemap { uint8_t content; uint8_t echo; };
constexpr ChannelRemap kChannelRemaps[] = {
    { 0,  6  },   // ch1  -> ch7
    { 1,  7  },   // ch2  -> ch8
    { 2,  8  },   // ch3  -> ch9
    { 3,  11 },   // ch4  -> ch12
    { 4,  13 },   // ch5  -> ch14
    { 5,  14 },   // ch6  -> ch15
    { 9,  10 },   // ch10 -> ch11 (drum trigger; unchanged from before)
    { 12, 15 },   // ch13 -> ch16
};
constexpr int kNumChannelRemaps = 8;

// Returns the echo channel for a content channel, or -1 if it's not one of the channels ever
// routed to Internal.
int echoChannelFor(uint8_t contentChannel) {
    for (int i = 0; i < kNumChannelRemaps; ++i)
        if (kChannelRemaps[i].content == contentChannel)
            return kChannelRemaps[i].echo;
    return -1;
}

// Returns true if channel is one of the 8 reserved echo channels — these are unconditionally
// dropped on input since nothing but our own Internal sends should ever use them.
bool isEchoChannel(uint8_t channel) {
    for (int i = 0; i < kNumChannelRemaps; ++i)
        if (kChannelRemaps[i].echo == channel)
            return true;
    return false;
}

uint32_t externalDestinations(const int16_t* v) {
    uint32_t dest = 0;
    if (v[kParamDestBreakout])  dest |= kNT_destinationBreakout;
    if (v[kParamDestUSB])       dest |= kNT_destinationUSB;
    if (v[kParamDestSelectBus]) dest |= kNT_destinationSelectBus;
    return dest;
}

void sendMidi(uint32_t dest, uint8_t status, uint8_t data1, uint8_t data2) {
    if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
        NT_sendMidi2ByteMessage(dest, status, data1);
    else
        NT_sendMidi3ByteMessage(dest, status, data1, data2);
}

// Sends to Breakout/USB/Select Bus (whichever are enabled) with the original channel,
// unrestricted. Internal only ever receives channels listed in kChannelRemaps, each remapped
// to its own dedicated echo channel — every other channel is withheld from Internal entirely,
// because anything reaching Internal echoes straight back into midiMessage() with no way to
// tell it apart from new input, so any channel sent there without a reserved echo channel of
// its own would loop forever the same way channel 10 did before it was remapped.
void sendToDestinations(const int16_t* v, uint8_t status, uint8_t data1, uint8_t data2) {
    uint32_t extDest = externalDestinations(v);
    if (extDest)
        sendMidi(extDest, status, data1, data2);

    if (v[kParamDestInternal]) {
        int echo = echoChannelFor(status & 0x0F);
        if (echo >= 0)
            sendMidi(kNT_destinationInternal, (status & 0xF0) | (uint8_t)echo, data1, data2);
    }
}

// Standard MIDI "All Notes Off" Channel Mode Message (CC 123, value 0) on every channel, to
// whichever destinations are enabled. Sent alongside the explicit termination below in case a
// receiver does honor it, but not relied on alone — see terminateActiveNotes().
void allNotesOff(const int16_t* v) {
    for (int ch = 0; ch < 16; ++ch)
        sendToDestinations(v, (uint8_t)(0xB0 | ch), 123, 0);
}

// Updates the active-note bitset from a Note On/Off message that was just sent out (live
// pass-through or recorded playback firing) — Note On with velocity > 0 sets the bit, Note Off
// (or Note On with velocity 0, the standard MIDI running-status idiom for note-off) clears it.
void trackNoteMessage(MIDILooper_DTC* dtc, uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t hiNibble = status & 0xF0;
    uint8_t channel  = status & 0x0F;
    uint8_t mask     = (uint8_t)(1 << (data1 & 7));
    if (hiNibble == 0x90 && data2 > 0)
        dtc->activeNotes[channel][data1 >> 3] |= mask;
    else if (hiNibble == 0x80 || hiNibble == 0x90)
        dtc->activeNotes[channel][data1 >> 3] &= (uint8_t)~mask;
}

// Sends an explicit Note Off for every (channel, note) pair currently tracked as active, then
// clears the bitset. Works regardless of whether the receiver implements CC 123.
void terminateActiveNotes(const int16_t* v, MIDILooper_DTC* dtc) {
    for (int ch = 0; ch < 16; ++ch) {
        for (int i = 0; i < 16; ++i) {
            uint8_t bits = dtc->activeNotes[ch][i];
            if (!bits)
                continue;
            for (int bit = 0; bit < 8; ++bit) {
                if (bits & (1 << bit)) {
                    uint8_t note = (uint8_t)(i * 8 + bit);
                    sendToDestinations(v, (uint8_t)(0x80 | ch), note, 0);
                }
            }
        }
    }
    memset(dtc->activeNotes, 0, sizeof(dtc->activeNotes));
}

// Sends the delta of desired LED states (kLedNoteRecord/kLedNoteLoop, both sustained Note On/
// Off on kLedChannel) whenever mode has changed since the last call. Hardcoded to the DIN
// output (kNT_destinationBreakout) directly — deliberately bypasses sendToDestinations() and
// the configurable destination toggles entirely, always sent regardless of their state.
// Cheap to call every step(); it only actually sends anything when mode has actually changed.
void updateLedNotes(MIDILooper_DTC* dtc) {
    if (dtc->mode == dtc->lastLedMode)
        return;

    bool oldRecordLed = (dtc->lastLedMode == kModeRecording || dtc->lastLedMode == kModeOverdub);
    bool newRecordLed = (dtc->mode        == kModeRecording || dtc->mode        == kModeOverdub);
    bool oldLoopLed   = (dtc->lastLedMode == kModeOverdub   || dtc->lastLedMode == kModePlayback);
    bool newLoopLed   = (dtc->mode        == kModeOverdub   || dtc->mode        == kModePlayback);

    if (newRecordLed != oldRecordLed)
        sendMidi(kNT_destinationBreakout, (uint8_t)((newRecordLed ? 0x90 : 0x80) | kLedChannel), kLedNoteRecord, newRecordLed ? 127 : 0);
    if (newLoopLed != oldLoopLed)
        sendMidi(kNT_destinationBreakout, (uint8_t)((newLoopLed ? 0x90 : 0x80) | kLedChannel), kLedNoteLoop, newLoopLed ? 127 : 0);

    dtc->lastLedMode = dtc->mode;
}

// Note On and Note Off are treated as the same identity category (a note, regardless of which
// half of its lifecycle this particular message is) so a same-step Off can replace an On and
// vice versa; every other message type keeps its own distinct category.
uint8_t messageCategory(uint8_t status) {
    uint8_t hiNibble = status & 0xF0;
    return hiNibble == 0x80 ? 0x90 : hiNibble;
}

// Scans loopBuffer[0..count) for an existing event at the same step with the same identity
// (channel + message category + data1 — e.g. same note number, or same CC number) as the new
// one being captured. Returns its index, or -1 if this is genuinely new content at this step.
int findExistingEvent(const LoopEvent* buffer, uint16_t count, uint8_t step, uint8_t status, uint8_t data1) {
    uint8_t channel  = status & 0x0F;
    uint8_t category = messageCategory(status);
    for (uint16_t i = 0; i < count; ++i) {
        if (buffer[i].step == step &&
            (buffer[i].status & 0x0F) == channel &&
            messageCategory(buffer[i].status) == category &&
            buffer[i].data1 == data1)
            return (int)i;
    }
    return -1;
}

// Capture (if Recording/Overdub) and pass-through for one note/CC — the actual processing
// behind both the immediate path and the quantized-hold path in midiMessage(). Identical to
// what midiMessage() always did before Quant2Clk existed; factored out so both paths (process
// now vs. process at the next RecClock edge) share one implementation. offset is an explicit
// parameter (rather than always reading dtc->samplesSinceEdge internally) so quantized callers
// can force it to 0 — landing exactly on a step boundary — while the normal, unquantized path
// still passes the real elapsed time.
void processNote(MIDILooperAlgorithm* alg, const int16_t* v, uint8_t status, uint8_t data1, uint8_t data2, uint32_t offset) {
    MIDILooper_DTC* dtc = alg->dtc;

    // Capture during Recording/Overdub, independent of pass-through destinations. If an event
    // with the same identity (channel + note/CC number) already exists at this step, overwrite
    // it in place — this is what lets Overdub re-record the same note/CC repeatedly across many
    // laps without the buffer growing every time, and is why the cap is only ever hit by
    // genuinely new content, not by repeated content. Only genuinely new identities consume a
    // fresh slot, and only then subject to the buffer-full cap (transport keeps running either
    // way, only capture of brand-new content stops).
    if (dtc->mode == kModeRecording || dtc->mode == kModeOverdub) {
        int existing = findExistingEvent(alg->loopBuffer, dtc->loopEventCount, dtc->loopStep, status, data1);
        if (existing >= 0) {
            LoopEvent& ev = alg->loopBuffer[existing];
            ev.offset = offset;
            ev.status = status;
            ev.data1  = data1;
            ev.data2  = data2;
        } else if (dtc->loopEventCount < kLoopBufferSize) {
            LoopEvent& ev = alg->loopBuffer[dtc->loopEventCount];
            ev.offset = offset;
            ev.step   = dtc->loopStep;
            ev.status = status;
            ev.data1  = data1;
            ev.data2  = data2;
            ++dtc->loopEventCount;
        }
    }

    // Pass-through: unconditional in Live/Recording/Overdub. No masking yet in Playback
    // (next stage) — for now Playback also passes everything straight through.
    sendToDestinations(v, status, data1, data2);
    trackNoteMessage(dtc, status, data1, data2);
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs, const _NT_algorithmRequirements&,
                          const int32_t*) {
    MIDILooper_DTC* dtc = (MIDILooper_DTC*)ptrs.dtc;
    memset(dtc, 0, sizeof(MIDILooper_DTC));

    LoopEvent* loopBuffer = (LoopEvent*)ptrs.dram;

    MIDILooperAlgorithm* alg = new (ptrs.sram) MIDILooperAlgorithm(dtc, loopBuffer);
    alg->parameters     = kParameters;
    alg->parameterPages = &kParameterPages;
    return alg;
}

void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    MIDILooperAlgorithm* alg = (MIDILooperAlgorithm*)self;
    MIDILooper_DTC* dtc = alg->dtc;
    const int16_t* v = alg->v;
    int numFrames = numFramesBy4 * 4;

    // Record on/off edge detection — checked every step() call against a DTC-cached
    // previous value, matching midilooper.cpp's pattern (no parameterChanged()).
    int16_t recordValue = v[kParamRecord];
    if (recordValue != dtc->lastRecordValue) {
        if (recordValue) {
            if (dtc->mode == kModePlayback) {
                dtc->mode = kModeOverdub;   // resume immediately, keep the existing loop as-is
            } else {
                dtc->recordArmed = true;    // fresh take: wait for the next Sync edge to start
            }
        } else {
            dtc->recordArmed = false;       // cancel if Sync never fired
            if (dtc->mode == kModeRecording || dtc->mode == kModeOverdub) {
                dtc->mode = kModePlayback;  // stop capturing, keep looping
                allNotesOff(v);
                terminateActiveNotes(v, dtc);
            }
        }
        dtc->lastRecordValue = recordValue;
    }

    // Sync rising-edge detection — only ever consumed while a fresh take is armed (see above).
    // This is what actually starts the recording, aligning its step 0 to this edge. RecClock's
    // own role (bar/step timing, playback scheduling) is unchanged and independent of this.
    int syncBus = v[kParamSync];
    const float* syncIn = (syncBus >= 1 && syncBus <= kNT_lastBus)
                         ? busFrames + (syncBus - 1) * numFrames : nullptr;
    if (syncIn) {
        for (int i = 0; i < numFrames; ++i) {
            float sy = syncIn[i];
            if (dtc->lastSync < 1.0f && sy >= 1.0f && dtc->recordArmed) {
                dtc->loopStep       = 0;
                dtc->loopEventCount = 0;
                dtc->mode           = kModeRecording;
                dtc->recordArmed    = false;
                dtc->samplesSinceEdge = 0;
                dtc->recordedLenSteps = 0;   // fresh take: not yet established
            }
            dtc->lastSync = sy;
        }
    }

    // Live! (0->1 trigger) edge detection — clears the recording, returns to Live/pass-through.
    int16_t liveValue = v[kParamLive];
    if (liveValue && !dtc->lastLiveValue) {
        dtc->loopEventCount = 0;
        dtc->mode = kModeLive;
        dtc->recordedLenSteps = 0;
        allNotesOff(v);
        terminateActiveNotes(v, dtc);
        // Force Record back to 0 so the UI reflects reality, not just internal state. Set our
        // own state first (above) — this may take effect as late as the next step() call, and
        // the mode guard on the Record "turn off" branch above already handles that safely.
        NT_setParameterFromAudio(NT_algorithmIndex(self), kParamRecord + NT_parameterOffset(), 0);
    }
    dtc->lastLiveValue = liveValue;

    // Quant2Clk On->Off: flush anything currently held rather than stranding it waiting for an
    // edge that will no longer release it (Off skips the drain-on-edge logic below entirely).
    // Uses the real elapsed offset, matching normal Off-state behavior, not offset 0.
    int16_t quant2ClkValue = v[kParamQuant2Clk];
    if (!quant2ClkValue && dtc->lastQuant2ClkValue) {
        for (int s = 0; s < kQuantHoldPool; ++s) {
            if (!dtc->quantHold[s].active)
                continue;
            processNote(alg, v, dtc->quantHold[s].status, dtc->quantHold[s].data1,
                        dtc->quantHold[s].data2, dtc->samplesSinceEdge);
            dtc->quantHold[s].active = false;
        }
    }
    dtc->lastQuant2ClkValue = quant2ClkValue;

    // LED feedback — must run regardless of RecClock connectivity (checked before the
    // RecClock-gated early return below).
    updateLedNotes(dtc);

    int recClockBus = v[kParamRecClock];
    const float* recClockIn = (recClockBus >= 1 && recClockBus <= kNT_lastBus)
                             ? busFrames + (recClockBus - 1) * numFrames : nullptr;

    if (!recClockIn)
        return;

    // Requested vs. active loop length. During Live/Recording (nothing established yet, or
    // still defining the first pass) the active length directly tracks the request. Once a
    // length is recorded (Overdub/Playback), shrinking takes effect immediately (clamped to
    // the request); growing only takes effect immediately if it doesn't exceed what's actually
    // recorded — a longer request just stays pending, read fresh from v[] by the next take.
    // Recorded content beyond a shrunk active length is never deleted, only excluded from the
    // step-match below, so growing back within the recorded length restores it automatically.
    {
        int enumIndex = v[kParamRecLength];
        if (enumIndex < 0) enumIndex = 0;
        if (enumIndex >= kNumRecLengthValues) enumIndex = kNumRecLengthValues - 1;
        uint16_t requestedLen = kRecLengthSteps[enumIndex];

        if (dtc->mode == kModeLive || dtc->mode == kModeRecording) {
            dtc->activeLenSteps = requestedLen;
        } else if (requestedLen <= dtc->recordedLenSteps) {
            dtc->activeLenSteps = requestedLen;
        }
        // else: requested is longer than what's recorded — leave activeLenSteps unchanged.

        if (dtc->activeLenSteps == 0)
            dtc->activeLenSteps = kRecLengthSteps[0];   // safety, shouldn't happen
        if (dtc->loopStep >= dtc->activeLenSteps)
            dtc->loopStep = (uint8_t)(dtc->loopStep % dtc->activeLenSteps);   // clamp after a shrink
    }

    for (int i = 0; i < numFrames; ++i) {
        float rc = recClockIn[i];

        if (dtc->lastRecClock < 1.0f && rc >= 1.0f) {
            dtc->loopStep = (uint8_t)((dtc->loopStep + 1) % dtc->activeLenSteps);

            if (dtc->loopStep == 0 && dtc->mode == kModeRecording) {
                dtc->mode = kModeOverdub;   // auto transition after one full lap
                dtc->recordedLenSteps = dtc->activeLenSteps;   // lock in what was just recorded
            }

            // Arm pending playback for events recorded at this step. Must run before the
            // Quant2Clk drain below — a note released from quantHold this same edge gets
            // captured with .step == this loopStep, and if the arming scan ran after that, it
            // would immediately re-arm (and double-fire) the note it just live-passed-through.
            // Scanning first means a freshly quantized capture is only picked up on a later lap,
            // exactly like a normal (non-quantized) capture already behaves.
            if (dtc->mode == kModeOverdub || dtc->mode == kModePlayback) {
                for (uint16_t e = 0; e < dtc->loopEventCount; ++e) {
                    if (alg->loopBuffer[e].step != dtc->loopStep)
                        continue;
                    for (int s = 0; s < kLoopPendingPool; ++s) {
                        if (dtc->pending[s].active)
                            continue;
                        dtc->pending[s].active    = true;
                        dtc->pending[s].countdown = alg->loopBuffer[e].offset;
                        dtc->pending[s].status    = alg->loopBuffer[e].status;
                        dtc->pending[s].data1     = alg->loopBuffer[e].data1;
                        dtc->pending[s].data2     = alg->loopBuffer[e].data2;
                        break;
                    }
                }
            }

            // Quant2Clk: release every note held awaiting this edge, right on it, offset 0.
            // loopStep has already advanced above, so held notes land on the step that just
            // started — the "closer to the upcoming edge" case from midiMessage().
            if (v[kParamQuant2Clk]) {
                for (int s = 0; s < kQuantHoldPool; ++s) {
                    if (!dtc->quantHold[s].active)
                        continue;
                    processNote(alg, v, dtc->quantHold[s].status, dtc->quantHold[s].data1,
                                dtc->quantHold[s].data2, 0);
                    dtc->quantHold[s].active = false;
                }
            }

            // Measure the just-completed interval for Quant2Clk's nearest-edge estimate —
            // captured before the reset, so it reflects the interval that just ended.
            dtc->lastStepDuration = dtc->samplesSinceEdge;
            dtc->samplesSinceEdge = 0;
        }

        dtc->lastRecClock = rc;
        ++dtc->samplesSinceEdge;

        // Fire any pending scheduled playback events whose countdown has elapsed.
        for (int s = 0; s < kLoopPendingPool; ++s) {
            if (!dtc->pending[s].active)
                continue;
            if (dtc->pending[s].countdown == 0) {
                sendToDestinations(v, dtc->pending[s].status, dtc->pending[s].data1, dtc->pending[s].data2);
                trackNoteMessage(dtc, dtc->pending[s].status, dtc->pending[s].data1, dtc->pending[s].data2);
                dtc->pending[s].active = false;
            } else {
                --dtc->pending[s].countdown;
            }
        }
    }
}

void midiMessage(_NT_algorithm* self, uint8_t byte0, uint8_t byte1, uint8_t byte2) {
    // Drop our own Internal-destination echoes before any other processing — see header
    // comment. Without this, enabling Internal creates an infinite send/receive loop.
    if (isEchoChannel(byte0 & 0x0F))
        return;

    MIDILooperAlgorithm* alg = (MIDILooperAlgorithm*)self;
    MIDILooper_DTC* dtc = alg->dtc;
    const int16_t* v = self->v;

    uint8_t hiNibble = byte0 & 0xF0;
    bool isNoteMsg   = (hiNibble == 0x80 || hiNibble == 0x90);
    bool isNoteOn    = (hiNibble == 0x90 && byte2 > 0);

    // Record button (kLedNoteRecord's input side) — intercepted and consumed here, both Note
    // On and Note Off, so its own press/release never contaminates the recording or gets
    // passed through. This is the same physical pad whose LED we drive from updateLedNotes();
    // pressing it apparently also sends this note back to us. No function is wired to it yet —
    // this only protects the recording's integrity.
    if ((byte0 & 0x0F) == kLedChannel && isNoteMsg && byte1 == kLedNoteRecord)
        return;

    // RecLength +/- control notes — intercepted and consumed here, never captured or passed
    // through. Note On only (byte2 > 0); Note Off is ignored, not just a no-op fallthrough.
    if ((byte0 & 0x0F) == kLedChannel && isNoteOn) {
        if (byte1 == kRecLengthIncNote || byte1 == kRecLengthDecNote) {
            int16_t newIndex = v[kParamRecLength] + (byte1 == kRecLengthIncNote ? 1 : -1);
            if (newIndex < 0) newIndex = 0;
            if (newIndex > kNumRecLengthValues - 1) newIndex = kNumRecLengthValues - 1;
            NT_setParameterFromAudio(NT_algorithmIndex(self), kParamRecLength + NT_parameterOffset(), newIndex);
            return;
        }
    }

    if (!v[kParamQuant2Clk]) {
        // Off: exactly the original, unquantized behavior — process now, real elapsed offset.
        processNote(alg, v, byte0, byte1, byte2, dtc->samplesSinceEdge);
        return;
    }

    // On: nearest-edge decision. Closer to the edge that already happened (less than half of
    // the last measured inter-edge interval has elapsed) -> process now, quantized to offset 0
    // (that edge is in the past, so "processing on it" just means processing immediately).
    // Closer to the upcoming edge -> hold; step()'s RecClock edge handler releases it, also
    // with offset 0, exactly when that edge arrives. See header comment for the full rationale.
    // lastStepDuration == 0 means no edge has been measured yet (e.g. RecClock not connected,
    // or just started) — treat as immediate rather than holding into a queue nothing may ever
    // drain.
    if (dtc->lastStepDuration == 0 || dtc->samplesSinceEdge < dtc->lastStepDuration / 2) {
        processNote(alg, v, byte0, byte1, byte2, 0);
        return;
    }

    for (int s = 0; s < kQuantHoldPool; ++s) {
        if (dtc->quantHold[s].active)
            continue;
        dtc->quantHold[s].active  = true;
        dtc->quantHold[s].status = byte0;
        dtc->quantHold[s].data1  = byte1;
        dtc->quantHold[s].data2  = byte2;
        break;
        // Pool exhausted: silently dropped, same self-healing-next-lap spirit as the existing
        // playback pending pool — not expected to matter at 16 concurrent held notes.
    }
}

static const _NT_factory kFactory = {
    NT_MULTICHAR('M','I','D','L'),
    "MIDILooper",
    "MIDI clock-synced looper",
    0,
    nullptr,
    nullptr,                   // calculateStaticRequirements
    nullptr,                   // initialise
    calculateRequirements,
    construct,
    nullptr,                   // parameterChanged — direct v[] reads instead, see nt_midi_router.cpp
    step,
    nullptr,                   // draw
    nullptr,                   // midiRealtime
    midiMessage,
    kNT_tagUtility,
    nullptr,                   // hasCustomUi
    nullptr,                   // customUi
    nullptr,                   // setupUi
    nullptr,                   // serialise
    nullptr,                   // deserialise
    nullptr,                   // midiSysEx
    nullptr,                   // parameterUiPrefix
    nullptr,                   // parameterString
};

} // namespace

extern "C" uintptr_t pluginEntry(_NT_selector selector, uint32_t data) {
    switch (selector) {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo:  return data == 0 ? reinterpret_cast<uintptr_t>(&kFactory) : 0;
    }
    return 0;
}
