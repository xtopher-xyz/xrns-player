#ifndef XRNS_PLAYER_H
#define XRNS_PLAYER_H

#ifdef WIN32
#define XRNS_DLL_EXPORT __declspec(dllexport)
#else
#define XRNS_DLL_EXPORT
#endif

#include <stdint.h>

typedef struct _XRNSPlaybackState XRNSPlaybackState;

XRNS_DLL_EXPORT XRNSPlaybackState * xrns_create_playback_state(char *p_filename);
XRNS_DLL_EXPORT XRNSPlaybackState * xrns_create_playback_state_from_bytes(void *p_bytes, uint32_t num_bytes);
void xrns_produce_samples_int16(void *xstate, unsigned int num_samples, int16_t *p_samples);

XRNS_DLL_EXPORT void xrns_free_playback_state(void *xstate);

#endif
