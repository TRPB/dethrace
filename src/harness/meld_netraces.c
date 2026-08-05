// MeldNetRaces: inject net-only tracks into the SP campaign.
// Reads NETRACES.TXT from each configured game dir, collects tracks not present
// in any SP RACES.TXT, and rebuilds the merged races buffer with them interleaved
// by rank. Arena accessor helpers (ArenaMapPixName, etc.) also live here since
// the arena metadata they read is written during Meld_NetRaces_Init.

#include "meld_internal.h"
#include "harness/meld.h"
#include "harness/config.h"
#include "harness/os.h"
#include "harness/trace.h"

#include "brender.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MELD_MAX_NET_MAPS 32

static tMeld_race s_net_races[MELD_MAX_NET_MAPS];
static int s_net_map_count = 0;
static int s_net_map_base_count = 0;

// ---------------------------------------------------------------------------
// Decode helpers (used only by the netraces track-file parser)
// ---------------------------------------------------------------------------

/* XOR-decode a method-1 game data line in place.
 * Lines that start with '@' are encoded; the '@' is stripped and the rest decoded. */
static void meld_decode_line_m1(char* s) {
    static const unsigned char key[16] = {
        0x6c,0x1b,0x99,0x5f, 0xb9,0xcd,0x5f,0x13,
        0xcb,0x04,0x20,0x0e, 0x5e,0x1c,0xa1,0x0e
    };
    unsigned char* p;
    int len, seed, i;
    len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    if (len == 0 || s[0] != '@') {
        return;
    }
    memmove(s, s + 1, (size_t)len); /* drop '@' */
    len--;
    p = (unsigned char*)s;
    seed = len % 16;
    for (i = 0; i < len; i++) {
        if (p[i] == 0x9f) p[i] = '\t';
        p[i] = ((key[seed] ^ (p[i] - 32)) & 0x7F) + 32;
        seed = (seed + 7) % 16;
        if (p[i] == 0x9f) p[i] = '\t';
    }
}

/* Read one significant (non-blank, non-comment) decoded line from a track file.
 * Returns 1 on success, 0 on EOF. */
static int meld_read_decoded_sig_line(FILE* f, int method, char* out, int out_len) {
    char buf[256];
    while (fgets(buf, sizeof(buf), f) != NULL) {
        char* p = buf;
        char* end;
        if (method == 1) {
            meld_decode_line_m1(buf);
        } else {
            int l = (int)strlen(buf);
            while (l > 0 && (buf[l - 1] == '\r' || buf[l - 1] == '\n')) buf[--l] = '\0';
            if (l > 0 && buf[0] == '@') memmove(buf, buf + 1, (size_t)l);
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == '\n' || *p == '\r' || (p[0] == '/' && p[1] == '/')) {
            continue;
        }
        end = p + strlen(p) - 1;
        while (end > p && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
            *end-- = '\0';
        }
        {
            char* cm = strstr(p, "//");
            if (cm != NULL) {
                char* t = cm - 1;
                while (t > p && (*t == ' ' || *t == '\t')) {
                    t--;
                }
                *(t + 1) = '\0';
            }
        }
        if (*p == '\0') {
            continue;
        }
        strncpy(out, p, (size_t)(out_len - 1));
        out[out_len - 1] = '\0';
        return 1;
    }
    return 0;
}

/* Open and scan a track file (DATA/RACES/<name>) from game dir game_idx,
 * returning the map-image pixelmap basename via out[]. */
static int meld_extract_map_pix(int game_idx, int method, const char* track_file, char* out, int out_len) {
    char path[MAX_PATH];
    FILE* f;
    char line[256];
    int mat_streak;
    size_t ln;

    meld_join(path, sizeof(path), harness_game_config.game_dirs[game_idx].directory, "DATA");
    meld_join(path, sizeof(path), path, "RACES");
    meld_join(path, sizeof(path), path, track_file);

    f = OS_fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }

    mat_streak = 0;
    while (meld_read_decoded_sig_line(f, method, line, sizeof(line))) {
        ln = strlen(line);
        if (ln >= 4 && strcasecmp(line + ln - 4, ".MAT") == 0) {
            mat_streak++;
        } else {
            if (mat_streak == 3) {
                int screens, skip, i;
                screens = atoi(line);
                skip = screens * 2;
                for (i = 0; i < skip; i++) {
                    if (!meld_read_decoded_sig_line(f, method, line, sizeof(line))) {
                        goto done;
                    }
                }
                if (!meld_read_decoded_sig_line(f, method, line, sizeof(line))) {
                    break;
                }
                strncpy(out, meld_basename(line), (size_t)(out_len - 1));
                out[out_len - 1] = '\0';
                fclose(f);
                return 1;
            }
            mat_streak = 0;
        }
    }
done:
    fclose(f);
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int Meld_IsArenaTrack(void) {
    return gMeld_use_net_starts;
}

char* Meld_ArenaMapPixName(void) {
    if (s_current_race_index >= 0 && s_current_race_index < s_race_count && s_race_is_arena[s_current_race_index]) {
        return s_race_map_pix[s_current_race_index];
    }
    return NULL;
}

int Meld_ArenaHasSceneFli(void) {
    if (s_current_race_index >= 0 && s_current_race_index < s_race_count) {
        return s_race_has_scene_fli[s_current_race_index];
    }
    return 0;
}

void Meld_DrawArenaMapPanel(br_pixelmap* back_screen, br_pixelmap* map_image,
    br_pixelmap* src_palette, br_pixelmap* dst_palette, int pl, int pt, int pr, int pb) {
    int pw = pr - pl;
    int ph = pb - pt;
    int mw, mh, dx, dy, sx, sy, cw, ch;
    if (map_image == NULL) {
        return;
    }
    mw = map_image->width;
    mh = map_image->height;
    dx = pl + (pw - mw) / 2;
    dy = pt + (ph - mh) / 2;
    sx = dx < pl ? pl - dx : 0;
    sy = dy < pt ? pt - dy : 0;
    cw = mw - sx * 2;
    ch = mh - sy * 2;
    if (dx < pl) { dx = pl; }
    if (dy < pt) { dy = pt; }
    if (cw > pw) { cw = pw; }
    if (ch > ph) { ch = ph; }
    if (cw <= 0 || ch <= 0) {
        return;
    }
    if (src_palette != NULL && dst_palette != NULL && src_palette != dst_palette) {
        unsigned char remap[256];
        const unsigned char* sp = (const unsigned char*)src_palette->pixels;
        const unsigned char* dp = (const unsigned char*)dst_palette->pixels;
        const unsigned char* src_row;
        unsigned char* dst_row;
        int i, j, row, col;
        for (i = 0; i < 256; i++) {
            int sr = sp[i * 4], sg = sp[i * 4 + 1], sb = sp[i * 4 + 2];
            long best = 0x7fffffff;
            int best_j = 0;
            for (j = 0; j < 256; j++) {
                int dr = sr - dp[j * 4], dg = sg - dp[j * 4 + 1], db = sb - dp[j * 4 + 2];
                long dist = dr * dr + dg * dg + db * db;
                if (dist < best) { best = dist; best_j = j; if (dist == 0) break; }
            }
            remap[i] = (unsigned char)best_j;
        }
        src_row = (const unsigned char*)map_image->pixels + sy * map_image->row_bytes + sx;
        dst_row = (unsigned char*)back_screen->pixels + dy * back_screen->row_bytes + dx;
        for (row = 0; row < ch; row++) {
            for (col = 0; col < cw; col++) {
                dst_row[col] = remap[src_row[col]];
            }
            src_row += map_image->row_bytes;
            dst_row += back_screen->row_bytes;
        }
    } else {
        BrPixelmapRectangleCopy(back_screen, dx, dy, map_image, sx, sy, cw, ch);
    }
}

void Meld_NetRaces_Init(void) {
    int g, i, l, n, m;
    tMeld_race r;
    FILE* f;
    int sp_count;
    tMeld_buf buf;
    int game_tc[MELD_MAX_GAMES];
    int main_idx[MELD_MAX_RACES];
    int solo_idx[MELD_MAX_RACES];
    int main_count, solo_count;
    tMeld_race* result[MELD_MAX_RACES];
    int result_game[MELD_MAX_RACES];

    if (!harness_game_config.meld_net_races) {
        return;
    }
    if (harness_game_config.game_dirs_count < 1) {
        return;
    }

    for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
        if (s_game_method[g] == 0) {
            s_game_method[g] = meld_detect_method(g);
        }
    }

    if (!gMeld_active && s_race_count == 0) {
        for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
            int method = s_game_method[g];
            f = meld_open_data(g, "RACES.TXT", "rb");
            if (f == NULL) {
                continue;
            }
            while (s_race_count < MELD_MAX_RACES) {
                int rc = meld_read_one_race(f, method, &s_races[s_race_count]);
                if (rc <= 0) {
                    break;
                }
                if (s_races[s_race_count].num_lines <= 4) {
                    continue;
                }
                s_races[s_race_count].game_idx = g;
                s_race_count++;
            }
            fclose(f);
        }
    }

    sp_count = s_race_count;

    s_net_map_count = 0;
    for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
        int method = s_game_method[g];
        f = meld_open_data(g, "NETRACES.TXT", "rb");
        if (f == NULL) {
            continue;
        }
        for (;;) {
            int found, rc;
            if (s_net_map_count >= MELD_MAX_NET_MAPS) {
                break;
            }
            rc = meld_read_one_race(f, method, &r);
            if (rc <= 0) {
                break;
            }
            found = 0;
            for (i = 0; i < sp_count && !found; i++) {
                if (strcasecmp(r.lines[0], s_races[i].lines[0]) == 0) {
                    found = 1;
                }
            }
            if (found) {
                continue;
            }
            for (i = 0; i < s_net_map_count && !found; i++) {
                if (strcasecmp(r.lines[0], s_net_races[i].lines[0]) == 0) {
                    found = 1;
                }
            }
            if (found) {
                continue;
            }
            s_net_races[s_net_map_count] = r;
            s_net_races[s_net_map_count].game_idx = g;
            s_net_map_count++;
        }
        fclose(f);
    }

    if (s_net_map_count == 0) {
        return;
    }

    {
        int total = sp_count + s_net_map_count;
        int solo_pos, net_start, net_end;

        memset(game_tc, 0, sizeof(game_tc));
        for (i = 0; i < sp_count; i++) {
            int gi = s_races[i].game_idx;
            if (gi >= 0 && gi < MELD_MAX_GAMES) {
                game_tc[gi]++;
            }
        }
        main_count = 0;
        solo_count = 0;
        for (i = 0; i < sp_count; i++) {
            int gi = s_races[i].game_idx;
            if (gi >= 0 && gi < MELD_MAX_GAMES && game_tc[gi] == 1) {
                solo_idx[solo_count++] = i;
            } else {
                main_idx[main_count++] = i;
            }
        }

        memset(result, 0, sizeof(tMeld_race*) * (size_t)total);
        memset(result_game, 0, sizeof(int) * (size_t)total);

        solo_pos = (total > 1) ? (49 * (total - 1) / 98) : 0;
        net_end = (total > 1) ? (89 * (total - 1) / 98) : (total - 1);
        net_start = solo_pos + solo_count;
        if (net_start > net_end) {
            net_end = net_start + s_net_map_count;
        }

        for (i = 0; i < solo_count && solo_pos + i < total; i++) {
            result[solo_pos + i] = &s_races[solo_idx[i]];
            result_game[solo_pos + i] = s_races[solo_idx[i]].game_idx;
        }

        for (n = 0; n < s_net_map_count; n++) {
            int range = net_end - net_start;
            int net_pos = net_start + (s_net_map_count == 1 ? range / 2 : (n + 1) * range / (s_net_map_count + 1));
            while (net_pos < total && result[net_pos] != NULL) {
                net_pos++;
            }
            if (net_pos >= total) {
                net_pos = net_end;
                while (net_pos >= net_start && result[net_pos] != NULL) {
                    net_pos--;
                }
            }
            if (net_pos >= 0 && net_pos < total) {
                result[net_pos] = &s_net_races[n];
                result_game[net_pos] = s_net_races[n].game_idx;
            }
        }

        m = 0;
        for (i = 0; i < total && m < main_count; i++) {
            if (result[i] == NULL) {
                result[i] = &s_races[main_idx[m]];
                result_game[i] = s_races[main_idx[m]].game_idx;
                m++;
            }
        }

        free(s_races_buf);
        s_races_buf = NULL;
        s_races_len = 0;
        mbuf_init(&buf);
        memset(s_race_is_arena, 0, sizeof(s_race_is_arena));
        memset(s_race_map_pix, 0, sizeof(s_race_map_pix));
        {
        int seq = 0;
        for (i = 0; i < total; i++) {
            int is_net;
            if (result[i] == NULL) {
                continue;
            }
            s_race_source_game[seq] = result_game[i];
            is_net = (result[i] >= s_net_races && result[i] < s_net_races + MELD_MAX_NET_MAPS);
            s_race_is_arena[seq] = is_net;
            if (is_net) {
                meld_extract_map_pix(result_game[i], s_game_method[result_game[i]], result[i]->lines[2],
                    s_race_map_pix[seq], MELD_MAP_PIX_LEN);
            }
            seq++;
            if (is_net) {
                char fli_line[MELD_LINE_LEN];
                char tmp[MELD_LINE_LEN];
                char *tok1, *tok2, *tok3;
                mbuf_append_m1(&buf, result[i]->lines[0]);
                strncpy(tmp, result[i]->lines[1], MELD_LINE_LEN - 1);
                tmp[MELD_LINE_LEN - 1] = '\0';
                tok1 = strtok(tmp, ",");
                tok2 = strtok(NULL, ",");
                tok3 = strtok(NULL, ",");
                if (tok1 && tok2 && tok3) {
                    char scene_fli[MELD_LINE_LEN];
                    char track_stem[MELD_LINE_LEN];
                    char anim_rel[MELD_LINE_LEN];
                    char *dot;
                    int gd;
                    FILE *probe;
                    strncpy(track_stem, result[i]->lines[2], MELD_LINE_LEN - 1);
                    track_stem[MELD_LINE_LEN - 1] = '\0';
                    dot = strrchr(track_stem, '.');
                    if (dot) {
                        *dot = '\0';
                    }
                    if (strlen(track_stem) > MELD_LINE_LEN - 11) {
                        track_stem[MELD_LINE_LEN - 11] = '\0';
                    }
                    snprintf(anim_rel, sizeof(anim_rel), "ANIM" MELD_SEP "%s.FLI", track_stem);
                    scene_fli[0] = '\0';
                    for (gd = 0; gd < harness_game_config.game_dirs_count && gd < MELD_MAX_GAMES; gd++) {
                        probe = meld_open_data(gd, anim_rel, "rb");
                        if (probe) {
                            fclose(probe);
                            snprintf(scene_fli, sizeof(scene_fli), "%s.FLI", track_stem);
                            s_race_has_scene_fli[seq - 1] = 1;
                            break;
                        }
                    }
                    snprintf(fli_line, MELD_LINE_LEN, "%s,%s,%s",
                        scene_fli[0] != '\0' ? scene_fli : tok3, tok2, tok3);
                } else {
                    strncpy(fli_line, result[i]->lines[1], MELD_LINE_LEN - 1);
                    fli_line[MELD_LINE_LEN - 1] = '\0';
                }
                mbuf_append_m1(&buf, fli_line);
                mbuf_append_m1(&buf, result[i]->lines[2]);
                mbuf_append_m1(&buf, "1");
                mbuf_append_m1(&buf, "5,3");
                mbuf_append_m1(&buf, "0,99");
                mbuf_append_m1(&buf, "1");
                mbuf_append_m1(&buf, "(\?\?\?)");
            } else {
                for (l = 0; l < result[i]->num_lines; l++) {
                    mbuf_append_m1(&buf, result[i]->lines[l]);
                }
            }
        }
        mbuf_append_m1(&buf, "END");

        s_net_map_base_count = sp_count;
        s_race_count = total;
        }
    }

    s_races_buf = buf.data;
    s_races_len = buf.len;
    gMeld_net_races_active = 1;

    gEncryption_method = 0;

}
