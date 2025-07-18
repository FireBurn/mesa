/*
 * Copyright © 2019 Google, Inc.
 * SPDX-License-Identifier: MIT
 */

#ifndef FREEDRENO_LAYOUT_H_
#define FREEDRENO_LAYOUT_H_

#include <stdbool.h>
#include <stdint.h>

#include "util/format/u_format.h"
#include "util/u_debug.h"
#include "util/u_math.h"

#include "common/freedreno_common.h"
#include "common/freedreno_dev_info.h"

BEGINC;

/* Shared freedreno mipmap layout helper
 *
 * It does *not* attempt to track surface transitions, in particular
 * about UBWC state.  Possibly it should, but
 *  (a) I'm not sure if in all cases we can transparently do in-
 *      place transitions (ie. a5xx textures with interleaved
 *      meta and pixel data
 *  (b) Even if we can, we probably can't assume that we have
 *      figured out yet how to do in-place transition for every
 *      generation.
 */

/* Texture Layout on a3xx:
 * -----------------------
 *
 * Each mipmap-level contains all of it's layers (ie. all cubmap
 * faces, all 1d/2d array elements, etc).  The texture sampler is
 * programmed with the start address of each mipmap level, and hw
 * derives the layer offset within the level.
 *
 *
 * Texture Layout on a4xx+:
 * -----------------------
 *
 * For cubemap and 2d array, each layer contains all of it's mipmap
 * levels (layer_first layout).
 *
 * 3d textures are laid out as on a3xx.
 *
 * In either case, the slice represents the per-miplevel information,
 * but in layer_first layout it only includes the first layer, and
 * an additional offset of (rsc->layer_size * layer) must be added.
 *
 *
 * UBWC Color Compressions (a5xx+):
 * -------------------------------
 *
 * Color compression is only supported for tiled layouts.  In general
 * the meta "flag" buffer (ie. what holds the compression state for
 * each block) can be separate from the color data, except for textures
 * on a5xx where it needs to be interleaved with layers/levels of a
 * texture.
 */

#define FDL_MAX_MIP_LEVELS 15

struct fdl_slice {
   uint32_t offset; /* offset of first layer in slice */
   uint32_t size0;  /* size of first layer in slice */
};

/* parameters for explicit (imported) layout */
struct fdl_explicit_layout {
   uint32_t offset;
   uint32_t pitch;
};

/**
 * General layout params for images.
 */
struct fdl_image_params {
   enum pipe_format format;
   uint32_t nr_samples;
   uint32_t width0;
   uint32_t height0;
   uint32_t depth0;
   uint32_t mip_levels;
   uint32_t array_size;

   /**
    * Preferred tile mode, may be overriden by layout constraints.
    */
   uint32_t tile_mode;

   /**
    * UBWC preferred, may be overriden
    */
   bool ubwc;

   /**
    * Force UBWC, even when below the minimum width.
    */
   bool force_ubwc;

   bool is_3d;
   bool is_mutable;
};

/**
 * Metadata shared between vk and gallium driver for interop.
 *
 * NOTE: EXT_external_objects requires app to check device and driver
 * UUIDs to ensure that the vk and gl driver are compatible.  So for
 * now we don't need any additional versioning of the metadata.
 */
struct fdl_metadata {
   uint64_t modifier;
};

/**
 * Encapsulates the layout of a resource, including position of given 2d
 * surface (layer, level) within.  Or rather all the information needed
 * to derive this.
 */
struct fdl_layout {
   struct fdl_slice slices[FDL_MAX_MIP_LEVELS];
   struct fdl_slice ubwc_slices[FDL_MAX_MIP_LEVELS];
   uint32_t pitch0;
   uint32_t ubwc_width0;
   uint64_t layer_size;
   uint64_t ubwc_layer_size; /* in bytes */
   bool ubwc : 1;
   bool layer_first : 1; /* see above description */
   bool tile_all : 1;
   bool is_mutable : 1;

   /* Note that for tiled textures, beyond a certain mipmap level (ie.
    * when width is less than block size) things switch to linear.  In
    * general you should not directly look at fdl_layout::tile_mode,
    * but instead use fdl_surface::tile_mode which will correctly take
    * this into account.
    */
   uint32_t tile_mode : 2;
   /* Bytes per pixel (where a "pixel" is a single row of a block in the case
    * of compression), including each sample in the case of multisample
    * layouts.
    */
   uint8_t cpp;

   /**
    * Left shift necessary to multiply by cpp.  Invalid for NPOT cpp, please
    * use fdl_cpp_shift() to sanity check you aren't hitting that case.
    */
   uint8_t cpp_shift;

   uint32_t width0, height0, depth0;
   uint32_t mip_levels;
   uint32_t nr_samples;
   enum pipe_format format;

   uint64_t size;       /* Size of the whole image, in bytes. */
   uint32_t base_align; /* Alignment of the base address, in bytes. */
   uint8_t pitchalign;  /* log2(pitchalign) */
};

static inline uint32_t
fdl_cpp_shift(const struct fdl_layout *layout)
{
   assert(util_is_power_of_two_or_zero(layout->cpp));
   return layout->cpp_shift;
}

static inline uint32_t
fdl_pitch(const struct fdl_layout *layout, unsigned level)
{
   return align(u_minify(layout->pitch0, level), 1 << layout->pitchalign);
}

#define RGB_TILE_WIDTH_ALIGNMENT  64
#define RGB_TILE_HEIGHT_ALIGNMENT 16
#define UBWC_PLANE_SIZE_ALIGNMENT 4096

static inline uint32_t
fdl_ubwc_pitch(const struct fdl_layout *layout, unsigned level)
{
   if (!layout->ubwc)
      return 0;
   return align(u_minify(layout->ubwc_width0, level), RGB_TILE_WIDTH_ALIGNMENT);
}

static inline uint32_t
fdl_layer_stride(const struct fdl_layout *layout, unsigned level)
{
   if (layout->layer_first)
      return layout->layer_size;
   else
      return layout->slices[level].size0;
}

/* a2xx is special and needs PoT alignment for mipmaps: */
static inline uint32_t
fdl2_pitch(const struct fdl_layout *layout, unsigned level)
{
   uint32_t pitch = fdl_pitch(layout, level);
   if (level)
      pitch = util_next_power_of_two(pitch);
   return pitch;
}

static inline uint32_t
fdl2_pitch_pixels(const struct fdl_layout *layout, unsigned level)
{
   return fdl2_pitch(layout, level) >> fdl_cpp_shift(layout);
}

static inline uint32_t
fdl_surface_offset(const struct fdl_layout *layout, unsigned level,
                   unsigned layer)
{
   const struct fdl_slice *slice = &layout->slices[level];
   return slice->offset + fdl_layer_stride(layout, level) * layer;
}

static inline uint32_t
fdl_ubwc_offset(const struct fdl_layout *layout, unsigned level, unsigned layer)
{
   const struct fdl_slice *slice = &layout->ubwc_slices[level];
   return slice->offset + layer * layout->ubwc_layer_size;
}

/* Minimum layout width to enable UBWC. */
#define FDL_MIN_UBWC_WIDTH 16

static inline bool
fdl_level_linear(const struct fdl_layout *layout, int level)
{
   if (layout->tile_all)
      return false;

   unsigned w = u_minify(layout->width0, level);
   if (w < FDL_MIN_UBWC_WIDTH)
      return true;

   return false;
}

static inline uint32_t
fdl_tile_mode(const struct fdl_layout *layout, int level)
{
   if (layout->tile_mode && fdl_level_linear(layout, level))
      return 0; /* linear */
   else
      return layout->tile_mode;
}

static inline bool
fdl_ubwc_enabled(const struct fdl_layout *layout, int level)
{
   return layout->ubwc && !fdl_level_linear(layout, level);
}

const char *fdl_tile_mode_desc(const struct fdl_layout *layout, int level);

void fdl_layout_buffer(struct fdl_layout *layout, uint32_t size);

void fdl5_layout_image(struct fdl_layout *layout, const struct fdl_image_params *params);
bool fdl6_layout_image(struct fdl_layout *layout, const struct fd_dev_info *info,
                       const struct fdl_image_params *params,
                       const struct fdl_explicit_layout *explicit_layout);

static inline void
fdl_set_pitchalign(struct fdl_layout *layout, unsigned pitchalign)
{
   uint32_t nblocksx = util_format_get_nblocksx(layout->format, layout->width0);
   layout->pitchalign = pitchalign;
   layout->pitch0 = align(nblocksx * layout->cpp, 1 << pitchalign);
}

void fdl_dump_layout(struct fdl_layout *layout);

void fdl6_get_ubwc_blockwidth(const struct fdl_layout *layout,
                              uint32_t *blockwidth, uint32_t *blockheight);

enum fdl_view_type {
   FDL_VIEW_TYPE_1D = 0,
   FDL_VIEW_TYPE_2D = 1,
   FDL_VIEW_TYPE_CUBE = 2,
   FDL_VIEW_TYPE_3D = 3,
   FDL_VIEW_TYPE_BUFFER = 4,
};

enum fdl_chroma_location {
   FDL_CHROMA_LOCATION_COSITED_EVEN = 0,
   FDL_CHROMA_LOCATION_MIDPOINT = 1,
};

struct fdl_view_args {
   uint32_t chip;
   uint64_t iova;
   uint32_t base_miplevel;
   uint32_t level_count;
   uint32_t base_array_layer;
   uint32_t layer_count;
   float min_lod_clamp;
   unsigned char swiz[4];
   enum pipe_format format;
   enum fdl_view_type type;
   enum fdl_chroma_location chroma_offsets[2];
};

#define FDL6_TEX_CONST_DWORDS 16

struct fdl6_view {
   uint64_t base_addr;
   uint64_t ubwc_addr;
   uint32_t layer_size;
   uint32_t ubwc_layer_size;

   uint32_t offset;

   uint32_t width, height;
   bool need_y2_align;

   bool ubwc_enabled;
   bool is_mutable;
   uint8_t color_swap;

   enum pipe_format format;

   uint32_t descriptor[FDL6_TEX_CONST_DWORDS];

   /* Descriptor for use as a storage image as opposed to a sampled image.
    * This has a few differences for cube maps (e.g. type).
    */
   uint32_t storage_descriptor[FDL6_TEX_CONST_DWORDS];

   uint32_t pitch;

   /* pre-filled register values */
   uint32_t FLAG_BUFFER_PITCH;

   uint32_t RB_MRT_BUF_INFO;
   uint32_t SP_PS_MRT_REG;

   uint32_t TPL1_A2D_SRC_TEXTURE_INFO;
   uint32_t TPL1_A2D_SRC_TEXTURE_SIZE;

   uint32_t RB_A2D_DEST_BUFFER_INFO;

   uint32_t RB_RESOLVE_SYSTEM_BUFFER_INFO;

   uint32_t GRAS_LRZ_VIEW_INFO;
};

void
fdl6_view_init(struct fdl6_view *view, const struct fdl_layout **layouts,
               const struct fdl_view_args *args, bool has_z24uint_s8uint);
void
fdl6_buffer_view_init(uint32_t *descriptor, enum pipe_format format,
                      const uint8_t *swiz, uint64_t iova, uint32_t size);

void
fdl6_format_swiz(enum pipe_format format, bool has_z24uint_s8uint,
                 unsigned char *format_swiz);

enum fdl_macrotile_mode {
   FDL_MACROTILE_4_CHANNEL,
   FDL_MACROTILE_8_CHANNEL,
   /* Used internally by turnip */
   FDL_MACROTILE_INVALID = ~0,
};

/* Parameters that affect UBWC swizzling. Note that because we don't handle
 * compression, this isn't a complete set of knobs. See the documentation in
 * fd6_tiled_memcpy.c for a description of each one.
 */
struct fdl_ubwc_config {
   unsigned highest_bank_bit;
   unsigned bank_swizzle_levels;
   enum fdl_macrotile_mode macrotile_mode;
};

void
fdl6_memcpy_linear_to_tiled(uint32_t x_start, uint32_t y_start,
                            uint32_t width, uint32_t height,
                            char *dst, const char *src,
                            const struct fdl_layout *dst_layout,
                            unsigned dst_miplevel,
                            uint32_t src_pitch,
                            const struct fdl_ubwc_config *config);

void
fdl6_memcpy_tiled_to_linear(uint32_t x_start, uint32_t y_start,
                            uint32_t width, uint32_t height,
                            char *dst, const char *src,
                            const struct fdl_layout *src_layout,
                            unsigned src_miplevel,
                            uint32_t dst_pitch,
                            const struct fdl_ubwc_config *config);

ENDC;

#endif /* FREEDRENO_LAYOUT_H_ */
