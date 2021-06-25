// Copyright 2014 Carnegie Mellon University
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __SHM_H__
#define __SHM_H__

#pragma once

#include "common.h"
#include <linux/limits.h>

MEHCACHED_BEGIN

struct mehcached_shm_page
{
	char path[PATH_MAX];
	void *addr;
	void *paddr;
	size_t numa_node;
	size_t in_use;
};

void mehcached_shm_set_dma_map(int flag);

void
mehcached_shm_map_all(int port_id);

size_t
mehcached_shm_adjust_size(size_t size);

void
mehcached_shm_dump_page_info();

void
mehcached_shm_init(size_t page_size, size_t num_numa_nodes, size_t num_pages_to_try, size_t num_pages_to_reserve);

void *
mehcached_shm_find_free_address(size_t size);

size_t
mehcached_shm_alloc(size_t length, size_t numa_node);

int
mehcached_shm_schedule_remove(size_t entry_id);

int
mehcached_shm_map(size_t entry_id, void *ptr, size_t offset, size_t length);

int
mehcached_shm_unmap(void *ptr);

size_t
mehcached_shm_get_page_size();

size_t
mehcached_shm_get_memuse();

void *
mehcached_shm_malloc_contiguous(size_t size, size_t numa_node);

void *
mehcached_shm_malloc_contiguous_local(size_t size);

void
mehcached_shm_free_contiguous(void *ptr);

void *
mehcached_shm_malloc_striped(size_t size);

void
mehcached_shm_free_striped(void *ptr);

MEHCACHED_END

#endif
