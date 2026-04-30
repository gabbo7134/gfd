#include "raylib.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ────────────────────────────────────────────────────────────────
 */

#define SCREEN_W   1280
#define SCREEN_H    720

/* Physics */
#define GRAVITY       950.0f
#define JUMP_VEL     -490.0f
#define P_SPEED       220.0f

/* Player dimensions (no sprite now, but keep same size) */
#define PF_W       48
#define PF_H       48
#define PF_COLS     6
#define P_SCALE     1.5f
#define P_DISP_W   (PF_W * P_SCALE)
#define P_DISP_H   (PF_H * P_SCALE)

/* Slime dimensions */
#define SF_W       32
#define SF_H       32
#define SF_COLS     7
#define S_SCALE     2.0f
#define S_DISP_W   (SF_W * S_SCALE)
#define S_DISP_H   (SF_H * S_SCALE)

/* Door dimensions */
#define DF_W       48
#define DF_H       32

#define MAX_PLAT   10
#define MAX_SLIMES  5

typedef enum { PHASE_PLAY, PHASE_DEAD, PHASE_WIN } Phase;

typedef struct {
    float x, y;
    float vx, vy;
    bool  on_ground;
    bool  face_right;
    int   anim_col;
    int   anim_row;
    float anim_t;
} Player;

typedef struct {
    float x, y;
    float vx;
    float ground_y;
    bool  face_right;
    int   anim_col;
    float anim_t;
    float patrol_l;
    float patrol_r;
} Slime;

typedef struct { float x, y, w, h; } Rect;

typedef struct {
    Player player;
    Slime  slimes[MAX_SLIMES];
    int    nslimes;
    Rect   plats[MAX_PLAT];
    int    nplats;
    Rect   door;
    Phase  phase;
} Game;

/* ────────────────────────────────────────────────────────────────
 * SHARED STATE
 * ────────────────────────────────────────────────────────────────
 */

static Game            g;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static struct {
    bool left, right, jump;
    pthread_mutex_t mu;
} inp;

static atomic_bool g_reset;
static atomic_bool g_quit;

/* ────────────────────────────────────────────────────────────────
 * UTILS
 * ────────────────────────────────────────────────────────────────
 */

static bool rects_overlap(Rect a, Rect b)
{
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

static void precise_sleep(double seconds)
{
    if (seconds <= 0.0) return;
    double target = GetTime() + seconds;
    long ns = (long)(seconds * 0.8 * 1e9);
    if (ns > 0) {
        struct timespec ts = { ns / 1000000000L, ns % 1000000000L };
        nanosleep(&ts, NULL);
    }
    while (GetTime() < target) {}
}

/* ────────────────────────────────────────────────────────────────
 * GAME INIT
 * ────────────────────────────────────────────────────────────────
 */

static void game_init(Game *gp)
{
    memset(gp, 0, sizeof(*gp));
    gp->phase = PHASE_PLAY;

    const Rect plats[] = {
        {    0, 668, 1280,  52 },
        {  150, 556,  200,  18 },
        {  390, 468,  200,  18 },
        {  630, 382,  170,  18 },
        {  840, 470,  215,  18 },
        { 1050, 352,  230,  18 },
        {  -30,   0,   30, 720 },
        { 1280,   0,   30, 720 },
    };
    gp->nplats = sizeof(plats)/sizeof(plats[0]);
    memcpy(gp->plats, plats, sizeof(plats));

    gp->door = (Rect){ 1170, 280, DF_W, DF_H };

    gp->player.x = 80;
    gp->player.y = 668 - P_DISP_H;
    gp->player.face_right = true;

    const struct { float x,y,vx,pl,pr; } sd[] = {
        { 200, 556 - S_DISP_H, 60, 155, 330 },
        { 430, 468 - S_DISP_H, 55, 395, 566 },
        { 670, 382 - S_DISP_H, 65, 635, 776 },
        { 880, 470 - S_DISP_H, 70, 845,1021 },
    };
    gp->nslimes = sizeof(sd)/sizeof(sd[0]);

    for (int i = 0; i < gp->nslimes; i++) {
        gp->slimes[i] = (Slime){
            sd[i].x, sd[i].y, sd[i].vx,
            sd[i].y, true, 0, 0,
            sd[i].pl, sd[i].pr
        };
    }
}

/* ────────────────────────────────────────────────────────────────
 * COLLISIONS
 * ────────────────────────────────────────────────────────────────
 */

static void resolve_collisions(Game *gp)
{
    Player *p = &gp->player;

    float hoff = P_DISP_W * 0.20f;
    float htop = P_DISP_H * 0.25f;
    float hx   = p->x + hoff;
    float hy   = p->y + htop;
    float hw   = P_DISP_W * 0.60f;
    float hh   = P_DISP_H - htop;

    p->on_ground = false;

    for (int i = 0; i < gp->nplats; i++) {
        const Rect *pl = &gp->plats[i];

        if (hx + hw <= pl->x || hx >= pl->x + pl->w) continue;
        if (hy + hh <= pl->y || hy >= pl->y + pl->h) continue;

        float ol = (hx + hw) - pl->x;
        float or_ = (pl->x + pl->w) - hx;
        float ot = (hy + hh) - pl->y;
        float ob = (pl->y + pl->h) - hy;

        float min_v = (ot < ob) ? ot : ob;
        float min_h = (ol < or_) ? ol : or_;

        if (min_v <= min_h) {
            if (ot < ob && p->vy >= 0) {
                p->y = pl->y - hh - htop;
                p->vy = 0;
                p->on_ground = true;
            } else if (ob <= ot && p->vy < 0 && ob > 5.0f) {
                p->y = pl->y + pl->h - htop;
                p->vy = 0;
            }
        } else {
            if (ol < or_)
                p->x = pl->x - hw - hoff;
            else
                p->x = pl->x + pl->w - hoff;
            p->vx = 0;
        }

        hx = p->x + hoff;
        hy = p->y + htop;
    }
}

/* ────────────────────────────────────────────────────────────────
 * PHYSICS THREAD
 * ──────────────────────────────────────────────────��─────────────
 */

static void *physics_fn(void *arg)
{
    const double period = 1.0/120.0;
    double next = GetTime() + period;

    while (!atomic_load(&g_quit)) {

        if (atomic_exchange(&g_reset, false)) {
            pthread_mutex_lock(&g_mu);
            game_init(&g);
            pthread_mutex_unlock(&g_mu);
            next = GetTime() + period;
            continue;
        }

        float dt = period;

        pthread_mutex_lock(&g_mu);

        if (g.phase == PHASE_PLAY) {
            Player *p = &g.player;

            pthread_mutex_lock(&inp.mu);
            bool go_l = inp.left;
            bool go_r = inp.right;
            bool jump = inp.jump;
            inp.jump = false;
            pthread_mutex_unlock(&inp.mu);

            if (go_l && !go_r) { p->vx = -P_SPEED; p->face_right = false; }
            else if (go_r && !go_l) { p->vx = P_SPEED; p->face_right = true; }
            else p->vx = 0;

            if (jump && p->on_ground) {
                p->vy = JUMP_VEL;
                p->on_ground = false;
            }

            p->vy += GRAVITY * dt;
            p->x  += p->vx * dt;
            p->y  += p->vy * dt;

            resolve_collisions(&g);

            if (p->y > SCREEN_H + 150)
                g.phase = PHASE_DEAD;

            Rect pr = {
                p->x + P_DISP_W*0.20f,
                p->y,
                P_DISP_W*0.60f,
                P_DISP_H
            };
            if (rects_overlap(pr, g.door))
                g.phase = PHASE_WIN;
        }

        pthread_mutex_unlock(&g_mu);

        double rem = next - GetTime();
        if (rem > 0) precise_sleep(rem);
        next += period;
    }

    return NULL;
}

/* ────────────────────────────────────────────────────────────────
 * AI THREAD
 * ────────────────────────────────────────────────────────────────
 */

static void *ai_fn(void *arg)
{
    const double period = 1.0/30.0;
    double next = GetTime() + period;

    while (!atomic_load(&g_quit)) {

        float dt = period;

        pthread_mutex_lock(&g_mu);

        if (g.phase == PHASE_PLAY) {
            Player *p = &g.player;

            float px = p->x + P_DISP_W*0.22f;
            float py = p->y + P_DISP_H*0.10f;
            float pw = P_DISP_W*0.56f;
            float ph = P_DISP_H*0.85f;

            for (int i = 0; i < g.nslimes; i++) {
                Slime *s = &g.slimes[i];

                s->x += s->vx * dt;
                s->y  = s->ground_y;

                if (s->x < s->patrol_l) {
                    s->x = s->patrol_l;
                    s->vx = fabsf(s->vx);
                    s->face_right = true;
                }
                if (s->x + S_DISP_W > s->patrol_r) {
                    s->x = s->patrol_r - S_DISP_W;
                    s->vx = -fabsf(s->vx);
                    s->face_right = false;
                }

                s->anim_t += dt;
                const float frame_time = 1.0f / 8.0f;
                while (s->anim_t >= frame_time) {
                    s->anim_t -= frame_time;
                    s->anim_col = (s->anim_col + 1) % SF_COLS;
                }

                float sx = s->x + S_DISP_W*0.32f;
                float sy = s->y + S_DISP_H*0.35f;
                float sw = S_DISP_W*0.36f;
                float sh = S_DISP_H*0.50f;

                if (px+pw > sx && px < sx+sw &&
                    py+ph > sy && py < sy+sh)
                {
                    g.phase = PHASE_DEAD;
                }
            }
        }

        pthread_mutex_unlock(&g_mu);

        double rem = next - GetTime();
        if (rem > 0) precise_sleep(rem);
        next += period;
    }

    return NULL;
}
/* ────────────────────────────────────────────────────────────────
 * MAIN + RENDERING (SPRITES)
 * ────────────────────────────────────────────────────────────────
 */

int main(void)
{
    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);
    InitWindow(SCREEN_W, SCREEN_H, "Dungeon Platformer — Sprites");
    SetTargetFPS(60);

    Texture2D tex_player = LoadTexture("sprites_characters_player.png");
    Texture2D tex_slime  = LoadTexture("sprites_characters_slime.png");
    Texture2D tex_door   = LoadTexture("wooden_door.png");

    pthread_mutex_init(&inp.mu, NULL);
    atomic_store(&g_reset, false);
    atomic_store(&g_quit, false);
    game_init(&g);

    pthread_t th_phys, th_ai;
    pthread_create(&th_phys, NULL, physics_fn, NULL);
    pthread_create(&th_ai,   NULL, ai_fn,      NULL);

    while (!WindowShouldClose()) {

        pthread_mutex_lock(&inp.mu);
        inp.left  = IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A);
        inp.right = IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D);
        if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
            inp.jump = true;
        pthread_mutex_unlock(&inp.mu);

        if (IsKeyPressed(KEY_R))
            atomic_store(&g_reset, true);

        pthread_mutex_lock(&g_mu);
        Game snap = g;
        pthread_mutex_unlock(&g_mu);

        BeginDrawing();

        ClearBackground((Color){22,14,34,255});

        for (int yy = 0; yy < SCREEN_H; yy += 64)
            DrawLine(0, yy, SCREEN_W, yy, (Color){40,30,55,255});

        /* Platforms */
        for (int i = 0; i < snap.nplats; i++) {
            Rect r = snap.plats[i];
            if (r.x < -50 || r.x > SCREEN_W + 50) continue;

            DrawRectangleRec(
                (Rectangle){r.x, r.y, r.w, r.h},
                (Color){120,110,90,255}
            );
            DrawRectangleRec(
                (Rectangle){r.x, r.y, r.w, 6},
                (Color){160,150,120,255}
            );
        }

        /* Door */
        DrawTexturePro(
            tex_door,
            (Rectangle){0, 0, (float)tex_door.width, (float)tex_door.height},
            (Rectangle){snap.door.x, snap.door.y, snap.door.w, snap.door.h},
            (Vector2){0, 0},
            0.0f,
            WHITE
        );
        DrawRectangleLinesEx(
            (Rectangle){snap.door.x-4, snap.door.y-4,
                        snap.door.w+8, snap.door.h+8},
            2,
            (Color){255,220,60,180}
        );

        /* Slimes */
        for (int i = 0; i < snap.nslimes; i++) {
            Slime *s = &snap.slimes[i];
            int s_frame = s->anim_col;
            float s_fw = (float)SF_W;
            float s_fh = (float)SF_H;
            float s_flip = s->face_right ? 1.0f : -1.0f;
            float s_src_x = s->face_right ? s_frame * s_fw : (s_frame + 1) * s_fw;

            Rectangle src = { s_src_x, 0, s_fw * s_flip, s_fh };
            Rectangle dst = { s->x, s->y, S_DISP_W, S_DISP_H };

            DrawTexturePro(tex_slime, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
        }

        /* Player */
        {
            Player *p = &snap.player;
            int p_frame = (int)(GetTime() * 10.0f) % PF_COLS;
            float p_fw = (float)PF_W;
            float p_fh = (float)PF_H;
            float p_flip = p->face_right ? 1.0f : -1.0f;
            float p_src_x = p->face_right ? p_frame * p_fw : (p_frame + 1) * p_fw;

            Rectangle src = { p_src_x, 0, p_fw * p_flip, p_fh };
            Rectangle dst = { p->x, p->y, P_DISP_W, P_DISP_H };

            DrawTexturePro(tex_player, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
        }

        DrawText("← → / A D : Move     Space / W / ↑ : Jump     R : Restart",
                 10, 8, 15, (Color){180,180,200,190});

        if (snap.phase == PHASE_DEAD) {
            DrawRectangle(0,0,SCREEN_W,SCREEN_H,(Color){180,0,0,100});
            DrawText("YOU DIED",
                     SCREEN_W/2 - MeasureText("YOU DIED",64)/2,
                     SCREEN_H/2 - 60,
                     64, RED);
        }

        if (snap.phase == PHASE_WIN) {
            DrawRectangle(0,0,SCREEN_W,SCREEN_H,(Color){0,180,80,90});
            DrawText("YOU WIN!",
                     SCREEN_W/2 - MeasureText("YOU WIN!",64)/2,
                     SCREEN_H/2 - 60,
                     64, (Color){80,255,140,255});
        }

        EndDrawing();
    }

    atomic_store(&g_quit, true);
    pthread_join(th_phys, NULL);
    pthread_join(th_ai,   NULL);

    UnloadTexture(tex_player);
    UnloadTexture(tex_slime);
    UnloadTexture(tex_door);

    CloseWindow();
    return 0;
}
