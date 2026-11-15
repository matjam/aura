/* WebP lossy (VP8 key frame) decoder, after RFC 6386 and libwebp.
 *
 * Macroblocks are decoded a row at a time: each is predicted and
 * reconstructed in a small work buffer (with its top and left borders)
 * and copied into a row cache; when a row is complete the loop filter
 * runs over it and the rows that no later filtering can change are
 * converted to RGB and emitted. The cache keeps 8 luma rows (4 chroma)
 * of the row above, which the next row's filtering may still change.
 * Intra prediction uses the unfiltered pixels, kept in separate top
 * lines. Memory is a few rows of the frame, whatever its height.
 *
 * YUV to RGB conversion and the "fancy" chroma upsampling are libwebp's
 * (its default, which dwebp uses), so output matches dwebp exactly. */
#include "webp_int.h"

#define BPS 32
#define Y_OFF (BPS * 1 + 8)
#define U_OFF (Y_OFF + BPS * 16 + BPS)
#define V_OFF (U_OFF + 16)
#define YUV_SIZE (BPS * 17 + BPS * 9)
#define EXTRA 8 /* luma rows held back for the loop filter */

/* ---- Tables (RFC 6386) ---- */

static const uint8_t k_norm[256] = {
    7, 7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

static const uint8_t k_dc_table[128] = {
    4,   5,   6,   7,   8,   9,   10,  10,  11,  12,  13,  14,  15,  16,  17,  17,  18,  19,  20,  20,  21,  21,
    22,  22,  23,  23,  24,  25,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  37,  38,  39,
    40,  41,  42,  43,  44,  45,  46,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,
    61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  76,  77,  78,  79,  80,  81,
    82,  83,  84,  85,  86,  87,  88,  89,  91,  93,  95,  96,  98,  100, 101, 102, 104, 106, 108, 110, 112, 114,
    116, 118, 122, 124, 126, 128, 130, 132, 134, 136, 138, 140, 143, 145, 148, 151, 154, 157,
};

static const uint16_t k_ac_table[128] = {
    4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,
    26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
    48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  60,  62,  64,  66,  68,  70,  72,  74,  76,  78,  80,
    82,  84,  86,  88,  90,  92,  94,  96,  98,  100, 102, 104, 106, 108, 110, 112, 114, 116, 119, 122, 125, 128,
    131, 134, 137, 140, 143, 146, 149, 152, 155, 158, 161, 164, 167, 170, 173, 177, 181, 185, 189, 193, 197, 201,
    205, 209, 213, 217, 221, 225, 229, 234, 239, 245, 249, 254, 259, 264, 269, 274, 279, 284,
};

/* Coefficient probabilities: [type][band][context][11], flattened. */
static const uint8_t k_coeff_update[4 * 8 * 3 * 11] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    176, 246, 255, 255, 255, 255, 255, 255, 255, 255, 255, 223, 241, 252, 255, 255, 255, 255, 255, 255, 255, 255, 249, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 244, 252, 255, 255, 255, 255, 255, 255, 255, 255, 234, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 246, 254, 255, 255, 255, 255, 255, 255, 255, 255, 239, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, 254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255, 251, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, 251, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 254, 253, 255, 254, 255, 255, 255, 255, 255, 255, 250, 255, 254, 255, 254, 255, 255, 255, 255, 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    217, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 225, 252, 241, 253, 255, 255, 254, 255, 255, 255, 255, 234, 250, 241, 250, 253, 255, 253, 254, 255, 255, 255,
    255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 223, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 238, 253, 254, 254, 255, 255, 255, 255, 255, 255, 255,
    255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255, 249, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 247, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, 252, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255, 250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    186, 251, 250, 255, 255, 255, 255, 255, 255, 255, 255, 234, 251, 244, 254, 255, 255, 255, 255, 255, 255, 255, 251, 251, 243, 253, 254, 255, 254, 255, 255, 255, 255,
    255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, 236, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, 251, 253, 253, 254, 254, 255, 255, 255, 255, 255, 255,
    255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 254, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    248, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 250, 254, 252, 254, 255, 255, 255, 255, 255, 255, 255, 248, 254, 249, 253, 255, 255, 255, 255, 255, 255, 255,
    255, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255, 246, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255, 252, 254, 251, 254, 254, 255, 255, 255, 255, 255, 255,
    255, 254, 252, 255, 255, 255, 255, 255, 255, 255, 255, 248, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255, 253, 255, 254, 254, 255, 255, 255, 255, 255, 255, 255,
    255, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255, 245, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255, 253, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 251, 253, 255, 255, 255, 255, 255, 255, 255, 255, 252, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 252, 255, 255, 255, 255, 255, 255, 255, 255, 255, 249, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 253, 255, 255, 255, 255, 255, 255, 255, 255, 250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

static const uint8_t k_coeff_default[4 * 8 * 3 * 11] = {
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    253, 136, 254, 255, 228, 219, 128, 128, 128, 128, 128, 189, 129, 242, 255, 227, 213, 255, 219, 128, 128, 128, 106, 126, 227, 252, 214, 209, 255, 255, 128, 128, 128,
    1, 98, 248, 255, 236, 226, 255, 255, 128, 128, 128, 181, 133, 238, 254, 221, 234, 255, 154, 128, 128, 128, 78, 134, 202, 247, 198, 180, 255, 219, 128, 128, 128,
    1, 185, 249, 255, 243, 255, 128, 128, 128, 128, 128, 184, 150, 247, 255, 236, 224, 128, 128, 128, 128, 128, 77, 110, 216, 255, 236, 230, 128, 128, 128, 128, 128,
    1, 101, 251, 255, 241, 255, 128, 128, 128, 128, 128, 170, 139, 241, 252, 236, 209, 255, 255, 128, 128, 128, 37, 116, 196, 243, 228, 255, 255, 255, 128, 128, 128,
    1, 204, 254, 255, 245, 255, 128, 128, 128, 128, 128, 207, 160, 250, 255, 238, 128, 128, 128, 128, 128, 128, 102, 103, 231, 255, 211, 171, 128, 128, 128, 128, 128,
    1, 152, 252, 255, 240, 255, 128, 128, 128, 128, 128, 177, 135, 243, 255, 234, 225, 128, 128, 128, 128, 128, 80, 129, 211, 255, 194, 224, 128, 128, 128, 128, 128,
    1, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128, 246, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128, 255, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    198, 35, 237, 223, 193, 187, 162, 160, 145, 155, 62, 131, 45, 198, 221, 172, 176, 220, 157, 252, 221, 1, 68, 47, 146, 208, 149, 167, 221, 162, 255, 223, 128,
    1, 149, 241, 255, 221, 224, 255, 255, 128, 128, 128, 184, 141, 234, 253, 222, 220, 255, 199, 128, 128, 128, 81, 99, 181, 242, 176, 190, 249, 202, 255, 255, 128,
    1, 129, 232, 253, 214, 197, 242, 196, 255, 255, 128, 99, 121, 210, 250, 201, 198, 255, 202, 128, 128, 128, 23, 91, 163, 242, 170, 187, 247, 210, 255, 255, 128,
    1, 200, 246, 255, 234, 255, 128, 128, 128, 128, 128, 109, 178, 241, 255, 231, 245, 255, 255, 128, 128, 128, 44, 130, 201, 253, 205, 192, 255, 255, 128, 128, 128,
    1, 132, 239, 251, 219, 209, 255, 165, 128, 128, 128, 94, 136, 225, 251, 218, 190, 255, 255, 128, 128, 128, 22, 100, 174, 245, 186, 161, 255, 199, 128, 128, 128,
    1, 182, 249, 255, 232, 235, 128, 128, 128, 128, 128, 124, 143, 241, 255, 227, 234, 128, 128, 128, 128, 128, 35, 77, 181, 251, 193, 211, 255, 205, 128, 128, 128,
    1, 157, 247, 255, 236, 231, 255, 255, 128, 128, 128, 121, 141, 235, 255, 225, 227, 255, 255, 128, 128, 128, 45, 99, 188, 251, 195, 217, 255, 224, 128, 128, 128,
    1, 1, 251, 255, 213, 255, 128, 128, 128, 128, 128, 203, 1, 248, 255, 255, 128, 128, 128, 128, 128, 128, 137, 1, 177, 255, 224, 255, 128, 128, 128, 128, 128,
    253, 9, 248, 251, 207, 208, 255, 192, 128, 128, 128, 175, 13, 224, 243, 193, 185, 249, 198, 255, 255, 128, 73, 17, 171, 221, 161, 179, 236, 167, 255, 234, 128,
    1, 95, 247, 253, 212, 183, 255, 255, 128, 128, 128, 239, 90, 244, 250, 211, 209, 255, 255, 128, 128, 128, 155, 77, 195, 248, 188, 195, 255, 255, 128, 128, 128,
    1, 24, 239, 251, 218, 219, 255, 205, 128, 128, 128, 201, 51, 219, 255, 196, 186, 128, 128, 128, 128, 128, 69, 46, 190, 239, 201, 218, 255, 228, 128, 128, 128,
    1, 191, 251, 255, 255, 128, 128, 128, 128, 128, 128, 223, 165, 249, 255, 213, 255, 128, 128, 128, 128, 128, 141, 124, 248, 255, 255, 128, 128, 128, 128, 128, 128,
    1, 16, 248, 255, 255, 128, 128, 128, 128, 128, 128, 190, 36, 230, 255, 236, 255, 128, 128, 128, 128, 128, 149, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128,
    1, 226, 255, 128, 128, 128, 128, 128, 128, 128, 128, 247, 192, 255, 128, 128, 128, 128, 128, 128, 128, 128, 240, 128, 255, 128, 128, 128, 128, 128, 128, 128, 128,
    1, 134, 252, 255, 255, 128, 128, 128, 128, 128, 128, 213, 62, 250, 255, 255, 128, 128, 128, 128, 128, 128, 55, 93, 255, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    202, 24, 213, 235, 186, 191, 220, 160, 240, 175, 255, 126, 38, 182, 232, 169, 184, 228, 174, 255, 187, 128, 61, 46, 138, 219, 151, 178, 240, 170, 255, 216, 128,
    1, 112, 230, 250, 199, 191, 247, 159, 255, 255, 128, 166, 109, 228, 252, 211, 215, 255, 174, 128, 128, 128, 39, 77, 162, 232, 172, 180, 245, 178, 255, 255, 128,
    1, 52, 220, 246, 198, 199, 249, 220, 255, 255, 128, 124, 74, 191, 243, 183, 193, 250, 221, 255, 255, 128, 24, 71, 130, 219, 154, 170, 243, 182, 255, 255, 128,
    1, 182, 225, 249, 219, 240, 255, 224, 128, 128, 128, 149, 150, 226, 252, 216, 205, 255, 171, 128, 128, 128, 28, 108, 170, 242, 183, 194, 254, 223, 255, 255, 128,
    1, 81, 230, 252, 204, 203, 255, 192, 128, 128, 128, 123, 102, 209, 247, 188, 196, 255, 233, 128, 128, 128, 20, 95, 153, 243, 164, 173, 255, 203, 128, 128, 128,
    1, 222, 248, 255, 216, 213, 128, 128, 128, 128, 128, 168, 175, 246, 252, 235, 205, 255, 255, 128, 128, 128, 47, 116, 215, 255, 211, 212, 255, 255, 128, 128, 128,
    1, 121, 236, 253, 212, 214, 255, 255, 128, 128, 128, 141, 84, 213, 252, 201, 202, 255, 219, 128, 128, 128, 42, 80, 160, 240, 162, 185, 255, 205, 128, 128, 128,
    1, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128, 244, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128, 238, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128,
};

/* 4x4 intra mode probabilities: [above][left][9], flattened. */
static const uint8_t k_bmodes_proba[10 * 10 * 9] = {
    231, 120, 48, 89, 115, 113, 120, 152, 112, 152, 179, 64, 126, 170, 118, 46, 70, 95,
    175, 69, 143, 80, 85, 82, 72, 155, 103, 56, 58, 10, 171, 218, 189, 17, 13, 152,
    114, 26, 17, 163, 44, 195, 21, 10, 173, 121, 24, 80, 195, 26, 62, 44, 64, 85,
    144, 71, 10, 38, 171, 213, 144, 34, 26, 170, 46, 55, 19, 136, 160, 33, 206, 71,
    63, 20, 8, 114, 114, 208, 12, 9, 226, 81, 40, 11, 96, 182, 84, 29, 16, 36,
    134, 183, 89, 137, 98, 101, 106, 165, 148, 72, 187, 100, 130, 157, 111, 32, 75, 80,
    66, 102, 167, 99, 74, 62, 40, 234, 128, 41, 53, 9, 178, 241, 141, 26, 8, 107,
    74, 43, 26, 146, 73, 166, 49, 23, 157, 65, 38, 105, 160, 51, 52, 31, 115, 128,
    104, 79, 12, 27, 217, 255, 87, 17, 7, 87, 68, 71, 44, 114, 51, 15, 186, 23,
    47, 41, 14, 110, 182, 183, 21, 17, 194, 66, 45, 25, 102, 197, 189, 23, 18, 22,
    88, 88, 147, 150, 42, 46, 45, 196, 205, 43, 97, 183, 117, 85, 38, 35, 179, 61,
    39, 53, 200, 87, 26, 21, 43, 232, 171, 56, 34, 51, 104, 114, 102, 29, 93, 77,
    39, 28, 85, 171, 58, 165, 90, 98, 64, 34, 22, 116, 206, 23, 34, 43, 166, 73,
    107, 54, 32, 26, 51, 1, 81, 43, 31, 68, 25, 106, 22, 64, 171, 36, 225, 114,
    34, 19, 21, 102, 132, 188, 16, 76, 124, 62, 18, 78, 95, 85, 57, 50, 48, 51,
    193, 101, 35, 159, 215, 111, 89, 46, 111, 60, 148, 31, 172, 219, 228, 21, 18, 111,
    112, 113, 77, 85, 179, 255, 38, 120, 114, 40, 42, 1, 196, 245, 209, 10, 25, 109,
    88, 43, 29, 140, 166, 213, 37, 43, 154, 61, 63, 30, 155, 67, 45, 68, 1, 209,
    100, 80, 8, 43, 154, 1, 51, 26, 71, 142, 78, 78, 16, 255, 128, 34, 197, 171,
    41, 40, 5, 102, 211, 183, 4, 1, 221, 51, 50, 17, 168, 209, 192, 23, 25, 82,
    138, 31, 36, 171, 27, 166, 38, 44, 229, 67, 87, 58, 169, 82, 115, 26, 59, 179,
    63, 59, 90, 180, 59, 166, 93, 73, 154, 40, 40, 21, 116, 143, 209, 34, 39, 175,
    47, 15, 16, 183, 34, 223, 49, 45, 183, 46, 17, 33, 183, 6, 98, 15, 32, 183,
    57, 46, 22, 24, 128, 1, 54, 17, 37, 65, 32, 73, 115, 28, 128, 23, 128, 205,
    40, 3, 9, 115, 51, 192, 18, 6, 223, 87, 37, 9, 115, 59, 77, 64, 21, 47,
    104, 55, 44, 218, 9, 54, 53, 130, 226, 64, 90, 70, 205, 40, 41, 23, 26, 57,
    54, 57, 112, 184, 5, 41, 38, 166, 213, 30, 34, 26, 133, 152, 116, 10, 32, 134,
    39, 19, 53, 221, 26, 114, 32, 73, 255, 31, 9, 65, 234, 2, 15, 1, 118, 73,
    75, 32, 12, 51, 192, 255, 160, 43, 51, 88, 31, 35, 67, 102, 85, 55, 186, 85,
    56, 21, 23, 111, 59, 205, 45, 37, 192, 55, 38, 70, 124, 73, 102, 1, 34, 98,
    125, 98, 42, 88, 104, 85, 117, 175, 82, 95, 84, 53, 89, 128, 100, 113, 101, 45,
    75, 79, 123, 47, 51, 128, 81, 171, 1, 57, 17, 5, 71, 102, 57, 53, 41, 49,
    38, 33, 13, 121, 57, 73, 26, 1, 85, 41, 10, 67, 138, 77, 110, 90, 47, 114,
    115, 21, 2, 10, 102, 255, 166, 23, 6, 101, 29, 16, 10, 85, 128, 101, 196, 26,
    57, 18, 10, 102, 102, 213, 34, 20, 43, 117, 20, 15, 36, 163, 128, 68, 1, 26,
    102, 61, 71, 37, 34, 53, 31, 243, 192, 69, 60, 71, 38, 73, 119, 28, 222, 37,
    68, 45, 128, 34, 1, 47, 11, 245, 171, 62, 17, 19, 70, 146, 85, 55, 62, 70,
    37, 43, 37, 154, 100, 163, 85, 160, 1, 63, 9, 92, 136, 28, 64, 32, 201, 85,
    75, 15, 9, 9, 64, 255, 184, 119, 16, 86, 6, 28, 5, 64, 255, 25, 248, 1,
    56, 8, 17, 132, 137, 255, 55, 116, 128, 58, 15, 20, 82, 135, 57, 26, 121, 40,
    164, 50, 31, 137, 154, 133, 25, 35, 218, 51, 103, 44, 131, 131, 123, 31, 6, 158,
    86, 40, 64, 135, 148, 224, 45, 183, 128, 22, 26, 17, 131, 240, 154, 14, 1, 209,
    45, 16, 21, 91, 64, 222, 7, 1, 197, 56, 21, 39, 155, 60, 138, 23, 102, 213,
    83, 12, 13, 54, 192, 255, 68, 47, 28, 85, 26, 85, 85, 128, 128, 32, 146, 171,
    18, 11, 7, 63, 144, 171, 4, 4, 246, 35, 27, 10, 146, 174, 171, 12, 26, 128,
    190, 80, 35, 99, 180, 80, 126, 54, 45, 85, 126, 47, 87, 176, 51, 41, 20, 32,
    101, 75, 128, 139, 118, 146, 116, 128, 85, 56, 41, 15, 176, 236, 85, 37, 9, 62,
    71, 30, 17, 119, 118, 255, 17, 18, 138, 101, 38, 60, 138, 55, 70, 43, 26, 142,
    146, 36, 19, 30, 171, 255, 97, 27, 20, 138, 45, 61, 62, 219, 1, 81, 188, 64,
    32, 41, 20, 117, 151, 142, 20, 21, 163, 112, 19, 12, 61, 195, 128, 48, 4, 24,
};

static const uint8_t k_bands[17] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
static const uint8_t k_zigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const uint8_t k_cat3[] = {173, 148, 140, 0};
static const uint8_t k_cat4[] = {176, 155, 140, 135, 0};
static const uint8_t k_cat5[] = {180, 157, 141, 134, 130, 0};
static const uint8_t k_cat6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};
static const uint8_t *const k_cat3456[4] = {k_cat3, k_cat4, k_cat5, k_cat6};
static const uint16_t k_scan[16] = {
    0 + 0 * BPS,  4 + 0 * BPS,  8 + 0 * BPS,  12 + 0 * BPS, 0 + 4 * BPS,  4 + 4 * BPS,  8 + 4 * BPS,  12 + 4 * BPS,
    0 + 8 * BPS,  4 + 8 * BPS,  8 + 8 * BPS,  12 + 8 * BPS, 0 + 12 * BPS, 4 + 12 * BPS, 8 + 12 * BPS, 12 + 12 * BPS,
};

/* Prediction modes: 16x16 and chroma use the first four. */
enum { B_DC, B_TM, B_VE, B_HE, B_RD, B_VR, B_LD, B_VL, B_HD, B_HU, DC_NOTOP, DC_NOLEFT, DC_NOTOPLEFT };

/* ---- Boolean decoder ---- */

typedef struct {
    const uint8_t *buf, *end;
    uint32_t value;
    int bits;       /* valid bits in value beyond the 8 being decoded */
    uint32_t range; /* range - 1 */
    bool eof;
} Vp8Bits;

static void br_load_final(Vp8Bits *b)
{
    if (b->buf < b->end) {
        b->bits += 8;
        b->value = (b->value << 8) | *b->buf++;
    } else if (!b->eof) {
        b->value <<= 8;
        b->bits += 8;
        b->eof = true;
    } else {
        b->bits = 0; /* keep reading zeros */
    }
}

static inline void br_load(Vp8Bits *b)
{
    if (b->end - b->buf >= 3) {
        b->value = (b->value << 24) | ((uint32_t)b->buf[0] << 16) | ((uint32_t)b->buf[1] << 8) | b->buf[2];
        b->buf += 3;
        b->bits += 24;
    } else {
        br_load_final(b);
    }
}

static void br_init(Vp8Bits *b, const uint8_t *p, size_t n)
{
    b->buf = p;
    b->end = p + n;
    b->value = 0;
    b->bits = -8;
    b->range = 254;
    b->eof = false;
    br_load(b);
}

static inline int br_bit(Vp8Bits *b, int prob)
{
    uint32_t range = b->range;
    if (b->bits < 0)
        br_load(b);
    int pos = b->bits;
    uint32_t split = (range * (uint32_t)prob) >> 8;
    uint32_t value = b->value >> pos;
    int bit = value > split;
    if (bit) {
        range -= split;
        b->value -= (split + 1) << pos;
    } else {
        range = split + 1;
    }
    int shift = k_norm[range];
    b->range = (range << shift) - 1;
    b->bits -= shift;
    return bit;
}

static uint32_t br_value(Vp8Bits *b, int n)
{
    uint32_t v = 0;
    while (n-- > 0)
        v |= (uint32_t)br_bit(b, 0x80) << n;
    return v;
}

static int br_signed_value(Vp8Bits *b, int n)
{
    int v = (int)br_value(b, n);
    return br_bit(b, 0x80) ? -v : v;
}

/* ---- Decoder state ---- */

typedef struct {
    int y1[2], y2[2], uv[2];
} Vp8Quant;

typedef struct {
    uint8_t limit, ilevel, hev, inner;
} Vp8FInfo;

typedef struct {
    AuImageDecoder *d;
    AuWebpEmit emit;
    void *ctx;
    int w, h, mbw, mbh;
    Vp8Bits br;
    Vp8Bits parts[8];
    int nparts_m1;

    bool use_segment, update_map, absolute_delta;
    int seg_q[4], seg_f[4];
    uint8_t seg_prob[3];
    bool simple;
    int level, sharpness;
    bool use_lf_delta;
    int ref_lf[4], mode_lf[4];
    int filter_type; /* 0 none, 1 simple, 2 normal */
    Vp8FInfo fstr[4][2];
    Vp8Quant q[4];
    uint8_t proba[4][8][3][11];
    bool use_skip;
    int skip_p;

    /* Per macroblock column. */
    uint8_t *intra_t; /* 4 per column */
    uint8_t *nz_t, *nzdc_t;
    Vp8FInfo *finfo;
    uint8_t *ytop, *utop, *vtop; /* unfiltered bottom row of the row above */
    uint8_t intra_l[4];
    uint8_t nz_l, nzdc_l;

    /* The macroblock being decoded. */
    int segment;
    bool is_i4x4;
    uint8_t imodes[16];
    int uvmode;
    uint32_t nz_y, nz_uv;
    int16_t coeffs[384];
    uint8_t yuv[YUV_SIZE];

    /* Row cache: EXTRA held-back rows, then one macroblock row. */
    uint8_t *cy, *cu, *cv;
    int ystride, uvstride;

    /* Output. */
    uint32_t *argb;
    uint8_t *uline, *vline; /* upsampled chroma for one row */
    uint8_t *saved_y, *saved_u, *saved_v;
    int next_y;

    void *mem;
    size_t mem_bytes;
} Vp8Dec;

/* ---- Header parsing ---- */

bool au_webp_vp8_size(const uint8_t *p, size_t n, int *w, int *h)
{
    if (n < 10)
        return false;
    uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    if ((bits & 1) || ((bits >> 1) & 7) > 3 || !((bits >> 4) & 1))
        return false; /* not a key frame, bad profile, or not shown */
    if (p[3] != 0x9d || p[4] != 0x01 || p[5] != 0x2a)
        return false;
    *w = (int)(au_img_le16(p + 6) & 0x3fff);
    *h = (int)(au_img_le16(p + 8) & 0x3fff);
    return *w > 0 && *h > 0;
}

static void parse_segment_header(Vp8Dec *v)
{
    Vp8Bits *br = &v->br;
    v->absolute_delta = true;
    v->seg_prob[0] = v->seg_prob[1] = v->seg_prob[2] = 255;
    v->use_segment = br_bit(br, 0x80);
    if (v->use_segment) {
        v->update_map = br_bit(br, 0x80);
        if (br_bit(br, 0x80)) {
            v->absolute_delta = br_bit(br, 0x80);
            for (int s = 0; s < 4; s++)
                v->seg_q[s] = br_bit(br, 0x80) ? br_signed_value(br, 7) : 0;
            for (int s = 0; s < 4; s++)
                v->seg_f[s] = br_bit(br, 0x80) ? br_signed_value(br, 6) : 0;
        }
        if (v->update_map)
            for (int s = 0; s < 3; s++)
                v->seg_prob[s] = br_bit(br, 0x80) ? (uint8_t)br_value(br, 8) : 255;
    } else {
        v->update_map = false;
    }
}

static void parse_filter_header(Vp8Dec *v)
{
    Vp8Bits *br = &v->br;
    v->simple = br_bit(br, 0x80);
    v->level = (int)br_value(br, 6);
    v->sharpness = (int)br_value(br, 3);
    v->use_lf_delta = br_bit(br, 0x80);
    if (v->use_lf_delta && br_bit(br, 0x80)) {
        for (int i = 0; i < 4; i++)
            if (br_bit(br, 0x80))
                v->ref_lf[i] = br_signed_value(br, 6);
        for (int i = 0; i < 4; i++)
            if (br_bit(br, 0x80))
                v->mode_lf[i] = br_signed_value(br, 6);
    }
    v->filter_type = v->level == 0 ? 0 : v->simple ? 1 : 2;
}

static int clipq(int q, int m) { return q < 0 ? 0 : q > m ? m : q; }

static void parse_quant(Vp8Dec *v)
{
    Vp8Bits *br = &v->br;
    int base = (int)br_value(br, 7);
    int dy1dc = br_bit(br, 0x80) ? br_signed_value(br, 4) : 0;
    int dy2dc = br_bit(br, 0x80) ? br_signed_value(br, 4) : 0;
    int dy2ac = br_bit(br, 0x80) ? br_signed_value(br, 4) : 0;
    int duvdc = br_bit(br, 0x80) ? br_signed_value(br, 4) : 0;
    int duvac = br_bit(br, 0x80) ? br_signed_value(br, 4) : 0;
    for (int i = 0; i < 4; i++) {
        int q;
        if (v->use_segment) {
            q = v->seg_q[i];
            if (!v->absolute_delta)
                q += base;
        } else if (i > 0) {
            v->q[i] = v->q[0];
            continue;
        } else {
            q = base;
        }
        Vp8Quant *m = &v->q[i];
        m->y1[0] = k_dc_table[clipq(q + dy1dc, 127)];
        m->y1[1] = k_ac_table[clipq(q, 127)];
        m->y2[0] = k_dc_table[clipq(q + dy2dc, 127)] * 2;
        m->y2[1] = (k_ac_table[clipq(q + dy2ac, 127)] * 101581) >> 16;
        if (m->y2[1] < 8)
            m->y2[1] = 8;
        m->uv[0] = k_dc_table[clipq(q + duvdc, 117)];
        m->uv[1] = k_ac_table[clipq(q + duvac, 127)];
    }
}

static void parse_proba(Vp8Dec *v)
{
    Vp8Bits *br = &v->br;
    int i = 0;
    for (int t = 0; t < 4; t++)
        for (int b = 0; b < 8; b++)
            for (int c = 0; c < 3; c++)
                for (int p = 0; p < 11; p++, i++)
                    v->proba[t][b][c][p] =
                        br_bit(br, k_coeff_update[i]) ? (uint8_t)br_value(br, 8) : k_coeff_default[i];
    v->use_skip = br_bit(br, 0x80);
    if (v->use_skip)
        v->skip_p = (int)br_value(br, 8);
}

static void precompute_filters(Vp8Dec *v)
{
    if (!v->filter_type)
        return;
    for (int s = 0; s < 4; s++) {
        int base;
        if (v->use_segment) {
            base = v->seg_f[s];
            if (!v->absolute_delta)
                base += v->level;
        } else {
            base = v->level;
        }
        for (int i4 = 0; i4 <= 1; i4++) {
            Vp8FInfo *f = &v->fstr[s][i4];
            int level = base;
            if (v->use_lf_delta) {
                level += v->ref_lf[0];
                if (i4)
                    level += v->mode_lf[0];
            }
            level = level < 0 ? 0 : level > 63 ? 63 : level;
            if (level > 0) {
                int ilevel = level;
                if (v->sharpness > 0) {
                    ilevel >>= v->sharpness > 4 ? 2 : 1;
                    if (ilevel > 9 - v->sharpness)
                        ilevel = 9 - v->sharpness;
                }
                if (ilevel < 1)
                    ilevel = 1;
                f->ilevel = (uint8_t)ilevel;
                f->limit = (uint8_t)(2 * level + ilevel);
                f->hev = level >= 40 ? 2 : level >= 15 ? 1 : 0;
            } else {
                f->limit = 0;
            }
            f->inner = (uint8_t)i4;
        }
    }
}

/* ---- Macroblock parsing ---- */

static void parse_intra_mode(Vp8Dec *v, int mbx)
{
    Vp8Bits *br = &v->br;
    uint8_t *top = v->intra_t + 4 * mbx;
    uint8_t *left = v->intra_l;
    if (v->update_map)
        v->segment = !br_bit(br, v->seg_prob[0]) ? br_bit(br, v->seg_prob[1]) : br_bit(br, v->seg_prob[2]) + 2;
    else
        v->segment = 0;
    int skip = v->use_skip ? br_bit(br, v->skip_p) : 0;
    v->nz_y = (uint32_t)skip; /* stash; parse_residuals reads it */
    v->is_i4x4 = !br_bit(br, 145);
    if (!v->is_i4x4) {
        int ymode = br_bit(br, 156) ? (br_bit(br, 128) ? B_TM : B_HE) : (br_bit(br, 163) ? B_VE : B_DC);
        v->imodes[0] = (uint8_t)ymode;
        memset(top, ymode, 4);
        memset(left, ymode, 4);
    } else {
        uint8_t *modes = v->imodes;
        for (int y = 0; y < 4; y++) {
            int ymode = left[y];
            for (int x = 0; x < 4; x++) {
                const uint8_t *prob = k_bmodes_proba + (top[x] * 10 + ymode) * 9;
                if (!br_bit(br, prob[0]))
                    ymode = B_DC;
                else if (!br_bit(br, prob[1]))
                    ymode = B_TM;
                else if (!br_bit(br, prob[2]))
                    ymode = B_VE;
                else if (!br_bit(br, prob[3]))
                    ymode = !br_bit(br, prob[4]) ? B_HE : !br_bit(br, prob[5]) ? B_RD : B_VR;
                else
                    ymode = !br_bit(br, prob[6])   ? B_LD
                            : !br_bit(br, prob[7]) ? B_VL
                            : !br_bit(br, prob[8]) ? B_HD
                                                   : B_HU;
                top[x] = (uint8_t)ymode;
            }
            memcpy(modes, top, 4);
            modes += 4;
            left[y] = (uint8_t)ymode;
        }
    }
    v->uvmode = !br_bit(br, 142) ? B_DC : !br_bit(br, 114) ? B_VE : br_bit(br, 183) ? B_TM : B_HE;
}

static int large_value(Vp8Bits *br, const uint8_t *p)
{
    int v;
    if (!br_bit(br, p[3])) {
        if (!br_bit(br, p[4]))
            v = 2;
        else
            v = 3 + br_bit(br, p[5]);
    } else if (!br_bit(br, p[6])) {
        if (!br_bit(br, p[7]))
            v = 5 + br_bit(br, 159);
        else {
            v = 7 + 2 * br_bit(br, 165);
            v += br_bit(br, 145);
        }
    } else {
        int bit1 = br_bit(br, p[8]);
        int bit0 = br_bit(br, p[9 + bit1]);
        int cat = 2 * bit1 + bit0;
        v = 0;
        for (const uint8_t *tab = k_cat3456[cat]; *tab; ++tab)
            v += v + br_bit(br, *tab);
        v += 3 + (8 << cat);
    }
    return v;
}

/* Reads one block's coefficients from position n; returns the position
 * after the last coded one (as libwebp's GetCoeffs). Large values are
 * clamped to lim so the transforms cannot overflow on hostile input
 * (valid streams stay far inside it). */
static int get_coeffs(Vp8Bits *br, uint8_t (*prob)[3][11], int ctx, const int *dq, int n, int16_t *out, int lim)
{
    const uint8_t *p = prob[k_bands[n]][ctx];
    for (; n < 16; ++n) {
        if (!br_bit(br, p[0]))
            return n;
        while (!br_bit(br, p[1])) {
            p = prob[k_bands[++n]][0];
            if (n == 16)
                return 16;
        }
        uint8_t(*pctx)[11] = prob[k_bands[n + 1]];
        int c;
        if (!br_bit(br, p[2])) {
            c = dq[n > 0];
            p = pctx[1];
        } else {
            c = large_value(br, p) * dq[n > 0];
            if (c > lim)
                c = lim;
            p = pctx[2];
        }
        out[k_zigzag[n]] = (int16_t)(br_bit(br, 0x80) ? -c : c);
    }
    return 16;
}

static inline uint32_t nz_code(uint32_t nz_coeffs, int nz, int dc_nz)
{
    nz_coeffs <<= 2;
    nz_coeffs |= (uint32_t)(nz > 3 ? 3 : nz > 1 ? 2 : dc_nz);
    return nz_coeffs;
}

static void transform_wht(const int16_t *in, int16_t *out)
{
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        int a0 = in[0 + i] + in[12 + i], a1 = in[4 + i] + in[8 + i];
        int a2 = in[4 + i] - in[8 + i], a3 = in[0 + i] - in[12 + i];
        tmp[0 + i] = a0 + a1;
        tmp[8 + i] = a0 - a1;
        tmp[4 + i] = a3 + a2;
        tmp[12 + i] = a3 - a2;
    }
    for (int i = 0; i < 4; i++) {
        int dc = tmp[0 + i * 4] + 3;
        int a0 = dc + tmp[3 + i * 4], a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
        int a2 = tmp[1 + i * 4] - tmp[2 + i * 4], a3 = dc - tmp[3 + i * 4];
        int o[4] = {(a0 + a1) >> 3, (a3 + a2) >> 3, (a0 - a1) >> 3, (a3 - a2) >> 3};
        for (int k = 0; k < 4; k++) {
            int c = o[k] > 8191 ? 8191 : o[k] < -8191 ? -8191 : o[k];
            out[16 * k] = (int16_t)c;
        }
        out += 64;
    }
}

/* Returns true when the macroblock has no non-zero coefficient. */
static bool parse_residuals(Vp8Dec *v, int mbx, Vp8Bits *br)
{
    const Vp8Quant *q = &v->q[v->segment];
    int16_t *dst = v->coeffs;
    uint8_t *nz_t = &v->nz_t[mbx], *nzdc_t = &v->nzdc_t[mbx];
    uint32_t non_zero_y = 0, non_zero_uv = 0;
    int first;
    uint8_t(*ac_proba)[3][11];
    memset(dst, 0, 384 * sizeof *dst);
    if (!v->is_i4x4) {
        int16_t dc[16] = {0};
        int ctx = *nzdc_t + v->nzdc_l;
        int nz = get_coeffs(br, v->proba[1], ctx, q->y2, 0, dc, 32767);
        *nzdc_t = v->nzdc_l = nz > 0;
        if (nz > 1) {
            transform_wht(dc, dst);
        } else {
            int dc0 = (dc[0] + 3) >> 3;
            for (int i = 0; i < 16 * 16; i += 16)
                dst[i] = (int16_t)dc0;
        }
        first = 1;
        ac_proba = v->proba[0];
    } else {
        first = 0;
        ac_proba = v->proba[3];
    }
    uint32_t tnz = *nz_t & 0x0f, lnz = v->nz_l & 0x0f;
    for (int y = 0; y < 4; y++) {
        uint32_t l = lnz & 1, nz_coeffs = 0;
        for (int x = 0; x < 4; x++) {
            int ctx = (int)(l + (tnz & 1));
            int nz = get_coeffs(br, ac_proba, ctx, q->y1, first, dst, 8191);
            l = nz > first;
            tnz = (tnz >> 1) | (l << 7);
            nz_coeffs = nz_code(nz_coeffs, nz, dst[0] != 0);
            dst += 16;
        }
        tnz >>= 4;
        lnz = (lnz >> 1) | (l << 7);
        non_zero_y = (non_zero_y << 8) | nz_coeffs;
    }
    uint32_t out_t = tnz, out_l = lnz >> 4;
    for (int ch = 0; ch < 4; ch += 2) {
        uint32_t nz_coeffs = 0;
        tnz = (uint32_t)*nz_t >> (4 + ch);
        lnz = (uint32_t)v->nz_l >> (4 + ch);
        for (int y = 0; y < 2; y++) {
            uint32_t l = lnz & 1;
            for (int x = 0; x < 2; x++) {
                int ctx = (int)(l + (tnz & 1));
                int nz = get_coeffs(br, v->proba[2], ctx, q->uv, 0, dst, 8191);
                l = nz > 0;
                tnz = (tnz >> 1) | (l << 3);
                nz_coeffs = nz_code(nz_coeffs, nz, dst[0] != 0);
                dst += 16;
            }
            tnz >>= 2;
            lnz = (lnz >> 1) | (l << 5);
        }
        non_zero_uv |= nz_coeffs << (4 * ch);
        out_t |= (tnz << 4) << ch;
        out_l |= (lnz & 0xf0) << ch;
    }
    *nz_t = (uint8_t)out_t;
    v->nz_l = (uint8_t)out_l;
    v->nz_y = non_zero_y;
    v->nz_uv = non_zero_uv;
    return !(non_zero_y | non_zero_uv);
}

/* ---- Transforms and prediction ---- */

static inline uint8_t clip8b(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

#define MUL1(a) ((((a) * 20091) >> 16) + (a))
#define MUL2(a) (((a) * 35468) >> 16)
#define STORE(x, y, v) dst[(x) + (y) * BPS] = clip8b(dst[(x) + (y) * BPS] + ((v) >> 3))

static void transform_one(const int16_t *in, uint8_t *dst)
{
    int C[16], *tmp = C;
    for (int i = 0; i < 4; i++, in++, tmp += 4) {
        int a = in[0] + in[8], b = in[0] - in[8];
        int c = MUL2(in[4]) - MUL1(in[12]);
        int d = MUL1(in[4]) + MUL2(in[12]);
        tmp[0] = a + d;
        tmp[1] = b + c;
        tmp[2] = b - c;
        tmp[3] = a - d;
    }
    tmp = C;
    for (int i = 0; i < 4; i++, tmp++, dst += BPS) {
        int dc = tmp[0] + 4;
        int a = dc + tmp[8], b = dc - tmp[8];
        int c = MUL2(tmp[4]) - MUL1(tmp[12]);
        int d = MUL1(tmp[4]) + MUL2(tmp[12]);
        STORE(0, 0, a + d);
        STORE(1, 0, b + c);
        STORE(2, 0, b - c);
        STORE(3, 0, a - d);
    }
}

static void transform_dc(const int16_t *in, uint8_t *dst)
{
    int dc = in[0] + 4;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            STORE(i, j, dc);
}

/* bits: the block's 2-bit code from nz_code. */
static inline void do_transform(uint32_t bits, const int16_t *src, uint8_t *dst)
{
    switch (bits >> 30) {
    case 3:
    case 2: transform_one(src, dst); break;
    case 1: transform_dc(src, dst); break;
    default: break;
    }
}

static void do_uv_transform(uint32_t bits, const int16_t *src, uint8_t *dst)
{
    if (!(bits & 0xff))
        return;
    static const uint16_t off[4] = {0, 4, 4 * BPS, 4 * BPS + 4};
    for (int k = 0; k < 4; k++) {
        if (bits & 0xaa)
            transform_one(src + 16 * k, dst + off[k]);
        else
            transform_dc(src + 16 * k, dst + off[k]);
    }
}

#define DST(x, y) dst[(x) + (y) * BPS]
#define AVG3(a, b, c) ((uint8_t)(((a) + 2 * (b) + (c) + 2) >> 2))
#define AVG2(a, b) ((uint8_t)(((a) + (b) + 1) >> 1))

static void true_motion(uint8_t *dst, int size)
{
    const uint8_t *top = dst - BPS;
    int tl = top[-1];
    for (int y = 0; y < size; y++, dst += BPS) {
        int l = dst[-1] - tl;
        for (int x = 0; x < size; x++)
            dst[x] = clip8b(top[x] + l);
    }
}

static void pred4(int mode, uint8_t *dst)
{
    const uint8_t *top = dst - BPS;
    int I = dst[-1], J = dst[-1 + BPS], K = dst[-1 + 2 * BPS], L = dst[-1 + 3 * BPS], X = top[-1];
    int A = top[0], B = top[1], C = top[2], D = top[3], E = top[4], F = top[5], G = top[6], H = top[7];
    switch (mode) {
    case B_DC: {
        int dc = 4;
        for (int i = 0; i < 4; i++)
            dc += top[i] + dst[-1 + i * BPS];
        dc >>= 3;
        for (int i = 0; i < 4; i++)
            memset(dst + i * BPS, dc, 4);
        break;
    }
    case B_TM: true_motion(dst, 4); break;
    case B_VE: {
        uint8_t vals[4] = {AVG3(X, A, B), AVG3(A, B, C), AVG3(B, C, D), AVG3(C, D, E)};
        for (int i = 0; i < 4; i++)
            memcpy(dst + i * BPS, vals, 4);
        break;
    }
    case B_HE:
        memset(dst, AVG3(X, I, J), 4);
        memset(dst + BPS, AVG3(I, J, K), 4);
        memset(dst + 2 * BPS, AVG3(J, K, L), 4);
        memset(dst + 3 * BPS, AVG3(K, L, L), 4);
        break;
    case B_RD:
        DST(0, 3) = AVG3(J, K, L);
        DST(1, 3) = DST(0, 2) = AVG3(I, J, K);
        DST(2, 3) = DST(1, 2) = DST(0, 1) = AVG3(X, I, J);
        DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = AVG3(A, X, I);
        DST(3, 2) = DST(2, 1) = DST(1, 0) = AVG3(B, A, X);
        DST(3, 1) = DST(2, 0) = AVG3(C, B, A);
        DST(3, 0) = AVG3(D, C, B);
        break;
    case B_VR:
        DST(0, 0) = DST(1, 2) = AVG2(X, A);
        DST(1, 0) = DST(2, 2) = AVG2(A, B);
        DST(2, 0) = DST(3, 2) = AVG2(B, C);
        DST(3, 0) = AVG2(C, D);
        DST(0, 3) = AVG3(K, J, I);
        DST(0, 2) = AVG3(J, I, X);
        DST(0, 1) = DST(1, 3) = AVG3(I, X, A);
        DST(1, 1) = DST(2, 3) = AVG3(X, A, B);
        DST(2, 1) = DST(3, 3) = AVG3(A, B, C);
        DST(3, 1) = AVG3(B, C, D);
        break;
    case B_LD:
        DST(0, 0) = AVG3(A, B, C);
        DST(1, 0) = DST(0, 1) = AVG3(B, C, D);
        DST(2, 0) = DST(1, 1) = DST(0, 2) = AVG3(C, D, E);
        DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = AVG3(D, E, F);
        DST(3, 1) = DST(2, 2) = DST(1, 3) = AVG3(E, F, G);
        DST(3, 2) = DST(2, 3) = AVG3(F, G, H);
        DST(3, 3) = AVG3(G, H, H);
        break;
    case B_VL:
        DST(0, 0) = AVG2(A, B);
        DST(1, 0) = DST(0, 2) = AVG2(B, C);
        DST(2, 0) = DST(1, 2) = AVG2(C, D);
        DST(3, 0) = DST(2, 2) = AVG2(D, E);
        DST(0, 1) = AVG3(A, B, C);
        DST(1, 1) = DST(0, 3) = AVG3(B, C, D);
        DST(2, 1) = DST(1, 3) = AVG3(C, D, E);
        DST(3, 1) = DST(2, 3) = AVG3(D, E, F);
        DST(3, 2) = AVG3(E, F, G);
        DST(3, 3) = AVG3(F, G, H);
        break;
    case B_HD:
        DST(0, 0) = DST(2, 1) = AVG2(I, X);
        DST(0, 1) = DST(2, 2) = AVG2(J, I);
        DST(0, 2) = DST(2, 3) = AVG2(K, J);
        DST(0, 3) = AVG2(L, K);
        DST(3, 0) = AVG3(A, B, C);
        DST(2, 0) = AVG3(X, A, B);
        DST(1, 0) = DST(3, 1) = AVG3(I, X, A);
        DST(1, 1) = DST(3, 2) = AVG3(J, I, X);
        DST(1, 2) = DST(3, 3) = AVG3(K, J, I);
        DST(1, 3) = AVG3(L, K, J);
        break;
    default: /* B_HU */
        DST(0, 0) = AVG2(I, J);
        DST(2, 0) = DST(0, 1) = AVG2(J, K);
        DST(2, 1) = DST(0, 2) = AVG2(K, L);
        DST(1, 0) = AVG3(I, J, K);
        DST(3, 0) = DST(1, 1) = AVG3(J, K, L);
        DST(3, 1) = DST(1, 2) = AVG3(K, L, L);
        DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = (uint8_t)L;
        break;
    }
}

/* 16x16 (size 16) and chroma (size 8) prediction. */
static void pred_block(int mode, uint8_t *dst, int size)
{
    int shift = size == 16 ? 4 : 3;
    switch (mode) {
    case B_VE:
        for (int j = 0; j < size; j++)
            memcpy(dst + j * BPS, dst - BPS, (size_t)size);
        break;
    case B_HE:
        for (int j = 0; j < size; j++)
            memset(dst + j * BPS, dst[j * BPS - 1], (size_t)size);
        break;
    case B_TM: true_motion(dst, size); break;
    default: {
        int dc;
        if (mode == B_DC) {
            dc = size;
            for (int j = 0; j < size; j++)
                dc += dst[-1 + j * BPS] + dst[j - BPS];
            dc >>= shift + 1;
        } else if (mode == DC_NOTOP) {
            dc = size >> 1;
            for (int j = 0; j < size; j++)
                dc += dst[-1 + j * BPS];
            dc >>= shift;
        } else if (mode == DC_NOLEFT) {
            dc = size >> 1;
            for (int j = 0; j < size; j++)
                dc += dst[j - BPS];
            dc >>= shift;
        } else {
            dc = 0x80;
        }
        for (int j = 0; j < size; j++)
            memset(dst + j * BPS, dc, (size_t)size);
        break;
    }
    }
}

static inline int check_mode(int mbx, int mby, int mode)
{
    if (mode == B_DC) {
        if (mbx == 0)
            return mby == 0 ? DC_NOTOPLEFT : DC_NOLEFT;
        return mby == 0 ? DC_NOTOP : B_DC;
    }
    return mode;
}

/* Initialises the work buffer's left border at the start of a row. */
static void init_left(Vp8Dec *v, int mby)
{
    uint8_t *y = v->yuv + Y_OFF, *u = v->yuv + U_OFF, *vv = v->yuv + V_OFF;
    for (int j = 0; j < 16; j++)
        y[j * BPS - 1] = 129;
    for (int j = 0; j < 8; j++) {
        u[j * BPS - 1] = 129;
        vv[j * BPS - 1] = 129;
    }
    if (mby > 0) {
        y[-1 - BPS] = u[-1 - BPS] = vv[-1 - BPS] = 129;
    } else {
        memset(y - BPS - 1, 127, 16 + 4 + 1);
        memset(u - BPS - 1, 127, 8 + 1);
        memset(vv - BPS - 1, 127, 8 + 1);
    }
}

static void reconstruct(Vp8Dec *v, int mbx, int mby)
{
    uint8_t *ydst = v->yuv + Y_OFF, *udst = v->yuv + U_OFF, *vdst = v->yuv + V_OFF;
    if (mbx > 0) {
        for (int j = -1; j < 16; j++)
            memcpy(ydst + j * BPS - 4, ydst + j * BPS + 12, 4);
        for (int j = -1; j < 8; j++) {
            memcpy(udst + j * BPS - 4, udst + j * BPS + 4, 4);
            memcpy(vdst + j * BPS - 4, vdst + j * BPS + 4, 4);
        }
    }
    uint8_t *ytop = v->ytop + 16 * mbx, *utop = v->utop + 8 * mbx, *vtop = v->vtop + 8 * mbx;
    if (mby > 0) {
        memcpy(ydst - BPS, ytop, 16);
        memcpy(udst - BPS, utop, 8);
        memcpy(vdst - BPS, vtop, 8);
    }
    const int16_t *coeffs = v->coeffs;
    uint32_t bits = v->nz_y;
    if (v->is_i4x4) {
        uint8_t *tr = ydst - BPS + 16;
        if (mby > 0) {
            if (mbx >= v->mbw - 1)
                memset(tr, ytop[15], 4);
            else
                memcpy(tr, ytop + 16, 4);
        }
        for (int k = 1; k <= 3; k++)
            memcpy(tr + 4 * k * BPS, tr, 4);
        for (int n = 0; n < 16; n++, bits <<= 2) {
            uint8_t *dst = ydst + k_scan[n];
            pred4(v->imodes[n], dst);
            do_transform(bits, coeffs + n * 16, dst);
        }
    } else {
        pred_block(check_mode(mbx, mby, v->imodes[0]), ydst, 16);
        if (bits)
            for (int n = 0; n < 16; n++, bits <<= 2)
                do_transform(bits, coeffs + n * 16, ydst + k_scan[n]);
    }
    int uvmode = check_mode(mbx, mby, v->uvmode);
    pred_block(uvmode, udst, 8);
    pred_block(uvmode, vdst, 8);
    do_uv_transform(v->nz_uv >> 0, coeffs + 16 * 16, udst);
    do_uv_transform(v->nz_uv >> 8, coeffs + 20 * 16, vdst);
    if (mby < v->mbh - 1) {
        memcpy(ytop, ydst + 15 * BPS, 16);
        memcpy(utop, udst + 7 * BPS, 8);
        memcpy(vtop, vdst + 7 * BPS, 8);
    }
    uint8_t *yo = v->cy + EXTRA * v->ystride + 16 * mbx;
    uint8_t *uo = v->cu + (EXTRA / 2) * v->uvstride + 8 * mbx;
    uint8_t *vo = v->cv + (EXTRA / 2) * v->uvstride + 8 * mbx;
    for (int j = 0; j < 16; j++)
        memcpy(yo + j * v->ystride, ydst + j * BPS, 16);
    for (int j = 0; j < 8; j++) {
        memcpy(uo + j * v->uvstride, udst + j * BPS, 8);
        memcpy(vo + j * v->uvstride, vdst + j * BPS, 8);
    }
}

/* ---- Loop filter ---- */

static inline int abs0(int v) { return v < 0 ? -v : v; }
static inline int sclip1(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
static inline int sclip2(int v) { return v < -16 ? -16 : v > 15 ? 15 : v; }

static inline void do_filter2(uint8_t *p, int step)
{
    int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    int a = 3 * (q0 - p0) + sclip1(p1 - q1);
    int a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3);
    p[-step] = clip8b(p0 + a2);
    p[0] = clip8b(q0 - a1);
}

static inline void do_filter4(uint8_t *p, int step)
{
    int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    int a = 3 * (q0 - p0);
    int a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3), a3 = (a1 + 1) >> 1;
    p[-2 * step] = clip8b(p1 + a3);
    p[-step] = clip8b(p0 + a2);
    p[0] = clip8b(q0 - a1);
    p[step] = clip8b(q1 - a3);
}

static inline void do_filter6(uint8_t *p, int step)
{
    int p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step], q2 = p[2 * step];
    int a = sclip1(3 * (q0 - p0) + sclip1(p1 - q1));
    int a1 = (27 * a + 63) >> 7, a2 = (18 * a + 63) >> 7, a3 = (9 * a + 63) >> 7;
    p[-3 * step] = clip8b(p2 + a3);
    p[-2 * step] = clip8b(p1 + a2);
    p[-step] = clip8b(p0 + a1);
    p[0] = clip8b(q0 - a1);
    p[step] = clip8b(q1 - a2);
    p[2 * step] = clip8b(q2 - a3);
}

static inline bool hev(const uint8_t *p, int step, int t)
{
    int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    return abs0(p1 - p0) > t || abs0(q1 - q0) > t;
}

static inline bool needs_filter(const uint8_t *p, int step, int t)
{
    int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    return 4 * abs0(p0 - q0) + abs0(p1 - q1) <= t;
}

static inline bool needs_filter2(const uint8_t *p, int step, int t, int it)
{
    int p3 = p[-4 * step], p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
    int q0 = p[0], q1 = p[step], q2 = p[2 * step], q3 = p[3 * step];
    if (4 * abs0(p0 - q0) + abs0(p1 - q1) > t)
        return false;
    return abs0(p3 - p2) <= it && abs0(p2 - p1) <= it && abs0(p1 - p0) <= it && abs0(q3 - q2) <= it &&
           abs0(q2 - q1) <= it && abs0(q1 - q0) <= it;
}

/* Simple filter across an edge: hstride steps across it, vstride along. */
static void simple_filter(uint8_t *p, int hstride, int vstride, int thresh)
{
    int t2 = 2 * thresh + 1;
    for (int i = 0; i < 16; i++, p += vstride)
        if (needs_filter(p, hstride, t2))
            do_filter2(p, hstride);
}

static void filter_loop(uint8_t *p, int hstride, int vstride, int size, int thresh, int ithresh, int hev_t,
                        bool edge)
{
    int t2 = 2 * thresh + 1;
    for (; size-- > 0; p += vstride) {
        if (!needs_filter2(p, hstride, t2, ithresh))
            continue;
        if (hev(p, hstride, hev_t))
            do_filter2(p, hstride);
        else if (edge)
            do_filter6(p, hstride);
        else
            do_filter4(p, hstride);
    }
}

static void do_filter(Vp8Dec *v, int mbx, int mby)
{
    const Vp8FInfo *f = &v->finfo[mbx];
    int limit = f->limit;
    if (!limit)
        return;
    int ys = v->ystride;
    uint8_t *y = v->cy + EXTRA * ys + 16 * mbx;
    if (v->filter_type == 1) {
        if (mbx > 0)
            simple_filter(y, 1, ys, limit + 4);
        if (f->inner)
            for (int k = 4; k < 16; k += 4)
                simple_filter(y + k, 1, ys, limit);
        if (mby > 0)
            simple_filter(y, ys, 1, limit + 4);
        if (f->inner)
            for (int k = 4; k < 16; k += 4)
                simple_filter(y + k * ys, ys, 1, limit);
        return;
    }
    int uvs = v->uvstride, il = f->ilevel, ht = f->hev;
    uint8_t *u = v->cu + (EXTRA / 2) * uvs + 8 * mbx, *vv = v->cv + (EXTRA / 2) * uvs + 8 * mbx;
    if (mbx > 0) {
        filter_loop(y, 1, ys, 16, limit + 4, il, ht, true);
        filter_loop(u, 1, uvs, 8, limit + 4, il, ht, true);
        filter_loop(vv, 1, uvs, 8, limit + 4, il, ht, true);
    }
    if (f->inner) {
        for (int k = 4; k < 16; k += 4)
            filter_loop(y + k, 1, ys, 16, limit, il, ht, false);
        filter_loop(u + 4, 1, uvs, 8, limit, il, ht, false);
        filter_loop(vv + 4, 1, uvs, 8, limit, il, ht, false);
    }
    if (mby > 0) {
        filter_loop(y, ys, 1, 16, limit + 4, il, ht, true);
        filter_loop(u, uvs, 1, 8, limit + 4, il, ht, true);
        filter_loop(vv, uvs, 1, 8, limit + 4, il, ht, true);
    }
    if (f->inner) {
        for (int k = 4; k < 16; k += 4)
            filter_loop(y + k * ys, ys, 1, 16, limit, il, ht, false);
        filter_loop(u + 4 * uvs, uvs, 1, 8, limit, il, ht, false);
        filter_loop(vv + 4 * uvs, uvs, 1, 8, limit, il, ht, false);
    }
}

/* ---- Output: fancy upsampling and YUV to RGB, as libwebp ---- */

static inline int mulhi(int v, int c) { return (v * c) >> 8; }
static inline int clip_yuv(int v) { return (v & ~16383) == 0 ? v >> 6 : v < 0 ? 0 : 255; }

static inline uint32_t yuv_px(int y, int u, int v)
{
    int yy = mulhi(y, 19077);
    int r = clip_yuv(yy + mulhi(v, 26149) - 14234);
    int g = clip_yuv(yy - mulhi(u, 6419) - mulhi(v, 13320) + 8708);
    int b = clip_yuv(yy + mulhi(u, 33050) - 17685);
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Upsamples one chroma plane for one output row: near is the chroma row
 * nearer the luma row (weight 3), far the other (weight 1). */
static void upsample_line(const uint8_t *near, const uint8_t *far, uint8_t *out, int len)
{
    int last = (len - 1) >> 1;
    int nl = near[0], fl = far[0];
    out[0] = (uint8_t)((3 * nl + fl + 2) >> 2);
    for (int x = 1; x <= last; x++) {
        int nr = near[x], fr = far[x];
        int avg = nl + nr + fl + fr + 8;
        int da = (avg + 2 * (nr + fl)) >> 3, db = (avg + 2 * (nl + fr)) >> 3;
        out[2 * x - 1] = (uint8_t)((da + nl) >> 1);
        out[2 * x] = (uint8_t)((db + nr) >> 1);
        nl = nr;
        fl = fr;
    }
    if (!(len & 1))
        out[len - 1] = (uint8_t)((3 * nl + fl + 2) >> 2);
}

/* Emits the final rows: luma [ys, ye) and chroma [cs, ce) are in the
 * cache (luma row Y at cache row Y - y0c, chroma C at C - c0c); one luma
 * row and one chroma row before them may be in the saved lines. */
static void emit_rows(Vp8Dec *v, int ys, int ye, int cs, int ce, int y0c, int c0c, bool last)
{
    int w = v->w, uvw = (w + 1) / 2, ch = (v->h + 1) / 2;
    int y;
    for (y = v->next_y; y < ye; y++) {
        int cn, cf;
        if (y == 0) {
            cn = cf = 0;
        } else if (y & 1) {
            cn = (y - 1) >> 1;
            cf = (y + 1) >> 1;
        } else {
            cn = y >> 1;
            cf = cn - 1;
        }
        if (cf >= ch)
            cf = ch - 1;
        if ((cn > cf ? cn : cf) >= ce)
            break;
        const uint8_t *yr = y >= ys ? v->cy + (size_t)(y - y0c) * (size_t)v->ystride : v->saved_y;
        const uint8_t *nu = cn >= cs ? v->cu + (size_t)(cn - c0c) * (size_t)v->uvstride : v->saved_u;
        const uint8_t *nv = cn >= cs ? v->cv + (size_t)(cn - c0c) * (size_t)v->uvstride : v->saved_v;
        const uint8_t *fu = cf >= cs ? v->cu + (size_t)(cf - c0c) * (size_t)v->uvstride : v->saved_u;
        const uint8_t *fv = cf >= cs ? v->cv + (size_t)(cf - c0c) * (size_t)v->uvstride : v->saved_v;
        upsample_line(nu, fu, v->uline, w);
        upsample_line(nv, fv, v->vline, w);
        for (int x = 0; x < w; x++)
            v->argb[x] = yuv_px(yr[x], v->uline[x], v->vline[x]);
        v->emit(v->ctx, y, v->argb);
    }
    v->next_y = y;
    if (!last) {
        if (y < ye)
            memcpy(v->saved_y, v->cy + (size_t)(ye - 1 - y0c) * (size_t)v->ystride, (size_t)w);
        memcpy(v->saved_u, v->cu + (size_t)(ce - 1 - c0c) * (size_t)v->uvstride, (size_t)uvw);
        memcpy(v->saved_v, v->cv + (size_t)(ce - 1 - c0c) * (size_t)v->uvstride, (size_t)uvw);
    }
}

/* Filters the macroblock row just decoded, emits what is final and
 * moves the held-back rows to the top of the cache. */
static void finish_row(Vp8Dec *v, int mby)
{
    bool last = mby == v->mbh - 1;
    if (v->filter_type)
        for (int mbx = 0; mbx < v->mbw; mbx++)
            do_filter(v, mbx, mby);
    int ys = mby == 0 ? 0 : mby * 16 - EXTRA;
    int ye = last ? v->h : (mby + 1) * 16 - EXTRA;
    int cs = ys / 2, ce = last ? (v->h + 1) / 2 : ye / 2;
    /* Cache row 0 is luma row mby * 16 - EXTRA (chroma mby * 8 - EXTRA / 2). */
    emit_rows(v, ys, ye, cs, ce, mby * 16 - EXTRA, mby * 8 - EXTRA / 2, last);
    if (!last) {
        memcpy(v->cy, v->cy + 16 * v->ystride, (size_t)EXTRA * (size_t)v->ystride);
        memcpy(v->cu, v->cu + 8 * v->uvstride, (size_t)(EXTRA / 2) * (size_t)v->uvstride);
        memcpy(v->cv, v->cv + 8 * v->uvstride, (size_t)(EXTRA / 2) * (size_t)v->uvstride);
    }
}

/* ---- Frame ---- */

static const char *parse_partitions(Vp8Dec *v, const uint8_t *buf, size_t size, bool truncated)
{
    v->nparts_m1 = (1 << br_value(&v->br, 2)) - 1;
    size_t last = (size_t)v->nparts_m1;
    if (size < 3 * last)
        return truncated ? "truncated" : "bad partitions";
    const uint8_t *sz = buf, *start = buf + 3 * last;
    size_t left = size - 3 * last;
    for (size_t p = 0; p < last; p++) {
        size_t psize = (size_t)sz[0] | ((size_t)sz[1] << 8) | ((size_t)sz[2] << 16);
        if (psize > left)
            psize = left;
        br_init(&v->parts[p], start, psize);
        start += psize;
        left -= psize;
        sz += 3;
    }
    br_init(&v->parts[last], start, left);
    if (!left)
        return truncated ? "truncated" : "bad partitions";
    return NULL;
}

static void alloc_state(Vp8Dec *v)
{
    int mbw = v->mbw;
    v->ystride = 16 * mbw;
    v->uvstride = 8 * mbw;
    size_t n = 0;
    size_t o_intra = n;
    n += 4 * (size_t)mbw;
    size_t o_nz = n;
    n += (size_t)mbw;
    size_t o_nzdc = n;
    n += (size_t)mbw;
    size_t o_ytop = n;
    n += 16 * (size_t)mbw + 4;
    size_t o_utop = n;
    n += 8 * (size_t)mbw;
    size_t o_vtop = n;
    n += 8 * (size_t)mbw;
    size_t o_cy = n;
    n += (size_t)(16 + EXTRA) * (size_t)v->ystride;
    size_t o_cu = n;
    n += (size_t)(8 + EXTRA / 2) * (size_t)v->uvstride;
    size_t o_cv = n;
    n += (size_t)(8 + EXTRA / 2) * (size_t)v->uvstride;
    size_t o_line = n;
    n += 2 * (size_t)v->w;
    size_t o_saved = n;
    n += (size_t)v->w + 2 * (size_t)v->uvstride;
    n = (n + 7) & ~(size_t)7;
    size_t o_finfo = n;
    n += (size_t)mbw * sizeof(Vp8FInfo);
    n = (n + 7) & ~(size_t)7;
    size_t o_argb = n;
    n += (size_t)v->w * 4;
    uint8_t *m = au_calloc(n);
    v->mem = m;
    v->mem_bytes = n;
    au_img_mem(v->d, (ptrdiff_t)n);
    v->intra_t = m + o_intra;
    v->nz_t = m + o_nz;
    v->nzdc_t = m + o_nzdc;
    v->ytop = m + o_ytop;
    v->utop = m + o_utop;
    v->vtop = m + o_vtop;
    v->cy = m + o_cy;
    v->cu = m + o_cu;
    v->cv = m + o_cv;
    v->uline = m + o_line;
    v->vline = m + o_line + v->w;
    v->saved_y = m + o_saved;
    v->saved_u = m + o_saved + v->w;
    v->saved_v = m + o_saved + v->w + v->uvstride;
    v->finfo = (Vp8FInfo *)(void *)(m + o_finfo);
    v->argb = (uint32_t *)(void *)(m + o_argb);
}

static const char *decode_frame(Vp8Dec *v, const uint8_t *data, size_t len, bool truncated)
{
    if (!au_webp_vp8_size(data, len, &v->w, &v->h))
        return len < 10 && truncated ? "truncated" : "bad VP8 header";
    uint32_t part0 = ((uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16)) >> 5;
    data += 10;
    len -= 10;
    if (part0 > len)
        return truncated ? "truncated" : "bad partition length";
    v->mbw = (v->w + 15) >> 4;
    v->mbh = (v->h + 15) >> 4;
    memcpy(v->proba, k_coeff_default, sizeof v->proba);
    br_init(&v->br, data, part0);
    br_bit(&v->br, 0x80); /* colour space */
    br_bit(&v->br, 0x80); /* clamping type */
    parse_segment_header(v);
    parse_filter_header(v);
    const char *err = parse_partitions(v, data + part0, len - part0, truncated);
    if (err)
        return err;
    parse_quant(v);
    br_bit(&v->br, 0x80); /* update_proba: ignored for a key frame */
    parse_proba(v);
    if (v->br.eof)
        return truncated ? "truncated" : "bad VP8 header";
    precompute_filters(v);
    alloc_state(v);

    for (int mby = 0; mby < v->mbh; mby++) {
        Vp8Bits *tbr = &v->parts[mby & v->nparts_m1];
        memset(v->intra_l, B_DC, sizeof v->intra_l);
        v->nz_l = v->nzdc_l = 0;
        init_left(v, mby);
        for (int mbx = 0; mbx < v->mbw; mbx++) {
            parse_intra_mode(v, mbx);
            bool skip = v->nz_y != 0;
            if (!skip) {
                skip = parse_residuals(v, mbx, tbr);
            } else {
                v->nz_l = v->nz_t[mbx] = 0;
                if (!v->is_i4x4)
                    v->nzdc_l = v->nzdc_t[mbx] = 0;
                v->nz_y = v->nz_uv = 0;
            }
            if (v->filter_type) {
                Vp8FInfo *f = &v->finfo[mbx];
                *f = v->fstr[v->segment][v->is_i4x4];
                f->inner |= !skip;
            }
            reconstruct(v, mbx, mby);
        }
        if (v->br.eof || tbr->eof)
            return truncated ? "truncated" : "premature end of VP8 data";
        finish_row(v, mby);
    }
    return NULL;
}

const char *au_webp_vp8_decode(AuImageDecoder *d, const uint8_t *data, size_t len, bool truncated, AuWebpEmit emit,
                               void *ctx)
{
    Vp8Dec *v = au_calloc(sizeof *v);
    v->d = d;
    v->emit = emit;
    v->ctx = ctx;
    au_img_mem(d, (ptrdiff_t)sizeof *v);
    const char *err = decode_frame(v, data, len, truncated);
    if (v->mem) {
        au_free(v->mem);
        au_img_mem(d, -(ptrdiff_t)v->mem_bytes);
    }
    au_img_mem(d, -(ptrdiff_t)sizeof *v);
    au_free(v);
    return err;
}
