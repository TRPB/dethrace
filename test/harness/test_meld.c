#include "harness/meld.h"
#include "common/loading.h"
#include "common/utility.h"
#include "tests.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#define unlink _unlink
#define rmdir _rmdir
static char* mkdtemp(char* tmpl) {
    char* name = _tempnam(NULL, "test_meld_");
    if (name == NULL) {
        return NULL;
    }
    strncpy(tmpl, name, 255);
    tmpl[255] = 0;
    free(name);
    if (_mkdir(tmpl) != 0) {
        return NULL;
    }
    return tmpl;
}
#else
#include <unistd.h>
#endif

extern int gEncryption_method;
extern void EncodeLine(char* pS);
extern char* GetALineWithNoPossibleService(FILE* pF, unsigned char* pS);

// Write one method-1 encoded "@line\n" to f.
static void write_m1_line(FILE* f, const char* plaintext) {
    char buf[512];
    strncpy(buf, plaintext, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    gEncryption_method = 1;
    EncodeLine(buf);
    fputc('@', f);
    fwrite(buf, 1, strlen(buf), f);
    fputc('\n', f);
}

// Read one decoded line from f using the game's decoder. Returns 1 on success.
static int read_m1_line(FILE* f, char* out, size_t outsize) {
    unsigned char s[256];
    gEncryption_method = 1;
    char* r = GetALineWithNoPossibleService(f, s);
    if (r == NULL) {
        return 0;
    }
    strncpy(out, (char*)s, outsize - 1);
    out[outsize - 1] = 0;
    return 1;
}

// Create a temp dir and a file named `car_name` inside it.
// file_path and dir_path receive the full paths; caller must fclose(),
// unlink(file_path), and rmdir(dir_path) when done.
static FILE* make_named_temp(const char* car_name, char file_path[256], char dir_path[256]) {
    strncpy(dir_path, "/tmp/test_meld_XXXXXX", 255);
    dir_path[255] = 0;
    if (mkdtemp(dir_path) == NULL) {
        return NULL;
    }
    snprintf(file_path, 256, "%s/%s", dir_path, car_name);
    return fopen(file_path, "wb");
}

// The first encoded line of a car TXT is the file's own identity ("SUBFRAME.TXT").
// Patching must not alter it, otherwise the game's corruption check fails.
void test_meld_identity_line_preserved(void) {
    char file_path[256], dir_path[256];
    FILE* src = make_named_temp("SUBFRAME.TXT", file_path, dir_path);
    TEST_ASSERT_NOT_NULL(src);

    write_m1_line(src, "SUBFRAME.TXT");   // identity (line index 0)
    write_m1_line(src, "WHEEL.PIX\t1\t0"); // data line — WHEEL.PIX is a conflict
    fclose(src);

    Meld_Test_ClearConflicts();
    Meld_Test_AddConflict("WHEEL.PIX");

    FILE* result = Meld_Test_PatchTxt(file_path, 1);
    TEST_ASSERT_NOT_NULL(result);

    char line[256];
    int ok;

    ok = read_m1_line(result, line, sizeof(line));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("SUBFRAME.TXT", line);

    ok = read_m1_line(result, line, sizeof(line));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("1:WHEEL.PIX\t1\t0", line);

    fclose(result);
    unlink(file_path);
    rmdir(dir_path);
}

// CARSPLAT scenario: identity line decodes to "SUBFRAME.TXT" + 9 trailing
// high-byte chars (0x82-0x8A). These are not strtok separators, so without
// the fix GetAString returns the garbage-bearing token and the corruption
// check fails.
void test_meld_identity_garbage_stripped(void) {
    char file_path[256], dir_path[256];
    FILE* src = make_named_temp("SUBFRAME.TXT", file_path, dir_path);
    TEST_ASSERT_NOT_NULL(src);

    write_m1_line(src, "SUBFRAME.TXT\x82\x83\x84\x85\x86\x87\x88\x89\x8a");
    fclose(src);

    Meld_Test_ClearConflicts();

    FILE* result = Meld_Test_PatchTxt(file_path, 0);
    TEST_ASSERT_NOT_NULL(result);

    char s[256];
    GetAString(result, s);
    TEST_ASSERT_EQUAL_STRING("SUBFRAME.TXT", s);

    fclose(result);
    unlink(file_path);
    rmdir(dir_path);
}

// GetAString (used by the corruption check) must return the car filename.
void test_meld_getastring_identity(void) {
    char file_path[256], dir_path[256];
    FILE* src = make_named_temp("VLAD2.TXT", file_path, dir_path);
    TEST_ASSERT_NOT_NULL(src);

    write_m1_line(src, "VLAD2.TXT");
    write_m1_line(src, "SOMEMODEL.DAT\t0\t0");
    fclose(src);

    Meld_Test_ClearConflicts();
    Meld_Test_AddConflict("SOMEMODEL.DAT");

    FILE* result = Meld_Test_PatchTxt(file_path, 2);
    TEST_ASSERT_NOT_NULL(result);

    char s[256];
    GetAString(result, s);

    // Must match the car name — this is the check that fires "File X is corrupted".
    TEST_ASSERT_EQUAL_STRING("VLAD2.TXT", s);

    fclose(result);
    unlink(file_path);
    rmdir(dir_path);
}

// Lines with no conflict in the map must pass through unchanged.
void test_meld_non_conflict_unchanged(void) {
    char file_path[256], dir_path[256];
    FILE* src = make_named_temp("MYCAR.TXT", file_path, dir_path);
    TEST_ASSERT_NOT_NULL(src);

    write_m1_line(src, "MYCAR.TXT");
    write_m1_line(src, "SAFE.PIX\t1");
    fclose(src);

    Meld_Test_ClearConflicts();

    FILE* result = Meld_Test_PatchTxt(file_path, 0);
    TEST_ASSERT_NOT_NULL(result);

    char line[256];

    read_m1_line(result, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("MYCAR.TXT", line);

    read_m1_line(result, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("SAFE.PIX\t1", line);

    fclose(result);
    unlink(file_path);
    rmdir(dir_path);
}

// Plain-text (non-@) lines must pass through unchanged.
void test_meld_plaintext_lines_unchanged(void) {
    char file_path[256], dir_path[256];
    FILE* src = make_named_temp("MYCAR.TXT", file_path, dir_path);
    TEST_ASSERT_NOT_NULL(src);

    // Write a mix: plain-text comment then encoded data.
    fprintf(src, "// This is a comment\n");
    write_m1_line(src, "MYCAR.TXT");
    fclose(src);

    Meld_Test_ClearConflicts();

    FILE* result = Meld_Test_PatchTxt(file_path, 0);
    TEST_ASSERT_NOT_NULL(result);

    // Read raw lines back (not using the game decoder, just fgets) to check
    // the comment line is verbatim.
    char raw[256];
    fgets(raw, sizeof(raw), result);
    TEST_ASSERT_EQUAL_STRING("// This is a comment\n", raw);

    // The identity line should still decode correctly.
    char line[256];
    read_m1_line(result, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("MYCAR.TXT", line);

    fclose(result);
    unlink(file_path);
    rmdir(dir_path);
}

void test_meld_suite(void) {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_meld_identity_line_preserved);
    RUN_TEST(test_meld_identity_garbage_stripped);
    RUN_TEST(test_meld_getastring_identity);
    RUN_TEST(test_meld_non_conflict_unchanged);
    RUN_TEST(test_meld_plaintext_lines_unchanged);
}
