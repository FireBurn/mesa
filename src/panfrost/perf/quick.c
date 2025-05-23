#include <xf86drm.h>
#include <stdio.h>
#include <lib/kmod/pan_kmod.h>
#include <lib/pan_props.h>
#include "pan_perf.h"

int
main(void)
{
   int fd = drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER);

   if (fd < 0) {
      fprintf(stderr, "No panfrost device\n");
      exit(1);
   }

   void *ctx = ralloc_context(NULL);
   struct pan_perf *perf = rzalloc(ctx, struct pan_perf);

   pan_perf_init(perf, fd);

   int ret = pan_perf_enable(perf);

   if (ret < 0) {
      fprintf(stderr, "failed to enable counters (%d)\n", ret);
      fprintf(
         stderr,
         "try `# echo Y > /sys/module/panfrost/parameters/unstable_ioctls`\n");

      exit(1);
   }

   sleep(1);

   pan_perf_dump(perf);

   for (unsigned i = 0; i < perf->cfg->n_categories; ++i) {
      const struct pan_perf_category *cat = &perf->cfg->categories[i];
      printf("%s\n", cat->name);

      for (unsigned j = 0; j < cat->n_counters; ++j) {
         const struct pan_perf_counter *ctr = &cat->counters[j];
         uint32_t val = pan_perf_counter_read(ctr, perf);
         printf("%s (%s): %u\n", ctr->name, ctr->symbol_name, val);
      }

      printf("\n");
   }

   if (pan_perf_disable(perf) < 0) {
      fprintf(stderr, "failed to disable counters\n");
      exit(1);
   }

   return 0;
}
