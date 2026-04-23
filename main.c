#include "raylib.h"
#include "rlgl.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(void, web_save_text, (const char *key, const char *content), {
    try { localStorage.setItem(UTF8ToString(key), UTF8ToString(content)); } catch (e) {}
});

EM_JS(char *, web_load_text, (const char *key), {
    try {
        var v = localStorage.getItem(UTF8ToString(key));
        if (v === null) return 0;
        var n = lengthBytesUTF8(v) + 1;
        var p = _malloc(n);
        stringToUTF8(v, p, n);
        return p;
    } catch (e) { return 0; }
});

static void web_restore_file(const char *path) {
    char *stored = web_load_text(path);
    if (!stored) return;
    FILE *f = fopen(path, "w");
    if (f) { fputs(stored, f); fclose(f); }
    free(stored);
}

static void web_persist_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, len, f);
    buf[n] = 0;
    fclose(f);
    web_save_text(path, buf);
    free(buf);
}
#else
#define web_restore_file(p) ((void)0)
#define web_persist_file(p) ((void)0)
#endif

#define SCREEN_W 960
#define SCREEN_H 660
#define VERB_BAR_H 120
#define PLAY_H (SCREEN_H - VERB_BAR_H)
#define ACTOR_SPEED 140.0f
#define MESSAGE_TIME 3.0f
#define MAX_WB_VERTS 128
#define MAX_WB_POLYS 8
#define MAX_FG_POLYS 16
#define VERT_GRAB_RADIUS 10.0f
#define EDGE_GRAB_RADIUS 12.0f
#define GAMES_DIR "games"
#define DEFAULT_GAME "monkey1"
#define DEFAULT_ACTOR "guybrush"
#define MAX_GAMES 64
#define PLAYER_POS_PATH "player.txt"
#define PLAYER_POS_INTERVAL 0.1f
#define MAX_SPRITES 300
#define MAX_ANIM_FRAMES 32
#define ANIM_COUNT 9
#define SPRITES_DIR "guybrush_sprites_v3"
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

static int containing_poly(Vector2 p, const Walkbox *polys, int count) {
    for (int i = 0; i < count; i++) {
        if (point_in_polygon(p, &polys[i])) return i;
    }
    return -1;
}

static bool is_walkable(Vector2 p, const Walkbox *polys, int poly_count,
                        const Walkbox *holes, int hole_count) {
    if (containing_poly(p, polys, poly_count) < 0) return false;
    if (hole_count > 0 && containing_poly(p, holes, hole_count) >= 0) return false;
    return true;
}

static bool segment_clear_multi(Vector2 a, Vector2 b,
                                const Walkbox *polys, int poly_count,
                                const Walkbox *holes, int hole_count) {
    if (poly_count <= 0) return false;
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) return true;
    int steps = (int)(len / 2.0f);
    if (steps < 4) steps = 4;
    for (int i = 1; i < steps; i++) {
        float t = (float)i / (float)steps;
        Vector2 p = { a.x + dx * t, a.y + dy * t };
        if (!is_walkable(p, polys, poly_count, holes, hole_count)) return false;
    }
    return true;
}

static Vector2 clamp_to_walkable(Vector2 p,
                                 const Walkbox *polys, int poly_count,
                                 const Walkbox *holes, int hole_count) {
    if (poly_count <= 0) return p;
    if (is_walkable(p, polys, poly_count, holes, hole_count)) return p;
    Vector2 best = p;
    float best_d2 = INFINITY;
    for (int k = 0; k < poly_count; k++) {
        const Walkbox *wb = &polys[k];
        if (wb->n < 2) continue;
        for (int i = 0; i < wb->n; i++) {
            Vector2 c = closest_on_segment(wb->p[i], wb->p[(i + 1) % wb->n], p);
            float ddx = c.x - p.x, ddy = c.y - p.y;
            float d2 = ddx * ddx + ddy * ddy;
            if (d2 < best_d2) { best_d2 = d2; best = c; }
        }
    }
    for (int k = 0; k < hole_count; k++) {
        const Walkbox *h = &holes[k];
        if (h->n < 2) continue;
        for (int i = 0; i < h->n; i++) {
            Vector2 c = closest_on_segment(h->p[i], h->p[(i + 1) % h->n], p);
            float ddx = c.x - p.x, ddy = c.y - p.y;
            float d2 = ddx * ddx + ddy * ddy;
            if (d2 < best_d2) { best_d2 = d2; best = c; }
        }
    }
    return best_d2 == INFINITY ? p : best;
}

typedef struct {
    float y_top, s_top;
    float y_bot, s_bot;
} ScaleConfig;

static ScaleConfig default_scale_for_walkboxes(const Walkbox *polys, int count) {
    float y_top = INFINITY, y_bot = -INFINITY;
    for (int k = 0; k < count; k++) {
        for (int i = 0; i < polys[k].n; i++) {
            if (polys[k].p[i].y < y_top) y_top = polys[k].p[i].y;
            if (polys[k].p[i].y > y_bot) y_bot = polys[k].p[i].y;
        }
    }
    if (y_top == INFINITY) { y_top = 0.0f; y_bot = (float)PLAY_H; }
    return (ScaleConfig){ .y_top = y_top, .s_top = 0.2f, .y_bot = y_bot, .s_bot = 1.0f };
}

static bool load_scale(const char *path, ScaleConfig *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    float yt, st, yb, sb;
    int ok = fscanf(f, "%f %f %f %f", &yt, &st, &yb, &sb);
    fclose(f);
    if (ok != 4) return false;
    cfg->y_top = yt; cfg->s_top = st; cfg->y_bot = yb; cfg->s_bot = sb;
    return true;
}

static bool save_scale(const char *path, const ScaleConfig *cfg) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "%.2f %.4f\n%.2f %.4f\n", cfg->y_top, cfg->s_top, cfg->y_bot, cfg->s_bot);
    fclose(f);
    web_persist_file(path);
    return true;
}

static float actor_scale_at_y(float y, const ScaleConfig *cfg) {
    float dy = cfg->y_bot - cfg->y_top;
    float t = (fabsf(dy) < 0.001f) ? 0.5f : (y - cfg->y_top) / dy;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return cfg->s_top + (cfg->s_bot - cfg->s_top) * t;
}

static int find_path_multi(Vector2 start, Vector2 end,
                           const Walkbox *polys, int poly_count,
                           const Walkbox *holes, int hole_count,
                           Vector2 *out, int max_out) {
    if (max_out < 2) return 0;
    if (poly_count <= 0) { out[0] = start; out[1] = end; return 2; }
    if (segment_clear_multi(start, end, polys, poly_count, holes, hole_count)) {
        out[0] = start;
        out[1] = end;
        return 2;
    }

    Vector2 nodes[MAX_WB_VERTS + 2];
    int poly_of[MAX_WB_VERTS + 2];
    int vert_of[MAX_WB_VERTS + 2];
    int kind_of[MAX_WB_VERTS + 2]; // 0 = walkbox, 1 = hole
    int n = 0;
    for (int k = 0; k < poly_count && n < MAX_WB_VERTS; k++) {
        for (int i = 0; i < polys[k].n && n < MAX_WB_VERTS; i++) {
            nodes[n] = polys[k].p[i];
            poly_of[n] = k;
            vert_of[n] = i;
            kind_of[n] = 0;
            n++;
        }
    }
    for (int k = 0; k < hole_count && n < MAX_WB_VERTS; k++) {
        for (int i = 0; i < holes[k].n && n < MAX_WB_VERTS; i++) {
            nodes[n] = holes[k].p[i];
            poly_of[n] = k;
            vert_of[n] = i;
            kind_of[n] = 1;
            n++;
        }
    }
    int SI = n;
    int EI = n + 1;
    int N = n + 2;
    nodes[SI] = start; poly_of[SI] = -1; vert_of[SI] = -1; kind_of[SI] = -1;
    nodes[EI] = end;   poly_of[EI] = -1; vert_of[EI] = -1; kind_of[EI] = -1;

    static float W[MAX_WB_VERTS + 2][MAX_WB_VERTS + 2];
    for (int i = 0; i < N; i++) {
        W[i][i] = 0;
        for (int j = i + 1; j < N; j++) {
            bool adj = false;
            if (i < n && j < n && kind_of[i] == kind_of[j] && poly_of[i] == poly_of[j]) {
                int ring = (kind_of[i] == 0) ? polys[poly_of[i]].n : holes[poly_of[i]].n;
                int vi = vert_of[i], vj = vert_of[j];
                if (vj == (vi + 1) % ring || vi == (vj + 1) % ring) adj = true;
            }
            bool clear = adj || segment_clear_multi(nodes[i], nodes[j], polys, poly_count, holes, hole_count);
            if (clear) {
                float ddx = nodes[i].x - nodes[j].x;
                float ddy = nodes[i].y - nodes[j].y;
                W[i][j] = W[j][i] = sqrtf(ddx * ddx + ddy * ddy);
            } else {
                W[i][j] = W[j][i] = INFINITY;
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
    web_persist_file(path);
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
    web_persist_file(path);
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

typedef struct {
    char name[64];
    char actor_name[64];

    Walkbox docks[MAX_WB_POLYS];  int dock_count;
    Walkbox fgs[MAX_FG_POLYS];    int fg_count;
    Walkbox holes[MAX_WB_POLYS];  int hole_count;
    ScaleConfig scale_cfg;

    Texture2D bg;
    AnimTextures actor_anims[ANIM_COUNT];

    char walkbox_path[160];
    char fg_path[160];
    char hole_path[160];
    char scale_path[160];
    char bg_path[160];
    char actor_base[160];
    char actor_meta_path[160];
    char actor_decl_path[160];
} Game;

static void game_read_actor_decl(const char *game_name, char *out, size_t cap) {
    char path[160];
    snprintf(path, sizeof(path), GAMES_DIR "/%s/actor.txt", game_name);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(out, cap, "%s", DEFAULT_ACTOR); return; }
    char buf[64] = { 0 };
    if (!fgets(buf, sizeof(buf), f)) { snprintf(out, cap, "%s", DEFAULT_ACTOR); fclose(f); return; }
    fclose(f);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = 0;
    if (n == 0) snprintf(out, cap, "%s", DEFAULT_ACTOR);
    else        snprintf(out, cap, "%s", buf);
}

static void game_build_paths(Game *g) {
    snprintf(g->walkbox_path,    sizeof(g->walkbox_path),    GAMES_DIR "/%s/walkbox.txt", g->name);
    snprintf(g->fg_path,         sizeof(g->fg_path),         GAMES_DIR "/%s/fg.txt",      g->name);
    snprintf(g->hole_path,       sizeof(g->hole_path),       GAMES_DIR "/%s/holes.txt",   g->name);
    snprintf(g->scale_path,      sizeof(g->scale_path),      GAMES_DIR "/%s/scale.txt",   g->name);
    snprintf(g->bg_path,         sizeof(g->bg_path),         GAMES_DIR "/%s/bg.png",      g->name);
    snprintf(g->actor_decl_path, sizeof(g->actor_decl_path), GAMES_DIR "/%s/actor.txt",   g->name);
    snprintf(g->actor_base,      sizeof(g->actor_base),      GAMES_DIR "/%s/actors/%s",   g->name, g->actor_name);
    snprintf(g->actor_meta_path, sizeof(g->actor_meta_path), GAMES_DIR "/%s/actors/%s/sprites.txt",
             g->name, g->actor_name);
}

static void game_load(Game *g, const char *name) {
    memset(g, 0, sizeof(*g));
    snprintf(g->name, sizeof(g->name), "%s", name);
    game_read_actor_decl(g->name, g->actor_name, sizeof(g->actor_name));
    game_build_paths(g);

    web_restore_file(g->walkbox_path);
    web_restore_file(g->fg_path);
    web_restore_file(g->hole_path);
    web_restore_file(g->scale_path);
    web_restore_file(g->actor_meta_path);

    g->bg = LoadTexture(g->bg_path);

    g->dock_count = load_fg_list(g->walkbox_path, g->docks, MAX_WB_POLYS);
    if (g->dock_count <= 0) {
        g->dock_count = 1;
        g->docks[0] = (Walkbox){
            .p = { { 260, 320 }, { 680, 320 }, { 940, 540 }, { 20, 540 } },
            .n = 4,
        };
    }

    g->fg_count = load_fg_list(g->fg_path, g->fgs, MAX_FG_POLYS);
    g->hole_count = load_fg_list(g->hole_path, g->holes, MAX_WB_POLYS);
    g->scale_cfg = default_scale_for_walkboxes(g->docks, g->dock_count);
    load_scale(g->scale_path, &g->scale_cfg);

    load_actor_anims(g->actor_base, g->actor_anims);
}

static void game_unload(Game *g) {
    if (g->bg.id != 0) UnloadTexture(g->bg);
    g->bg = (Texture2D){ 0 };
    unload_actor_anims(g->actor_anims);
}

static int cmp_cstrs(const void *a, const void *b) {
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}

static int game_list(char out[][64], int max) {
    DIR *d = opendir(GAMES_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[192];
        snprintf(path, sizeof(path), GAMES_DIR "/%s", e->d_name);
        DIR *sub = opendir(path);
        if (!sub) continue;
        closedir(sub);
        snprintf(out[n++], 64, "%s", e->d_name);
    }
    closedir(d);
    const char *ptrs[MAX_GAMES];
    for (int i = 0; i < n; i++) ptrs[i] = out[i];
    qsort(ptrs, n, sizeof(ptrs[0]), cmp_cstrs);
    char tmp[MAX_GAMES][64];
    for (int i = 0; i < n; i++) snprintf(tmp[i], 64, "%s", ptrs[i]);
    for (int i = 0; i < n; i++) snprintf(out[i], 64, "%s", tmp[i]);
    return n;
}

static int game_find_index(char names[][64], int n, const char *name) {
    for (int i = 0; i < n; i++) if (strcmp(names[i], name) == 0) return i;
    return -1;
}

int main(int argc, char **argv) {
    InitWindow(SCREEN_W, SCREEN_H, "scumm-game");
    SetTargetFPS(60);

    { FILE *pf = fopen(PLAYER_POS_PATH, "w"); if (pf) fclose(pf); }

    const char *start_game = (argc > 1) ? argv[1] : DEFAULT_GAME;

    Game game;
    game_load(&game, start_game);

    Texture2D sprites[MAX_SPRITES];
    int sprite_count = load_sprites_from_dir(SPRITES_DIR, sprites, MAX_SPRITES);
    Animation anims[ANIM_COUNT];
    load_anims_from_file(game.actor_meta_path, anims);

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
    actor.pos = clamp_to_walkable(actor.pos, game.docks, game.dock_count, game.holes, game.hole_count);
    actor.target = actor.pos;

    char game_flash_text[96] = "";
    float game_flash = 0.0f;
    char game_names[MAX_GAMES][64];
    bool game_menu = false;
    int  game_menu_idx = 0;
    int  game_menu_count = 0;

    Verb selected_verb = VERB_LOOK;
    char message[256] = "";
    float message_timer = 0.0f;
    char status_line[128] = "";
    bool debug_overlay = false;
    bool edit_mode = false;
    Camera2D edit_cam = { .target = { 0, 0 }, .offset = { 0, 0 }, .rotation = 0.0f, .zoom = 1.0f };
    enum { EDIT_WB, EDIT_FG, EDIT_HOLE, EDIT_SCALE };
    int edit_target = EDIT_WB;
    int editing_fg_idx = 0;
    int editing_wb_idx = 0;
    int editing_hole_idx = 0;
    int dragging_vert = -1;
    int dragging_scale_line = -1;
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
                if (save_anims_to_file(game.actor_meta_path, anims)) save_flash = 1.5f;
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
                    if (save_anims_to_file(game.actor_meta_path, anims)) save_flash = 1.5f;
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

        if (game_menu) {
            bool should_select = false;
            if (game_menu_count > 0) {
                if (IsKeyPressed(KEY_UP)   || IsKeyPressedRepeat(KEY_UP))
                    game_menu_idx = (game_menu_idx - 1 + game_menu_count) % game_menu_count;
                if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN))
                    game_menu_idx = (game_menu_idx + 1) % game_menu_count;
                if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))
                    should_select = true;
            }
            if (IsKeyPressed(KEY_ESCAPE)) {
                game_menu = false;
            }

            int row_h = 36;
            int total_h = game_menu_count * row_h;
            int start_y = (SCREEN_H - total_h) / 2;
            int row_w = 520;
            int row_x = (SCREEN_W - row_w) / 2;

            for (int i = 0; i < game_menu_count; i++) {
                Rectangle row = { (float)row_x, (float)(start_y + i * row_h - 4),
                                  (float)row_w, (float)(row_h - 4) };
                if (CheckCollisionPointRec(mouse_screen, row)) {
                    game_menu_idx = i;
                    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) should_select = true;
                }
            }

            if (should_select && game_menu_count > 0) {
                const char *selected = game_names[game_menu_idx];
                if (strcmp(selected, game.name) != 0) {
                    char chosen[64];
                    snprintf(chosen, sizeof(chosen), "%s", selected);
                    game_unload(&game);
                    game_load(&game, chosen);
                    load_anims_from_file(game.actor_meta_path, anims);
                    actor.pos = (Vector2){ SCREEN_W / 2.0f, PLAY_H - 80 };
                    actor.pos = clamp_to_walkable(actor.pos, game.docks, game.dock_count,
                                                  game.holes, game.hole_count);
                    actor.target = actor.pos;
                    actor.moving = false;
                    actor.waypoint_count = 0;
                    snprintf(game_flash_text, sizeof(game_flash_text),
                             "Now playing: %s  (actor: %s)", game.name, game.actor_name);
                    game_flash = 2.0f;
                }
                game_menu = false;
            }

            BeginDrawing();
            ClearBackground((Color){ 15, 12, 28, 255 });

            const char *title = "SELECT GAME";
            int tw = MeasureText(title, 32);
            DrawText(title, (SCREEN_W - tw) / 2, 60, 32, (Color){ 255, 220, 120, 255 });

            for (int i = 0; i < game_menu_count; i++) {
                int y = start_y + i * row_h;
                bool is_sel = (i == game_menu_idx);
                bool is_cur = (strcmp(game_names[i], game.name) == 0);
                if (is_sel) {
                    DrawRectangle(row_x, y - 4, row_w, row_h - 4, (Color){ 60, 50, 20, 200 });
                    DrawRectangleLines(row_x, y - 4, row_w, row_h - 4, (Color){ 255, 220, 120, 220 });
                }
                char line[96];
                snprintf(line, sizeof(line), "%s%s",
                         game_names[i], is_cur ? "   (current)" : "");
                Color c = is_sel ? (Color){ 255, 240, 140, 255 }
                                 : (is_cur ? (Color){ 180, 200, 255, 255 }
                                           : (Color){ 220, 220, 230, 255 });
                DrawText(line, row_x + 24, y, 22, c);
            }

            if (game_menu_count == 0) {
                const char *empty = "(no game folders found in games/)";
                int ew = MeasureText(empty, 18);
                DrawText(empty, (SCREEN_W - ew) / 2, SCREEN_H / 2, 18, (Color){ 200, 200, 210, 200 });
            }

            const char *footer = "[Up/Down] navigate   [Enter/click] select   [Esc] cancel";
            int fw = MeasureText(footer, 14);
            DrawText(footer, (SCREEN_W - fw) / 2, SCREEN_H - 36, 14,
                     (Color){ 160, 160, 180, 220 });

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
                bool ok_wb = save_fg_list(game.walkbox_path, game.docks, game.dock_count);
                bool ok_fg = save_fg_list(game.fg_path, game.fgs, game.fg_count);
                bool ok_h  = save_fg_list(game.hole_path, game.holes, game.hole_count);
                bool ok_s  = save_scale(game.scale_path, &game.scale_cfg);
                if (ok_wb || ok_fg || ok_h || ok_s) save_flash = 1.5f;
                actor.pos = clamp_to_walkable(actor.pos, game.docks, game.dock_count, game.holes, game.hole_count);
                actor.target = actor.pos;
                actor.moving = false;
            }
        }
        if (edit_mode && IsKeyPressed(KEY_W)) {
            dragging_vert = -1;
            if (edit_target == EDIT_WB) {
                if (game.dock_count > 1) editing_wb_idx = (editing_wb_idx + 1) % game.dock_count;
            } else {
                edit_target = EDIT_WB;
                if (editing_wb_idx >= game.dock_count) editing_wb_idx = 0;
            }
        }
        if (edit_mode && IsKeyPressed(KEY_F)) {
            dragging_vert = -1;
            if (edit_target == EDIT_FG) {
                if (game.fg_count > 1) editing_fg_idx = (editing_fg_idx + 1) % game.fg_count;
            } else {
                edit_target = EDIT_FG;
                if (game.fg_count == 0) { game.fg_count = 1; game.fgs[0].n = 0; }
                if (editing_fg_idx >= game.fg_count) editing_fg_idx = 0;
            }
        }
        if (edit_mode && IsKeyPressed(KEY_H)) {
            dragging_vert = -1;
            if (edit_target == EDIT_HOLE) {
                if (game.hole_count > 1) editing_hole_idx = (editing_hole_idx + 1) % game.hole_count;
            } else {
                edit_target = EDIT_HOLE;
                if (game.hole_count == 0) { game.hole_count = 1; game.holes[0].n = 0; }
                if (editing_hole_idx >= game.hole_count) editing_hole_idx = 0;
            }
        }
        if (edit_mode && IsKeyPressed(KEY_K)) {
            dragging_vert = -1;
            dragging_scale_line = -1;
            edit_target = EDIT_SCALE;
        }
        if (!edit_mode && !browser_mode && !game_menu && IsKeyPressed(KEY_G)) {
            game_menu_count = game_list(game_names, MAX_GAMES);
            int cur = game_find_index(game_names, game_menu_count, game.name);
            game_menu_idx = (cur < 0) ? 0 : cur;
            game_menu = true;
        }
        if (edit_mode && IsKeyPressed(KEY_N)) {
            if (edit_target == EDIT_FG && game.fg_count < MAX_FG_POLYS) {
                game.fgs[game.fg_count].n = 0;
                editing_fg_idx = game.fg_count;
                game.fg_count++;
                dragging_vert = -1;
            } else if (edit_target == EDIT_WB && game.dock_count < MAX_WB_POLYS) {
                game.docks[game.dock_count].n = 0;
                editing_wb_idx = game.dock_count;
                game.dock_count++;
                dragging_vert = -1;
            } else if (edit_target == EDIT_HOLE && game.hole_count < MAX_WB_POLYS) {
                game.holes[game.hole_count].n = 0;
                editing_hole_idx = game.hole_count;
                game.hole_count++;
                dragging_vert = -1;
            }
        }
        if (edit_mode && IsKeyPressed(KEY_BACKSPACE)) {
            if (edit_target == EDIT_FG && game.fg_count > 0) {
                for (int i = editing_fg_idx; i < game.fg_count - 1; i++) game.fgs[i] = game.fgs[i + 1];
                game.fg_count--;
                if (game.fg_count == 0) { game.fg_count = 1; game.fgs[0].n = 0; editing_fg_idx = 0; }
                else if (editing_fg_idx >= game.fg_count) editing_fg_idx = game.fg_count - 1;
                dragging_vert = -1;
            } else if (edit_target == EDIT_WB && game.dock_count > 0) {
                for (int i = editing_wb_idx; i < game.dock_count - 1; i++) game.docks[i] = game.docks[i + 1];
                game.dock_count--;
                if (game.dock_count == 0) { game.dock_count = 1; game.docks[0].n = 0; editing_wb_idx = 0; }
                else if (editing_wb_idx >= game.dock_count) editing_wb_idx = game.dock_count - 1;
                dragging_vert = -1;
            } else if (edit_target == EDIT_HOLE && game.hole_count > 0) {
                for (int i = editing_hole_idx; i < game.hole_count - 1; i++) game.holes[i] = game.holes[i + 1];
                game.hole_count--;
                if (game.hole_count == 0) { game.hole_count = 1; game.holes[0].n = 0; editing_hole_idx = 0; }
                else if (editing_hole_idx >= game.hole_count) editing_hole_idx = game.hole_count - 1;
                dragging_vert = -1;
            }
        }

        Hotspot *hover = NULL;

        if (hover) {
            snprintf(status_line, sizeof(status_line), "%s %s", verb_names[selected_verb], hover->name);
        } else {
            snprintf(status_line, sizeof(status_line), "%s", verb_names[selected_verb]);
        }

        if (edit_mode && edit_target == EDIT_SCALE) {
            if (IsKeyPressed(KEY_R)) {
                game.scale_cfg = default_scale_for_walkboxes(game.docks, game.dock_count);
                dragging_scale_line = -1;
            }
            if (IsKeyPressed(KEY_S)) {
                if (save_scale(game.scale_path, &game.scale_cfg)) save_flash = 1.5f;
            }
            if (mouse_screen.y < PLAY_H) {
                float d_top = fabsf(mouse.y - game.scale_cfg.y_top);
                float d_bot = fabsf(mouse.y - game.scale_cfg.y_bot);
                int nearest = (d_top < d_bot) ? 0 : 1;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    dragging_scale_line = nearest;
                }
                if (dragging_scale_line >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    float y = mouse.y;
                    if (y < 0) y = 0;
                    if (y > PLAY_H) y = (float)PLAY_H;
                    if (dragging_scale_line == 0) game.scale_cfg.y_top = y;
                    else                          game.scale_cfg.y_bot = y;
                }
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) dragging_scale_line = -1;
                bool up   = IsKeyPressed(KEY_UP)   || IsKeyPressedRepeat(KEY_UP);
                bool down = IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN);
                if (up || down) {
                    float delta = up ? 0.02f : -0.02f;
                    float *s = (nearest == 0) ? &game.scale_cfg.s_top : &game.scale_cfg.s_bot;
                    *s += delta;
                    if (*s < 0.05f) *s = 0.05f;
                    if (*s > 3.0f)  *s = 3.0f;
                }
            }
        } else if (edit_mode) {
            Walkbox *active;
            if (edit_target == EDIT_FG)        active = &game.fgs[editing_fg_idx];
            else if (edit_target == EDIT_HOLE) active = &game.holes[editing_hole_idx];
            else                               active = &game.docks[editing_wb_idx];
            if (IsKeyPressed(KEY_R)) { active->n = 0; dragging_vert = -1; }
            if (IsKeyPressed(KEY_O)) { sort_poly_by_centroid(active); dragging_vert = -1; }
            if (IsKeyPressed(KEY_S)) {
                bool ok;
                if (edit_target == EDIT_FG)        ok = save_fg_list(game.fg_path, game.fgs, game.fg_count);
                else if (edit_target == EDIT_HOLE) ok = save_fg_list(game.hole_path, game.holes, game.hole_count);
                else                               ok = save_fg_list(game.walkbox_path, game.docks, game.dock_count);
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
                actor.pos = clamp_to_walkable(actor.pos, game.docks, game.dock_count, game.holes, game.hole_count);
                Vector2 final_target = clamp_to_walkable(mouse, game.docks, game.dock_count, game.holes, game.hole_count);
                int nw = find_path_multi(actor.pos, final_target,
                                         game.docks, game.dock_count, game.holes, game.hole_count,
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
        if (game_flash > 0) game_flash -= dt;

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
            Vector2 c = clamp_to_walkable(actor.pos, game.docks, game.dock_count, game.holes, game.hole_count);
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

        if (game.bg.id != 0) {
            Rectangle src = { 0, 0, (float)game.bg.width, (float)game.bg.height };
            Rectangle dst = { 0, 0, SCREEN_W, PLAY_H };
            DrawTexturePro(game.bg, src, dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
        } else {
            DrawRectangle(0, 0, SCREEN_W, PLAY_H, (Color){ 40, 50, 80, 255 });
            DrawText(TextFormat("(missing %s)", game.bg_path), 20, 20, 20, RED);
        }

        float s = actor_scale_at_y(actor.pos.y, &game.scale_cfg);
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
        int play_anim = resolve_anim(want_anim, game.actor_anims);
        AnimTextures *ta = play_anim >= 0 ? &game.actor_anims[play_anim] : NULL;

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
            for (int i = 0; i < game.fg_count; i++) {
                draw_fg_polygon(game.bg, (float)SCREEN_W, (float)PLAY_H, &game.fgs[i], actor.pos.y);
            }
        }

        if (debug_overlay) {
            for (int i = 0; i < game.fg_count; i++) {
                draw_fg_debug_fill(&game.fgs[i], (Color){ 255, 0, 255, 90 });
            }
        }

        if (debug_overlay || edit_mode) {
            Color wb_edge = { 0, 220, 255, 220 };
            Color wb_vert = { 0, 220, 255, 255 };
            Color fg_edge = { 255, 100, 220, 220 };
            Color fg_vert = { 255, 100, 220, 255 };
            Color hole_edge = { 255, 140, 40, 230 };
            Color hole_vert = { 255, 140, 40, 255 };
            for (int pi = 0; pi < game.dock_count; pi++) {
                const Walkbox *wb = &game.docks[pi];
                bool is_active = edit_mode && edit_target == EDIT_WB && pi == editing_wb_idx;
                Color we = wb_edge, wv = wb_vert;
                if (edit_mode && !is_active) { we.a = 100; wv.a = 140; }
                for (int i = 0; i < wb->n; i++) {
                    Vector2 a = wb->p[i];
                    if (wb->n >= 2) {
                        Vector2 b = wb->p[(i + 1) % wb->n];
                        if (wb->n >= 3 || i + 1 < wb->n) DrawLineEx(a, b, 2.0f, we);
                    }
                    float r = (is_active && i == dragging_vert) ? 7.0f : 5.0f;
                    DrawCircleV(a, r, wv);
                    if (is_active || !edit_mode) {
                        char lbl[8]; snprintf(lbl, sizeof(lbl), "%d", i);
                        DrawText(lbl, (int)a.x + 8, (int)a.y - 8, 14, wv);
                    }
                }
            }
            for (int pi = 0; pi < game.fg_count; pi++) {
                const Walkbox *wb = &game.fgs[pi];
                bool is_active = edit_mode && edit_target == EDIT_FG && pi == editing_fg_idx;
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
            for (int pi = 0; pi < game.hole_count; pi++) {
                const Walkbox *wb = &game.holes[pi];
                bool is_active = edit_mode && edit_target == EDIT_HOLE && pi == editing_hole_idx;
                Color ec = hole_edge, vc = hole_vert;
                if (edit_mode && !is_active) { ec.a = 100; vc.a = 140; }
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
            if (edit_mode) {
                bool scale_active = (edit_target == EDIT_SCALE);
                unsigned char base_a = scale_active ? 220 : 90;
                unsigned char near_a = scale_active ? 255 : 130;
                float d_top = fabsf(mouse.y - game.scale_cfg.y_top);
                float d_bot = fabsf(mouse.y - game.scale_cfg.y_bot);
                int hot = scale_active ? ((d_top < d_bot) ? 0 : 1) : -1;
                for (int k = 0; k < 2; k++) {
                    float y = (k == 0) ? game.scale_cfg.y_top : game.scale_cfg.y_bot;
                    float sv = (k == 0) ? game.scale_cfg.s_top : game.scale_cfg.s_bot;
                    bool hot_line = (hot == k);
                    unsigned char a = hot_line ? near_a : base_a;
                    Color c = { 255, 220, 80, a };
                    DrawLineEx((Vector2){ 0, y }, (Vector2){ (float)SCREEN_W, y },
                               hot_line ? 3.0f : 2.0f, c);
                    char lbl[64];
                    snprintf(lbl, sizeof(lbl), "%s  y=%d  s=%.2f",
                             (k == 0) ? "TOP" : "BOT", (int)y, sv);
                    DrawText(lbl, 8, (int)y - 18, 14, c);
                }
            }
            if (edit_mode && edit_target != EDIT_SCALE && dragging_vert < 0 && mouse.y < PLAY_H) {
                Walkbox *active;
                if (edit_target == EDIT_FG)        active = &game.fgs[editing_fg_idx];
                else if (edit_target == EDIT_HOLE) active = &game.holes[editing_hole_idx];
                else                               active = &game.docks[editing_wb_idx];
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
            Color head_col;
            Walkbox *active_warn = NULL;
            const char *hint =
                "[W]/[F]/[H]/[K] wb/fg/hole/scale  [N] new  [Bksp] delete  [O] auto-order  [R] reset  [S] save  [E] exit";
            if (edit_target == EDIT_FG) {
                snprintf(edit_status, sizeof(edit_status), "EDIT FOREGROUND #%d/%d  verts: %d",
                         editing_fg_idx + 1, game.fg_count, game.fgs[editing_fg_idx].n);
                head_col = (Color){ 255, 180, 230, 255 };
                active_warn = &game.fgs[editing_fg_idx];
            } else if (edit_target == EDIT_HOLE) {
                snprintf(edit_status, sizeof(edit_status), "EDIT HOLE #%d/%d  verts: %d",
                         editing_hole_idx + 1, game.hole_count,
                         game.hole_count > 0 ? game.holes[editing_hole_idx].n : 0);
                head_col = (Color){ 255, 180, 80, 255 };
                active_warn = game.hole_count > 0 ? &game.holes[editing_hole_idx] : NULL;
            } else if (edit_target == EDIT_SCALE) {
                snprintf(edit_status, sizeof(edit_status),
                         "EDIT SCALE  TOP y=%d s=%.2f   BOT y=%d s=%.2f",
                         (int)game.scale_cfg.y_top, game.scale_cfg.s_top,
                         (int)game.scale_cfg.y_bot, game.scale_cfg.s_bot);
                head_col = (Color){ 255, 220, 80, 255 };
                hint = "[drag] move line  [Up/Down] scale +/-  [R] reset to walkbox extent  [S] save  [E] exit";
            } else {
                snprintf(edit_status, sizeof(edit_status), "EDIT WALKBOX #%d/%d  verts: %d",
                         editing_wb_idx + 1, game.dock_count, game.docks[editing_wb_idx].n);
                head_col = (Color){ 255, 220, 100, 255 };
                active_warn = &game.docks[editing_wb_idx];
            }
            DrawText(edit_status, 230, PLAY_H + 30, 22, head_col);
            DrawText(hint, 230, PLAY_H + 70, 14, (Color){ 180, 180, 200, 255 });
            if (active_warn && has_self_intersection(active_warn)) {
                DrawText("(!) edges cross -- press [O] to auto-order",
                         230, PLAY_H + 55, 14, (Color){ 255, 130, 130, 255 });
            }
        } else {
            DrawText(status_line, 230, PLAY_H + 30, 22, (Color){ 200, 200, 220, 255 });
            DrawText("Click a verb, click an object, click floor to walk.  [D] overlay  [E] edit  [B] sprite browser  [G] games",
                     230, PLAY_H + 70, 14, (Color){ 140, 140, 160, 255 });
        }
        if (save_flash > 0) {
            DrawText("saved", SCREEN_W - 100, PLAY_H + 20, 16,
                     (Color){ 120, 255, 160, (unsigned char)(255 * (save_flash / 1.5f)) });
        }
        if (game_flash > 0) {
            unsigned char a = (unsigned char)(255 * (game_flash / 2.0f));
            if (a > 255) a = 255;
            int tw = MeasureText(game_flash_text, 20);
            DrawRectangle((SCREEN_W - tw) / 2 - 12, 18, tw + 24, 32, (Color){ 0, 0, 0, (unsigned char)(a * 0.6f) });
            DrawText(game_flash_text, (SCREEN_W - tw) / 2, 24, 20, (Color){ 255, 240, 120, a });
        }

        EndDrawing();
    }

    game_unload(&game);
    for (int i = 0; i < sprite_count; i++) UnloadTexture(sprites[i]);
    CloseWindow();
    return 0;
}
