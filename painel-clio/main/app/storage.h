// storage.h - NVS (flash interna). O ESP32-S3 nao perde isso ao desligar.
#pragma once
#include <stdbool.h>
#include "injmon.h"
#include "settings.h"
#include "trip.h"

void storage_init(void);
bool storage_load_settings(settings_t *s);
void storage_save_settings(const settings_t *s);
bool storage_load_trip(trip_persist_t *t);
void storage_save_trip(const trip_persist_t *t);
bool storage_load_injmap(injmon_map_t *m);
void storage_save_injmap(const injmon_map_t *m);
