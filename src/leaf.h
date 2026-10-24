/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */
#ifndef HDESKBAR_LEAF_H
#define HDESKBAR_LEAF_H

#include <vector>

#include "menu.h"

// The menu of the header (the Deskbar's leaf, the Be menu of Haiku): the applications by category, the power actions, quit.
std::vector<MenuItem> LeafMenuItems();

#endif
