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
// is a jet: a small glowing cone at the pipe as the hot core, and round it a flamethrower of
// thin streaks fired down the pipe with spread and turbulence, on M_AttachmentLight, yellow
// core, orange body and slow red wisps. They ride with the car. Each burst has an energy, the
// fuel that went unburnt: it sets how many streaks fire and how many leave the middle for a
// circle a little way out, around a noise-driven hot spot, so no two are the same.
//
// Each vehicle is tuned in the game: the garage's Tuning menu gets an Exhaust button beside
// Wheel/Tire Size, which opens rows of the game's own W_Setting, kept per Net_VehicleId in
// ecu_tuning.ini, [Vehicle<id>]. A tuned vehicle pops at PopsPerSecond * BackfireChance;
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
// Timing (0.5), FuelMap (0.5), Boost (0.5), AntiLag (0), AntiLagRevs (0.5), DPFDelete (0), SmokeAmount (3), SmokeLasts (4).
// Also Range (10000 cm).
//
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
struct Spark { Ref c; uintptr_t mid; Vec pos, vel; double age, life, width, length, kick, heat; bool live; };
struct Slot { Ref owner; Pipe pipe; Vec dir, r1, r2; Ref cone[2], light; uintptr_t coneMid[2]; double glow, flash, bias, coneLength, style, size, debt, spread, energy, swirl, spin, phase[3]; DWORD start, end; bool on; std::vector<Spark> sparks; };
struct Voice { Ref owner; DWORD at; float pitch, volume, lowpass, highpass; };
struct Flutter { double pitch, low, high, volume; int shots; DWORD gap; int turbos; };
struct Trim { float base, wrote; bool set; };
struct Car { DWORD flutterAt = 0, ecuAt = 0, bangAt = 0, alsUntil = 0, liftAt = 0; bool lagging = false, host = false, pedalSeen = false, armed = false; Trim trim[13] = {}; Ref whine = {}; DWORD whineCheck = 0, whineAt = 0; int blower = -1; bool whineNear = false; float whineLevel = 0; float rpm = 0, fuel = 0, spool = 0; DWORD next = 0; bool read = false; std::vector<Pipe> pipes; std::vector<Slot> slots; std::vector<Ref> smokes; };
struct FStr { const wchar_t* data; int num, max; };
struct Tune { double on, pops, revs, volume, flames, length, size, dpf, smoke, lasts, spread, chance, bang, timing, fuel, boost, lag, lagRevs, flameRevs; bool own; };

static const struct { const wchar_t* key; double Tune::*field; } KEYS[] = {
    {L"Enabled", &Tune::on}, {L"PopRate", &Tune::pops}, {L"MinRevs", &Tune::revs}, {L"PopVolume", &Tune::volume},
    {L"Flames", &Tune::flames}, {L"FlameLength", &Tune::length}, {L"FlameSize", &Tune::size},
    {L"DPFDelete", &Tune::dpf}, {L"SmokeAmount", &Tune::smoke}, {L"SmokeLasts", &Tune::lasts}, {L"FlameWidth", &Tune::spread},
    {L"BackfireChance", &Tune::chance}, {L"BangChance", &Tune::bang},
    {L"Timing", &Tune::timing}, {L"FuelMap", &Tune::fuel}, {L"Boost", &Tune::boost}, {L"AntiLag", &Tune::lag}, {L"AntiLagRevs", &Tune::lagRevs}, {L"FlameRevs", &Tune::flameRevs}};
enum { TOGGLE, SLIDER, REAL };
enum { BURNS = 1, POPS_ = 2, DIESEL = 4, TURBO = 8 };  // what a row needs from the engine
// A slider always reads 0 to 1 and maps onto min..max. Timing, fuel and boost have stock at 0.50.
static const struct { const wchar_t* label; double Tune::*field; float min, max; int kind, needs; } ROWS[] = {
    {L"Ignition timing", &Tune::timing, 0, 1, SLIDER, BURNS},
    {L"Fuel map", &Tune::fuel, 0, 1, SLIDER, BURNS},
    {L"Boost pressure", &Tune::boost, 0, 1, SLIDER, TURBO},
    {L"Anti-lag", &Tune::lag, 0, 1, TOGGLE, TURBO},
    {L"Anti-lag starts at share of rev limit", &Tune::lagRevs, 0.1f, 0.9f, REAL, TURBO},
    {L"Backfire", &Tune::on, 0, 1, TOGGLE, POPS_},
    {L"Backfire chance", &Tune::chance, 0, 1, SLIDER, POPS_},
    {L"Pops per second", &Tune::pops, 1, 30, REAL, POPS_},
    {L"Bang chance", &Tune::bang, 0, 0.2f, SLIDER, POPS_},
    {L"Pops start at share of rev limit", &Tune::revs, 0.1f, 0.9f, REAL, POPS_},
    {L"Pop volume", &Tune::volume, 1, 3, SLIDER, POPS_},
    {L"Flames", &Tune::flames, 0, 1, TOGGLE, POPS_},
    {L"Flames start at share of rev limit", &Tune::flameRevs, 0.1f, 0.9f, REAL, POPS_},
    {L"Flame size", &Tune::size, 0.1f, 0.3f, SLIDER, POPS_},
    {L"Flame length", &Tune::length, 0.01f, 0.1f, SLIDER, POPS_},
    {L"Flame spread", &Tune::spread, 0.1f, 0.35f, SLIDER, POPS_},
    {L"DPF delete", &Tune::dpf, 0, 1, TOGGLE, DIESEL},
    {L"Smoke amount", &Tune::smoke, 0, 5, SLIDER, DIESEL},
    {L"Smoke lasts, times normal", &Tune::lasts, 1, 5, REAL, DIESEL}};
enum { NROWS = 19 };

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
    uintptr_t button, inner;
    bool buttonHeld, open;
    int labelFrames;
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
static std::set<std::wstring> tuned;  // ini sections, read once: no file work during play
static volatile LONG queued;
static DWORD lastTick;
static int logged;

static uintptr_t cFx, cSetting, cButton, cButtonBase, cText, cUButton, gameplayCDO, systemCDO, stringCDO, niagaraCDO, widgetCDO, textCDO;
static uintptr_t fLoc, fVelocity, fPawn, fRoot, fBounds, fSound, fSpawn, fSetFloat, fSetColor, fSetVec, fDilation, fStop, fToName, fPath, fRef, fLoad;
static uintptr_t fCreate, fAddChild, fChildIndex, fChildAt, fChildCount, fToText, fSetText, fSetRich, fPressed, fRemove, fSetVis, fIsVisible, fPadding;
static uintptr_t sound, cone, ball, glow, smoke, playerNow;
static uintptr_t cMeshComp, fAddComp, fFinish, fMobility, fMesh, fCollision, fShadow, fMakeMid, fMidColor, fMidScalar, fRelLoc, fMeshVis;
static bool canFlame = true, canGlow = true, canEcu = true;
static uintptr_t fIsServer, whistle, fWhinePitch, fWhineVolume, fWhineIsOn, fWhinePlay, fWhineStop;
static int oFuelType = -1, oBrake = -1, oPedal = -1, oTurbineSpeed = -1;
static uintptr_t waves[6];
static int oTrim[13];
static uintptr_t cLight, fIntensity, fLightColor, fLightRadius, fLightShadows, fLightUnits;
static int oRelLocAt, oRelRotAt, oTurbo = -1, oParts = -1, oPartKey = -1, oPartSlot = -1, partSize = 0;
static uintptr_t lastMid, fRelRot, fFlutter, fLowOn, fLowHz, fHighOn, fHighHz, flutterShot, flutterRoom;
static bool canFlutter = true;
static uint64_t lightColorName, lightPowerName;
static uintptr_t mathCDO, fRelScale, fAttached, fToWorld, fInverse;
static uint64_t smokeAmountName, smokeRadiusName, smokePressureName, smokeColorName, smokeVelocityName;
static int oEngine, oId, oData, oProp, oHot, oNative, oAfter, oMax, oMotor, oRpm, oThrottle, oFxSmoke;
static int oVehicle, oBox, oPipes, oRowName, oRowSpin, oSpinText, oSpinValue, oBtnName, oBtnRich, oBtnText, oBtnInner, pBoundsExtent, pBoundsOrigin, pEffectAt, pEffectRot;
static void (__fastcall* spinRange)(uintptr_t self, float min, float max, float step, bool snap);
static void (__fastcall* spinSet)(uintptr_t self, float value);
static int pPathIn, pPathRet, pRefIn, pRefRet, pLoadIn, pLoadRet, pNameIn, pNameRet, pTextIn, pTextRet;
static int pCtx, pSound, pAt, pVolume, pPitch, pSystem, pAttach, pDestroy, pActivate, pSpawnRet;

static bool alive(const Ref& r) { return r.ptr && api->object(r.index) == r.ptr; }
static double roll() { return rand() / (double)RAND_MAX; }
static FStr str(const wchar_t* s) { int n = (int)wcslen(s) + 1; return {s, n, n}; }

static Vec callVec(uintptr_t obj, uintptr_t fn) {
    Vec v{};
    api->process_event(obj, fn, &v);
    return v;
}

// x = 0 at MinRevs, 1 at the limiter.
static double popRate(double ratio, double prob, const Tune& t) {
    double x = (ratio - t.revs) / (1 - t.revs);
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    return t.pops * (prob > 1 ? 1 : prob) * x * x;
}

static std::wstring section(int64_t id) { return L"Vehicle" + std::to_wstring(id); }

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

// shortcut: engine+oPipes-0x10 is an unreflected TArray<FTransform> beside ExhaustBlackSmokes;
// the bounds check drops it if an update moves it, and the next source takes over.
// shortcut: GetAttachedActors' array stays allocated, once per vehicle; plugins have no FMemory free.
static void readPipes(uintptr_t v, uintptr_t engine, Car& car) {
    car.read = true;
    car.pipes.clear();
    uintptr_t list = get<uintptr_t>(engine + oPipes - 0x10), root = rootOf(v);
    int n = get<int>(engine + oPipes - 8);
    for (int i = 0; list && n > 0 && n <= 16 && i < n; i++) {
        Pipe pipe{};
        double scale = 0;
        rd(list + i * 0x60ull, pipe.spin, sizeof pipe.spin);
        rd(list + i * 0x60ull + 0x20, &pipe.at, sizeof pipe.at);
        rd(list + i * 0x60ull + 0x40, &scale, sizeof scale);
        pipe.radius = scale * 5;  // what the game gives its own smoke
        if (fabs(pipe.at.x) < 3000 && fabs(pipe.at.y) < 1000 && fabs(pipe.at.z) < 1000 && pipe.radius > 0) car.pipes.push_back(pipe);
    }
    int sockets = (int)car.pipes.size(), tips = 0;
    if (car.pipes.empty()) {  // the fitted exhaust tips
        unsigned char p[0x20] = {0};
        p[off(fAttached, "bResetArray")] = 1;
        p[off(fAttached, "bRecursivelyIncludeAttachedActors")] = 1;
        api->process_event(v, fAttached, p);
        uintptr_t data = get<uintptr_t>((uintptr_t)p + off(fAttached, "OutActors"));
        int num = get<int>((uintptr_t)p + off(fAttached, "OutActors") + 8);
        for (int i = 0; data && i < num && i < 64; i++) {
            uintptr_t a = get<uintptr_t>(data + i * 8ull);
            std::string name = a ? objName(classOf(a)) : "";
            for (auto& c : name) c = (char)tolower(c);
            if (name.find("exhaust") == std::string::npos && name.find("muffler") == std::string::npos) continue;
            car.pipes.push_back({toRoot(root, callVec(a, fLoc)), {0, 0, 1, 0}, 5});
            tips++;
        }
    }
    if (car.pipes.empty()) {  // a pair low on the rear end; the long side of the box is the length
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
    car.slots.resize(car.pipes.size());
    MT_LOG("%s: %d sockets, %d fitted tips, firing from %d pipes", objName(classOf(v)).c_str(), sockets, tips, (int)car.pipes.size());
}

// The way the pipe blows: a socket's X.
static Vec aim(const Pipe& p) {
    const double x = p.spin[0], y = p.spin[1], z = p.spin[2], w = p.spin[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};
}

// FQuat::Rotator, in degrees.
static Vec rotatorOf(const Pipe& p) {
    const double x = p.spin[0], y = p.spin[1], z = p.spin[2], w = p.spin[3], deg = 180 / 3.14159265358979;
    return {asin(2 * (z * x - w * y)) * deg, atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)) * deg,
            atan2(-2 * (w * x + y * z), 1 - 2 * (x * x + y * y)) * deg};
}

static Ref refOf(uintptr_t o) { return {o ? get<int>(o + L().obj_index) : 0, o}; }

static uintptr_t makeMesh(uintptr_t v, uintptr_t mesh, const Pipe& pipe, float r, float g, float b) {
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
    memcpy(f + off(fFinish, "Component"), &c, 8);
    memcpy(f + off(fFinish, "RelativeTransform"), p + at, 0x60);
    api->process_event(v, fFinish, f);
    unsigned char d[0x30] = {0};
    memcpy(d + off(fMakeMid, "SourceMaterial"), &glow, 8);
    api->process_event(c, fMakeMid, d);
    uintptr_t mid = 0;
    memcpy(&mid, d + off(fMakeMid, "ReturnValue"), 8);
    if (mid) {
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
    memcpy(f + off(fFinish, "Component"), &c, 8);
    memcpy(f + off(fFinish, "RelativeTransform"), p + at, 0x60);
    api->process_event(v, fFinish, f);
    return c;
}

// Pooled per pipe and only shown or hidden: creating these per pop is object churn for the
// garbage collector to stall on. Traffic gets a smaller pool than the player.
static void flame(uintptr_t v, Car& car, int i, const Tune& t, double energy, double seconds, double bigger, double flash, int style) {
    if (!cone || !ball || !glow || !canFlame) return;
    Slot& s = car.slots[i];
    const Pipe& pipe = car.pipes[i];
    DWORD now = GetTickCount();
    s.owner = refOf(v);
    s.pipe = pipe;
    s.dir = aim(pipe);
    double ux = fabs(s.dir.z) > 0.9 ? 1 : 0, uz = 1 - ux;  // any axis not along the pipe
    Vec a{s.dir.y * uz, s.dir.z * ux - s.dir.x * uz, -s.dir.y * ux};
    double n = sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    s.r1 = {a.x / n, a.y / n, a.z / n};
    s.r2 = {s.dir.y * s.r1.z - s.dir.z * s.r1.y, s.dir.z * s.r1.x - s.dir.x * s.r1.z, s.dir.x * s.r1.y - s.dir.y * s.r1.x};
    s.size = 0.7 * bigger * t.size;
    s.bias = (t.fuel - 0.5) * 0.5;  // rich burns redder, lean bluer
    s.flash = flash;
    if (s.sparks.empty()) s.sparks.resize(v == playerNow ? 14 : 3);
    s.style = style;
    s.spread = t.spread * (style == 2 ? 1.8 : 1);
    s.energy = energy;
    if (!s.on) {  // a new burst gets its own noise, so no two are alike
        s.swirl = roll() * 6.2831853;
        s.spin = (roll() < 0.5 ? -1 : 1) * (4 + 8 * roll());
        for (double& f : s.phase) f = roll() * 6.2831853;
    }
    for (int c = 0; c < 2; c++)
        if (!alive(s.cone[c])) { s.cone[c] = refOf(makeMesh(v, cone, pipe, 1, 0.5f, 0.1f)); s.coneMid[c] = lastMid; }
    s.glow = canGlow;
    if (s.glow > 0 && !alive(s.light)) s.light = refOf(makeLight(v, 200 + 1500 * s.size));
    s.coneLength = s.size * 1.6 * (0.75 + 0.5 * roll());
    s.start = now;
    s.end = now + (DWORD)(seconds * 1000 < 34 ? 34 : seconds * 1000);
    if (seconds < 0.05 && !s.on && style != 1) s.debt = 12;
    showMesh(s.cone[0], style != 2);
    showMesh(s.cone[1], style != 2);
    showMesh(s.light, s.glow > 0);
    s.on = true;
}

// One streak off the pipe. Three kinds, by pool position: a thin fast yellow core, the orange
// body, and a slow wide red wisp that scatters like the diesel smoke does.
// Blue at its hottest, through yellow and orange, out as red. x runs 0 to 1.
static void heatRgb(double x, float* rgb) {
    static const float RAMP[4][3] = {{0.02f, 0.08f, 1}, {1, 0.55f, 0.01f}, {1, 0.12f, 0}, {0.9f, 0.01f, 0}};
    double at = (x < 0 ? 0 : x > 1 ? 1 : x) * 2.999;
    int band = (int)at;
    float mix = (float)(at - band);
    for (int i = 0; i < 3; i++) rgb[i] = RAMP[band][i] + (RAMP[band + 1][i] - RAMP[band][i]) * mix;
}

static void heatColor(uintptr_t mid, double x) {
    if (!mid) return;
    struct { uint64_t name; float rgba[4]; } color = {lightColorName, {0, 0, 0, 1}};
    heatRgb(x, color.rgba);
    api->process_event(mid, fMidColor, &color);
}

static void launch(Slot& s, Spark& p, int kind, double seconds) {
    static const double WIDTH[3] = {0.08, 0.12, 0.2}, LENGTH[3] = {1.6, 2, 1.2}, LIFE[3] = {0.05, 0.07, 0.1}, SPREAD[3] = {0.03, 0.05, 0.11}, PACE[3] = {1, 1, 0.55};
    double speed = s.size * 2400 * (0.7 + 0.6 * roll()) * (0.8 + 0.4 * s.energy) * PACE[kind];
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
    p.life = LIFE[kind] * (1 + roll());
    double fan = speed * SPREAD[kind] * roll() + circle / (p.life * 0.6);  // sideways enough to reach the circle in its flight
    p.vel = {s.dir.x * speed + out.x * fan, s.dir.y * speed + out.y * fan, s.dir.z * speed + out.z * fan};
    p.width = s.size * WIDTH[kind] * 1.4;
    p.length = s.size * LENGTH[kind];
    p.kick = speed * 0.8;
    p.heat = s.bias + (1 - s.energy) * 0.33 + kind * 0.22;  // the hotter the burst, the further down the ramp it starts
    p.live = true;
    // Laid along its own flight, so the streaks fan out as a cone instead of marching in parallel.
    double n = sqrt(p.vel.x * p.vel.x + p.vel.y * p.vel.y + p.vel.z * p.vel.z);
    if (n > 0) {
        double vx = p.vel.x / n, vy = p.vel.y / n, vz = p.vel.z / n, w = 1 + vz;  // the shortest turn from Z
        Pipe laid{};
        if (w < 0.001) laid.spin[0] = 1;
        else {
            double m = sqrt(vy * vy + vx * vx + w * w);
            laid.spin[0] = -vy / m; laid.spin[1] = vx / m; laid.spin[3] = w / m;
        }
        Vec rotation = rotatorOf(laid);
        unsigned char turn2[0x120] = {0};
        memcpy(turn2 + oRelRotAt, &rotation, sizeof rotation);
        api->process_event(p.c.ptr, fRelRot, turn2);
    }
    showMesh(p.c, true);
}

static void tickFlames(DWORD now, double dt) {
    static unsigned frame;
    frame++;
    int budget = 4;  // new streak meshes per frame, so a first burst cannot hitch
    for (auto& entry : cars)
        for (Slot& s : entry.second.slots) {
            if (s.on && (int)(now - s.end) >= 0) {
                s.on = false;
                showMesh(s.cone[0], false);
                showMesh(s.cone[1], false);
                showMesh(s.light, false);
            }
            if (s.on) {
                double u = (now - s.start) / (double)(s.end > s.start ? s.end - s.start : 1);
                double shape = s.end - s.start <= 50 ? 1 : u < 0.25 ? u / 0.25 : 1 - (u - 0.25) / 0.75;
                if (s.glow > 0 && alive(s.light)) {
                    double reach = 35 + s.coneLength * 50 * shape;
                    placeMesh(s.light, {s.pipe.at.x + s.dir.x * reach, s.pipe.at.y + s.dir.y * reach, s.pipe.at.z + s.dir.z * reach}, 1, 1);
                    struct { float rgba[4]; bool srgb; } tint = {{0, 0, 0, 1}, false};
                    heatRgb(s.bias + (1 - s.energy) * 0.33 + 0.15 + u * 0.6, tint.rgba);
                    if (frame % 2 == 0) api->process_event(s.light.ptr, fLightColor, &tint);
                    float power = (float)(40 * s.flash * s.size / 0.07 * (0.5 + s.energy) * shape * (0.7 + 0.6 * roll()));
                    api->process_event(s.light.ptr, fIntensity, &power);
                }
                for (int c = 0; c < 2; c++) {
                    if (!alive(s.cone[c]) || s.style == 2) continue;
                    double inner = c ? 0.55 : 1, length = s.coneLength * inner * shape * (0.85 + 0.3 * roll()), half = length * 50;
                    Vec at{s.pipe.at.x + s.dir.x * half, s.pipe.at.y + s.dir.y * half, s.pipe.at.z + s.dir.z * half};
                    heatColor(s.coneMid[c], s.bias + (1 - s.energy) * 0.33 + (c ? 0 : 0.25) + u * 0.6);  // the inner cone runs hotter
                    placeMesh(s.cone[c], at, s.size * 0.22 * inner, length);
                }
                if (s.style != 1) s.debt += 70 * (0.5 + s.energy) * dt;
                for (size_t i = 0; i < s.sparks.size() && s.debt >= 1; i++) {
                    Spark& p = s.sparks[i];
                    if (p.live) continue;
                    if (!alive(p.c)) {
                        if (budget <= 0 || !alive(s.owner)) continue;
                        budget--;
                        p.c = refOf(makeMesh(s.owner.ptr, ball, s.pipe, 1, 0.5f, 0.1f));
                        p.mid = lastMid;
                        if (!p.c.ptr) continue;
                    }
                    launch(s, p, (int)(i % 3), (now - s.start) / 1000.0 + entry.first % 97);
                    s.debt -= 1;
                }
                if (s.debt > 12) s.debt = 12;
            }
            for (Spark& p : s.sparks) {
                if (!p.live) continue;
                p.age += dt;
                if (p.age >= p.life || !alive(p.c)) {
                    p.live = false;
                    showMesh(p.c, false);
                    continue;
                }
                double u = p.age / p.life, k = sin(3.14159265 * sqrt(u));  // swells fast, tapers out
                double lick = p.kick * dt * (1 + 3 * s.spread);  // the turbulence that makes it lick
                p.vel.x += (roll() - 0.5) * lick;
                p.vel.y += (roll() - 0.5) * lick;
                p.vel.z += (roll() - 0.5) * lick;
                p.pos.x += p.vel.x * dt; p.pos.y += p.vel.y * dt; p.pos.z += p.vel.z * dt;
                if ((frame + (unsigned)(&p - s.sparks.data())) % 2 == 0) heatColor(p.mid, p.heat + u * (1 - p.heat));
                placeMesh(p.c, p.pos, p.width * k, p.length * (1 - 0.4 * u));
            }
        }
}

// 0 on a stock map, up to 1.2 with the timing fully retarded and the fuel fully rich.
static double unburnt(const Tune& t) {
    double retard = (0.5 - t.timing) * 2, rich = (t.fuel - 0.5) * 2;
    return 0.6 * (retard > 0 ? retard : 0) + 0.6 * (rich > 0 ? rich : 0);
}

// Kinds of pop and bang. One sound, so a kind is its pitch, weight, how many times it fires and how fast.
// style: 0 cone and streaks, 1 a jet (cone only), 2 scattered streaks with no cone.
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

struct Fired { Ref owner; DWORD at; float volume, pitch; double flame, seconds, size, flash, energy, ratio; int style; bool everyPipe; };
static std::vector<Fired> fired;

static void fire(uintptr_t v, uintptr_t engine, Car& car, const Tune& t, const Shot& k, double energy, double ratio, bool everyPipe, DWORD now) {
    if (!car.read) readPipes(v, engine, car);
    for (int i = 0; i < k.repeats && fired.size() < 64; i++)
        fired.push_back({refOf(v), now + i * k.gap, (float)(t.volume * k.volume * (everyPipe ? 1 : 1.5) * (0.8 + 0.4 * roll()) * (1 - 0.15 * i)),
                         (float)(k.pitch * (1 + k.wander * (2 * roll() - 1))), k.flame, k.seconds, k.size, k.flash, energy, ratio, k.style, everyPipe});
    if (logged < 10) {
        logged++;
        MT_LOG("%s on %s: %d pipes, flame %d", everyPipe ? "bang" : "pop", objName(classOf(v)).c_str(), (int)car.pipes.size(), t.flames != 0);
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
        const Tune& t = tuneOf(get<int64_t>(v + oId));
        Vec at = callVec(v, fLoc);
        unsigned char p[0xa0] = {0};
        memcpy(p + pCtx, &v, 8);
        uintptr_t wave = waves[rand() % 6];
        if (!wave) wave = sound;
        memcpy(p + pSound, &wave, 8);
        memcpy(p + pAt, &at, sizeof at);
        memcpy(p + pVolume, &x.volume, 4);
        memcpy(p + pPitch, &x.pitch, 4);
        api->process_event(gameplayCDO, fSound, p);
        if (t.flames == 0 || car.pipes.empty() || x.ratio < t.flameRevs || roll() >= x.flame) continue;
        // A retarded or rich map burns in the pipe, and no two are quite the same length.
        double seconds = 2 * t.length * x.seconds * (0.5 + 0.5 * x.energy) * (1 + unburnt(t)) * (0.6 + 0.8 * roll());
        if (seconds > 0.36) seconds = 0.36;
        if (x.everyPipe) for (int i2 = 0; i2 < (int)car.pipes.size(); i2++) flame(v, car, i2, t, x.energy, seconds, x.size, x.flash, x.style);
        else flame(v, car, rand() % (int)car.pipes.size(), t, x.energy, seconds, x.size, x.flash, x.style);
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
        for (const Pipe& pipe : car.pipes) {
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
        if (!alive(x.owner)) continue;
        uintptr_t root = rootOf(x.owner.ptr);
        unsigned char p[0xc0] = {0};
        memcpy(p + off(fFlutter, "Sound"), &flutterShot, 8);
        memcpy(p + off(fFlutter, "AttachToComponent"), &root, 8);
        memcpy(p + off(fFlutter, "VolumeMultiplier"), &x.volume, 4);
        memcpy(p + off(fFlutter, "PitchMultiplier"), &x.pitch, 4);
        memcpy(p + off(fFlutter, "AttenuationSettings"), &flutterRoom, 8);
        p[off(fFlutter, "bAutoDestroy")] = 1;
        api->process_event(gameplayCDO, fFlutter, p);
        uintptr_t audio = 0;
        memcpy(&audio, p + off(fFlutter, "ReturnValue"), 8);
        if (!audio) continue;
        if (x.lowpass < 20000) {
            unsigned char on[8] = {1};
            api->process_event(audio, fLowOn, on);
            api->process_event(audio, fLowHz, &x.lowpass);
        }
        if (x.highpass > 0) {
            unsigned char on[8] = {1};
            api->process_event(audio, fHighOn, on);
            api->process_event(audio, fHighHz, &x.highpass);
        }
    }
}

static int engineHas(uintptr_t vehicle);

// A part swap rewrites these, so a value that is not what was last written is the new base.
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
    double scale[13] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 + 0.08 * a};
    for (int fuel = 0; fuel < 2; fuel++) {
        scale[fuel * 6] = torque; scale[fuel * 6 + 1] = torque * (1 + 0.25 * b);
        scale[fuel * 6 + 2] = heat; scale[fuel * 6 + 3] = burn; scale[fuel * 6 + 4] = als ? 0.5 : 1;
        scale[fuel * 6 + 5] = 1 + 0.35 * b;  // BoostBar is this times the turbine speed
    }
    for (int i = 0; i < 13; i++) {
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
        car.whineCheck = now + 1000;
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
        // Teh's Hellcat at the top of its revs, which is nearly all blower. The vanilla whistle
        // is only for a game without his pack: it hisses more than it whines.
        whistle = loadAsset(L"/Game/Cars/Parts/Engine/Sound/hellcatv8/hellcathigh.hellcathigh");
        if (!whistle) whistle = loadAsset(L"/Game/Sounds/Vehicle/Intake/TurboWhistle_01_High.TurboWhistle_01_High");
        flutterRoom = loadAsset(L"/Game/Sounds/EngineSoundAttenuation.EngineSoundAttenuation");
        MT_LOG("supercharger whine %s", whistle ? "loaded" : "NOT LOADED");
        if (!whistle) return;
    }
    if (!alive(car.whine)) {
        uintptr_t root = rootOf(v);
        float quiet = 0.001f, one = 1;
        unsigned char q[0xc0] = {0};
        memcpy(q + off(fFlutter, "Sound"), &whistle, 8);
        memcpy(q + off(fFlutter, "AttachToComponent"), &root, 8);
        memcpy(q + off(fFlutter, "VolumeMultiplier"), &quiet, 4);
        memcpy(q + off(fFlutter, "PitchMultiplier"), &one, 4);
        memcpy(q + off(fFlutter, "AttenuationSettings"), &flutterRoom, 8);
        int stops = off(fFlutter, "bStopWhenAttachedToDestroyed");
        if (stops >= 0) q[stops] = 1;
        api->process_event(gameplayCDO, fFlutter, q);
        uintptr_t audio = 0;
        memcpy(&audio, q + off(fFlutter, "ReturnValue"), 8);
        if (!audio) return;
        car.whine = refOf(audio);
        // The rumble under it is the V8. Taking that off leaves the whine for any engine.
        unsigned char filter[8] = {1};
        float above = 1400;
        api->process_event(audio, fHighOn, filter);
        api->process_event(audio, fHighHz, &above);
        MT_LOG("supercharger on %s, stage %d, sound %s", objName(classOf(v)).c_str(), car.blower, objName(whistle).c_str());
    }
    // Belt driven, so it follows the revs alone; load is what makes it loud.
    double want = ratio < 0.02 ? 0 : pedal > 0.1f ? pedal * (0.25 + 0.75 * ratio) : 0.05 * ratio;
    car.whineLevel += (float)((want - car.whineLevel) * 0.35);
    double spin = ratio / 0.8;  // the clip is high in the rev range, so about here it plays as recorded
    float pitch = (float)((spin < 0.55 ? 0.55 : spin > 1.5 ? 1.5 : spin) * (1 + 0.03 * car.blower)), volume = (float)(2 * car.whineLevel * (0.7 + 0.2 * car.blower));
    if (volume < 0.001f) volume = 0.001f;
    api->process_event(car.whine.ptr, fWhinePitch, &pitch);
    api->process_event(car.whine.ptr, fWhineVolume, &volume);
    if (!flag(car.whine.ptr, fWhineIsOn)) {  // the wave is not a loop: start it over
        float from = 0;
        api->process_event(car.whine.ptr, fWhinePlay, &from);
    }
}

static void one(uintptr_t v, uintptr_t pawn, const Vec& me, double dt, DWORD now) {
    uintptr_t engine = get<uintptr_t>(v + oEngine);
    uintptr_t data = engine ? get<uintptr_t>(engine + oData) : 0;
    if (!data) return;
    float prob = get<float>(data + oProp + oAfter), maxRpm = get<float>(data + oProp + oMax);
    int has = engineHas(v);
    if (maxRpm <= 0 || !has) return;  // electric: no map, no exhaust

    const Tune& t = tuneOf(get<int64_t>(v + oId));
    Car& car = cars[v];
    tuneEngine(v, engine, car, t, now);
    float rpm = get<float>(engine + oHot + oRpm), throttle = get<float>(engine + oHot + oThrottle);
    // The engine's own throttle drops at the limiter and on every shift. Only the pedal is a lift.
    float pedal = throttle;
    if (oPedal >= 0) {
        float raw = get<float>(v + oPedal);
        if (raw > 0.05f) car.pedalSeen = true;
        if (car.pedalSeen) pedal = raw;
    }
    double ratio = rpm / maxRpm;
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
        double push = 0.6 * speed + 0.4 * ratio * ratio;
        car.bangAt = roll() < t.bang ? now + 40 + rand() % 300 : 0;  // the dice are per lift, nothing to do with pops
        if (v == playerNow && flutterLogged < 300) {
            flutterLogged++;
            MT_LOG("lift: revs %.2f turbine %.2f push %.2f turbo %d anti-lag %d", ratio, speed, push, turbo, lagging);
        }
        if (turbo && push > 0.2 && ratio > 0.3 && (int)(now - car.flutterAt) >= 0 && inRange(v, pawn, me)) {
            car.flutterAt = now + 400;
            queueFlutter(v, t, push > 1 ? 1 : push, now);
        }
    }
    car.fuel = pedal > car.fuel ? pedal : car.fuel * (float)(1 - (dt > 0.05 ? 0.05 : dt));  // fades over a second or so
    car.rpm = rpm;
    if ((has & DIESEL) && (t.dpf != 0 || !car.smokes.empty())) tickSmoke(v, engine, car, t, rpm, throttle, inRange(v, pawn, me));

    if (prob <= 0) return;
    if (get<int>(engine + oNative + 8)) { int none = 0; wr(engine + oNative + 8, &none, 4); }  // silences the game's own
    if (t.on == 0) return;
    // A stock map rarely fires in the pipe. The map decides how often, and for how long after a lift.
    double late = unburnt(t);
    prob = (float)(t.chance * (0.4 + late));  // the engine's own number only says it can pop at all
    if (lagging && prob < 0.25f) prob = 0.25f;  // it burns fuel in the pipe by design
    if (ratio <= t.revs) return;
    bool afterLift = car.liftAt && (int)(now - car.liftAt) < (int)(250 + 900 * late + 400 * car.fuel);
    if (!afterLift && !(ratio >= 0.97 && late > 0.2) && !(lagging && pedal < 0.1f)) return;
    bool popped = (int)(now - car.next) >= 0 && roll() < popRate(ratio, prob, t) * dt;
    double x = (ratio - t.revs) / (1 - t.revs);
    bool banged = car.bangAt && (int)(now - car.bangAt) >= 0;
    if (!popped && !banged) return;
    if (!inRange(v, pawn, me)) return;
    if (!sound || !cone || !ball || !glow) {  // a blocking load, so on a pop and never while a world is loading
        sound = loadAsset(L"/Game/Cars/Parts/Engine/Sound/Backfire/SC_Backfire.SC_Backfire");
        cone = loadAsset(L"/Engine/BasicShapes/Cone.Cone");
        ball = loadAsset(L"/Engine/BasicShapes/Sphere.Sphere");
        glow = loadAsset(L"/Game/Cars/Materials/M_AttachmentLight.M_AttachmentLight");
        for (int w = 0; w < 6; w++) {
            wchar_t path[96];
            swprintf_s(path, L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_0%d.Backfire_0%d", w + 1, w + 1);
            waves[w] = loadAsset(path);
        }
        MT_LOG("sound %s, cone %d, sphere %d, glow %d", sound ? objName(sound).c_str() : "NOT LOADED", cone != 0, ball != 0, glow != 0);
        if (!sound) return;
    }
    // What went unburnt: the revs, the fuel of the throttle just lifted off, and a turbo pushing back.
    double energy = x * (0.4 + 0.6 * car.fuel);
    if (oTurbo >= 0 && get<uint8_t>(engine + oTurbo)) energy += 0.3;
    energy = energy < 0 ? 0 : energy > 1 ? 1 : energy;
    if (popped) {
        // A rich or retarded map leans to crackle, burble and chatter.
        fire(v, engine, car, t, POPS[roll() < late * 0.5 ? UNBURNT_POPS[rand() % 3] : rand() % 6], energy, ratio, false, now);
        car.next = now + 60;
    }
    if (banged) {
        fire(v, engine, car, t, BANGS[rand() % 6], energy, ratio, true, now);
        car.bangAt = 0;
    }
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
    if (ROWS[i].kind == TOGGLE) swprintf_s(text, L"%s", v != 0 ? L"On" : L"Off");
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
        if (spinRange) {
            spinRange(spin, 1, (float)spinTop(i, t), 0.01f, true);
            spinSet(spin, (float)toSpin(i, t));
        }
        showRow(i, t);
        setVisible(w, (engineHas(vehicle) & ROWS[i].needs) != 0);
    }
    panel.open = true;
}

static void closeRows() {
    for (auto& r : panel.rows)
        if (r.widget) api->process_event(r.widget, fRemove, nullptr);
    for (uintptr_t w : panel.hidden) setVisible(w, true);
    panel.hidden.clear();
    memset(panel.rows, 0, sizeof panel.rows);
    panel.open = false;
}

static void tickPanel(DWORD now) {
    if (panel.dirty && now - panel.dirty > 800) { saveTune(panel.id, tuneOf(panel.id)); panel.dirty = 0; }
    uintptr_t screen = 0, box = 0;
    for (auto& r : screens) {
        if (!alive(r)) continue;
        uintptr_t b = get<uintptr_t>(r.ptr + oBox);
        if (b && flag(r.ptr, fIsVisible)) { screen = r.ptr; box = b; break; }
    }
    if (!screen) {  // left with the rows open: the menu must be back next time
        if (panel.open && alive(panel.screen)) closeRows();
        return;
    }
    uintptr_t vehicle = get<uintptr_t>(screen + oVehicle);
    int64_t id = vehicle ? get<int64_t>(vehicle + oId) : 0;
    if (!id) return;
    if (panel.screen.ptr != screen) {
        if (panel.dirty) saveTune(panel.id, tuneOf(panel.id));
        panel = {};
        panel.screen = {get<int>(screen + L().obj_index), screen};
    }
    Tune& t = tuneOf(id);
    if (panel.open) {
        if (childIndex(box, panel.rows[0].widget) < 0) {  // the game rebuilt the list and our rows with it
            panel.open = false; panel.button = 0; panel.hidden.clear();
            memset(panel.rows, 0, sizeof panel.rows);
            return;
        }
        if (panel.id != id) {
            if (panel.dirty) { saveTune(panel.id, tuneOf(panel.id)); panel.dirty = 0; }
            panel.id = id;
            for (int i = 0; i < NROWS; i++) {
                if (spinRange) spinRange(panel.rows[i].spin, 1, (float)spinTop(i, t), 0.01f, true);
                if (spinSet) spinSet(panel.rows[i].spin, (float)toSpin(i, t));
                showRow(i, t);
                setVisible(panel.rows[i].widget, (engineHas(vehicle) & ROWS[i].needs) != 0);
            }
            return;
        }
        for (int i = 0; spinRange && i < NROWS; i++) {
            float v = get<float>(panel.rows[i].spin + oSpinValue);
            if (fabs(v - toSpin(i, t)) < 0.002) continue;
            t.*ROWS[i].field = fromUi(i, fromSpin(i, v, t));
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
    if (!isA(first.ret, cButtonBase)) { panel.button = 0; return; }
    if (!engineHas(vehicle)) { panel.button = 0; return; }
    panel.button = create(screen, cButton, box);
    if (!panel.button) return;
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
        {&flutterShot, L"/Game/Sounds/Vehicle/Intake/QX_Flutter_Shot.QX_Flutter_Shot"},
        {&flutterRoom, L"/Game/Sounds/EngineSoundAttenuation.EngineSoundAttenuation"},
        {&whistle, L"/Game/Cars/Parts/Engine/Sound/hellcatv8/hellcathigh.hellcathigh"},
        {&waves[0], L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_01.Backfire_01"},
        {&waves[1], L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_02.Backfire_02"},
        {&waves[2], L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_03.Backfire_03"},
        {&waves[3], L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_04.Backfire_04"},
        {&waves[4], L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_05.Backfire_05"},
        {&waves[5], L"/Game/Cars/Parts/Engine/Sound/Backfire/Backfire_06.Backfire_06"},
    };
    for (auto& h : held) {
        if (!*h.ptr) continue;
        // Forget it and let the next use load it: the game collects these every minute, and a
        // load here on every collection was a hitch whether anything needed the asset or not.
        if (h.ref.ptr == *h.ptr && !alive(h.ref)) { *h.ptr = 0; h.ref = {}; flutterTries = 0; continue; }
        h.ref = refOf(*h.ptr);
    }
}

// shortcut: a diagnostic for the frame hiccups, remove once they are found.
struct SlowFrame {
    LARGE_INTEGER from;
    SlowFrame() { QueryPerformanceCounter(&from); }
    ~SlowFrame() {
        static int said;
        LARGE_INTEGER to, hz;
        QueryPerformanceCounter(&to);
        QueryPerformanceFrequency(&hz);
        double ms = (to.QuadPart - from.QuadPart) * 1000.0 / hz.QuadPart;
        if (ms > 4 && said < 60) { said++; MT_LOG("slow tick %.1f ms: %d cars, %d voices, %d fired", ms, (int)cars.size(), (int)voices.size(), (int)fired.size()); }
    }
};

static void tick(void*) {
    SlowFrame slow;
    InterlockedExchange(&queued, 0);
    keepLoaded();
    std::lock_guard<std::mutex> g(lock);
    if (vehicles.empty()) return;
    DWORD now = GetTickCount();
    double dt = (now - lastTick) / 1000.0;
    dt = dt > 0.05 ? 0.05 : dt;
    lastTick = now;
    if (!smokeVelocityName) {
        lightColorName = nameOf(L"Light Color");
        lightPowerName = nameOf(L"LightIntensity");
        smokeAmountName = nameOf(L"User.SmokeAmount");
        smokeRadiusName = nameOf(L"User.SmokeExhaustRadius");
        smokePressureName = nameOf(L"User.SmokePressure");
        smokeColorName = nameOf(L"User.SmokeColor");
        smokeVelocityName = nameOf(L"User.SmokeVehicleVelocity");
    }
    tickFlames(now, dt);
    tickVoices(now);
    tickFired(now);
    tickPanel(now);
    struct { uintptr_t context; int index; uintptr_t ret; } who = {0, 0, 0};
    for (auto& r : vehicles)
        if (alive(r)) { who.context = r.ptr; break; }
    if (!who.context) return;
    api->process_event(gameplayCDO, fPawn, &who);
    playerNow = who.ret;
    Vec me = who.ret ? callVec(who.ret, fLoc) : Vec{};
    for (auto& r : vehicles)
        if (alive(r)) one(r.ptr, who.ret, me, dt, now);
}

static uintptr_t findEffect(int field) {
    for (int i = 0, n = api->num_objects(); i < n; i++) {
        uintptr_t o = api->object(i);
        if (o && !isCDO(o) && classOf(o) == cFx && get<uintptr_t>(o + field)) return get<uintptr_t>(o + field);
    }
    return 0;
}

static DWORD WINAPI run(LPVOID) {
    MT_LOG("ECU Tuning loaded");
    {
        static wchar_t names[65536];
        GetPrivateProfileSectionNamesW(names, 65536, (gameDir() + L"ecu_tuning.ini").c_str());
        for (const wchar_t* n = names; *n; n += wcslen(n) + 1) tuned.insert(n);
    }
    base.on = iniNum(L"ecu_tuning.ini", L"ECU", L"Enabled", 1);
    base.pops = iniNum(L"ecu_tuning.ini", L"ECU", L"PopRate", 5);
    base.revs = iniNum(L"ecu_tuning.ini", L"ECU", L"MinRevs", 0.55);
    base.volume = iniNum(L"ecu_tuning.ini", L"ECU", L"PopVolume", 2);
    base.flames = iniNum(L"ecu_tuning.ini", L"ECU", L"Flames", 1);
    base.length = iniNum(L"ecu_tuning.ini", L"ECU", L"FlameLength", 0.04);
    base.size = iniNum(L"ecu_tuning.ini", L"ECU", L"FlameSize", 0.2);
    kRange = iniNum(L"ecu_tuning.ini", L"ECU", L"Range", 10000);
    base.dpf = iniNum(L"ecu_tuning.ini", L"ECU", L"DPFDelete", 0);
    base.smoke = iniNum(L"ecu_tuning.ini", L"ECU", L"SmokeAmount", 3);
    base.spread = iniNum(L"ecu_tuning.ini", L"ECU", L"FlameWidth", 0.2);
    base.bang = iniNum(L"ecu_tuning.ini", L"ECU", L"BangChance", 0);
    base.flameRevs = iniNum(L"ecu_tuning.ini", L"ECU", L"FlameRevs", 0.6);
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
    fWhinePitch = api->find_object("Function", "SetPitchMultiplier", "AudioComponent");
    fWhineVolume = api->find_object("Function", "SetVolumeMultiplier", "AudioComponent");
    fWhineIsOn = api->find_object("Function", "IsPlaying", "AudioComponent");
    fWhinePlay = api->find_object("Function", "Play", "AudioComponent");
    fWhineStop = api->find_object("Function", "Stop", "AudioComponent");
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
    oData = off(cComp, "EngineData"); oHot = off(cComp, "NetLC_EngineHotState"); oNative = off(cComp, "AfterfireAudioComponents"); oPipes = off(cComp, "ExhaustBlackSmokes");
    oProp = off(cData, "EngineProperty"); oAfter = off(sProp, "AfterFireProbability"); oMax = off(sProp, "MaxRPM"); oFuelType = off(sProp, "FuelType"); oMotor = off(sProp, "MotorMaxPower");
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
    }
    oPedal = off(cVeh, "Throttle");
    oBrake = off(cVeh, "Brake");
    if (oTurbo == 0x3c4) oTurbineSpeed = 0x364;  // shortcut: TurbineSpeedRatio is not reflected, 0x60 before the turbo struct; the oTurbo test turns it off after an update, recheck it then
    MT_LOG("pedal at %x, turbine speed at %x", oPedal, oTurbineSpeed);
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

    auto range = find_sig("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F B6 44 24 50 48 8B D9 88 81 ?? ?? ?? ?? F3 0F 11 89");
    auto value = find_sig("40 53 48 83 EC 20 80 B9 ?? ?? ?? ?? 00 48 8B D9 F3 0F 10 81 ?? ?? ?? ?? F3 0F 5D C1 F3 0F 5F 81");
    if (range.size() == 1 && value.size() == 1) {
        spinRange = (decltype(spinRange))range[0];
        spinSet = (decltype(spinSet))value[0];
        oSpinValue = get<int>(range[0] + 57);  // the fourth store: value = min
        MT_LOG("number rows ready, value at +%x", oSpinValue);
    } else MT_LOG("number rows not found (%d, %d matches): the ECU rows show but cannot change", (int)range.size(), (int)value.size());

    for (int beat = 0;; beat++) {
        if (beat % 60 == 0) {  // about a second
            std::vector<Ref> v, w;
            for (int i = 0, n = api->num_objects(); i < n; i++) {
                uintptr_t o = api->object(i);
                if (!o || isCDO(o)) continue;
                if (api->is_a(classOf(o), cVeh)) v.push_back({i, o});
                else if (canTune && api->is_a(classOf(o), cScreen)) w.push_back({i, o});
            }
            if (canTune && !cButton && !w.empty()) {  // the blueprints are only there once a garage has opened
                cSetting = api->find_object("WidgetBlueprintGeneratedClass", "W_Setting_C", "/Game/UI/Component/W_Setting");
                cButton = api->find_object("WidgetBlueprintGeneratedClass", "W_Button_C", "/Game/UI/Button/W_Button");
                if (!cSetting) cButton = 0;
            }
            if (!cButton) w.clear();
            uintptr_t ex = smoke ? smoke : findEffect(oFxSmoke);
            std::lock_guard<std::mutex> g(lock);
            vehicles.swap(v);
            screens.swap(w);
            for (auto c = cars.begin(); c != cars.end();) {
                bool found = false;
                for (auto& r : vehicles) found = found || r.ptr == c->first;
                c = found ? std::next(c) : cars.erase(c);
            }
            if (ex && !smoke) { smoke = ex; MT_LOG("smoke %s", objName(smoke).c_str()); }
        }
        if (beat % 600 == 0) InterlockedExchange(&queued, 0);  // a queue dropped at a level change
        if (api->game_thread_ready() && !InterlockedCompareExchange(&queued, 1, 0)) api->run_on_game_thread(tick, nullptr);
        Sleep(16);
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
