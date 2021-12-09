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
    buf[0] = 0;                                                            \
    if (!av_channel_layout_from_mask(&layout, x))                          \
        av_channel_layout_describe(&layout, buf, sizeof(buf));

#define CHANNEL_LAYOUT_FROM_STRING(layout, x)                              \
    av_channel_layout_uninit(&layout);                                     \
    buf[0] = 0;                                                            \
    if (!av_channel_layout_from_string(&layout, x))                         \
        av_channel_layout_describe(&layout, buf, sizeof(buf));

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
    char buf[64];
    int ret;

    printf("Testing av_channel_name\n");
    av_channel_name(buf, sizeof(buf), AV_CHAN_FRONT_LEFT);
    printf("With AV_CHAN_FRONT_LEFT: %27s\n", buf);
    av_channel_name(buf, sizeof(buf), AV_CHAN_FRONT_RIGHT);
    printf("With AV_CHAN_FRONT_RIGHT: %26s\n", buf);
    av_channel_name(buf, sizeof(buf), AV_CHAN_AMBISONIC_BASE);
    printf("With AV_CHAN_AMBISONIC_BASE: %23s\n", buf);
    av_channel_name(buf, sizeof(buf), AV_CHAN_AMBISONIC_END);
    printf("With AV_CHAN_AMBISONIC_END: %24s\n", buf);

    printf("Testing av_channel_description\n");
    av_channel_description(buf, sizeof(buf), AV_CHAN_FRONT_LEFT);
    printf("With AV_CHAN_FRONT_LEFT: %27s\n", buf);
    av_channel_description(buf, sizeof(buf), AV_CHAN_FRONT_RIGHT);
    printf("With AV_CHAN_FRONT_RIGHT: %26s\n", buf);
    av_channel_description(buf, sizeof(buf), AV_CHAN_AMBISONIC_BASE);
    printf("With AV_CHAN_AMBISONIC_BASE: %23s\n", buf);
    av_channel_description(buf, sizeof(buf), AV_CHAN_AMBISONIC_END);
    printf("With AV_CHAN_AMBISONIC_END: %24s\n", buf);

    printf("\nTesting av_channel_from_string\n");
    printf("With \"FL\": %41d\n", av_channel_from_string("FL"));
    printf("With \"FR\": %41d\n", av_channel_from_string("FR"));
    printf("With \"ambisonic 0\": %32d\n", av_channel_from_string("ambisonic 0"));
    printf("With \"ambisonic 1023\": %29d\n", av_channel_from_string("ambisonic 1023"));

    printf("\nTesting av_channel_layout_from_string\n");
    CHANNEL_LAYOUT_FROM_STRING(surround, "0x3f");
    printf("With \"0x3f\": %39s\n", buf);
    CHANNEL_LAYOUT_FROM_STRING(surround, "6c");
    printf("With \"6c\": %41s\n", buf);
    CHANNEL_LAYOUT_FROM_STRING(surround, "6");
    printf("With \"6\": %42s\n", buf);
    CHANNEL_LAYOUT_FROM_STRING(surround, "6 channels");
    printf("With \"6 channels\": %33s\n", buf);
    CHANNEL_LAYOUT_FROM_STRING(surround, "FL|FR|FC|BL|BR|LFE");
    printf("With \"FL|FR|FC|BL|BR|LFE\": %25s\n", buf);
    CHANNEL_LAYOUT_FROM_STRING(surround, "5.1");
    printf("With \"5.1\": %40s\n", buf);
    CHANNEL_LAYOUT_FROM_STRING(surround, "FL|FR|FC|SL|SR|LFE");
    printf("With \"FL|FR|FC|SL|SR|LFE\": %25s\n", buf);
    CHANNEL_LAYOUT_FROM_STRING(surround, "5.1(side)");
    printf("With \"5.1(side)\": %34s\n", buf);

    printf("\n==Native layouts==\n");

    printf("\nTesting av_channel_layout_from_mask\n");
    CHANNEL_LAYOUT_FROM_MASK(surround, AV_CH_LAYOUT_5POINT1);
    printf("With AV_CH_LAYOUT_5POINT1: %25s\n", buf);

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

    custom.order = AV_CHANNEL_ORDER_CUSTOM;
    custom.nb_channels = 6;
    custom.u.map = av_calloc(6, sizeof(*custom.u.map));
    if (!custom.u.map)
        return 1;
    custom.u.map[0].id = AV_CHAN_AMBISONIC_BASE;
    custom.u.map[1].id = AV_CHAN_AMBISONIC_BASE + 1;
    custom.u.map[2].id = AV_CHAN_AMBISONIC_BASE + 2;
    custom.u.map[3].id = AV_CHAN_AMBISONIC_BASE + 3;
    custom.u.map[4].id = AV_CHAN_FRONT_RIGHT;
    custom.u.map[5].id = AV_CHAN_FRONT_LEFT;
    buf[0] = 0;
    printf("\nTesting av_channel_layout_describe\n");
    av_channel_layout_describe(&custom, buf, sizeof(buf));
    printf("On \"ambisonic 1|FR|FL\" layout: %21s\n", buf);

    custom.nb_channels = 3;
    custom.u.map[0].id = AV_CHAN_FRONT_RIGHT;
    custom.u.map[1].id = AV_CHAN_FRONT_LEFT;
    custom.u.map[2].id = 63;
    av_channel_layout_describe(&custom, buf, sizeof(buf));
    printf("On \"FR|FL|Ch63\" layout: %28s\n", buf);

    printf("\nTesting av_channel_layout_index_from_string\n");
    CHANNEL_LAYOUT_INDEX_FROM_STRING(custom, "FR");
    printf("On \"FR|FL|Ch63\" layout with \"FR\": %18d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_STRING(custom, "FL");
    printf("On \"FR|FL|Ch63\" layout with \"FL\": %18d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_STRING(custom, "BC");
    printf("On \"FR|FL|Ch63\" layout with \"BC\": %18d\n", ret);

    printf("\nTesting av_channel_layout_channel_from_string\n");
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(custom, "FR");
    printf("On \"FR|FL|Ch63\" layout with \"FR\": %18d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(custom, "FL");
    printf("On \"FR|FL|Ch63\" layout with \"FL\": %18d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_STRING(custom, "BC");
    printf("On \"FR|FL|Ch63\" layout with \"BC\": %18d\n", ret);

    printf("\nTesting av_channel_layout_index_from_channel\n");
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(custom, AV_CHAN_FRONT_RIGHT);
    printf("On \"FR|FL|Ch63\" layout with AV_CHAN_FRONT_RIGHT: %3d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(custom, AV_CHAN_FRONT_LEFT);
    printf("On \"FR|FL|Ch63\" layout with AV_CHAN_FRONT_LEFT: %4d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(custom, 63);
    printf("On \"FR|FL|Ch63\" layout with 63: %20d\n", ret);
    CHANNEL_LAYOUT_INDEX_FROM_CHANNEL(custom, AV_CHAN_BACK_CENTER);
    printf("On \"FR|FL|Ch63\" layout with AV_CHAN_BACK_CENTER: %3d\n", ret);

    printf("\nTesting av_channel_layout_channel_from_index\n");
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(custom, 0);
    printf("On \"FR|FL|Ch63\" layout with 0: %21d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(custom, 1);
    printf("On \"FR|FL|Ch63\" layout with 1: %21d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(custom, 2);
    printf("On \"FR|FL|Ch63\" layout with 2: %21d\n", ret);
    CHANNEL_LAYOUT_CHANNEL_FROM_INDEX(custom, 3);
    printf("On \"FR|FL|Ch63\" layout with 3: %21d\n", ret);
    av_channel_layout_uninit(&custom);

    printf("\n==Ambisonic layouts==\n");

    custom.order = AV_CHANNEL_ORDER_AMBISONIC;
    custom.nb_channels = 4;
    printf("\nTesting av_channel_layout_describe\n");
    av_channel_layout_describe(&custom, buf, sizeof(buf));
    printf("On \"ambisonic 1\" layout: %27s\n", buf);
    custom.nb_channels = 11;
    custom.u.mask = AV_CH_LAYOUT_STEREO;
    av_channel_layout_describe(&custom, buf, sizeof(buf));
    printf("On \"ambisonic 2|stereo\" layout: %20s\n", buf);

    av_channel_layout_uninit(&surround);
    av_channel_layout_uninit(&custom);

    return 0;
}
