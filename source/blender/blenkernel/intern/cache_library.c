/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2015 by NaN Holding BV.
 * All rights reserved.
 *
 * Contributor(s): Lukas Toenne
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/cache_library.c
 *  \ingroup bke
 */

#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_ghash.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_cache_library_types.h"
#include "DNA_group_types.h"
#include "DNA_object_types.h"
#include "DNA_particle_types.h"

#include "BKE_cache_library.h"
#include "BKE_global.h"
#include "BKE_library.h"
#include "BKE_main.h"

CacheLibrary *BKE_cache_library_add(Main *bmain, const char *name)
{
	CacheLibrary *cachelib;

	cachelib = BKE_libblock_alloc(bmain, ID_CL, name);

	BLI_strncpy(cachelib->filepath, "//cache/", sizeof(cachelib->filepath));

	return cachelib;
}

CacheLibrary *BKE_cache_library_copy(CacheLibrary *cachelib)
{
	CacheLibrary *cachelibn;
	
	cachelibn = BKE_libblock_copy(&cachelib->id);
	
	BLI_duplicatelist(&cachelibn->items, &cachelib->items);
	
	if (cachelib->id.lib) {
		BKE_id_lib_local_paths(G.main, cachelib->id.lib, &cachelibn->id);
	}
	
	return cachelibn;
}

void BKE_cache_library_free(CacheLibrary *cachelib)
{
	BLI_freelistN(&cachelib->items);
}

/* ========================================================================= */

static void cache_path_object(CacheItemPath *path, Object *ob)
{
	path->type = CACHE_TYPE_OBJECT;
	path->id = &ob->id;
	path->index = -1;
}

static void cache_path_derived_mesh(CacheItemPath *path, Object *ob)
{
	path->type = CACHE_TYPE_DERIVED_MESH;
	path->id = &ob->id;
	path->index = -1;
}

static void cache_path_hair(CacheItemPath *path, Object *ob, ParticleSystem *psys)
{
	path->type = CACHE_TYPE_HAIR;
	path->id = &ob->id;
	path->index = BLI_findindex(&ob->particlesystem, psys);
}

static void cache_path_hair_paths(CacheItemPath *path, Object *ob, ParticleSystem *psys)
{
	path->type = CACHE_TYPE_HAIR_PATHS;
	path->id = &ob->id;
	path->index = BLI_findindex(&ob->particlesystem, psys);
}

static void cache_path_copy(CacheItemPath *dst, const CacheItemPath *src)
{
	memcpy(dst, src, sizeof(CacheItemPath));
}

static void cache_library_walk_recursive(CacheLibrary *cachelib, CacheGroupWalkFunc walk, void *userdata, int level, Object *ob)
{
	CacheItemPath path;
	
	if (level > MAX_CACHE_GROUP_LEVEL)
		return;
	
	/* object dm */
	cache_path_object(&path, ob);
	walk(userdata, cachelib, &path);
	
	/* dupli group recursion */
	if ((ob->transflag & OB_DUPLIGROUP) && ob->dup_group) {
		GroupObject *gob;
		
		for (gob = ob->dup_group->gobject.first; gob; gob = gob->next) {
			cache_library_walk_recursive(cachelib, walk, userdata, level + 1, gob->ob);
		}
	}
}

void BKE_cache_library_walk(CacheLibrary *cachelib, CacheGroupWalkFunc walk, void *userdata)
{
	if (cachelib && cachelib->group) {
		GroupObject *gob;
		
		for (gob = cachelib->group->gobject.first; gob; gob = gob->next) {
			cache_library_walk_recursive(cachelib, walk, userdata, 0, gob->ob);
		}
	}
}

/* ========================================================================= */

BLI_INLINE unsigned int hash_int_2d(unsigned int kx, unsigned int ky)
{
#define rot(x,k) (((x)<<(k)) | ((x)>>(32-(k))))

	uint a, b, c;

	a = b = c = 0xdeadbeef + (2 << 2) + 13;
	a += kx;
	b += ky;

	c ^= b; c -= rot(b,14);
	a ^= c; a -= rot(c,11);
	b ^= a; b -= rot(a,25);
	c ^= b; c -= rot(b,16);
	a ^= c; a -= rot(c,4);
	b ^= a; b -= rot(a,14);
	c ^= b; c -= rot(b,24);

	return c;

#undef rot
}

static unsigned int cache_item_hash(const void *key)
{
	const CacheItemPath *path = key;
	unsigned int hash;
	
	hash = BLI_ghashutil_inthash(path->type);
	
	hash = hash_int_2d(hash, BLI_ghashutil_ptrhash(path->id));
	if (path->index >= 0)
		hash = hash_int_2d(hash, BLI_ghashutil_inthash(path->index));
	
	return hash;
}

static bool cache_item_cmp(const void *key_a, const void *key_b)
{
	const CacheItemPath *path_a = key_a, *path_b = key_b;
	
	if (path_a->type != path_b->type)
		return true;
	if (path_a->id != path_b->id)
		return true;
	if (path_a->index >= 0 || path_b->index >= 0) {
		if (path_a->index != path_b->index)
			return true;
	}
	
	return false;
}

static void cache_library_insert_item_hash(CacheLibrary *cachelib, CacheItem *item, bool replace)
{
	CacheItemPath *path = &item->path;
	CacheItem *exist = BLI_ghash_lookup(cachelib->items_hash, path);
	if (exist && replace) {
		BLI_remlink(&cachelib->items, exist);
		BLI_ghash_remove(cachelib->items_hash, path, NULL, NULL);
		MEM_freeN(exist);
	}
	if (!exist || replace)
		BLI_ghash_insert(cachelib->items_hash, path, item);
}

static void cache_library_ensure_items_hash(CacheLibrary *cachelib)
{
	CacheItem *item;
	
	if (cachelib->items_hash) {
		BLI_ghash_clear(cachelib->items_hash, NULL, NULL);
	}
	else {
		cachelib->items_hash = BLI_ghash_new(cache_item_hash, cache_item_cmp, "cache item hash");
	}
	
	for (item = cachelib->items.first; item; item = item->next) {
		cache_library_insert_item_hash(cachelib, item, true);
	}
}

CacheItem *BKE_cache_library_find_item(CacheLibrary *cachelib, const CacheItemPath *path)
{
	return BLI_ghash_lookup(cachelib->items_hash, path);
}

CacheItem *BKE_cache_library_add_item(CacheLibrary *cachelib, const CacheItemPath *path)
{
	CacheItem *item = BLI_ghash_lookup(cachelib->items_hash, path);
	
	if (!item) {
		item = MEM_callocN(sizeof(CacheItem), "cache library item");
		cache_path_copy(&item->path, path);
		
		BLI_addtail(&cachelib->items, item);
		cache_library_insert_item_hash(cachelib, item, false);
	}
	
	return item;
}

bool BKE_cache_library_remove_item(CacheLibrary *cachelib, const CacheItemPath *path)
{
	CacheItem *item = BLI_ghash_lookup(cachelib->items_hash, path);
	if (item) {
		BLI_ghash_remove(cachelib->items_hash, (CacheItemPath *)path, NULL, NULL);
		BLI_remlink(&cachelib->items, item);
		MEM_freeN(item);
		return true;
	}
	else
		return false;
}

/* ========================================================================= */

#if 0
typedef struct UpdateItemsData {
	CacheItem *cur;
} UpdateItemsData;

static void cache_library_update_items_walk(void *userdata, CacheLibrary *cachelib)
{
	UpdateItemsData *data = userdata;
	CacheItem *item;
	
	if (data->cur) {
		item = data->cur;
		data->cur = data->cur->next;
	}
	else {
		item = MEM_callocN(sizeof(CacheItem), "cache library item");
		BLI_addtail(&cachelib->items, item);
	}
}

void BKE_cache_library_update_items(CacheLibrary *cachelib)
{
	UpdateItemsData data;
	
	data.cur = cachelib->items.first;
	BKE_cache_library_walk(cachelib, cache_library_update_items_walk, &data);
	
	/* truncate items list */
	if (data.cur) {
		cachelib->items.last = data.cur->prev;
		while (data.cur) {
			CacheItem *item = data.cur;
			data.cur = data.cur->next;
			
			BLI_remlink(&cachelib->items, item);
			MEM_freeN(item);
		}
	}
}
#endif
