#ifndef XRNS_PLAYER_H
#define XRNS_PLAYER_H

#ifdef WIN32
#define XRNS_DLL_EXPORT __declspec(dllexport)
#else
#define XRNS_DLL_EXPORT
#endif

#define XRNS_WOULD_WRAP_ROW           ( 1)
#define XRNS_SUCCESS                  ( 0)
#define XRNS_ERR_INVALID_INPUT_PARAM  (-1)
#define XRNS_ERR_NULL_STATE           (-2)
#define XRNS_ERR_WRONG_INPUT_SIZE     (-3)
#define XRNS_ERR_OUT_OF_SAMPLES       (-4)
#define XRNS_ERR_INVALID_TRACK_NAME   (-5) 
#define XRNS_ERR_TRACK_NOT_FOUND      (-6) 
#define XRNS_ERR_PARSING_FAIL         (-7) 

typedef struct _XRNSPlaybackState XRNSPlaybackState;

XRNS_DLL_EXPORT XRNSPlaybackState * xrns_create_playback_state(char *p_filename);
XRNS_DLL_EXPORT XRNSPlaybackState * xrns_create_playback_state_from_bytes(void *p_bytes, unsigned int num_bytes);
void xrns_produce_samples_int16(void *xstate, unsigned int num_samples, char *p_samples);

XRNS_DLL_EXPORT void xrns_free_playback_state(void *xstate);

#endif
