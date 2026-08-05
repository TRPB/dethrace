#include "harness/iso.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Raw Mode-1 sector layout: 12 sync + 4 header + 2048 data + 288 ECC */
#define ISO_SECTOR_RAW  2352
#define ISO_SECTOR_SKIP 16
#define ISO_SECTOR_DATA 2048

#define ISO_PVD_SECTOR            16
#define ISO_PVD_ROOT_RECORD_OFFS  156

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

struct tIso_image {
    FILE* f;
};

static int iso_read_sector(FILE* f, uint32_t lba, uint8_t* buf) {
    long offset = (long)lba * ISO_SECTOR_RAW + ISO_SECTOR_SKIP;
    if (fseek(f, offset, SEEK_SET) != 0) {
        return 0;
    }
    return (int)fread(buf, 1, ISO_SECTOR_DATA, f) == ISO_SECTOR_DATA;
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int iso_name_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Find one path component named 'target' inside the directory at dir_lba.
   Sets *out_lba, *out_size, *out_is_dir on success. Returns 1 if found. */
static int iso_find_entry(FILE* f, uint32_t dir_lba, uint32_t dir_size,
                          const char* target, int want_dir,
                          uint32_t* out_lba, uint32_t* out_size, int* out_is_dir) {
    uint32_t sectors = (dir_size + ISO_SECTOR_DATA - 1) / ISO_SECTOR_DATA;
    uint32_t s;
    uint8_t buf[ISO_SECTOR_DATA];

    for (s = 0; s < sectors; s++) {
        int pos = 0;
        if (!iso_read_sector(f, dir_lba + s, buf)) {
            return 0;
        }
        while (pos < ISO_SECTOR_DATA) {
            uint8_t rec_len = buf[pos];
            if (rec_len == 0) {
                break;
            }
            {
                int is_dir = (buf[pos + 25] & 0x02) != 0;
                uint8_t id_len = buf[pos + 32];
                char id[33];

                if (id_len > 0 && id_len <= 32) {
                    char* semi;
                    memcpy(id, &buf[pos + 33], id_len);
                    id[id_len] = '\0';
                    semi = strchr(id, ';');
                    if (semi) {
                        *semi = '\0';
                    }
                    if ((want_dir < 0 || is_dir == want_dir) && iso_name_eq(id, target)) {
                        *out_lba = le32(&buf[pos + 2]);
                        *out_size = le32(&buf[pos + 10]);
                        *out_is_dir = is_dir;
                        return 1;
                    }
                }
            }
            pos += rec_len;
        }
    }
    return 0;
}

tIso_image* Iso_Open(const char* path) {
    FILE* f;
    uint8_t pvd[ISO_SECTOR_DATA];
    tIso_image* img;

    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (!iso_read_sector(f, ISO_PVD_SECTOR, pvd)) {
        fclose(f);
        return NULL;
    }
    if (pvd[0] != 1 || memcmp(&pvd[1], "CD001", 5) != 0) {
        fclose(f);
        return NULL;
    }
    img = (tIso_image*)malloc(sizeof(*img));
    if (img == NULL) {
        fclose(f);
        return NULL;
    }
    img->f = f;
    return img;
}

void Iso_Close(tIso_image* img) {
    if (img == NULL) {
        return;
    }
    fclose(img->f);
    free(img);
}

int Iso_ListDir(tIso_image* img, const char* dir_path,
                tIso_entry* entries, int max_entries, int* count) {
    uint8_t pvd[ISO_SECTOR_DATA];
    uint32_t lba, size;
    int is_dir;
    /* Work through each slash-separated component of dir_path. */
    char path_copy[256];
    char* component;

    *count = 0;
    if (!iso_read_sector(img->f, ISO_PVD_SECTOR, pvd)) {
        return -1;
    }
    lba = le32(&pvd[ISO_PVD_ROOT_RECORD_OFFS + 2]);
    size = le32(&pvd[ISO_PVD_ROOT_RECORD_OFFS + 10]);

    strncpy(path_copy, dir_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    /* Navigate each path component. */
    component = strtok(path_copy, "/\\");
    while (component != NULL) {
        if (!iso_find_entry(img->f, lba, size, component, 1, &lba, &size, &is_dir)) {
            return -1;
        }
        component = strtok(NULL, "/\\");
    }

    /* List files in the final directory. */
    {
        uint32_t sectors = (size + ISO_SECTOR_DATA - 1) / ISO_SECTOR_DATA;
        uint32_t s;
        uint8_t buf[ISO_SECTOR_DATA];

        for (s = 0; s < sectors && *count < max_entries; s++) {
            int pos = 0;
            if (!iso_read_sector(img->f, lba + s, buf)) {
                return -1;
            }
            while (pos < ISO_SECTOR_DATA && *count < max_entries) {
                uint8_t rec_len = buf[pos];
                if (rec_len == 0) {
                    break;
                }
                {
                    int entry_is_dir = (buf[pos + 25] & 0x02) != 0;
                    uint8_t id_len = buf[pos + 32];
                    if (!entry_is_dir && id_len > 0 && id_len <= 31) {
                        char id[32];
                        char* semi;
                        int j;
                        memcpy(id, &buf[pos + 33], id_len);
                        id[id_len] = '\0';
                        semi = strchr(id, ';');
                        if (semi) {
                            *semi = '\0';
                        }
                        for (j = 0; id[j]; j++) {
                            id[j] = (char)toupper((unsigned char)id[j]);
                        }
                        memset(&entries[*count], 0, sizeof(entries[*count]));
                        memcpy(entries[*count].name, id, sizeof(entries[*count].name) - 1);
                        entries[*count].lba = le32(&buf[pos + 2]);
                        entries[*count].data_size = le32(&buf[pos + 10]);
                        (*count)++;
                    }
                }
                pos += rec_len;
            }
        }
    }
    return 0;
}

FILE* Iso_ServeEntry(tIso_image* img, const tIso_entry* entry) {
    uint32_t remaining = entry->data_size;
    uint32_t lba = entry->lba;
    uint8_t sector_buf[ISO_SECTOR_DATA];
    FILE* out;

    out = tmpfile();
    if (out == NULL) {
        return NULL;
    }
    while (remaining > 0) {
        uint32_t chunk = remaining < ISO_SECTOR_DATA ? remaining : ISO_SECTOR_DATA;
        if (!iso_read_sector(img->f, lba, sector_buf)) {
            fclose(out);
            return NULL;
        }
        if (fwrite(sector_buf, 1, chunk, out) != chunk) {
            fclose(out);
            return NULL;
        }
        remaining -= chunk;
        lba++;
    }
    rewind(out);
    return out;
}

int Iso_FindGogInDir(const char* dir, char* out, int out_size) {
#ifdef _WIN32
    WIN32_FIND_DATAA find_data;
    HANDLE h;
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s\\*.GOG", dir);
    h = FindFirstFileA(pattern, &find_data);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(pattern, sizeof(pattern), "%s\\*.gog", dir);
        h = FindFirstFileA(pattern, &find_data);
        if (h == INVALID_HANDLE_VALUE) {
            return 0;
        }
    }
    snprintf(out, out_size, "%s\\%s", dir, find_data.cFileName);
    FindClose(h);
    return 1;
#else
    DIR* d = opendir(dir);
    struct dirent* ent;
    if (d == NULL) {
        return 0;
    }
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen >= 4) {
            const char* ext = ent->d_name + nlen - 4;
            if (toupper((unsigned char)ext[0]) == 'G' &&
                toupper((unsigned char)ext[1]) == 'O' &&
                toupper((unsigned char)ext[2]) == 'G' &&
                ext[3] == '\0' && ext[-1] == '.') {
                snprintf(out, out_size, "%s/%s", dir, ent->d_name);
                closedir(d);
                return 1;
            }
        }
    }
    closedir(d);
    return 0;
#endif
}
