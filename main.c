#include "raylib.h"
#include "rlgl.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 960
#define SCREEN_H 660
#define VERB_BAR_H 120
#define PLAY_H (SCREEN_H - VERB_BAR_H)
#define ACTOR_SPEED 140.0f
#define MESSAGE_TIME 3.0f
#define ACTOR_SCALE_TOP    0.55f
#define ACTOR_SCALE_BOTTOM 1.0f
#define MAX_WB_VERTS 128
#define MAX_FG_POLYS 16
#define VERT_GRAB_RADIUS 10.0f
#define EDGE_GRAB_RADIUS 12.0f
#define WALKBOX_PATH "walkbox.txt"
#define FG_PATH "fg.txt"
#define PLAYER_POS_PATH "player.txt"
#define PLAYER_POS_INTERVAL 0.1f
#define MAX_SPRITES 300
#define MAX_ANIM_FRAMES 32
#define ANIM_COUNT 9
#define SPRITES_DIR "guybrush_sprites_v3"
#define SPRITES_META_PATH "sprites.txt"
#define ACTOR_SPRITES_DIR "sprites/guybrush"
#define ACTOR_ANIM_FPS 8.0f
#define ACTOR_SPRITE_SCALE 2.5f

enum {
    ANIM_IDLE,
    ANIM_WALK_DOWN, ANIM_WALK_UP, ANIM_WALK_LEFT, ANIM_WALK_RIGHT,
    ANIM_FACE_DOWN, ANIM_FACE_UP, ANIM_FACE_LEFT, ANIM_FACE_RIGHT
};

static const char *anim_names[ANIM_COUNT] = {
    "idle",
    "walk_down", "walk_up", "walk_left", "walk_right",
    "face_down", "face_up", "face_left", "face_right"
};

typedef struct {
    int frames[MAX_ANIM_FRAMES];
    int count;
} Animation;

enum { DIR_DOWN, DIR_UP, DIR_LEFT, DIR_RIGHT };

typedef struct {
    Texture2D frames[MAX_ANIM_FRAMES];
    int count;
} AnimTextures;

typedef struct {
    Vector2 p[MAX_WB_VERTS];
    int n;
} Walkbox;

static float cross2(Vector2 a, Vector2 b, Vector2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static bool point_in_polygon(Vector2 p, const Walkbox *wb) {
    if (wb->n < 3) return false;
    bool inside = false;
    for (int i = 0, j = wb->n - 1; i < wb->n; j = i++) {
        Vector2 a = wb->p[i], b = wb->p[j];
        if (((a.y > p.y) != (b.y > p.y)) &&
            (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) {
            inside = !inside;
        }
    }
    return inside;
}

static Vector2 closest_on_segment(Vector2 a, Vector2 b, Vector2 p) {
    Vector2 ab = { b.x - a.x, b.y - a.y };
    float len2 = ab.x * ab.x + ab.y * ab.y;
    if (len2 == 0) return a;
    float t = ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (Vector2){ a.x + ab.x * t, a.y + ab.y * t };
}

static bool segments_cross(Vector2 p1, Vector2 p2, Vector2 p3, Vector2 p4);

static bool segment_clear(Vector2 a, Vector2 b, const Walkbox *wb) {
    Vector2 mid = { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
    if (!point_in_polygon(mid, wb)) return false;
    for (int i = 0; i < wb->n; i++) {
        Vector2 p = wb->p[i], q = wb->p[(i + 1) % wb->n];
        if (segments_cross(a, b, p, q)) return false;
    }
    return true;
}

static int find_path(Vector2 start, Vector2 end, const Walkbox *wb, Vector2 *out, int max_out) {
    if (wb->n < 3 || max_out < 2) {
        if (max_out >= 2) { out[0] = start; out[1] = end; return 2; }
        return 0;
    }
    if (segment_clear(start, end, wb)) {
        out[0] = start;
        out[1] = end;
        return 2;
    }

    int n = wb->n;
    int SI = n;
    int EI = n + 1;
    int N = n + 2;

    Vector2 nodes[MAX_WB_VERTS + 2];
    for (int i = 0; i < n; i++) nodes[i] = wb->p[i];
    nodes[SI] = start;
    nodes[EI] = end;

    static float W[MAX_WB_VERTS + 2][MAX_WB_VERTS + 2];
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            if (i == j) { W[i][j] = 0; continue; }
            bool adj = (i < n && j < n) &&
                       (j == (i + 1) % n || i == (j + 1) % n);
            bool clear = adj || segment_clear(nodes[i], nodes[j], wb);
            if (clear) {
                float dx = nodes[i].x - nodes[j].x;
                float dy = nodes[i].y - nodes[j].y;
                W[i][j] = sqrtf(dx * dx + dy * dy);
            } else {
                W[i][j] = INFINITY;
            }
        }
    }

    float d[MAX_WB_VERTS + 2];
    int prev[MAX_WB_VERTS + 2];
    bool visited[MAX_WB_VERTS + 2] = { 0 };
    for (int i = 0; i < N; i++) { d[i] = INFINITY; prev[i] = -1; }
    d[SI] = 0;

    for (int k = 0; k < N; k++) {
        int u = -1;
        float best = INFINITY;
        for (int i = 0; i < N; i++) {
            if (!visited[i] && d[i] < best) { best = d[i]; u = i; }
        }
        if (u < 0 || u == EI) break;
        visited[u] = true;
        for (int v = 0; v < N; v++) {
            if (visited[v]) continue;
            float alt = d[u] + W[u][v];
            if (alt < d[v]) { d[v] = alt; prev[v] = u; }
        }
    }

    if (d[EI] == INFINITY) {
        out[0] = start;
        return 1;
    }

    int chain[MAX_WB_VERTS + 2];
    int len = 0;
    for (int u = EI; u != -1 && len < N; u = prev[u]) chain[len++] = u;

    int out_count = 0;
    for (int i = len - 1; i >= 0 && out_count < max_out; i--) {
        out[out_count++] = nodes[chain[i]];
    }
    return out_count;
}

static Vector2 clamp_to_walkbox(Vector2 p, const Walkbox *wb) {
    if (wb->n < 3) return p;
    if (point_in_polygon(p, wb)) return p;
    Vector2 best = wb->p[0];
    float best_d2 = INFINITY;
    for (int i = 0; i < wb->n; i++) {
        Vector2 c = closest_on_segment(wb->p[i], wb->p[(i + 1) % wb->n], p);
        float dx = c.x - p.x, dy = c.y - p.y;
        float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) { best_d2 = d2; best = c; }
    }
    return best;
}

static float actor_scale_at(Vector2 pos, const Walkbox *wb) {
    if (wb->n < 1) return 1.0f;
    float y_top = wb->p[0].y, y_bot = wb->p[0].y;
    for (int i = 1; i < wb->n; i++) {
        if (wb->p[i].y < y_top) y_top = wb->p[i].y;
        if (wb->p[i].y > y_bot) y_bot = wb->p[i].y;
    }
    float t = (y_bot <= y_top) ? 1.0f : (pos.y - y_top) / (y_bot - y_top);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return ACTOR_SCALE_TOP + (ACTOR_SCALE_BOTTOM - ACTOR_SCALE_TOP) * t;
}

static int nearest_vertex(Vector2 p, const Walkbox *wb, float max_radius) {
    int best = -1;
    float best_d2 = max_radius * max_radius;
    for (int i = 0; i < wb->n; i++) {
        float dx = wb->p[i].x - p.x, dy = wb->p[i].y - p.y;
        float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }
    return best;
}

static int nearest_edge(Vector2 p, const Walkbox *wb, float max_radius, Vector2 *out_proj) {
    if (wb->n < 2) return -1;
    int best = -1;
    float best_d2 = max_radius * max_radius;
    Vector2 best_proj = { 0 };
    for (int i = 0; i < wb->n; i++) {
        Vector2 a = wb->p[i], b = wb->p[(i + 1) % wb->n];
        Vector2 c = closest_on_segment(a, b, p);
        float dx = c.x - p.x, dy = c.y - p.y;
        float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) { best_d2 = d2; best = i; best_proj = c; }
    }
    if (out_proj && best >= 0) *out_proj = best_proj;
    return best;
}

static int insert_vertex(Walkbox *wb, int after_idx, Vector2 v) {
    if (wb->n >= MAX_WB_VERTS) return -1;
    int at = after_idx + 1;
    for (int i = wb->n; i > at; i--) wb->p[i] = wb->p[i - 1];
    wb->p[at] = v;
    wb->n++;
    return at;
}

static bool save_walkbox(const char *path, const Walkbox *wb) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    for (int i = 0; i < wb->n; i++) {
        fprintf(f, "%d %d\n", (int)wb->p[i].x, (int)wb->p[i].y);
    }
    fclose(f);
    return true;
}

static bool load_walkbox(const char *path, Walkbox *wb) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    wb->n = 0;
    float x, y;
    while (wb->n < MAX_WB_VERTS && fscanf(f, "%f %f", &x, &y) == 2) {
        wb->p[wb->n++] = (Vector2){ x, y };
    }
    fclose(f);
    return wb->n >= 3;
}

static int load_fg_list(const char *path, Walkbox *out, int max_polys) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int idx = 0;
    out[0].n = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\n' || *p == '\0' || *p == '#') {
            if (out[idx].n >= 3 && idx + 1 < max_polys) {
                idx++;
                out[idx].n = 0;
            }
            continue;
        }
        float x, y;
        if (sscanf(p, "%f %f", &x, &y) == 2 && out[idx].n < MAX_WB_VERTS) {
            out[idx].p[out[idx].n++] = (Vector2){ x, y };
        }
    }
    fclose(f);
    int count = idx + (out[idx].n >= 3 ? 1 : 0);
    return count;
}

static void load_actor_anims(const char *base, AnimTextures *out) {
    for (int i = 0; i < ANIM_COUNT; i++) {
        out[i].count = 0;
        for (int f = 1; f <= MAX_ANIM_FRAMES; f++) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s/%02d.png", base, anim_names[i], f);
            if (!FileExists(path)) break;
            Texture2D t = LoadTexture(path);
            SetTextureFilter(t, TEXTURE_FILTER_POINT);
            out[i].frames[out[i].count++] = t;
        }
    }
}

static void unload_actor_anims(AnimTextures *anims) {
    for (int i = 0; i < ANIM_COUNT; i++) {
        for (int f = 0; f < anims[i].count; f++) UnloadTexture(anims[i].frames[f]);
        anims[i].count = 0;
    }
}

static int resolve_anim(int want, const AnimTextures *anims) {
    if (anims[want].count > 0) return want;
    switch (want) {
        case ANIM_FACE_DOWN:  if (anims[ANIM_WALK_DOWN].count > 0)  return ANIM_WALK_DOWN;  break;
        case ANIM_FACE_UP:    if (anims[ANIM_WALK_UP].count > 0)    return ANIM_WALK_UP;    break;
        case ANIM_FACE_LEFT:  if (anims[ANIM_WALK_LEFT].count > 0)  return ANIM_WALK_LEFT;  break;
        case ANIM_FACE_RIGHT: if (anims[ANIM_WALK_RIGHT].count > 0) return ANIM_WALK_RIGHT; break;
    }
    if (anims[ANIM_IDLE].count > 0) return ANIM_IDLE;
    for (int i = 0; i < ANIM_COUNT; i++) if (anims[i].count > 0) return i;
    return -1;
}

static int load_sprites_from_dir(const char *dir, Texture2D *out, int max_count) {
    int count = 0;
    for (int i = 1; i <= max_count; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/sprite_%03d.png", dir, i);
        if (!FileExists(path)) break;
        out[count++] = LoadTexture(path);
    }
    return count;
}

static void load_anims_from_file(const char *path, Animation *anims) {
    for (int i = 0; i < ANIM_COUNT; i++) anims[i].count = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char name[32];
        if (sscanf(line, "%31s", name) != 1) continue;
        int idx = -1;
        for (int i = 0; i < ANIM_COUNT; i++) {
            if (strcmp(name, anim_names[i]) == 0) { idx = i; break; }
        }
        if (idx < 0) continue;
        anims[idx].count = 0;
        const char *p = strstr(line, name);
        if (p) p += strlen(name);
        while (p && *p && anims[idx].count < MAX_ANIM_FRAMES) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\n' || *p == '\0' || *p == '\r') break;
            int n;
            if (sscanf(p, "%d", &n) == 1) {
                anims[idx].frames[anims[idx].count++] = n;
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
            } else break;
        }
    }
    fclose(f);
}

static bool save_anims_to_file(const char *path, const Animation *anims) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    for (int i = 0; i < ANIM_COUNT; i++) {
        fprintf(f, "%s", anim_names[i]);
        for (int j = 0; j < anims[i].count; j++) fprintf(f, " %d", anims[i].frames[j]);
        fprintf(f, "\n");
    }
    fclose(f);
    return true;
}

static bool save_fg_list(const char *path, const Walkbox *fgs, int count) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    int written = 0;
    for (int i = 0; i < count; i++) {
        if (fgs[i].n < 3) continue;
        if (written > 0) fprintf(f, "\n");
        for (int j = 0; j < fgs[i].n; j++) {
            fprintf(f, "%d %d\n", (int)fgs[i].p[j].x, (int)fgs[i].p[j].y);
        }
        written++;
    }
    fclose(f);
    return true;
}

static float signed_area(const Vector2 *pts, const int *idx, int n) {
    float a = 0;
    for (int i = 0; i < n; i++) {
        Vector2 p = pts[idx[i]], q = pts[idx[(i + 1) % n]];
        a += p.x * q.y - q.x * p.y;
    }
    return a * 0.5f;
}

static bool segments_cross(Vector2 p1, Vector2 p2, Vector2 p3, Vector2 p4) {
    float d1 = cross2(p3, p4, p1);
    float d2 = cross2(p3, p4, p2);
    float d3 = cross2(p1, p2, p3);
    float d4 = cross2(p1, p2, p4);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
           ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

static bool has_self_intersection(const Walkbox *wb) {
    if (wb->n < 4) return false;
    for (int i = 0; i < wb->n; i++) {
        Vector2 a1 = wb->p[i];
        Vector2 a2 = wb->p[(i + 1) % wb->n];
        for (int j = i + 2; j < wb->n; j++) {
            if (i == 0 && j == wb->n - 1) continue;
            Vector2 b1 = wb->p[j];
            Vector2 b2 = wb->p[(j + 1) % wb->n];
            if (segments_cross(a1, a2, b1, b2)) return true;
        }
    }
    return false;
}

static void sort_poly_by_centroid(Walkbox *wb) {
    if (wb->n < 3) return;
    Vector2 c = { 0, 0 };
    for (int i = 0; i < wb->n; i++) { c.x += wb->p[i].x; c.y += wb->p[i].y; }
    c.x /= wb->n;
    c.y /= wb->n;
    for (int i = 1; i < wb->n; i++) {
        Vector2 key = wb->p[i];
        float key_ang = atan2f(key.y - c.y, key.x - c.x);
        int j = i - 1;
        while (j >= 0 && atan2f(wb->p[j].y - c.y, wb->p[j].x - c.x) > key_ang) {
            wb->p[j + 1] = wb->p[j];
            j--;
        }
        wb->p[j + 1] = key;
    }
}

static bool point_in_tri(Vector2 p, Vector2 a, Vector2 b, Vector2 c) {
    float d1 = cross2(a, b, p);
    float d2 = cross2(b, c, p);
    float d3 = cross2(c, a, p);
    bool has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(has_neg && has_pos);
}

static int triangulate(const Walkbox *poly, int *tri_idx_out) {
    if (poly->n < 3) return 0;

    int idx[MAX_WB_VERTS];
    int n = poly->n;
    for (int i = 0; i < n; i++) idx[i] = i;

    float sign = signed_area(poly->p, idx, n) >= 0 ? 1.0f : -1.0f;

    int tri_count = 0;
    int guard = MAX_WB_VERTS * MAX_WB_VERTS;

    while (n > 3 && guard-- > 0) {
        bool found = false;
        for (int i = 0; i < n; i++) {
            int ia = idx[(i - 1 + n) % n];
            int ib = idx[i];
            int ic = idx[(i + 1) % n];
            Vector2 a = poly->p[ia], b = poly->p[ib], c = poly->p[ic];

            if (sign * cross2(a, b, c) <= 0) continue;

            bool clean = true;
            for (int j = 0; j < n; j++) {
                int ij = idx[j];
                if (ij == ia || ij == ib || ij == ic) continue;
                if (point_in_tri(poly->p[ij], a, b, c)) { clean = false; break; }
            }
            if (!clean) continue;

            tri_idx_out[tri_count * 3 + 0] = ia;
            tri_idx_out[tri_count * 3 + 1] = ib;
            tri_idx_out[tri_count * 3 + 2] = ic;
            tri_count++;

            for (int j = i; j < n - 1; j++) idx[j] = idx[j + 1];
            n--;
            found = true;
            break;
        }
        if (!found) break;
    }
    if (n == 3) {
        tri_idx_out[tri_count * 3 + 0] = idx[0];
        tri_idx_out[tri_count * 3 + 1] = idx[1];
        tri_idx_out[tri_count * 3 + 2] = idx[2];
        tri_count++;
    }
    return tri_count;
}

static void draw_fg_polygon(Texture2D bg, float draw_w, float draw_h, const Walkbox *poly, float actor_y) {
    if (poly->n < 3 || bg.id == 0) return;

    float miny = poly->p[0].y, maxy = poly->p[0].y;
    for (int i = 1; i < poly->n; i++) {
        if (poly->p[i].y < miny) miny = poly->p[i].y;
        if (poly->p[i].y > maxy) maxy = poly->p[i].y;
    }
    if (actor_y > maxy) return;
    int y_start = (int)floorf(miny);
    int y_end   = (int)ceilf(maxy);

    Rectangle src = { 0, 0, (float)bg.width, (float)bg.height };
    Rectangle dst = { 0, 0, draw_w, draw_h };

    float xs[MAX_WB_VERTS];
    for (int y = y_start; y <= y_end; y++) {
        float py = (float)y + 0.5f;
        int nx = 0;
        for (int i = 0; i < poly->n; i++) {
            Vector2 a = poly->p[i];
            Vector2 b = poly->p[(i + 1) % poly->n];
            if ((a.y > py) == (b.y > py)) continue;
            float t = (py - a.y) / (b.y - a.y);
            xs[nx++] = a.x + t * (b.x - a.x);
        }
        for (int i = 1; i < nx; i++) {
            float key = xs[i];
            int j = i - 1;
            while (j >= 0 && xs[j] > key) { xs[j + 1] = xs[j]; j--; }
            xs[j + 1] = key;
        }
        for (int i = 0; i + 1 < nx; i += 2) {
            int x0 = (int)floorf(xs[i]);
            int x1 = (int)ceilf(xs[i + 1]);
            if (x1 <= x0) continue;
            BeginScissorMode(x0, y, x1 - x0, 1);
            DrawTexturePro(bg, src, dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
            EndScissorMode();
        }
    }
}

static void draw_fg_debug_fill(const Walkbox *poly, Color color) {
    if (poly->n < 3) return;
    int tri_idx[MAX_WB_VERTS * 3];
    int tri_count = triangulate(poly, tri_idx);
    for (int t = 0; t < tri_count; t++) {
        Vector2 a = poly->p[tri_idx[t * 3 + 0]];
        Vector2 b = poly->p[tri_idx[t * 3 + 1]];
        Vector2 c = poly->p[tri_idx[t * 3 + 2]];
        DrawTriangle(a, b, c, color);
        DrawTriangle(a, c, b, color);
    }
}

typedef enum { VERB_LOOK, VERB_USE, VERB_PICKUP, VERB_COUNT } Verb;

static const char *verb_names[VERB_COUNT] = { "Look at", "Use", "Pick up" };

typedef struct {
    Rectangle rect;
    const char *name;
    const char *look_text;
    const char *use_text;
    const char *pickup_text;
    bool visible;
} Hotspot;

typedef struct {
    Vector2 pos;
    Vector2 target;
    bool moving;
    int facing;
    float anim_timer;
    int anim_frame;
    Verb pending_verb;
    Hotspot *pending_hotspot;
    Vector2 waypoints[MAX_WB_VERTS + 2];
    int waypoint_count;
    int current_waypoint;
} Actor;

static void update_actor_facing(Actor *a) {
    float dx = a->target.x - a->pos.x;
    float dy = a->target.y - a->pos.y;
    if (fabsf(dx) > fabsf(dy)) {
        a->facing = dx > 0 ? DIR_RIGHT : DIR_LEFT;
    } else if (fabsf(dy) > 0.01f) {
        a->facing = dy > 0 ? DIR_DOWN : DIR_UP;
    }
}

static void draw_wrapped_text(const char *text, int x, int y, int max_w, int font_size, Color color) {
    char buf[512];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int line_y = y;
    char *line_start = buf;
    char *last_space = NULL;
    for (char *p = buf; *p; p++) {
        if (*p == ' ') last_space = p;
        char save = *(p + 1);
        *(p + 1) = '\0';
        int w = MeasureText(line_start, font_size);
        *(p + 1) = save;
        if (w > max_w && last_space && last_space > line_start) {
            *last_space = '\0';
            DrawText(line_start, x, line_y, font_size, color);
            line_y += font_size + 4;
            line_start = last_space + 1;
            last_space = NULL;
        }
    }
    if (*line_start) DrawText(line_start, x, line_y, font_size, color);
}

int main(void) {
    InitWindow(SCREEN_W, SCREEN_H, "scumm-game");
    SetTargetFPS(60);

    { FILE *pf = fopen(PLAYER_POS_PATH, "w"); if (pf) fclose(pf); }

    Texture2D bg = LoadTexture("assets/bg-dock.png");

    Walkbox dock = {
        .p = {
            { 260, 320 },
            { 680, 320 },
            { 940, 540 },
            {  20, 540 },
        },
        .n = 4,
    };
    load_walkbox(WALKBOX_PATH, &dock);

    Walkbox fgs[MAX_FG_POLYS] = { 0 };
    int fg_count = load_fg_list(FG_PATH, fgs, MAX_FG_POLYS);

    Texture2D sprites[MAX_SPRITES];
    int sprite_count = load_sprites_from_dir(SPRITES_DIR, sprites, MAX_SPRITES);
    Animation anims[ANIM_COUNT];
    load_anims_from_file(SPRITES_META_PATH, anims);

    AnimTextures actor_anims[ANIM_COUNT];
    load_actor_anims(ACTOR_SPRITES_DIR, actor_anims);

    Actor actor = {
        .pos = { SCREEN_W / 2.0f, PLAY_H - 80 },
        .target = { SCREEN_W / 2.0f, PLAY_H - 80 },
        .moving = false,
        .facing = DIR_DOWN,
        .anim_timer = 0,
        .anim_frame = 0,
        .pending_verb = VERB_LOOK,
        .pending_hotspot = NULL,
    };
    actor.pos = clamp_to_walkbox(actor.pos, &dock);
    actor.target = actor.pos;

    Verb selected_verb = VERB_LOOK;
    char message[256] = "";
    float message_timer = 0.0f;
    char status_line[128] = "";
    bool debug_overlay = false;
    bool edit_mode = false;
    Camera2D edit_cam = { .target = { 0, 0 }, .offset = { 0, 0 }, .rotation = 0.0f, .zoom = 1.0f };
    bool editing_fg = false;
    int editing_fg_idx = 0;
    int dragging_vert = -1;
    float save_flash = 0.0f;
    float pos_log_timer = 0.0f;
    Vector2 last_logged_pos = { -9999, -9999 };
    bool browser_mode = false;
    int browser_idx = 0;

    Rectangle verb_rects[VERB_COUNT];
    int verb_w = 180, verb_h = 40, verb_pad = 12;
    int verb_x = 20, verb_y = PLAY_H + 20;
    for (int i = 0; i < VERB_COUNT; i++) {
        verb_rects[i] = (Rectangle){ verb_x, verb_y + i * (verb_h + verb_pad), verb_w, verb_h };
    }

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        Vector2 mouse_screen = GetMousePosition();
        Vector2 mouse = mouse_screen;
        if (edit_mode) {
            float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) {
                Vector2 before = GetScreenToWorld2D(mouse_screen, edit_cam);
                edit_cam.zoom *= (wheel > 0) ? 1.15f : 1.0f / 1.15f;
                if (edit_cam.zoom < 1.0f) edit_cam.zoom = 1.0f;
                if (edit_cam.zoom > 8.0f) edit_cam.zoom = 8.0f;
                Vector2 after = GetScreenToWorld2D(mouse_screen, edit_cam);
                edit_cam.target.x += (before.x - after.x);
                edit_cam.target.y += (before.y - after.y);
            }
            if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
                Vector2 d = GetMouseDelta();
                edit_cam.target.x -= d.x / edit_cam.zoom;
                edit_cam.target.y -= d.y / edit_cam.zoom;
            }
            if (IsKeyPressed(KEY_Z)) {
                edit_cam.zoom = 1.0f;
                edit_cam.target = (Vector2){ 0, 0 };
                edit_cam.offset = (Vector2){ 0, 0 };
            }
            mouse = GetScreenToWorld2D(mouse_screen, edit_cam);
        }

        if (IsKeyPressed(KEY_B) && !edit_mode) {
            browser_mode = !browser_mode;
            if (!browser_mode) {
                if (save_anims_to_file(SPRITES_META_PATH, anims)) save_flash = 1.5f;
            }
        }

        if (browser_mode) {
            if (sprite_count > 0) {
                if (IsKeyPressed(KEY_LEFT))  browser_idx = (browser_idx - 1 + sprite_count) % sprite_count;
                if (IsKeyPressed(KEY_RIGHT)) browser_idx = (browser_idx + 1) % sprite_count;
                if (IsKeyPressed(KEY_UP))    browser_idx = (browser_idx - 10 + sprite_count) % sprite_count;
                if (IsKeyPressed(KEY_DOWN))  browser_idx = (browser_idx + 10) % sprite_count;
                if (IsKeyPressed(KEY_HOME))  browser_idx = 0;
                if (IsKeyPressed(KEY_END))   browser_idx = sprite_count - 1;

                int assign = -1;
                if (IsKeyPressed(KEY_ZERO))  assign = 0;
                if (IsKeyPressed(KEY_ONE))   assign = 1;
                if (IsKeyPressed(KEY_TWO))   assign = 2;
                if (IsKeyPressed(KEY_THREE)) assign = 3;
                if (IsKeyPressed(KEY_FOUR))  assign = 4;
                if (IsKeyPressed(KEY_FIVE))  assign = 5;
                if (IsKeyPressed(KEY_SIX))   assign = 6;
                if (IsKeyPressed(KEY_SEVEN)) assign = 7;
                if (IsKeyPressed(KEY_EIGHT)) assign = 8;
                if (assign >= 0) {
                    int sprite_num = browser_idx + 1;
                    int pos = -1;
                    for (int j = 0; j < anims[assign].count; j++) {
                        if (anims[assign].frames[j] == sprite_num) { pos = j; break; }
                    }
                    if (pos >= 0) {
                        for (int j = pos; j < anims[assign].count - 1; j++) {
                            anims[assign].frames[j] = anims[assign].frames[j + 1];
                        }
                        anims[assign].count--;
                    } else if (anims[assign].count < MAX_ANIM_FRAMES) {
                        anims[assign].frames[anims[assign].count++] = sprite_num;
                    }
                }

                if (IsKeyPressed(KEY_S)) {
                    if (save_anims_to_file(SPRITES_META_PATH, anims)) save_flash = 1.5f;
                }
            }
            if (save_flash > 0) save_flash -= dt;

            BeginDrawing();
            ClearBackground((Color){ 25, 25, 30, 255 });

            if (sprite_count == 0) {
                DrawText("no sprites found in " SPRITES_DIR, 20, 20, 22, RED);
                DrawText("[B] exit", 20, 60, 16, WHITE);
            } else {
                Texture2D *tex = &sprites[browser_idx];
                char hdr[128];
                snprintf(hdr, sizeof(hdr), "SPRITE %d / %d   %dx%d",
                         browser_idx + 1, sprite_count, tex->width, tex->height);
                DrawText(hdr, 20, 20, 28, WHITE);

                float scale = 5.0f;
                float dx = (SCREEN_W - tex->width * scale) / 2;
                float dy = 70;
                DrawRectangle((int)dx - 10, (int)dy - 10,
                              (int)(tex->width * scale) + 20, (int)(tex->height * scale) + 20,
                              (Color){ 50, 50, 60, 255 });
                SetTextureFilter(*tex, TEXTURE_FILTER_POINT);
                DrawTextureEx(*tex, (Vector2){ dx, dy }, 0, scale, WHITE);

                DrawText("[<-/->] prev/next   [Up/Down] +/-10   [Home/End] first/last   [S] save   [B] exit",
                         20, SCREEN_H - 210, 14, (Color){ 180, 180, 200, 255 });
                DrawText("[0] idle   [1-4] walk down/up/left/right   [5-8] face down/up/left/right   (toggle)",
                         20, SCREEN_H - 190, 14, (Color){ 180, 180, 200, 255 });

                int col_w = (SCREEN_W - 40) / 2;
                int pair_order[5][2] = {
                    { ANIM_IDLE,       -1 },
                    { ANIM_WALK_DOWN,  ANIM_FACE_DOWN },
                    { ANIM_WALK_UP,    ANIM_FACE_UP },
                    { ANIM_WALK_LEFT,  ANIM_FACE_LEFT },
                    { ANIM_WALK_RIGHT, ANIM_FACE_RIGHT },
                };
                int y = SCREEN_H - 160;
                for (int r = 0; r < 5; r++) {
                    for (int c = 0; c < 2; c++) {
                        int ai = pair_order[r][c];
                        if (ai < 0) continue;
                        int x = 20 + c * col_w;
                        char line[512];
                        int off = snprintf(line, sizeof(line), "[%d] %-10s (%d):", ai, anim_names[ai], anims[ai].count);
                        int budget = col_w - 12;
                        for (int j = 0; j < anims[ai].count && off < (int)sizeof(line) - 8; j++) {
                            int add = snprintf(line + off, sizeof(line) - off, " %d", anims[ai].frames[j]);
                            if (MeasureText(line, 16) > budget) {
                                line[off] = '\0';
                                snprintf(line + off, sizeof(line) - off, " ...");
                                break;
                            }
                            off += add;
                        }
                        bool in_this = false;
                        for (int j = 0; j < anims[ai].count; j++) {
                            if (anims[ai].frames[j] == browser_idx + 1) { in_this = true; break; }
                        }
                        DrawText(line, x, y, 16, in_this ? YELLOW : WHITE);
                    }
                    y += 22;
                }
            }
            if (save_flash > 0) {
                DrawText("saved sprites.txt", SCREEN_W - 220, 24, 16,
                         (Color){ 120, 255, 160, (unsigned char)(255 * (save_flash / 1.5f)) });
            }
            EndDrawing();
            continue;
        }

        if (IsKeyPressed(KEY_D)) debug_overlay = !debug_overlay;
        if (IsKeyPressed(KEY_E)) {
            edit_mode = !edit_mode;
            dragging_vert = -1;
            edit_cam.zoom = 1.0f;
            edit_cam.target = (Vector2){ 0, 0 };
            edit_cam.offset = (Vector2){ 0, 0 };
            if (!edit_mode) {
                bool ok_wb = (dock.n >= 3) ? save_walkbox(WALKBOX_PATH, &dock) : false;
                bool ok_fg = save_fg_list(FG_PATH, fgs, fg_count);
                if (ok_wb || ok_fg) save_flash = 1.5f;
                actor.pos = clamp_to_walkbox(actor.pos, &dock);
                actor.target = actor.pos;
                actor.moving = false;
            }
        }
        if (edit_mode && IsKeyPressed(KEY_W)) { editing_fg = false; dragging_vert = -1; }
        if (edit_mode && IsKeyPressed(KEY_F)) {
            dragging_vert = -1;
            if (!editing_fg) {
                editing_fg = true;
                if (fg_count == 0) { fg_count = 1; fgs[0].n = 0; }
                if (editing_fg_idx >= fg_count) editing_fg_idx = 0;
            } else if (fg_count > 1) {
                editing_fg_idx = (editing_fg_idx + 1) % fg_count;
            }
        }
        if (edit_mode && editing_fg && IsKeyPressed(KEY_N) && fg_count < MAX_FG_POLYS) {
            fgs[fg_count].n = 0;
            editing_fg_idx = fg_count;
            fg_count++;
            dragging_vert = -1;
        }
        if (edit_mode && editing_fg && IsKeyPressed(KEY_BACKSPACE) && fg_count > 0) {
            for (int i = editing_fg_idx; i < fg_count - 1; i++) fgs[i] = fgs[i + 1];
            fg_count--;
            if (fg_count == 0) { fg_count = 1; fgs[0].n = 0; editing_fg_idx = 0; }
            else if (editing_fg_idx >= fg_count) editing_fg_idx = fg_count - 1;
            dragging_vert = -1;
        }

        Hotspot *hover = NULL;

        if (hover) {
            snprintf(status_line, sizeof(status_line), "%s %s", verb_names[selected_verb], hover->name);
        } else {
            snprintf(status_line, sizeof(status_line), "%s", verb_names[selected_verb]);
        }

        if (edit_mode) {
            Walkbox *active = editing_fg ? &fgs[editing_fg_idx] : &dock;
            if (IsKeyPressed(KEY_R)) { active->n = 0; dragging_vert = -1; }
            if (IsKeyPressed(KEY_O)) { sort_poly_by_centroid(active); dragging_vert = -1; }
            if (IsKeyPressed(KEY_S)) {
                bool ok = editing_fg ? save_fg_list(FG_PATH, fgs, fg_count)
                                     : (active->n >= 3 && save_walkbox(WALKBOX_PATH, active));
                if (ok) save_flash = 1.5f;
            }
            if (mouse_screen.y < PLAY_H) {
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    int near_v = nearest_vertex(mouse, active, VERT_GRAB_RADIUS);
                    if (near_v >= 0) {
                        dragging_vert = near_v;
                    } else {
                        Vector2 proj;
                        int near_e = nearest_edge(mouse, active, EDGE_GRAB_RADIUS, &proj);
                        if (near_e >= 0) {
                            int inserted = insert_vertex(active, near_e, proj);
                            dragging_vert = inserted;
                        } else if (active->n < MAX_WB_VERTS) {
                            active->p[active->n++] = mouse;
                        }
                    }
                }
                if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && active->n > 0) {
                    active->n--;
                    if (dragging_vert >= active->n) dragging_vert = -1;
                }
            }
            if (dragging_vert >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                active->p[dragging_vert] = mouse;
            }
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) dragging_vert = -1;
        } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            bool clicked_verb = false;
            for (int i = 0; i < VERB_COUNT; i++) {
                if (CheckCollisionPointRec(mouse, verb_rects[i])) {
                    selected_verb = (Verb)i;
                    clicked_verb = true;
                    break;
                }
            }

            if (!clicked_verb && mouse.y < PLAY_H) {
                actor.pos = clamp_to_walkbox(actor.pos, &dock);
                Vector2 final_target = clamp_to_walkbox(mouse, &dock);
                int nw = find_path(actor.pos, final_target, &dock,
                                   actor.waypoints, MAX_WB_VERTS + 2);
                actor.pending_hotspot = hover;
                actor.pending_verb = selected_verb;
                if (nw >= 2) {
                    actor.waypoint_count = nw;
                    actor.current_waypoint = 1;
                    actor.target = actor.waypoints[1];
                    actor.moving = true;
                    update_actor_facing(&actor);
                    actor.anim_frame = 0;
                    actor.anim_timer = 0;
                } else {
                    actor.moving = false;
                    actor.target = actor.pos;
                    actor.waypoint_count = 0;
                }
            }
        }

        if (save_flash > 0) save_flash -= dt;

        pos_log_timer += dt;
        if (pos_log_timer >= PLAYER_POS_INTERVAL) {
            pos_log_timer = 0;
            if ((int)actor.pos.x != (int)last_logged_pos.x ||
                (int)actor.pos.y != (int)last_logged_pos.y) {
                FILE *pf = fopen(PLAYER_POS_PATH, "w");
                if (pf) {
                    fprintf(pf, "%d %d\n", (int)actor.pos.x, (int)actor.pos.y);
                    fclose(pf);
                    last_logged_pos = actor.pos;
                }
            }
        }

        if (actor.moving && !edit_mode) {
            Vector2 d = { actor.target.x - actor.pos.x, actor.target.y - actor.pos.y };
            float dist = sqrtf(d.x * d.x + d.y * d.y);
            float step = ACTOR_SPEED * dt;
            if (dist <= step) {
                actor.pos = actor.target;
                actor.current_waypoint++;
                if (actor.current_waypoint < actor.waypoint_count) {
                    actor.target = actor.waypoints[actor.current_waypoint];
                    update_actor_facing(&actor);
                } else {
                    actor.moving = false;
                    actor.waypoint_count = 0;

                    if (actor.pending_hotspot) {
                        Hotspot *h = actor.pending_hotspot;
                        const char *txt = NULL;
                        switch (actor.pending_verb) {
                            case VERB_LOOK:   txt = h->look_text; break;
                            case VERB_USE:    txt = h->use_text; break;
                            case VERB_PICKUP: txt = h->pickup_text; break;
                            default: break;
                        }
                        if (txt) {
                            strncpy(message, txt, sizeof(message) - 1);
                            message[sizeof(message) - 1] = '\0';
                            message_timer = MESSAGE_TIME;
                        }
                        actor.pending_hotspot = NULL;
                    }
                }
            } else {
                actor.pos.x += d.x / dist * step;
                actor.pos.y += d.y / dist * step;
            }
        } else if (!edit_mode && !browser_mode) {
            Vector2 c = clamp_to_walkbox(actor.pos, &dock);
            if (c.x != actor.pos.x || c.y != actor.pos.y) {
                actor.pos = c;
                actor.target = c;
            }
        }

        if (message_timer > 0) {
            message_timer -= dt;
            if (message_timer <= 0) message[0] = '\0';
        }

        BeginDrawing();
        ClearBackground((Color){ 20, 22, 30, 255 });

        if (edit_mode) BeginMode2D(edit_cam);

        if (bg.id != 0) {
            Rectangle src = { 0, 0, (float)bg.width, (float)bg.height };
            Rectangle dst = { 0, 0, SCREEN_W, PLAY_H };
            DrawTexturePro(bg, src, dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
        } else {
            DrawRectangle(0, 0, SCREEN_W, PLAY_H, (Color){ 40, 50, 80, 255 });
            DrawText("(missing assets/bg-dock.png)", 20, 20, 20, RED);
        }

        float s = actor_scale_at(actor.pos, &dock);
        int ax = (int)actor.pos.x, ay = (int)actor.pos.y;

        int want_anim;
        if (actor.moving) {
            switch (actor.facing) {
                default:
                case DIR_DOWN:  want_anim = ANIM_WALK_DOWN; break;
                case DIR_UP:    want_anim = ANIM_WALK_UP; break;
                case DIR_LEFT:  want_anim = ANIM_WALK_LEFT; break;
                case DIR_RIGHT: want_anim = ANIM_WALK_RIGHT; break;
            }
        } else {
            switch (actor.facing) {
                default:
                case DIR_DOWN:  want_anim = ANIM_FACE_DOWN; break;
                case DIR_UP:    want_anim = ANIM_FACE_UP; break;
                case DIR_LEFT:  want_anim = ANIM_FACE_LEFT; break;
                case DIR_RIGHT: want_anim = ANIM_FACE_RIGHT; break;
            }
        }
        int play_anim = resolve_anim(want_anim, actor_anims);
        AnimTextures *ta = play_anim >= 0 ? &actor_anims[play_anim] : NULL;

        if (ta && ta->count > 0) {
            if (actor.moving) {
                actor.anim_timer += dt;
                float step = 1.0f / ACTOR_ANIM_FPS;
                while (actor.anim_timer >= step) {
                    actor.anim_timer -= step;
                    actor.anim_frame = (actor.anim_frame + 1) % ta->count;
                }
            } else {
                actor.anim_frame = 0;
                actor.anim_timer = 0;
            }
            if (actor.anim_frame >= ta->count) actor.anim_frame = 0;
            Texture2D *tex = &ta->frames[actor.anim_frame];
            float w = tex->width * s * ACTOR_SPRITE_SCALE;
            float h = tex->height * s * ACTOR_SPRITE_SCALE;
            Rectangle src = { 0, 0, (float)tex->width, (float)tex->height };
            Rectangle dst = { actor.pos.x - w * 0.5f, actor.pos.y - h, w, h };
            DrawTexturePro(*tex, src, dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
        } else {
            DrawCircle(ax, ay - (int)(30 * s), (int)(10 * s), (Color){ 240, 210, 180, 255 });
            DrawRectangle(ax - (int)(8 * s), ay - (int)(20 * s), (int)(16 * s), (int)(25 * s), (Color){ 180, 60, 60, 255 });
            DrawRectangle(ax - (int)(6 * s), ay + (int)(5 * s), (int)(5 * s), (int)(20 * s), (Color){ 40, 40, 90, 255 });
            DrawRectangle(ax + (int)(1 * s), ay + (int)(5 * s), (int)(5 * s), (int)(20 * s), (Color){ 40, 40, 90, 255 });
        }

        if (!edit_mode) {
            for (int i = 0; i < fg_count; i++) {
                draw_fg_polygon(bg, (float)SCREEN_W, (float)PLAY_H, &fgs[i], actor.pos.y);
            }
        }

        if (debug_overlay) {
            for (int i = 0; i < fg_count; i++) {
                draw_fg_debug_fill(&fgs[i], (Color){ 255, 0, 255, 90 });
            }
        }

        if (debug_overlay || edit_mode) {
            Color wb_edge = { 0, 220, 255, 220 };
            Color wb_vert = { 0, 220, 255, 255 };
            Color fg_edge = { 255, 100, 220, 220 };
            Color fg_vert = { 255, 100, 220, 255 };
            bool wb_active = edit_mode && !editing_fg;
            Color we = wb_edge, wv = wb_vert;
            if (edit_mode && !wb_active) { we.a = 100; wv.a = 140; }
            for (int i = 0; i < dock.n; i++) {
                Vector2 a = dock.p[i];
                if (dock.n >= 2) {
                    Vector2 b = dock.p[(i + 1) % dock.n];
                    if (dock.n >= 3 || i + 1 < dock.n) DrawLineEx(a, b, 2.0f, we);
                }
                float r = (wb_active && i == dragging_vert) ? 7.0f : 5.0f;
                DrawCircleV(a, r, wv);
                if (wb_active || !edit_mode) {
                    char lbl[8]; snprintf(lbl, sizeof(lbl), "%d", i);
                    DrawText(lbl, (int)a.x + 8, (int)a.y - 8, 14, wv);
                }
            }
            for (int pi = 0; pi < fg_count; pi++) {
                const Walkbox *wb = &fgs[pi];
                bool is_active = edit_mode && editing_fg && pi == editing_fg_idx;
                Color ec = fg_edge, vc = fg_vert;
                if (edit_mode && !is_active) { ec.a = 80; vc.a = 120; }
                for (int i = 0; i < wb->n; i++) {
                    Vector2 a = wb->p[i];
                    if (wb->n >= 2) {
                        Vector2 b = wb->p[(i + 1) % wb->n];
                        if (wb->n >= 3 || i + 1 < wb->n) DrawLineEx(a, b, 2.0f, ec);
                    }
                    float r = (is_active && i == dragging_vert) ? 7.0f : 5.0f;
                    DrawCircleV(a, r, vc);
                    if (is_active || !edit_mode) {
                        char lbl[8]; snprintf(lbl, sizeof(lbl), "%d", i);
                        DrawText(lbl, (int)a.x + 8, (int)a.y - 8, 14, vc);
                    }
                }
            }
            if (!edit_mode) DrawCircleV(actor.target, 4, RED);
            if (edit_mode && dragging_vert < 0 && mouse.y < PLAY_H) {
                Walkbox *active = editing_fg ? &fgs[editing_fg_idx] : &dock;
                int near_v = nearest_vertex(mouse, active, VERT_GRAB_RADIUS);
                if (near_v >= 0) {
                    DrawCircleLines((int)active->p[near_v].x, (int)active->p[near_v].y, 10, YELLOW);
                } else {
                    Vector2 proj;
                    int near_e = nearest_edge(mouse, active, EDGE_GRAB_RADIUS, &proj);
                    if (near_e >= 0) {
                        DrawCircleV(proj, 5, (Color){ 255, 180, 0, 220 });
                        DrawText("+", (int)proj.x + 8, (int)proj.y - 22, 16, (Color){ 255, 180, 0, 255 });
                    }
                }
            }
        }

        if (message[0]) {
            int msg_y = (int)actor.pos.y - 70;
            if (msg_y < 20) msg_y = 20;
            draw_wrapped_text(message, 20, msg_y, SCREEN_W - 40, 20, WHITE);
        }

        if (edit_mode) EndMode2D();

        DrawRectangle(0, PLAY_H, SCREEN_W, VERB_BAR_H, (Color){ 15, 15, 20, 255 });
        DrawLine(0, PLAY_H, SCREEN_W, PLAY_H, (Color){ 80, 80, 100, 255 });

        for (int i = 0; i < VERB_COUNT; i++) {
            bool is_selected = (selected_verb == (Verb)i);
            bool is_hover = CheckCollisionPointRec(mouse, verb_rects[i]);
            Color bg = is_selected ? (Color){ 80, 70, 40, 255 } :
                       is_hover    ? (Color){ 50, 50, 60, 255 } :
                                     (Color){ 30, 30, 40, 255 };
            DrawRectangleRec(verb_rects[i], bg);
            DrawRectangleLinesEx(verb_rects[i], 1, (Color){ 120, 120, 140, 255 });
            int tw = MeasureText(verb_names[i], 20);
            DrawText(verb_names[i],
                     (int)(verb_rects[i].x + (verb_rects[i].width - tw) / 2),
                     (int)(verb_rects[i].y + 10),
                     20,
                     is_selected ? YELLOW : WHITE);
        }

        if (edit_mode) {
            char edit_status[160];
            if (editing_fg) {
                snprintf(edit_status, sizeof(edit_status), "EDIT FOREGROUND #%d/%d  verts: %d",
                         editing_fg_idx + 1, fg_count, fgs[editing_fg_idx].n);
            } else {
                snprintf(edit_status, sizeof(edit_status), "EDIT WALKBOX  verts: %d", dock.n);
            }
            Color head_col = editing_fg ? (Color){ 255, 180, 230, 255 } : (Color){ 255, 220, 100, 255 };
            DrawText(edit_status, 230, PLAY_H + 30, 22, head_col);
            DrawText("[W] walkbox  [F] fg/cycle  [N] new fg  [Bksp] delete  [O] auto-order  [R] reset  [S] save  [E] exit",
                     230, PLAY_H + 70, 14, (Color){ 180, 180, 200, 255 });
            Walkbox *active_warn = editing_fg ? &fgs[editing_fg_idx] : &dock;
            if (has_self_intersection(active_warn)) {
                DrawText("(!) edges cross -- press [O] to auto-order",
                         230, PLAY_H + 55, 14, (Color){ 255, 130, 130, 255 });
            }
        } else {
            DrawText(status_line, 230, PLAY_H + 30, 22, (Color){ 200, 200, 220, 255 });
            DrawText("Click a verb, click an object, click floor to walk.  [D] overlay  [E] edit walkbox  [B] sprite browser",
                     230, PLAY_H + 70, 14, (Color){ 140, 140, 160, 255 });
        }
        if (save_flash > 0) {
            DrawText("saved", SCREEN_W - 100, PLAY_H + 20, 16,
                     (Color){ 120, 255, 160, (unsigned char)(255 * (save_flash / 1.5f)) });
        }

        EndDrawing();
    }

    UnloadTexture(bg);
    for (int i = 0; i < sprite_count; i++) UnloadTexture(sprites[i]);
    unload_actor_anims(actor_anims);
    CloseWindow();
    return 0;
}
