// ECU Tuning -- per vehicle, in the garage: ignition timing, fuel map, boost and anti-lag make
// the power, and the exhausts pop, bang and spit fire to match.
// plugins\mt_plugin_ECUTuning.dll.
//
// The game has an afterfire of its own, but it hardly reads the revs and most mod engines
// never play it: they set AfterFireProbability and have no BackfireSound. So this runs its
// own pops on every engine with AfterFireProbability > 0 (diesels have none) and empties
// the engine's AfterfireAudioComponents so the two do not double up.
//
// A pop can happen while the revs fall off the throttle, or on the limiter. The chance per
// second is PopsPerSecond * BackfireChance (at most 1) * x^2, where x runs from 0 at
// MinRevs to 1 at the limiter. So a lift at redline crackles and fades as the revs drop.
//
// Pipes, best first: the actor-space transforms the engine keeps of the mesh's Exhaust*
// sockets (about 90 vehicles have them); attached actors with Exhaust in the class name, the
// fitted exhaust tips; else a pair guessed from the bounds.
//
// Smoke and flames leave along the socket's own rotation, as the game's smoke does: a truck
// stack can blow sideways (the Duro's yaw is 103). A flame is built here, as nothing in the game
// is a jet: each burst is a few tongues of see-through segments (M_GhostCar) that leave the
// pipe as one tight throat, swell, branch apart and thin into tips, licking and rising, and die
// from the pipe outwards; a hot opaque core (M_AttachmentLight) sits in the throat. The kind of
// firing sets colour and character (LOOKS): crackle, gunshot, rich pop, anti-lag. They ride with the car. Each burst has an energy, the
// fuel that went unburnt: it sets how many streaks fire and how many leave the middle for a
// circle a little way out, around a noise-driven hot spot, so no two are the same.
//
// Each vehicle is tuned in the game: the garage's Tuning menu gets an Exhaust button beside
// Wheel/Tire Size, which opens rows of the game's own W_Setting, kept on the car itself (its
// customisation, for other players) and in ecu_tuning.ini, [Vehicle<id>], which is what persists. A tuned vehicle pops at PopsPerSecond * BackfireChance;
// AfterFireProbability only says the engine can pop at all. The game clears that list
// for each of its menus, so the button goes back only when the first child is a menu button
// (the wheel rows are not). The rows run in the game's own number mode, found by signature,
// so its arrows, keys and held repeat all work; the plugin only watches the value. The
// Exhaust button is polled: delegates cannot be bound from a plugin.
// Everything is this client's alone: no table or asset changes, so nothing to mismatch
// with a server, and other players do not see your settings.
//
// Shutting the throttle on a turbo plays its release, the stu-tu-tu (see flutterOf). The shot is
// QX_Flutter_Shot, shipped in MoreTuning under Sounds/, which the server paks drop.
//
// Settings: Binaries\Win64\ecu_tuning.ini, [ECU], the defaults for an untuned vehicle:
// Enabled (1), PopsPerSecond (8), MinRevs (0.55, share of the limiter), Volume (1), Flames
// (1), FlameLength (0.04), FlameScale (0.1), FlameWidth (0.2), BackfireChance (0), BangChance (0),
// Timing (0.5), FuelMap (0.5), Boost (0.5), AntiLag (0), AntiLagRevs (0.5), DPFDelete (0), SmokeAmount (3), SmokeLasts (4),
// FlameBrightness (1.6, of the see-through flame), FlameDepthBias (-40), FlameLife (0.04, how long a burst lasts;
// FlameLength is how far it reaches), FlameDensity (1), FlameShape (0 Auto, see shapeFor; 1-9 one shape).
// Also Range (10000 cm), LowEffects (0: lean flames and smoke only on the game's Low effects; 1: always).
//
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "../include/mt_plugin.hpp"

using namespace mt;

struct Vec { double x, y, z; };
struct Ref { int index; uintptr_t ptr; };
struct Pipe { Vec at; double spin[4]; double radius; };
enum { MAX_TONGUES = 5, MAX_SEGS = 4 };
// One tongue of a burst: it leaves the pipe in the shared throat and branches off at `branch`
// (share of its length), `angle` away from the axis towards `azimuth`.
struct Tongue { double azimuth, angle, branch, length, width, phase, shade, shift; Ref seg[MAX_SEGS]; uintptr_t mid[MAX_SEGS]; bool vis[MAX_SEGS], parked[MAX_SEGS]; };
struct Glow { double peakK, mouthK, premix; };  // hottest point (kelvin), gas at the mouth, share burning premixed
struct Spark { Ref c; uintptr_t mid; Vec pos, vel; double age, life, width, length, kick, heat, out, tint; bool live, shown; };
enum { MAX_LAYERS = 5 };  // classic: cone 0 the outer shell, 1 the opaque core, 2.. inner layers
// shape 0: tongues; 1: classic, which uses cone*, sparks and the fields after pace.
enum { POOL = MAX_TONGUES * MAX_SEGS };
struct Slot { Ref owner; Pipe pipe; Vec dir, r1, r2, lightAt; Ref core, light; uintptr_t coreMid; double glow, flash, advance, rich, size, reach, spread, energy, detail, lod, ratio, wobble, pace,
    coneLength, style, dense, debt, swirl, spin, phase[3]; Glow flameHue; Ref cone[MAX_LAYERS]; uintptr_t coneMid[MAX_LAYERS]; DWORD start, end; int kind, tongues, segs, layers, shape, count; double rnd[POOL][4];
    bool on, culled, coreVis, coreParked, idle; Tongue tongue[MAX_TONGUES]; std::vector<Spark> sparks; };
struct Voice { Ref owner; DWORD at; float pitch, volume, lowpass, highpass; };
struct Flutter { double pitch, low, high, volume; int shots; DWORD gap; int turbos; };
struct Trim { float base, wrote; bool set; };
struct Car { std::vector<Pipe> fire; int listed = 0; DWORD sockAt = 0; std::string sockSig; DWORD flutterAt = 0, ecuAt = 0, bangAt = 0, alsUntil = 0, liftAt = 0; bool lagging = false, host = false, pedalSeen = false, armed = false, throttleSeen = false, repSeen = false; double egt = 0.25, pipeFuel = 0, loss = 0, burn = 0, burnFor = 0, soak = 0; bool licked = false; double wall[5] = {650, 600, 560, 500, 450}, mouthK = 600, tipFuel = 0, tipO2 = 0.5; DWORD lastLight = 0, burnPause = 0, burnNext = 0; std::vector<DWORD> recent; float lastRatio = 0, guess = 0; DWORD seenAt = 0; Trim trim[14] = {}; Ref whine = {}, self = {}, voice[8] = {}; uintptr_t voiceSound[8] = {}; int nextVoice = 0; DWORD whineCheck = 0, whineAt = 0; int blower = -1; bool whineNear = false; float whineLevel = 0; float rpm = 0, fuel = 0, spool = 0; DWORD next = 0; bool read = false; DWORD readAgain = 0; std::string pipeSig; uintptr_t engine = 0; std::vector<Vec> wrote; std::vector<Pipe> pipes; std::vector<Slot> slots; std::vector<Ref> smokes;
    double burnK = 0, tankLast = -1, learnDrop = 0, learnFlow = 0, learnT = 0; };  // flames burn real fuel (exhaust())
struct FStr { const wchar_t* data; int num, max; };
struct Tune { double on, pops, revs, volume, flames, length, size, dpf, smoke, lasts, spread, chance, bang, timing, fuel, boost, lag, lagRevs, flameRevs, cool, life, density, shape, twin, gap, p1, p2, p3, p4, x1, x2, x3, x4, y1, y2, y3, y4, z1, z2, z3, z4, ovFuel, ovRetard, spark, decat, wide, header, back; bool own; };

static const struct { const wchar_t* key; double Tune::*field; } KEYS[] = {
    {L"Enabled", &Tune::on}, {L"PopRate", &Tune::pops}, {L"MinRevs", &Tune::revs}, {L"PopVolume", &Tune::volume},
    {L"Flames", &Tune::flames}, {L"FlameLength", &Tune::length}, {L"FlameSize", &Tune::size},
    {L"DPFDelete", &Tune::dpf}, {L"SmokeAmount", &Tune::smoke}, {L"SmokeLasts", &Tune::lasts}, {L"FlameWidth", &Tune::spread},
    {L"BackfireChance", &Tune::chance}, {L"BangChance", &Tune::bang},
    {L"Timing", &Tune::timing}, {L"FuelMap", &Tune::fuel}, {L"Boost", &Tune::boost}, {L"AntiLag", &Tune::lag}, {L"AntiLagRevs", &Tune::lagRevs}, {L"FlameRevs", &Tune::flameRevs}, {L"CoolingBoost", &Tune::cool}, {L"FlameLife", &Tune::life}, {L"FlameDensity", &Tune::density}, {L"FlameShape", &Tune::shape}, {L"TwinTips", &Tune::twin}, {L"TwinSpacing", &Tune::gap},
    {L"Pipe1", &Tune::p1}, {L"Pipe2", &Tune::p2}, {L"Pipe3", &Tune::p3}, {L"Pipe4", &Tune::p4},
    {L"Pipe1X", &Tune::x1}, {L"Pipe2X", &Tune::x2}, {L"Pipe3X", &Tune::x3}, {L"Pipe4X", &Tune::x4},
    {L"Pipe1Y", &Tune::y1}, {L"Pipe2Y", &Tune::y2}, {L"Pipe3Y", &Tune::y3}, {L"Pipe4Y", &Tune::y4},
    {L"Pipe1Z", &Tune::z1}, {L"Pipe2Z", &Tune::z2}, {L"Pipe3Z", &Tune::z3}, {L"Pipe4Z", &Tune::z4},
    {L"OverrunFuel", &Tune::ovFuel}, {L"OverrunRetard", &Tune::ovRetard}, {L"LimiterSpark", &Tune::spark}, {L"CatDelete", &Tune::decat}, {L"WideTips", &Tune::wide}, {L"BackPressure", &Tune::back}};
enum { TOGGLE, SLIDER, REAL, HEADER };  // HEADER: a section title, its field a placeholder
static bool textRow(int kind) { return kind >= HEADER; }
enum { BURNS = 1, POPS_ = 2, DIESEL = 4, TURBO = 8, PIPE1 = 16, PIPE2 = 32, PIPE3 = 64, PIPE4 = 128 };  // what a row needs: from the engine, or that many exhaust sockets
// A slider always reads 0 to 1 and maps onto min..max. Timing, fuel and boost have stock at 0.50.
// In sections, like the Exhaust Flame Bench. There is no backfire chance, pop rate or flame shape:
// pops, bangs and flames come out of the tune and the engine (see exhaust()).
static const struct { const wchar_t* label; double Tune::*field; float min, max; int kind, needs; } ROWS[] = {
    {L"ENGINE", &Tune::header, 0, 0, HEADER, BURNS},
    {L"Ignition timing", &Tune::timing, 0, 1, SLIDER, BURNS},
    {L"Fuel map", &Tune::fuel, 0, 1, SLIDER, BURNS},
    {L"Cooling boost", &Tune::cool, 0, 1, SLIDER, BURNS},
    {L"Boost pressure", &Tune::boost, 0, 1, SLIDER, TURBO},
    {L"OVERRUN", &Tune::header, 0, 0, HEADER, POPS_},     // what happens when you lift
    {L"Overrun fuel", &Tune::ovFuel, 0, 1, SLIDER, POPS_}, // 0 is fuel cut
    {L"Overrun retard", &Tune::ovRetard, 0, 1, SLIDER, POPS_},
    // Labels stay at two words: longer ones crowd the row. Revs rows are a share of the rev limit.
    {L"Pop revs", &Tune::revs, 0.1f, 0.9f, REAL, POPS_},
    {L"LIMITER", &Tune::header, 0, 0, HEADER, POPS_},
    {L"Limiter type", &Tune::spark, 0, 1, TOGGLE, POPS_},  // fuel cut or spark cut
    {L"ANTI-LAG", &Tune::header, 0, 0, HEADER, TURBO},
    {L"Anti-lag", &Tune::lag, 0, 1, TOGGLE, TURBO},
    {L"Anti-lag revs", &Tune::lagRevs, 0.1f, 0.9f, REAL, TURBO},
    {L"EXHAUST", &Tune::header, 0, 0, HEADER, POPS_},
    {L"Cat delete", &Tune::decat, 0, 1, TOGGLE, POPS_},
    {L"Back pressure", &Tune::back, 0, 1, SLIDER, POPS_},  // 0 a straight pipe, 1 a restrictive muffler
    {L"Wide tips", &Tune::wide, 0, 1, TOGGLE, POPS_},
    {L"Twin tips", &Tune::twin, 0, 1, TOGGLE, POPS_},     // a socket placed between twin tips fires from both
    {L"Twin spacing", &Tune::gap, 3, 20, REAL, POPS_},    // cm from the socket to each tip
    // Each exhaust socket the car has: on or off (one always stays on), and where it sits, in cm
    // from the game's own spot: X along the car, Y across it, Z up. Flames and smoke follow it.
    {L"Pipe 1", &Tune::p1, 0, 1, TOGGLE, PIPE1}, {L"Pipe 1 X", &Tune::x1, -50, 50, REAL, PIPE1}, {L"Pipe 1 Y", &Tune::y1, -50, 50, REAL, PIPE1}, {L"Pipe 1 Z", &Tune::z1, -50, 50, REAL, PIPE1},
    {L"Pipe 2", &Tune::p2, 0, 1, TOGGLE, PIPE2}, {L"Pipe 2 X", &Tune::x2, -50, 50, REAL, PIPE2}, {L"Pipe 2 Y", &Tune::y2, -50, 50, REAL, PIPE2}, {L"Pipe 2 Z", &Tune::z2, -50, 50, REAL, PIPE2},
    {L"Pipe 3", &Tune::p3, 0, 1, TOGGLE, PIPE3}, {L"Pipe 3 X", &Tune::x3, -50, 50, REAL, PIPE3}, {L"Pipe 3 Y", &Tune::y3, -50, 50, REAL, PIPE3}, {L"Pipe 3 Z", &Tune::z3, -50, 50, REAL, PIPE3},
    {L"Pipe 4", &Tune::p4, 0, 1, TOGGLE, PIPE4}, {L"Pipe 4 X", &Tune::x4, -50, 50, REAL, PIPE4}, {L"Pipe 4 Y", &Tune::y4, -50, 50, REAL, PIPE4}, {L"Pipe 4 Z", &Tune::z4, -50, 50, REAL, PIPE4},
    {L"DIESEL", &Tune::header, 0, 0, HEADER, DIESEL},
    {L"DPF delete", &Tune::dpf, 0, 1, TOGGLE, DIESEL},
    {L"Smoke amount", &Tune::smoke, 0, 5, SLIDER, DIESEL},
    {L"Smoke duration", &Tune::lasts, 1, 5, REAL, DIESEL}};
static const int NROWS = (int)(sizeof ROWS / sizeof *ROWS);

static bool scaled(int i) { return ROWS[i].kind == SLIDER; }
static float stepOf(int i) { return scaled(i) ? 0.01f : ROWS[i].kind == TOGGLE || ROWS[i].max >= 10 ? 1 : ROWS[i].max >= 5 ? 0.1f : 0.01f; }
static double uiLow(int i, const Tune&) { return scaled(i) ? 0 : ROWS[i].min; }
static double uiHigh(int i, const Tune&) { return scaled(i) ? 1 : ROWS[i].max; }
static double toUi(int i, const Tune& t) { double v = t.*ROWS[i].field; return scaled(i) ? (v - ROWS[i].min) / (ROWS[i].max - ROWS[i].min) : v; }
// The game's spin box scales its step by the magnitude of the value: 0.01 below 1, 10 from 10 up.
// So what it holds is a step count in 1.00 .. 9.99, where the scale is exactly 1, and the row
// shows the real number as text.
static double stepsTo(int i, double ui, const Tune& t) { return floor((ui - uiLow(i, t)) / stepOf(i) + 0.5); }
static double toSpin(int i, const Tune& t);
static double spinTop(int i, const Tune& t) { return 1 + 0.01 * stepsTo(i, uiHigh(i, t), t); }
static double fromUi(int i, double ui) { return scaled(i) ? ROWS[i].min + ui * (ROWS[i].max - ROWS[i].min) : ui; }

static double toSpin(int i, const Tune& t) { return 1 + 0.01 * stepsTo(i, toUi(i, t), t); }
static double fromSpin(int i, double v, const Tune& t) { return uiLow(i, t) + floor((v - 1) / 0.01 + 0.5) * stepOf(i); }

// Keeps every row inside what the stage allows, so the effects never see more than the screen shows.
static void limit(Tune& t) {
    for (int i = 0; i < NROWS; i++) {
        double ui = toUi(i, t), low = uiLow(i, t), high = uiHigh(i, t);
        t.*ROWS[i].field = fromUi(i, ui < low ? low : ui > high ? high : ui);
    }
}

// What is on the Tuning screen. Widgets belong to the screen, so a new screen means a new one.
struct Row { uintptr_t widget, spin, value; };
struct Panel {
    Ref screen;
    uintptr_t button, inner, reset, resetInner;
    bool buttonHeld, open, resetHeld;
    int labelFrames, resetFrames;
    Row rows[NROWS];
    std::vector<uintptr_t> hidden;
    int64_t id;
    DWORD dirty;
};

static Tune base;
static std::map<int64_t, Tune> tunes;
static double kRange;
static Panel panel;

static std::mutex lock;
static std::vector<Ref> vehicles, screens;
static std::vector<Voice> voices;
static std::map<uintptr_t, Car> cars;
static volatile LONG queued;
static DWORD lastTick;
static int logged;

static uintptr_t cFx, cSetting, cButton, cButtonBase, cText, cUButton, gameplayCDO, systemCDO, stringCDO, niagaraCDO, widgetCDO, textCDO;
static uintptr_t fLoc, fVelocity, fPawn, fRoot, fBounds, fSound, fSpawn, fSetFloat, fSetColor, fSetVec, fDilation, fStop, fToName, fPath, fRef, fLoad;
static uintptr_t fCreate, fAddChild, fChildIndex, fChildAt, fChildCount, fToText, fSetText, fSetRich, fPressed, fRemove, fSetVis, fIsVisible, fPadding;
static uintptr_t sound, cone, ball, glow, ghost, smoke, playerNow;
static uint64_t colorName, opacityName, depthBiasName;
// M_GhostCar is translucent and unlit, so layers of it read as a body of flame where the opaque
// M_AttachmentLight is a flat silhouette. Without it everything falls back to the old look.
static float kGhostGain = 1.6f;  // [ECU] FlameBrightness: its colour is its whole output; much past 1 the bloom turns it white
// In sunlight the camera's exposure comes right down, and an unlit flame bright at night drew as a
// black blob over the sunlit road. The sun's height and strength (`daylight`, 0 night to 1 noon) lift
// it by up to [ECU] FlameDaylight times more.
static float kDayGain = 12;
static double daylight;
static float flameGain() { return kGhostGain * (float)(1 + kDayGain * daylight); }
// [ECU] FlameOpacity: how solid a flame piece is, times its own fade. Above 1 is less see-through.
static float kOpacity = 1.35f;
static float solid(double alpha) { double a = alpha * kOpacity; return (float)(a > 1 ? 1 : a); }
// [ECU] FlameMaterial: 0 M_GhostCar (the one with Color, Opacity and DepthBias), 1 the engine's plain
// unlit translucent (takes no Color: every see-through piece draws black, plain in daylight), 2 the game's interaction
// highlight, 3 none (every piece solid, M_AttachmentLight). The ghost material may fade surfaces by
// the way they face, which would hide the lower half of every round piece: this is to find out.
static int kMaterial = 0;
static bool ghostTried;
static const wchar_t* ghostPath() {
    static const wchar_t* const PATHS[3] = {L"/Game/Cars/Materials/M_GhostCar.M_GhostCar",
        L"/Engine/EngineDebugMaterials/M_SimpleUnlitTranslucent.M_SimpleUnlitTranslucent", L"/Game/Objects/M_Interactor2.M_Interactor2"};
    return kMaterial >= 0 && kMaterial < 3 ? PATHS[kMaterial] : nullptr;
}
static float kDepthBias = -40;   // [ECU] FlameDepthBias: drawn this far towards the camera, so the road cannot cover its lower half
static uintptr_t layer() { return ghost ? ghost : glow; }
static uintptr_t cMeshComp, fAddComp, fFinish, fMobility, fMesh, fCollision, fShadow, fMakeMid, fMidColor, fMidScalar, fRelLoc, fMeshVis;
static bool canFlame = true, canGlow = true, canEcu = true;
// Effects quality Low (or LowEffects=1): one flame shape at its leanest, the smoke at its least.
// Only what this PC draws; the tune carried to others is untouched, and the flame light stays.
static bool lowFx, lowForced;
static bool whineIsOurs;
static int whineTries;  // reset when the collector frees the whine, or it stops loading after the 3rd collection
static uintptr_t fSetSound, fIsServer, whistle, fWhinePitch, fWhineVolume, fWhineIsOn, fWhinePlay, fWhineStop, fFadeOut;
static int oTankFuel = -1, oTankCap = -1;  // MTVehicle.NetLC_VehicleState.Fuel, FuelTankCapacityInLiter
static int oFuelType = -1, oBrake = -1, oPedal = -1, oRepPedal = -1, oAiDriving = -1, oTurbineSpeed = -1;
static uintptr_t waves[6], booms[2];
static int oTrim[14];
static uintptr_t cLight, fIntensity, fLightColor, fLightRadius, fLightShadows, fLightUnits;
static int oRelLocAt, oRelRotAt, oTurbo = -1, oParts = -1, oPartKey = -1, oPartSlot = -1, partSize = 0;
static uintptr_t lastMid, fRelRot, fFlutter, fLowOn, fLowHz, fHighOn, fHighHz, flutterShot, flutterRoom;
static bool canFlutter = true;
static uint64_t lightColorName, lightPowerName;
static uintptr_t mathCDO, fRelScale, fAttached, fToWorld, fInverse;
static uint64_t smokeAmountName, smokeRadiusName, smokePressureName, smokeColorName, smokeVelocityName;
static int oWhite = -1, oTorque = -1;
static int oEngine, oId, oData, oProp, oHot, oNative, oAfter, oMax, oMotor, oRpm, oThrottle, oFxSmoke;
static int oVehicle, oBox, oPipes, oRowName, oRowSpin, oSpinText, oSpinValue, oBtnName, oBtnRich, oBtnText, oBtnInner, pBoundsExtent, pBoundsOrigin, pEffectAt, pEffectRot;
static void (__fastcall* spinRange)(uintptr_t self, float min, float max, float step, bool snap);
static void (__fastcall* spinSet)(uintptr_t self, float value);
static int pPathIn, pPathRet, pRefIn, pRefRet, pLoadIn, pLoadRet, pNameIn, pNameRet, pTextIn, pTextRet;
static int pCtx, pSound, pAt, pVolume, pPitch, pSystem, pAttach, pDestroy, pActivate, pSpawnRet;

static bool alive(const Ref& r) { return r.ptr && api->object(r.index) == r.ptr; }
static bool dying(uintptr_t o) { return (get<uint32_t>(o + L().obj_flags) & 0x18000) != 0; }  // RF_BeginDestroyed | RF_FinishDestroyed
static double roll() { return rand() / (double)RAND_MAX; }
static FStr str(const wchar_t* s) { int n = (int)wcslen(s) + 1; return {s, n, n}; }

static Vec callVec(uintptr_t obj, uintptr_t fn) {
    Vec v{};
    api->process_event(obj, fn, &v);
    return v;
}


// Your tunes are kept in ecu_tuning.ini, [Vehicle<Net_VehicleId>]; an owned car keeps its id from one
// session to the next. They also travel on the car (sendTune) so other players see them, but that copy
// is not what persists: the game saves a car's customisation list only now and then, not as it changes.
static std::wstring section(int64_t id) { return L"Vehicle" + std::to_wstring(id); }
static std::set<std::wstring> tuned;  // ini sections, read once: no file work during play

static Tune& tuneOf(int64_t id) {
    if (!id) return base;
    auto known = tunes.find(id);
    if (known != tunes.end()) return known->second;
    Tune t = base;
    if (tuned.count(section(id)))
        for (auto& k : KEYS) {
            std::wstring v = iniGet(L"ecu_tuning.ini", section(id).c_str(), k.key);
            if (!v.empty()) { t.*k.field = _wtof(v.c_str()); t.own = true; }
        }
    limit(t);
    return tunes[id] = t;
}

// ---- The tune travels on the car, the way Custom Plates' design does ----
// A vehicle's Customization, a list of (material slot, colour), goes to every player and is
// saved with the car. Ten entries named QXE0..QXE9 carry the tune: each row as its step number,
// packed into the colours' 24 bits, with a checksum. No material has those slot names, so they
// paint nothing, and a machine without this plugin ignores them. Every machine then fires a
// car's pops and flames from the car's own tune -- and the host applies its power.
enum { CARRY = 10 };
static uint64_t carrierE[CARRY];
static uintptr_t fGetPC, fSendCustom, cVehCls;
static int oCustom = -1, cColors, cSlot, cColor, cSize, customSize, pSendVehicle, pSendCustom;

static int rowTop(int i) { Tune t{}; return (int)stepsTo(i, uiHigh(i, t), t); }
static int rowBits(int i) { int b = 1; while ((1 << b) <= rowTop(i)) b++; return b; }

static void packTune(const Tune& t, int out[CARRY]) {
    uint8_t bits[CARRY * 24] = {0};
    int at = 0, sum = 0;
    auto put = [&](int v, int n) { for (int k = 0; k < n; k++) bits[at++] = (v >> k) & 1; };
    for (int i = 0; i < NROWS; i++) {
        if (textRow(ROWS[i].kind)) continue;
        int v = (int)stepsTo(i, toUi(i, t), t), top = rowTop(i);
        v = v < 0 ? 0 : v > top ? top : v;
        put(v, rowBits(i));
        sum += v * (i + 1);
    }
    put(sum & 0xFF, 8);
    for (int e = 0; e < CARRY; e++) {
        out[e] = 0;
        for (int k = 0; k < 24; k++) out[e] |= bits[e * 24 + k] << k;
    }
}

static bool unpackTune(const int in[CARRY], Tune& t) {
    int at = 0, sum = 0;
    auto take = [&](int n) { int v = 0; for (int k = 0; k < n; k++, at++) v |= ((in[at / 24] >> (at % 24)) & 1) << k; return v; };
    t = base;
    for (int i = 0; i < NROWS; i++) {
        if (textRow(ROWS[i].kind)) continue;
        int v = take(rowBits(i));
        if (v > rowTop(i)) return false;
        t.*ROWS[i].field = fromUi(i, uiLow(i, t) + v * stepOf(i));
        sum += v * (i + 1);
    }
    if (take(8) != (sum & 0xFF)) return false;
    t.own = true;
    return true;
}

static int carriedAt(uint64_t name) {
    for (int i = 0; i < CARRY; i++)
        if (carrierE[i] && carrierE[i] == name) return i;
    return -1;
}

static bool readTune(uintptr_t car, Tune& out) {
    if (oCustom < 0 || !carrierE[0]) return false;
    uintptr_t d = get<uintptr_t>(car + oCustom + cColors);
    int n = get<int>(car + oCustom + cColors + 8), in[CARRY] = {0}, got = 0;
    for (int i = 0; d && i < n && i < 200; i++) {
        uintptr_t e = d + (uintptr_t)i * cSize;
        int at = carriedAt(get<uint64_t>(e + cSlot));
        if (at < 0) continue;
        in[at] = get<int>(e + cColor) & 0xFFFFFF;
        got |= 1 << at;
    }
    return got == (1 << CARRY) - 1 && unpackTune(in, out);
}

// The list goes back whole: everything else on it as it was (Custom Plates' entries among it),
// then ours. Sent only when ours differ.
static void sendTune(uintptr_t car, const Tune& t) {
    if (oCustom < 0 || !carrierE[0]) return;
    int want[CARRY];
    packTune(t, want);
    uintptr_t d = get<uintptr_t>(car + oCustom + cColors);
    int n = get<int>(car + oCustom + cColors + 8), have = 0, seen = 0;
    static unsigned char list[0x40 * 220];  // the car's list is shared: plates, the tune, Attachment Tweaks' lights
    if (n > 200 || cSize > 0x40) return;
    bool same = true;
    for (int i = 0; d && i < n; i++) {
        uintptr_t e = d + (uintptr_t)i * cSize;
        int at = carriedAt(get<uint64_t>(e + cSlot));
        if (at >= 0) { seen |= 1 << at; same = same && (get<int>(e + cColor) & 0xFFFFFF) == want[at]; continue; }
        rd(e, list + (uintptr_t)have * cSize, cSize);
        have++;
    }
    if (same && seen == (1 << CARRY) - 1) return;
    for (int i = 0; i < CARRY; i++) {
        unsigned char* e = list + (uintptr_t)(have + i) * cSize;
        memset(e, 0, cSize);
        uint32_t color = (uint32_t)want[i] | 0xFF000000u;
        memcpy(e + cSlot, &carrierE[i], 8);
        memcpy(e + cColor, &color, 4);
    }
    struct { uintptr_t context; int index; uintptr_t ret; } pc = {car, 0, 0};
    api->process_event(gameplayCDO, fGetPC, &pc);
    if (!pc.ret) return;
    unsigned char q[0x40] = {0};
    memcpy(q + pSendVehicle, &car, 8);
    rd(car + oCustom, q + pSendCustom, customSize);
    struct { void* data; int num, max; } colors = {list, have + CARRY, have + CARRY};
    memcpy(q + pSendCustom + cColors, &colors, sizeof colors);
    api->process_event(pc.ret, fSendCustom, q);
    static int said;
    if (said < 20) { said++; MT_LOG("tune sent on the car: %d other entries kept", have); }
}

static Ref refOf(uintptr_t o);
// A car's tune: your own as the panel holds it; anyone else's as the car carries it, else the ini's.
static const Tune& tuneFor(uintptr_t v) {
    struct Carried { DWORD at; bool have; Tune t; Ref self; };
    // By the car itself, not its id: every car still without one (id 0) shared an entry and wore
    // the last one read's tune, pipe offsets and all.
    static std::map<uintptr_t, Carried> carried;
    if (carried.size() > 512) carried.clear();  // cars gone; a second's re-read refills it
    int64_t id = get<int64_t>(v + oId);
    if (v == playerNow || oCustom < 0) return tuneOf(id);
    DWORD now = GetTickCount();
    Carried& c = carried[v];
    Ref self = refOf(v);
    if (c.self.index != self.index || !c.at || now - c.at > 1000) { c.at = now | 1; c.have = readTune(v, c.t); c.self = self; }
    return c.have ? c.t : tuneOf(id);
}

static uintptr_t loadAsset(const wchar_t* path) {
    unsigned char a[0x80] = {0}, b[0x80] = {0}, c[0x80] = {0};
    FStr s = str(path);
    memcpy(a + pPathIn, &s, sizeof s);
    api->process_event(systemCDO, fPath, a);
    memcpy(b + pRefIn, a + pPathRet, pRefRet - pRefIn);
    api->process_event(systemCDO, fRef, b);
    memcpy(c + pLoadIn, b + pRefRet, pLoadRet - pLoadIn);
    api->process_event(systemCDO, fLoad, c);
    uintptr_t got = 0;
    memcpy(&got, c + pLoadRet, 8);
    return got;
}

static uint64_t nameOf(const wchar_t* text) {
    unsigned char p[0x40] = {0};
    FStr s = str(text);
    memcpy(p + pNameIn, &s, sizeof s);
    api->process_event(stringCDO, fToName, p);
    uint64_t n = 0;
    memcpy(&n, p + pNameRet, 8);
    return n;
}

static uintptr_t attachEffect(uintptr_t system, uintptr_t parent, const Vec& at, const Vec& rotation) {
    unsigned char p[0xa0] = {0};
    memcpy(p + pSystem, &system, 8);
    memcpy(p + pAttach, &parent, 8);
    memcpy(p + pEffectAt, &at, sizeof at);
    memcpy(p + pEffectRot, &rotation, sizeof rotation);
    p[pDestroy] = 1;
    p[pActivate] = 1;
    api->process_event(niagaraCDO, fSpawn, p);
    uintptr_t nc = 0;
    memcpy(&nc, p + pSpawnRet, 8);
    return nc;
}

static void setVar(uintptr_t nc, uint64_t name, float value) {
    struct { uint64_t name; float value; } p = {name, value};
    api->process_event(nc, fSetFloat, &p);
}

static uintptr_t rootOf(uintptr_t v) {
    uintptr_t root = 0;
    api->process_event(v, fRoot, &root);
    return root;
}

// World to root space.
static Vec toRoot(uintptr_t root, Vec world) {
    alignas(16) unsigned char p[0xc0] = {0};  // an FTransform in the parameters needs it
    alignas(16) unsigned char t[0x60] = {0};
    api->process_event(root, fToWorld, t);
    memcpy(p + off(fInverse, "T"), t, sizeof t);
    memcpy(p + off(fInverse, "Location"), &world, sizeof world);
    api->process_event(mathCDO, fInverse, p);
    Vec r;
    memcpy(&r, p + off(fInverse, "ReturnValue"), sizeof r);
    return r;
}

// The spots the game made, kept apart from its list (which applySockets edits), and where each
// smoke emitter first sat, so a moved socket moves everything by its offset exactly once.
struct SocketBase { Ref owner; std::vector<Vec> at; };
struct EmitterBase { Ref owner; Vec at; };
static std::map<uintptr_t, SocketBase> socketBase;
static std::map<uintptr_t, EmitterBase> emitterBase;
static int oSceneRel = -1;
static uintptr_t fMeshBox;
static int oStaticMesh = -1, oChildren = -1, oMeshSockets = -1, oSockName = -1, oSockLoc = -1, oSockRot = -1;

// The body: the root or a component under it (two levels) whose mesh is named Body, else the biggest
// mesh there. Its box, in root space (child offsets added; cars do not turn their body).
static Vec aim(const Pipe& p);
static void spinOf(double pitch, double yaw, double roll, double* q) {  // FRotator::Quaternion
    const double r = 3.14159265358979 / 360;
    double sp = sin(pitch * r), cp = cos(pitch * r), sy = sin(yaw * r), cy = cos(yaw * r), sr = sin(roll * r), cr = cos(roll * r);
    q[0] = cr * sp * sy - sr * cp * cy;
    q[1] = -cr * sp * cy - sr * cp * sy;
    q[2] = cr * cp * sy - sr * sp * cy;
    q[3] = cr * cp * cy + sr * sp * sy;
}

// Also the meshes' own exhaust sockets (a body or bumper can carry them; the game's smoke list then
// stays empty), in root space, into `tips`.
static bool bodyBox(uintptr_t root, Vec& lo, Vec& hi, std::string& said, std::vector<Pipe>* tips = nullptr) {
    if (!fMeshBox || oStaticMesh < 0) return false;
    struct Found { uintptr_t mesh; Vec shift; double size; bool body; };
    Found best{};
    auto look = [&](uintptr_t c, Vec shift) {
        if (!c || !api->is_a(classOf(c), cMeshComp)) return;
        uintptr_t mesh = get<uintptr_t>(c + oStaticMesh);
        if (!mesh) return;
        unsigned char b[0x40] = {0};
        api->process_event(mesh, fMeshBox, b);
        Vec l, h;
        memcpy(&l, b + off(fMeshBox, "ReturnValue"), sizeof l);
        memcpy(&h, b + off(fMeshBox, "ReturnValue") + sizeof(Vec), sizeof h);
        double size = (h.x - l.x) * (h.y - l.y);
        bool body = objName(mesh) == "Body";
        char line[160];
        snprintf(line, sizeof line, " %s at %.0f,%.0f,%.0f box %.0fx%.0fx%.0f", objName(mesh).c_str(), shift.x, shift.y, shift.z, h.x - l.x, h.y - l.y, h.z - l.z);
        said += line;
        uintptr_t list = oMeshSockets >= 0 ? get<uintptr_t>(mesh + oMeshSockets) : 0;
        int count = list ? get<int>(mesh + oMeshSockets + 8) : 0;
        for (int i = 0; i < count && i < 64; i++) {
            uintptr_t k = get<uintptr_t>(list + i * 8ull);
            char name[96] = {0};
            if (!k || !api->fname(k + oSockName, name, sizeof name)) continue;
            said += i ? ", " : " sockets ";
            said += name;
            std::string low = name;
            for (auto& ch : low) ch = (char)tolower(ch);
            if (!tips || (low.find("exhaust") == std::string::npos && low.find("muffler") == std::string::npos)) continue;
            Vec at = get<Vec>(k + oSockLoc), rot = get<Vec>(k + oSockRot);  // FRotator: pitch, yaw, roll
            Pipe p{{at.x + shift.x, at.y + shift.y, at.z + shift.z}, {0, 0, 1, 0}, 5};
            spinOf(rot.x, rot.y, rot.z, p.spin);
            Vec d = aim(p);
            if (d.x > -0.3) { p.spin[0] = 0; p.spin[1] = 0; p.spin[2] = 1; p.spin[3] = 0; }  // not blowing back: their X is not the pipe's
            tips->push_back(p);
        }
        said += ";";
        if (size > 0 && (body > best.body || (body == best.body && size > best.size))) {
            best = {mesh, shift, size, body};
            lo = {l.x + shift.x, l.y + shift.y, l.z + shift.z};
            hi = {h.x + shift.x, h.y + shift.y, h.z + shift.z};
        }
    };
    auto children = [&](uintptr_t c, auto& self, Vec shift, int depth) -> void {
        look(c, shift);
        if (depth == 2 || oChildren < 0) return;
        uintptr_t data = get<uintptr_t>(c + oChildren);
        int n = get<int>(c + oChildren + 8);
        for (int i = 0; data && i < n && i < 64; i++) {
            uintptr_t k = get<uintptr_t>(data + i * 8ull);
            Vec rel = k && oSceneRel >= 0 ? get<Vec>(k + oSceneRel) : Vec{};
            if (k) self(k, self, Vec{shift.x + rel.x, shift.y + rel.y, shift.z + rel.z}, depth + 1);
        }
    };
    children(root, children, Vec{}, 0);
    return best.mesh != 0;
}

// shortcut: engine+oPipes-0x10 is an unreflected TArray<FTransform> beside ExhaustBlackSmokes;
// the bounds check drops it if an update moves it, and the next source takes over.
static Vec aim(const Pipe& p);
static void readPipes(uintptr_t v, uintptr_t engine, Car& car) {
    if ((int)(GetTickCount() - car.readAgain) < 0) return;  // a guess just failed: the car is still being set up
    car.read = true;
    car.pipes.clear();
    uintptr_t list = get<uintptr_t>(engine + oPipes - 0x10), root = rootOf(v);
    int n = get<int>(engine + oPipes - 8);
    // The game's list is where applySockets writes the moved spots, so the spots the game made are
    // kept apart, per engine, the first time it is read: read back, the list would add the offset
    // again each time this car is picked up.
    SocketBase& kept = socketBase[engine];
    if (!alive(kept.owner) || (int)kept.at.size() != n) {
        kept.owner = {get<int>(engine + L().obj_index), engine};
        kept.at.assign(n > 0 && n <= 16 ? n : 0, Vec{});
        for (int i = 0; list && i < (int)kept.at.size(); i++) rd(list + i * 0x60ull + 0x20, &kept.at[i], sizeof(Vec));
    }
    for (int i = 0; list && n > 0 && n <= 16 && i < n; i++) {
        Pipe pipe{};
        double scale = 0;
        rd(list + i * 0x60ull, pipe.spin, sizeof pipe.spin);
        pipe.at = kept.at[i];
        rd(list + i * 0x60ull + 0x40, &scale, sizeof scale);
        pipe.radius = scale * 5;  // what the game gives its own smoke
        if (fabs(pipe.at.x) < 3000 && fabs(pipe.at.y) < 1000 && fabs(pipe.at.z) < 1000 && pipe.radius > 0) car.pipes.push_back(pipe);
    }
    int sockets = (int)car.pipes.size(), tips = 0;
    car.listed = sockets == n ? n : 0;  // only an unfiltered list lines up with the game's smoke, socket for socket
    // Fitted exhaust tips are not followed: anything can stand in for one, so the player places the
    // flames with the pipe offsets, from the sockets or the fixed guess below.
    // A pair low on the rear end, from the body mesh's own box: fixed, in the car's axes. The actor's
    // bounds are the world's axes (a car at an angle got pipes outside itself) and read garbage while
    // the car is being set up: one Spider got 0,0,0 and a point a kilometre off in the same second.
    Vec lo{}, hi{};
    std::string meshes;
    std::vector<Pipe> meshTips;
    const char* guess = "";
    // No list, but the game's own smoke emitters: they sit on its exhausts, wherever it got them from.
    if (car.pipes.empty() && oSceneRel >= 0) {
        std::string at;
        for (int arr : {oPipes, oWhite}) {
            uintptr_t data = arr < 0 ? 0 : get<uintptr_t>(engine + arr);
            int count = data ? get<int>(engine + arr + 8) : 0;
            for (int i = 0; i < count && i < 8; i++)
                if (uintptr_t c = get<uintptr_t>(data + i * 8ull)) {
                    EmitterBase& b = emitterBase[c];  // where the game first put it, not where applySockets moved it
                    Vec p = alive(b.owner) ? b.at : get<Vec>(c + oSceneRel);
                    char s[64];
                    snprintf(s, sizeof s, " %.0f,%.0f,%.0f", p.x, p.y, p.z);
                    at += s;
                    if (arr == oPipes && (p.x != 0 || p.y != 0 || p.z != 0)) car.pipes.push_back({p, {0, 0, 1, 0}, 5});  // unplaced ones sit at 0
                }
        }
        static int said;
        if (!at.empty() && said < 30) { said++; MT_LOG("%s: no socket list, smoke emitters at%s", objName(classOf(v)).c_str(), at.c_str()); }
        if (!car.pipes.empty()) guess = " (the game's smoke emitters)";
    }
    if (car.pipes.empty() && root && bodyBox(root, lo, hi, meshes, &meshTips)) {
        static int said;
        if (said < 30) { said++; MT_LOG("%s has no exhaust sockets in the smoke list; meshes under its root:%s", objName(classOf(v)).c_str(), meshes.c_str()); }
        if (!meshTips.empty()) {
            car.pipes = meshTips;
            guess = " (the meshes' exhaust sockets)";
        } else if (hi.x - lo.x > 50 && hi.y - lo.y > 50) {
            guess = " (guessed from the body mesh)";
            double midZ = (lo.z + hi.z) / 2, halfZ = (hi.z - lo.z) / 2, halfY = (hi.y - lo.y) / 2, midY = (lo.y + hi.y) / 2;
            car.pipes.push_back({{lo.x, midY + halfY * 0.5, midZ - halfZ * 0.4}, {0, 0, 1, 0}, 5});
            car.pipes.push_back({{lo.x, midY - halfY * 0.5, midZ - halfZ * 0.4}, {0, 0, 1, 0}, 5});
        }
    }
    if (car.pipes.empty()) {  // a pair low on the rear end; the long side of the box is the length
        guess = " (from the world box)";
        unsigned char b[0x60] = {0};
        b[0] = 1;  // bOnlyCollidingComponents
        api->process_event(v, fBounds, b);
        Vec ext, origin;
        memcpy(&ext, b + pBoundsExtent, sizeof ext);
        memcpy(&origin, b + pBoundsOrigin, sizeof origin);
        Vec mid = toRoot(root, origin);
        double length = ext.x > ext.y ? ext.x : ext.y, width = ext.x > ext.y ? ext.y : ext.x;
        car.pipes.push_back({{mid.x - length, width * 0.5, mid.z - ext.z * 0.4}, {0, 0, 1, 0}, 5});
        car.pipes.push_back({{mid.x - length, -width * 0.5, mid.z - ext.z * 0.4}, {0, 0, 1, 0}, 5});
    }
    for (const Pipe& p : car.pipes)  // a guess off the car (bounds read mid set-up): no pipes, read again next time
        if (!(fabs(p.at.x) < 1500 && fabs(p.at.y) < 400 && fabs(p.at.z) < 400) || (p.at.x == 0 && p.at.y == 0 && p.at.z == 0)) {
            static int said;
            if (said < 20) { said++; MT_LOG("%s: guessed a pipe at %.0f %.0f %.0f, off the car: reading again later", objName(classOf(v)).c_str(), p.at.x, p.at.y, p.at.z); }
            car.pipes.clear();
            car.read = false;
            car.readAgain = GetTickCount() + 3000;
            break;
        }
    car.slots.resize(car.pipes.size());
    Vec d = car.pipes.empty() ? Vec{} : aim(car.pipes[0]);
    Vec p0 = car.pipes.empty() ? Vec{} : car.pipes[0].at;
    char sig[64];
    snprintf(sig, sizeof sig, "%d %.0f %.0f %.0f", (int)car.pipes.size(), p0.x, p0.y, p0.z);
    if (car.pipeSig == sig) return;  // re-read the same: said already (guesses are re-read every 5 s)
    car.pipeSig = sig;
    MT_LOG("%s: %d sockets, %d fitted tips, firing from %d pipes%s, the first at %.0f %.0f %.0f aimed %.2f %.2f %.2f (up %.0f deg)", objName(classOf(v)).c_str(), sockets, tips,
           (int)car.pipes.size(), guess, p0.x, p0.y, p0.z, d.x, d.y, d.z, asin(d.z < -1 ? -1 : d.z > 1 ? 1 : d.z) * 57.3);
}

// The way the pipe blows: a socket's X.
static Vec aim(const Pipe& p) {
    const double x = p.spin[0], y = p.spin[1], z = p.spin[2], w = p.spin[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};
}

// The game's exhaust sockets are aimed for its smoke, tilted up so it rises; fire leaves along
// the pipe. Up is taken out of the aim (down is kept, for pipes that point at the road).
static Vec flat(Vec d) {
    if (d.z > 0) d.z = 0;
    double n = sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    return n > 1e-6 ? Vec{d.x / n, d.y / n, d.z / n} : Vec{-1, 0, 0};
}

static double Tune::* const PIPE_ON[4] = {&Tune::p1, &Tune::p2, &Tune::p3, &Tune::p4};
static double Tune::* const PIPE_X[4] = {&Tune::x1, &Tune::x2, &Tune::x3, &Tune::x4};
static double Tune::* const PIPE_Y[4] = {&Tune::y1, &Tune::y2, &Tune::y3, &Tune::y4};
static double Tune::* const PIPE_Z[4] = {&Tune::z1, &Tune::z2, &Tune::z3, &Tune::z4};

// Socket i where the tune puts it.
static Pipe movedSocket(const Car& car, const Tune& t, int i) {
    Pipe p = car.pipes[i];
    if (i < 4) p.at = {p.at.x + t.*PIPE_X[i], p.at.y + t.*PIPE_Y[i], p.at.z + t.*PIPE_Z[i]};
    return p;
}

static bool socketOn(const Car& car, const Tune& t, int i) {
    if (i >= 4) return true;
    if (t.*PIPE_ON[i] != 0) return true;
    for (int k = 0; k < (int)car.pipes.size() && k < 4; k++)  // all off: the first stays on
        if (t.*PIPE_ON[k] != 0) return false;
    return i == 0;
}

// The sockets that are on, each where the tune puts it.
static std::vector<Pipe> socketPoints(const Car& car, const Tune& t) {
    std::vector<Pipe> out;
    for (int i = 0; i < (int)car.pipes.size(); i++)
        if (socketOn(car, t, i)) out.push_back(movedSocket(car, t, i));
    return out;
}

// The game's own smoke (black for damage and diesel, white) is one emitter per socket, and the
// socket list is what anything else reads. An enabled socket's emitters go where the tune puts it;
// a disabled one's go onto the first enabled pipe, so all the smoke leaves by the pipes that are
// on. Moved, not removed: the game keeps its list the length it made it.
static void stopSmoke(Car& car);
static void setRelLoc(uintptr_t c, const Vec& at) {
    unsigned char p[0x120] = {0};
    memcpy(p + oRelLocAt, &at, sizeof at);
    api->process_event(c, fRelLoc, p);
}

static void applySockets(uintptr_t v, uintptr_t engine, Car& car, const Tune& t, DWORD now) {
    if ((int)(now - car.sockAt) < 0) return;
    car.sockAt = now + 500;
    // The game rebuilt its list (a part changed, another engine): the spots kept from before are
    // another layout's, and offsetting them put the pipes somewhere else. Read it again.
    uintptr_t now_list = get<uintptr_t>(engine + oPipes - 0x10);
    int now_n = get<int>(engine + oPipes - 8);
    const std::vector<Vec>& expect = car.wrote.empty() ? socketBase[engine].at : car.wrote;
    bool rebuilt = car.read && (engine != car.engine || (now_list && now_n > 0 && now_n <= 16 && now_n != (int)expect.size()));
    for (int i = 0; car.read && !rebuilt && now_list && i < now_n && i < (int)expect.size(); i++) {
        Vec at = get<Vec>(now_list + i * 0x60ull + 0x20);
        rebuilt = fabs(at.x - expect[i].x) + fabs(at.y - expect[i].y) + fabs(at.z - expect[i].z) > 0.5;
    }
    if (rebuilt) {
        static int said;
        if (said < 20) { said++; MT_LOG("%s: the game rebuilt its exhaust sockets, reading them again", objName(classOf(v)).c_str()); }
        socketBase.erase(engine);
        car.read = false;
        car.sockSig.clear();
        car.wrote.clear();
    }
    car.engine = engine;
    if (!car.read) readPipes(v, engine, car);
    if (!car.listed || !fRelLoc || oRelLocAt < 0) return;
    int first = 0;
    while (first < car.listed - 1 && !socketOn(car, t, first)) first++;
    std::vector<Vec> to(car.listed);
    std::string sig;
    for (int i = 0; i < car.listed; i++) {
        to[i] = movedSocket(car, t, socketOn(car, t, i) ? i : first).at;
        char b[64];
        snprintf(b, sizeof b, "%.1f,%.1f,%.1f;", to[i].x, to[i].y, to[i].z);
        sig += b;
    }
    if (sig == car.sockSig) return;
    car.sockSig = sig;
    uintptr_t list = get<uintptr_t>(engine + oPipes - 0x10);
    if (list && get<int>(engine + oPipes - 8) == car.listed) {
        for (int i = 0; i < car.listed; i++) wr(list + i * 0x60ull + 0x20, &to[i], sizeof to[i]);
        car.wrote = to;
    }
    for (int arr : {oPipes, oWhite}) {
        if (arr < 0) continue;
        uintptr_t data = get<uintptr_t>(engine + arr);
        int n = get<int>(engine + arr + 8);
        // Each emitter moves by the socket's offset from where the game first put it: it may sit
        // on the socket itself, so its own location is not the socket's.
        for (int i = 0; data && i < n && i < car.listed; i++)
            if (uintptr_t c = get<uintptr_t>(data + i * 8ull)) {
                EmitterBase& b = emitterBase[c];
                if (!alive(b.owner)) { b.owner = {get<int>(c + L().obj_index), c}; b.at = oSceneRel >= 0 ? get<Vec>(c + oSceneRel) : Vec{}; }
                const Vec& from = car.pipes[i].at;
                setRelLoc(c, {b.at.x + to[i].x - from.x, b.at.y + to[i].y - from.y, b.at.z + to[i].z - from.z});
            }
    }
    if (!car.smokes.empty()) stopSmoke(car);  // the plugin's own diesel smoke starts again at the new spots
}

// Where a burst fires from: the sockets, or with Twin tips each socket as two tips either side
// of it, level and across the way it blows. Many twin exhausts have one smoke socket placed
// between their two tips. Slots only ever grow, so turning Twin tips off leaves nothing showing.
static void firePoints(Car& car, const Tune& t) {
    car.fire.clear();
    for (const Pipe& p : socketPoints(car, t)) {
        if (t.twin == 0) { car.fire.push_back(p); continue; }
        Vec d = aim(p);
        double n = sqrt(d.x * d.x + d.y * d.y);
        Vec side = n > 0.01 ? Vec{d.y / n, -d.x / n, 0} : Vec{0, 1, 0};
        for (int k = -1; k <= 1; k += 2) {
            Pipe tip = p;
            tip.at = {p.at.x + side.x * t.gap * k, p.at.y + side.y * t.gap * k, p.at.z};
            tip.radius = p.radius * 0.7;
            car.fire.push_back(tip);
        }
    }
    if (car.slots.size() < car.fire.size()) car.slots.resize(car.fire.size());
}

// Every tip's own light was the GPU's cost, so tips close together share one, between them. Tips
// more than 60 cm apart across the car (one each side) get one light a side: a single one in the
// middle sat behind the bumper, away from either flame.
// Returns the light's share of the car's power (1, or 1/2 a side), 0 when tip i carries none: two at
// full power doubled the light on the bumper, and a see-through flame dimmer than what is lit behind it
// darkens it -- black flames again.
static double lightSpot(const Car& car, int i, Vec& at) {
    if (i >= (int)car.fire.size()) return 0;
    double lo = 1e9, hi = -1e9;
    for (const Pipe& f : car.fire) { lo = f.at.y < lo ? f.at.y : lo; hi = f.at.y > hi ? f.at.y : hi; }
    double mid = (lo + hi) / 2;
    bool split = hi - lo > 60;
    auto side = [&](int k) { return !split || (car.fire[k].at.y >= mid) == (car.fire[i].at.y >= mid); };
    for (int k = 0; k < i; k++)
        if (side(k)) return 0;  // an earlier tip on this side carries it
    at = {};
    int n = 0;
    for (int k = 0; k < (int)car.fire.size(); k++)
        if (side(k)) { at.x += car.fire[k].at.x; at.y += car.fire[k].at.y; at.z += car.fire[k].at.z; n++; }
    at = {at.x / n, at.y / n, at.z / n};
    return split ? 0.5 : 1;
}

// FQuat::Rotator, in degrees.
static Vec rotatorOf(const Pipe& p) {
    const double x = p.spin[0], y = p.spin[1], z = p.spin[2], w = p.spin[3], deg = 180 / 3.14159265358979;
    return {asin(2 * (z * x - w * y)) * deg, atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)) * deg,
            atan2(-2 * (w * x + y * z), 1 - 2 * (x * x + y * y)) * deg};
}

static Ref refOf(uintptr_t o) { return {o ? get<int>(o + L().obj_index) : 0, o}; }

// The garage screen, caught as the game builds it (its Construct goes through ProcessEvent), so the
// button does not wait for a scan. Its own lock: the tick makes widgets, and their Construct lands here.
static uintptr_t gScreenClass;
static std::mutex builtLock;
static std::vector<Ref> built;
static int onBuilt(uintptr_t obj, uintptr_t, void*, void*) {
    if (gScreenClass && obj && api->is_a(classOf(obj), gScreenClass)) { std::lock_guard<std::mutex> g(builtLock); built.push_back(refOf(obj)); }
    return 0;
}

// New components a frame, across every pool, light and car: making one (register, render proxy,
// material instance) is the heaviest thing a flame does, and a lift used to make 16-28 in its first
// frame. What is over waits a frame; a callers' missing piece is simply not drawn yet.
static int makeBudget = 4;
// Pieces that appear a frame: after a quiet spell every piece on every tip used to appear in the same
// frame (80 with twin tips), each with its colour and a visibility change. The rest follow a frame on.
static int showBudget = 16;

static uintptr_t makeMesh(uintptr_t v, uintptr_t mesh, uintptr_t mat, const Pipe& pipe, float r, float g, float b) {
    lastMid = 0;
    if (makeBudget <= 0) return 0;
    makeBudget--;
    alignas(16) unsigned char p[0x100] = {0};
    int at = off(fAddComp, "RelativeTransform");
    // The meshes run along Z: this lays them down the pipe's X (its rotation after a quarter turn on Y).
    const double s = 0.70710678, *q = pipe.spin;
    double spin[4] = {(q[0] - q[2]) * s, (q[1] + q[3]) * s, (q[2] + q[0]) * s, (q[3] - q[1]) * s};
    double scale[3] = {0.001, 0.001, 0.001};
    memcpy(p + off(fAddComp, "Class"), &cMeshComp, 8);
    memcpy(p + at, spin, sizeof spin);
    memcpy(p + at + 0x40, scale, sizeof scale);
    p[off(fAddComp, "bDeferredFinish")] = 1;
    api->process_event(v, fAddComp, p);
    uintptr_t c = 0;
    memcpy(&c, p + off(fAddComp, "ReturnValue"), 8);
    if (!c) return 0;
    uint8_t movable = 2, none = 0;  // a static mesh defaults to Static, which cannot ride a car
    api->process_event(c, fMobility, &movable);
    // Before it attaches: with collision on, the car's physics body would be rebuilt around it.
    api->process_event(c, fCollision, &none);
    api->process_event(c, fShadow, &none);
    unsigned char m[0x10] = {0};
    memcpy(m, &mesh, 8);
    api->process_event(c, fMesh, m);
    alignas(16) unsigned char f[0x100] = {0};
    neverCull(c);  // else the maps' cull-distance volumes drop it a little way off
    memcpy(f + off(fFinish, "Component"), &c, 8);
    memcpy(f + off(fFinish, "RelativeTransform"), p + at, 0x60);
    api->process_event(v, fFinish, f);
    unsigned char d[0x30] = {0};
    memcpy(d + off(fMakeMid, "SourceMaterial"), &mat, 8);
    api->process_event(c, fMakeMid, d);
    uintptr_t mid = 0;
    memcpy(&mid, d + off(fMakeMid, "ReturnValue"), 8);
    if (mid && mat == ghost) {
        struct { uint64_t name; float rgba[4]; } color = {colorName, {r * flameGain(), g * flameGain(), b * flameGain(), 1}};
        api->process_event(mid, fMidColor, &color);
        struct { uint64_t name; float value; } clear = {opacityName, 0};
        api->process_event(mid, fMidScalar, &clear);
        // M_GhostCar draws as if further back (it is made to sit over a car), so the road just
        // below the pipe covered the lower half of every flame: pulled towards the camera instead.
        struct { uint64_t name; float value; } flat = {depthBiasName, kDepthBias};
        if (depthBiasName) api->process_event(mid, fMidScalar, &flat);
    } else if (mid) {
        struct { uint64_t name; float rgba[4]; } color = {lightColorName, {r, g, b, 1}};
        api->process_event(mid, fMidColor, &color);
        struct { uint64_t name; float value; } power = {lightPowerName, 8};
        api->process_event(mid, fMidScalar, &power);
    }
    lastMid = mid;
    return c;
}

static void showMesh(const Ref& r, bool on) {
    if (!alive(r)) return;
    struct { bool visible, propagate; } p = {on, false};
    api->process_event(r.ptr, fMeshVis, &p);
}

static void placeMesh(const Ref& r, const Vec& at, double width, double length) {
    Vec size{width, width, length < 0.01 ? 0.01 : length};
    api->process_event(r.ptr, fRelScale, &size);
    unsigned char p[0x120] = {0};
    memcpy(p + oRelLocAt, &at, sizeof at);
    api->process_event(r.ptr, fRelLoc, p);
}

// One per pipe, no shadows: it lights the road and the bumper around the flame.
static uintptr_t makeLight(uintptr_t v, double radius) {
    if (makeBudget <= 0) return 0;
    makeBudget--;
    alignas(16) unsigned char p[0x100] = {0};
    int at = off(fAddComp, "RelativeTransform");
    double spin[4] = {0, 0, 0, 1}, scale[3] = {1, 1, 1};
    memcpy(p + off(fAddComp, "Class"), &cLight, 8);
    memcpy(p + at, spin, sizeof spin);
    memcpy(p + at + 0x40, scale, sizeof scale);
    p[off(fAddComp, "bDeferredFinish")] = 1;
    api->process_event(v, fAddComp, p);
    uintptr_t c = 0;
    memcpy(&c, p + off(fAddComp, "ReturnValue"), 8);
    if (!c) return 0;
    uint8_t movable = 2, lumens = 2;  // ELightUnits::Lumens, the unit vanilla's attachment lights use
    unsigned char shadows[8] = {0};
    float reach = (float)radius;
    api->process_event(c, fMobility, &movable);
    api->process_event(c, fLightShadows, shadows);
    api->process_event(c, fLightUnits, &lumens);
    api->process_event(c, fLightRadius, &reach);
    alignas(16) unsigned char f[0x100] = {0};
    neverCull(c);  // else the maps' cull-distance volumes drop it a little way off
    memcpy(f + off(fFinish, "Component"), &c, 8);
    memcpy(f + off(fFinish, "RelativeTransform"), p + at, 0x60);
    api->process_event(v, fFinish, f);
    return c;
}

// Culling. The camera is read once a frame (tickFlames); a flame behind it is hidden and not
// worked on, and from 25 m out it thins with distance: fewer streaks, fewer layers, colour
// written less often. 0 = culled, else the share of detail kept.
static uintptr_t fCamMgr, fCamLoc, fCamRot, fSetXform;
static int oXform = -1;
static Vec camAt, camFwd;
static bool haveCam;

static void readCamera(uintptr_t context) {
    struct { uintptr_t context; int index; uintptr_t ret; } who = {context, 0, 0};
    haveCam = false;
    if (!context || !fCamMgr || !fCamLoc || !fCamRot) return;
    api->process_event(gameplayCDO, fCamMgr, &who);
    if (!who.ret) return;
    camAt = callVec(who.ret, fCamLoc);
    Vec r = callVec(who.ret, fCamRot);  // pitch, yaw, roll in degrees
    double p = r.x * 3.14159265 / 180, y = r.y * 3.14159265 / 180;
    camFwd = {cos(p) * cos(y), cos(p) * sin(y), sin(p)};
    haveCam = true;
}

static double view(const Slot& s) {
    if (!haveCam || !alive(s.owner)) return 1;
    Vec at = callVec(s.owner.ptr, fLoc);
    double dx = at.x - camAt.x, dy = at.y - camAt.y, dz = at.z - camAt.z, d = sqrt(dx * dx + dy * dy + dz * dz);
    if (d < 800) return 1;  // close up a car's length can be on screen while its middle is not
    if ((dx * camFwd.x + dy * camFwd.y + dz * camFwd.z) / d < 0.3) return 0;
    return d < 2500 ? 1 : 2500 / d < 0.25 ? 0.25 : 2500 / d;
}

// The four ways a pipe fires, each with its own colour and character. Colours run root -> body
// -> tip. Blue is not a kind: it is heat, so it comes only from high revs (see lookRgb).
enum { DECEL, BANG, RICH, ALS };
struct Look { double size, reach, tongues, wobble, pace, lasts, heat; };
static const Look LOOKS[4] = {
    {.6, .55, 1.5, 1.2, 1.3, 0.8, 0},       // lift-off crackle: small, quick
    {1.3, 1, 3, .6, 1.8, 0.6, 0.1},         // gunshot: a big dump all at once, burns hot
    {.9, .7, 2, 1.8, .6, 1.4, -0.25},       // rich, low revs: lazy, sooty, cool
    {1.15, 1.3, 3, 1, 1, 1, -0.3}};         // anti-lag: fuel burnt late on purpose, cool and yellow

static double smooth(double from, double to, double x) {
    double t = (x - from) / (to - from);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

// Colour is temperature and how the fuel burns, the two things a real exhaust flame shows:
//  - blue is the light of the burning itself (CH and C2 radicals), from fuel already mixed with
//    air and burning fast and hot, past about 1500 C: a lean or advanced tune, a cut that pushes
//    a fresh charge into a hot pipe;
//  - yellow, orange and red are soot glowing at the temperature it is at (a black body): fuel
//    burning only where it meets the air at the tip, too rich to burn clean, 1000-1300 C.
// Hot enough, the glow and the blue together are brighter than the eye takes: white.
// The temperatures come from the pipe (exhaust(): the gas worked out section by section from the
// manifold to the tip), then along the flame: hottest just past the mouth where the rest burns,
// cooling as it draws in cold air, from the axis out, and over the burst's life.
static void blackbody(double K, double* c) {  // a black body's colour, 0..1 a channel (Helland's fit)
    double t = K / 100;
    c[0] = 1;
    c[1] = t <= 66 ? (99.4708025861 * log(t) - 161.1195681661) / 255 : 1;
    c[2] = t <= 19 ? 0 : t >= 66 ? 1 : (138.5177312231 * log(t - 10) - 305.0447927307) / 255;
    for (int i = 1; i < 3; i++) c[i] = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
}

static double flameK(const Glow& g, double along, double out, double u) {
    double T = g.mouthK + (g.peakK - g.mouthK) * smooth(0, 0.15, along);
    double cool = along > 0.15 ? exp(-1.3 * (along - 0.15)) : 1;
    return 300 + (T - 300) * cool * (1 - 0.35 * out) * (1 - 0.45 * u);
}

// along: 0 at the pipe, 1 at a tip; out: 0 on the axis, 1 at the edge; u: share of the burst's life.
static void heatRgb(const Slot& s, double along, double out, double u, float* rgb) {
    static const double BLUE[3] = {0.03, 0.12, 1};  // CH and C2 light: a deep, saturated blue
    const Glow& g = s.flameHue;
    double T = flameK(g, along < 0 ? 0 : along > 1 ? 1 : along, out, u), bb[3], c[3], most = 0;
    blackbody(T, bb);
    double blue = 1.1 * g.premix * smooth(1200, 1800, T);
    double soot = (0.05 + 0.95 * pow(1 - g.premix, 1.5)) * pow(T / 1600, 5);  // glows as about T^5 in the visible; premixed makes little
    for (int i = 0; i < 3; i++) {
        c[i] = 1 - exp(-1.3 * (BLUE[i] * blue + bb[i] * soot));  // past what the eye takes, white
        most = c[i] > most ? c[i] : most;
    }
    // Mostly the hue, so a cool tip stays red, not black; but part of the brightness follows the
    // light, so a blue burn darkens as it cools down its path instead of staying a pale glow.
    // Not below 70%: the flame blends over what is behind it, so a dim red piece darkened the road
    // and the whole flame read as black smoke.
    double level = 0.7 + 0.3 * most;
    for (int i = 0; i < 3; i++) rgb[i] = (float)((most > 1e-4 ? c[i] / most : bb[i]) * level);
    // The look wanted, not the physics: blue, green, orange and red, never purple (blue light over
    // red soot) and never washed out to white. Purple hues go to the nearer of blue or red, and the
    // weakest channel is held down so every colour stays saturated.
    double r = rgb[0], gr = rgb[1], b = rgb[2], hi = r > gr ? (r > b ? r : b) : (gr > b ? gr : b), lo = r < gr ? (r < b ? r : b) : (gr < b ? gr : b);
    if (hi <= 0) return;
    double h = 0, d = hi - lo;  // hue in degrees
    if (d > 1e-6) h = hi == r ? 60 * fmod((gr - b) / d + 6, 6) : hi == gr ? 60 * ((b - r) / d + 2) : 60 * ((r - gr) / d + 4);
    double sat = d / hi;
    if (h > 240 && h < 345) h = h < 292 ? 235 : 0;
    if (sat < 0.75) sat = 0.75;
    double cc = hi * sat, x = cc * (1 - fabs(fmod(h / 60, 2) - 1)), m = hi - cc, o[3];
    int k = (int)(h / 60) % 6;
    const double table[6][3] = {{cc, x, 0}, {x, cc, 0}, {0, cc, x}, {0, x, cc}, {x, 0, cc}, {cc, 0, x}};
    for (int i = 0; i < 3; i++) o[i] = table[k][i] + m;
    for (int i = 0; i < 3; i++) rgb[i] = (float)o[i];
}

static void lookRgb(const Slot& s, double along, double u, float* rgb) { heatRgb(s, along, 0, u, rgb); }

// The light a flame casts: yellow-orange only, whatever colour the flame itself shows. Yellow while
// a hard burst is fresh, orange as it dies.
static void lightRgb(const Slot& s, double u, float* rgb) {
    double hot = s.energy * (1 - u);
    hot = hot < 0 ? 0 : hot > 1 ? 1 : hot;
    rgb[0] = 1;
    rgb[1] = (float)(0.42 + 0.38 * hot);
    rgb[2] = (float)(0.08 + 0.22 * hot);
}

// alpha >= 0: a see-through segment on M_GhostCar, whose colour is its whole output, pushed past
// 1 to bloom; else the opaque M_AttachmentLight, coloured by Light Color.
static void paint(uintptr_t mid, const float* rgb, double alpha) {
    if (!mid) return;
    float gain = alpha >= 0 ? flameGain() : 1;
    // Kept see-through so overlapping pieces mix into one body; doubled, each one covered the next.
    struct { uint64_t name; float rgba[4]; } color = {alpha >= 0 ? colorName : lightColorName, {rgb[0] * gain, rgb[1] * gain, rgb[2] * gain, alpha >= 0 ? solid(alpha) : 1}};
    api->process_event(mid, fMidColor, &color);
    if (alpha < 0) return;
    struct { uint64_t name; float value; } fade = {opacityName, solid(alpha)};
    api->process_event(mid, fMidScalar, &fade);
}

static void setVis(const Ref& r, bool& state, bool want) {
    if (state == want) return;
    state = want;
    showMesh(r, want);
}

// Between bursts a piece is shrunk to nothing, not hidden: a visibility change rebuilds its render
// state, and a crackling lift flipped every piece twice a burst. Hidden for real once the pipe has
// been quiet for a second.
static void park(const Ref& r) {
    if (!alive(r)) return;
    Vec none{1e-4, 1e-4, 1e-4};
    api->process_event(r.ptr, fRelScale, &none);
}
static void parkSeg(Tongue& g, int c) {
    if (g.vis[c] && !g.parked[c]) { park(g.seg[c]); g.parked[c] = true; }
}
static void parkFlame(Slot& s) {
    for (Tongue& g : s.tongue)
        for (int c = 0; c < MAX_SEGS; c++) parkSeg(g, c);
    if (s.coreVis && !s.coreParked) { park(s.core); s.coreParked = true; }
}
static void dimLight(Slot& s) {
    float none = 0;
    if (alive(s.light)) api->process_event(s.light.ptr, fIntensity, &none);
}

static void hideFlame(Slot& s) {
    for (Tongue& g : s.tongue)
        for (int c = 0; c < MAX_SEGS; c++) setVis(g.seg[c], g.vis[c], false);
    setVis(s.core, s.coreVis, false);
}

// A segment laid from its centre along d (unit), one call: location, turn and scale together.
// A piece fatter than a third of its length reads as a bulb, not fire: it is drawn slender, at the
// same volume, so it reaches further instead (the classic streaks already are).
static void slim(double& width, double& length) {
    if (width * 3 <= length) return;
    width = cbrt(width * width * length / 3);
    length = 3 * width;
}

static void placeSeg(const Ref& r, const Vec& at, const Vec& d, double width, double length);

// A flame piece: drawn slender, and never reaching back past the pipe's mouth into the pipe or the
// bumper round it. Its back end is half its length (the sphere is 100 cm across at scale 1) behind it.
static Vec pieceAt(const Slot& s, Vec at, const Vec& d, double length) {
    Vec back{at.x - d.x * length * 50, at.y - d.y * length * 50, at.z - d.z * length * 50};
    double behind = (back.x - s.pipe.at.x) * s.dir.x + (back.y - s.pipe.at.y) * s.dir.y + (back.z - s.pipe.at.z) * s.dir.z;
    if (behind < 0) at = {at.x - s.dir.x * behind, at.y - s.dir.y * behind, at.z - s.dir.z * behind};
    return at;
}
static void placePiece(const Slot& s, const Ref& r, const Vec& at, const Vec& d, double width, double length) {
    slim(width, length);
    placeSeg(r, pieceAt(s, at, d, length), d, width, length);
}

static void placeSeg(const Ref& r, const Vec& at, const Vec& d, double width, double length) {
    double w = 1 + d.z, q[4] = {1, 0, 0, 0};  // the shortest turn from the mesh's Z onto d
    if (w > 0.001) { double m = sqrt(d.y * d.y + d.x * d.x + w * w); q[0] = -d.y / m; q[1] = d.x / m; q[2] = 0; q[3] = w / m; }
    if (fSetXform && oXform >= 0) {
        alignas(16) unsigned char p[0x400] = {0};
        double scale[3] = {width, width, length < 0.005 ? 0.005 : length};
        memcpy(p + oXform, q, sizeof q);
        memcpy(p + oXform + 0x20, &at, sizeof at);
        memcpy(p + oXform + 0x40, scale, sizeof scale);
        api->process_event(r.ptr, fSetXform, p);
        return;
    }
    Pipe laid{};
    memcpy(laid.spin, q, sizeof q);
    Vec rotation = rotatorOf(laid);
    unsigned char turn[0x120] = {0};
    memcpy(turn + oRelRotAt, &rotation, sizeof rotation);
    api->process_event(r.ptr, fRelRot, turn);
    placeMesh(r, at, width, length);
}

// Where a tongue is at a (0 pipe .. 1 tip), L its length in cm. All tongues share the throat,
// then bend away after their branch point; the far part licks sideways and rises with the heat.
static Vec pathAt(const Slot& s, const Tongue& g, double a, double L, double time) {
    double ca = cos(g.azimuth), sa = sin(g.azimuth);
    Vec e{s.r1.x * ca + s.r2.x * sa, s.r1.y * ca + s.r2.y * sa, s.r1.z * ca + s.r2.z * sa};
    Vec q{s.r2.x * ca - s.r1.x * sa, s.r2.y * ca - s.r1.y * sa, s.r2.z * ca - s.r1.z * sa};
    double d = a * L, side = d * g.angle * smooth(g.branch, 1, a) * 1.5;
    double lick = s.wobble * L * 0.08 * a * a * sin(time * 22 * s.pace + g.phase + a * 5);
    double lift = L * 0.02 * a * a;  // barely: more and the tips sit visibly above the mouth
    return {s.pipe.at.x + s.dir.x * d + e.x * side + q.x * lick, s.pipe.at.y + s.dir.y * d + e.y * side + q.y * lick,
            s.pipe.at.z + s.dir.z * d + e.z * side + q.z * lick + lift};
}

static void hideClassic(Slot& s) {
    for (Ref& r : s.cone) showMesh(r, false);
    for (Spark& p : s.sparks)
        if (p.shown) { p.live = false; p.shown = false; showMesh(p.c, false); }
}


// ---- More shapes (Flame shape 2..11). Each places up to POOL pieces (the tongue segments) per
// frame, in the pipe's frame: f along the pipe, x/y across it, up the world's up. They take the
// colour of how the pipe fired (LOOKS) like the tongues do. Based on what exhaust flames do:
static const wchar_t* const SHAPE_NAMES[12] = {L"Tongues", L"Classic", L"Diamonds", L"Pulses", L"Lazy lick", L"Blowtorch",
                                                L"Puffs", L"", L"", L"", L"Fan", L"Corkscrew"};  // 7-9 dropped
//                                     Diamonds Pulses Lick Torch Darts Ring Mushroom Sparks Fan Corkscrew
static const double SHAPE_PIECES[12] = {0, 0, 12, 10, 9, 12, 8, 13, 13, 16, 15, 12};
static const double SHAPE_LASTS[12] = {1, 1, 1.3, 1.6, 2.2, 1.5, 1.4, 1.8, 1.6, 2.0, 1.0, 1.3};

struct Piece { Vec at, d; double width, length, along, alpha; };

static Vec inPipe(const Slot& s, double f, double x, double y, double up) {
    return {s.pipe.at.x + s.dir.x * f + s.r1.x * x + s.r2.x * y, s.pipe.at.y + s.dir.y * f + s.r1.y * x + s.r2.y * y,
            s.pipe.at.z + s.dir.z * f + s.r1.z * x + s.r2.z * y + up};
}
static Vec unit(Vec v) { double n = sqrt(v.x * v.x + v.y * v.y + v.z * v.z); return n > 0 ? Vec{v.x / n, v.y / n, v.z / n} : Vec{0, 0, 1}; }
static Vec across(const Slot& s, double f, double x, double y, double up) {
    return unit({s.dir.x * f + s.r1.x * x + s.r2.x * y, s.dir.y * f + s.r1.y * x + s.r2.y * y, s.dir.z * f + s.r1.z * x + s.r2.z * y + up});
}

// A long jet follows the ground, not the pipe's tilt: level, drooping a little at its far end.
static Vec levelDir(const Slot& s) { return unit({s.dir.x, s.dir.y, s.dir.z > 0 ? 0 : s.dir.z}); }
static Vec onLevel(const Slot& s, double f, double L) {
    Vec d = levelDir(s);
    double droop = L > 0 ? 0.06 * f * f / L : 0;
    return {s.pipe.at.x + d.x * f, s.pipe.at.y + d.y * f, s.pipe.at.z + d.z * f - droop};
}

// A chain of m pieces along path(a), a from low to top: each from one point to the next, a
// little longer so they overlap. width/along/alpha are taken at the middle of each piece.
template <class P, class W> static int chain(Piece* out, int m, double low, double top, P path, W look) {
    int n = 0;
    for (int c = 0; c < m; c++) {
        double a0 = low + (top - low) * c / m, a1 = low + (top - low) * (c + 1) / m, am = (a0 + a1) / 2;
        Vec p0 = path(a0 < 0 ? 0 : a0), p1 = path(a1), d{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
        double len = sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        Piece& q = out[n++];
        q.at = {(p0.x + p1.x) / 2, (p0.y + p1.y) / 2, (p0.z + p1.z) / 2};
        q.d = len > 0 ? Vec{d.x / len, d.y / len, d.z / len} : Vec{0, 0, 1};
        q.length = len * 1.3;
        look(am, q);
    }
    return n;
}

static int shapePieces(const Slot& s, double u, double time, double L, double W, double fade, Piece* out) {
    const double PI2 = 6.2831853;
    int n = s.count, made = 0;
    const double (*r)[4] = s.rnd;
    switch (s.shape) {
    case 2: {  // Diamonds: shock diamonds, separate pointed knots in a faint sheath, with gaps between
        int knots = n / 2;
        double jet = L * 1.3 * smooth(0, 0.15, u), gap = jet / (knots ? knots : 1);
        for (int k = 0; k < knots; k++) {
            double f = gap * (k + 0.5), kk = knots > 1 ? k / (double)(knots - 1) : 0;
            // The first knot at its own size, growing to the third of the way, then shrinking to the back.
            double grow = kk < 0.3 ? 1 + 0.35 * kk / 0.3 : 1.35 - 1.05 * (kk - 0.3) / 0.7;
            Piece& a = out[made++];
            a = {onLevel(s, f, L), levelDir(s), W * 0.5 * grow * (0.9 + 0.2 * sin(time * 40 + k)), gap * 0.5 * grow, 0.05 + 0.5 * kk, 0.5 * fade};
            Piece& b = out[made++];
            b = {onLevel(s, f, L), levelDir(s), W * 0.8 * grow, gap * 0.7, 0.35 + 0.5 * kk, 0.1 * fade};
        }
        break;
    }
    case 3: {  // Pulses: a rotary's machine-gun, soft puffs fired down the pipe on a beat, bright and dim in turn
        for (int i = 0; i < n; i++) {
            double born = 0.6 * i / n, age = (u - born) / 0.3;
            if (age < 0 || age > 1) continue;
            double size = W * (0.6 + 0.7 * age) * (i % 2 ? 0.8 : 1.1);
            out[made++] = {onLevel(s, L * 1.2 * age, L), levelDir(s), size, size * 1.7, 0.1 + 0.7 * age, (i % 2 ? 0.45 : 0.8) * (1 - age * age)};
        }
        break;
    }
    case 4: {  // Lazy lick: a slow rich flame that heat lifts and curls, more up than out
        int chains = n >= 9 ? 3 : 2, m = n / chains;
        double top = smooth(0, 0.5, u), low = 0.8 * smooth(0.5, 1, u);
        for (int j = 0; j < chains; j++) {
            double off = (j - (chains - 1) / 2.0) * W * 0.5, ph = r[j][0] * PI2;
            made += chain(out + made, m, low, top,
                [&](double a) { return inPipe(s, L * 0.8 * a, off + W * 1.2 * a * sin(time * 4 + ph + a * 3), W * 0.4 * a * cos(time * 3 + ph), L * 0.15 * a * a); },
                [&](double a, Piece& q) { q.width = W * (1.1 - 0.8 * a); q.along = 0.05 + 0.9 * a; q.alpha = (0.75 - 0.4 * a) * fade; });
        }
        break;
    }
    case 5: {  // Blowtorch: a lean high-rev jet, long and steady, a bright narrow core in a faint sheath
        int m = n / 2;
        double jet = L * 1.5 * smooth(0, 0.12, u), wob = 0.03 * sin(time * 30);
        auto path = [&](double a) { Vec p = onLevel(s, jet * a, L); double w = W * wob * a * a * 4; return Vec{p.x + s.r1.x * w, p.y + s.r1.y * w, p.z + s.r1.z * w}; };
        made += chain(out + made, m, 0, 1, path, [&](double a, Piece& q) { q.width = W * 0.35 * (1 - 0.6 * a); q.along = 0.6 * a; q.alpha = 0.9 * fade; });
        made += chain(out + made, m, 0, 1, path, [&](double a, Piece& q) { q.width = W * (0.8 - 0.3 * a); q.along = 0.3 + 0.6 * a; q.alpha = 0.3 * fade; });
        break;
    }
    case 6: {  // Puffs: a small lead puff, a bigger one behind it, then smaller ones breaking off and fading
        static const double SIZE[8] = {0.7, 1.4, 1.0, 0.8, 0.65, 0.5, 0.42, 0.35};
        for (int i = 0; i < n && i < 8; i++) {
            double born = 0.05 * i, age = (u - born) / 0.55;
            if (age < 0 || age > 1) continue;
            double f = L * (0.15 + 1.1 * (1 - exp(-2.5 * age))), side = W * 0.35 * (r[i][1] - 0.5), lift = L * 0.05 * age;
            double size = W * SIZE[i] * (0.75 + 0.5 * sin(3.14159 * sqrt(age))) * (0.85 + 0.3 * r[i][2]);
            out[made++] = {inPipe(s, f, side, W * 0.3 * (r[i][3] - 0.5), lift), s.dir, size, size * 1.3, 0.08 + 0.8 * age, (0.8 - 0.2 * i / 8.0) * (1 - age * age)};
        }
        break;
    }
    case 10: {  // Fan: wide or slotted tips spread the flame in a flat horizontal sheet
        int chains = n >= 12 ? 5 : 4, m = n / chains;
        Vec h = unit({s.dir.y, -s.dir.x, 0});  // level and across the pipe
        double top = smooth(0, 0.3, u * 1.2), low = 0.85 * smooth(0.45, 1, u);
        for (int j = 0; j < chains; j++) {
            double phi = (j / (double)(chains - 1) - 0.5) * 1.2 * s.spread, Lj = L * 0.8 * (1 - 0.3 * fabs(phi)), ph = r[j][0] * PI2;
            Vec d{s.dir.x * cos(phi) + h.x * sin(phi), s.dir.y * cos(phi) + h.y * sin(phi), s.dir.z * cos(phi)};
            made += chain(out + made, m, low, top,
                [&](double a) { return Vec{s.pipe.at.x + d.x * Lj * a, s.pipe.at.y + d.y * Lj * a, s.pipe.at.z + d.z * Lj * a + W * 0.4 * a * sin(time * 18 + ph)}; },
                [&](double a, Piece& q) { q.width = W * (0.8 - 0.6 * a); q.along = 0.05 + 0.9 * a; q.alpha = (0.8 - 0.4 * a) * fade; });
        }
        break;
    }
    case 11: {  // Corkscrew: a swirling exhaust, one or two helices wound round the axis
        int helices = n >= 8 ? 2 : 1, m = n / helices;
        double grow = smooth(0, 0.3, u), low = 0.85 * smooth(0.45, 1, u);
        for (int j = 0; j < helices; j++)
            made += chain(out + made, m, low, grow,
                [&](double a) { double ang = a * 12.566 + time * 14 + j * 3.14159, rr = W * (0.06 + 0.28 * a); return inPipe(s, L * 1.1 * a, rr * cos(ang), rr * sin(ang), 0); },
                [&](double a, Piece& q) { q.width = W * (0.55 - 0.3 * a); q.along = 0.05 + 0.85 * a; q.alpha = (0.85 - 0.4 * a) * fade; });
        break;
    }
    }
    return made;
}

static void drawShape(Slot& s, double u, double time, double L, double fade, unsigned frame, unsigned every, int& budget) {
    Piece pieces[POOL];
    double W = (s.pipe.radius * 2 > 6 ? s.pipe.radius * 2 : 6) * s.size / 0.21 * (0.8 + 0.4 * s.energy);
    int n = shapePieces(s, u, time, L, W, fade, pieces);
    for (int e = 0; e < POOL; e++) {
        Tongue& g = s.tongue[e / MAX_SEGS];
        int c = e % MAX_SEGS;
        if (e >= n || pieces[e].length < 0.5 || pieces[e].alpha <= 0.01) { parkSeg(g, c); continue; }
        if (!alive(g.seg[c])) {
            if (budget <= 0) continue;
            budget--;
            g.seg[c] = refOf(makeMesh(s.owner.ptr, ball, layer(), s.pipe, 1, 0.5f, 0.1f));
            g.mid[c] = lastMid;
            g.vis[c] = false;
            if (!g.seg[c].ptr) continue;
        }
        if (!g.vis[c]) { if (showBudget <= 0) continue; showBudget--; }
        const Piece& q = pieces[e];
        placePiece(s, g.seg[c], q.at, q.d, q.width / 100, q.length / 100);
        g.parked[c] = false;
        if ((frame + e) % every == 0 || !g.vis[c]) {
            float rgb[3];
            double along = q.along + 0.15 * s.rnd[e][3] - 0.05;  // each piece its own shade, for depth
            lookRgb(s, along < 0 ? 0 : along > 1 ? 1 : along, u, rgb);
            double lit = (0.7 + 0.3 * s.rnd[e][2]) * (0.85 + 0.3 * roll());
            for (float& x : rgb) x = (float)(x * lit);
            paint(g.mid[c], rgb, ghost ? q.alpha : -1);
        }
        setVis(g.seg[c], g.vis[c], true);
    }
}

// Pooled per pipe and only shown or hidden: creating these per pop is object churn for the
// garbage collector to stall on. The meshes are made in tickFlames, a few a frame.
static void flame(uintptr_t v, Car& car, int i, const Tune& t, double energy, double seconds, double bigger, double flash, int kind, double ratio, int shape) {
    if (!ball || !glow || !canFlame) return;
    Slot& s = car.slots[i];
    if (s.shape == 1) hideClassic(s);
    s.shape = shape;
    const Pipe& pipe = car.fire[i];
    const Look& k = LOOKS[kind];
    DWORD now = GetTickCount();
    s.owner = refOf(v);
    s.pipe = pipe;
    s.dir = flat(aim(pipe));
    double ux = fabs(s.dir.z) > 0.9 ? 1 : 0, uz = 1 - ux;  // any axis not along the pipe
    Vec a{s.dir.y * uz, s.dir.z * ux - s.dir.x * uz, -s.dir.y * ux};
    double n = sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    s.r1 = {a.x / n, a.y / n, a.z / n};
    s.r2 = {s.dir.y * s.r1.z - s.dir.z * s.r1.y, s.dir.z * s.r1.x - s.dir.x * s.r1.z, s.dir.x * s.r1.y - s.dir.y * s.r1.x};
    s.kind = kind;
    s.ratio = ratio;
    s.energy = energy;
    s.flash = flash;
    s.advance = (t.timing - 0.5) * 2;  // -1 retarded .. 1 advanced
    s.rich = (t.fuel - 0.5) * 2;       // -1 lean .. 1 rich
    // The rows are relative to their defaults (size 0.2, length 0.04, spread 0.2): size is how
    // thick, length how far it reaches, spread how wide the branches open, density how many.
    s.size = 1.05 * bigger * t.size * k.size * (1 + 0.25 * (s.rich > 0 ? s.rich : 0));
    s.reach = 0.75 * t.length / 0.04 * k.reach * (0.7 + 0.6 * energy);  // a pipe fires the same with others switched off
    s.spread = 0.7 * t.spread / 0.2 * (t.wide != 0 ? 1.4 : 1);  // wide tips fan it
    s.wobble = k.wobble;
    s.pace = k.pace;
    s.detail = (t.density - 0.25) / 2.75;
    s.lod = view(s);
    // Twin tips sit side by side and overlap: past two tips, each one draws its share of two.
    double tips = car.fire.size() > 2 ? 2.0 / car.fire.size() : 1;
    int tongues = (int)floor(k.tongues * (0.6 + 1.2 * s.detail) * tips + roll()), segs = s.detail > 0.5 ? 4 : 3;
    if (v != playerNow || lowFx || s.lod < 0.6) { if (tongues > 2) tongues = 2; segs = 2; }  // traffic, far away, a weak PC
    s.tongues = tongues < 1 ? 1 : tongues > MAX_TONGUES ? MAX_TONGUES : tongues;
    s.segs = segs;
    double turn = roll() * 6.2831853;
    for (int j = 0; j < s.tongues; j++) {
        Tongue& g = s.tongue[j];
        g.azimuth = turn + j * 6.2831853 / s.tongues + (roll() - 0.5) * 2.5 / s.tongues;
        g.angle = j == 0 ? 0.08 * roll() : (0.25 + 0.35 * roll()) * s.spread;
        g.branch = 0.2 + 0.3 * roll();
        g.length = j == 0 ? 1 : 0.4 + 0.3 * roll();
        g.width = j == 0 ? 1 : 0.6 + 0.3 * roll();
        g.phase = roll() * 6.2831853;
        // Depth: the main tongue is the brightest; the branches round it are dimmer and a step
        // further along the gradient (redder), each its own amount, so they read as behind it.
        g.shade = j == 0 ? 1 : 0.55 + 0.35 * roll();
        g.shift = j == 0 ? 0 : 0.08 + 0.22 * roll();
    }
    s.glow = canGlow ? lightSpot(car, i, s.lightAt) : 0;
    if (s.glow > 0 && !alive(s.light)) s.light = refOf(makeLight(v, 150 + 700 * s.size));
    if (shape >= 2) {  // the other shapes: how many pieces, and each piece's own dice for this burst
        int pieces = (int)floor(SHAPE_PIECES[shape] * (0.5 + s.detail) * tips + 0.5);
        if (v != playerNow || lowFx) pieces = pieces > 6 ? 6 : pieces;
        else if (s.lod < 0.6) pieces /= 2;
        s.count = pieces < 4 ? 4 : pieces > POOL ? POOL : pieces;
        for (auto& d : s.rnd) for (double& x : d) x = roll();
        seconds *= SHAPE_LASTS[shape];
    }
    seconds *= k.lasts;
    s.start = now;
    s.end = now + (DWORD)(seconds * 1000 < 60 ? 60 : seconds * 1000);
    s.culled = s.lod == 0;
    // Only what this burst leaves unused is hidden: hiding the lot and showing it again flipped
    // every piece's visibility each burst, and each flip rebuilds its render state.
    if (s.culled) hideFlame(s);
    else if (shape < 2)
        for (int j = 0; j < MAX_TONGUES; j++)
            for (int c = 0; c < MAX_SEGS; c++)
                if (j >= s.tongues || c >= s.segs) parkSeg(s.tongue[j], c);
    showMesh(s.light, s.glow > 0);
    s.idle = false;
    s.on = true;
}

static void tickFlames(DWORD now, double dt) {
    static unsigned frame;
    frame++;
    int budget = 6;  // new segment meshes per frame, so a first burst cannot hitch
    makeBudget = 4;  // one frame's worth for everything made until the next tickFlames
    showBudget = 16;
    uintptr_t context = 0;
    for (auto& entry : cars)
        for (Slot& s : entry.second.slots)
            if (!context && s.on && alive(s.owner)) context = s.owner.ptr;
    if (context) readCamera(context);
    for (auto& entry : cars)
        for (Slot& s : entry.second.slots) {
            if (!s.on) {  // quiet for a second: hidden for real
                if (!s.idle && s.shape != 1 && (int)(now - s.end) > 1000) { hideFlame(s); showMesh(s.light, false); s.idle = true; }
                continue;
            }
            if (s.shape == 1) continue;
            if ((int)(now - s.end) >= 0 || !alive(s.owner)) {
                s.on = false;
                if (alive(s.owner)) parkFlame(s); else hideFlame(s);
                if (alive(s.owner)) dimLight(s); else { showMesh(s.light, false); s.idle = true; }
                continue;
            }
            s.lod = view(s);
            if ((s.lod == 0) != s.culled) { s.culled = s.lod == 0; if (s.culled) hideFlame(s); }
            double u = (now - s.start) / (double)(s.end > s.start ? s.end - s.start : 1), time = (now - s.start) / 1000.0;
            double fade = 1 - smooth(0.55, 1, u);
            double grow = smooth(0, 0.3, u * s.pace);            // it shoots out,
            double tail = 0.85 * smooth(0.45, 1, u);             // then leaves the pipe and burns out at the tips
            double L = s.reach * s.size * 140;                   // cm
            if (s.glow > 0 && alive(s.light)) {  // the light stays when culled: it lands on road the camera can see
                double reach = 35 + L * grow * 0.5;
                placeMesh(s.light, {s.lightAt.x + s.dir.x * reach, s.lightAt.y + s.dir.y * reach, s.lightAt.z + s.dir.z * reach}, 1, 1);
                struct { float rgba[4]; bool srgb; } tint = {{0, 0, 0, 1}, false};
                lightRgb(s, u, tint.rgba);
                if (frame % 2 == 0) api->process_event(s.light.ptr, fLightColor, &tint);
                float power = (float)(8.1 * s.glow * s.flash * s.size / 0.105 * (0.5 + s.energy) * fade * grow * (0.7 + 0.6 * roll()));
                api->process_event(s.light.ptr, fIntensity, &power);
            }
            if (s.culled) continue;
            unsigned every = s.lod < 0.5 ? 4 : 2;  // far away, colour is not worth a call a frame
            // No opaque core at the pipe any more: a solid bulb there read as the bumper exploding.
            if (s.shape >= 2) { drawShape(s, u, time, L, fade, frame, every, budget); continue; }
            for (int j = 0; j < s.tongues; j++) {
                Tongue& g = s.tongue[j];
                double Lj = L * g.length, top = grow, low = tail < top - 0.05 ? tail : top - 0.05;
                for (int c = 0; c < s.segs; c++) {
                    if (!alive(g.seg[c])) {
                        if (budget <= 0) continue;
                        budget--;
                        g.seg[c] = refOf(makeMesh(s.owner.ptr, ball, layer(), s.pipe, 1, 0.5f, 0.1f));
                        g.mid[c] = lastMid;
                        g.vis[c] = false;
                        if (!g.seg[c].ptr) continue;
                    }
                    if (!g.vis[c]) { if (showBudget <= 0) continue; showBudget--; }
                    double a0 = low + (top - low) * c / s.segs, a1 = low + (top - low) * (c + 1) / s.segs, am = (a0 + a1) / 2;
                    Vec p0 = pathAt(s, g, a0 < 0 ? 0 : a0, Lj, time), p1 = pathAt(s, g, a1, Lj, time);
                    Vec d{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
                    double len = sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                    if (len < 0.5) { parkSeg(g, c); continue; }
                    // As wide as the pipe at its mouth, tightening as it goes: the shared trunk reads
                    // as a short cone that narrows and splits into thin branches.
                    double mouth = (s.pipe.radius * 2 > 6 ? s.pipe.radius * 2 : 6) * s.size / 0.21;
                    double width = mouth * g.width * (1 - 0.8 * pow(am, 0.7)) * (0.8 + 0.4 * s.energy) * (0.6 + 0.4 * fade);
                    placePiece(s, g.seg[c], {(p0.x + p1.x) / 2, (p0.y + p1.y) / 2, (p0.z + p1.z) / 2}, {d.x / len, d.y / len, d.z / len}, width / 100, len * 1.35 / 100);
                    g.parked[c] = false;
                    if ((frame + j * 3 + c) % every == 0 || !g.vis[c]) {
                        float rgb[3];
                        double along = am + g.shift;
                        lookRgb(s, along > 1 ? 1 : along, u, rgb);
                        double lit = g.shade * (0.85 + 0.3 * roll());  // and each segment flickers on its own
                        for (float& x : rgb) x = (float)(x * lit);
                        paint(g.mid[c], rgb, ghost ? (0.8 - 0.45 * am) * fade * (0.7 + 0.3 * g.shade) : -1);
                    }
                    setVis(g.seg[c], g.vis[c], true);
                }
            }
        }
}

// The classic flame (Flame shape: Classic): cones and a flamethrower of streaks.
// Pooled per pipe and only shown or hidden: creating these per pop is object churn for the
// garbage collector to stall on. Traffic gets a smaller pool than the player.
static void flameClassic(uintptr_t v, Car& car, int i, const Tune& t, double energy, double seconds, double bigger, double flash, int style, int kind, double ratio) {
    if (!cone || !ball || !glow || !canFlame) return;
    Slot& s = car.slots[i];
    if (s.shape != 1) { hideFlame(s); s.shape = 1; }  // the tongues of a burst still on the pipe
    s.kind = kind;
    s.ratio = ratio;
    const Pipe& pipe = car.fire[i];
    DWORD now = GetTickCount();
    s.owner = refOf(v);
    s.pipe = pipe;
    s.dir = flat(aim(pipe));
    double ux = fabs(s.dir.z) > 0.9 ? 1 : 0, uz = 1 - ux;  // any axis not along the pipe
    Vec a{s.dir.y * uz, s.dir.z * ux - s.dir.x * uz, -s.dir.y * ux};
    double n = sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    s.r1 = {a.x / n, a.y / n, a.z / n};
    s.r2 = {s.dir.y * s.r1.z - s.dir.z * s.r1.y, s.dir.z * s.r1.x - s.dir.x * s.r1.z, s.dir.x * s.r1.y - s.dir.y * s.r1.x};
    s.size = 1.05 * bigger * t.size;
    s.advance = (t.timing - 0.5) * 2;  // -1 retarded .. 1 advanced
    s.rich = (t.fuel - 0.5) * 2;       // -1 lean .. 1 rich
    s.flash = flash;
    // See-through streaks need more to build a body; Flame density scales the pool and the rate.
    s.dense = t.density * (car.fire.size() > 2 ? 2.0 / car.fire.size() : 1);  // twin tips overlap: each draws its share of two
    size_t pool = v == playerNow && !lowFx ? (size_t)ceil(14 * s.dense) : 3;
    if (s.sparks.size() < pool) s.sparks.resize(pool > 60 ? 60 : pool);
    s.style = style;
    s.spread = 0.7 * 1.5 * t.spread * (style == 2 ? 1.8 : 1) * (1 - 0.25 * (t.timing - 0.5) * 2) * (t.wide != 0 ? 1.4 : 1);
    s.energy = energy;
    if (!s.on) {  // a new burst gets its own noise, so no two are alike
        s.swirl = roll() * 6.2831853;
        s.spin = (roll() < 0.5 ? -1 : 1) * (4 + 8 * roll());
        for (double& f : s.phase) f = roll() * 6.2831853;
    }
    // Density fills the cone too: up to three more see-through layers between core and shell,
    // each a step along the colour gradient. A far flame keeps two, it would not show more.
    s.detail = (s.dense - 0.25) / 2.75;
    s.lod = view(s);
    s.layers = s.lod < 0.6 || lowFx ? 2 : 2 + (int)(s.detail * 3);
    if (s.layers > MAX_LAYERS) s.layers = MAX_LAYERS;
    for (int c = 0; c < s.layers; c++)
        if (!alive(s.cone[c])) { s.cone[c] = refOf(makeMesh(v, cone, c == 1 ? glow : layer(), pipe, 1, 0.5f, 0.1f)); s.coneMid[c] = lastMid; }
    s.glow = canGlow ? lightSpot(car, i, s.lightAt) : 0;
    if (s.glow > 0 && !alive(s.light)) s.light = refOf(makeLight(v, 150 + 700 * s.size));
    // The rows are relative to their defaults (length 0.04, spread 0.2, size 0.2), so each one
    // moves the shape it names: length is how far it reaches, spread how wide it fans.
    s.reach = 0.75 * t.length / 0.04;
    s.coneLength = s.size * 2.16 * s.reach * (0.75 + 0.5 * roll());
    s.start = now;
    s.end = now + (DWORD)(seconds * 1000 < 34 ? 34 : seconds * 1000);
    if (seconds < 0.05 && !s.on && style != 1) s.debt = 12 * s.dense;
    s.culled = s.lod == 0;
    for (int c = 0; c < MAX_LAYERS; c++) showMesh(s.cone[c], c < s.layers && style != 2 && !s.culled);
    showMesh(s.light, s.glow > 0);
    s.idle = false;
    s.on = true;
}

// One streak off the pipe. Three kinds, by pool position: a thin fast yellow core, the orange
// body, and a slow wide red wisp that scatters like the diesel smoke does.
// A flame's colour is where a point sits in it, not a colour of its own. along runs 0 at the pipe
// to 1 at the tip, out 0 on the axis to 1 at the edge: a blue base where it burns hottest, red
// through the body and spread, yellow tips where the soot glows, and the edge stays red.
// Advanced timing and a lean mix burn hotter and cleaner, so the blue climbs up the flame. Rich
// or retarded burns late and sooty, so the yellow comes down it, and a hard burst adds blue.
static void flameRgb(const Slot& s, double along, double out, float* rgb) { heatRgb(s, along, out, 0, rgb); }

// alpha >= 0: a see-through layer on M_GhostCar, whose colour is its own unlit output, so it
// is pushed past 1 to bloom the way M_AttachmentLight's intensity does.
static void heatColor(uintptr_t mid, const Slot& s, double along, double out, double alpha = -1) {
    if (!mid) return;
    struct { uint64_t name; float rgba[4]; } color = {alpha >= 0 ? colorName : lightColorName, {0, 0, 0, 1}};
    flameRgb(s, along, out, color.rgba);
    if (alpha >= 0) for (int i = 0; i < 3; i++) color.rgba[i] *= flameGain();
    if (alpha >= 0) color.rgba[3] = solid(alpha);
    api->process_event(mid, fMidColor, &color);
    if (alpha < 0) return;
    struct { uint64_t name; float value; } fade = {opacityName, solid(alpha)};
    api->process_event(mid, fMidScalar, &fade);
}

static bool launch(Slot& s, Spark& p, int kind, double seconds) {
    static const double WIDTH[3] = {0.08, 0.12, 0.2}, LENGTH[3] = {1.6, 2, 1.2}, LIFE[3] = {0.05, 0.07, 0.1}, SPREAD[3] = {0.03, 0.05, 0.11}, PACE[3] = {1, 1, 0.55};
    double burn = 1 + 0.3 * s.advance - 0.15 * (s.rich > 0 ? s.rich : 0);
    double speed = burn * s.size * 3240 * (0.7 + 0.6 * roll()) * (0.8 + 0.4 * s.energy) * PACE[kind];
    // Slow noise, sines at unrelated rates with this burst's phases: it moves a hot spot round the
    // circle and breathes it in and out, so the shape keeps changing and never repeats.
    double noise = (sin(seconds * 7.3 + s.phase[0]) + sin(seconds * 11.9 + s.phase[1]) + sin(seconds * 19.1 + s.phase[2])) / 3;
    double hot = s.swirl + s.spin * seconds + noise * 2.5, turn = roll() < 0.6 ? hot + (roll() - roll()) * 1.4 : roll() * 6.2831853;
    double c = cos(turn), d = sin(turn);
    // The chance a streak leaves the middle for the circle, and how wide that circle is, both
    // grow with the Spread row and with the energy that went unburnt.
    bool ring = roll() < s.spread * (0.4 + 0.6 * s.energy);
    double circle = ring ? s.spread * s.size * 50 * (0.5 + s.energy) * (0.75 + 0.25 * noise) * sqrt(roll()) : 0;
    Vec out{s.r1.x * c + s.r2.x * d, s.r1.y * c + s.r2.y * d, s.r1.z * c + s.r2.z * d};
    double off0 = s.pipe.radius * 0.6 * roll();
    p.pos = {s.pipe.at.x + out.x * off0, s.pipe.at.y + out.y * off0, s.pipe.at.z + out.z * off0};
    p.age = 0;
    p.life = LIFE[kind] * s.reach * (1 + roll()) * (1 - 0.25 * s.advance + 0.3 * (s.rich > 0 ? s.rich : 0));
    double fan = speed * SPREAD[kind] * (s.spread / 0.3) * (s.spread / 0.3) * roll() + circle / (p.life * 0.6);  // sideways enough to reach the circle in its flight
    p.vel = {s.dir.x * speed + out.x * fan, s.dir.y * speed + out.y * fan, s.dir.z * speed + out.z * fan};
    p.width = s.size * WIDTH[kind] * 1.4 * (1 + 0.35 * (s.rich > 0 ? s.rich : 0) - 0.2 * s.advance);
    p.length = s.size * LENGTH[kind] * sqrt(s.reach);
    p.kick = speed * 0.8;
    p.heat = (1 - s.energy) * 0.2 + kind * 0.15;  // the wider kinds are born further up the flame
    p.out = ring ? 0.5 + 0.5 * roll() : 0.3 * roll();
    p.tint = (roll() - 0.5) * 0.3 * s.detail;  // dense flames shift colour streak by streak
    // Neighbour check: a streak leaving with the same heading as one launched a moment ago would
    // be drawn inside it and add nothing but cost, so it is not launched (it still counts).
    double n = sqrt(p.vel.x * p.vel.x + p.vel.y * p.vel.y + p.vel.z * p.vel.z);
    for (const Spark& q : s.sparks) {
        if (&q == &p || !q.live || q.age > 0.03 || n <= 0) continue;
        double m = sqrt(q.vel.x * q.vel.x + q.vel.y * q.vel.y + q.vel.z * q.vel.z);
        if (m > 0 && (p.vel.x * q.vel.x + p.vel.y * q.vel.y + p.vel.z * q.vel.z) / (n * m) > 0.997) return false;
    }
    p.live = true;
    if (!p.shown) { p.shown = true; showMesh(p.c, true); }
    return true;
}

static void tickClassic(DWORD now, double dt) {
    static unsigned frame;
    frame++;
    int budget = 4;  // new streak meshes per frame, so a first burst cannot hitch
    for (auto& entry : cars)
        for (Slot& s : entry.second.slots) {
            if (s.shape != 1) continue;
            if (s.on && (int)(now - s.end) >= 0) {
                s.on = false;
                for (Ref& c : s.cone) park(c);
                dimLight(s);
            }
            if (!s.on && !s.idle && (int)(now - s.end) > 1000) {  // quiet for a second: hidden for real
                for (Ref& c : s.cone) showMesh(c, false);
                showMesh(s.light, false);
                for (Spark& p : s.sparks)
                    if (p.shown && !p.live) { p.shown = false; showMesh(p.c, false); }
                s.idle = true;
            }
            if (s.on) {
                s.lod = view(s);
                bool cull = s.lod == 0;
                if (cull != s.culled) {  // the light stays: it lands on road the camera can see
                    s.culled = cull;
                    for (int c = 0; c < s.layers; c++) showMesh(s.cone[c], !cull && s.style != 2);
                    if (cull)
                        for (Spark& p : s.sparks)
                            if (p.live) { p.live = false; p.shown = false; showMesh(p.c, false); }
                }
            }
            if (s.on) {
                double u = (now - s.start) / (double)(s.end > s.start ? s.end - s.start : 1);
                double shape = s.end - s.start <= 50 ? 1 : u < 0.25 ? u / 0.25 : 1 - (u - 0.25) / 0.75;
                if (s.glow > 0 && alive(s.light)) {
                    double reach = 35 + s.coneLength * 50 * shape;
                    placeMesh(s.light, {s.lightAt.x + s.dir.x * reach, s.lightAt.y + s.dir.y * reach, s.lightAt.z + s.dir.z * reach}, 1, 1);
                    struct { float rgba[4]; bool srgb; } tint = {{0, 0, 0, 1}, false};
                    lightRgb(s, u, tint.rgba);
                    if (frame % 2 == 0) api->process_event(s.light.ptr, fLightColor, &tint);
                    float power = (float)(8.1 * s.glow * s.flash * s.size / 0.105 * (0.5 + s.energy) * shape * (0.7 + 0.6 * roll()));
                    api->process_event(s.light.ptr, fIntensity, &power);
                }
                if (s.culled) continue;
                double t = (now - s.start) / 1000.0;
                for (int c = 0; c < s.layers; c++) {
                    if (!alive(s.cone[c]) || s.style == 2) continue;
                    // o: 0 at the core, 1 at the shell; the inner layers sit evenly between, and
                    // drift along the gradient on their own beat, more the denser the flame.
                    double o = c == 0 ? 1 : c == 1 ? 0 : (c - 1) / (double)(s.layers - 1);
                    double inner = 0.55 + 0.45 * o, length = s.coneLength * inner * shape * (0.85 + 0.3 * roll()), half = length * 50;
                    double along = 0.02 + 0.28 * o + u * 0.6 + (c > 1 ? 0.1 * s.detail * sin(t * 9 + c * 1.7 + s.phase[c % 3]) : 0);
                    Vec at{s.pipe.at.x + s.dir.x * half, s.pipe.at.y + s.dir.y * half, s.pipe.at.z + s.dir.z * half};
                    heatColor(s.coneMid[c], s, along < 0 ? 0 : along > 1 ? 1 : along, 0.4 * o, c == 1 || !ghost ? -1 : (c == 0 ? 0.45 : 0.3) * shape);
                    placeSeg(s.cone[c], at, s.dir, s.size * 0.22 * inner, length);
                }
                if (s.style != 1) s.debt += 70 * s.dense * s.lod * (0.5 + s.energy) * dt;
                for (size_t i = 0; i < s.sparks.size() && s.debt >= 1; i++) {
                    Spark& p = s.sparks[i];
                    if (p.live) continue;
                    if (!alive(p.c)) {
                        if (budget <= 0 || !alive(s.owner)) continue;
                        budget--;
                        p.c = refOf(makeMesh(s.owner.ptr, ball, layer(), s.pipe, 1, 0.5f, 0.1f));
                        p.mid = lastMid;
                        if (!p.c.ptr) continue;
                    }
                    launch(s, p, (int)(i % 3), (now - s.start) / 1000.0 + entry.first % 97);
                    s.debt -= 1;
                }
                if (s.debt > 12 * s.dense) s.debt = 12 * s.dense;
            }
            for (Spark& p : s.sparks) {
                if (!p.live) continue;
                p.age += dt;
                if (p.age >= p.life || !alive(p.c)) {
                    p.live = false;
                    park(p.c);  // shrunk, still shown: the next streak reuses it without a visibility change
                    continue;
                }
                double u = p.age / p.life, k = sin(3.14159265 * sqrt(u));  // swells fast, tapers out
                double lick = p.kick * dt * (1 + 3 * s.spread);  // the turbulence that makes it lick
                p.vel.x += (roll() - 0.5) * lick;
                p.vel.y += (roll() - 0.5) * lick;
                p.vel.z += (roll() - 0.5) * lick;
                p.pos.x += p.vel.x * dt; p.pos.y += p.vel.y * dt; p.pos.z += p.vel.z * dt;
                // Overlapping see-through streaks add up: dense in the middle, thin at the edges and tips.
                static const double ALPHA[3] = {0.7, 0.5, 0.28};
                int kind = (int)((&p - s.sparks.data()) % 3);
                unsigned every = s.lod < 0.5 ? 4 : 2;  // far away colour changes are not worth a call a frame
                double along = p.heat + u * (1 - p.heat) + p.tint;
                if ((frame + (unsigned)(&p - s.sparks.data())) % every == 0)
                    heatColor(p.mid, s, along < 0 ? 0 : along > 1 ? 1 : along, p.out, ghost ? ALPHA[kind] * (1 - u * u) : -1);
                // Laid along its own flight, so the streaks fan out as a cone and lick as they turn.
                double plen = p.length * (1 - 0.4 * u);
                Vec pd = unit(p.vel);
                placeSeg(p.c, pieceAt(s, p.pos, pd, plen), pd, p.width * k * (ghost ? 1.3 : 1), plen);
                if (!p.shown) { p.shown = true; showMesh(p.c, true); }
            }
        }
}

// Kinds of pop and bang. One sound, so a kind is its pitch, weight, how many times it fires and how fast.
// style: the classic shape's 0 cone and streaks, 1 a jet (cone only), 2 scattered streaks with no cone.
struct Shot { double pitch, wander, volume; int repeats; DWORD gap; double flame, seconds, size, flash; int style; };
static const Shot POPS[6] = {
    {1.0, 0.15, 1, 1, 0, 0.7, 1, 1, 1, 0},            // pop
    {1.35, 0.2, 0.55, 4, 55, 0.35, 0.5, 0.7, 0.6, 2},   // crackle
    {0.7, 0.1, 0.7, 2, 110, 0.15, 0.6, 0.8, 0.7, 0},    // burble
    {1.6, 0.2, 0.5, 1, 0, 0.9, 0.4, 0.6, 0.6, 1},       // spit
    {1.15, 0.25, 0.4, 6, 35, 0.25, 0.35, 0.6, 0.5, 2},  // chatter
    {0.85, 0.1, 0.8, 1, 0, 0.4, 0.7, 0.9, 0.8, 1}};     // cough
static const int UNBURNT_POPS[3] = {1, 2, 4};
// A zero gap stacks the shots on one instant: volume alone tops out.
static const Shot BANGS[6] = {
    {0.6, 0.08, 2.5, 1, 0, 1, 1.2, 1.2, 2, 0},        // boom
    {0.75, 0.1, 2.2, 2, 95, 1, 0.8, 1.1, 2, 0},       // double
    {1.25, 0.1, 5, 3, 0, 1, 0.4, 0.9, 2.5, 1},        // crack
    {0.48, 0.05, 6, 4, 0, 1, 1.6, 1.4, 2.5, 0},       // cannon
    {0.9, 0.15, 2, 3, 70, 1, 0.5, 1, 1.5, 2},         // burst
    {0.42, 0.04, 5, 3, 0, 1, 1.3, 1.3, 2.2, 2}};      // thunder

static void playOn(uintptr_t v, Car& car, uintptr_t snd, float volume, float pitch, float lowpass, float highpass, float fade = 0);

static int engineHas(uintptr_t vehicle);

struct Fired { Ref owner; DWORD at; float volume, pitch, lowpass; double flash, energy, ratio; Glow heat; int kind, shape; bool seen, boom; };
static std::vector<Fired> fired;

// How hard the exhaust pushes back: the muffler (Back pressure row) and a cat, which adds a quarter.
static double backOf(const Tune& t) { double b = 0.75 * t.back + (t.decat == 0 ? 0.25 : 0); return b < 0 ? 0 : b > 1 ? 1 : b; }

static void fire(uintptr_t v, uintptr_t engine, Car& car, const Tune& t, const Shot& k, double energy, double ratio, bool bang, int kind, int shape, Glow heat, bool seen, DWORD now, double loud = 1) {
    if (!car.read) readPipes(v, engine, car);
    // The engine caps a sound at volume 4, so a bang cannot just be louder. A pop is a soft, low,
    // muffled burp; a bang is an explosion: the crack at the cap and open, and under it a real
    // blast (the game's firework burst, pitched down) 20 ms on. Once per bang, not per shot.
    // A free-flowing pipe lights the fuel inside and the fire has to travel out: the bang first,
    // its flame after. A restrictive one holds the gas back hot until it leaves all at once, the
    // bang and the flame together. The flame alone is an entry with no sound.
    DWORD late = (DWORD)((1 - backOf(t)) * (bang ? 160 : 90));
    if (seen && late > 5) fired.push_back({refOf(v), now + late, 0.0f, 1, 20000, k.flash, energy, ratio, heat, kind, shape, true, false});
    if (late > 5) seen = false;
    if (bang) {
        float pitch = (float)(k.pitch * (1 + k.wander * (2 * roll() - 1)));
        fired.push_back({refOf(v), now, 4.0f, pitch, 20000, k.flash, energy, ratio, heat, kind, shape, seen, false});
        fired.push_back({refOf(v), now + 20, 4.0f, (float)(0.62 + 0.12 * roll()), 20000, k.flash, energy, ratio, heat, kind, shape, false, true});
        return;
    }
    for (int i = 0; i < k.repeats && fired.size() < 60; i++) {
        float pitch = (float)(0.85 * k.pitch * (1 + k.wander * (2 * roll() - 1)));
        float volume = (float)(loud * 0.35 * t.volume * k.volume * (0.8 + 0.4 * roll()) * (1 - 0.25 * i));
        fired.push_back({refOf(v), now + i * k.gap, volume > 1.0f ? 1.0f : volume, pitch, 1600, k.flash, energy, ratio, heat, kind, shape, seen && i == 0, false});
    }
}

static void tickFired(DWORD now) {
    for (size_t i = 0; i < fired.size();) {
        Fired x = fired[i];
        if ((int)(now - x.at) < 0) { i++; continue; }
        fired.erase(fired.begin() + i);
        auto known = cars.find(x.owner.ptr);
        if (!alive(x.owner) || known == cars.end() || !sound) continue;
        uintptr_t v = x.owner.ptr;
        Car& car = known->second;
        const Tune& t = tuneFor(v);
        uintptr_t wave = x.boom ? booms[rand() % 2] : waves[rand() % 6];
        // The firework burst is the blast of a bang; its recording goes on into a long crackle of sparks,
        // which a bang does not have, so that layer fades out over half a second.
        if (x.volume > 0) playOn(v, car, wave ? wave : sound, x.volume, x.pitch, x.lowpass, 0, x.boom ? 0.5f : 0);
        if (!x.seen || car.pipes.empty() || base.flames == 0) continue;  // Flames=0: heard, never seen
        double seconds = 2.7 * t.life * (0.4 + 0.9 * x.energy) * (0.6 + 0.8 * roll());
        if (seconds > 0.6) seconds = 0.6;
        double bp = backOf(t);
        seconds *= 0.7 + 0.6 * bp;
        double bigger = (0.6 + 0.9 * x.energy) * (0.8 + 0.5 * bp), energy = x.energy;
        // One manifold's pressure reaches every tip: all pipes fire together, each with its own shape.
        firePoints(car, t);
        for (int i2 = 0; i2 < (int)car.fire.size(); i2++) {
            car.slots[i2].flameHue = x.heat;
            if (x.shape == 1) flameClassic(v, car, i2, t, energy, seconds, bigger, x.flash, 0, x.kind, x.ratio);
            else flame(v, car, i2, t, energy, seconds, bigger, x.flash, x.kind, x.ratio, x.shape);
        }
    }
}

static bool inRange(uintptr_t v, uintptr_t pawn, const Vec& me) {
    if (v == pawn) return true;
    Vec at = callVec(v, fLoc);
    double dx = at.x - me.x, dy = at.y - me.y, dz = at.z - me.z;
    return dx * dx + dy * dy + dz * dz <= kRange * kRange;
}

static void stopSmoke(Car& car) {
    for (auto& r : car.smokes)
        if (alive(r)) api->process_event(r.ptr, fStop, nullptr);  // bAutoDestroy removes it
    car.smokes.clear();
}

static void tickSmoke(uintptr_t v, uintptr_t engine, Car& car, const Tune& t, float rpm, float throttle, bool close) {
    if (t.dpf == 0 || t.smoke <= 0 || rpm <= 0 || !close || !smoke) { if (!car.smokes.empty()) stopSmoke(car); return; }
    if (!car.read) readPipes(v, engine, car);
    for (auto& r : car.smokes)
        if (!alive(r)) { car.smokes.clear(); break; }
    if (car.smokes.empty())
        for (const Pipe& pipe : socketPoints(car, t)) {
            uintptr_t nc = attachEffect(smoke, rootOf(v), pipe.at, rotatorOf(pipe));
            if (!nc) continue;
            car.smokes.push_back({get<int>(nc + L().obj_index), nc});
            struct { uint64_t name; float rgba[4]; } color = {smokeColorName, {0, 0, 0, 1}};  // the game's own black
            api->process_event(nc, fSetColor, &color);
            setVar(nc, smokeRadiusName, (float)pipe.radius);
        }
    double load = throttle < 0 ? 0 : throttle > 1 ? 1 : throttle;
    // The effect's lifetime is baked in, so it runs slowed: each puff lasts Lasts times as long.
    // Slowing it slows everything in it, so the exit speed and the car's velocity go in that many
    // times higher or the smoke just hangs at the pipe; with drag per sim second they also carry
    // it that much further. Emission stays, so a longer trail spreads the smoke, not thickens it.
    double lasts = t.lasts < 1 ? 1 : t.lasts > 5 ? 5 : t.lasts, made = t.smoke > 5 ? 5 : t.smoke;
    if (lowFx) { lasts = 1; made = made > 1 ? 1 : made; }  // particles alive go with amount x duration
    float amount = (float)(made * (0.2 + 0.8 * sqrt(load))), dilation = (float)(1 / lasts), push = (float)((0.3 + 0.7 * load) * lasts);
    Vec moving = callVec(v, fVelocity);
    struct { uint64_t name; Vec v; } speed = {smokeVelocityName, {moving.x * lasts, moving.y * lasts, moving.z * lasts}};
    for (auto& r : car.smokes) {
        api->process_event(r.ptr, fDilation, &dilation);
        setVar(r.ptr, smokeAmountName, amount);
        setVar(r.ptr, smokePressureName, push);
        api->process_event(r.ptr, fSetVec, &speed);
    }
}

// A turbo's release: one flutter shot played over and over with the gaps opening up, the
// compressor surging against a shut throttle. What sets it apart is the fitted part: its row
// name says the family, so a compound is low and dark and a dragster thin and high, and a
// twin or quad plays that many slightly detuned voices.
static Flutter flutterOf(const std::string& key) {
    std::string low = key;
    for (auto& c : low) c = (char)tolower(c);
    auto has = [&](const char* w) { return low.find(w) != std::string::npos; };
    Flutter f{1, 9000, 0, 0.7, 3, 75, 1};
    if (has("compound")) f = {0.9, 7000, 0, 1.3, 4, 90, 1};
    else if (has("dragster")) f = {1.12, 12000, 0, 1.4, 4, 60, 1};
    else if (has("heavy")) f = {0.93, 7500, 0, 1.2, 3, 85, 1};
    else if (has("eco")) f = {1.05, 8000, 0, 0.45, 2, 75, 1};
    size_t stage = low.find("stage");
    if (stage != std::string::npos && stage + 5 < low.size()) {
        int n = low[stage + 5] - '0';
        f.pitch *= 1 + 0.01 * n;
        f.volume *= 1 + 0.12 * n;
        f.shots += n / 2;
    }
    if (key.size() > 2 && key.compare(key.size() - 2, 2, "TT") == 0) f.turbos = 2;
    if (key.size() > 2 && key.compare(key.size() - 2, 2, "QT") == 0) f.turbos = 4;
    return f;
}

static std::string fittedPart(uintptr_t v, int slot) {
    uintptr_t data = get<uintptr_t>(v + oParts);
    int n = get<int>(v + oParts + 8);
    for (int i = 0; data && i < n && i < 256; i++) {
        uintptr_t part = data + (uintptr_t)i * partSize;
        if (get<uint8_t>(part + oPartSlot) == slot) return fname(part + oPartKey);
    }
    return "";
}

static int flutterTries, flutterLogged;

static uintptr_t spawnKept(uintptr_t v, uintptr_t sound_, float volume, float pitch) {
    uintptr_t root = rootOf(v);
    unsigned char q[0xc0] = {0};
    memcpy(q + off(fFlutter, "Sound"), &sound_, 8);
    memcpy(q + off(fFlutter, "AttachToComponent"), &root, 8);
    memcpy(q + off(fFlutter, "VolumeMultiplier"), &volume, 4);
    memcpy(q + off(fFlutter, "PitchMultiplier"), &pitch, 4);
    memcpy(q + off(fFlutter, "AttenuationSettings"), &flutterRoom, 8);
    int stops = off(fFlutter, "bStopWhenAttachedToDestroyed");
    if (stops >= 0) q[stops] = 1;
    api->process_event(gameplayCDO, fFlutter, q);
    uintptr_t audio = 0;
    memcpy(&audio, q + off(fFlutter, "ReturnValue"), 8);
    return audio;
}

// Every shot used to spawn a component: thousands of objects for the collector, each one a chance
// to hand the audio thread something half destroyed. A car keeps a fixed set and reuses them;
// they ride on the car, so other players' cars are placed in 3D like the player's.
static void playOn(uintptr_t v, Car& car, uintptr_t snd, float volume, float pitch, float lowpass, float highpass, float fade) {
    if (!snd || !fSetSound || !fWhinePitch || !fWhineVolume || !fWhinePlay) return;
    int i = car.nextVoice++ % (v == playerNow ? 8 : 2);
    Ref& r = car.voice[i];
    if (!alive(r) || dying(r.ptr)) {
        uintptr_t audio = spawnKept(v, snd, 0.001f, 1);
        if (!audio) return;
        if (fWhineStop) api->process_event(audio, fWhineStop, nullptr);
        r = refOf(audio);
        car.voiceSound[i] = snd;
    }
    if (car.voiceSound[i] != snd) {
        unsigned char q[0x10] = {0};
        memcpy(q, &snd, 8);
        api->process_event(r.ptr, fSetSound, q);
        car.voiceSound[i] = snd;
    }
    unsigned char low[8] = {(unsigned char)(lowpass < 20000)}, high[8] = {(unsigned char)(highpass > 0)};
    api->process_event(r.ptr, fLowOn, low);
    if (low[0]) api->process_event(r.ptr, fLowHz, &lowpass);
    api->process_event(r.ptr, fHighOn, high);
    if (high[0]) api->process_event(r.ptr, fHighHz, &highpass);
    api->process_event(r.ptr, fWhinePitch, &pitch);
    api->process_event(r.ptr, fWhineVolume, &volume);
    float from = 0;
    api->process_event(r.ptr, fWhinePlay, &from);
    if (fade > 0 && fFadeOut) {  // fades out from now: only the start of the sound is heard
        unsigned char q[0x10] = {0};
        memcpy(q + off(fFadeOut, "FadeOutDuration"), &fade, 4);
        api->process_event(r.ptr, fFadeOut, q);
    }
}

// A sound played from a plugin is only referenced while it plays, so the game collects it and
// the next play can reach the audio thread half destroyed. A silent stopped component on the car
// is a real reference; if the car goes, the dying() check covers the gap until the next one.
static Ref flutterHeld;

static void holdFlutter(uintptr_t v) {
    if (alive(flutterHeld) && !dying(flutterHeld.ptr)) return;
    uintptr_t audio = spawnKept(v, flutterShot, 0.001f, 1);
    if (!audio) return;
    if (fWhineStop) api->process_event(audio, fWhineStop, nullptr);
    flutterHeld = refOf(audio);
}

static void queueFlutter(uintptr_t v, const Tune& t, double push, DWORD now) {
    if (!canFlutter) return;
    std::string key = fittedPart(v, 7);  // EMTVehiclePartSlot::Turbocharger
    if (key.empty()) { if (flutterLogged < 300) { flutterLogged++; MT_LOG("flutter: no turbo part fitted"); } return; }
    if (!flutterShot) {
        if (flutterTries >= 6) return;
        flutterTries++;
        const wchar_t* places[] = {L"/Game/Sounds/Vehicle/Intake/QX_Flutter_Shot.QX_Flutter_Shot"};
        int found = -1;
        for (int i = 0; i < 1 && !flutterShot; i++)
            if ((flutterShot = loadAsset(places[i]))) found = i;
        flutterRoom = loadAsset(L"/Game/Sounds/EngineSoundAttenuation.EngineSoundAttenuation");
        // The package loads where the dotted path comes back empty, so ask it directly.
        if (flutterTries == 1 && !flutterShot) {
            uintptr_t got = loadAsset(L"/Game/Sounds/Vehicle/Intake/QX_Flutter_Shot");
            MT_LOG("package load gives %s %s", got ? objName(classOf(got)).c_str() : "nothing", got ? objName(got).c_str() : "");
            if (got && objName(classOf(got)) == "SoundWave") flutterShot = got;
            int seen = 0;
            for (int i = 0, n = api->num_objects(); i < n && seen < 8; i++) {
                uintptr_t o = api->object(i);
                if (!o || objName(o).find("Flutter_Shot") == std::string::npos) continue;
                seen++;
                MT_LOG("in memory: %s %s, flags %x", objName(classOf(o)).c_str(), objName(o).c_str(), get<int>(o + L().obj_flags));
                if (!flutterShot && objName(classOf(o)) == "SoundWave") flutterShot = o;
            }
            MT_LOG("%d objects named Flutter_Shot in memory", seen);
        }
        MT_LOG("flutter try %d: %s, place %d, attenuation %d", flutterTries, flutterShot ? "loaded" : "NOT LOADED", found, flutterRoom != 0);
        if (!flutterShot) return;
    }
    holdFlutter(v);
    Flutter f = flutterOf(key);
    for (int voice = 0; voice < f.turbos; voice++) {
        double detune = 1 + voice * 0.035 * (voice % 2 ? 1 : -1);
        DWORD when = now + voice * 17, gap = f.gap;
        int shots = (int)(f.shots * (0.5 + 0.5 * push) + 0.5);
        for (int i = 0; i < shots; i++) {
            float fade = (float)(1 - i / (double)(shots + 1));
            voices.push_back({refOf(v), when, (float)(f.pitch * detune * (1 + 0.05 * (roll() - 0.5))),
                              (float)(0.75 * f.volume * push * fade), (float)f.low, (float)f.high});
            when += gap;
            gap += gap / 5;  // the surge dies away, each flutter a little later
        }
    }
    if (flutterLogged < 300) {
        flutterLogged++;
        MT_LOG("flutter %s: spool %.2f, pitch %.2f, %d voices", key.c_str(), push, f.pitch, f.turbos);
    }
}

static void tickVoices(DWORD now) {
    for (size_t i = 0; i < voices.size();) {
        Voice x = voices[i];
        if ((int)(now - x.at) < 0) { i++; continue; }
        voices.erase(voices.begin() + i);
        auto known = cars.find(x.owner.ptr);
        if (!alive(x.owner) || !flutterShot || known == cars.end()) continue;
        playOn(x.owner.ptr, known->second, flutterShot, x.volume, x.pitch, x.lowpass, x.highpass);
    }
}

static int engineHas(uintptr_t vehicle);

static void tuneEngine(uintptr_t v, uintptr_t engine, Car& car, const Tune& t, DWORD now) {
    if (!canEcu || (int)(now - car.ecuAt) < 0) return;
    car.ecuAt = now + 500;
    if (!t.own && !car.trim[0].set) return;
    unsigned char who[0x10] = {0};
    memcpy(who + off(fIsServer, "WorldContextObject"), &v, 8);
    api->process_event(systemCDO, fIsServer, who);
    bool host = who[off(fIsServer, "ReturnValue")] != 0;
    car.host = host;
    double a = host ? (t.timing - 0.5) * 2 : 0, f = host ? (t.fuel - 0.5) * 2 : 0, b = host ? (t.boost - 0.5) * 2 : 0;
    double torque = 1 + 0.08 * a - 0.03 * fabs(f);
    double heat = 1 + 0.3 * (a > 0 ? a : 0) + 0.4 * (b > 0 ? b : 0);
    double burn = 1 + 0.35 * (f > 0 ? f : 0) - 0.1 * (f < 0 ? -f : 0) + 0.2 * (b > 0 ? b : 0);
    bool als = host && car.lagging;
    if (als) { heat *= 1.15; burn *= 1.25; }
    double scale[14] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 + 0.08 * a, host ? 1 + 2 * t.cool : 1};
    for (int fuel = 0; fuel < 2; fuel++) {
        scale[fuel * 6] = torque; scale[fuel * 6 + 1] = torque * (1 + 0.25 * b);
        scale[fuel * 6 + 2] = heat; scale[fuel * 6 + 3] = burn; scale[fuel * 6 + 4] = als ? 0.5 : 1;
        scale[fuel * 6 + 5] = 1 + 0.35 * b;  // BoostBar is this times the turbine speed
    }
    for (int i = 0; i < 14; i++) {
        Trim& x = car.trim[i];
        float now2 = get<float>(engine + oTrim[i]);
        if (!x.set || fabs(now2 - x.wrote) > 0.0001f) { x.base = now2; x.set = true; }
        x.wrote = (float)(x.base * scale[i]);
        if (x.wrote != now2) wr(engine + oTrim[i], &x.wrote, 4);
    }
}

static bool flag(uintptr_t obj, uintptr_t fn);

// MoreTuning's supercharger intakes are plain intake parts: their row name is all that says so,
// and it says the stage too. Without this plugin they simply run silent.
static void tickWhine(uintptr_t v, Car& car, float pedal, double ratio, uintptr_t pawn, const Vec& me, DWORD now) {
    if (!canFlutter || !fWhinePitch || !fWhineVolume || !fWhineIsOn || !fWhinePlay || !fWhineStop) return;
    if ((int)(now - car.whineCheck) >= 0) {
        car.whineCheck = now + (v == playerNow ? 1000 : 5000);
        std::string key = fittedPart(v, 5);  // EMTVehiclePartSlot::Intake
        for (auto& c : key) c = (char)tolower(c);
        size_t stage = key.find("stage");
        car.blower = key.find("supercharger") == std::string::npos ? -1 : stage != std::string::npos && stage + 5 < key.size() ? key[stage + 5] - '0' : 0;
        car.whineNear = inRange(v, pawn, me);
    }
    if (car.blower < 0 || !car.whineNear) {
        if (alive(car.whine) && flag(car.whine.ptr, fWhineIsOn)) api->process_event(car.whine.ptr, fWhineStop, nullptr);
        return;
    }
    if ((int)(now - car.whineAt) < 0) return;
    car.whineAt = now + 50;
    if (!whistle) {
        // Our own blower, in MoreTuning: a synthesized seamless loop. Failing that Teh's Hellcat at
        // the top of its revs, nearly all blower. Neither is required: without them there is no
        // whine, and asking is a blocking load so it stops. (The whine the user lived with from 11-03 to 11-07
        // and asked back: Teh's first and louder stages both read as cartoonish.)
        if (whineTries >= 3) return;
        whineTries++;
        whistle = loadAsset(L"/Game/MoreTuning/Sounds/SW_QX_BlowerWhine.SW_QX_BlowerWhine");
        whineIsOurs = whistle != 0;
        if (!whistle) whistle = loadAsset(L"/Game/Cars/Parts/Engine/Sound/hellcatv8/hellcathigh.hellcathigh");
        flutterRoom = loadAsset(L"/Game/Sounds/EngineSoundAttenuation.EngineSoundAttenuation");
        MT_LOG("supercharger whine %s", whistle ? "loaded" : "NOT LOADED");
        if (!whistle) return;
    }
    if (!alive(car.whine)) {
        uintptr_t audio = spawnKept(v, whistle, 0.001f, 1);
        if (!audio) return;
        car.whine = refOf(audio);
        // The rumble under Teh's clip is the V8. Taking that off leaves the whine for any engine.
        if (!whineIsOurs) {
            unsigned char filter[8] = {1};
            float above = 1400;
            api->process_event(audio, fHighOn, filter);
            api->process_event(audio, fHighHz, &above);
        }
        MT_LOG("supercharger on %s, stage %d, sound %s", objName(classOf(v)).c_str(), car.blower, objName(whistle).c_str());
    }
    // Belt driven, so the revs alone set it: next to nothing at idle, climbing to full at the limiter.
    // (Stage volumes up to 4.0 were tried on 11-06 and pushed the whine to the front: back to 0.5 for all.)
    double want = 0.5 * pow(ratio > 1 ? 1 : ratio, 2.5);
    car.whineLevel += (float)((want - car.whineLevel) * 0.35);
    double spin = ratio / 0.8;  // the clip is high in the rev range, so about here it plays as recorded
    float pitch = (float)((spin < 0.55 ? 0.55 : spin > 1.5 ? 1.5 : spin) * (1 + 0.03 * car.blower)), volume = car.whineLevel;
    if (volume < 0.001f) volume = 0.001f;
    api->process_event(car.whine.ptr, fWhinePitch, &pitch);
    api->process_event(car.whine.ptr, fWhineVolume, &volume);
    if (!flag(car.whine.ptr, fWhineIsOn)) {  // the wave is not a loop: start it over
        float from = 0;
        api->process_event(car.whine.ptr, fWhinePlay, &from);
    }
}

// ---- The exhaust (the Exhaust Flame Bench, on the car's real numbers) ----
// Fire needs unburnt fuel in the pipe, air, and heat. Fuel goes in from a rich map (fuel with no
// air), late timing (still burning as the valve opens), overrun fuel lit late on a lift, a spark-cut
// limiter (fuel that never fires) and anti-lag; more power and boost mean more of all of it. A cat
// burns most of it off. The pipe heats with load and cools after a lift. A small light is a pop
// you mostly hear; a big build-up is a bang you see. Fuel burnt in the pipe is power lost.
// Power is reckoned from the engine's own MaxTorque and MaxRPM; boost is the fitted turbo's own
// pressure times the turbine, as the game reckons it. The rest of the numbers are the bench's.
static double oTurboSpeedRead(uintptr_t, double speed, bool turbo) { return turbo ? (speed > 1.5 ? 1.5 : speed) : 0; }
static double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static double above0(double x) { return x > 0 ? x : 0; }

// A blocking load, so on a light or a burn and never while a world is loading.
static bool flameAssets() {
    if (sound && ball && glow && cone && (ghost || ghostTried)) return true;
    sound = loadAsset(L"/Game/Cars/Parts/Engine/Sound/Backfire/SC_Backfire.SC_Backfire");
    cone = loadAsset(L"/Engine/BasicShapes/Cone.Cone");
    ball = loadAsset(L"/Engine/BasicShapes/Sphere.Sphere");
    glow = loadAsset(L"/Game/Cars/Materials/M_AttachmentLight.M_AttachmentLight");
    ghost = ghostPath() ? loadAsset(ghostPath()) : 0;
    ghostTried = true;
    for (int w = 0; w < 6; w++) {
        wchar_t path[96];
        swprintf_s(path, L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_0%d.Backfire_0%d", w + 1, w + 1);
        waves[w] = loadAsset(path);
    }
    if (!flutterRoom) flutterRoom = loadAsset(L"/Game/Sounds/EngineSoundAttenuation.EngineSoundAttenuation");
    booms[0] = loadAsset(L"/Game/Sounds/Fireworks/FireworkBurst_01.FireworkBurst_01");
    booms[1] = loadAsset(L"/Game/Sounds/Fireworks/FireworkBurst_02.FireworkBurst_02");
    MT_LOG("sound %s, cone %d, sphere %d, glow %d, boom %d, flame material %d: %s", sound ? objName(sound).c_str() : "NOT LOADED", cone != 0, ball != 0, glow != 0, booms[0] != 0,
           kMaterial, ghost ? objName(ghost).c_str() : "solid");
    return sound && ball && glow;
}

static void lightPipe(uintptr_t v, uintptr_t engine, Car& car, const Tune& t, double amount, double scale, double ratio, float pedal, double boost,
                      double lam, double retard, double advance, bool als, bool cut, DWORD now) {
    if (!flameAssets()) return;
    double size = amount / (0.04 * (scale > 0.3 ? scale : 0.3));  // 1 = a proper bang for this engine
    bool bang = size > 0.6;
    // Seen or only heard: a small pop burns inside the pipe, and a cat hides more of them.
    double seenChance = bang ? 1 : clamp01((size - 0.08) * 1.6 * (0.5 + backOf(t))) * smooth(0.4, 0.8, car.egt);
    // A cat burns what reaches it: pops and crackles get through, flame does not, however big the shot.
    bool seen = t.decat != 0 && roll() < seenChance;
    // How hot it burnt: the pipe's heat, a lean mix and early timing hotter; rich and late cooler.
    // A pocket of fuel lit in the pipe: the more of it, the hotter it gets; it burns premixed with
    // the oxygen the gas carried, so a lean tune or a cut's fresh charge burns it blue and white.
    double conc = clamp01(size);
    Glow heat{car.mouthK + (2300 - car.mouthK) * (0.35 + 0.65 * conc), car.mouthK, clamp01(car.tipO2 / (car.tipO2 + 0.25 + 0.5 * conc))};
    // The shape comes from what this light is like: size, how fast the gas leaves, how rich it is,
    // and its rhythm (lights in the last 0.4 s). Each shape has a home among those; one is drawn
    // by closeness, so the best fit mostly wins and any shape can turn up when the stats lean.
    car.recent.push_back(now);
    while (!car.recent.empty() && now - car.recent.front() > 400) car.recent.erase(car.recent.begin());
    double st[4] = {clamp01(size / 1.5), clamp01(ratio * (0.3 + 0.7 * (pedal > 0.5 ? 1 : 0)) * (1 + boost) / 1.4 + (als ? 0.3 : 0)),
                    clamp01(above0(1 - lam) / 0.2 + (pedal < 0.3f ? 0.5 * t.ovFuel : 0)), clamp01((car.recent.size() - 1) / 5.0)};
    static const double HOMES[6][6] = {{1, 0.8, 0.65, 0.5, 0.2, 1}, {3, 0.5, 0.7, 0.4, 0.9, 1}, {4, 0.35, 0.15, 0.8, 0.2, 1}, {6, 0.15, 0.75, 0.3, 0.5, 1},
                                       {10, 0.75, 0.45, 0.5, 0.2, 0}, {0, 0.6, 0.6, 0.45, 0.3, 1.6}};  // the last: Tongues
    double score[6], total = 0;
    for (int k = 0; k < 6; k++) {
        const double* h = HOMES[k];
        double odds = k == 4 ? (t.wide != 0 ? 3 : 0.35) : h[5];
        double d = 1.2 * (st[0] - h[1]) * (st[0] - h[1]) + (st[1] - h[2]) * (st[1] - h[2]) + 0.8 * (st[2] - h[3]) * (st[2] - h[3]) + (st[3] - h[4]) * (st[3] - h[4]);
        double e = exp(-d / 0.08);
        score[k] = odds * e * e;
        total += score[k];
    }
    int shape = 1;
    double pick = roll() * total;
    for (int k = 0; k < 6; k++) { pick -= score[k]; if (pick <= 0) { shape = (int)HOMES[k][0]; break; } }
    int kind = als ? ALS : bang ? BANG : shape == 4 ? RICH : DECEL;
    const Shot& shot = bang ? BANGS[rand() % 6] : size > 0.3 ? POPS[rand() % 2 ? 0 : 5] : POPS[UNBURNT_POPS[rand() % 3]];
    // A crackle dies away: quieter from the moment of the lift on, and with the revs, so the tail
    // of a long overrun or an engine dropping to idle is not as loud as the first pops.
    double loud = clamp01(ratio * 1.6);
    if (pedal < 0.3f && car.liftAt) loud *= exp(-(now - car.liftAt) / 1400.0);
    fire(v, engine, car, t, shot, bang ? (st[0] > 0.6 ? st[0] : 0.6) : st[0], ratio, bang, kind, shape, heat, seen, now, loud);
    if (logged < 40 && loggingOn()) {
        logged++;
        MT_LOG("%s on %s: %.0f mg%s, shape %d, peak %.0f C premixed %.2f, pipe %.0f/%.0f/%.0f/%.0f/%.0f C mouth %.0f C, size %.1f speed %.1f rich %.1f rhythm %.1f%s",
               bang ? "BANG" : "pop", objName(classOf(v)).c_str(), amount * 1000, seen ? " seen" : " heard", shape, heat.peakK - 273, heat.premix,
               car.wall[0] - 273, car.wall[1] - 273, car.wall[2] - 273, car.wall[3] - 273, car.wall[4] - 273, car.mouthK - 273, st[0], st[1], st[2], st[3], cut ? " (limiter)" : "");
    }
}

// The gas from the manifold to the tip, in five sections: manifold, downpipe, cat, mid pipe,
// tip. It leaves the engine at a temperature the tune sets, carrying unburnt fuel and oxygen
// (both as shares of a full charge). Each section takes heat from it towards its wall, which warms
// slowly and loses heat to the air, the further back the more. Where the gas is past about 550 C
// and has both, the fuel burns and heats it, 1700 K for a whole charge; a lit cat (past about
// 250 C) burns nearly all of it, which is why it glows on overrun. What reaches the tip sets the
// flame: its temperature, how much fuel is left to burn there, and the oxygen it has with it.
static void pipeHeat(Car& car, const Tune& t, double ratio, bool on, bool cut, bool als, double lam, double retard, double advance,
                     double boost, double step) {
    double K, fuel, o2;
    if (als) { K = 1350; fuel = 0.3; o2 = 0.35; }  // anti-lag: fuel and air lit in the manifold
    else if (cut) { K = 950; fuel = t.spark != 0 ? 1 : 0; o2 = 1; }  // the cut charge leaves whole, spark cut keeps its fuel
    else if (on) {
        // Wide open a petrol engine's gas leaves at 850-950 C; late timing burns into the pipe,
        // early timing finishes in the cylinder, a rich map cools it.
        K = 780 + 380 * ratio + 230 * retard - 70 * advance + 120 * boost - 300 * above0(1 - lam);
        fuel = above0(1 / lam - 1) + 0.08 * retard;
        o2 = above0(1 - 1 / lam) + 0.03;
    } else {
        // a crackle map adds up to a third of a charge
        double inj = ratio > t.revs && t.ovFuel > 0.02 ? 0.3 * t.ovFuel : 0, late = 0.25 + 0.6 * t.ovRetard;
        K = 560 + 160 * ratio + 550 * inj * t.ovRetard;  // closed throttle pumps cool air, unless fuel burns late
        fuel = inj * late;
        o2 = 1 - inj * (1 - late) > 0.2 ? 1 - inj * (1 - late) : 0.2;
    }
    static const double TAKE[5] = {0.10, 0.12, 0.15, 0.18, 0.12}, LOSE[5] = {0.02, 0.03, 0.04, 0.06, 0.08};
    double bp = backOf(t), flow = 0.15 + 0.85 * ratio * (on ? 1 : 0.5), res = (1 / flow > 6 ? 6 : 1 / flow) * (0.6 + 0.8 * bp);
    if (on && !als) K += 100 * bp;  // pumping against it heats the gas
    for (int i = 0; i < 5; i++) {
        double& wall = car.wall[i];
        K = wall + (K - wall) * exp(-TAKE[i] * res);
        bool cat = i == 2 && t.decat == 0;
        double lit = cat ? smooth(500, 620, wall) : smooth(820, 980, K);
        double b = (fuel < o2 ? fuel : o2) * lit * (cat ? 0.9 : clamp01(0.25 * res));
        fuel -= b; o2 -= b;
        K += 1700 * b;
        K = K > 2300 ? 2300 : K;
        double take = 0.25 * flow, want = (K * take + 300 * LOSE[i]) / (take + LOSE[i]);
        wall += (want - wall) * clamp01(step / 8);
    }
    double k = clamp01(step / 0.15);  // the mouth averages over a few firings, so a limiter's cuts do not flicker
    car.mouthK += (K - car.mouthK) * k;
    car.tipFuel += (fuel - car.tipFuel) * k;
    car.tipO2 += (o2 - car.tipO2) * k;
}

// The flames burn real fuel (the user, 2026-11-09): what the tune sends into the pipe (rich, overrun fuel, anti-lag) comes
// out of the tank. The rate is learnt per car: over 5 s windows on throttle, the tank's own drop against the model's flow
// gives tank units per unit of flow, so whatever the game's units and the engine's consumption, a pipe full of fuel costs
// what that much fuel costs the engine. Only where this machine owns the vehicle's state (the server, single player).
static void burnFuel(uintptr_t v, Car& car, double toPipe, double flow, bool learning, double step) {
    if (!car.host || oTankFuel < 0 || step <= 0) return;
    float tank = get<float>(v + oTankFuel), cap = oTankCap >= 0 ? get<float>(v + oTankCap) : 0;
    if (!(tank >= 0) || (cap > 0 && tank > cap * 1.01f) || tank > 100000) { car.tankLast = -1; return; }
    if (car.tankLast >= 0 && learning) {
        car.learnDrop += car.tankLast - tank; car.learnFlow += flow * step; car.learnT += step;
        if (car.learnT >= 5) {
            if (car.learnFlow > 0.5 && car.learnDrop > 0) {
                double k = car.learnDrop / car.learnFlow;
                car.burnK = car.burnK <= 0 ? k : car.burnK + 0.3 * (k - car.burnK);
            }
            car.learnDrop = car.learnFlow = car.learnT = 0;
        }
    } else car.learnDrop = car.learnFlow = car.learnT = 0;
    double extra = car.burnK * toPipe * step;
    if (extra > 0) {
        tank = (float)(tank > extra ? tank - extra : 0);
        wr(v + oTankFuel, &tank, 4);
    }
    car.tankLast = tank;
}

static void exhaust(uintptr_t v, uintptr_t engine, Car& car, const Tune& t, double ratio, float throttle, float pedal, bool als, double turbine, double dt, DWORD now, bool inView) {
    if (dt <= 0) return;
    uintptr_t data = get<uintptr_t>(engine + oData);
    double hp = oTorque >= 0 ? get<float>(data + oProp + oTorque) * get<float>(data + oProp + oMax) * 0.8 / 7121 : 400;
    double scale = hp / 400;
    scale = scale < 0.2 ? 0.2 : scale > 4 ? 4 : scale;
    int map = (engineHas(v) & DIESEL) ? 6 : 0;
    double boost = turbine > 0 ? clamp01(turbine * get<float>(engine + oTrim[map + 5]) / 2) : 0;
    double lam = 1.15 - 0.35 * t.fuel, retard = above0((0.5 - t.timing) * 2), advance = above0((t.timing - 0.5) * 2);
    bool on = pedal > 0.5f, lifted = pedal < 0.3f;
    // At the limit with the pedal down it is bouncing off the limiter. The engine's own throttle does
    // not always drop there, so the cut is the throttle shutting, or else every other 60 ms.
    bool atLimit = on && ratio >= 0.96;
    bool cut = atLimit && (throttle < 0.3f || ((now / 60) & 1));
    double flow = ratio * (0.12 + 0.88 * (on ? 1 : 0)) * scale * (1 + boost), toPipe = 0, heatWant, loss = 0;
    if (on) {
        if (cut) { if (t.spark != 0) { toPipe += flow; loss = 1; } }  // spark cut: in it goes, and never fires
        else {
            double rich = above0(1 - lam), late = 0.25 * retard;
            toPipe += flow * (rich + 0.3 * late);
            loss = rich + late * 0.6;
        }
        heatWant = 0.35 + 0.4 * ratio + 0.2 * retard + 0.15 * boost - 0.08 * advance;
    } else {
        double since = car.liftAt ? (now - car.liftAt) / 1000.0 : 10;
        if (ratio > t.revs && t.ovFuel > 0.02) {
            double inj = t.ovFuel * ratio * 0.35 * scale;
            toPipe += inj * (0.25 + 0.6 * t.ovRetard);
            heatWant = 0.3 + 0.35 * t.ovFuel * t.ovRetard + 0.25 * exp(-since / 1.5);
        } else heatWant = 0.22 + 0.25 * exp(-since / 1.2);
        if (als) { toPipe += 0.45 * scale * (1 + t.boost); heatWant = 0.95; loss = 0.35; }
    }
    double step = dt > 0.4 ? 0.4 : dt;
    burnFuel(v, car, toPipe, flow, on && !cut && ratio > 0.3, step);
    car.loss += (loss - car.loss) * (step / 0.2 > 1 ? 1 : step / 0.2);
    double tau = heatWant > car.egt ? 1.2 : 2.6;
    car.egt += ((heatWant > 1 ? 1 : heatWant) - car.egt) * (step / tau > 1 ? 1 : step / tau);
    if (on && !cut) toPipe *= 0.4;  // under load a rich mix mostly leaves unlit: there is no air in the pipe to burn it
    if (t.decat == 0) toPipe *= 0.25;  // a cat oxidises most of it before it reaches the tip
    car.pipeFuel += toPipe * step;
    if (t.decat == 0) car.pipeFuel -= car.pipeFuel * clamp01(step * 2.2 * smooth(0.3, 0.6, car.egt));
    double air = clamp01(0.15 + 1.5 * above0(lam - 1) + (lifted ? 0.45 : 0) + (als ? 0.35 : 0) + (cut ? 0.25 : 0));
    pipeHeat(car, t, ratio, on, cut, als, lam, retard, advance, boost, step);
    // Small lights are rarer and spaced, so fuel can build; a bang's worth lights far more readily
    // and goes almost all at once. That is what makes a bang a bang and not more crackle.
    // On throttle the pipe does not pop. Unburnt fuel burns as it leaves, a flame that grows with
    // the revs and the fuel; what does not burn off builds until it goes as one boom, and the flame
    // goes out for a moment and comes back bigger. Pops and crackle are for lifts and the limiter.
    // On throttle the engine is burning its fuel anyway and the gas never stops moving: nothing
    // pops or bangs. What is too much leaves as flame, silently, with no ignition behind it -- and
    // past 60% of a bang's worth the rest goes out that way instead of building. The pipe fills on
    // a lift, when the revs drop and nothing eats the fuel: that is where pops and bangs come from.
    bool pulling = on && !atLimit;
    double level = 0.035 * (scale > 0.3 ? scale : 0.3);
    if (pulling) {
        // Only fuel that reaches the tip, at a mouth hot enough to light it: a rich or late map, no
        // cat to eat it. And it does not hold: the gas speeds up and the mix leans as the engine
        // settles into the pull, so a standing flame shrinks away over a second or two.
        double want = clamp01((car.tipFuel - 0.05) / 0.25) * smooth(950, 1150, car.mouthK) * (0.4 + 1.2 * backOf(t));
        // Not at the limit: bouncing off it, every cut sends fresh air into a hot pipe full of what
        // is left, and that is a bang. So up there the excess stays in the pipe for the cut to light.
        if (car.pipeFuel > 0.6 * level && ratio < 0.95) {
            double over = 0.5 * clamp01((car.pipeFuel / level - 0.6) * 2);
            want = want > over ? want : over;
            car.pipeFuel -= (car.pipeFuel - 0.6 * level) * clamp01(step * 3);
        }
        // It takes a pipe soaked in heat and fuel: five seconds and more held near the limit. Then
        // the fuel keeps coming faster than it leaves and the flame stands at the tip.
        car.soak = ratio > 0.85 ? car.soak + step : car.soak - 2 * step;
        car.soak = car.soak < 0 ? 0 : car.soak > 12 ? 12 : car.soak;
        want *= smooth(5, 8, car.soak);
        car.burn += (want - car.burn) * (step / 0.6 > 1 ? 1 : step / 0.6);
        car.pipeFuel -= car.pipeFuel * clamp01(step * 0.8 * car.burn);
    } else { car.burn -= car.burn * (step / 0.15 > 1 ? 1 : step / 0.15); car.soak -= car.soak * clamp01(step); }
    // A lift does not bang at once. The last rich charges are still in the pipe and leave as a
    // quiet, narrow lick of flame; the overrun fuel has to reach the hot pipe first. So for
    // 350 ms after the lift nothing pops, and the lick burns what there is.
    bool justLifted = lifted && car.liftAt && (int)(now - car.liftAt) < 350;
    if (on) car.licked = false;
    if (justLifted && !car.licked && inView && base.flames != 0 && t.decat != 0 && car.mouthK > 820 && (car.tipFuel > 0.02 || car.pipeFuel > 0.002 || car.burn > 0.05) && flameAssets()) {
        car.licked = true;
        if (!car.read) readPipes(v, engine, car);
        firePoints(car, t);
        double p = clamp01(car.tipO2 / (car.tipO2 + car.tipFuel + 0.02));
        Glow heat{car.mouthK + (2150 - car.mouthK) * clamp01(0.3 + car.tipFuel * 2), car.mouthK, p > 0.6 ? 0.6 : p};
        double e = clamp01(0.3 + 2 * car.tipFuel + car.burn);
        for (int i = 0; i < (int)car.fire.size(); i++) {
            car.slots[i].flameHue = heat;
            flameClassic(v, car, i, t, e * 0.6, 0.7, 1.1, 0.3, 0, RICH, ratio);  // Classic: the piece shape read as odd here
            car.slots[i].spread *= 0.35;  // contained: a lick, not a burst
        }
    }
    bool big = car.pipeFuel > level;
    double rate = pulling || justLifted ? 0 : (big ? 30 : lifted ? 9 : 4) * smooth(0.32, 0.62, car.egt) * air * car.pipeFuel / (car.pipeFuel + 0.03);
    if (ratio < 0.05) { car.pipeFuel -= car.pipeFuel * clamp01(step * 3); rate = 0; }  // engine stopped: nothing pushes it out, it does not light
    if (car.pipeFuel > 0.002 && roll() < 1 - exp(-rate * step) && (int)(now - car.lastLight) > (big ? 40 : 90)) {
        double amount = car.pipeFuel * (big ? 0.85 + 0.15 * roll() : 0.3 + 0.4 * roll());
        car.pipeFuel -= amount;
        car.lastLight = now;
        if (inView) lightPipe(v, engine, car, t, amount, scale, ratio, pedal, boost, lam, retard, advance, als, cut, now);
        if (big) { car.burnPause = now + 450; car.burn *= 0.3; }  // a bang blows any flame still at the pipe out
    }
    // The burn itself: a jet of cones off every pipe, refreshed so it licks and grows.
    if (inView && base.flames != 0 && t.decat != 0 && car.burn > 0.12 && (int)(now - car.burnPause) >= 0 && (int)(now - car.burnNext) >= 0 && flameAssets()) {
        car.burnNext = now + 140;
        if (!car.read) readPipes(v, engine, car);
        firePoints(car, t);
        // What is left burns only where it meets the air at the tip: sooty, unless the gas brought oxygen.
        double p = clamp01(car.tipO2 / (car.tipO2 + car.tipFuel + 0.02));
        Glow heat{car.mouthK + (2250 - car.mouthK) * clamp01(car.tipFuel * 2.5), car.mouthK, p > 0.5 ? 0.5 : p};
        for (int i = 0; i < (int)car.fire.size(); i++) {
            car.slots[i].flameHue = heat;
            flameClassic(v, car, i, t, car.burn, 0.22, 0.8 + 1.4 * car.burn, 0.6, 1, DECEL, ratio);
        }
    }
}

// The replicated byte once it has ever moved (a stuck 0 means it is not sent), else the car's field.
static float otherPedal(float field, uint8_t rep, bool& seen) {
    if (rep > 12) seen = true;
    return seen ? rep / 255.0f : field;
}

static void one(uintptr_t v, uintptr_t pawn, const Vec& me, double dt, DWORD now) {
    uintptr_t engine = get<uintptr_t>(v + oEngine);
    uintptr_t data = engine ? get<uintptr_t>(engine + oData) : 0;
    if (!data) return;
    float prob = get<float>(data + oProp + oAfter), maxRpm = get<float>(data + oProp + oMax);
    int has = engineHas(v);
    if (maxRpm <= 0 || !has) return;  // electric: no map, no exhaust

    const Tune& t = tuneFor(v);
    Car& car = cars[v];
    // A new car the game put where a gone one was: its pipes, sockets and slots are not this one's.
    if (car.self.ptr && car.self.index != get<int>(v + L().obj_index)) { stopSmoke(car); car = Car{}; }
    car.self = refOf(v);
    tuneEngine(v, engine, car, t, now);
    applySockets(v, engine, car, t, now);
    float rpm = get<float>(engine + oHot + oRpm), throttle = get<float>(engine + oHot + oThrottle);
    // The engine's own throttle drops at the limiter and on every shift. Only the pedal is a lift.
    float pedal = throttle;
    if (oPedal >= 0) {
        float raw = get<float>(v + oPedal);
        // Someone else's car: its pedal reaches this machine in the replicated movement, a byte the
        // server sends on, while the car's own field is not kept up here (no input drives it).
        if (v != playerNow && oRepPedal >= 0) raw = otherPedal(raw, get<uint8_t>(v + oRepPedal), car.repSeen);
        if (raw > 0.05f) car.pedalSeen = true;
        if (car.pedalSeen) pedal = raw;
    }
    double ratio = rpm / maxRpm;
    if (throttle > 0.05f) car.throttleSeen = true;
    if (!car.pedalSeen && !car.throttleSeen && ratio > 0.05 && dt > 0) {
        // Someone else's car can arrive on this machine with neither its pedal nor its throttle (the
        // host gets neither for a client's car), only its revs. Then the revs are the pedal:
        // climbing is on throttle, a fast fall from high revs is a lift.
        double rate = (ratio - car.lastRatio) / dt;
        pedal = rate > 0.12 ? 1.0f : rate < -0.3 ? 0.0f : car.guess;
        car.guess = pedal;
    }
    car.lastRatio = (float)ratio;
    if (v != playerNow && loggingOn() && (int)(now - car.seenAt) >= 0) {
        // What this machine sees of other players' cars, to tell a missing tune from a missing pedal.
        // Only running engines that no AI drives: parked cars at 0 revs used the whole budget.
        static int lines;
        Tune carried;
        bool player = readTune(v, carried), ai = oAiDriving >= 0 && get<uint8_t>(v + oAiDriving);
        if (ratio > 0.05 && !ai && lines < 300) {
            lines++;
            MT_LOG("other car %s: revs %.2f throttle %.2f pedal %.2f (read from %s), %s, overrun fuel %.2f cat delete %.0f",
                   objName(classOf(v)).c_str(), ratio, throttle, pedal, car.repSeen ? "replicated pedal" : car.pedalSeen ? "pedal" : car.throttleSeen ? "throttle" : "revs",
                   player ? "tune carried" : "no tune carried (this machine's)", t.ovFuel, t.decat);
        }
        car.seenAt = now + 3000;
    }
    bool turbo = oTurbo >= 0 && get<uint8_t>(engine + oTurbo);
    if (pedal > 0.3f) car.armed = true;
    bool lifted = car.armed && pedal < 0.1f;
    float brake = oBrake >= 0 ? get<float>(v + oBrake) : 0;
    if (lifted) car.alsUntil = now + 2500;  // heat is what limits it, so it only runs for a moment
    bool lagging = t.lag != 0 && turbo && pedal < 0.1f && brake < 0.1f && ratio >= t.lagRevs && (int)(now - car.alsUntil) < 0;
    car.lagging = lagging;
    double speed = car.spool;
    if (oTurbineSpeed >= 0) {
        speed = get<float>(engine + oTurbineSpeed);
        if (lagging && car.host && speed < 0.85) {  // anti-lag: the turbo stays spun up, whatever the pedal does
            float held = 0.85f;
            wr(engine + oTurbineSpeed, &held, 4);
            speed = held;
        }
    } else {
        double breath = sqrt(ratio) * pedal, step = dt > 0.05 ? 0.05 : dt;
        car.spool += (float)((breath - car.spool) * step * (breath > car.spool ? 2 : 0.8));
    }
    tickWhine(v, car, pedal, ratio, pawn, me, now);
    if (lifted) {
        car.armed = false;
        car.liftAt = now;
        // Mostly the turbine, and the revs so a high-rev lift is always the loud one.
        double push = 0.6 * (speed > 1 ? 1 : speed) + 0.4 * ratio * ratio;  // the turbine reads past 1
        if (v == playerNow && flutterLogged < 300) {
            flutterLogged++;
            MT_LOG("lift: revs %.2f turbine %.2f push %.2f turbo %d anti-lag %d", ratio, speed, push, turbo, lagging);
        }
        // Only an engine that revs flutters: a truck diesel tops out too low to hear it, and a lift
        // from low in the range has nothing built up to dump.
        if (turbo && maxRpm >= 4000 && push > 0.45 && ratio >= 0.5 && (int)(now - car.flutterAt) >= 0 && inRange(v, pawn, me)) {
            car.flutterAt = now + 1500;
            queueFlutter(v, t, push * (0.35 + 1.3 * (ratio - 0.5)), now);  // quiet just over half revs, full at the limiter
        }
    }
    car.fuel = pedal > car.fuel ? pedal : car.fuel * (float)(1 - (dt > 0.05 ? 0.05 : dt));  // fades over a second or so
    car.rpm = rpm;
    // Another player's car can arrive here with revs but no throttle (a client never gets the host's):
    // then the load is the pedal read off the revs, or its smoke never shows on this machine.
    float load = car.throttleSeen ? throttle : pedal;
    if ((has & DIESEL) && (t.dpf != 0 || !car.smokes.empty())) tickSmoke(v, engine, car, t, rpm, load, inRange(v, pawn, me));

    if (prob <= 0) return;  // the game's own word on whether this engine can fire in the pipe at all
    if (get<int>(engine + oNative + 8)) { int none = 0; wr(engine + oNative + 8, &none, 4); }  // silences the game's own
    exhaust(v, engine, car, t, ratio, throttle, pedal, lagging, oTurboSpeedRead(engine, speed, turbo), dt, now, inRange(v, pawn, me));
}

static bool isA(uintptr_t o, uintptr_t cls) { return o && api->is_a(classOf(o), cls); }

static bool flag(uintptr_t obj, uintptr_t fn) {
    unsigned char p[0x10] = {0};
    api->process_event(obj, fn, p);
    return p[0] != 0;
}

static void setText(uintptr_t block, const wchar_t* text) {
    unsigned char p[0x40] = {0};
    FStr s = str(text);
    memcpy(p + pTextIn, &s, sizeof s);
    api->process_event(textCDO, fToText, p);
    api->process_event(block, isA(block, cText) ? fSetText : fSetRich, p + pTextRet);
}

static uintptr_t create(uintptr_t screen, uintptr_t cls, uintptr_t box) {
    struct { uintptr_t context, cls, player, ret; } c = {screen, cls, 0, 0};
    api->process_event(widgetCDO, fCreate, &c);
    if (!c.ret) return 0;
    struct { uintptr_t content, slot; } add = {c.ret, 0};
    api->process_event(box, fAddChild, &add);
    float pad[4] = {5, 5, 5, 5};  // what the game gives its own
    if (add.slot) api->process_event(add.slot, fPadding, pad);
    return c.ret;
}

static int childIndex(uintptr_t box, uintptr_t child) {
    struct { uintptr_t content; int ret; } at = {child, -1};
    if (child) api->process_event(box, fChildIndex, &at);
    return at.ret;
}

// 0 for anything that burns no petrol or diesel: an EV has no ECU map and no exhaust.
// A row shows when the engine has what it needs, and socket rows only for sockets the car has.
static int engineHas(uintptr_t vehicle);
static bool rowShown(uintptr_t vehicle, int i) {
    int needs = ROWS[i].needs;
    if (needs >= PIPE1) {
        uintptr_t engine = vehicle ? get<uintptr_t>(vehicle + oEngine) : 0;
        if (!engine) return false;
        Car& car = cars[vehicle];
        if (!car.read) readPipes(vehicle, engine, car);
        int k = needs == PIPE1 ? 1 : needs == PIPE2 ? 2 : needs == PIPE3 ? 3 : 4;
        return engineHas(vehicle) != 0 && (int)car.pipes.size() >= k;
    }
    return (engineHas(vehicle) & needs) != 0;
}

static int engineHas(uintptr_t vehicle) {
    uintptr_t engine = vehicle ? get<uintptr_t>(vehicle + oEngine) : 0;
    uintptr_t data = engine ? get<uintptr_t>(engine + oData) : 0;
    if (!data || get<float>(data + oProp + oMotor) > 0) return 0;
    uint8_t fuel = get<uint8_t>(data + oProp + oFuelType);  // EMTFuelType: 1 gasoline, 2 diesel
    if (fuel != 1 && fuel != 2) return 0;
    int has = BURNS;
    if (get<float>(data + oProp + oAfter) > 0) has |= POPS_;
    if (fuel == 2) has |= DIESEL;
    if (oTurbo >= 0 && get<uint8_t>(engine + oTurbo)) has |= TURBO;
    return has;
}

static void setVisible(uintptr_t widget, bool visible) {
    uint8_t v = visible ? 0 : 1;  // ESlateVisibility: Visible, Collapsed
    api->process_event(widget, fSetVis, &v);
}

// True on the release of a press, which is when a real button counts as clicked.
static bool clicked(uintptr_t button, bool& held) {
    bool now = flag(button, fPressed);
    bool up = held && !now;
    held = now;
    return up;
}

static void showRow(int i, const Tune& t) {
    double v = t.*ROWS[i].field;
    wchar_t text[32];
    if (textRow(ROWS[i].kind)) { setText(panel.rows[i].value, L""); return; }
    if (ROWS[i].field == &Tune::spark) swprintf_s(text, L"%s", v != 0 ? L"Spark cut" : L"Fuel cut");
    else if (ROWS[i].kind == TOGGLE) swprintf_s(text, L"%s", v != 0 ? L"On" : L"Off");
    else swprintf_s(text, stepOf(i) == 1 ? L"%.0f" : L"%.2f", toUi(i, t));
    setText(panel.rows[i].value, text);
}

// W_Button takes its label from ButtonName when it is built. The text is moved in raw: its
// reference came from the buffer, which nothing destroys.
static void labelButton(uintptr_t button, const wchar_t* text) {
    unsigned char p[0x40] = {0};
    FStr s = str(text);
    memcpy(p + pTextIn, &s, sizeof s);
    api->process_event(textCDO, fToText, p);
    wr(button + oBtnText, p + pTextRet, 16);
}

// W_Button rewrites its text blocks as it is built, a frame or more after AddChild.
static void labelBlocks(uintptr_t button, const wchar_t* text) {
    for (int at : {oBtnName, oBtnRich}) {
        uintptr_t block = get<uintptr_t>(button + at);
        if (block) setText(block, text);
    }
}

static void saveTune(int64_t id, const Tune& t) {
    tuned.insert(section(id));
    for (auto& k : KEYS) {
        wchar_t v[32];
        swprintf_s(v, L"%g", t.*k.field);
        iniSet(L"ecu_tuning.ini", section(id).c_str(), k.key, v, true);
    }
}

static void openRows(uintptr_t screen, uintptr_t box, const Tune& t, uintptr_t vehicle) {
    struct { int ret; } count = {0};
    api->process_event(box, fChildCount, &count);
    panel.hidden.clear();
    for (int i = 0; i < count.ret; i++) {
        struct { int index; uintptr_t ret; } at = {i, 0};
        api->process_event(box, fChildAt, &at);
        if (at.ret) { setVisible(at.ret, false); panel.hidden.push_back(at.ret); }
    }
    for (int i = 0; i < NROWS; i++) {
        uintptr_t w = create(screen, cSetting, box);
        uintptr_t spin = w ? get<uintptr_t>(w + oRowSpin) : 0;
        if (!spin) { MT_LOG("could not create the ECU rows"); return; }
        panel.rows[i] = {w, spin, get<uintptr_t>(spin + oSpinText)};
        setText(get<uintptr_t>(w + oRowName), ROWS[i].label);
        if (textRow(ROWS[i].kind)) {  // a section title: its name, no arrows
            setVisible(spin, false);
            setVisible(w, rowShown(vehicle, i));
            continue;
        }
        if (spinRange) {
            spinRange(spin, 1, (float)spinTop(i, t), 0.01f, true);
            spinSet(spin, (float)toSpin(i, t));
        }
        showRow(i, t);
        setVisible(w, rowShown(vehicle, i));
    }
    // Under the rows: back to the defaults ([ECU] in the ini).
    panel.reset = create(screen, cButton, box);
    if (panel.reset) {
        labelButton(panel.reset, L"Reset to default");
        panel.resetInner = get<uintptr_t>(panel.reset + oBtnInner);
        panel.resetHeld = false;
        panel.resetFrames = 60;
    }
    panel.open = true;
}

static void closeRows() {
    for (auto& r : panel.rows)
        if (r.widget) api->process_event(r.widget, fRemove, nullptr);
    if (panel.reset) api->process_event(panel.reset, fRemove, nullptr);
    panel.reset = 0;
    for (uintptr_t w : panel.hidden) setVisible(w, true);
    panel.hidden.clear();
    memset(panel.rows, 0, sizeof panel.rows);
    panel.open = false;
}

// Hold H in the ECU rows to rev the engine and hear and see the tune, as the horn rows let you
// hear the horn: the engine's throttle and the pedal the exhaust model reads, both held open.
static uintptr_t fSetThrottle, fSetStarter, fIgnitionOn;
static bool revving;
static void revWithH(uintptr_t vehicle, bool menuOpen) {
    if (!fSetThrottle) {
        fSetThrottle = api->find_object("Function", "SetThrottle", "MTEngineComponent");
        fSetStarter = api->find_object("Function", "SetStarterOn", "MTEngineComponent");
        fIgnitionOn = api->find_object("Function", "IsIgnitionOn", "MTEngineComponent");
        if (!fSetThrottle) return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool want = menuOpen && vehicle && pid == GetCurrentProcessId() && (GetAsyncKeyState('H') & 0x8000);
    if (!want && !revving) return;
    uintptr_t engine = vehicle ? get<uintptr_t>(vehicle + oEngine) : 0;
    if (!engine) { revving = false; return; }
    float open = want ? 1.0f : 0.0f;
    unsigned char q[0x10] = {0};
    memcpy(q, &open, 4);
    api->process_event(engine, fSetThrottle, q);
    if (oPedal >= 0) wr(vehicle + oPedal, &open, 4);
    if (want && fIgnitionOn && fSetStarter && !flag(engine, fIgnitionOn)) {  // off: crank it first
        unsigned char on[0x10] = {1};
        api->process_event(engine, fSetStarter, on);
    }
    revving = want;
}

static void tickPanel(DWORD now) {
    if (panel.dirty && now - panel.dirty > 800) { saveTune(panel.id, tuneOf(panel.id)); panel.dirty = 0; }
    uintptr_t screen = 0, box = 0;
    for (auto& r : screens) {
        if (!alive(r)) continue;
        uintptr_t b = get<uintptr_t>(r.ptr + oBox);
        if (b && flag(r.ptr, fIsVisible)) { screen = r.ptr; box = b; break; }
    }
    // shortcut: menu timing in the log, to find why the button comes late or twice. Remove once known.
    static uintptr_t shownScreen;
    static DWORD shownAt;
    static int waits;
    if (screen != shownScreen) {
        if (screen) MT_LOG("menu %lu: garage screen %p showing (%d known)", now, (void*)screen, (int)screens.size());
        else MT_LOG("menu %lu: garage screen closed", now);
        shownScreen = screen; shownAt = now; waits = 0;
    }
    if (!screen) {  // left with the rows open: the menu must be back next time
        if (panel.open && alive(panel.screen)) closeRows();
        return;
    }
    uintptr_t vehicle = get<uintptr_t>(screen + oVehicle);
    int64_t id = vehicle ? get<int64_t>(vehicle + oId) : 0;
    if (!id) { if (waits++ == 0) MT_LOG("menu %lu: waiting, the screen has no vehicle id yet", now); return; }
    if (panel.screen.ptr != screen) {
        if (panel.dirty) saveTune(panel.id, tuneOf(panel.id));
        if (panel.open && alive(panel.screen)) closeRows();  // else they stay on the old screen beside new ones
        panel = {};
        panel.screen = {get<int>(screen + L().obj_index), screen};
    }
    Tune& t = tuneOf(id);
    revWithH(vehicle, panel.open);
    if (panel.open) {
        if (childIndex(box, panel.rows[0].widget) < 0) {  // the game rebuilt the list and our rows with it
            panel.open = false; panel.button = 0; panel.reset = 0; panel.hidden.clear();
                    memset(panel.rows, 0, sizeof panel.rows);
            return;
        }
        if (panel.reset && childIndex(box, panel.reset) >= 0) {
            if (panel.resetFrames > 0) { panel.resetFrames--; labelBlocks(panel.reset, L"Reset to default"); }
            if (clicked(panel.resetInner, panel.resetHeld)) {
                // Saved as the defaults, not forgotten: the car then carries the reset to other players too.
                t = base;
                t.own = true;
                saveTune(id, t);
                panel.dirty = 0;
                for (int i = 0; i < NROWS; i++) {
                    if (!textRow(ROWS[i].kind) && spinSet) spinSet(panel.rows[i].spin, (float)toSpin(i, t));
                    showRow(i, t);
                }
                MT_LOG("tune reset to default for vehicle %lld", (long long)id);
                return;
            }
        }
        if (panel.id != id) {
            if (panel.dirty) { saveTune(panel.id, tuneOf(panel.id)); panel.dirty = 0; }
            panel.id = id;
                    for (int i = 0; i < NROWS; i++) {
                if (textRow(ROWS[i].kind)) { setVisible(panel.rows[i].widget, rowShown(vehicle, i)); continue; }
                if (spinRange) spinRange(panel.rows[i].spin, 1, (float)spinTop(i, t), 0.01f, true);
                if (spinSet) spinSet(panel.rows[i].spin, (float)toSpin(i, t));
                showRow(i, t);
                setVisible(panel.rows[i].widget, rowShown(vehicle, i));
            }
            return;
        }
        for (int i = 0; spinRange && i < NROWS; i++) {
            if (textRow(ROWS[i].kind)) continue;
            float v = get<float>(panel.rows[i].spin + oSpinValue);
            // Held at the end of its range the spin box keeps writing its own step count over the row.
            if (fabs(v - toSpin(i, t)) < 0.002) { showRow(i, t); continue; }
            t.*ROWS[i].field = fromUi(i, fromSpin(i, v, t));
            for (int k = 0; k < 4; k++)  // switching off the last pipe that is on switches it back on
                if (ROWS[i].field == PIPE_ON[k] && t.*PIPE_ON[k] == 0) {
                    bool any = false;
                    for (int j = 0; j < 4; j++) any = any || (j != k && t.*PIPE_ON[j] != 0 && rowShown(vehicle, i + (j - k) * 4));
                    if (!any) { t.*PIPE_ON[k] = 1; if (spinSet) spinSet(panel.rows[i].spin, (float)toSpin(i, t)); }
                }
            t.own = true;
            panel.dirty = now;
            showRow(i, t);
        }
        return;
    }
    if (childIndex(box, panel.button) >= 0) {
        if (panel.labelFrames > 0) { panel.labelFrames--; labelBlocks(panel.button, L"ECU Tuning"); }
        if (clicked(panel.inner, panel.buttonHeld)) { panel.id = id; openRows(screen, box, t, vehicle); }
        return;
    }
    // Only on the game's menu: on the wheel rows the first child is a W_Setting, not a button.
    struct { int index; uintptr_t ret; } first = {0, 0};
    api->process_event(box, fChildAt, &first);
    if (!isA(first.ret, cButtonBase)) {
        if (panel.button) MT_LOG("menu %lu: lost the button, the list starts with %s", now, first.ret ? objName(classOf(first.ret)).c_str() : "nothing");
        panel.button = 0;
        return;
    }
    if (!engineHas(vehicle)) { panel.button = 0; return; }
    // The game's buttons hidden: another plugin's rows are open (License Plate), and a button made now
    // would sit among them. Made once they close.
    if (!flag(first.ret, fIsVisible)) return;
    // A button made before, still in the list: the panel lost track of it (another screen, the
    // list showing something else a moment), and making one more is what put two in the menu.
    static std::vector<Ref> made;
    for (auto& r : made)
        if (alive(r) && childIndex(box, r.ptr) >= 0) {
            panel.button = r.ptr;
            panel.inner = get<uintptr_t>(r.ptr + oBtnInner);
            panel.buttonHeld = false;
            MT_LOG("menu %lu: the button was still in the list, kept it (a second one before this fix)", now);
            return;
        }
    made.erase(std::remove_if(made.begin(), made.end(), [](const Ref& r) { return !alive(r); }), made.end());
    panel.button = create(screen, cButton, box);
    if (!panel.button) return;
    made.push_back(refOf(panel.button));
    MT_LOG("menu %lu: button made, %lu ms after the screen showed", now, now - shownAt);
    labelButton(panel.button, L"ECU Tuning");
    panel.labelFrames = 60;
    panel.inner = get<uintptr_t>(panel.button + oBtnInner);
    panel.buttonHeld = false;
    if (!isA(panel.inner, cUButton)) { MT_LOG("the menu button has no Button inside -- Exhaust cannot be clicked"); }
    MT_LOG("Exhaust button added for vehicle %lld", (long long)id);
}

// A loaded asset nothing references is collected, which left these pointing at freed memory.
static void keepLoaded() {
    static struct { uintptr_t* ptr; const wchar_t* path; Ref ref; } held[] = {
        {&sound, L"/Game/Cars/Parts/Engine/Sound/Backfire/SC_Backfire.SC_Backfire"},
        {&cone, L"/Engine/BasicShapes/Cone.Cone"},
        {&ball, L"/Engine/BasicShapes/Sphere.Sphere"},
        {&glow, L"/Game/Cars/Materials/M_AttachmentLight.M_AttachmentLight"},
        {&ghost, L"/Game/Cars/Materials/M_GhostCar.M_GhostCar"},
        {&flutterShot, L"/Game/Sounds/Vehicle/Intake/QX_Flutter_Shot.QX_Flutter_Shot"},
        {&flutterRoom, L"/Game/Sounds/EngineSoundAttenuation.EngineSoundAttenuation"},
        {&whistle, L"/Game/MoreTuning/Sounds/SW_QX_BlowerWhine.SW_QX_BlowerWhine"},
        {&waves[0], L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_01.Backfire_01"},
        {&waves[1], L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_02.Backfire_02"},
        {&waves[2], L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_03.Backfire_03"},
        {&waves[3], L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_04.Backfire_04"},
        {&waves[4], L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_05.Backfire_05"},
        {&waves[5], L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_06.Backfire_06"},
        {&booms[0], L"/Game/Sounds/Fireworks/FireworkBurst_01.FireworkBurst_01"},
        {&booms[1], L"/Game/Sounds/Fireworks/FireworkBurst_02.FireworkBurst_02"},
    };
    bool collected = false;
    for (auto& h : held) {
        if (!*h.ptr) continue;
        if (h.ref.ptr == *h.ptr && (!alive(h.ref) || dying(h.ref.ptr)) && !collected && loggingOn()) {
            collected = true;
            MT_LOG("garbage collection ran (assets freed)");  // to line hitches up against
        }
        // Forget it and let the next use load it: the game collects these every minute, and a
        // load here on every collection was a hitch whether anything needed the asset or not.
        if (h.ref.ptr == *h.ptr && (!alive(h.ref) || dying(h.ref.ptr))) { *h.ptr = 0; h.ref = {}; flutterTries = 0; whineTries = 0; continue; }
        h.ref = refOf(*h.ptr);
    }
}

// The game's MOD WARNING popup at start (a rich text block, "Game modifications detected"): under
// it, a photosensitivity warning, since the flames flash. Rich text blocks are found by the object
// scan (run) for the first minutes, each read once, and the one with the game's notice gets ours.
static const wchar_t* WARNING =  // the popup wraps it: a line break only between paragraphs
    L"\n\n<Warning>PHOTOSENSITIVITY WARNING</>\n"
    L"ECU Tuning, part of these mods, adds exhaust flames, backfire flashes and flickering light. "
    L"A small share of people can have seizures from flashing light, even with no history of epilepsy. "
    L"If you or your family have epilepsy, ask a doctor before playing. Stop at once if you feel dizzy "
    L"or have altered vision, eye or face twitching, disorientation or movements you cannot control.\n"
    L"<Highlight>No flames: set Flames=0 under [ECU] in Binaries\\Win64\\plugins\\ecu_tuning.ini</>";
static uintptr_t cRich, fGetRich, fTextToStr;
static std::vector<Ref> richBlocks;
static std::set<uintptr_t> richRead;
static bool warned;
static std::wstring readText(uintptr_t block) {
    static int pIn = off(fTextToStr, "InText"), pRet = off(fTextToStr, "ReturnValue"), pGot = off(fGetRich, "ReturnValue");
    if (pIn < 0 || pRet < 0 || pGot < 0) return L"";
    unsigned char q[0x60] = {0};
    api->process_event(block, fGetRich, q + pIn - pGot);
    api->process_event(textCDO, fTextToStr, q);
    FStr had = get<FStr>((uintptr_t)(q + pRet));
    std::wstring out;
    for (int i = 0; had.data && i < had.num - 1 && i < 4000; i++) out += get<wchar_t>((uintptr_t)(had.data + i));
    return out;
}
static void warnOnModNotice() {
    for (auto& r : richBlocks) {
        if (warned || !alive(r) || !richRead.insert(r.ptr).second) continue;
        std::wstring text = readText(r.ptr);
        if (text.find(L"Game modifications detected") == std::wstring::npos || text.find(L"PHOTOSENSITIVITY") != std::wstring::npos) continue;
        setText(r.ptr, (text + WARNING).c_str());
        warned = true;
        MT_LOG("photosensitivity warning added to the game's MOD WARNING");
    }
    richBlocks.clear();
}

// With logging on, a line every 2 s: the game's frame rate, this plugin's share of each frame, and
// what the flames have out. Ties a frame-rate drop to what was on screen when it happened.
static uintptr_t fWorldDelta;
static double perfMs, perfDt, perfWorst;
static int perfCalls;
struct PerfScope {
    LARGE_INTEGER a;
    PerfScope() { QueryPerformanceCounter(&a); }
    ~PerfScope() {
        static LARGE_INTEGER hz;
        if (!hz.QuadPart) QueryPerformanceFrequency(&hz);
        LARGE_INTEGER b;
        QueryPerformanceCounter(&b);
        perfMs += (b.QuadPart - a.QuadPart) * 1000.0 / hz.QuadPart;
    }
};
static void perfLine(DWORD now) {
    static DWORD at;
    if (!loggingOn() || !fWorldDelta) return;
    // Any live car for the world: the list is refreshed every 2 s, and a car freed since then (a garage
    // swap, a collection) crashed the game here on 3 Nov.
    uintptr_t context = 0;
    for (auto& r : vehicles) if (alive(r) && !dying(r.ptr)) { context = r.ptr; break; }
    if (!context) return;
    struct { uintptr_t context; double ret; } d = {context, 0};  // a double on UE 5
    api->process_event(gameplayCDO, fWorldDelta, &d);
    perfDt += d.ret; perfCalls++;
    perfWorst = d.ret > perfWorst ? d.ret : perfWorst;
    if (now - at < 2000) return;
    int pieces = 0, streaks = 0, cones = 0, lights = 0, burning = 0;
    for (auto& entry : cars)
        for (Slot& s : entry.second.slots) {
            for (Tongue& g : s.tongue) for (int c = 0; c < MAX_SEGS; c++) pieces += g.vis[c] && !g.parked[c];
            for (Spark& p : s.sparks) streaks += p.live;
            if (s.on && s.shape == 1) cones += s.layers;
            lights += s.on && s.glow > 0 && alive(s.light);
            burning += s.on;
        }
    if (perfCalls) MT_LOG("perf: %.0f fps (worst frame %.0f ms), ECU %.2f ms a frame | %d tips burning: %d pieces, %d streaks, %d cones, %d lights | %d shots queued",
                          perfDt > 0 ? perfCalls / perfDt : 0, perfWorst * 1000, perfMs / perfCalls, burning, pieces, streaks, cones, lights, (int)fired.size());
    at = now; perfDt = perfMs = perfWorst = 0; perfCalls = 0;
}

// The scene's directional lights (sun, moon), found by the scan. Daylight is the strongest one's height
// over the horizon, times its strength against the strongest seen: the moon is high at night too.
static std::vector<Ref> suns;
static uintptr_t fForward;
static int oIntensity = -1;
static void tickDaylight(DWORD now) {
    static DWORD at;
    static double strongest;
    if (now - at < 1000) return;
    at = now;
    if (!fForward || oIntensity < 0) {
        static bool told;
        if (!told) { told = true; MT_LOG("no daylight: GetForwardVector %p, Intensity at %d", (void*)fForward, oIntensity); }
        return;
    }
    double best = 0, bestUp = 0, bestI = 0;
    for (auto& r : suns) {
        if (!alive(r)) continue;
        Vec f = callVec(r.ptr, fForward);  // the way it shines: down while it is up
        double I = get<float>(r.ptr + oIntensity);
        strongest = I > strongest ? I : strongest;
        double d = smooth(0, 0.25, -f.z) * (strongest > 0 ? I / strongest : 0);
        if (d >= best) { best = d; bestUp = -f.z; bestI = I; }
    }
    static double said = -1;
    if (fabs(best - said) > 0.1) {
        said = best;
        MT_LOG("daylight %.2f (sun up %.2f, intensity %.2f of %.2f, %d lights): flames x%.1f", best, bestUp, bestI, strongest, (int)suns.size(), 1 + kDayGain * best);
    }
    daylight = best;
}

static void tick(void*) {
    PerfScope timed;
    // shortcut: finds the freezes a few seconds into a world. Remove once they are explained.
    static DWORD hitchFrom;
    DWORD stamp = GetTickCount();
    if (hitchFrom && stamp - hitchFrom > 150 && loggingOn()) MT_LOG("hitch %lu ms", stamp - hitchFrom);
    hitchFrom = stamp;
    InterlockedExchange(&queued, 0);
    keepLoaded();
    std::lock_guard<std::mutex> g(lock);
    if (fGetRich && fTextToStr && fSetRich && !warned) warnOnModNotice();
    if (vehicles.empty()) return;
    DWORD now = GetTickCount();
    double dt = (now - lastTick) / 1000.0;
    dt = dt > 0.05 ? 0.05 : dt;
    lastTick = now;
    if (!smokeVelocityName) {
        for (int i = 0; i < CARRY; i++) { wchar_t n[8]; swprintf_s(n, L"QXE%d", i); carrierE[i] = nameOf(n); }
        lightColorName = nameOf(L"Light Color");
        lightPowerName = nameOf(L"LightIntensity");
        colorName = nameOf(L"Color");
        opacityName = nameOf(L"Opacity");
        depthBiasName = nameOf(L"DepthBias");
        smokeAmountName = nameOf(L"User.SmokeAmount");
        smokeRadiusName = nameOf(L"User.SmokeExhaustRadius");
        smokePressureName = nameOf(L"User.SmokePressure");
        smokeColorName = nameOf(L"User.SmokeColor");
        smokeVelocityName = nameOf(L"User.SmokeVehicleVelocity");
    }
    perfLine(now);
    static DWORD fxAt;
    static int fxWas = -2;
    if (now - fxAt > 5000) {  // the player can change it in the settings at any time
        fxAt = now;
        int q = effectsQuality();
        lowFx = lowForced || q == 0;
        if (q != fxWas) MT_LOG("effects quality %d: %s", q, lowFx ? "low effects (one flame shape, least smoke)" : "full effects");
        fxWas = q;
    }
    tickDaylight(now);
    tickFlames(now, dt);
    tickClassic(now, dt);
    tickVoices(now);
    tickFired(now);
    tickPanel(now);
    struct { uintptr_t context; int index; uintptr_t ret; } who = {0, 0, 0};
    static unsigned turn;
    unsigned place = 0;
    turn++;
    for (auto& r : vehicles)
        if (alive(r)) { who.context = r.ptr; break; }
    if (!who.context) return;
    api->process_event(gameplayCDO, fPawn, &who);
    playerNow = who.ret;
    // Your tune goes onto the car you drive, so every other player fires it the same way.
    static DWORD sendAt;
    bool driving = playerNow && cVehCls && api->is_a(classOf(playerNow), cVehCls);
    if (driving && (int)(now - sendAt) >= 0) {
        sendAt = now + 1000;
        int64_t id = get<int64_t>(playerNow + oId);
        const Tune& mine = tuneOf(id);
        if (id > 0 && mine.own) sendTune(playerNow, mine);
    }
    Vec me = who.ret ? callVec(who.ret, fLoc) : Vec{};
    for (auto& r : vehicles) {
        if (r.ptr == playerNow) { if (alive(r)) one(r.ptr, who.ret, me, dt, now); continue; }
        // The player every frame, the rest of the traffic one frame in eight, staggered.
        if (++place % 8 == turn % 8 && alive(r)) one(r.ptr, who.ret, me, dt * 8, now);
    }
}

// ---- Probe: the game's save data in memory, and whether the cars in it carry the tune ----
// shortcut: logs only. It finds the records the game writes to disk, so the tune can go into them
// next; remove the probe then. Plugin thread, every 30 s, logged only when what it sees changes.
static std::string fieldList(uintptr_t st) {
    std::string s;
    for (uintptr_t f = get<uintptr_t>(st + L().struct_child_props); f; f = get<uintptr_t>(f + L().field_next))
        s += fname(f + L().field_name) + "@" + std::to_string(get<int>(f + L().field_offset)) + " ";
    return s;
}
static int structSize(uintptr_t st) { return get<int>(st + L().struct_child_props + 8); }

static void probeSave() {
    static uintptr_t cSave = api->find_object("Class", "SaveGame", "/Script/Engine");
    static uintptr_t sVeh = api->find_object("ScriptStruct", "MTSaveGameVehicle", "/Script/MotorTown");
    static uintptr_t sCus = api->find_object("ScriptStruct", "MTSaveGameVehicleCustomization", "/Script/MotorTown");
    static uintptr_t sCol = api->find_object("ScriptStruct", "MTSaveGameVehicleCustomizationColor", "/Script/MotorTown");
    static bool told;
    if (!cSave || !sVeh || !sCus || !sCol) {
        if (!told) { told = true; MT_LOG("save probe: missing SaveGame %p, vehicle %p, customisation %p, colour %p", (void*)cSave, (void*)sVeh, (void*)sCus, (void*)sCol); }
        return;
    }
    int vSize = structSize(sVeh), colSize = structSize(sCol), vCus = off(sVeh, "Customization"), cArr = off(sCus, "BodyColors");
    int vId = -1;
    for (const char* n : {"ID", "Id", "VehicleId", "Net_VehicleId", "VehicleID"}) if (vId < 0) vId = off(sVeh, n);
    if (!told) {
        told = true;
        MT_LOG("save probe: MTSaveGameVehicle (%d bytes) %s", vSize, fieldList(sVeh).c_str());
        MT_LOG("save probe: customisation (%d) %s", structSize(sCus), fieldList(sCus).c_str());
        MT_LOG("save probe: colour (%d) %s", colSize, fieldList(sCol).c_str());
    }
    if (vSize <= 0 || colSize <= 0 || vCus < 0 || vId < 0) return;
    if (cArr < 0) cArr = 0;
    std::set<int64_t> ids;
    {
        std::lock_guard<std::mutex> g(lock);
        for (auto& t : tuned) if (t.rfind(L"Vehicle", 0) == 0) ids.insert(_wtoi64(t.c_str() + 7));
    }
    std::string seen;
    static std::string said;
    static std::set<uintptr_t> listed;
    for (int i = 0, n = api->num_objects(); i < n; i++) {
        uintptr_t o = api->object(i);
        if (!o || isCDO(o) || !api->is_a(classOf(o), cSave)) continue;
        uintptr_t cls = classOf(o);
        if (listed.insert(cls).second)
            for (uintptr_t c = cls; c && c != cSave; c = superOf(c))
                MT_LOG("save probe: %s (%d bytes): %s", objName(c).c_str(), structSize(c), fieldList(c).c_str());
        // Any TArray inside the object whose records are cars this plugin has tuned.
        int size = structSize(cls);
        for (int at = 0x28; at + 16 <= size && at < 0x10000; at += 8) {
            uintptr_t data = get<uintptr_t>(o + at);
            int cnt = get<int>(o + at + 8), cap = get<int>(o + at + 12);
            if (!data || cnt <= 0 || cnt > 2000 || cap < cnt || cap > 8192) continue;
            std::string cars;
            for (int k = 0; k < cnt && k < 500; k++) {
                uintptr_t r = data + (uintptr_t)k * vSize;
                int64_t id = get<int64_t>(r + vId);
                if (!ids.count(id)) continue;
                uintptr_t cols = get<uintptr_t>(r + vCus + cArr);
                int cn = get<int>(r + vCus + cArr + 8), ours = 0;
                for (int e = 0; cols && e < cn && e < 200; e++)
                    for (int b = 0; b + 8 <= colSize; b += 4)
                        if (carriedAt(get<uint64_t>(cols + (uintptr_t)e * colSize + b)) >= 0) { ours++; break; }
                char line[96];
                snprintf(line, sizeof line, " car %lld: %d colours, %d of %d tune entries;", (long long)id, cn, ours, CARRY);
                cars += line;
            }
            if (!cars.empty()) seen += objName(cls) + " " + objName(o) + " +" + std::to_string(at) + " (" + std::to_string(cnt) + " records):" + cars + "\n";
        }
    }
    if (seen == said) return;
    said = seen;
    MT_LOG("save probe %lu: %s", GetTickCount(), seen.empty() ? "no tuned car found in any save object" : seen.c_str());
}

static uintptr_t findEffect(int field) {
    for (int i = 0, n = api->num_objects(); i < n; i++) {
        uintptr_t o = api->object(i);
        if (o && !isCDO(o) && classOf(o) == cFx && get<uintptr_t>(o + field)) return get<uintptr_t>(o + field);
    }
    return 0;
}

// The tick is queued from a thread of its own: the full scan takes about a second, and queuing from the
// scanning thread stalled the tick for that second every two.
static DWORD WINAPI pump(LPVOID) {
    for (int beat = 1;; beat++) {
        if (beat % 600 == 0) InterlockedExchange(&queued, 0);  // a queue dropped at a level change
        if (api->game_thread_ready() && !InterlockedCompareExchange(&queued, 1, 0)) api->run_on_game_thread(tick, nullptr);
        Sleep(16);
    }
}

static DWORD WINAPI run(LPVOID) {
    MT_LOG("ECU Tuning loaded");
    {
        static wchar_t names[65536];
        GetPrivateProfileSectionNamesW(names, 65536, iniPath(L"ecu_tuning.ini").c_str());
        for (const wchar_t* n = names; *n; n += wcslen(n) + 1) tuned.insert(n);
    }
    base.on = iniNum(L"ecu_tuning.ini", L"ECU", L"Enabled", 1);
    base.pops = iniNum(L"ecu_tuning.ini", L"ECU", L"PopRate", 5);
    base.revs = iniNum(L"ecu_tuning.ini", L"ECU", L"MinRevs", 0.55);
    base.volume = iniNum(L"ecu_tuning.ini", L"ECU", L"PopVolume", 2);
    base.flames = iniNum(L"ecu_tuning.ini", L"ECU", L"Flames", 1);
    base.length = iniNum(L"ecu_tuning.ini", L"ECU", L"FlameLength", 0.04);
    base.life = iniNum(L"ecu_tuning.ini", L"ECU", L"FlameLife", 0.04);
    base.density = iniNum(L"ecu_tuning.ini", L"ECU", L"FlameDensity", 1);
    base.shape = iniNum(L"ecu_tuning.ini", L"ECU", L"FlameShape", 0);
    base.twin = iniNum(L"ecu_tuning.ini", L"ECU", L"TwinTips", 0);
    base.gap = iniNum(L"ecu_tuning.ini", L"ECU", L"TwinSpacing", 8);
    base.p1 = base.p2 = base.p3 = base.p4 = 1;  // every socket on, where the game put it
    base.ovFuel = iniNum(L"ecu_tuning.ini", L"ECU", L"OverrunFuel", 0.2);
    base.ovRetard = iniNum(L"ecu_tuning.ini", L"ECU", L"OverrunRetard", 0.5);
    base.spark = iniNum(L"ecu_tuning.ini", L"ECU", L"LimiterSpark", 0);
    base.decat = iniNum(L"ecu_tuning.ini", L"ECU", L"CatDelete", 0);
    base.wide = iniNum(L"ecu_tuning.ini", L"ECU", L"WideTips", 0);
    base.back = iniNum(L"ecu_tuning.ini", L"ECU", L"BackPressure", 0.5);
    lowForced = iniNum(L"ecu_tuning.ini", L"ECU", L"LowEffects", 0) != 0;
    base.size = iniNum(L"ecu_tuning.ini", L"ECU", L"FlameSize", 0.2);
    kRange = iniNum(L"ecu_tuning.ini", L"ECU", L"Range", 10000);
    base.dpf = iniNum(L"ecu_tuning.ini", L"ECU", L"DPFDelete", 0);
    base.smoke = iniNum(L"ecu_tuning.ini", L"ECU", L"SmokeAmount", 3);
    base.spread = iniNum(L"ecu_tuning.ini", L"ECU", L"FlameWidth", 0.2);
    base.bang = iniNum(L"ecu_tuning.ini", L"ECU", L"BangChance", 0);
    base.flameRevs = iniNum(L"ecu_tuning.ini", L"ECU", L"FlameRevs", 0.6);
    kGhostGain = (float)iniNum(L"ecu_tuning.ini", L"ECU", L"FlameBrightness", 1.6);
    kDayGain = (float)iniNum(L"ecu_tuning.ini", L"ECU", L"FlameDaylight", 12);
    kOpacity = (float)iniNum(L"ecu_tuning.ini", L"ECU", L"FlameOpacity", 1.35);
    fForward = api->find_object("Function", "GetForwardVector", "SceneComponent");
    oIntensity = off(api->find_object("Class", "LightComponentBase", "/Script/Engine"), "Intensity");
    kDepthBias = (float)iniNum(L"ecu_tuning.ini", L"ECU", L"FlameDepthBias", -40);
    kMaterial = (int)iniNum(L"ecu_tuning.ini", L"ECU", L"FlameMaterial", 0);
    base.cool = iniNum(L"ecu_tuning.ini", L"ECU", L"CoolingBoost", 0);
    base.lag = iniNum(L"ecu_tuning.ini", L"ECU", L"AntiLag", 0);
    base.lagRevs = iniNum(L"ecu_tuning.ini", L"ECU", L"AntiLagRevs", 0.5);
    base.timing = iniNum(L"ecu_tuning.ini", L"ECU", L"Timing", 0.5);
    base.fuel = iniNum(L"ecu_tuning.ini", L"ECU", L"FuelMap", 0.5);
    base.boost = iniNum(L"ecu_tuning.ini", L"ECU", L"Boost", 0.5);
    base.chance = iniNum(L"ecu_tuning.ini", L"ECU", L"BackfireChance", 0);
    base.lasts = iniNum(L"ecu_tuning.ini", L"ECU", L"SmokeLasts", 4);
    if (base.lasts < 1) base.lasts = 1;
    if (base.revs < 0 || base.revs >= 1) { MT_LOG("FAILED: MinRevs must be 0 to below 1"); return 0; }

    if (!api->wait_engine(INFINITE)) { MT_LOG("FAILED: no engine"); return 0; }
    // The engine can be ready before Niagara and UMG are in, and one empty lookup would end the plugin.
    for (int i = 0; i < 240 && !(api->find_object("NiagaraFunctionLibrary", "Default__NiagaraFunctionLibrary", "/Script/Niagara")
                                 && api->find_object("Class", "VerticalBox", "/Script/UMG")); i++) Sleep(500);
    uintptr_t cVeh = api->find_object("Class", "MTVehicle", "/Script/MotorTown");
    if (uintptr_t sState = api->find_object("ScriptStruct", "MTVehicleState", "/Script/MotorTown"); sState && cVeh) {
        int a = off(cVeh, "NetLC_VehicleState"), b = off(sState, "Fuel");
        oTankFuel = a >= 0 && b >= 0 ? a + b : -1;
        oTankCap = off(cVeh, "FuelTankCapacityInLiter");
    }
    MT_LOG("flames burn fuel: %s", oTankFuel >= 0 ? "yes" : "no -- MTVehicle.NetLC_VehicleState.Fuel not found");
    cVehCls = cVeh;
    {  // the car's customization list, which carries the tune to other players
        uintptr_t sCustom = api->find_object("ScriptStruct", "MTVehicleCustomization", "/Script/MotorTown");
        uintptr_t sColor = api->find_object("ScriptStruct", "MTVehicleCustomizationColor", "/Script/MotorTown");
        fGetPC = api->find_object("Function", "GetPlayerController", "GameplayStatics");
        fSendCustom = api->find_object("Function", "ServerSetVehicleCustomization", "MotorTownPlayerController");
        oCustom = cVeh ? off(cVeh, "Customization") : -1;
        if (sCustom && sColor) {
            cColors = off(sCustom, "BodyColors"); cSlot = off(sColor, "MaterialSlotName"); cColor = off(sColor, "Color");
            cSize = get<int>(sColor + L().struct_child_props + 8); customSize = get<int>(sCustom + L().struct_child_props + 8);
        }
        if (fSendCustom) { pSendVehicle = off(fSendCustom, "Vehicle"); pSendCustom = off(fSendCustom, "Customization"); }
        if (!sCustom || !sColor || !fGetPC || !fSendCustom || oCustom < 0 || cColors < 0 || cSlot < 0 || cColor < 0 || cSize <= 0 || pSendCustom < 0 || pSendVehicle < 0) {
            MT_LOG("customization calls not found -- tunes stay on this machine");
            oCustom = -1;
        }
    }
    uintptr_t cComp = api->find_object("Class", "MTEngineComponent", "/Script/MotorTown");
    uintptr_t cData = api->find_object("Class", "MHEngineDataAsset", "/Script/MotorTown");
    uintptr_t sProp = api->find_object("ScriptStruct", "MTEngineProperty", "/Script/MotorTown");
    uintptr_t sHot = api->find_object("ScriptStruct", "MTEngineHotState", "/Script/MotorTown");
    cFx = api->find_object("Class", "MotorTownFXAsset", "/Script/MotorTown");
    gameplayCDO = api->find_object("GameplayStatics", "Default__GameplayStatics", "/Script/Engine");
    systemCDO = api->find_object("KismetSystemLibrary", "Default__KismetSystemLibrary", "/Script/Engine");
    stringCDO = api->find_object("KismetStringLibrary", "Default__KismetStringLibrary", "/Script/Engine");
    niagaraCDO = api->find_object("NiagaraFunctionLibrary", "Default__NiagaraFunctionLibrary", "/Script/Niagara");
    fLoc = api->find_object("Function", "K2_GetActorLocation", "Actor");
    fVelocity = api->find_object("Function", "GetVelocity", "Actor");
    fPawn = api->find_object("Function", "GetPlayerPawn", "GameplayStatics");
    fSetXform = api->find_object("Function", "K2_SetRelativeTransform", "SceneComponent");
    if (fSetXform) oXform = off(fSetXform, "NewTransform");
    fWorldDelta = api->find_object("Function", "GetWorldDeltaSeconds", "GameplayStatics");  // the perf line only
    fCamMgr = api->find_object("Function", "GetPlayerCameraManager", "GameplayStatics");  // culling only: without it nothing is culled
    fCamLoc = api->find_object("Function", "GetCameraLocation", "PlayerCameraManager");
    fCamRot = api->find_object("Function", "GetCameraRotation", "PlayerCameraManager");
    if (!fCamMgr || !fCamLoc || !fCamRot) MT_LOG("no camera functions: flames are never culled");
    fRoot = api->find_object("Function", "K2_GetRootComponent", "Actor");
    fBounds = api->find_object("Function", "GetActorBounds", "Actor");
    mathCDO = api->find_object("KismetMathLibrary", "Default__KismetMathLibrary", "/Script/Engine");
    fRelScale = api->find_object("Function", "SetRelativeScale3D", "SceneComponent");
    cMeshComp = api->find_object("Class", "StaticMeshComponent", "/Script/Engine");
    fAddComp = api->find_object("Function", "AddComponentByClass", "Actor");
    fFinish = api->find_object("Function", "FinishAddComponent", "Actor");
    fMobility = api->find_object("Function", "SetMobility", "SceneComponent");
    fMesh = api->find_object("Function", "SetStaticMesh", "StaticMeshComponent");
    fCollision = api->find_object("Function", "SetCollisionEnabled", "PrimitiveComponent");
    fShadow = api->find_object("Function", "SetCastShadow", "PrimitiveComponent");
    fMakeMid = api->find_object("Function", "CreateDynamicMaterialInstance", "PrimitiveComponent");
    fMidColor = api->find_object("Function", "SetVectorParameterValue", "MaterialInstanceDynamic");
    fMidScalar = api->find_object("Function", "SetScalarParameterValue", "MaterialInstanceDynamic");
    fRelLoc = api->find_object("Function", "K2_SetRelativeLocation", "SceneComponent");
    if (uintptr_t cScene = api->find_object("Class", "SceneComponent", "/Script/Engine")) { oSceneRel = off(cScene, "RelativeLocation"); oChildren = off(cScene, "AttachChildren"); }
    fMeshVis = api->find_object("Function", "SetVisibility", "SceneComponent");
    auto lightFn = [](const char* name, const char* owner, const char* other) {
        uintptr_t f = api->find_object("Function", name, owner);
        return f ? f : api->find_object("Function", name, other);
    };
    cLight = api->find_object("Class", "PointLightComponent", "/Script/Engine");
    fIntensity = lightFn("SetIntensity", "LightComponent", "LocalLightComponent");
    fLightColor = lightFn("SetLightColor", "LightComponent", "LightComponentBase");
    fLightRadius = lightFn("SetAttenuationRadius", "LocalLightComponent", "PointLightComponent");
    fLightShadows = lightFn("SetCastShadows", "LightComponentBase", "LightComponent");
    fLightUnits = lightFn("SetIntensityUnits", "LocalLightComponent", "PointLightComponent");
    fRelRot = api->find_object("Function", "K2_SetRelativeRotation", "SceneComponent");
    fFlutter = api->find_object("Function", "SpawnSoundAttached", "GameplayStatics");
    fLowOn = api->find_object("Function", "SetLowPassFilterEnabled", "AudioComponent");
    fLowHz = api->find_object("Function", "SetLowPassFilterFrequency", "AudioComponent");
    fHighOn = api->find_object("Function", "SetHighPassFilterEnabled", "AudioComponent");
    fHighHz = api->find_object("Function", "SetHighPassFilterFrequency", "AudioComponent");
    fSetSound = api->find_object("Function", "SetSound", "AudioComponent");
    fWhinePitch = api->find_object("Function", "SetPitchMultiplier", "AudioComponent");
    fWhineVolume = api->find_object("Function", "SetVolumeMultiplier", "AudioComponent");
    fWhineIsOn = api->find_object("Function", "IsPlaying", "AudioComponent");
    fWhinePlay = api->find_object("Function", "Play", "AudioComponent");
    fWhineStop = api->find_object("Function", "Stop", "AudioComponent");
    fFadeOut = api->find_object("Function", "FadeOut", "AudioComponent");
    fAttached = api->find_object("Function", "GetAttachedActors", "Actor");
    fToWorld = api->find_object("Function", "K2_GetComponentToWorld", "SceneComponent");
    fInverse = api->find_object("Function", "InverseTransformLocation", "KismetMathLibrary");
    fSound = api->find_object("Function", "PlaySoundAtLocation", "GameplayStatics");
    fSpawn = api->find_object("Function", "SpawnSystemAttached", "NiagaraFunctionLibrary");
    fSetFloat = api->find_object("Function", "SetVariableFloat", "NiagaraComponent");
    fSetColor = api->find_object("Function", "SetVariableLinearColor", "NiagaraComponent");
    fSetVec = api->find_object("Function", "SetVariableVec3", "NiagaraComponent");
    fDilation = api->find_object("Function", "SetCustomTimeDilation", "NiagaraComponent");
    fStop = api->find_object("Function", "Deactivate", "ActorComponent");  // what is in the air fades out
    fToName = api->find_object("Function", "Conv_StringToName", "KismetStringLibrary");
    fPath = api->find_object("Function", "MakeSoftObjectPath", "KismetSystemLibrary");
    fRef = api->find_object("Function", "Conv_SoftObjPathToSoftObjRef", "KismetSystemLibrary");
    fLoad = api->find_object("Function", "LoadAsset_Blocking", "KismetSystemLibrary");
    uintptr_t need[] = {cVeh, cComp, cData, sProp, sHot, cFx, gameplayCDO, systemCDO, stringCDO, niagaraCDO, fLoc, fVelocity, fPawn, fRoot, fBounds,
                        mathCDO, fRelScale, fAttached, fToWorld, fInverse, fSound, fSpawn, fSetFloat, fSetColor, fSetVec, fDilation, fStop, fToName, fPath, fRef, fLoad};
    bool ok = true;
    for (int i = 0; i < (int)(sizeof need / sizeof *need); i++)
        if (!need[i]) { MT_LOG("FAILED: engine or spawn function %d of the list not found -- exhausts stay vanilla", i); ok = false; }
    if (!ok) return 0;
    oEngine = off(cVeh, "EngineComponent"); oId = off(cVeh, "Net_VehicleId");
    oData = off(cComp, "EngineData"); oHot = off(cComp, "NetLC_EngineHotState"); oNative = off(cComp, "AfterfireAudioComponents"); oPipes = off(cComp, "ExhaustBlackSmokes"); oWhite = off(cComp, "ExhaustWhiteSmokes");
    oProp = off(cData, "EngineProperty"); oAfter = off(sProp, "AfterFireProbability"); oMax = off(sProp, "MaxRPM"); oTorque = off(sProp, "MaxTorque"); oFuelType = off(sProp, "FuelType"); oMotor = off(sProp, "MotorMaxPower");
    oRpm = off(sHot, "CurrentRPM"); oThrottle = off(sHot, "Throttle"); oFxSmoke = off(cFx, "ExhaustNS");
    uintptr_t sTurbo = api->find_object("ScriptStruct", "MTVehiclePartTurbocharger", "/Script/MotorTown");
    int turboValid = sTurbo ? off(sTurbo, "bIsValid") : -1, turbo = off(cComp, "Turbocharger");
    if (turboValid >= 0 && turbo >= 0) oTurbo = turbo + turboValid;
    fIsServer = api->find_object("Function", "IsServer", "KismetSystemLibrary");
    uintptr_t sFuelMap = api->find_object("ScriptStruct", "MTTurbochargerFuelSettings", "/Script/MotorTown");
    uintptr_t sIntake = api->find_object("ScriptStruct", "MTVehiclePartIntake", "/Script/MotorTown");
    int oIntake = off(cComp, "Intake"), oMaps[2] = {sTurbo ? off(sTurbo, "Gasoline") : -1, sTurbo ? off(sTurbo, "Diesel") : -1};
    const char* trimmed[6] = {"BaseTorqueMultiplier", "TorqueMultiplier", "HeatingMultiplier", "FuelConsumptionMultiplier", "TurbineWeight", "IntakePressureMultiplier"};
    canEcu = fIsServer && sFuelMap && sIntake && turbo >= 0 && oIntake >= 0 && oMaps[0] >= 0 && oMaps[1] >= 0
        && off(fIsServer, "WorldContextObject") >= 0 && off(fIsServer, "ReturnValue") >= 0;
    for (int m = 0; canEcu && m < 2; m++)
        for (int f = 0; f < 6; f++) {
            int at = off(sFuelMap, trimmed[f]);
            canEcu = canEcu && at >= 0;
            oTrim[m * 6 + f] = turbo + oMaps[m] + at;
        }
    if (canEcu) {
        int at = off(sIntake, "IntakeSpeedEfficencyMultiplier");
        canEcu = at >= 0;
        oTrim[12] = oIntake + at;
        uintptr_t sRadiator = api->find_object("ScriptStruct", "MTVehiclePartCoolantRadiator", "/Script/MotorTown");
        int radiator = off(cComp, "CoolantRadiator"), power = sRadiator ? off(sRadiator, "CoolingPower") : -1;
        canEcu = canEcu && radiator >= 0 && power >= 0;
        oTrim[13] = radiator + power;
    }
    oPedal = off(cVeh, "Throttle");
    uintptr_t sRepMove = api->find_object("ScriptStruct", "MTVehicleRepMovement", "/Script/MotorTown");
    int repMove = off(cVeh, "VehicleReplicatedMovement"), repThrottle = sRepMove ? off(sRepMove, "Throttle") : -1;
    if (repMove >= 0 && repThrottle >= 0) oRepPedal = repMove + repThrottle;
    uintptr_t sCold = api->find_object("ScriptStruct", "MTVehicleColdState", "/Script/MotorTown");
    int cold = off(cVeh, "NetLC_ColdState"), ai = sCold ? off(sCold, "bIsAIDriving") : -1;
    if (cold >= 0 && ai >= 0) oAiDriving = cold + ai;
    oBrake = off(cVeh, "Brake");
    if (oTurbo == 0x3c4) oTurbineSpeed = 0x364;  // shortcut: TurbineSpeedRatio is not reflected, 0x60 before the turbo struct; the oTurbo test turns it off after an update, recheck it then
    MT_LOG("pedal at %x, replicated pedal at %x, turbine speed at %x", oPedal, oRepPedal, oTurbineSpeed);
    MT_LOG("engine map %s", canEcu ? "ready" : "NOT available: rows only change the exhaust");
    pPathIn = off(fPath, "PathString"); pPathRet = off(fPath, "ReturnValue");
    pRefIn = off(fRef, "SoftObjectPath"); pRefRet = off(fRef, "ReturnValue");
    pLoadIn = off(fLoad, "Asset"); pLoadRet = off(fLoad, "ReturnValue");
    pNameIn = off(fToName, "InString"); pNameRet = off(fToName, "ReturnValue");
    pCtx = off(fSound, "WorldContextObject"); pSound = off(fSound, "Sound"); pAt = off(fSound, "Location");
    pVolume = off(fSound, "VolumeMultiplier"); pPitch = off(fSound, "PitchMultiplier");
    pSystem = off(fSpawn, "SystemTemplate"); pAttach = off(fSpawn, "AttachToComponent"); pEffectAt = off(fSpawn, "Location"); pEffectRot = off(fSpawn, "Rotation"); pBoundsExtent = off(fBounds, "BoxExtent"); pBoundsOrigin = off(fBounds, "Origin");
    pDestroy = off(fSpawn, "bAutoDestroy"); pActivate = off(fSpawn, "bAutoActivate"); pSpawnRet = off(fSpawn, "ReturnValue");
    int have[] = {oFuelType, oEngine, oId, oPipes, oData, oHot, oNative, oProp, oAfter, oMax, oMotor, oRpm, oThrottle, oFxSmoke, pPathIn, pPathRet, pRefIn, pRefRet,
                  pEffectAt, pEffectRot, pBoundsExtent, pBoundsOrigin, pLoadIn, pLoadRet, pNameIn, pNameRet, pCtx, pSound, pAt, pVolume, pPitch, pSystem, pAttach,
                  pDestroy, pActivate, pSpawnRet};
    struct { uintptr_t fn; const char* field; } args[] = {
        {fInverse, "T"}, {fInverse, "Location"}, {fInverse, "ReturnValue"},
        {fAttached, "OutActors"}, {fAttached, "bResetArray"}, {fAttached, "bRecursivelyIncludeAttachedActors"}};
    // Flames alone: a wrong name here must not cost the menu, the pops and the smoke.
    uintptr_t flameFns[] = {cMeshComp, fAddComp, fFinish, fMobility, fMesh, fCollision, fShadow, fMakeMid, fMidColor, fMidScalar, fRelLoc, fMeshVis, fRelRot};
    for (int i = 0; i < 13; i++)
        if (!flameFns[i]) { MT_LOG("no flames: function %d of the list not found", i); canFlame = false; }
    if (canFlame) {
        struct { uintptr_t fn; const char* field; } flameArgs[] = {{fAddComp, "Class"}, {fAddComp, "RelativeTransform"}, {fAddComp, "bDeferredFinish"}, {fAddComp, "ReturnValue"},
            {fFinish, "Component"}, {fFinish, "RelativeTransform"}, {fMakeMid, "SourceMaterial"}, {fMakeMid, "ReturnValue"}, {fRelLoc, "NewLocation"}, {fRelRot, "NewRotation"}};
        for (auto& a : flameArgs)
            if (off(a.fn, a.field) < 0) { MT_LOG("no flames: no parameter %s", a.field); canFlame = false; }
        oRelLocAt = off(fRelLoc, "NewLocation");
        oRelRotAt = off(fRelRot, "NewRotation");
    }
    for (uintptr_t f : {cLight, fIntensity, fLightColor, fLightRadius, fLightShadows, fLightUnits}) canGlow = canGlow && f;
    if (!canGlow) MT_LOG("no flame light: %d %d %d %d %d %d", cLight != 0, fIntensity != 0, fLightColor != 0, fLightRadius != 0, fLightShadows != 0, fLightUnits != 0);
    uintptr_t sPart = api->find_object("ScriptStruct", "MTVehiclePart", "/Script/MotorTown");
    oParts = off(cVeh, "Net_Parts");
    if (sPart) { oPartKey = off(sPart, "Key"); oPartSlot = off(sPart, "Slot"); partSize = get<int>(sPart + L().struct_child_props + 8); }
    for (uintptr_t f : {fFlutter, fLowOn, fLowHz, fHighOn, fHighHz}) canFlutter = canFlutter && f;
    canFlutter = canFlutter && oParts >= 0 && oPartKey >= 0 && oPartSlot >= 0 && partSize > 0;
    if (canFlutter)
        for (const char* field : {"Sound", "AttachToComponent", "VolumeMultiplier", "PitchMultiplier", "AttenuationSettings", "bAutoDestroy", "ReturnValue"})
            if (off(fFlutter, field) < 0) { MT_LOG("no flutter: SpawnSoundAttached has no parameter %s", field); canFlutter = false; }
    if (!canFlutter) MT_LOG("no flutter: audio or part fields not found");
    for (auto& a : args)
        if (off(a.fn, a.field) < 0) { MT_LOG("FAILED: no parameter %s -- exhausts stay vanilla", a.field); return 0; }
    for (int i = 0; i < (int)(sizeof have / sizeof *have); i++)
        if (have[i] < 0) { MT_LOG("FAILED: field %d not found -- exhausts stay vanilla", i); return 0; }

    uintptr_t cScreen = api->find_object("Class", "GarageTuningWidget", "/Script/MotorTown");
    uintptr_t cChild = api->find_object("Class", "GarageChildWidget", "/Script/MotorTown");
    uintptr_t cRowBase = api->find_object("Class", "SettingWidget", "/Script/MotorTown");
    uintptr_t cSpinRow = api->find_object("Class", "SpinBoxString", "/Script/MotorTown");
    cButtonBase = api->find_object("Class", "MTButtonWidget", "/Script/MotorTown");
    cText = api->find_object("Class", "TextBlock", "/Script/UMG");
    cUButton = api->find_object("Class", "Button", "/Script/UMG");
    widgetCDO = api->find_object("WidgetBlueprintLibrary", "Default__WidgetBlueprintLibrary", "/Script/UMG");
    textCDO = api->find_object("KismetTextLibrary", "Default__KismetTextLibrary", "/Script/Engine");
    fCreate = api->find_object("Function", "Create", "WidgetBlueprintLibrary");
    fAddChild = api->find_object("Function", "AddChild", "PanelWidget");
    fChildIndex = api->find_object("Function", "GetChildIndex", "PanelWidget");
    fChildAt = api->find_object("Function", "GetChildAt", "PanelWidget");
    fChildCount = api->find_object("Function", "GetChildrenCount", "PanelWidget");
    fToText = api->find_object("Function", "Conv_StringToText", "KismetTextLibrary");
    fSetText = api->find_object("Function", "SetText", "TextBlock");
    fSetRich = api->find_object("Function", "SetText", "RichTextBlock");
    fPressed = api->find_object("Function", "IsPressed", "Button");
    fRemove = api->find_object("Function", "RemoveFromParent", "Widget");
    fSetVis = api->find_object("Function", "SetVisibility", "Widget");
    fIsVisible = api->find_object("Function", "IsVisible", "Widget");
    cRich = api->find_object("Class", "RichTextBlock", "/Script/UMG");
    fGetRich = api->find_object("Function", "GetText", "RichTextBlock");
    fTextToStr = api->find_object("Function", "Conv_TextToString", "KismetTextLibrary");
    fPadding = api->find_object("Function", "SetPadding", "VerticalBoxSlot");
    uintptr_t ui[] = {cScreen, cChild, cRowBase, cSpinRow, cButtonBase, cText, cUButton, widgetCDO, textCDO, fCreate, fAddChild, fChildIndex,
                      fChildAt, fChildCount, fToText, fSetText, fSetRich, fPressed, fRemove, fSetVis, fIsVisible, fPadding};
    bool canTune = true;
    for (int i = 0; i < (int)(sizeof ui / sizeof *ui); i++)
        if (!ui[i]) { MT_LOG("no Exhaust menu: garage widget %d of the list not found -- pops run on the ini defaults", i); canTune = false; }
    if (canTune) {
        oVehicle = off(cChild, "Vehicle"); oBox = off(cScreen, "TuningVerticalBox");
        oRowName = off(cRowBase, "NameTextBlock"); oRowSpin = off(cRowBase, "SpinBoxString");
        oSpinText = off(cSpinRow, "TextBlock"); 
        oBtnName = off(cButtonBase, "NameTextBlock"); oBtnRich = off(cButtonBase, "NameRichTextBlock"); oBtnText = off(cButtonBase, "ButtonName"); oBtnInner = off(cButtonBase, "MenuButton");
        pTextIn = off(fToText, "InString"); pTextRet = off(fToText, "ReturnValue");
        int fields[] = {oVehicle, oBox, oRowName, oRowSpin, oSpinText, oBtnName, oBtnRich, oBtnText, oBtnInner, pTextIn, pTextRet};
        for (int i = 0; i < (int)(sizeof fields / sizeof *fields); i++)
            if (fields[i] < 0) { MT_LOG("no Exhaust menu: garage field %d of the list not found -- pops run on the ini defaults", i); canTune = false; }
    }
    static const char* kConstruct[] = {"Construct"};
    if (canTune) { gScreenClass = cScreen; MT_LOG("garage screen watch %s", api->watch_functions(kConstruct, 1, onBuilt, nullptr) ? "on" : "OFF -- scans only"); }

    auto range = find_sig("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F B6 44 24 50 48 8B D9 88 81 ?? ?? ?? ?? F3 0F 11 89");
    auto value = find_sig("40 53 48 83 EC 20 80 B9 ?? ?? ?? ?? 00 48 8B D9 F3 0F 10 81 ?? ?? ?? ?? F3 0F 5D C1 F3 0F 5F 81");
    if (range.size() == 1 && value.size() == 1) {
        spinRange = (decltype(spinRange))range[0];
        spinSet = (decltype(spinSet))value[0];
        oSpinValue = get<int>(range[0] + 57);  // the fourth store: value = min
        MT_LOG("number rows ready, value at +%x", oSpinValue);
    } else MT_LOG("number rows not found (%d, %d matches): the ECU rows show but cannot change", (int)range.size(), (int)value.size());

    // The game builds a new garage screen each time the menu opens, and Construct does not reach
    // ProcessEvent for it. So between the full scans a quick one looks for just the screen's own
    // class (learnt from its default object): one compare an object, not an is_a.
    std::vector<uintptr_t> screenClasses;
    uintptr_t cSun = api->find_object("Class", "DirectionalLightComponent", "/Script/Engine");
    if (!fForward) fForward = api->find_object("Function", "GetForwardVector", "SceneComponent");
    fMeshBox = api->find_object("Function", "GetBoundingBox", "StaticMesh");
    oStaticMesh = off(cMeshComp, "StaticMesh");
    oMeshSockets = off(api->find_object("Class", "StaticMesh", "/Script/Engine"), "Sockets");
    if (uintptr_t cSock = api->find_object("Class", "StaticMeshSocket", "/Script/Engine")) {
        oSockName = off(cSock, "SocketName"); oSockLoc = off(cSock, "RelativeLocation"); oSockRot = off(cSock, "RelativeRotation");
    }
    if (oSockName < 0 || oSockLoc < 0 || oSockRot < 0) oMeshSockets = -1;
    MT_LOG("mesh sockets: list at %d, name %d, location %d, rotation %d", oMeshSockets, oSockName, oSockLoc, oSockRot);
    if (!fMeshBox || oStaticMesh < 0) MT_LOG("no mesh box (%p, %d): cars without exhaust sockets guess from the world box", (void*)fMeshBox, oStaticMesh);
    if (oIntensity < 0) oIntensity = off(api->find_object("Class", "LightComponentBase", "/Script/Engine"), "Intensity");
    MT_LOG("daylight from %p, GetForwardVector %p, Intensity at %d", (void*)cSun, (void*)fForward, oIntensity);
    CloseHandle(CreateThread(nullptr, 0, pump, nullptr, 0, nullptr));
    for (int beat = 0;; beat++) {
        {
            std::vector<Ref> got;
            { std::lock_guard<std::mutex> g(builtLock); got.swap(built); }
            if (!got.empty()) {
                if (canTune && !cButton) {
                    cSetting = api->find_object("WidgetBlueprintGeneratedClass", "W_Setting_C", "/Game/UI/Component/W_Setting");
                    cButton = api->find_object("WidgetBlueprintGeneratedClass", "W_Button_C", "/Game/UI/Button/W_Button");
                    if (!cSetting) cButton = 0;
                }
                std::lock_guard<std::mutex> g(lock);
                for (auto& r : got)
                    if (std::none_of(screens.begin(), screens.end(), [&](const Ref& s) { return s.ptr == r.ptr; })) {
                        screens.push_back(r);
                        MT_LOG("menu %lu: garage screen %p built", GetTickCount(), (void*)r.ptr);
                    }
            }
        }
        if (beat % 120 != 0 && beat % 6 == 0 && cButton && !screenClasses.empty()) {  // about 100 ms
            static int timed;
            DWORD from = GetTickCount();
            std::vector<Ref> found;
            for (int i = 0, n = api->num_objects(); i < n; i++) {
                uintptr_t o = api->object(i);
                if (o && std::find(screenClasses.begin(), screenClasses.end(), classOf(o)) != screenClasses.end() && !isCDO(o)) found.push_back({i, o});
            }
            if (timed < 3) { timed++; MT_LOG("quick garage screen scan: %lu ms", GetTickCount() - from); }
            std::lock_guard<std::mutex> g(lock);
            for (auto& r : found)
                if (std::none_of(screens.begin(), screens.end(), [&](const Ref& s) { return s.ptr == r.ptr; })) {
                    MT_LOG("menu %lu: the quick scan found garage screen %p", GetTickCount(), (void*)r.ptr);
                    screens.push_back(r);
                }
        }
        if (beat % 120 == 0) {  // about two seconds
            std::vector<Ref> v, w, m, sun;
            bool lookForNotice = !warned && beat < 120 * 90;  // the notice comes up in the first minutes
            for (int i = 0, n = api->num_objects(); i < n; i++) {
                uintptr_t o = api->object(i);
                if (!o) continue;
                if (isCDO(o)) {  // until the screen's class is known (a screen found below teaches it too)
                    if (canTune && screenClasses.empty() && classOf(o) != cScreen && api->is_a(classOf(o), cScreen)) screenClasses.push_back(classOf(o));
                    continue;
                }
                if (cSun && classOf(o) == cSun) sun.push_back({i, o});
                else if (api->is_a(classOf(o), cVeh)) v.push_back({i, o});
                else if (lookForNotice && cRich && classOf(o) == cRich && !richRead.count(o)) m.push_back({i, o});
                else if (canTune && api->is_a(classOf(o), cScreen)) w.push_back({i, o});
            }
            if (canTune && !cButton && !w.empty()) {  // the blueprints are only there once a garage has opened
                cSetting = api->find_object("WidgetBlueprintGeneratedClass", "W_Setting_C", "/Game/UI/Component/W_Setting");
                cButton = api->find_object("WidgetBlueprintGeneratedClass", "W_Button_C", "/Game/UI/Button/W_Button");
                if (!cSetting) cButton = 0;
            }
            if (!cButton) w.clear();
            for (auto& r : w)
                if (std::find(screenClasses.begin(), screenClasses.end(), classOf(r.ptr)) == screenClasses.end()) screenClasses.push_back(classOf(r.ptr));
            uintptr_t ex = smoke ? smoke : findEffect(oFxSmoke);
            std::lock_guard<std::mutex> g(lock);
            vehicles.swap(v);
            suns.swap(sun);
            for (auto& r : w)
                if (std::none_of(screens.begin(), screens.end(), [&](const Ref& s) { return s.ptr == r.ptr; }))
                    MT_LOG("menu %lu: the scan found garage screen %p", GetTickCount(), (void*)r.ptr);
            screens.swap(w);
            if (!m.empty()) richBlocks.swap(m);
            std::set<uintptr_t> here;
            for (auto& r : vehicles) here.insert(r.ptr);
            for (auto c = cars.begin(); c != cars.end();) c = here.count(c->first) ? std::next(c) : cars.erase(c);
            if (ex && !smoke) { smoke = ex; MT_LOG("smoke %s", objName(smoke).c_str()); }
        }
        if (beat % 1800 == 900) probeSave();  // about 30 s
        Sleep(16);
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    if (iniDisabled(L"ecu_tuning.ini")) return 0;
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
