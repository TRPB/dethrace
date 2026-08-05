#ifndef HARNESS_ISO_H
#define HARNESS_ISO_H

#if defined(_MSC_VER) && _MSC_VER <= 1020
typedef unsigned char  uint8_t;
typedef unsigned long  uint32_t;
typedef unsigned long  uint64_t;
#else
#include <stdint.h>
#endif
#include <stdio.h>

typedef struct {
    char     name[32];   /* uppercase, no version suffix e.g. "CRASH.SMK" */
    uint32_t lba;
    uint32_t data_size;
} tIso_entry;

typedef struct tIso_image tIso_image;

/* Open a raw Mode-1 ISO 9660 image (2352-byte sectors).
   Returns NULL if the file cannot be opened or is not a valid ISO. */
tIso_image* Iso_Open(const char* path);
void        Iso_Close(tIso_image* img);

/* List files in a slash-separated directory path within the image
   (e.g. "DATA/CUTSCENE").  Fills entries[] up to max_entries.
   Returns 0 on success, -1 if the directory was not found. */
int Iso_ListDir(tIso_image* img, const char* dir_path,
                tIso_entry* entries, int max_entries, int* count);

/* Read an entry's data and return it as a seekable FILE* (tmpfile-backed).
   Returns NULL on failure.  Caller closes the FILE* when done. */
FILE* Iso_ServeEntry(tIso_image* img, const tIso_entry* entry);

/* Scan dir for any file with a .GOG extension and write its full path
   into out (size out_size).  Returns 1 if found, 0 otherwise. */
int Iso_FindGogInDir(const char* dir, char* out, int out_size);

#endif /* HARNESS_ISO_H */
