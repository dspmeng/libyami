/*
 *  blend.cl - alpha blending opencl kernel
 *
 *  Copyright (C) 2015 Intel Corporation
 *    Author: Jia Meng<jia.meng@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */

__kernel void blend(__write_only image2d_t dst_y,
                    __write_only image2d_t dst_uv,
                    __read_only image2d_t bg_y,
                    __read_only image2d_t bg_uv,
                    __read_only image2d_t fg,
                    int padding,
                    int crop_x, int crop_y, int crop_w, int crop_h)
{
    sampler_t sampler = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;
    int i;
    int id_x = get_global_id(0);
    int id_y = get_global_id(1) * 2;
    int id_z = id_x * 4 - padding;
    int id_w = id_y;

    float4 y1, y2;
    float4 y1_dst, y2_dst;
    float4 uv, uv_dst;
    float4 rgba[8];
    float4 alpha;

    id_x += crop_x;
    id_y += crop_y;
    y1 = read_imagef(bg_y, sampler, (int2)(id_x, id_y));
    y2 = read_imagef(bg_y, sampler, (int2)(id_x, id_y + 1));
    uv = read_imagef(bg_uv, sampler, (int2)(id_x, id_y / 2));

    rgba[0] = read_imagef(fg, sampler, (int2)(id_z    , id_w));
    rgba[1] = read_imagef(fg, sampler, (int2)(id_z + 1, id_w));
    rgba[2] = read_imagef(fg, sampler, (int2)(id_z + 2, id_w));
    rgba[3] = read_imagef(fg, sampler, (int2)(id_z + 3, id_w));
    rgba[4] = read_imagef(fg, sampler, (int2)(id_z    , id_w + 1));
    rgba[5] = read_imagef(fg, sampler, (int2)(id_z + 1, id_w + 1));
    rgba[6] = read_imagef(fg, sampler, (int2)(id_z + 2, id_w + 1));
    rgba[7] = read_imagef(fg, sampler, (int2)(id_z + 3, id_w + 1));

    // handle padding area
    if (id_z < 0 || id_z >= crop_w) {
        rgba[0].w = 0;
        rgba[1].w = 0;
        rgba[4].w = 0;
        rgba[5].w = 0;
    }
    if (id_z + 2 < 0 || id_z + 2 >= crop_w) {
        rgba[2].w = 0;
        rgba[3].w = 0;
        rgba[6].w = 0;
        rgba[7].w = 0;
    }
    if (id_w >= crop_h) {
        for (i = 0; i < 8; i++) rgba[i].w = 0;
    }

    alpha = (float4)(rgba[0].w, rgba[1].w, rgba[2].w, rgba[3].w);
    y1_dst = 0.299 * (float4)(rgba[0].x, rgba[1].x, rgba[2].x, rgba[3].x);
    y1_dst = mad(0.587, (float4)(rgba[0].y, rgba[1].y, rgba[2].y, rgba[3].y), y1_dst);
    y1_dst = mad(0.114, (float4)(rgba[0].z, rgba[1].z, rgba[2].z, rgba[3].z), y1_dst);
    y1_dst *= alpha;
    y1_dst = mad(1 - alpha, y1, y1_dst);

    alpha = (float4)(rgba[4].w, rgba[5].w, rgba[6].w, rgba[7].w);
    y2_dst = 0.299 * (float4)(rgba[4].x, rgba[5].x, rgba[6].x, rgba[7].x);
    y2_dst = mad(0.587, (float4)(rgba[4].y, rgba[5].y, rgba[6].y, rgba[7].y), y2_dst);
    y2_dst = mad(0.114, (float4)(rgba[4].z, rgba[5].z, rgba[6].z, rgba[7].z), y2_dst);
    y2_dst *= alpha;
    y2_dst = mad(1 - alpha, y2, y2_dst);

    uv_dst.x = rgba[0].w * (-0.14713 * rgba[0].x - 0.28886 * rgba[0].y + 0.43600 * rgba[0].z + 0.5);
    uv_dst.y = rgba[0].w * ( 0.61500 * rgba[0].x - 0.51499 * rgba[0].y - 0.10001 * rgba[0].z + 0.5);
    uv_dst.z = rgba[2].w * (-0.14713 * rgba[2].x - 0.28886 * rgba[2].y + 0.43600 * rgba[2].z + 0.5);
    uv_dst.w = rgba[2].w * ( 0.61500 * rgba[2].x - 0.51499 * rgba[2].y - 0.10001 * rgba[2].z + 0.5);
    uv_dst.x = mad(1 - rgba[0].w, uv.x, uv_dst.x);
    uv_dst.y = mad(1 - rgba[0].w, uv.y, uv_dst.y);
    uv_dst.z = mad(1 - rgba[2].w, uv.z, uv_dst.z);
    uv_dst.w = mad(1 - rgba[2].w, uv.w, uv_dst.w);

    write_imagef(dst_y, (int2)(id_x, id_y), y1_dst);
    write_imagef(dst_y, (int2)(id_x, id_y + 1), y2_dst);
    write_imagef(dst_uv, (int2)(id_x, id_y / 2), uv_dst);
}
