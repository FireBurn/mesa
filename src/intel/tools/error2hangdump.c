/*
 * Copyright © 2022 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <assert.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <zlib.h>

#include "util/list.h"

#include "error_decode_lib.h"
#include "error2hangdump_lib.h"
#include "error2hangdump_xe.h"
#include "intel/dev/intel_device_info.h"

#define XE_KMD_ERROR_DUMP_IDENTIFIER "**** Xe Device Coredump ****"

static void
print_help(const char *progname, FILE *file)
{
   fprintf(file,
           "Usage: %s [OPTION]... [FILE]\n"
           "Convert an Intel GPU i915 error state to a hang dump file, replayable with intel_hang_replay.\n"
           "  -h, --help          display this help and exit\n"
           "  -o, --output=FILE   the output dump file (default FILE.dmp)\n",
           progname);
}

struct bo {
   enum address_space {
      PPGTT,
      GGTT,
   } gtt;
   enum bo_type {
      BO_TYPE_UNKNOWN = 0,
      BO_TYPE_BATCH,
      BO_TYPE_USER,
      BO_TYPE_CONTEXT,
      BO_TYPE_RINGBUFFER,
      BO_TYPE_STATUS,
      BO_TYPE_CONTEXT_WA,
   } type;
   const char *name;
   uint64_t addr;
   uint8_t *data;
   uint64_t size;

   enum intel_engine_class engine_class;
   int engine_instance;

   struct list_head link;
};

static struct bo *
find_or_create(struct list_head *bo_list, uint64_t addr,
               enum address_space gtt,
               enum intel_engine_class engine_class,
               int engine_instance)
{
   list_for_each_entry(struct bo, bo_entry, bo_list, link) {
      if (bo_entry->addr == addr &&
          bo_entry->gtt == gtt &&
          bo_entry->engine_class == engine_class &&
          bo_entry->engine_instance == engine_instance)
         return bo_entry;
   }

   struct bo *new_bo = calloc(1, sizeof(*new_bo));
   new_bo->addr = addr;
   new_bo->gtt = gtt;
   new_bo->engine_class = engine_class;
   new_bo->engine_instance = engine_instance;
   list_addtail(&new_bo->link, bo_list);

   return new_bo;
}

static void
engine_from_name(const char *engine_name,
                 enum intel_engine_class *engine_class,
                 int *engine_instance)
{
   const struct {
      const char *match;
      enum intel_engine_class engine_class;
      bool parse_instance;
   } rings[] = {
      { "rcs", INTEL_ENGINE_CLASS_RENDER, true },
      { "vcs", INTEL_ENGINE_CLASS_VIDEO, true },
      { "vecs", INTEL_ENGINE_CLASS_VIDEO_ENHANCE, true },
      { "bcs", INTEL_ENGINE_CLASS_COPY, true },
      { "global", INTEL_ENGINE_CLASS_INVALID, false },
      { "render command stream", INTEL_ENGINE_CLASS_RENDER, false },
      { "blt command stream", INTEL_ENGINE_CLASS_COPY, false },
      { "bsd command stream", INTEL_ENGINE_CLASS_VIDEO, false },
      { "vebox command stream", INTEL_ENGINE_CLASS_VIDEO_ENHANCE, false },
      { NULL, INTEL_ENGINE_CLASS_INVALID },
   }, *r;

   for (r = rings; r->match; r++) {
      if (strncasecmp(engine_name, r->match, strlen(r->match)) == 0) {
         *engine_class = r->engine_class;
         if (r->parse_instance)
            *engine_instance = strtol(engine_name + strlen(r->match), NULL, 10);
         else
            *engine_instance = 0;
         return;
      }
   }

   fail("Unknown engine %s\n", engine_name);
}

static void
read_i915_data_file(FILE *err_file, FILE *hang_file, bool verbose, enum intel_engine_class capture_engine)
{
   enum address_space active_gtt = PPGTT;
   enum address_space default_gtt = PPGTT;

   int num_ring_bos = 0;

   struct list_head bo_list;
   list_inithead(&bo_list);

   struct bo *last_bo = NULL;

   enum intel_engine_class active_engine_class = INTEL_ENGINE_CLASS_INVALID;
   int active_engine_instance = -1;

   char *line = NULL;
   size_t line_size;
   while (getline(&line, &line_size, err_file) > 0) {
      if (strstr(line, " command stream:")) {
         engine_from_name(line, &active_engine_class, &active_engine_instance);
         continue;
      }

      if (num_ring_bos > 0) {
         unsigned hi, lo, size;
         if (sscanf(line, " %x_%x %d", &hi, &lo, &size) == 3) {
            struct bo *bo_entry = find_or_create(&bo_list, ((uint64_t)hi) << 32 | lo,
                                                 active_gtt,
                                                 active_engine_class,
                                                 active_engine_instance);
            bo_entry->size = size;
            num_ring_bos--;
         } else {
            fail("Not enough BO entries in the active table\n");
         }
         continue;
      }

      if (line[0] == ':' || line[0] == '~') {
         if (!last_bo || last_bo->type == BO_TYPE_UNKNOWN)
            continue;

         int count = ascii85_decode(line+1, (uint32_t **) &last_bo->data, line[0] == ':');
         fail_if(count == 0, "ASCII85 decode failed.\n");
         last_bo->size = count * 4;
         continue;
      }

      char *dashes = strstr(line, " --- ");
      if (dashes) {
         dashes += 5;

         engine_from_name(line, &active_engine_class, &active_engine_instance);

         uint32_t hi, lo;
         char *bo_address_str = strchr(dashes, '=');
         if (!bo_address_str || sscanf(bo_address_str, "= 0x%08x %08x\n", &hi, &lo) != 2)
            continue;

         const struct {
            const char *match;
            enum bo_type type;
            enum address_space gtt;
         } bo_types[] = {
            { "gtt_offset", BO_TYPE_BATCH,      default_gtt },
            { "batch",      BO_TYPE_BATCH,      default_gtt },
            { "user",       BO_TYPE_USER,       default_gtt },
            { "HW context", BO_TYPE_CONTEXT,    GGTT },
            { "ringbuffer", BO_TYPE_RINGBUFFER, GGTT },
            { "HW Status",  BO_TYPE_STATUS,     GGTT },
            { "WA context", BO_TYPE_CONTEXT_WA, GGTT },
            { "unknown",    BO_TYPE_UNKNOWN,    GGTT },
         }, *b;

         for (b = bo_types; b->type != BO_TYPE_UNKNOWN; b++) {
            if (strncasecmp(dashes, b->match, strlen(b->match)) == 0)
               break;
         }

         last_bo = find_or_create(&bo_list, ((uint64_t) hi) << 32 | lo,
                                  b->gtt,
                                  active_engine_class, active_engine_instance);

         /* The batch buffer will appear twice as gtt_offset and user. Only
          * keep the batch type.
          */
         if (last_bo->type == BO_TYPE_UNKNOWN) {
            last_bo->type = b->type;
            last_bo->name = b->match;
         }

         continue;
      }
   }

   if (verbose) {
      fprintf(stdout, "BOs found:\n");
      list_for_each_entry(struct bo, bo_entry, &bo_list, link) {
         fprintf(stdout, "\t type=%i addr=0x%016" PRIx64 " size=%" PRIu64 "\n",
                 bo_entry->type, bo_entry->addr, bo_entry->size);
      }
   }

   /* Find the batch that trigger the hang */
   struct bo *batch_bo = NULL, *hw_image_bo = NULL;
   list_for_each_entry(struct bo, bo_entry, &bo_list, link) {
      if (batch_bo != NULL && hw_image_bo != NULL)
         break;

      if (bo_entry->engine_class != capture_engine)
         continue;

      switch (bo_entry->type) {
      case BO_TYPE_BATCH:
         batch_bo = bo_entry;
         break;
      case BO_TYPE_CONTEXT:
         hw_image_bo = bo_entry;
         break;
      default:
         break;
      }
   }
   fail_if(!batch_bo, "Failed to find batch buffer.\n");
   fail_if(!hw_image_bo, "Failed to find HW image buffer.\n");

   /* Add all the user BOs to the aub file */
   list_for_each_entry(struct bo, bo_entry, &bo_list, link) {
      if (bo_entry->type == BO_TYPE_USER && bo_entry->gtt == PPGTT)
         write_buffer(hang_file, bo_entry->addr, bo_entry->data, bo_entry->size, "user");
   }

   write_buffer(hang_file, batch_bo->addr, batch_bo->data, batch_bo->size, "batch");
   fprintf(stderr, "writing image buffer 0x%016"PRIx64" size=0x%016"PRIx64"\n",
           hw_image_bo->addr, hw_image_bo->size);
   write_hw_image_buffer(hang_file, hw_image_bo->data, hw_image_bo->size);
   write_exec(hang_file, batch_bo->addr);

   /* Cleanup */
   list_for_each_entry_safe(struct bo, bo_entry, &bo_list, link) {
      list_del(&bo_entry->link);
      free(bo_entry->data);
      free(bo_entry);
   }

   free(line);
}

int
main(int argc, char *argv[])
{
   int i, c;
   bool help = false, verbose = false;
   char *out_filename = NULL, *in_filename = NULL, *capture_engine_name = "rcs";
   const struct option aubinator_opts[] = {
      { "help",       no_argument,       NULL,     'h' },
      { "output",     required_argument, NULL,     'o' },
      { "verbose",    no_argument,       NULL,     'v' },
      { "engine",     required_argument, NULL,     'e' },
      { NULL,         0,                 NULL,     0 }
   };
   char *line = NULL;
   size_t line_size;

   i = 0;
   while ((c = getopt_long(argc, argv, "ho:v", aubinator_opts, &i)) != -1) {
      switch (c) {
      case 'h':
         help = true;
         break;
      case 'o':
         out_filename = strdup(optarg);
         break;
      case 'v':
         verbose = true;
         break;
      case 'e':
         capture_engine_name = optarg;
         break;
      default:
         break;
      }
   }

   if (optind < argc)
      in_filename = argv[optind++];

   if (help || argc == 1 || !in_filename) {
      print_help(argv[0], stderr);
      return in_filename ? EXIT_SUCCESS : EXIT_FAILURE;
   }

   enum intel_engine_class capture_engine;
   engine_from_name(capture_engine_name, &capture_engine, &c);

   if (out_filename == NULL) {
      int out_filename_size = strlen(in_filename) + 5;
      out_filename = malloc(out_filename_size);
      snprintf(out_filename, out_filename_size, "%s.dmp", in_filename);
   }

   FILE *err_file = fopen(in_filename, "r");
   fail_if(!err_file, "Failed to open error file \"%s\": %m\n", in_filename);

   FILE *hang_file = fopen(out_filename, "w");
   fail_if(!hang_file, "Failed to open aub file \"%s\": %m\n", out_filename);

   getline(&line, &line_size, err_file);
   rewind(err_file);
   if (strncmp(line, XE_KMD_ERROR_DUMP_IDENTIFIER, strlen(XE_KMD_ERROR_DUMP_IDENTIFIER)) == 0)
      read_xe_data_file(err_file, hang_file, verbose);
   else
      read_i915_data_file(err_file, hang_file, verbose, capture_engine);

   free(line);
   free(out_filename);
   if (err_file)
      fclose(err_file);
   fclose(hang_file);

   return EXIT_SUCCESS;
}

/* vim: set ts=8 sw=8 tw=0 cino=:0,(0 noet :*/
