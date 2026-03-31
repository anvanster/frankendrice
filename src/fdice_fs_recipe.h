// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_FS_RECIPE_H_
#define _FDICE_FS_RECIPE_H_

/**
 * DOC: FrankenDrICE Recipe Manager Header
 *
 * E810 switch recipe lifecycle management with hash-based sharing.
 * Manages the limited set of 64 hardware recipes, allowing multiple
 * flow groups with identical match criteria to share a single recipe.
 *
 * Architecture reference: docs/architecture/04-flow-steering.md
 */

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/bitmap.h>
#include <linux/refcount.h>

#include "fdice_fs_core.h"

/* Forward declarations */
struct fdice_dev;

/**
 * struct fdice_recipe - Single E810 switch recipe
 * @recipe_id: hardware recipe index (0-63)
 * @refcount: shared reference count across flow groups
 * @mask: extraction pattern this recipe implements
 * @hash_node: linkage in recipe manager's match hashtable
 * @hw_programmed: true if recipe has been sent to firmware via AQ
 */
struct fdice_recipe {
	u16			recipe_id;
	refcount_t		refcount;
	struct fdice_match_criteria mask;
	struct hlist_node	hash_node;
	bool			hw_programmed;
};

/**
 * struct fdice_recipe_mgr - E810 recipe lifecycle manager
 * @dev: backpointer to core device
 * @lock: serializes recipe allocation and release
 * @recipes: recipe array indexed by recipe ID
 * @match_ht: hash table for O(1) match-criteria-to-recipe lookup
 * @used_recipes: bitmap tracking allocated recipe slots
 * @num_used: count of currently allocated recipes
 *
 * Centralizes recipe management to prevent the redundant allocation
 * that occurs when multiple subsystems need the same match pattern.
 */
struct fdice_recipe_mgr {
	struct fdice_dev	*dev;
	struct mutex		lock;

	struct fdice_recipe	recipes[FDICE_MAX_RECIPES];

	DECLARE_HASHTABLE(match_ht, 6);

	DECLARE_BITMAP(used_recipes, FDICE_MAX_RECIPES);
	u16			num_used;
};

/* -----------------------------------------------------------------------
 * Recipe Manager API
 * ----------------------------------------------------------------------- */

int fdice_recipe_mgr_init(struct fdice_recipe_mgr *mgr,
			   struct fdice_dev *fdev);
void fdice_recipe_mgr_cleanup(struct fdice_recipe_mgr *mgr);

struct fdice_recipe *
fdice_recipe_alloc(struct fdice_recipe_mgr *mgr,
		   struct fdice_match_criteria *mask);

void fdice_recipe_release(struct fdice_recipe_mgr *mgr,
			  struct fdice_recipe *recipe);

u32 fdice_recipe_hash(const struct fdice_match_criteria *mask);

bool fdice_recipe_match(const struct fdice_match_criteria *a,
			const struct fdice_match_criteria *b);

#endif /* _FDICE_FS_RECIPE_H_ */
