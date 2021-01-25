/* 
 * Standalone playback of Renoise (https://www.renoise.com) project files in realtime. 
 * This software is in the public domain, please see the below comments.
 * For more information, check the README file.
 *
 * Christopher Hines - http://topherhin.es
 * @xtopherhin
 * 
 * GitHub: https://github.com/xtopher-xyz/xrns-player
 *
 * 24/01/2021
 *
 * This is free and unencumbered software released into the public domain.
 * 
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 * 
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 * 
 * For more information, please refer to <http://unlicense.org/>
 */

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
XRNS_DLL_EXPORT int                 xrns_produce_samples(void *xstate, unsigned int num_samples, float *p_samples);
XRNS_DLL_EXPORT void                xrns_free_playback_state(void *xstate);

#endif
