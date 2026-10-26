/**
 * @file    kns_mod_subset.c
 * @brief   Restrict the Kineis stack to the A2 / A2-legacy / VLDA4 uplink formats.
 *
 * libkineis.a (kns_ff.o) is built with -ffunction-sections, so each frame encoder sits in its
 * own section. KNS_MAC_TXcmn_buildFrm (kns_mac_ll_tx_common.o) holds a hard reference to
 * KNS_FF_encodeLDK, which keeps that encoder alive under --gc-sections.
 *
 * Linking with -Wl,--wrap=KNS_FF_encodeLDK redirects that reference here. Nothing then
 * references .text.KNS_FF_encodeLDK and the linker discards it (~576 bytes of flash).
 *
 * KNS_FF_encodeHDA4 needs no wrap: no object in this build of libkineis.a references it, so
 * --gc-sections already drops it together with its 2040 bytes of .bss buffers.
 *
 * Calling contract (from disassembly of KNS_MAC_TXcmn_buildFrm):
 *   r0 = user data ptr, r1 = user data bit length, r2 = output frame buffer,
 *   r3 = output bit length ptr, [sp] = service flags byte, [sp+4] = 0.
 *   Return 0 = frame built; any non-zero value makes the MAC free the TX FIFO slot and
 *   return -1, so the request fails cleanly instead of transmitting garbage.
 */

#ifdef KNS_MOD_A2_VLD_ONLY

#include <stdint.h>
#include "kns_types.h"

enum KNS_status_t __wrap_KNS_FF_encodeLDK(const uint8_t *usrdata, uint16_t usrdata_bitlen,
	uint8_t *frm, uint16_t *frm_bitlen, uint8_t sf, uint32_t rfu);

enum KNS_status_t __wrap_KNS_FF_encodeLDK(const uint8_t *usrdata, uint16_t usrdata_bitlen,
	uint8_t *frm, uint16_t *frm_bitlen, uint8_t sf, uint32_t rfu)
{
	(void)usrdata;
	(void)usrdata_bitlen;
	(void)frm;
	(void)frm_bitlen;
	(void)sf;
	(void)rfu;

	/* An LDK radio configuration is loaded but LDK support was compiled out. */
	return KNS_STATUS_BAD_SETTING;
}

#endif /* KNS_MOD_A2_VLD_ONLY */
