// fuel.h - consumo instantaneo em L/h a partir da melhor fonte disponivel
#pragma once
#include "settings.h"
#include "vehicle.h"

typedef enum {
    FUEL_SRC_NONE = 0,
    FUEL_SRC_ECU,      // a ECU ja manda "consumo instantaneo" (Renault 21AA) -> melhor
    FUEL_SRC_INJ,      // tempo de injecao x rpm x vazao do bico
    FUEL_SRC_MAF,      // sensor MAF (estes carros nao tem)
    FUEL_SRC_MAP,      // speed-density: MAP + IAT + rpm + cilindrada (pior)
} fuel_src_t;

// Retorna L/h (ja com o fator de calibracao) e a fonte usada.
float fuel_lph(const vehicle_sample_t *s, const settings_t *cfg, fuel_src_t *src);

// Consumo instantaneo para mostrar: km/L (ou L/100km). NAN quando parado.
float fuel_instant_display(float lph, float speed_kmh, int units_kml);

const char *fuel_src_str(fuel_src_t s);
