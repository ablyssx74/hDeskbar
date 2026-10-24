/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The check for a newer version, as hDesktop's: the VERSION file of the repository on GitHub, read with libcurl a few seconds after
 * the start. A newer one is told in a desktop notification (or, where there is no notification server, in a window).
 */
#ifndef HDESKBAR_UPDATE_H
#define HDESKBAR_UPDATE_H

void StartUpdateChecker();

#endif
