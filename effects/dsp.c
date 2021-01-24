/* Wrapper around DSP.
 *
 * A slight convinience and annoyance in C, but 
 * handy for giving to the shim for VST/AU/DSSI ...
 *
 */

#include <stdint.h>
#include <stdio.h>

#ifndef DSP_WRAPPERS
#define DSP_WRAPPERS
#endif

typedef void *deInitEffect(unsigned int NumChannelsIn, unsigned int NumChannelsOut);
typedef void  (*deProcessAudio)(void *State, float **Input, float **Output, int32_t NumSamples);
typedef void  (*deSetParameter)(void *State, int32_t index, float value);
typedef float (*deGetParameter)(void *State, int32_t index); 

struct dsp_effect_parameter_s;

typedef void (*deParameterFormatValue)(float Value, char *str, unsigned int max_str_len);

#define PARAMETER_MAX_NAME (1024)

struct dsp_effect_parameter_s
{
	char                     Name[PARAMETER_MAX_NAME];
	deParameterFormatValue   Format;
};

typedef struct dsp_effect_parameter_s dsp_effect_parameter;

typedef struct
{	
	void            *State;
	void           *(*Open)(void);
	void            (*Close)(void *);

	void            (*SetSampleRate)(void *, float);

	const char     *Name; 

	deProcessAudio  Process;
	deSetParameter  SetParameter;
	deGetParameter  GetParameter;

	/* Parameter stuff .. */
	unsigned int    NumParameters;

	dsp_effect_parameter *Parameters;

	uint32_t        Unique32BitCode;

} dsp_effect;

void DSPEffectPrintLinearToDB(float Value, char *str, unsigned int max_str_len)
{
	snprintf(str, max_str_len, "%02.2f dB", 10.0f * log10(Value + 0.00001f));
}

void DSPEffectPrintOnOff(float Value, char *str, unsigned int max_str_len)
{
	(void) max_str_len;

	if (Value != 0.0)
	{
		sprintf(str, "ON");
	}
	else
	{
		sprintf(str, "OFF");		
	}
}

void DSPEffectPrintLinearToPercent(float Value, char *str, unsigned int max_str_len)
{
	snprintf(str, max_str_len, "%02.1f %", 100.0f * Value);
}

void DSPEffectPrintLinearToLinear(float Value, char *str, unsigned int max_str_len)
{
	snprintf(str, max_str_len, "%02.2f", Value);
}

#ifndef CCONST
#define CCONST(a, b, c, d) \
	 ((((int32_t)a) << 24) | (((int32_t)b) << 16) | (((int32_t)c) << 8) | (((int32_t)d) << 0))
#endif
	 
void CopyBytesUpToNAndAppendNULL(char *ptr, const char *String, unsigned int LengthOfString)
{
	int i = 0;
	for (; i < LengthOfString && i < strlen(String); i++)
	{
		ptr[i] = String[i];
	}

	ptr[i] = '\0';
}

