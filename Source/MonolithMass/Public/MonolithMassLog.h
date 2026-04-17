// Copyright Matrix Team. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

/**
 * Log category for the MonolithMass MCP module.
 *
 * Covers four tool namespaces:
 *   - zg.*          — ZoneGraph queries/edits/build.
 *   - masstraffic.* — CitySample MassTraffic runtime/settings.
 *   - masscrowd.*   — Engine MassCrowd runtime/settings.
 *   - mass.*        — Generic Mass Entity queries/spawners.
 *
 * Suppressed at module level if the required plugins are not installed
 * (see WITH_ZONEGRAPH / WITH_MASSENTITY / WITH_MASSCROWD / WITH_MASSTRAFFIC).
 */
MONOLITHMASS_API DECLARE_LOG_CATEGORY_EXTERN(LogMonolithMass, Log, All);
