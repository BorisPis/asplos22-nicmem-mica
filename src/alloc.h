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

#pragma once

#include "common.h"

MEHCACHED_BEGIN

struct mehcached_alloc_item
{
	struct {
		uint32_t valid     : 1;
		uint32_t nicmem    : 1;
		uint32_t item_size : 30;		// XXX: isn't this breaking 8-byte alignment?
	};
	//uint64_t item_size;		// XXX: isn't this breaking 8-byte alignment?
	uint32_t refcount;
	uint8_t data[0];
};

MEHCACHED_END

