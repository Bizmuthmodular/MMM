/*
 * LED Helper for Expert Sleepers disting NT.
 *
 * An invertible MIDI note latch for controller LED feedback (e.g. the Vestax VCM-600), built
 * as a selective MIDI pass-through/filter rather than an independent feedback generator.
 *
 * Real hardware testing established how this class of controller actually behaves: its
 * buttons send a Note On (velocity > 0) on press and a release message (a real Note Off, or
 * a Note On with velocity 0 — the standard MIDI running-status idiom for note-off) on
 * release, and its LED genuinely does latch persistently from a bare MIDI Note On — a plain
 * Note On sent in isolation lights it and it stays lit until another message arrives, with
 * nothing held down. But when the button is used normally, the device's *own* release
 * message reaches it and always turns the LED back off on release, regardless of any separate
 * feedback this plugin tried to send — the plugin's independently-generated feedback message
 * couldn't reliably win against that. So instead of generating new messages, this plugin now
 * sits inline in the MIDI path and selectively swallows every other release for a configured
 * slot, using the device's own already-confirmed-working latch behaviour instead of competing
 * with it. This requires the device to receive MIDI *only* through this plugin — if it also
 * receives the controller's raw output via some parallel path, the real release still reaches
 * it and undoes the block regardless of what's swallowed here.
 *
 * 20 independent slots (LED1..LED20 pages), each watching one CH/Note pair. A press on a
 * slot's configured channel+note always passes straight through unmodified (so the on-press
 * side of the device's latch behaviour is untouched). On release, the slot's toggle flips:
 * if the new state is "held", the release is swallowed entirely (never forwarded), so the
 * device never receives an Off and its LED stays lit from the press that already went
 * through; if the new state is "released", the release is forwarded normally and the LED
 * goes dark. Net effect over successive press/release cycles: press+release #1 leaves the
 * LED lit, #2 leaves it dark, #3 lit again, and so on. Flip inverts which of those two
 * outcomes is which — useful when a button's LED should show the opposite of the raw toggle
 * (e.g. lit = unmuted). Every other MIDI message — including Note On for configured slots,
 * and anything at all for channel/note combinations no slot is watching — passes through
 * completely unchanged; only a swallowed release is ever dropped.
 *
 * Each slot tracks whether it's currently physically down (a press seen with no release yet)
 * so a release with no matching prior press — which shouldn't happen given the confirmed
 * clean 1:1 press/release behaviour, but costs nothing to guard against — is passed through
 * unmodified rather than spuriously toggling the latch.
 *
 * CH defaults to None (index 0 of its enum) and Note defaults to None (-1, shown as a plain
 * raw number 0..127 for real notes rather than named pitches) — a slot with either set to
 * None is inactive and never intercepts anything; matching traffic on that channel/note just
 * passes through like any other unmatched message.
 *
 * Routing page controls where everything is forwarded to: Breakout (DIN), USB, Select Bus —
 * any combination, all default Off. Internal is deliberately not offered: it is a same-unit
 * software bus with no per-message source info, so anything sent there would come straight
 * back into this plugin's own midiMessage() as if it were new incoming traffic and get
 * forwarded again — an unbounded loop, since this plugin now forwards almost everything by
 * design (unlike the old independent-feedback approach, which only ever sent a handful of
 * synthesised messages and so was merely at risk of re-triggering its own toggle).
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

constexpr int kNumLEDs = 20;

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
        for (int i = 0; i < kNumLEDs; ++i) {
            held[i] = false;
            down[i] = false;
        }
    }

    // held[i] = logical toggle state of LED slot i (0-based), flipped on every release this
    // plugin processes for that slot. Combined with Flip, this decides whether a given
    // release gets swallowed (LED stays lit) or forwarded (LED goes dark) — see midiMessage().
    bool held[kNumLEDs];

    // down[i] = slot i currently has an unmatched press (seen a press, no release yet).
    // Guards against toggling on a stray release with no matching prior press. See
    // midiMessage().
    bool down[kNumLEDs];
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

// Forwards one MIDI message unchanged to whichever destinations are enabled, using the
// correct message length (Program Change / Channel Pressure are 2-byte; everything else
// handled here is 3-byte).
void forwardMidi(uint32_t dest, uint8_t byte0, uint8_t byte1, uint8_t byte2) {
    if (!dest)
        return;
    uint8_t hiNibble = byte0 & 0xF0;
    if (hiNibble == 0xC0 || hiNibble == 0xD0)
        NT_sendMidi2ByteMessage(dest, byte0, byte1);
    else
        NT_sendMidi3ByteMessage(dest, byte0, byte1, byte2);
}

void midiMessage(_NT_algorithm* self, uint8_t byte0, uint8_t byte1, uint8_t byte2) {
    LEDHelperAlgorithm* alg = (LEDHelperAlgorithm*)self;
    const int16_t* v = self->v;
    uint32_t dest = destinationsFromV(v);

    uint8_t hiNibble = byte0 & 0xF0;
    if (hiNibble == 0x90 || hiNibble == 0x80) {
        uint8_t channel = byte0 & 0x0F;   // 0-based
        uint8_t note    = byte1;

        // Press = Note On with velocity > 0; anything else here (a real Note Off, or a Note
        // On with velocity 0 — the standard MIDI running-status idiom for note-off) is a
        // release.
        bool isPress = (hiNibble == 0x90 && byte2 > 0);

        for (int i = 0; i < kNumLEDs; ++i) {
            int base = kParamCH1 + i * 3;
            int16_t chParam   = v[base];       // 0 = None, else channel = chParam - 1
            int16_t noteParam = v[base + 1];   // -1 = None, else note = noteParam (raw 0..127)
            int16_t flipParam = v[base + 2];   // 0 = No, 1 = Yes

            if (chParam == 0 || noteParam < 0)
                continue;   // slot inactive — check the next slot

            uint8_t cfgChannel = (uint8_t)(chParam - 1);
            uint8_t cfgNote    = (uint8_t)noteParam;
            if (cfgChannel != channel || cfgNote != note)
                continue;   // not this slot — check the next one

            // Found the (at most one, by configuration) slot watching this channel+note.
            if (isPress) {
                alg->down[i] = true;
                break;   // presses always pass through — fall to forwardMidi() below
            }

            if (!alg->down[i])
                break;   // stray release, no matching press — pass through unmodified

            alg->down[i] = false;
            alg->held[i] = !alg->held[i];
            bool ledOn = flipParam ? !alg->held[i] : alg->held[i];
            if (ledOn)
                return;   // swallow this release entirely — that's what keeps the LED lit
            break;        // forward it below — that's what turns the LED off
        }
    }

    forwardMidi(dest, byte0, byte1, byte2);
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
