#include "raylib.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 960
#define SCREEN_H 660
#define VERB_BAR_H 120
#define PLAY_H (SCREEN_H - VERB_BAR_H)
#define ACTOR_SPEED 140.0f
#define MESSAGE_TIME 3.0f

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
    Verb pending_verb;
    Hotspot *pending_hotspot;
} Actor;

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

    Texture2D bg = LoadTexture("assets/bg-dock.png");

    Actor actor = {
        .pos = { SCREEN_W / 2.0f, PLAY_H - 80 },
        .target = { SCREEN_W / 2.0f, PLAY_H - 80 },
        .moving = false,
        .pending_verb = VERB_LOOK,
        .pending_hotspot = NULL,
    };

    Hotspot lamp = {
        .rect = { 680, PLAY_H - 180, 60, 110 },
        .name = "brass lamp",
        .look_text = "It's a tarnished brass lamp. It looks like it could use a good polish.",
        .use_text = "You rub the lamp. Nothing happens. You feel slightly silly.",
        .pickup_text = "You pick up the lamp. Well, you would, if this were a longer demo.",
        .visible = true,
    };

    Verb selected_verb = VERB_LOOK;
    char message[256] = "";
    float message_timer = 0.0f;
    char status_line[128] = "";

    Rectangle verb_rects[VERB_COUNT];
    int verb_w = 180, verb_h = 40, verb_pad = 12;
    int verb_x = 20, verb_y = PLAY_H + 20;
    for (int i = 0; i < VERB_COUNT; i++) {
        verb_rects[i] = (Rectangle){ verb_x, verb_y + i * (verb_h + verb_pad), verb_w, verb_h };
    }

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        Vector2 mouse = GetMousePosition();

        Hotspot *hover = NULL;
        if (lamp.visible && CheckCollisionPointRec(mouse, lamp.rect) && mouse.y < PLAY_H) {
            hover = &lamp;
        }

        if (hover) {
            snprintf(status_line, sizeof(status_line), "%s %s", verb_names[selected_verb], hover->name);
        } else {
            snprintf(status_line, sizeof(status_line), "%s", verb_names[selected_verb]);
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            bool clicked_verb = false;
            for (int i = 0; i < VERB_COUNT; i++) {
                if (CheckCollisionPointRec(mouse, verb_rects[i])) {
                    selected_verb = (Verb)i;
                    clicked_verb = true;
                    break;
                }
            }

            if (!clicked_verb && mouse.y < PLAY_H) {
                float walk_y = mouse.y < PLAY_H - 40 ? PLAY_H - 80 : mouse.y;
                if (walk_y > PLAY_H - 20) walk_y = PLAY_H - 20;
                actor.target = (Vector2){ mouse.x, walk_y };
                actor.moving = true;
                actor.pending_hotspot = hover;
                actor.pending_verb = selected_verb;
            }
        }

        if (actor.moving) {
            Vector2 d = { actor.target.x - actor.pos.x, actor.target.y - actor.pos.y };
            float dist = sqrtf(d.x * d.x + d.y * d.y);
            float step = ACTOR_SPEED * dt;
            if (dist <= step) {
                actor.pos = actor.target;
                actor.moving = false;

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
            } else {
                actor.pos.x += d.x / dist * step;
                actor.pos.y += d.y / dist * step;
            }
        }

        if (message_timer > 0) {
            message_timer -= dt;
            if (message_timer <= 0) message[0] = '\0';
        }

        BeginDrawing();
        ClearBackground((Color){ 20, 22, 30, 255 });

        if (bg.id != 0) {
            Rectangle src = { 0, 0, (float)bg.width, (float)bg.height };
            Rectangle dst = { 0, 0, SCREEN_W, PLAY_H };
            DrawTexturePro(bg, src, dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
        } else {
            DrawRectangle(0, 0, SCREEN_W, PLAY_H, (Color){ 40, 50, 80, 255 });
            DrawText("(missing assets/bg-dock.png)", 20, 20, 20, RED);
        }

        if (lamp.visible) {
            DrawRectangle(lamp.rect.x + 10, lamp.rect.y + 60, 40, 50, (Color){ 150, 100, 40, 255 });
            DrawCircle(lamp.rect.x + 30, lamp.rect.y + 40, 20, (Color){ 200, 160, 60, 255 });
            DrawRectangle(lamp.rect.x + 25, lamp.rect.y + 20, 10, 25, (Color){ 150, 100, 40, 255 });
            if (hover == &lamp) DrawRectangleLinesEx(lamp.rect, 2, YELLOW);
        }

        DrawCircle((int)actor.pos.x, (int)actor.pos.y - 30, 10, (Color){ 240, 210, 180, 255 });
        DrawRectangle((int)actor.pos.x - 8, (int)actor.pos.y - 20, 16, 25, (Color){ 180, 60, 60, 255 });
        DrawRectangle((int)actor.pos.x - 6, (int)actor.pos.y + 5, 5, 20, (Color){ 40, 40, 90, 255 });
        DrawRectangle((int)actor.pos.x + 1, (int)actor.pos.y + 5, 5, 20, (Color){ 40, 40, 90, 255 });

        if (message[0]) {
            int msg_y = (int)actor.pos.y - 70;
            if (msg_y < 20) msg_y = 20;
            draw_wrapped_text(message, 20, msg_y, SCREEN_W - 40, 20, WHITE);
        }

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

        DrawText(status_line, 230, PLAY_H + 30, 22, (Color){ 200, 200, 220, 255 });
        DrawText("Click a verb, then click an object. Click the floor to walk.",
                 230, PLAY_H + 70, 14, (Color){ 140, 140, 160, 255 });

        EndDrawing();
    }

    UnloadTexture(bg);
    CloseWindow();
    return 0;
}
