/*
 * Copyright © 2018 Jonathan Marek <jonathan@marek.ca>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Jonathan Marek <jonathan@marek.ca>
 */

#ifndef FD2_RESOURCE_H_
#define FD2_RESOURCE_H_

#include "freedreno_resource.h"

uint32_t fd2_layout_resource(struct fd_resource *rsc, enum fd_layout_type type);
unsigned fd2_tile_mode(const struct pipe_resource *tmpl);

#endif /* FD2_RESOURCE_H_ */
