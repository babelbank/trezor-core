/*
 * This file is part of the TREZOR project, https://trezor.io/
 *
 * Copyright (c) SatoshiLabs
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include "common.h"
#include "norcow.h"
#include "flash.h"

#include "py/runtime.h"
#include "py/obj.h"

// Norcow storage key of configured PIN.
#define PIN_KEY 0x0000

// Maximum PIN length.
#define PIN_MAXLEN 32

// Byte-length of flash section containing fail counters.
#define PIN_FAIL_KEY 0x0001
#define PIN_FAIL_SECTOR_SIZE 32

// Maximum number of failed unlock attempts.
#define PIN_MAX_TRIES 15

static secbool initialized = secfalse;
static secbool unlocked = secfalse;

void storage_init(void)
{
    initialized = secfalse;
    unlocked = secfalse;
    flash_init();
    norcow_init();
    initialized = sectrue;
}

static secbool pin_fails_reset(uint16_t ofs)
{
    return norcow_update(PIN_FAIL_KEY, ofs, 0);
}

static secbool pin_fails_increase(const uint32_t *ptr, uint16_t ofs)
{
    uint32_t ctr = *ptr;
    ctr = ctr << 1;

    if (sectrue != norcow_update(PIN_FAIL_KEY, ofs, ctr)) {
        return secfalse;
    }

    uint32_t check = *ptr;
    if (ctr != check) {
        return secfalse;
    }
    return sectrue;
}

static void pin_fails_check_max(uint32_t ctr)
{
    if (~ctr >= (1 << PIN_MAX_TRIES)) {
        norcow_wipe();
        ensure(secfalse, "pin_fails_check_max");
    }
}

static secbool pin_cmp(const uint32_t pin)
{
    const void *spin = NULL;
    uint16_t spinlen = 0;
    norcow_get(PIN_KEY, &spin, &spinlen);
    if (NULL != spin && spinlen == sizeof(uint32_t)) {
        return sectrue * (pin == *(const uint32_t*)spin);
    } else {
        return sectrue * (1 == pin);
    }
}

static secbool pin_get_fails(const uint32_t **pinfail, uint32_t *pofs)
{
    const void *vpinfail;
    uint16_t pinfaillen;
    unsigned int ofs;
    // The PIN_FAIL_KEY points to an area of words, initialized to
    // 0xffffffff (meaning no pin failures).  The first non-zero word
    // in this area is the current pin failure counter.  If  PIN_FAIL_KEY
    // has no configuration or is empty, the pin failure counter is 0.
    // We rely on the fact that flash allows to clear bits and we clear one
    // bit to indicate pin failure.  On success, the word is set to 0,
    // indicating that the next word is the pin failure counter.

    // Find the current pin failure counter
    if (secfalse != norcow_get(PIN_FAIL_KEY, &vpinfail, &pinfaillen)) {
        *pinfail = vpinfail;
        for (ofs = 0; ofs < pinfaillen / sizeof(uint32_t); ofs++) {
            if (((const uint32_t *) vpinfail)[ofs]) {
                *pinfail = vpinfail;
                *pofs = ofs;
                return sectrue;
            }
        }
    }

    // No pin failure section, or all entries used -> create a new one.
    uint32_t pinarea[PIN_FAIL_SECTOR_SIZE];
    memset(pinarea, 0xff, sizeof(pinarea));
    if (sectrue != norcow_set(PIN_FAIL_KEY, pinarea, sizeof(pinarea))) {
        return secfalse;
    }
    if (sectrue != norcow_get(PIN_FAIL_KEY, &vpinfail, &pinfaillen)) {
        return secfalse;
    }
    *pinfail = vpinfail;
    *pofs = 0;
    return sectrue;
}

secbool storage_check_pin(uint32_t pin, mp_obj_t callback)
{
    const uint32_t *pinfail = NULL;
    uint32_t ofs;
    uint32_t ctr;

    // Get the pin failure counter
    if (pin_get_fails(&pinfail, &ofs) != sectrue) {
        return secfalse;
    }

    // Read current failure counter
    ctr = pinfail[ofs];
    // Wipe storage if too many failures
    pin_fails_check_max(ctr);

    // Sleep for ~ctr seconds before checking the PIN.
    uint32_t progress;
    for (uint32_t wait = ~ctr; wait > 0; wait--) {
        for (int i = 0; i < 10; i++) {
            if (mp_obj_is_callable(callback)) {
                if ((~ctr) > 1000000) {  // precise enough
                    progress = (~ctr - wait) / ((~ctr) / 1000);
                } else {
                    progress = ((~ctr - wait) * 10 + i) * 100 / (~ctr);
                }
                mp_call_function_2(callback, mp_obj_new_int(wait), mp_obj_new_int(progress));
            }
            hal_delay(100);
        }
    }
    // Show last frame if we were waiting
    if ((~ctr > 0) && mp_obj_is_callable(callback)) {
        mp_call_function_2(callback, mp_obj_new_int(0), mp_obj_new_int(1000));
    }

    // First, we increase PIN fail counter in storage, even before checking the
    // PIN.  If the PIN is correct, we reset the counter afterwards.  If not, we
    // check if this is the last allowed attempt.
    if (sectrue != pin_fails_increase(pinfail + ofs, ofs * sizeof(uint32_t))) {
        return secfalse;
    }
    if (sectrue != pin_cmp(pin)) {
        // Wipe storage if too many failures
        pin_fails_check_max(ctr << 1);
        return secfalse;
    }
    // Finally set the counter to 0 to indicate success.
    return pin_fails_reset(ofs * sizeof(uint32_t));
}

secbool storage_unlock(const uint32_t pin, mp_obj_t callback)
{
    unlocked = secfalse;
    if (sectrue == initialized && sectrue == storage_check_pin(pin, callback)) {
        unlocked = sectrue;
    }
    return unlocked;
}

secbool storage_get(uint16_t key, const void **val, uint16_t *len)
{
    const uint8_t app = key >> 8;
    // APP == 0 is reserved for PIN related values
    if (sectrue != initialized || app == 0) {
        return secfalse;
    }
    // top bit of APP set indicates the value can be read from unlocked device
    if (sectrue != unlocked && ((app & 0x80) == 0)) {
        return secfalse;
    }
    return norcow_get(key, val, len);
}

secbool storage_set(uint16_t key, const void *val, uint16_t len)
{
    const uint8_t app = key >> 8;
    // APP == 0 is reserved for PIN related values
    if (sectrue != initialized || sectrue != unlocked || app == 0) {
        return secfalse;
    }
    return norcow_set(key, val, len);
}

secbool storage_has_pin(void)
{
    if (sectrue != initialized) {
        return secfalse;
    }
    return sectrue == pin_cmp(1) ? secfalse : sectrue;
}

secbool storage_change_pin(const uint32_t pin, const uint32_t newpin, mp_obj_t callback)
{
    if (sectrue != initialized || sectrue != unlocked) {
        return secfalse;
    }
    if (sectrue != storage_check_pin(pin, callback)) {
        return secfalse;
    }
    return norcow_set(PIN_KEY, &newpin, sizeof(uint32_t));
}

void storage_wipe(void)
{
    norcow_wipe();
}
