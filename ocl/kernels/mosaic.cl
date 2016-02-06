/*
 *  mosaic.cl - mosaic filter on mask region
 *
 *  Copyright (C) 2016 Intel Corporation
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

__kernel void mosaic(__write_only image2d_t img_y_dst,
                     __read_only image2d_t img_y,
                     __write_only image2d_t img_uv_dst,
                     __read_only image2d_t img_uv,
                     uint crop_x,
                     uint crop_y,
                     __constant uchar* mask,
                     uint blk_size)
{
    sampler_t sampler = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;
    size_t g_id_x = get_global_id(0);
    size_t g_id_y = get_global_id(1);

    uint4 Y = read_imageui(img_y, sampler, (int2)(g_id_x / blk_size * blk_size + crop_x, g_id_y * blk_size + crop_y));
    for (uint i = 0; i < blk_size; i++) {
        write_imageui(img_y_dst, (int2)(g_id_x + crop_x, g_id_y * blk_size + i + crop_y), Y);
    }

    uint4 U = read_imageui(img_uv, sampler, (int2)((g_id_x / blk_size * blk_size + crop_x) / 2 * 2, (g_id_y * blk_size + crop_y) / 2));
    uint4 V = read_imageui(img_uv, sampler, (int2)((g_id_x / blk_size * blk_size + crop_x) / 2 * 2 + 1, (g_id_y * blk_size + crop_y) / 2));
    for (uint i = 0; i < blk_size; i++) {
        write_imageui(img_uv_dst, (int2)((g_id_x + crop_x) / 2 * 2, (g_id_y * blk_size + i + crop_y) / 2), U);
        write_imageui(img_uv_dst, (int2)((g_id_x + crop_x) / 2 * 2 + 1, (g_id_y * blk_size + i + crop_y) / 2), V);
    }
}
