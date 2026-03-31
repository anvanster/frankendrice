// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE Recipe Manager
 *
 * Manages the limited set of E810 switch recipes (max 64). Implements
 * recipe sharing so that flow groups with identical match criteria bind
 * to the same hardware recipe rather than consuming a new slot.
 *
 * The sharing protocol uses a hash table keyed on match criteria content.
 * On allocation, the manager first checks for an existing recipe with
 * matching criteria. On release, the recipe is freed only when the last
 * reference drops.
 *
 * Architecture reference: docs/architecture/04-flow-steering.md
 */

#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/bitmap.h>
#include <linux/jhash.h>
#include <linux/device.h>

#include "fdice_dev.h"
#include "fdice_fs_core.h"
#include "fdice_fs_recipe.h"

/* -----------------------------------------------------------------------
 * Hash and Match Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_recipe_hash - Compute hash of match criteria for sharing lookup
 * @mask: match criteria to hash
 *
 * Uses Jenkins hash over the three criteria fields to produce a
 * distribution-friendly hash value for the recipe hashtable.
 *
 * Return: 32-bit hash value
 */
u32 fdice_recipe_hash(const struct fdice_match_criteria *mask)
{
	u32 val;

	val = jhash_3words(mask->outer_headers,
			   mask->inner_headers,
			   mask->misc,
			   0x46444943); /* "FDIC" seed */
	return val;
}

/**
 * fdice_recipe_match - Compare two match criteria for equality
 * @a: first match criteria
 * @b: second match criteria
 *
 * Two match criteria are equal when all three fields (outer_headers,
 * inner_headers, misc) are identical. This means the same extraction
 * pattern can be shared.
 *
 * Return: true if criteria are identical, false otherwise
 */
bool fdice_recipe_match(const struct fdice_match_criteria *a,
			const struct fdice_match_criteria *b)
{
	return a->outer_headers == b->outer_headers &&
	       a->inner_headers == b->inner_headers &&
	       a->misc == b->misc;
}

/* -----------------------------------------------------------------------
 * Recipe Manager Lifecycle
 * ----------------------------------------------------------------------- */

/**
 * fdice_recipe_mgr_init - Initialize the recipe manager
 * @mgr: recipe manager to initialize
 * @fdev: core device backpointer
 *
 * Initializes the hashtable, bitmap, and mutex. Reserves recipe 0
 * which is used by E810 firmware as the default recipe.
 *
 * Return: 0 on success
 */
int fdice_recipe_mgr_init(struct fdice_recipe_mgr *mgr,
			   struct fdice_dev *fdev)
{
	int i;

	mgr->dev = fdev;
	mutex_init(&mgr->lock);
	hash_init(mgr->match_ht);
	bitmap_zero(mgr->used_recipes, FDICE_MAX_RECIPES);

	/* Initialize all recipe slots */
	for (i = 0; i < FDICE_MAX_RECIPES; i++) {
		mgr->recipes[i].recipe_id = i;
		refcount_set(&mgr->recipes[i].refcount, 0);
		INIT_HLIST_NODE(&mgr->recipes[i].hash_node);
		mgr->recipes[i].hw_programmed = false;
		memset(&mgr->recipes[i].mask, 0,
		       sizeof(mgr->recipes[i].mask));
	}

	/*
	 * Reserve recipe 0 -- E810 firmware uses it as the default
	 * MAC-based forwarding recipe. Never allocate it for user rules.
	 */
	set_bit(0, mgr->used_recipes);
	mgr->num_used = 1;

	return 0;
}

/**
 * fdice_recipe_mgr_cleanup - Clean up the recipe manager
 * @mgr: recipe manager to clean up
 *
 * Removes all entries from the hashtable. Any recipes still holding
 * references indicate a leak -- warn but proceed with cleanup.
 */
void fdice_recipe_mgr_cleanup(struct fdice_recipe_mgr *mgr)
{
	struct fdice_recipe *recipe;
	struct hlist_node *tmp;
	int bkt;

	mutex_lock(&mgr->lock);

	hash_for_each_safe(mgr->match_ht, bkt, tmp, recipe, hash_node) {
		if (refcount_read(&recipe->refcount) > 0)
			dev_warn(&mgr->dev->pdev->dev,
				 "recipe %u leaked (refcount %u)\n",
				 recipe->recipe_id,
				 refcount_read(&recipe->refcount));
		hash_del(&recipe->hash_node);
	}

	bitmap_zero(mgr->used_recipes, FDICE_MAX_RECIPES);
	mgr->num_used = 0;

	mutex_unlock(&mgr->lock);
	mutex_destroy(&mgr->lock);
}

/* -----------------------------------------------------------------------
 * Recipe Allocation and Release
 * ----------------------------------------------------------------------- */

/**
 * fdice_recipe_alloc - Find or create a recipe for match criteria
 * @mgr: recipe manager
 * @mask: match criteria requiring a recipe
 *
 * Implements the sharing protocol:
 * 1. Hash the match criteria.
 * 2. Search the hashtable for an existing recipe with identical criteria.
 * 3. If found: increment refcount, return shared recipe.
 * 4. If not found: allocate a new recipe ID from the bitmap, initialize
 *    the recipe slot, insert into hashtable.
 *
 * The caller is responsible for programming the recipe to hardware
 * (via the command vtable) after allocation if recipe->hw_programmed
 * is false.
 *
 * Return: pointer to recipe on success, ERR_PTR on failure
 */
struct fdice_recipe *
fdice_recipe_alloc(struct fdice_recipe_mgr *mgr,
		   struct fdice_match_criteria *mask)
{
	struct fdice_recipe *recipe;
	u32 hash_key;
	int free_idx;

	hash_key = fdice_recipe_hash(mask);

	mutex_lock(&mgr->lock);

	/* Step 1: Check for existing recipe with matching criteria */
	hash_for_each_possible(mgr->match_ht, recipe, hash_node, hash_key) {
		if (fdice_recipe_match(&recipe->mask, mask)) {
			refcount_inc(&recipe->refcount);
			mutex_unlock(&mgr->lock);
			return recipe;
		}
	}

	/* Step 2: No match -- allocate a new recipe from bitmap */
	free_idx = find_first_zero_bit(mgr->used_recipes,
				       FDICE_MAX_RECIPES);
	if (free_idx >= FDICE_MAX_RECIPES) {
		mutex_unlock(&mgr->lock);
		return ERR_PTR(-ENOSPC);
	}

	/* Step 3: Initialize the new recipe slot */
	recipe = &mgr->recipes[free_idx];
	recipe->mask = *mask;
	refcount_set(&recipe->refcount, 1);
	recipe->hw_programmed = false;

	set_bit(free_idx, mgr->used_recipes);
	mgr->num_used++;

	/* Step 4: Insert into hashtable for future sharing lookups */
	hash_add(mgr->match_ht, &recipe->hash_node, hash_key);

	mutex_unlock(&mgr->lock);

	return recipe;
}

/**
 * fdice_recipe_release - Decrement recipe refcount, free when zero
 * @mgr: recipe manager
 * @recipe: recipe to release
 *
 * Decrements the reference count. When the last reference drops,
 * removes the recipe from the hashtable and frees the bitmap slot.
 * The hardware recipe slot is retained (E810 recipes persist until
 * reset), but the software tracking is released for reallocation.
 */
void fdice_recipe_release(struct fdice_recipe_mgr *mgr,
			  struct fdice_recipe *recipe)
{
	if (!recipe)
		return;

	mutex_lock(&mgr->lock);

	if (!refcount_dec_and_test(&recipe->refcount)) {
		mutex_unlock(&mgr->lock);
		return;
	}

	/* Last reference dropped -- free the slot */
	hash_del(&recipe->hash_node);
	clear_bit(recipe->recipe_id, mgr->used_recipes);
	mgr->num_used--;

	/* Reset the slot for future reuse */
	memset(&recipe->mask, 0, sizeof(recipe->mask));
	recipe->hw_programmed = false;
	INIT_HLIST_NODE(&recipe->hash_node);

	mutex_unlock(&mgr->lock);
}
