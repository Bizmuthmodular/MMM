/*
 * LED Helper for Expert Sleepers disting NT.
 *
 * An invertible MIDI note latch, built to drive the LED feedback on a fixed-value MIDI
 * controller (e.g. the Vestax VCM-600) whose buttons only ever send a raw Note On when
 * pressed — they have no built-in toggle/mute-state memory, and their LEDs are lit/unlit by
 * the controller receiving Note On messages back on the same channel/note (velocity > 0 =
 * lit, velocity 0 = unlit). Fixed-value pad/button controllers like this typically only
 * parse Note On for their LED state and never listen for a literal Note Off (0x80) status
 * byte, so this plugin never sends one — see the "released" case in midiMessage() below.
 *
 * 20 independent slots (LED1..LED20 pages), each watching one CH/Note pair. The first Note
 * On received on a slot's configured channel+note toggles that slot from released to held;
 * the next matching Note On toggles it back to released, and so on. On every toggle, this
 * plugin echoes the new state straight back out as a Note On on that same channel+note —
 * velocity 127 for held, velocity 0 for released — which is what actually lights/unlights
 * the physical LED. Flip inverts which physical state (lit/unlit) corresponds to held —
 * useful when a button's LED should show the opposite of the raw toggle (e.g. lit = unmuted).
 *
 * CH defaults to None (index 0 of its enum) and Note defaults to None (-1, shown as a plain
 * raw number 0..127 for real notes rather than named pitches) — a slot with either set to
 * None is inactive and never matches incoming MIDI. Only real Note On messages (velocity > 0)
 * count as presses; a Note On with velocity 0 (the standard MIDI running-status idiom for
 * note-off) is ignored, matching the note-on/note-on toggle behaviour described above rather
 * than being treated as a second, spurious trigger.
 *
 * Routing page controls where the LED feedback is sent: Breakout (DIN), USB, Select Bus —
 * any combination, all default Off. Internal is deliberately not offered: it is a same-unit
 * software bus with no per-message source info, so anything this plugin sent there would
 * come straight back into its own midiMessage() and re-trigger the same slot's toggle,
 * exactly the feedback problem documented in this project's MIDILooper plugin.
 *
 * No audio/CV busses at all — this plugin is pure MIDI, so step() is unused (left NULL).
 * Style modelled on this project's MIDILooper.cpp and the official nt_midi_router.cpp
 * example: direct v[] reads in midiMessage(), no parameterChanged().
 */

#include <distingnt/api.h>
#include <cstdint>
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
static char const * const kFlipStrings[]  = { "No", "Yes" };

// Index 0 = "None" (slot inactive); index n (1..16) = MIDI channel n (1-based display,
// 0-based on the wire).
static char const * const kChannelNames[] = {
    "None", "1", "2", "3", "4", "5", "6", "7",
    "8", "9", "10", "11", "12", "13", "14", "15",
    "16",
};

static_assert(ARRAY_SIZE(kChannelNames) == 17, "channel name table mismatch");

constexpr int kNumLEDs      = 20;
constexpr uint8_t kLedVelocity = 127;  // velocity used for the "held" (LED-on) Note On

enum {
    // Routing page — LED feedback destination(s)
    kParamDestBreakout,
    kParamDestUSB,
    kParamDestSelectBus,

    // LED1..LED20 pages — CH/Note/Flip triplets
    kParamCH1, kParamNote1, kParamFlip1,
    kParamCH2, kParamNote2, kParamFlip2,
    kParamCH3, kParamNote3, kParamFlip3,
    kParamCH4, kParamNote4, kParamFlip4,
    kParamCH5, kParamNote5, kParamFlip5,
    kParamCH6, kParamNote6, kParamFlip6,
    kParamCH7, kParamNote7, kParamFlip7,
    kParamCH8, kParamNote8, kParamFlip8,
    kParamCH9, kParamNote9, kParamFlip9,
    kParamCH10, kParamNote10, kParamFlip10,
    kParamCH11, kParamNote11, kParamFlip11,
    kParamCH12, kParamNote12, kParamFlip12,
    kParamCH13, kParamNote13, kParamFlip13,
    kParamCH14, kParamNote14, kParamFlip14,
    kParamCH15, kParamNote15, kParamFlip15,
    kParamCH16, kParamNote16, kParamFlip16,
    kParamCH17, kParamNote17, kParamFlip17,
    kParamCH18, kParamNote18, kParamFlip18,
    kParamCH19, kParamNote19, kParamFlip19,
    kParamCH20, kParamNote20, kParamFlip20,

    kNumParams,
};
static_assert(kNumParams == 3 + kNumLEDs * 3, "parameter count / kNumLEDs mismatch");

static const _NT_parameter kParameters[] = {
    makeParameter("Breakout",   0, 1, 0, kNT_unitEnum, kOffOnStrings),  // kParamDestBreakout
    makeParameter("USB",        0, 1, 0, kNT_unitEnum, kOffOnStrings),  // kParamDestUSB
    makeParameter("Select Bus", 0, 1, 0, kNT_unitEnum, kOffOnStrings),  // kParamDestSelectBus

    makeParameter("CH1",    0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH1
    makeParameter("Note1", -1, 127, -1, kNT_unitNone),     // kParamNote1
    makeParameter("Flip1",  0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip1
    makeParameter("CH2",    0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH2
    makeParameter("Note2", -1, 127, -1, kNT_unitNone),     // kParamNote2
    makeParameter("Flip2",  0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip2
    makeParameter("CH3",    0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH3
    makeParameter("Note3", -1, 127, -1, kNT_unitNone),     // kParamNote3
    makeParameter("Flip3",  0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip3
    makeParameter("CH4",    0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH4
    makeParameter("Note4", -1, 127, -1, kNT_unitNone),     // kParamNote4
    makeParameter("Flip4",  0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip4
    makeParameter("CH5",    0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH5
    makeParameter("Note5", -1, 127, -1, kNT_unitNone),     // kParamNote5
    makeParameter("Flip5",  0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip5
    makeParameter("CH6",    0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH6
    makeParameter("Note6", -1, 127, -1, kNT_unitNone),     // kParamNote6
    makeParameter("Flip6",  0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip6
    makeParameter("CH7",    0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH7
    makeParameter("Note7", -1, 127, -1, kNT_unitNone),     // kParamNote7
    makeParameter("Flip7",  0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip7
    makeParameter("CH8",    0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH8
    makeParameter("Note8", -1, 127, -1, kNT_unitNone),     // kParamNote8
    makeParameter("Flip8",  0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip8
    makeParameter("CH9",    0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH9
    makeParameter("Note9", -1, 127, -1, kNT_unitNone),     // kParamNote9
    makeParameter("Flip9",  0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip9
    makeParameter("CH10",   0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH10
    makeParameter("Note10", -1, 127, -1, kNT_unitNone),     // kParamNote10
    makeParameter("Flip10", 0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip10
    makeParameter("CH11",   0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH11
    makeParameter("Note11", -1, 127, -1, kNT_unitNone),     // kParamNote11
    makeParameter("Flip11", 0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip11
    makeParameter("CH12",   0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH12
    makeParameter("Note12", -1, 127, -1, kNT_unitNone),     // kParamNote12
    makeParameter("Flip12", 0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip12
    makeParameter("CH13",   0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH13
    makeParameter("Note13", -1, 127, -1, kNT_unitNone),     // kParamNote13
    makeParameter("Flip13", 0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip13
    makeParameter("CH14",   0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH14
    makeParameter("Note14", -1, 127, -1, kNT_unitNone),     // kParamNote14
    makeParameter("Flip14", 0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip14
    makeParameter("CH15",   0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH15
    makeParameter("Note15", -1, 127, -1, kNT_unitNone),     // kParamNote15
    makeParameter("Flip15", 0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip15
    makeParameter("CH16",   0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH16
    makeParameter("Note16", -1, 127, -1, kNT_unitNone),     // kParamNote16
    makeParameter("Flip16", 0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip16
    makeParameter("CH17",   0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH17
    makeParameter("Note17", -1, 127, -1, kNT_unitNone),     // kParamNote17
    makeParameter("Flip17", 0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip17
    makeParameter("CH18",   0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH18
    makeParameter("Note18", -1, 127, -1, kNT_unitNone),     // kParamNote18
    makeParameter("Flip18", 0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip18
    makeParameter("CH19",   0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH19
    makeParameter("Note19", -1, 127, -1, kNT_unitNone),     // kParamNote19
    makeParameter("Flip19", 0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip19
    makeParameter("CH20",   0, 16,  0, kNT_unitEnum, kChannelNames),  // kParamCH20
    makeParameter("Note20", -1, 127, -1, kNT_unitNone),     // kParamNote20
    makeParameter("Flip20", 0, 1,   0, kNT_unitEnum, kFlipStrings),   // kParamFlip20
};
static_assert(ARRAY_SIZE(kParameters) == kNumParams, "parameter table mismatch");

static const uint8_t kRoutingPage[] = { kParamDestBreakout, kParamDestUSB, kParamDestSelectBus };

static const uint8_t kLED1Page[]  = { kParamCH1, kParamNote1, kParamFlip1 };
static const uint8_t kLED2Page[]  = { kParamCH2, kParamNote2, kParamFlip2 };
static const uint8_t kLED3Page[]  = { kParamCH3, kParamNote3, kParamFlip3 };
static const uint8_t kLED4Page[]  = { kParamCH4, kParamNote4, kParamFlip4 };
static const uint8_t kLED5Page[]  = { kParamCH5, kParamNote5, kParamFlip5 };
static const uint8_t kLED6Page[]  = { kParamCH6, kParamNote6, kParamFlip6 };
static const uint8_t kLED7Page[]  = { kParamCH7, kParamNote7, kParamFlip7 };
static const uint8_t kLED8Page[]  = { kParamCH8, kParamNote8, kParamFlip8 };
static const uint8_t kLED9Page[]  = { kParamCH9, kParamNote9, kParamFlip9 };
static const uint8_t kLED10Page[] = { kParamCH10, kParamNote10, kParamFlip10 };
static const uint8_t kLED11Page[] = { kParamCH11, kParamNote11, kParamFlip11 };
static const uint8_t kLED12Page[] = { kParamCH12, kParamNote12, kParamFlip12 };
static const uint8_t kLED13Page[] = { kParamCH13, kParamNote13, kParamFlip13 };
static const uint8_t kLED14Page[] = { kParamCH14, kParamNote14, kParamFlip14 };
static const uint8_t kLED15Page[] = { kParamCH15, kParamNote15, kParamFlip15 };
static const uint8_t kLED16Page[] = { kParamCH16, kParamNote16, kParamFlip16 };
static const uint8_t kLED17Page[] = { kParamCH17, kParamNote17, kParamFlip17 };
static const uint8_t kLED18Page[] = { kParamCH18, kParamNote18, kParamFlip18 };
static const uint8_t kLED19Page[] = { kParamCH19, kParamNote19, kParamFlip19 };
static const uint8_t kLED20Page[] = { kParamCH20, kParamNote20, kParamFlip20 };

static const _NT_parameterPage kPages[] = {
    makePage("Routing", static_cast<uint8_t>(ARRAY_SIZE(kRoutingPage)), kRoutingPage),
    makePage("LED1",  static_cast<uint8_t>(ARRAY_SIZE(kLED1Page)),  kLED1Page),
    makePage("LED2",  static_cast<uint8_t>(ARRAY_SIZE(kLED2Page)),  kLED2Page),
    makePage("LED3",  static_cast<uint8_t>(ARRAY_SIZE(kLED3Page)),  kLED3Page),
    makePage("LED4",  static_cast<uint8_t>(ARRAY_SIZE(kLED4Page)),  kLED4Page),
    makePage("LED5",  static_cast<uint8_t>(ARRAY_SIZE(kLED5Page)),  kLED5Page),
    makePage("LED6",  static_cast<uint8_t>(ARRAY_SIZE(kLED6Page)),  kLED6Page),
    makePage("LED7",  static_cast<uint8_t>(ARRAY_SIZE(kLED7Page)),  kLED7Page),
    makePage("LED8",  static_cast<uint8_t>(ARRAY_SIZE(kLED8Page)),  kLED8Page),
    makePage("LED9",  static_cast<uint8_t>(ARRAY_SIZE(kLED9Page)),  kLED9Page),
    makePage("LED10", static_cast<uint8_t>(ARRAY_SIZE(kLED10Page)), kLED10Page),
    makePage("LED11", static_cast<uint8_t>(ARRAY_SIZE(kLED11Page)), kLED11Page),
    makePage("LED12", static_cast<uint8_t>(ARRAY_SIZE(kLED12Page)), kLED12Page),
    makePage("LED13", static_cast<uint8_t>(ARRAY_SIZE(kLED13Page)), kLED13Page),
    makePage("LED14", static_cast<uint8_t>(ARRAY_SIZE(kLED14Page)), kLED14Page),
    makePage("LED15", static_cast<uint8_t>(ARRAY_SIZE(kLED15Page)), kLED15Page),
    makePage("LED16", static_cast<uint8_t>(ARRAY_SIZE(kLED16Page)), kLED16Page),
    makePage("LED17", static_cast<uint8_t>(ARRAY_SIZE(kLED17Page)), kLED17Page),
    makePage("LED18", static_cast<uint8_t>(ARRAY_SIZE(kLED18Page)), kLED18Page),
    makePage("LED19", static_cast<uint8_t>(ARRAY_SIZE(kLED19Page)), kLED19Page),
    makePage("LED20", static_cast<uint8_t>(ARRAY_SIZE(kLED20Page)), kLED20Page),
};

static const _NT_parameterPages kParameterPages = {
    ARRAY_SIZE(kPages),
    kPages,
};

struct LEDHelperAlgorithm : public _NT_algorithm {
    LEDHelperAlgorithm() {
        parameters       = nullptr;
        parameterPages   = nullptr;
        vIncludingCommon  = nullptr;
        v                = nullptr;
        for (int i = 0; i < kNumLEDs; ++i)
            held[i] = false;
    }

    // held[i] = current toggle state of LED slot i (0-based), independent of Flip — Flip is
    // only applied when deciding what to actually send, not stored here.
    bool held[kNumLEDs];
};

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t*) {
    req.numParameters = ARRAY_SIZE(kParameters);
    req.sram = sizeof(LEDHelperAlgorithm);
    req.dram = 0;
    req.dtc  = 0;
    req.itc  = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs, const _NT_algorithmRequirements&,
                          const int32_t*) {
    LEDHelperAlgorithm* alg = new (ptrs.sram) LEDHelperAlgorithm();
    alg->parameters     = kParameters;
    alg->parameterPages = &kParameterPages;
    return alg;
}

// OR of whichever external destinations are enabled on the Routing page. Internal is
// intentionally never included — see header comment.
uint32_t destinationsFromV(const int16_t* v) {
    uint32_t dest = 0;
    if (v[kParamDestBreakout])  dest |= kNT_destinationBreakout;
    if (v[kParamDestUSB])       dest |= kNT_destinationUSB;
    if (v[kParamDestSelectBus]) dest |= kNT_destinationSelectBus;
    return dest;
}

void midiMessage(_NT_algorithm* self, uint8_t byte0, uint8_t byte1, uint8_t byte2) {
    // Only a genuine Note On (velocity > 0) is a "press" that toggles a slot. A Note On with
    // velocity 0 is the standard MIDI note-off idiom and is ignored, not treated as a second
    // trigger — see header comment.
    if ((byte0 & 0xF0) != 0x90 || byte2 == 0)
        return;

    uint8_t channel = byte0 & 0x0F;   // 0-based
    uint8_t note    = byte1;

    LEDHelperAlgorithm* alg = (LEDHelperAlgorithm*)self;
    const int16_t* v = self->v;
    uint32_t dest = destinationsFromV(v);

    for (int i = 0; i < kNumLEDs; ++i) {
        int base = kParamCH1 + i * 3;
        int16_t chParam   = v[base];       // 0 = None, else channel = chParam - 1
        int16_t noteParam = v[base + 1];   // -1 = None, else note = noteParam (raw 0..127)
        int16_t flipParam = v[base + 2];   // 0 = No, 1 = Yes

        if (chParam == 0 || noteParam < 0)
            continue;   // slot inactive

        uint8_t cfgChannel = (uint8_t)(chParam - 1);
        uint8_t cfgNote    = (uint8_t)noteParam;
        if (cfgChannel != channel || cfgNote != note)
            continue;

        alg->held[i] = !alg->held[i];
        if (!dest)
            continue;   // nowhere to send feedback, but state still tracked

        // Released state is sent as Note On with velocity 0, not a literal Note Off (0x80).
        // Fixed-value pad/button controllers (the VCM-600 included) typically only parse
        // Note On messages for their LED state and silently ignore real Note Off status
        // bytes — sending a genuine 0x80 made every other toggle invisible on the hardware
        // even though the internal held/released state was alternating correctly. Velocity-0
        // Note On is the universal MIDI convention for "note off" and is understood by every
        // receiver that a literal Note Off would work on, plus the Note-On-only ones that
        // wouldn't.
        bool ledOn = flipParam ? !alg->held[i] : alg->held[i];
        NT_sendMidi3ByteMessage(dest, (uint8_t)(0x90 | cfgChannel), cfgNote, ledOn ? kLedVelocity : 0);
    }
}

static const _NT_factory kFactory = {
    NT_MULTICHAR('L','E','D','H'),
    "LED Helper",
    "Invertible MIDI note latch for controller LED feedback",
    0,
    nullptr,
    nullptr,                   // calculateStaticRequirements
    nullptr,                   // initialise
    calculateRequirements,
    construct,
    nullptr,                   // parameterChanged — direct v[] reads instead
    nullptr,                   // step — no audio/CV busses, MIDI-only plugin
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
