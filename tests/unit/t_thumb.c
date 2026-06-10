/* Unit tests for the thumbnail math (user/lib/thumb.h) behind Files thumbnails
 * + Quick Look (files-app.md §11): aspect-preserving fit (no upscale, >=1px)
 * and the box-average ARGB downscaler. */
#include "unit.h"
#include "../../user/lib/thumb.h"

static void test_fit(void) {
    int w, h;
    thumb_fit(128, 128, 48, &w, &h);
    CHECK_INT(w, 48, "square fits the box");           CHECK_INT(h, 48, "square fits the box (h)");
    thumb_fit(200, 100, 50, &w, &h);
    CHECK_INT(w, 50, "landscape pins width");          CHECK_INT(h, 25, "landscape scales height");
    thumb_fit(100, 400, 100, &w, &h);
    CHECK_INT(h, 100, "portrait pins height");         CHECK_INT(w, 25, "portrait scales width");
    thumb_fit(20, 10, 64, &w, &h);
    CHECK_INT(w, 20, "small images never upscale");    CHECK_INT(h, 10, "small images never upscale (h)");
    thumb_fit(1000, 3, 32, &w, &h);
    CHECK_INT(w, 32, "extreme aspect clamps width");   CHECK(h >= 1, "dimension never collapses below 1");
    thumb_fit(0, 0, 16, &w, &h);
    CHECK(w >= 1 && h >= 1, "degenerate input still yields >=1x1");
}

static void test_scale_average(void) {
    /* 2x2 of pure channels -> 1x1 average */
    unsigned int src[4] = { 0xFF000000u, 0x00FF0000u, 0x0000FF00u, 0x000000FFu };
    unsigned int dst[1] = { 0 };
    thumb_scale(dst, 1, 1, src, 2, 2);
    CHECK_INT((int)((dst[0] >> 24) & 255), 63, "alpha averages 255/4");
    CHECK_INT((int)((dst[0] >> 16) & 255), 63, "red averages 255/4");
    CHECK_INT((int)((dst[0] >> 8) & 255), 63, "green averages 255/4");
    CHECK_INT((int)(dst[0] & 255), 63, "blue averages 255/4");
}

static void test_scale_solid_and_shape(void) {
    unsigned int src[8];                                /* 4x2 solid -> 2x1 stays solid */
    for (int i = 0; i < 8; i++) src[i] = 0xFF336699u;
    unsigned int dst[2] = { 0, 0 };
    thumb_scale(dst, 2, 1, src, 4, 2);
    CHECK(dst[0] == 0xFF336699u && dst[1] == 0xFF336699u, "solid color survives the box filter");

    unsigned int half[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu,  /* top row white, bottom black */
                             0xFF000000u, 0xFF000000u };
    unsigned int one = 0;
    thumb_scale(&one, 1, 1, half, 2, 2);
    CHECK_INT((int)((one >> 16) & 255), 127, "half white / half black averages mid-grey");
}

static void test_scale_identity(void) {
    unsigned int src[6] = { 1, 2, 3, 4, 5, 6 };         /* tw==sw th==sh copies through */
    unsigned int dst[6] = { 0 };
    thumb_scale(dst, 3, 2, src, 3, 2);
    int same = 1;
    for (int i = 0; i < 6; i++) if (dst[i] != src[i]) same = 0;
    CHECK(same, "1:1 scale is the identity");
}

int main(void) {
    RUN(test_fit);
    RUN(test_scale_average);
    RUN(test_scale_solid_and_shape);
    RUN(test_scale_identity);
    return UNIT_SUMMARY();
}
