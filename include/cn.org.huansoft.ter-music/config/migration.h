/**
 * @file config_migration.h
 * @brief v1 (JSON) → v2 (XML) config migration API
 *
 * @author ter-music team
 * @date 2026-06-01
 */

#ifndef CONFIG_MIGRATION_H
#define CONFIG_MIGRATION_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Check whether an old-format config.json exists and needs migration.
 * @return 1 if migration is needed, 0 otherwise
 */
int config_needs_migration(void);

/**
 * Perform v1→v2 migration: read config.json, write config.xml,
 * rename config.json → config.json.bak.
 * @return 0 on success, -1 on failure
 */
int config_migrate_v1_to_v2(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_MIGRATION_H */
