// Blade of Shadows -- Milestone 1
// Native C++17 / SDL2 port of the web build (ts-spaces/hollow-adventure),
// Stage 1 "Echo Cave". Physics constants, stage geometry, enemy behaviour
// and hero sprite metrics are ported from the web version's source.
//
// Logical view is 960x540 (the web canvas size); the scene renders into
// a 960x540 target that is upscaled to the 1280x720 window, so every
// gameplay number below is in the same units as the web build.

#include <SDL.h>
#include <SDL_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------- constants
static const int    VIEW_W   = 960;
static const int    VIEW_H   = 540;
static const int    WIN_W    = 1280;
static const int    WIN_H    = 720;
static const double STEP     = 1.0 / 60.0;

static const float WORLD_W     = 2700.f;   // STAGES[0].world
static const float EDGE_MARGIN = 80.f;     // physics clamp at world edges
static const float GRAVITY     = 1180.f;   // px/s^2
static const float MAX_RUN     = 265.f;    // max horizontal speed
static const float ACCEL_GND   = 1700.f;
static const float ACCEL_AIR   = 1350.f;
static const float JUMP_VEL    = -470.f;   // ground jump
static const float DJUMP_VEL   = -438.f;   // double jump
static const float COYOTE_TIME = 0.1f;
static const float JBUF_TIME   = 0.16f;
// Dash (Nora / wanderer outfit values from the web build)
static const float DASH_TIME   = 0.21f;
static const float DASH_CD     = 0.66f;
static const float DASH_SPEED  = 720.f;
static const float DASH_IFR    = 0.2f;
// Blade of Shadows (starting weapon)
static const float BLADE_REACH = 54.f;
static const int   BLADE_DMG   = 1;
static const float BLADE_COOL  = 0.22f;
// Hurt / i-frames
static const float HURT_IFR    = 1.25f;
static const float HURT_VY     = -285.f;
static const float HURT_VX     = 210.f;
// Light meter / Light Pulse (L)
static const float LIGHT_HIT   = 9.f;
static const float LIGHT_KILL  = 18.f;
static const int   PULSE_DMG   = 3;
static const float PULSE_RANGE = 230.f;
// Hero sprite metrics from the web build: ANIM_S and HERO_K, per-pose
// anchor fraction f and nominal source height h (HERO_POSES).
static const float ANIM_S = 0.48f;
static const float HERO_K = 0.572f;

static const SDL_Color ACCENT   = {0x65, 0xdf, 0xca, 255};  // stage accent
static const SDL_Color SKY_TOP  = {0x06, 0x14, 0x1a, 255};
static const SDL_Color SKY_MID  = {0x0b, 0x26, 0x26, 255};
static const SDL_Color SKY_BOT  = {0x08, 0x16, 0x19, 255};

// ------------------------------------------------------------------ helpers
struct FRect2 { float x, y, w, h; };
static bool rectsOverlap(const FRect2& a, const FRect2& b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
// deterministic hash for background silhouettes
static float hashf(int i) {
    float s = std::sin(i * 127.1f) * 43758.5453f;
    return s - std::floor(s);
}

static SDL_Renderer* gRen = nullptr;

static void setCol(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 255) {
    SDL_SetRenderDrawColor(gRen, r, g, b, a);
}
static void setCol(const SDL_Color& c, Uint8 a = 255) { setCol(c.r, c.g, c.b, a); }
static void fillRect(float x, float y, float w, float h) {
    SDL_FRect r{x, y, w, h};
    SDL_RenderFillRectF(gRen, &r);
}
// Scanline polygon fill (respects current draw colour + blend mode).
static void fillPoly(const std::vector<SDL_FPoint>& pts) {
    if (pts.size() < 3) return;
    float minY = pts[0].y, maxY = pts[0].y;
    for (auto& p : pts) { minY = std::min(minY, p.y); maxY = std::max(maxY, p.y); }
    int y0 = (int)std::floor(minY), y1 = (int)std::ceil(maxY);
    std::vector<float> xs;
    for (int y = y0; y < y1; ++y) {
        float cy = y + 0.5f;
        xs.clear();
        for (size_t i = 0; i < pts.size(); ++i) {
            SDL_FPoint a = pts[i], b = pts[(i + 1) % pts.size()];
            if ((a.y <= cy && b.y > cy) || (b.y <= cy && a.y > cy)) {
                float t = (cy - a.y) / (b.y - a.y);
                xs.push_back(a.x + t * (b.x - a.x));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            SDL_FRect r{xs[i], (float)y, xs[i + 1] - xs[i], 1.f};
            SDL_RenderFillRectF(gRen, &r);
        }
    }
}
static void fillEllipse(float cx, float cy, float rx, float ry) {
    std::vector<SDL_FPoint> pts;
    const int N = 28;
    for (int i = 0; i < N; ++i) {
        float a = (float)i / N * 6.2831853f;
        pts.push_back({cx + std::cos(a) * rx, cy + std::sin(a) * ry});
    }
    fillPoly(pts);
}
static void fillCircle(float cx, float cy, float r) { fillEllipse(cx, cy, r, r); }
static void thickLine(float x1, float y1, float x2, float y2, float w) {
    float dx = x2 - x1, dy = y2 - y1;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f) { fillCircle(x1, y1, w * 0.5f); return; }
    float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
    fillPoly({{x1 + nx, y1 + ny}, {x2 + nx, y2 + ny}, {x2 - nx, y2 - ny}, {x1 - nx, y1 - ny}});
}
static void arcStroke(float cx, float cy, float r, float a0, float a1, float w) {
    const int N = 20;
    for (int i = 0; i < N; ++i) {
        float t0 = a0 + (a1 - a0) * i / N, t1 = a0 + (a1 - a0) * (i + 1) / N;
        thickLine(cx + std::cos(t0) * r, cy + std::sin(t0) * r,
                  cx + std::cos(t1) * r, cy + std::sin(t1) * r, w);
    }
}
static void ringStroke(float cx, float cy, float r, float w) { arcStroke(cx, cy, r, 0.f, 6.2831853f, w); }
// vertical gradient with 3 stops (top / mid at 48% / bottom), like the web bg
static void gradRect(float x, float y, float w, float h,
                     const SDL_Color& top, const SDL_Color& mid, const SDL_Color& bot) {
    const float strip = 2.f;
    for (float yy = y; yy < y + h; yy += strip) {
        float t = (yy - y) / h;
        SDL_Color c;
        if (t < 0.48f) {
            float k = t / 0.48f;
            c = {(Uint8)lerpf(top.r, mid.r, k), (Uint8)lerpf(top.g, mid.g, k),
                 (Uint8)lerpf(top.b, mid.b, k), 255};
        } else {
            float k = (t - 0.48f) / 0.52f;
            c = {(Uint8)lerpf(mid.r, bot.r, k), (Uint8)lerpf(mid.g, bot.g, k),
                 (Uint8)lerpf(mid.b, bot.b, k), 255};
        }
        setCol(c);
        fillRect(x, yy, w, std::min(strip, y + h - yy));
    }
}

// -------------------------------------------------------------- bitmap font
// Minimal 5x7 font (A-Z, 0-9 and basic punctuation), drawn as blocks so the
// game needs no font files -- everything stays self-contained.
struct Glyph { char c; const char* r[7]; };
static const Glyph GLYPHS[] = {
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".####", "#....", "#....", "#....", "#....", "#....", ".####"}},
    {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".####", "#....", "#....", "#.###", "#...#", "#...#", ".###."}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"}},
    {'X', {"#...#", ".#.#.", "..#..", "..#..", "..#..", ".#.#.", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'0', {".###.", "#..##", "#.#.#", "##..#", "#...#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "..##.", ".#...", "#....", "#####"}},
    {'3', {"####.", "....#", "....#", ".###.", "....#", "....#", "####."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", "..#..", "..#..", "..#.."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."}},
    {' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {',', {".....", ".....", ".....", ".....", ".##..", ".#...", "#...."}},
    {'\'', {"..#..", "..#..", ".#...", ".....", ".....", ".....", "....."}},
    {'!', {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."}},
    {'?', {".###.", "#...#", "....#", "..##.", "..#..", ".....", "..#.."}},
    {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {'-', {".....", ".....", ".....", ".####", ".....", ".....", "....."}},
    {'/', {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}},
    {'%', {"##..#", "##.#.", "....#", "...#.", "..#..", ".#...", "#.###"}},
    {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {'(', {"...##", "..#..", ".#...", ".#...", ".#...", "..#..", "...##"}},
    {')', {"##...", "..#..", "...#.", "...#.", "...#.", "..#..", "##..."}},
};
static const Glyph* findGlyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (const auto& g : GLYPHS) if (g.c == c) return &g;
    return nullptr;
}
static float textWidth(const std::string& s, float scale) {
    return (float)s.size() * 6.f * scale;
}
static void drawText(float x, float y, float scale, const SDL_Color& col, const std::string& s) {
    setCol(col);
    float cx = x;
    for (char ch : s) {
        const Glyph* g = findGlyph(ch);
        if (g) {
            for (int row = 0; row < 7; ++row)
                for (int colI = 0; colI < 5; ++colI)
                    if (g->r[row][colI] == '#')
                        fillRect(cx + colI * scale, y + row * scale, scale, scale);
        }
        cx += 6.f * scale;
    }
}
static void drawTextCentered(float cx, float y, float scale, const SDL_Color& col, const std::string& s) {
    drawText(cx - textWidth(s, scale) * 0.5f, y, scale, col, s);
}

// ------------------------------------------------------------ game entities
struct Platform { float x, y, w, h; };
struct SpikeTrap { float x, y, w, arm = 0, warn = 0; };
struct Shard { float x, y; bool taken = false; };
struct Checkpoint { float x, y; const char* name; bool active = false; };
struct Enemy {
    std::string type;
    float x, y, baseY, w, h, mn, mx, v;
    int hp, maxHp;
    float hit = 0, phase = 0, shot = 1.2f;
    bool dead = false;
};
struct Particle { float x, y, vx, vy, life, maxLife, size; SDL_Color c; };
struct RingFx { float x, y, r, life, maxLife; SDL_Color c; float width; };
struct TrailGhost { int pose; float x, y; int dir; float life; };

enum class Pose { Idle, Run, Run2, Jump, Attack };
struct PoseTex {
    SDL_Texture* tex = nullptr;
    SDL_Texture* white = nullptr;   // white silhouette for the hurt flash
    int w = 0, h = 0;
    float f = 0.5f;                  // horizontal anchor fraction (HERO_POSES)
    float srcH = 352.f;              // nominal source height (HERO_POSES)
};

struct Player {
    FRect2 box{240, 388, 42, 50};    // w/h from the web build
    float vx = 0, vy = 0;
    int dir = 1;
    bool on = false;
    int hp = 5, maxHp = 5;
    float ifr = 0, attack = 0, attackCd = 0, dash = 0, dashCd = 0;
    int jumps = 0, got = 0;
    float light = 0;
    float land = 0, jumpStart = 0, coyote = 0, jbuf = 0, hurtT = 0;
    float water = 0, wallLock = 0, trailT = 0, dustT = 0;
    int wallDir = 0;
};

enum class GameState { Title, Intro, Playing, Paused, Dead, Won };

// ------------------------------------------------- Stage 1 "Echo Cave" data
// Copied 1:1 from STAGES[0] in the web build.
static const Platform STAGE_PLATFORMS[] = {
    {0, 470, 500, 70}, {580, 470, 420, 70}, {1080, 470, 420, 70},
    {1590, 470, 410, 70}, {2080, 470, 620, 70},
    {330, 365, 170, 22}, {690, 320, 170, 22}, {930, 255, 130, 20},
    {1180, 350, 180, 20}, {1430, 285, 145, 20}, {1700, 350, 185, 20},
    {1980, 290, 165, 20}, {2260, 345, 190, 20},
};
static const float STAGE_SPIKES[][3] = {  // x, y, w
    {500, 450, 80}, {1000, 450, 80}, {1500, 450, 90}, {2000, 450, 80},
};
static const float STAGE_SHARDS[][2] = { {440, 318}, {1495, 238}, {2355, 298} };
static const float STAGE_CHECKPOINT[] = { 1190, 428 };  // "Echo Altar"

struct EnemySpec { const char* type; float x, y, mn, mx; };
static const EnemySpec STAGE_ENEMIES[] = {
    {"crawler", 680, 435, 580, 990},  {"bat", 1030, 260, 930, 1230},
    {"crawler", 1220, 315, 1180, 1360}, {"sonic", 1810, 425, 1680, 2030},
    {"bat", 2040, 235, 1930, 2240},   {"crawler", 2320, 435, 2180, 2520},
};
// ------------------------------------------------------------------- state
static Player player;
static std::vector<Platform> platforms;
static std::vector<SpikeTrap> spikes;
static std::vector<Shard> shards;
static std::vector<Enemy> enemies;
static Checkpoint checkpoint;
static FRect2 gateRect;
static bool gateOpen = false;
static std::vector<Particle> particles;
static std::vector<RingFx> rings;
static std::vector<TrailGhost> trails;

static GameState state = GameState::Title;
static double tick = 0.0;
static float cam = 0, shake = 0, flash = 0, hitStop = 0;
static float respawnX = 240;
static float deathTimer = -1;
static float hintTimer = 0;
static int kills = 0;
static bool keyLeft = false, keyRight = false, keyUp = false, keyDown = false;

static PoseTex poseTex[(size_t)Pose::Attack + 1];
static SDL_Texture* slashTex = nullptr;
static bool gHeadless = false;  // screenshot mode: no Present, pixels read back
static SDL_Texture* gTarget = nullptr;  // 960x540 scene target, upscaled to window

static float heroSpawnX() {
    return std::max(EDGE_MARGIN + 16.f, std::min(240.f, VIEW_W * 0.32f));
}

static void burst(float x, float y, const SDL_Color& c, int n) {
    for (int i = 0; i < n; ++i) {
        float vx = (hashf((int)(tick * 1000) + i * 37) - 0.5f) * 210.f;
        float vy = (hashf((int)(tick * 777) + i * 91) - 0.75f) * 190.f;
        float life = 0.35f + hashf(i * 13 + (int)(tick * 500)) * 0.5f;
        particles.push_back({x, y, vx, vy, life, life, 2.f + hashf(i * 7) * 4.f, c});
    }
}
static void addRing(float x, float y, float r, float life, const SDL_Color& c, float w = 3.f) {
    rings.push_back({x, y, r, life, life, c, w});
}

static Enemy makeEnemy(const EnemySpec& s, int idx) {
    Enemy e;
    e.type = s.type;
    e.x = s.x; e.y = s.y; e.baseY = s.y;
    e.mn = s.mn; e.mx = s.mx;
    e.hp = e.maxHp = (e.type == "bat" || e.type == "crawler") ? 2 : 3;
    e.w = e.type == "bat" ? 40.f : 44.f;
    e.h = e.type == "bat" ? 30.f : 40.f;
    e.v = 44.f;  // stage-1 base patrol speed in the web build
    e.phase = hashf(idx * 31 + 7) * 6.f;
    e.shot = 1.2f + hashf(idx * 17 + 3);
    return e;
}

static void loadStage() {
    platforms.assign(std::begin(STAGE_PLATFORMS), std::end(STAGE_PLATFORMS));
    spikes.clear();
    for (auto& s : STAGE_SPIKES) spikes.push_back({s[0], s[1], s[2], 0, 0});
    shards.clear();
    for (auto& s : STAGE_SHARDS) shards.push_back({s[0], s[1], false});
    enemies.clear();
    int i = 0;
    for (auto& s : STAGE_ENEMIES) enemies.push_back(makeEnemy(s, i++));
    checkpoint = {STAGE_CHECKPOINT[0], STAGE_CHECKPOINT[1], "Echo Altar", false};
    gateRect = {WORLD_W - 86, 365, 62, 105};
    gateOpen = false;
    particles.clear(); rings.clear(); trails.clear();
    respawnX = heroSpawnX();
    cam = 0; shake = 0; flash = 0; hitStop = 0; kills = 0;
    deathTimer = -1; hintTimer = 0;
    player = Player{};
    player.box.x = respawnX;
    player.box.y = 388;
}

static void updateGate() {
    bool was = gateOpen;
    gateOpen = (player.got == 3);
    if (gateOpen && !was) addRing(gateRect.x + 31, gateRect.y + 53, 12, 0.8f, ACCENT);
}

// ------------------------------------------------------------------ combat
static void hurt(float fromX);
static void damageEnemy(Enemy& e, int amount, int dir, bool noCharge) {
    if (e.hit > 0 || e.dead) return;
    e.hp -= amount;
    e.hit = 0.24f;
    e.v = dir * 130.f;
    shake = 4;
    hitStop = 0.045f;
    if (!noCharge)
        player.light = std::min(100.f, player.light + (e.hp <= 0 ? LIGHT_KILL : LIGHT_HIT));
    burst(e.x + e.w / 2, e.y + e.h / 2, ACCENT, 16);
    if (e.hp <= 0) {
        e.dead = true;
        kills++;
        burst(e.x + e.w / 2, e.y + e.h / 2, ACCENT, 34);
    }
}
static void hurt(float fromX) {
    if (player.ifr > 0 || state != GameState::Playing || player.hp <= 0) return;
    player.hp--;
    player.ifr = HURT_IFR;
    player.hurtT = 0.3f;
    player.vy = HURT_VY;
    player.vx = (fromX < player.box.x) ? HURT_VX : -HURT_VX;
    shake = 11;
    flash = 0.15f;
    burst(player.box.x + 15, player.box.y + 20, {241, 154, 104, 255}, 22);
    if (player.hp <= 0) deathTimer = 0.95f;
}

// ------------------------------------------------------------------- input
static void tryJump() {
    if (state != GameState::Playing || player.hp <= 0) return;
    player.jbuf = 0;
    if (player.on || player.coyote > 0) {
        player.vy = JUMP_VEL;
        player.on = false;
        player.jumps = 1;
        player.coyote = 0;
        player.jumpStart = 0.2f;
        addRing(player.box.x + 17, player.box.y + 49, 14, 0.3f, {188, 223, 228, 255});
        burst(player.box.x + 19, player.box.y + 44, {134, 176, 182, 255}, 9);
    } else if (player.jumps < 2) {
        player.jumps++;
        player.vy = DJUMP_VEL;
        player.water = 0;
        player.jumpStart = 0.2f;
        addRing(player.box.x + 17, player.box.y + 36, 20, 0.36f, {186, 227, 255, 255});
        burst(player.box.x + 19, player.box.y + 46, {106, 163, 232, 255}, 18);
    }
}
static void pressJump() {
    if (state != GameState::Playing || player.hp <= 0) return;
    player.jbuf = JBUF_TIME;
    tryJump();
}
static void releaseJump() {
    if (state == GameState::Playing && player.vy < -150.f && player.hp > 0)
        player.vy *= 0.45f;
}
static void pressAttack() {
    if (state != GameState::Playing || player.hp <= 0) return;
    if (player.attackCd > 0) return;
    player.attack = BLADE_COOL;
    player.attackCd = BLADE_COOL;
    burst(player.box.x + 18 + player.dir * 28, player.box.y + 18,
          {215, 255, 246, 255}, 5);
}
static void pressDash() {
    if (state != GameState::Playing || player.hp <= 0) return;
    if (player.dashCd > 0) return;
    player.dash = DASH_TIME;             // Nora: longer dash (wanderer trait)
    player.dashCd = DASH_CD;
    player.vx = player.dir * DASH_SPEED;
    player.vy = 0;
    player.ifr = std::max(player.ifr, DASH_IFR);
    burst(player.box.x + 18, player.box.y + 25, {125, 216, 255, 255}, 18);
}
static void pressPulse() {
    if (state != GameState::Playing || player.hp <= 0) return;
    if (player.light < 100.f) { hintTimer = 1.6f; return; }
    player.light = 0;
    player.ifr = std::max(player.ifr, 0.5f);
    shake = 13;
    flash = 0.2f;
    addRing(player.box.x + 15, player.box.y + 22, 18, 0.72f, {255, 240, 166, 255});
    burst(player.box.x + 15, player.box.y + 22, {255, 240, 166, 255}, 52);
    for (auto& e : enemies)
        if (!e.dead && std::fabs(e.x + e.w / 2 - player.box.x) < PULSE_RANGE)
            damageEnemy(e, PULSE_DMG, e.x > player.box.x ? 1 : -1, true);
}

// ------------------------------------------------------------------ update
static void collidePlatforms(float dt) {
    float prevY = player.box.y;
    player.wallDir = 0;
    player.box.x += player.vx * dt;
    if (player.box.x <= EDGE_MARGIN) {
        player.box.x = EDGE_MARGIN; player.wallDir = -1;
        player.vx = std::max(0.f, player.vx);
    }
    if (player.box.x >= WORLD_W - player.box.w - EDGE_MARGIN) {
        player.box.x = WORLD_W - player.box.w - EDGE_MARGIN; player.wallDir = 1;
        player.vx = std::min(0.f, player.vx);
    }
    player.box.y += player.vy * dt;
    player.on = false;
    for (auto& p : platforms) {
        FRect2 pr{p.x, p.y, p.w, p.h};
        if (!rectsOverlap(player.box, pr)) continue;
        if (prevY + player.box.h <= p.y + 6 && player.vy >= 0) {
            player.box.y = p.y - player.box.h; player.vy = 0;
            player.on = true; player.jumps = 0;
        } else if (prevY >= p.y + p.h - 5 && player.vy < 0) {
            player.box.y = p.y + p.h; player.vy = 35;
        } else if (player.box.x + player.box.w / 2 < p.x + p.w / 2) {
            player.box.x = p.x - player.box.w; player.wallDir = 1;
            player.vx = std::min(0.f, player.vx);
        } else {
            player.box.x = p.x + p.w; player.wallDir = -1;
            player.vx = std::max(0.f, player.vx);
        }
    }
}

static void updateEnemies(float dt) {
    for (auto& e : enemies) {
        if (e.dead) continue;
        e.hit = std::max(0.f, e.hit - dt);
        e.phase += dt;
        e.shot -= dt;
        if (e.type == "bat") {
            e.x += e.v * dt;
            e.y = e.baseY + std::sin(e.phase * 3.2f) * 30.f;
            if (std::fabs(player.box.x - e.x) < 250.f)
                e.y += (player.box.y - e.y) * dt * 1.25f;
        } else if (e.type == "sonic") {
            e.x += e.v * 0.35f * dt;  // patrol only in M1 (sound wave is roadmap)
        } else {
            e.x += e.v * dt;         // crawler patrol
        }
        if (e.x < e.mn || e.x + e.w > e.mx) {
            e.v *= -1;
            e.x = clampf(e.x, e.mn, e.mx - e.w);
        }
        if (player.attack > 0) {
            FRect2 ar = player.dir > 0
                ? FRect2{player.box.x + 20, player.box.y - 3, BLADE_REACH, 51}
                : FRect2{player.box.x - BLADE_REACH + 10, player.box.y - 3, BLADE_REACH, 51};
            FRect2 er{e.x, e.y, e.w, e.h};
            if (rectsOverlap(ar, er)) damageEnemy(e, BLADE_DMG, player.dir, false);
        }
        if (rectsOverlap(player.box, {e.x, e.y, e.w, e.h})) hurt(e.x);
    }
}

static void stepGame(float dt) {
    tick += dt;
    if (hitStop > 0) { hitStop -= dt; return; }

    bool wasOn = player.on;
    // timers
    player.ifr = std::max(0.f, player.ifr - dt);
    player.attack = std::max(0.f, player.attack - dt);
    player.attackCd = std::max(0.f, player.attackCd - dt);
    player.dash = std::max(0.f, player.dash - dt);
    player.dashCd = std::max(0.f, player.dashCd - dt);
    player.land = std::max(0.f, player.land - dt);
    player.jumpStart = std::max(0.f, player.jumpStart - dt);
    player.wallLock = std::max(0.f, player.wallLock - dt);
    player.coyote = std::max(0.f, player.coyote - dt);
    player.jbuf = std::max(0.f, player.jbuf - dt);
    player.hurtT = std::max(0.f, player.hurtT - dt);
    player.water = std::max(0.f, player.water - dt);
    hintTimer = std::max(0.f, hintTimer - dt);
    if (deathTimer > 0) {
        deathTimer -= dt;
        if (deathTimer <= 0) { state = GameState::Dead; deathTimer = -1; }
    }

    // horizontal movement / dash
    if (player.hp > 0 && player.dash <= 0) {
        float ax = 0;
        if (keyLeft) ax -= 1;
        if (keyRight) ax += 1;
        if (ax != 0) {
            player.dir = (int)ax;
            player.vx += ax * (player.on ? ACCEL_GND : ACCEL_AIR) * dt;
        } else {
            player.vx *= std::pow(player.on ? 0.0008f : 0.1f, dt);
        }
        player.vx = clampf(player.vx, -MAX_RUN, MAX_RUN);
    } else if (player.hp > 0) {  // dashing: ghost trail + dust
        player.trailT += dt;
        if (player.trailT > 0.028f) {
            player.trailT = 0;
            trails.push_back({(int)Pose::Run, player.box.x, player.box.y, player.dir, 0.26f});
        }
        player.dustT += dt;
        if (player.dustT > 0.03f) {
            player.dustT = 0;
            burst(player.box.x + 19 - player.dir * 15, player.box.y + 25,
                  {125, 216, 255, 255}, 3);
        }
    }
    if (player.hp <= 0)
        player.vx *= std::pow(player.on ? 0.004f : 0.05f, dt);

    player.vy += (player.water > 0 ? 300.f : GRAVITY) * dt;
    collidePlatforms(dt);

    if (!wasOn && player.on) {
        player.land = 0.32f;
        player.dashCd = 0;  // landing refreshes the dash, as in the web build
        burst(player.box.x + 15, player.box.y + player.box.h, {158, 181, 175, 255}, 12);
        addRing(player.box.x + 17, player.box.y + 44, 6, 0.22f, {158, 181, 175, 255});
    }
    if (player.on && player.hp > 0) player.coyote = COYOTE_TIME;
    if (player.jbuf > 0 && (player.on || player.coyote > 0) && player.hp > 0) tryJump();

    // pit / water band, then the fall reset (web build behaviour)
    bool inWater = player.box.y > 515 && player.box.y < 590;
    if (inWater) {
        player.water = 0.34f;
        player.vx *= 0.9f;
        if (player.vy > 80) player.vy = 80;
    }
    if (player.box.y > 590) {
        player.box.x = respawnX;
        player.box.y = 350;
        player.vy = 0;
        hurt(player.box.x - 40);
    }

    // spike traps: arm up while the player runs nearby, as in the web build
    for (auto& t : spikes) {
        bool near = std::fabs(player.box.x + player.box.w / 2 - (t.x + t.w / 2)) < 150.f &&
                    std::fabs(player.vx) > 35.f;
        t.warn = near ? std::min(1.f, t.warn + dt * 3.f) : std::max(0.f, t.warn - dt * 2.f);
        t.arm = near ? std::min(1.f, t.arm + dt * 2.1f) : std::max(0.f, t.arm - dt * 1.25f);
        if (t.arm > 0.62f &&
            rectsOverlap(player.box, {t.x + 5, t.y + (1 - t.arm) * 17, t.w - 10, 20}))
            hurt(t.x);
    }
    // shards
    for (auto& sh : shards) {
        if (!sh.taken &&
            rectsOverlap(player.box, {sh.x - 14, sh.y - 19, 28, 38})) {
            sh.taken = true;
            player.got++;
            burst(sh.x, sh.y, ACCENT, 28);
            addRing(sh.x, sh.y, 8, 0.4f, ACCENT);
            updateGate();
        }
    }
    // checkpoint altar
    if (!checkpoint.active &&
        rectsOverlap(player.box, {checkpoint.x - 20, checkpoint.y - 25, 40, 55})) {
        checkpoint.active = true;
        respawnX = checkpoint.x - 28;
        player.hp = std::min(player.maxHp, player.hp + 1);
        player.light = std::min(100.f, player.light + 20.f);
        addRing(checkpoint.x, checkpoint.y, 10, 0.8f, ACCENT);
        burst(checkpoint.x, checkpoint.y, ACCENT, 38);
    }

    updateEnemies(dt);
    updateGate();

    // respawn point drifts forward with progress, as in the web build
    if (player.box.x > WORLD_W * 0.36f && respawnX < WORLD_W * 0.3f)
        respawnX = player.box.x - 40;
    if (player.box.x > WORLD_W * 0.7f && respawnX < WORLD_W * 0.65f)
        respawnX = player.box.x - 40;

    // exit gate
    if (rectsOverlap(player.box, gateRect)) {
        if (gateOpen) state = GameState::Won;
        else hintTimer = std::max(hintTimer, 0.5f);
    }

    // fx
    for (size_t i = particles.size(); i-- > 0;) {
        auto& q = particles[i];
        q.life -= dt; q.x += q.vx * dt; q.y += q.vy * dt; q.vy += 230.f * dt;
        if (q.life <= 0) particles.erase(particles.begin() + (long)i);
    }
    for (size_t i = rings.size(); i-- > 0;) {
        rings[i].life -= dt; rings[i].r += 180.f * dt;
        if (rings[i].life <= 0) rings.erase(rings.begin() + (long)i);
    }
    for (size_t i = trails.size(); i-- > 0;) {
        trails[i].life -= dt;
        if (trails[i].life <= 0) trails.erase(trails.begin() + (long)i);
    }

    float camTarget = clampf(player.box.x - VIEW_W * 0.38f, 0.f, WORLD_W - VIEW_W);
    cam += (camTarget - cam) * std::min(1.f, dt * 5.f);
    shake *= std::pow(0.012f, dt);
    flash = std::max(0.f, flash - dt);
}

// ----------------------------------------------------------------- textures
static SDL_Texture* loadTex(const std::string& path, int* outW = nullptr, int* outH = nullptr) {
    SDL_Texture* t = IMG_LoadTexture(gRen, path.c_str());
    if (!t) {
        std::fprintf(stderr, "Failed to load %s: %s\n", path.c_str(), IMG_GetError());
        return nullptr;
    }
    SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
    if (outW) SDL_QueryTexture(t, nullptr, nullptr, outW, outH);
    return t;
}
static SDL_Texture* makeWhiteVariant(const std::string& path) {
    SDL_Surface* s = IMG_Load(path.c_str());
    if (!s) return nullptr;
    SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(s);
    if (!c) return nullptr;
    if (SDL_LockSurface(c) == 0) {
        Uint32* px = (Uint32*)c->pixels;
        for (int i = 0; i < c->w * c->h; ++i) {
            Uint8 a = (px[i] >> 24) & 0xFF;
            px[i] = ((Uint32)a << 24) | 0x00FFFFFFu;
        }
        SDL_UnlockSurface(c);
    }
    SDL_Texture* t = SDL_CreateTextureFromSurface(gRen, c);
    SDL_FreeSurface(c);
    if (t) SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
    return t;
}
// Screen-space scrim, baked once: transparent above y=240, darkening to
// rgba(0,0,0,.34) at the floor -- the web build's readability gradient,
// reproduced 1:1 so the gameplay plane stays readable over the painting.
static SDL_Texture* makeScrimTex() {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, 1, VIEW_H, 32,
                                                    SDL_PIXELFORMAT_ARGB8888);
    if (!s) return nullptr;
    if (SDL_LockSurface(s) == 0) {
        Uint32* px = (Uint32*)s->pixels;
        for (int y = 0; y < VIEW_H; ++y) {
            float t = clampf((y - 240) / (float)(VIEW_H - 240), 0.f, 1.f);
            px[y] = (Uint32)(87.f * t) << 24;  // black, alpha 0 -> 87 (.34)
        }
        SDL_UnlockSurface(s);
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(gRen, s);
    SDL_FreeSurface(s);
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}
// ------------------------------------------------------- v3 enemy sprites
// Approved v3 pixel art for the Stage 1 roster, drawn at presence scale
// (x1.7 Crawler/Bat, x1.4 Sonic relative to the old code-drawn sizes).
// Each sprite's transparent border is trimmed at load; the trimmed art
// is fit inside its presence box keeping its own proportions. Ground
// enemies get the soft shadow ellipse; every enemy gets a faint
// stage-accent halo (Echo Cave teal). If a file is missing the renderer
// falls back to the original code-drawn shapes, so a partial asset set
// never breaks the build.
struct SprTex {
    SDL_Texture* tex = nullptr;
    SDL_Texture* white = nullptr;  // white silhouette for the hit flash
    int artW = 0, artH = 0;
};
static SprTex sprCrawlerIdle, sprCrawlerWalk1, sprCrawlerWalk2;
static SprTex sprBatUp, sprBatDown, sprSonicIdle, sprSonicFire;
static SDL_Texture* spikeTexV3[3] = {nullptr, nullptr, nullptr};  // retracted, rising, out
static SDL_Texture* shadowTex = nullptr, *haloTex = nullptr;
// Painted stage dressing (v5 map look): the Echo Cave panorama and the
// rock tile ported from the web build, plus a runtime-baked scrim.
// All non-fatal: drawBackground/drawPlatform fall back to the original
// code-drawn rendering when a texture is missing.
static SDL_Texture* bgFarTex = nullptr, *terrainTex = nullptr, *scrimTex = nullptr;

static bool loadSpr(SprTex& out, const std::string& path) {
    SDL_Surface* s = IMG_Load(path.c_str());
    if (!s) {
        std::fprintf(stderr, "sprite missing (code-drawn fallback): %s\n", path.c_str());
        return false;
    }
    SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(s);
    if (!c) return false;
    int minX = c->w, minY = c->h, maxX = -1, maxY = -1;
    if (SDL_LockSurface(c) == 0) {
        const Uint32* px = (const Uint32*)c->pixels;
        for (int yy = 0; yy < c->h; ++yy)
            for (int xx = 0; xx < c->w; ++xx)
                if (((px[yy * c->w + xx] >> 24) & 0xFF) > 8) {
                    if (xx < minX) minX = xx;
                    if (xx > maxX) maxX = xx;
                    if (yy < minY) minY = yy;
                    if (yy > maxY) maxY = yy;
                }
        SDL_UnlockSurface(c);
    }
    if (maxX < 0) { SDL_FreeSurface(c); return false; }
    SDL_Rect crop{minX, minY, maxX - minX + 1, maxY - minY + 1};
    SDL_Surface* t = SDL_CreateRGBSurfaceWithFormat(0, crop.w, crop.h, 32,
                                                    SDL_PIXELFORMAT_ARGB8888);
    if (!t) { SDL_FreeSurface(c); return false; }
    SDL_SetSurfaceBlendMode(c, SDL_BLENDMODE_NONE);
    SDL_BlitSurface(c, &crop, t, nullptr);
    SDL_FreeSurface(c);
    out.artW = crop.w;
    out.artH = crop.h;
    out.tex = SDL_CreateTextureFromSurface(gRen, t);
    if (SDL_LockSurface(t) == 0) {  // whiten in place for the flash variant
        Uint32* px = (Uint32*)t->pixels;
        for (int i = 0; i < t->w * t->h; ++i) {
            Uint8 a = (Uint8)((px[i] >> 24) & 0xFF);
            px[i] = ((Uint32)a << 24) | 0x00FFFFFFu;
        }
        SDL_UnlockSurface(t);
    }
    out.white = SDL_CreateTextureFromSurface(gRen, t);
    SDL_FreeSurface(t);
    if (out.tex) SDL_SetTextureScaleMode(out.tex, SDL_ScaleModeNearest);
    if (out.white) SDL_SetTextureScaleMode(out.white, SDL_ScaleModeNearest);
    return out.tex != nullptr;
}

static bool loadAssets(const std::string& base) {
    struct Spec { Pose p; const char* file; float f; float srcH; };
    const Spec specs[] = {
        {Pose::Idle,   "hero-idle.png",   0.519f, 384},
        {Pose::Run,    "hero-run.png",    0.516f, 352},
        {Pose::Run2,   "hero-run2.png",   0.516f, 352},
        {Pose::Jump,   "hero-jump.png",   0.560f, 392},
        {Pose::Attack, "hero-attack.png", 0.453f, 352},
    };
    for (const auto& s : specs) {
        PoseTex& pt = poseTex[(size_t)s.p];
        std::string path = base + s.file;
        pt.tex = loadTex(path, &pt.w, &pt.h);
        pt.white = makeWhiteVariant(path);
        pt.f = s.f;
        pt.srcH = s.srcH;
        if (!pt.tex) return false;
    }
    slashTex = loadTex(base + "slash-arc.png");
    // v3 enemy/trap art (Stage 1 set). Non-fatal: drawEnemy/drawSpikeTrap
    // fall back to the code-drawn rendering for any sprite that failed.
    loadSpr(sprCrawlerIdle, base + "enemy-crawler-idle-v3.png");
    loadSpr(sprCrawlerWalk1, base + "enemy-crawler-walk-1-v3.png");
    loadSpr(sprCrawlerWalk2, base + "enemy-crawler-walk-2-v3.png");
    loadSpr(sprBatUp, base + "enemy-bat-flap-up-v3.png");
    loadSpr(sprBatDown, base + "enemy-bat-flap-down-v3.png");
    loadSpr(sprSonicIdle, base + "enemy-sonic-idle-v3.png");
    loadSpr(sprSonicFire, base + "enemy-sonic-fire-v3.png");
    spikeTexV3[0] = loadTex(base + "trap-spikes-retracted-v3.png");
    spikeTexV3[1] = loadTex(base + "trap-spikes-rising-v3.png");
    spikeTexV3[2] = loadTex(base + "trap-spikes-out-v3.png");
    shadowTex = loadTex(base + "fx-shadow-ellipse.png");
    haloTex = loadTex(base + "fx-halo-teal.png");
    // Painted map look (non-fatal; see drawBackground / drawPlatform).
    bgFarTex = loadTex(base + "bg-cave-far.jpg");
    terrainTex = loadTex(base + "terrain-cave-tile.png");
    scrimTex = makeScrimTex();
    return slashTex != nullptr;
}

// ---------------------------------------------------------------- rendering
static float shX = 0, shY = 0;  // camera shake offsets for this frame

static void drawBackground() {
    gradRect(0, 0, VIEW_W, VIEW_H, SKY_TOP, SKY_MID, SKY_BOT);
    if (bgFarTex) {
        // Painted far layer: the web build's v5 Echo Cave panorama,
        // tiled at its native 1260x540 with parallax 0.30 -- the same
        // treatment the web build gives it -- plus the screen-space
        // scrim over the top so the gameplay plane stays readable.
        const float TW = 1260.f;
        float off = std::fmod(cam * 0.30f, TW);
        if (off < 0) off += TW;
        for (float bx = -off; bx < VIEW_W; bx += TW) {
            SDL_FRect dst{bx, 0, TW, (float)VIEW_H};
            SDL_RenderCopyF(gRen, bgFarTex, nullptr, &dst);
        }
        if (scrimTex) {
            SDL_FRect full{0, 0, (float)VIEW_W, (float)VIEW_H};
            SDL_RenderCopyF(gRen, scrimTex, nullptr, &full);
        }
    } else {
        // fallback far silhouette: ruined cave-city spires, parallax 0.08
        setCol(15, 42, 47);
        for (int i = -1; i < 14; ++i) {
            float bx = i * 110 - std::fmod(cam * 0.08f, 110.f);
            float hh = 60 + hashf(i * 3 + 40) * 120;
            float bw = 44 + hashf(i * 7 + 21) * 40;
            fillRect(bx, VIEW_H - 190 - hh, bw, hh + 190);
            fillRect(bx + 10, VIEW_H - 200 - hh, bw - 20, 12);  // broken crown
        }
    }
    // mid layer: stalactites + rock columns, parallax 0.35
    setCol(10, 31, 34);
    for (int i = -1; i < 16; ++i) {
        float bx = i * 86 - std::fmod(cam * 0.35f, 86.f);
        float len = 50 + hashf(i * 7 + 11) * 120;
        fillPoly({{bx, 0}, {bx + 34, 0}, {bx + 17, len}});
        float gl = 26 + hashf(i * 13 + 5) * 60;
        fillPoly({{bx + 8, VIEW_H}, {bx + 44, VIEW_H}, {bx + 26, VIEW_H - gl}});
    }
    // near layer: glowing crystal clusters, parallax 0.55
    for (int i = -1; i < 10; ++i) {
        float bx = i * 190 - std::fmod(cam * 0.55f, 190.f) + 60;
        float ch = 34 + hashf(i * 19 + 2) * 46;
        float base = VIEW_H - 60;
        setCol(ACCENT, 26);
        fillEllipse(bx + 20, base - ch * 0.4f, 44, ch * 0.8f);  // glow
        setCol(53, 140, 138, 150);
        fillPoly({{bx, base}, {bx + 14, base - ch}, {bx + 28, base}});
        setCol(101, 223, 202, 170);
        fillPoly({{bx + 20, base}, {bx + 30, base - ch * 0.62f}, {bx + 40, base}});
    }
    // hanging lanterns in world space (warm points of light)
    static const float lanternX[] = {430, 990, 1560, 2150, 2560};
    for (float lx0 : lanternX) {
        float lx = lx0 - cam;
        if (lx < -60 || lx > VIEW_W + 60) continue;
        // The spawn lantern (the first one) hangs higher, with a smaller
        // glow: at the opening camera positions its full-size disc sat
        // right on the shard counter and the light meter and muddied
        // both. The other lanterns are unchanged.
        bool spawnLamp = (lx0 == lanternX[0]);
        float chain = spawnLamp ? 30.f : 64 + hashf((int)lx0) * 46;
        float glowR = spawnLamp ? 15.f : 30.f;
        setCol(70, 84, 80);
        fillRect(lx - 1, 0, 2, chain);
        setCol(255, 179, 110, 30);
        fillEllipse(lx, chain + 12, glowR, glowR);  // glow
        setCol(46, 40, 30);
        fillRect(lx - 6, chain, 12, 17);
        setCol(255, 196, 120);
        fillRect(lx - 3, chain + 4, 6, 9);
    }
    // drifting dust motes
    setCol(180, 240, 230, 60);
    for (int i = 0; i < 36; ++i) {
        float mx = std::fmod(hashf(i * 23 + 1) * 1400.f - cam * 0.8f + (float)VIEW_W, (float)VIEW_W);
        float my = hashf(i * 29 + 8) * 460.f + std::sin(tick * 0.6 + i) * 8.f;
        fillRect(mx, my, 2, 2);
    }
}

static void drawPlatform(const Platform& p) {
    float x = p.x - cam + shX;
    if (x > VIEW_W || x + p.w < 0) return;
    float y = p.y + shY;
    // body: painted rock tile (v5 map look). Tile row 0 sits at the
    // platform's top edge and the pattern is anchored in world x, so it
    // never swims as the camera moves. Fallback: the original body
    // gradient, top #31504e -> #122425 -> #080e10 (stage 1 palette).
    if (terrainTex) {
        const float TW = 240.f;
        float srcX = std::fmod(p.x, TW);
        if (srcX < 0) srcX += TW;
        for (float relX = 0; relX < p.w;) {
            float segW = std::min(TW - srcX, p.w - relX);
            SDL_Rect src{(int)srcX, 0, (int)segW, (int)p.h};
            SDL_FRect dst{x + relX, y, segW, p.h};
            SDL_RenderCopyF(gRen, terrainTex, &src, &dst);
            relX += segW;
            srcX = 0;
        }
    } else {
        gradRect(x, y, p.w, p.h, {49, 80, 78, 255}, {18, 36, 37, 255}, {8, 14, 16, 255});
    }
    setCol(ACCENT, 89);  // accent lip, alpha .35 in the web build
    fillRect(x, y, p.w, 3);
    // cracks
    setCol(0, 0, 0, 76);
    for (int i = 18; i < p.w; i += 61) {
        float d = 12 + (i * 7) % 28;
        thickLine(x + i, y + 4, x + i - 7, y + d, 2);
        thickLine(x + i - 7, y + d, x + i + 5, y + d + 15, 2);
    }
    // speckles
    setCol(255, 255, 255, 14);
    for (int i = 10; i < p.w; i += 43) fillRect(x + i, y + 8 + (i % 19), 18, 2);
}

static void drawSpikeTrap(const SpikeTrap& t) {
    float x = t.x - cam + shX;
    float y = t.y + (1 - t.arm) * 17 + shY;
    if (t.warn > 0) {
        setCol(255, 170, 96, (Uint8)(255 * (0.12f + t.warn * 0.18f)));
        fillRect(x, t.y + 15 + shY, t.w, 5);
    }
    // v3 sprite strip: state follows the same arm logic as the web build
    // (retracted < .08, rising while warning, out > .62), tiles of 48x25
    // with the teeth base on the same line as the old polygons.
    int si = t.arm > 0.62f ? 2 : (t.arm >= 0.08f || t.warn > 0) ? 1 : 0;
    if (spikeTexV3[si]) {
        float baseY = t.y + 20 + (1 - t.arm) * 17 + shY;
        for (float tx = 0; tx < t.w; tx += 48) {
            float tw = std::min(48.f, t.w - tx);
            SDL_Rect srcR{0, 0, (int)(88 * tw / 48), 46};
            SDL_FRect dst{x + tx, baseY - 25, tw, 25};
            SDL_RenderCopyF(gRen, spikeTexV3[si], &srcR, &dst);
        }
        return;
    }
    for (int i = 0; i < t.w; i += 16) {
        setCol(115, 141, 140);
        fillPoly({{x + i, y + 20}, {x + i + 8, y - 2 - (i % 3)}, {x + i + 16, y + 20}});
        setCol(225, 238, 233);
        fillPoly({{x + i + 5, y + 9}, {x + i + 8, y - 2 - (i % 3)}, {x + i + 11, y + 9}});
    }
}

static void drawShard(const Shard& s, int idx) {
    if (s.taken) return;
    float x = s.x - cam + shX;
    float y = s.y + std::sin(tick * 3 + idx) * 5 + shY;
    float ang = std::sin(tick * 1.3 + idx) * 0.28f;
    const float pts[5][2] = {{0, -20}, {12, -3}, {7, 14}, {0, 20}, {-12, 0}};
    auto rot = [&](float px, float py, float k) {
        float ca = std::cos(ang), sa = std::sin(ang);
        return SDL_FPoint{x + (px * ca - py * sa) * k, y + (px * sa + py * ca) * k};
    };
    std::vector<SDL_FPoint> glow, body, core;
    for (auto& p : pts) {
        glow.push_back(rot(p[0], p[1], 1.6f));
        body.push_back(rot(p[0], p[1], 1.0f));
        core.push_back(rot(p[0], p[1], 0.52f));
    }
    setCol(ACCENT, 46);  fillPoly(glow);
    setCol(ACCENT);      fillPoly(body);
    setCol(240, 255, 252); fillPoly(core);
}

static void drawCheckpoint() {
    float x = checkpoint.x - cam + shX;
    if (x < -70 || x > VIEW_W + 70) return;
    float y = checkpoint.y + shY;
    setCol(83, 105, 101);
    fillPoly({{x - 13, y + 40}, {x - 7, y}, {x - 15, y - 15}, {x, y - 27},
              {x + 15, y - 15}, {x + 7, y}, {x + 13, y + 40}});
    setCol(20, 30, 32);
    fillPoly({{x - 7, y + 34}, {x - 3, y + 4}, {x + 3, y + 4}, {x + 7, y + 34}});
    if (checkpoint.active) setCol(ACCENT); else setCol(113, 131, 127);
    ringStroke(x, y - 10, 14, 2.5f);
    float pulse = 0.65f + 0.25f * std::sin(tick * 3);
    if (checkpoint.active) setCol(242, 255, 249, (Uint8)(255 * pulse));
    else setCol(ACCENT, 128);
    fillPoly({{x, y - 21}, {x + 8, y - 10}, {x, y + 3}, {x - 8, y - 10}});
}

static void drawGate() {
    float x = gateRect.x - cam + shX;
    float y = gateRect.y + shY;
    if (x > VIEW_W + 90 || x + 90 < 0) return;
    // portal
    if (gateOpen) setCol(ACCENT, 150); else setCol(10, 18, 20, 235);
    fillPoly({{x + 12, y + 104}, {x + 12, y + 40}, {x + 20, y + 12}, {x + 42, y + 12},
              {x + 50, y + 40}, {x + 50, y + 104}});
    // stone arch
    setCol(58, 74, 72);
    fillRect(x, y + 34, 13, 71);
    fillRect(x + 49, y + 34, 13, 71);
    arcStroke(x + 31, y + 38, 27, 3.14159f, 6.28319f, 12);
    setCol(102, 122, 117);
    fillRect(x, y + 34, 13, 8);
    fillRect(x + 49, y + 34, 13, 8);
    // three seal rings
    if (gateOpen) setCol(233, 255, 248); else setCol(67, 86, 83);
    for (int i = 0; i < 3; ++i) ringStroke(x + 31, y + 25 + i * 27, 4, 2);
}

// Fits one trimmed sprite inside the box (bw x bh) keeping proportions.
// (ax, ay): bottom-centre anchor for ground enemies, centre for fliers.
static void drawSprFit(const SprTex& s, float ax, float ay, float bw, float bh,
                       bool fly, int flip, bool hit) {
    float k = std::min(bw / (float)s.artW, bh / (float)s.artH);
    float dw = s.artW * k, dh = s.artH * k;
    SDL_FRect dst{dw > 0 ? ax - dw / 2 : ax, fly ? ay - dh / 2 : ay - dh, dw, dh};
    SDL_RendererFlip fl = flip < 0 ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;
    SDL_RenderCopyExF(gRen, s.tex, nullptr, &dst, 0, nullptr, fl);
    if (hit && s.white) {
        SDL_SetTextureAlphaMod(s.white, 150);
        SDL_RenderCopyExF(gRen, s.white, nullptr, &dst, 0, nullptr, fl);
        SDL_SetTextureAlphaMod(s.white, 255);
    }
}

// v3 sprite path for drawEnemy. Returns false when the type's sprite is
// unavailable so the caller runs the original code-drawn rendering.
// Presence boxes: the old code-drawn shapes measured ~44x40 (crawler,
// hitbox-sized), ~72x48 (bat, wing span) and ~44x45 (sonic); the boxes
// below are those sizes at x1.7 (crawler, bat) and x1.4 (sonic).
static bool drawEnemySprite(const Enemy& e, float x, float y, float cx,
                            float cy, int look, float wing) {
    const SprTex* spr = nullptr;
    float bw = 0, bh = 0;
    bool fly = false;
    if (e.type == "crawler") {
        spr = ((long)std::floor(tick * 7 + e.phase) % 2) ? &sprCrawlerWalk1
                                                         : &sprCrawlerWalk2;
        bw = 75; bh = 68;
    } else if (e.type == "bat") {
        spr = wing > 0 ? &sprBatUp : &sprBatDown;
        bw = 122; bh = 82; fly = true;
    } else if (e.type == "sonic") {
        spr = &sprSonicIdle;  // M1 sonic has no shot event (sound wave is
                              // roadmap), so the fire frame stays unused.
        bw = 62; bh = 63;
    }
    if (!spr || !spr->tex) return false;
    float bottom = y + e.h;
    if (haloTex) {  // stage-accent rim halo behind the sprite
        float hr = std::max(bw, bh) * 1.45f;
        float hcy = fly ? cy : bottom - bh / 2;
        SDL_FRect hd{cx - hr / 2, hcy - hr / 2, hr, hr};
        SDL_SetTextureAlphaMod(haloTex, 42);
        SDL_RenderCopyF(gRen, haloTex, nullptr, &hd);
        SDL_SetTextureAlphaMod(haloTex, 255);
    }
    if (!fly && shadowTex) {  // soft ground shadow
        float sw = bw * 0.74f;
        SDL_FRect sd{cx - sw / 2, bottom - 6, sw, 13};
        SDL_SetTextureAlphaMod(shadowTex, 215);
        SDL_RenderCopyF(gRen, shadowTex, nullptr, &sd);
        SDL_SetTextureAlphaMod(shadowTex, 255);
    }
    drawSprFit(*spr, cx, fly ? cy : bottom, bw, bh, fly, look, e.hit > 0);
    if (e.hp < e.maxHp) {  // health bar, same as the web build
        setCol(4, 8, 10, 204);
        fillRect(x, e.y - 11 + shY, e.w, 5);
        if (e.hit > 0) setCol(255, 241, 223); else setCol(ACCENT);
        fillRect(x + 1, e.y - 10 + shY, (e.w - 2) * (float)e.hp / e.maxHp, 3);
    }
    return true;
}

static void drawEnemy(const Enemy& e) {
    if (e.dead) return;
    float x = e.x - cam + shX;
    if (x > VIEW_W + 100 || x < -100) return;
    float y = e.y + shY;
    float cx = x + e.w / 2, cy = y + e.h / 2;
    int look = player.box.x > e.x ? 1 : -1;
    float wing = std::sin(tick * 11 + e.phase);
    Uint8 flashA = e.hit > 0 ? 110 : 0;
    if (drawEnemySprite(e, x, y, cx, cy, look, wing)) return;

    if (e.type == "bat") {
        setCol(38, 59, 66);
        fillPoly({{cx - 7, cy - 2}, {cx - 25, cy - 25 - wing * 8}, {cx - 36, cy - 4},
                  {cx - 25, cy + 1}, {cx - 29, cy + 11 + wing * 4}, {cx - 17, cy + 5},
                  {cx - 8, cy + 12}});
        fillPoly({{cx + 7, cy - 2}, {cx + 25, cy - 25 - wing * 8}, {cx + 36, cy - 4},
                  {cx + 25, cy + 1}, {cx + 29, cy + 11 + wing * 4}, {cx + 17, cy + 5},
                  {cx + 8, cy + 12}});
        setCol(66, 88, 94);
        fillEllipse(cx, cy + 3, 11, 15);
        setCol(23, 36, 40);
        fillEllipse(cx, cy + 6, 9, 11);
        setCol(11, 16, 18);
        fillPoly({{cx - 7, cy - 8}, {cx - 3, cy - 20}, {cx + 1, cy - 10}});
        fillPoly({{cx + 1, cy - 10}, {cx + 7, cy - 19}, {cx + 8, cy - 6}});
        setCol(157, 233, 242, 70);
        fillCircle(cx + 3 * look, cy - 3, 6);
        setCol(157, 233, 242);
        fillRect(cx + 3 * look - 2, cy - 4, 4, 3);
    } else if (e.type == "sonic") {
        setCol(57, 122, 130);
        fillPoly({{cx, cy - 24}, {cx + 21, cy - 14}, {cx + 19, cy + 10}, {cx + 8, cy + 21},
                  {cx - 13, cy + 18}, {cx - 22, cy - 2}, {cx - 15, cy - 15}});
        setCol(164, 233, 232);
        arcStroke(cx + 11, cy - 7, 9, -0.7f, 0.7f, 2);
        arcStroke(cx + 11, cy - 7, 16, -0.7f, 0.7f, 2);
        arcStroke(cx + 11, cy - 7, 23, -0.7f, 0.7f, 2);
        setCol(7, 19, 22);
        fillEllipse(cx + 3, cy - 7, 7, 4);
    } else {  // crawler
        setCol(43, 65, 64);
        fillEllipse(cx, cy + 5, e.w / 2, e.h / 2 - 3);
        setCol(64, 88, 86);
        fillEllipse(cx, cy - 1, e.w / 2 - 7, e.h / 2 - 10);
        setCol(145, 176, 170);
        thickLine(cx - 13, cy - 5, cx - 19, cy - 22, 2.5f);
        thickLine(cx - 19, cy - 22, cx - 6, cy - 27, 2.5f);
        thickLine(cx + 13, cy - 5, cx + 19, cy - 22, 2.5f);
        thickLine(cx + 19, cy - 22, cx + 6, cy - 27, 2.5f);
        setCol(38, 58, 57);
        for (int s = -1; s <= 1; s += 2) {
            thickLine(cx + s * 12, cy + 12, cx + s * 24, cy + 19 + wing * 2, 2);
            thickLine(cx + s * 10, cy + 5, cx + s * 24, cy + 6 - wing * 2, 2);
        }
        setCol(255, 179, 110, 60);
        fillCircle(cx + 7 * look, cy - 2, 8);
        setCol(255, 179, 110);
        fillCircle(cx + 7 * look, cy - 2, 3);
    }
    if (flashA) {  // hit flash
        setCol(255, 241, 223, flashA);
        fillEllipse(cx, cy, e.w / 2, e.h / 2);
    }
    if (e.hp < e.maxHp) {  // health bar, as in the web build
        setCol(4, 8, 10, 204);
        fillRect(x, e.y - 11 + shY, e.w, 5);
        if (e.hit > 0) setCol(255, 241, 223); else setCol(ACCENT);
        fillRect(x + 1, e.y - 10 + shY, (e.w - 2) * (float)e.hp / e.maxHp, 3);
    }
}

static Pose pickPose() {
    if (player.attack > 0) return Pose::Attack;
    if (!player.on) return Pose::Jump;
    if (std::fabs(player.vx) > 20.f)
        return ((long)std::floor(tick / 0.14) % 2 == 1) ? Pose::Run2 : Pose::Run;
    return Pose::Idle;
}

// Draws one hero sprite, feet-anchored at (wx, fy) -- the same transform
// the web build uses: drawn height = srcH * HERO_K * ANIM_S, anchored by
// the pose's f fraction, nearest-neighbour so pixels stay chunky.
static void drawHeroPose(Pose pose, float wx, float fy, int dir, float alpha, bool white) {
    const PoseTex& pt = poseTex[(size_t)pose];
    SDL_Texture* t = white ? pt.white : pt.tex;
    if (!t) return;
    float dh = pt.srcH * HERO_K * ANIM_S;
    float dw = dh * (float)pt.w / (float)pt.h;
    float sx = 1.f, sy = 1.f;
    if (pose == Pose::Idle) {  // idle breathing, 1.00 <-> 1.015 over ~2.4s
        float br = 1.f + 0.0075f * (1.f + std::sin(tick * 2.618 - 1.571));
        sy = br; sx = 1.f + (br - 1.f) * 0.5f;
    }
    float dwA = dw * sx, dhA = dh * sy;
    SDL_FRect dst;
    dst.w = dwA; dst.h = dhA;
    dst.x = (dir > 0) ? wx - pt.f * dwA : wx - (1.f - pt.f) * dwA;
    dst.y = fy - dhA + 1;
    SDL_SetTextureAlphaMod(t, (Uint8)(255 * alpha));
    SDL_RenderCopyExF(gRen, t, nullptr, &dst, 0, nullptr,
                      dir < 0 ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    SDL_SetTextureAlphaMod(t, 255);
}

static void drawPlayer() {
    float cxw = player.box.x - cam + player.box.w / 2 + shX;
    float feetY = player.box.y + player.box.h + shY;
    if (player.on && player.hp > 0) {  // ground shadow
        setCol(0, 0, 0, 82);
        fillEllipse(cxw, feetY, ANIM_S * 16, ANIM_S * 3.2f);
    }
    for (auto& tr : trails) {  // dash ghosts
        float a = 0.5f * (tr.life / 0.26f);
        drawHeroPose((Pose)tr.pose, tr.x - cam + player.box.w / 2 + shX,
                     tr.y + player.box.h + shY, tr.dir, a, false);
    }
    float alpha = 1.f;
    if (player.ifr > 0 && player.hp > 0 && (int)std::floor(player.ifr * 15) % 2) alpha = 0.46f;
    Pose pose = pickPose();
    drawHeroPose(pose, cxw, feetY, player.dir, alpha, false);
    // hurt flash: white silhouette of the same pose
    if (player.hurtT > 0 && player.hp > 0)
        drawHeroPose(pose, cxw, feetY, player.dir, alpha * std::min(1.f, player.hurtT / 0.3f) * 0.8f, true);
    // slash arc: chunky pixel crescent in front of the blade, first ~120ms
    if (player.attack > 0 && slashTex) {
        float el = BLADE_COOL - player.attack;
        if (el < 0.12f && player.on) {
            const PoseTex& pt = poseTex[(size_t)Pose::Attack];
            float dh = pt.srcH * HERO_K * ANIM_S;
            float dw = dh * (float)pt.w / (float)pt.h;
            float aw = dw * 0.60f;
            // Web build, in feet-anchored sprite space: the arc's top-left
            // sits at (dx + 0.5*dw, -0.88*dh); mirror x when facing left.
            float lx = -pt.f * dw + dw * 0.50f;
            float sxp = (player.dir > 0) ? cxw + lx : cxw - lx - aw;
            SDL_FRect dst{sxp, feetY - dh * 0.88f, aw, aw};
            SDL_SetTextureAlphaMod(slashTex, (Uint8)(255 * 0.9f * (1.f - el / 0.12f)));
            SDL_RenderCopyExF(gRen, slashTex, nullptr, &dst, 0, nullptr,
                              player.dir < 0 ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
            SDL_SetTextureAlphaMod(slashTex, 255);
        }
    }
}

static void drawFx() {
    for (auto& r : rings) {
        float a = std::min(1.f, r.life / r.maxLife * 2.f);
        setCol(r.c, (Uint8)(255 * a * 0.8f));
        ringStroke(r.x - cam + shX, r.y + shY, r.r, r.width);
    }
    for (auto& q : particles) {
        float a = std::max(0.f, q.life / q.maxLife);
        setCol(q.c, (Uint8)(255 * std::min(1.f, a * 1.8f)));
        fillRect(q.x - cam + shX - q.size / 2, q.y + shY - q.size / 2, q.size, q.size);
    }
}

static void drawHud() {
    // hearts as little masks (the web HUD uses mask icons)
    for (int i = 0; i < player.maxHp; ++i) {
        float mx = 26 + i * 34, my = 30;
        bool lost = i >= player.hp;
        if (lost) setCol(70, 84, 88); else setCol(238, 248, 246);
        fillEllipse(mx, my, 11, 13);
        fillPoly({{mx - 9, my - 8}, {mx - 4, my - 17}, {mx, my - 9}});   // horn nubs
        fillPoly({{mx + 9, my - 8}, {mx + 4, my - 17}, {mx, my - 9}});
        setCol(10, 16, 18);
        fillEllipse(mx - 4.5f, my - 1, 2.2f, 3.2f);
        fillEllipse(mx + 4.5f, my - 1, 2.2f, 3.2f);
    }
    // shard counter
    float sx0 = 16, sy0 = 62;
    setCol(ACCENT);
    fillPoly({{sx0 + 10, sy0}, {sx0 + 19, sy0 + 10}, {sx0 + 14, sy0 + 20},
              {sx0 + 10, sy0 + 24}, {sx0 + 1, sy0 + 12}});
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d / 3 SHARDS", player.got);
    drawText(sx0 + 28, sy0 + 4, 2.f, {220, 240, 238, 255}, buf);
    // light meter
    setCol(20, 30, 34, 200);
    fillRect(16, 94, 124, 9);
    setCol(255, 240, 166);
    fillRect(18, 96, 120 * player.light / 100.f, 5);
    drawText(148, 93, 1.5f, {200, 220, 216, 255}, "LIGHT - L");
    // stage name
    drawText(VIEW_W - 16 - textWidth("ECHO CAVE", 2.f), 18, 2.f, {150, 180, 176, 255}, "ECHO CAVE");
    if (hintTimer > 0 && state == GameState::Playing) {
        const char* msg = player.light < 100.f && gateOpen == false && player.got < 3
            ? "FIND THE THREE LIGHT SHARDS"
            : "THE LIGHT PULSE NEEDS 100 PERCENT";
        drawTextCentered(VIEW_W / 2, VIEW_H - 60, 2.f, {255, 220, 160, 255}, msg);
    }
}

static void drawWorld() {
    shX = (hashf((int)(tick * 977) % 1000) - 0.5f) * shake;
    shY = (hashf((int)(tick * 577) % 1000) - 0.5f) * shake;
    drawBackground();
    for (auto& p : platforms) drawPlatform(p);
    for (auto& t : spikes) drawSpikeTrap(t);
    drawCheckpoint();
    int si = 0;
    for (auto& s : shards) drawShard(s, si++);
    drawGate();
    for (auto& e : enemies) drawEnemy(e);
    drawFx();
    drawPlayer();
    if (flash > 0) {
        setCol(255, 130, 92, (Uint8)(255 * flash * 1.7f > 255 ? 255 : 255 * flash * 1.7f));
        fillRect(0, 0, VIEW_W, VIEW_H);
    }
}

static void drawTitle() {
    drawBackground();
    // a stage floor for the hero to stand on
    Platform ground{0, 470, VIEW_W, 70};
    float savedCam = cam;
    cam = 0; shX = shY = 0;
    drawPlatform(ground);
    cam = savedCam;
    drawTextCentered(VIEW_W / 2, 96, 8.f, {240, 250, 248, 255}, "BLADE OF");
    drawTextCentered(VIEW_W / 2, 96 + 66, 8.f, {240, 250, 248, 255}, "SHADOWS");
    drawTextCentered(VIEW_W / 2, 232, 2.f, ACCENT, "A LITTLE KNIGHT. A STOLEN LIGHT.");
    // the hero, standing on the floor at centre
    drawHeroPose(Pose::Idle, VIEW_W / 2, 470, 1, 1.f, false);
    if ((int)(tick * 2) % 2 == 0)
        drawTextCentered(VIEW_W / 2, 500 - 6, 2.f, {220, 240, 238, 255}, "PRESS ENTER");
}

static void drawIntro() {
    drawWorld();
    setCol(3, 7, 9, 190);
    fillRect(0, 0, VIEW_W, VIEW_H);
    drawTextCentered(VIEW_W / 2, 150, 3.f, {200, 220, 216, 255}, "01 - ECHO CAVE");
    drawTextCentered(VIEW_W / 2, 196, 4.f, {240, 250, 248, 255}, "ECHO CAVE");
    // intro text, wrapped by hand at ~46 chars (scale 2 -> 12px per char)
    drawTextCentered(VIEW_W / 2, 268, 2.f, {190, 215, 210, 255},
                     "BENEATH ORAN, THE WALLS WHISPER THE NAME OF");
    drawTextCentered(VIEW_W / 2, 292, 2.f, {190, 215, 210, 255},
                     "THE ONE WHO DARKENED THE LIGHTHOUSE. THREE");
    drawTextCentered(VIEW_W / 2, 316, 2.f, {190, 215, 210, 255},
                     "SHARDS LIE HIDDEN BETWEEN THE FANGS OF THE CAVE.");
    if ((int)(tick * 2) % 2 == 0)
        drawTextCentered(VIEW_W / 2, 380, 2.f, ACCENT, "PRESS ENTER");
}

static void drawOverlay(const char* title, const char* line1, const char* line2,
                        const char* prompt) {
    drawWorld();
    drawHud();
    setCol(3, 7, 9, 170);
    fillRect(0, 0, VIEW_W, VIEW_H);
    drawTextCentered(VIEW_W / 2, 190, 5.f, {240, 250, 248, 255}, title);
    drawTextCentered(VIEW_W / 2, 258, 2.f, {190, 215, 210, 255}, line1);
    if (line2) drawTextCentered(VIEW_W / 2, 282, 2.f, {190, 215, 210, 255}, line2);
    if ((int)(tick * 2) % 2 == 0)
        drawTextCentered(VIEW_W / 2, 340, 2.f, ACCENT, prompt);
}

static void render() {
    // Draw the scene into the 960x540 target at native resolution (integer
    // aligned, so fills tile perfectly), then upscale it to the window in
    // one nearest-neighbour blit. Drawing straight through a logical-size
    // transform leaves periodic 1px gaps from per-primitive rounding.
    SDL_SetRenderTarget(gRen, gTarget);
    SDL_SetRenderDrawColor(gRen, 0, 0, 0, 255);
    SDL_RenderClear(gRen);
    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);
    switch (state) {
        case GameState::Title: drawTitle(); break;
        case GameState::Intro: drawIntro(); break;
        case GameState::Playing:
        case GameState::Paused:
            drawWorld(); drawHud();
            if (state == GameState::Paused) {
                setCol(3, 7, 9, 150);
                fillRect(0, 0, VIEW_W, VIEW_H);
                drawTextCentered(VIEW_W / 2, 240, 5.f, {240, 250, 248, 255}, "PAUSED");
                drawTextCentered(VIEW_W / 2, 310, 2.f, ACCENT, "ESC TO CONTINUE");
            }
            break;
        case GameState::Dead:
            drawOverlay("THE DARKNESS", "SWALLOWED YOU. EVERY FALL REVEALS A NEW",
                        "TIMING FOR ATTACK AND DASH.", "PRESS ENTER TO TRY AGAIN");
            break;
        case GameState::Won:
            drawOverlay("STAGE COMPLETE", "THE GATE OPENS. THE SHARDS DRAW THE PATH",
                        "TO THE CRYSTAL VAULT.", "PRESS ENTER");
            break;
    }
    SDL_SetRenderTarget(gRen, nullptr);
    SDL_SetRenderDrawColor(gRen, 0, 0, 0, 255);
    SDL_RenderClear(gRen);
    SDL_RenderCopy(gRen, gTarget, nullptr, nullptr);
    if (!gHeadless) SDL_RenderPresent(gRen);
}

// ------------------------------------------------------------- entry points
static std::string assetDir() {
    std::string base;
    char* bp = SDL_GetBasePath();
    if (bp) { base = bp; SDL_free(bp); }
    return base + "assets/";
}

static void respawnFromDeath() {
    float savedRespawn = respawnX;
    bool hadCp = checkpoint.active;
    loadStage();
    if (hadCp) {  // respawn at the altar, world reset (web build reloads too)
        respawnX = savedRespawn;
        checkpoint.active = true;
        player.box.x = savedRespawn;
        player.box.y = 350;
        cam = clampf(player.box.x - VIEW_W * 0.38f, 0.f, WORLD_W - VIEW_W);
    }
    state = GameState::Playing;
}

static void handleKeyDown(SDL_Keycode k) {
    switch (state) {
        case GameState::Title:
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) state = GameState::Intro;
            break;
        case GameState::Intro:
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) state = GameState::Playing;
            break;
        case GameState::Playing:
            if (k == SDLK_SPACE) pressJump();
            else if (k == SDLK_j) pressAttack();
            else if (k == SDLK_k) pressDash();
            else if (k == SDLK_l) pressPulse();
            else if (k == SDLK_ESCAPE) state = GameState::Paused;
            break;
        case GameState::Paused:
            if (k == SDLK_ESCAPE) state = GameState::Playing;
            break;
        case GameState::Dead:
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) respawnFromDeath();
            break;
        case GameState::Won:
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                loadStage();
                state = GameState::Playing;
            }
            break;
    }
}

static int runInteractive() {
    bool running = true;
    Uint64 last = SDL_GetPerformanceCounter();
    double acc = 0;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            else if (ev.type == SDL_KEYDOWN) {
                if (ev.key.repeat) {
                    // held keys are tracked via key state below
                } else if (ev.key.keysym.sym == SDLK_a || ev.key.keysym.sym == SDLK_LEFT)
                    keyLeft = true;
                else if (ev.key.keysym.sym == SDLK_d || ev.key.keysym.sym == SDLK_RIGHT)
                    keyRight = true;
                else if (ev.key.keysym.sym == SDLK_w || ev.key.keysym.sym == SDLK_UP)
                    keyUp = true;
                else if (ev.key.keysym.sym == SDLK_s || ev.key.keysym.sym == SDLK_DOWN)
                    keyDown = true;
                else
                    handleKeyDown(ev.key.keysym.sym);
                if (state == GameState::Title && ev.key.keysym.sym == SDLK_ESCAPE)
                    running = false;
            } else if (ev.type == SDL_KEYUP) {
                if (ev.key.keysym.sym == SDLK_a || ev.key.keysym.sym == SDLK_LEFT)
                    keyLeft = false;
                else if (ev.key.keysym.sym == SDLK_d || ev.key.keysym.sym == SDLK_RIGHT)
                    keyRight = false;
                else if (ev.key.keysym.sym == SDLK_w || ev.key.keysym.sym == SDLK_UP)
                    keyUp = false;
                else if (ev.key.keysym.sym == SDLK_s || ev.key.keysym.sym == SDLK_DOWN)
                    keyDown = false;
                else if (ev.key.keysym.sym == SDLK_SPACE)
                    releaseJump();
            }
        }
        Uint64 now = SDL_GetPerformanceCounter();
        double frame = (double)(now - last) / (double)SDL_GetPerformanceFrequency();
        last = now;
        if (frame > 0.1) frame = 0.1;
        acc += frame;
        int steps = 0;
        while (acc >= STEP && steps < 5) {
            if (state == GameState::Playing) stepGame(STEP);
            else tick += STEP;
            acc -= STEP;
            ++steps;
        }
        if (steps == 5) acc = 0;
        render();
        SDL_Delay(1);
    }
    return 0;
}

// Headless QA: dummy video driver + software renderer, scripted inputs,
// one PNG out. Usage:
//   ./blade --screenshot out.png [frames]   gameplay (run right, jump)
//   ./blade --screenshot out.png title      title screen
//   ./blade --screenshot out.png attack     attack pose near the first crawler
static int runScreenshot(const char* outPath, const char* modeArg) {
    gHeadless = true;
    std::string mode = modeArg ? modeArg : "game";
    int frames = 132;
    if (mode != "title" && mode != "attack" && mode != "game") {
        frames = std::atoi(mode.c_str());
        if (frames <= 0) frames = 132;
        mode = "game";
    }
    loadStage();
    if (mode == "title") {
        state = GameState::Title;
        for (int i = 0; i < 30; ++i) tick += STEP;
    } else if (mode == "attack") {
        state = GameState::Playing;
        // same route as the gameplay script, then strike at the first crawler
        for (int f = 0; f < 137; ++f) {
            keyRight = (f >= 5 && f < 38) || (f >= 51);
            if (f == 51) pressDash();
            if (f == 58 || f == 68) pressJump();
            if (f == 131) pressAttack();
            stepGame(STEP);
        }
        keyRight = false;
    } else {  // scripted gameplay run
        state = GameState::Playing;
        // The route the stage design asks for: run, pause before the spike
        // trap (so it stays disarmed), dash the gap mouth, double-jump
        // across, land and run on toward the first crawler.
        for (int f = 0; f < frames; ++f) {
            keyRight = (f >= 5 && f < 38) || (f >= 51);
            if (f == 51) pressDash();
            if (f == 58 || f == 68) pressJump();
            stepGame(STEP);
        }
        keyRight = false;
    }
    render();
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(gRen, &ow, &oh);
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) { std::fprintf(stderr, "surface: %s\n", SDL_GetError()); return 1; }
    if (SDL_RenderReadPixels(gRen, nullptr, SDL_PIXELFORMAT_ARGB8888,
                             surf->pixels, surf->pitch) != 0) {
        std::fprintf(stderr, "readpixels: %s\n", SDL_GetError());
        return 1;
    }
    if (IMG_SavePNG(surf, outPath) != 0) {
        std::fprintf(stderr, "savepng: %s\n", IMG_GetError());
        return 1;
    }
    std::printf("screenshot saved: %s (%dx%d, mode=%s, frames=%d)\n",
                outPath, ow, oh, mode.c_str(), frames);
    std::printf("player pos: x=%.0f y=%.0f hp=%d shards=%d state=%d\n",
                player.box.x, player.box.y, player.hp, player.got, (int)state);
    SDL_FreeSurface(surf);
    return 0;
}

int main(int argc, char** argv) {
    bool shotMode = argc >= 3 && std::strcmp(argv[1], "--screenshot") == 0;
    if (shotMode) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    int imgFlags = IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    if ((imgFlags & IMG_INIT_PNG) == 0) {
        std::fprintf(stderr, "IMG_Init: %s\n", IMG_GetError());
        return 1;
    }
    if ((imgFlags & IMG_INIT_JPG) == 0)
        std::fprintf(stderr, "warning: no JPG support; painted background unavailable\n");
    SDL_Window* win = SDL_CreateWindow(
        "Blade of Shadows", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, shotMode ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    if (!win) { std::fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    gRen = SDL_CreateRenderer(win, -1, shotMode ? SDL_RENDERER_SOFTWARE
                                                : SDL_RENDERER_ACCELERATED);
    if (!gRen) gRen = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!gRen) { std::fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");  // nearest: chunky pixels
    gTarget = SDL_CreateTexture(gRen, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_TARGET, VIEW_W, VIEW_H);
    if (!gTarget) { std::fprintf(stderr, "target: %s\n", SDL_GetError()); return 1; }
    SDL_SetTextureScaleMode(gTarget, SDL_ScaleModeNearest);

    std::string dir = assetDir();
    if (!loadAssets(dir)) {
        // fall back to a plain relative path (e.g. running from project root)
        if (!loadAssets("assets/")) return 1;
    }
    loadStage();

    int rc = 0;
    if (shotMode) rc = runScreenshot(argv[2], argc >= 4 ? argv[3] : nullptr);
    else rc = runInteractive();

    SDL_DestroyTexture(gTarget);
    SDL_DestroyRenderer(gRen);
    SDL_DestroyWindow(win);
    IMG_Quit();
    SDL_Quit();
    return rc;
}
