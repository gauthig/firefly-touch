/*
 * firefly_version — the firmware version a panel reports and displays.
 *
 * Scheme: MAJOR.MINOR, rendered as "v1.00" (minor always two digits).
 *
 *   MAJOR  Global. Bump it for anything that must, or should, be flashed to
 *          EVERY device — a wire-format change, a shared-component fix, a
 *          protocol correction. A major bump means every device in
 *          docs/FLASHING.md's status table is out of date until reflashed.
 *
 *   MINOR  Per device. Bump only the affected panel's PANEL_VERSION_MINOR
 *          (in panels/<name>.h) for a fix that touches just that panel — a
 *          board quirk, its own screen layout, its own button set. Devices
 *          are expected to sit on DIFFERENT minors; that is the point, not
 *          drift. Minors are NOT reset by a major bump, so (major, minor)
 *          stays unique per panel.
 *
 * Where it shows up:
 *   - the status bar of every display panel, between the panel name and the
 *     right-hand readout (main/ui/ui.c);
 *   - the boot log, so a serial capture identifies the build;
 *   - docs/FLASHING.md's "Recommended version" column, which is just
 *     FIREFLY_VERSION_MAJOR . PANEL_VERSION_MINOR for that panel.
 *
 * ⚠️ Bump the version in the same commit as the change it describes, and
 * update docs/FLASHING.md's table in that commit — a version nobody updated
 * is worse than no version at all.
 *
 * Deliberately numbers, not a string: the two-digit minor is formatted with
 * snprintf at the one place that needs it, rather than through preprocessor
 * paste tricks that cannot zero-pad.
 */
#pragma once

#include "panel_config.h"

/* Global. See MAJOR above. */
#define FIREFLY_VERSION_MAJOR 1

/* Per panel; panels/<name>.h may override. See MINOR above. */
#ifndef PANEL_VERSION_MINOR
#define PANEL_VERSION_MINOR 0
#endif

#if PANEL_VERSION_MINOR < 0 || PANEL_VERSION_MINOR > 99
#error "PANEL_VERSION_MINOR must be 0-99 (it renders as two digits)"
#endif

/* Buffer big enough for "v99.99" plus NUL. */
#define FIREFLY_VERSION_BUF_LEN 8

/* printf format + args for the version, e.g. snprintf(buf, sizeof buf,
 * FIREFLY_VERSION_FMT, FIREFLY_VERSION_ARGS); */
#define FIREFLY_VERSION_FMT  "v%u.%02u"
#define FIREFLY_VERSION_ARGS (unsigned)FIREFLY_VERSION_MAJOR, (unsigned)PANEL_VERSION_MINOR
