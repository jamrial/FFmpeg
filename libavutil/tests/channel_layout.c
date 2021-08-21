/*
 * Copyright (c) 2021 James Almer
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/channel_layout.c"

#define CHANNEL_LAYOUT_FROM_MASK(layout, x)                                \
    av_channel_layout_uninit(&layout);                                     \
    if (!av_channel_layout_from_mask(&layout, x))                          \
        av_channel_layout_describe_bprint(&layout, &bp);

#define CHANNEL_LAYOUT_FROM_STRING(layout, x)                              \
    av_channel_layout_uninit(&layout);                                     \
    if (!av_channel_layout_from_string(&layout, x))                        \
        av_channel_layout_describe_bprint(&layout, &bp);

#define CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(layout, x)                       \
    ret = av_channel_layout_channel_from_index(&layout, x);                \
    if (ret < 0)                                                           \
        ret = -1

#define CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(layout, x)                       \
    ret = av_channel_layout_index_from_channel(&layout, x);                \
    if (ret < 0)                                                           \
        ret = -1

#define CHANNEL_LAYOUT_CHANNEL_FROM_STRING(layout, x)                      \
    ret = av_channel_layout_channel_from_string(&layout, x);               \
    if (ret < 0)                                                           \
        ret = -1

#define CHANNEL_LAYOUT_INDEX_FROM_STRING(layout, x)                        \
    ret = av_channel_layout_index_from_string(&layout, x);                 \
    if (ret < 0)                                                           \
        ret = -1

int main(void)
{
    AVChannelLayout surround = { 0 };
    AVChannelLayout custom = { 0 };
    AVBPrint bp;
    int ret;

    av_bprint_init(&bp, 64, AV_BPRINT_SIZE_AUTOMATIC);

    printf("Testing av_channel_name\n");
    av_channel_name_bprint(&bp, AV_CHAN_FRONT_LEFT);
    printf("With AV_CHAN_FRONT_LEFT: %27s\n", bp.str);
    av_channel_name_bprint(&bp, AV_CHAN_FRONT_RIGHT);
    printf("With AV_CHAN_FRONT_RIGHT: %26s\n", bp.str);
    av_channel_name_bprint(&bp, 63);
    printf("With 63: %43s\n", bp.str);

    printf("Testing av_channel_description\n");
    av_channel_description_bprint(&bp, AV_CHAN_FRONT_LEFT);
    printf("With AV_CHAN_FRONT_LEFT: %27s\n", bp.str);
    av_channel_description_bprint(&bp, AV_CHAN_FRONT_RIGHT);
    printf("With AV_CHAN_FRONT_RIGHT: %26s\n", bp.str);
    av_channel_description_bprint(&bp, 63);
    printf("With 63: %43s\n", bp.str);

    printf("\nTesting av_channel_from_string\n");
    printf("With \"FL\": %41d\n", av_channel_from_string("FL"));
    printf("With \"FR\": %41d\n", av_channel_from_string("FR"));
    printf("With \"USR63\": %38d\n", av_channel_from_string("USR63"));

    printf("\n==Native layouts==\n");

    printf("\nTesting av_channel_layout_from_string\n");
    CHANNEL_LAYOUT_FROM_STRING(surround, "0x3f");
    printf("With \"0x3f\": %39s\n", bp.str);
    CHANNEL_LAYOUT_FROM_STRING(surround, "6c");
    printf("With \"6c\": %41s\n", bp.str);
    CHANNEL_LAYOUT_FROM_STRING(surround, "6C");
    printf("With \"6C\": %41s\n", bp.str);
    CHANNEL_LAYOUT_FROM_STRING(surround, "6");
    printf("With \"6\": %42s\n", bp.str);
    CHANNEL_LAYOUT_FROM_STRING(surround, "6 channels");
    printf("With \"6 channels\": %33s\n", bp.str);
    CHANNEL_LAYOUT_FROM_STRING(surround, "FL+FR+FC+LFE+BL+BR");
    printf("With \"FL+FR+FC+LFE+BL+BR\": %25s\n", bp.str);
    CHANNEL_LAYOUT_FROM_STRING(surround, "5.1");
    printf("With \"5.1\": %40s\n", bp.str);
    CHANNEL_LAYOUT_FROM_STRING(surround, "FL+FR+USR63");
    printf("With \"FL+FR+USR63\": %32s\n", bp.str);
    CHANNEL_LAYOUT_FROM_STRING(surround, "FL+FR+FC+LFE+SL+SR");
    printf("With \"FL+FR+FC+LFE+SL+SR\": %25s\n", bp.str);
    CHANNEL_LAYOUT_FROM_STRING(surround, "5.1(side)");
    printf("With \"5.1(side)\": %34s\n", bp.str);

    printf("\nTesting av_channel_layout_from_mask\n");
    CHANNEL_LAYOUT_FROM_MASK(surround, AV_CH_LAYOUT_5POINT1);
    printf("With AV_CH_LAYOUT_5POINT1: %25s\n", bp.str);

    printf("\nTesting av_channel_layout_channel_from_index\n");
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(surround, 0);
    printf("On 5.1(side) layout with 0: %24d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(surround, 1);
    printf("On 5.1(side) layout with 1: %24d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(surround, 2);
    printf("On 5.1(side) layout with 2: %24d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(surround, 3);
    printf("On 5.1(side) layout with 3: %24d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(surround, 4);
    printf("On 5.1(side) layout with 4: %24d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(surround, 5);
    printf("On 5.1(side) layout with 5: %24d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(surround, 6);
    printf("On 5.1(side) layout with 6: %24d\n", ret);

    printf("\nTesting av_channel_layout_index_from_channel\n");
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(surround, AV_CHAN_FRONT_LEFT);
    printf("On 5.1(side) layout with AV_CHAN_FRONT_LEFT: %7d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(surround, AV_CHAN_FRONT_RIGHT);
    printf("On 5.1(side) layout with AV_CHAN_FRONT_RIGHT: %6d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(surround, AV_CHAN_FRONT_CENTER);
    printf("On 5.1(side) layout with AV_CHAN_FRONT_CENTER: %5d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(surround, AV_CHAN_LOW_FREQUENCY);
    printf("On 5.1(side) layout with AV_CHAN_LOW_FREQUENCY: %4d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(surround, AV_CHAN_SIDE_LEFT);
    printf("On 5.1(side) layout with AV_CHAN_SIDE_LEFT: %8d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(surround, AV_CHAN_SIDE_RIGHT);
    printf("On 5.1(side) layout with AV_CHAN_SIDE_RIGHT: %7d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(surround, AV_CHAN_BACK_CENTER);
    printf("On 5.1(side) layout with AV_CHAN_BACK_CENTER: %6d\n", ret);

    printf("\nTesting av_channel_layout_channel_from_string\n");
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(surround, "FL");
    printf("On 5.1(side) layout with \"FL\": %21d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(surround, "FR");
    printf("On 5.1(side) layout with \"FR\": %21d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(surround, "FC");
    printf("On 5.1(side) layout with \"FC\": %21d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(surround, "LFE");
    printf("On 5.1(side) layout with \"LFE\": %20d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(surround, "SL");
    printf("On 5.1(side) layout with \"SL\": %21d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(surround, "SR");
    printf("On 5.1(side) layout with \"SR\": %21d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(surround, "BC");
    printf("On 5.1(side) layout with \"BC\": %21d\n", ret);

    printf("\nTesting av_channel_layout_index_from_string\n");
    CHANNEL_LAYOUT_INDEX_FROM_STRING(surround, "FL");
    printf("On 5.1(side) layout with \"FL\": %21d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_STRING(surround, "FR");
    printf("On 5.1(side) layout with \"FR\": %21d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_STRING(surround, "FC");
    printf("On 5.1(side) layout with \"FC\": %21d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_STRING(surround, "LFE");
    printf("On 5.1(side) layout with \"LFE\": %20d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_STRING(surround, "SL");
    printf("On 5.1(side) layout with \"SL\": %21d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_STRING(surround, "SR");
    printf("On 5.1(side) layout with \"SR\": %21d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_STRING(surround, "BC");
    printf("On 5.1(side) layout with \"BC\": %21d\n", ret);

    printf("\n==Custom layouts==\n");

    printf("\nTesting av_channel_layout_from_string\n");
    CHANNEL_LAYOUT_FROM_STRING(custom, "FL+FR+FC+BL+BR+LFE");
    printf("With \"FL+FR+FC+BL+BR+LFE\": %25s\n", bp.str);
    CHANNEL_LAYOUT_FROM_STRING(custom, "FR+FL+USR63");
    printf("With \"FR+FL+USR63\" layout: %25s\n", bp.str);

    av_strlcpy(custom.u.map[2].name, "CUS", sizeof(custom.u.map[2].name));
    printf("\nTesting av_channel_layout_index_from_string\n");
    CHANNEL_LAYOUT_INDEX_FROM_STRING(custom, "FR");
    printf("On \"FR+FL+USR63\" layout with \"FR\": %17d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_STRING(custom, "FL");
    printf("On \"FR+FL+USR63\" layout with \"FL\": %17d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_STRING(custom, "USR63");
    printf("On \"FR+FL+USR63\" layout with \"USR63\": %14d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_STRING(custom, "CUS");
    printf("On \"FR+FL+USR63\" layout with custom label \"CUS\": %3d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_STRING(custom, "BC");
    printf("On \"FR+FL+USR63\" layout with \"BC\": %17d\n", ret);

    printf("\nTesting av_channel_layout_channel_from_string\n");
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(custom, "FR");
    printf("On \"FR+FL+USR63\" layout with \"FR\": %17d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(custom, "FL");
    printf("On \"FR+FL+USR63\" layout with \"FL\": %17d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(custom, "USR63");
    printf("On \"FR+FL+USR63\" layout with \"USR63\": %14d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(custom, "CUS");
    printf("On \"FR+FL+USR63\" layout with custom label \"CUS\": %3d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(custom, "BC");
    printf("On \"FR+FL+USR63\" layout with \"BC\": %17d\n", ret);

    printf("\nTesting av_channel_layout_index_from_channel\n");
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(custom, AV_CHAN_FRONT_RIGHT);
    printf("On \"FR+FL+USR63\" layout with AV_CHAN_FRONT_RIGHT: %2d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(custom, AV_CHAN_FRONT_LEFT);
    printf("On \"FR+FL+USR63\" layout with AV_CHAN_FRONT_LEFT: %3d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(custom, 63);
    printf("On \"FR+FL+USR63\" layout with 63: %19d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(custom, AV_CHAN_BACK_CENTER);
    printf("On \"FR+FL+USR63\" layout with AV_CHAN_BACK_CENTER: %2d\n", ret);

    printf("\nTesting av_channel_layout_channel_from_index\n");
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(custom, 0);
    printf("On \"FR+FL+USR63\" layout with 0: %20d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(custom, 1);
    printf("On \"FR+FL+USR63\" layout with 1: %20d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(custom, 2);
    printf("On \"FR+FL+USR63\" layout with 2: %20d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(custom, 3);
    printf("On \"FR+FL+USR63\" layout with 3: %20d\n", ret);

    av_channel_layout_uninit(&surround);
    av_channel_layout_uninit(&custom);
    av_bprint_finalize(&bp, NULL);

    return 0;
}
