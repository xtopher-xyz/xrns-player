#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include <xmmintrin.h>
#include <pmmintrin.h>

#define BASIC_FILTER_PARAM_FC   (0)
#define BASIC_FILTER_NUM_PARAMS (1)
#define SQRT_OF_2               (1.41421356f)
#define HALF_OF_TAU             (3.14159265f)

const char *BasicFilterName = "BasicFilter 1.0";

typedef struct
{
	float b0;
	float b1;
	float b2;
	float a1;
	float a2;
	float v[2];
} biquad;

void BiquadInit(biquad *Biquad)
{
	memset(Biquad, 0, sizeof(Biquad));
}

void BiquadComputeNewButterworthLPF(biquad *Biquad, float Fc, float Fs)
{
	float Wc    = 2.0f * HALF_OF_TAU * Fc;
	float Beta  = 1.0f / tan(Wc/(2.0f * Fs));
	float Beta2 = Beta * Beta;
	
	float a0I   = 1.0f / (Beta2 + SQRT_OF_2 * Beta + 1.0f);

	Biquad->b0  = a0I;
	Biquad->b1  = 2.0f * a0I;
	Biquad->b2  = a0I;
	
	Biquad->a1  = 2.0f * (1.0f - Beta2) * a0I;
	Biquad->a2  = (Beta2 - SQRT_OF_2 * Beta + 1.0f) * a0I;
}

void BiquadComputeNewButterworthHPF(biquad *Biquad, float Fc, float Fs)
{
	BiquadComputeNewButterworthLPF(Biquad, Fc, Fs);

	/* warping will need this coef... since we don't warp, Fc == NewFc */
	/* c = -cos(pi*(Fc + NewFc)/(Fs))/cos(pi*(Fc - NewFc)/(Fs)); */
	double c = -cos(HALF_OF_TAU * (2.0f * Fc)/Fs);

	double n0 = Biquad->b0              - Biquad->b1 * (c)           + Biquad->b2 * (c * c);
	double n1 = Biquad->b0 * (c * 2.0f) - Biquad->b1 * (c*c  + 1.0f) + Biquad->b2 * (c * 2.0f);
	double n2 = Biquad->b0 * (c * c)    - Biquad->b1 * (c)           + Biquad->b2;

	double k0 = 1.0f                    - Biquad->a1 * (c)           + Biquad->a2 * (c * c);
	double k1 = 1.0f * (c * 2.0f)       - Biquad->a1 * (c*c  + 1.0f) + Biquad->a2 * (c * 2.0f);
	double k2 = 1.0f * (c * c)          - Biquad->a1 * (c)           + Biquad->a2;

	Biquad->b0 = (double)n0/(double)k0;
	Biquad->b1 = (double)n1/(double)k0;
	Biquad->b2 = (double)n2/(double)k0;

	Biquad->a1 = (double)k1/(double)k0;
	Biquad->a2 = (double)k2/(double)k0;
}

float BiquadProcess(biquad *Biquad, float NewSample)
{
	float v = NewSample - Biquad->a2 * Biquad->v[1] - Biquad->a1 * Biquad->v[0];
	float y = v * Biquad->b0 + Biquad->b1 * Biquad->v[0] + Biquad->b2 * Biquad->v[1];

	Biquad->v[1] = Biquad->v[0];
	Biquad->v[0] = v; 

	return y;
}

static inline float BasicFilterParamToHz(float Value)
{
	/* log mapping from 100Hz to 18kHz .. ish
	 */
	if (Value < 0.0f) Value = 0.0f;
	if (Value > 1.0f) Value = 1.0f;

	return 100.0f + 44.3697f * (exp(6.0f * Value) - 1.0f);
}

typedef struct
{
	int          NumInputChannels;
	int          NumOutputChannels;
	float        SampleRate;
	float        Params[BASIC_FILTER_NUM_PARAMS];

	float        SmoothedFC;

	biquad       LPF[2];
} basic_filter_state;

void *BasicFilterOpen(void)
{
	basic_filter_state *bs = malloc(sizeof(basic_filter_state));
	memset(bs, 0, sizeof(basic_filter_state));

	_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
	_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

	bs->SmoothedFC = 0.475f;

	float Fc = BasicFilterParamToHz(bs->SmoothedFC);
	BiquadComputeNewButterworthLPF(&bs->LPF[0], Fc, 48000.0f);
	BiquadComputeNewButterworthLPF(&bs->LPF[1], Fc, 48000.0f);

	return bs;
}

void BasicFilterClose(basic_filter_state *state)
{
	free(state);
}

void basic_filter_process(basic_filter_state *state, float **Input, float **Output, int32_t NumSamples)
{
	int s;
	float Fs                  = state->SampleRate ? state->SampleRate : 48000.0f;
	float Cutoff_Normalised   = state->Params[BASIC_FILTER_PARAM_FC];
	float Cutoff_Hz;

	

	for (s = 0; s < NumSamples; s++)
	{
		state->SmoothedFC += 0.01f * (Cutoff_Normalised - state->SmoothedFC);
		Cutoff_Hz = BasicFilterParamToHz(state->SmoothedFC);
		BiquadComputeNewButterworthLPF(&state->LPF[0], Cutoff_Hz, Fs);
		BiquadComputeNewButterworthLPF(&state->LPF[1], Cutoff_Hz, Fs);	

		Output[0][s] = BiquadProcess(&state->LPF[0], Input[0][s]);
		Output[1][s] = BiquadProcess(&state->LPF[1], Input[1][s]);
	}
}

float BasicFilterGetParameter(basic_filter_state *state, int32_t index)
{
	if (index >= 0 && index < BASIC_FILTER_NUM_PARAMS)
		return state->Params[index];
	return 0.0f;
}

void BasicFilterSetParameter(basic_filter_state *state, int32_t index, float value)
{
	if (index >= 0 && index < BASIC_FILTER_NUM_PARAMS)
		state->Params[index] = value;
}

void BasicFilterSetSampleRate(basic_filter_state *state, float SampleRate)
{
	state->SampleRate = SampleRate;
}

#ifdef DSP_WRAPPERS

void BasicFilterCutoffFormat(float Value, char *str, unsigned int max_str_len)
{
	float Hz = BasicFilterParamToHz(Value);
	snprintf(str, max_str_len, "%02.2f kHz", Hz / 1000.0f);
}

void BasicFilterPopulateDSPStruct(dsp_effect *DSPEffect)
{
	DSPEffect->State         = 0;
	DSPEffect->Process       = basic_filter_process;
	DSPEffect->Open          = BasicFilterOpen;
	DSPEffect->Close         = BasicFilterClose;
	DSPEffect->GetParameter  = BasicFilterGetParameter;
	DSPEffect->SetParameter  = BasicFilterSetParameter;
	DSPEffect->Name          = BasicFilterName;
	DSPEffect->SetSampleRate = BasicFilterSetSampleRate;
	DSPEffect->NumParameters = BASIC_FILTER_NUM_PARAMS;

	DSPEffect->Unique32BitCode = CCONST('b', 'F', 'i', 'L');

	if (DSPEffect->NumParameters)
	{
		DSPEffect->Parameters = malloc(sizeof(dsp_effect_parameter) * DSPEffect->NumParameters);
		CopyBytesUpToNAndAppendNULL(DSPEffect->Parameters[BASIC_FILTER_PARAM_FC].Name, "Cutoff", PARAMETER_MAX_NAME);
		DSPEffect->Parameters[BASIC_FILTER_PARAM_FC].Format = BasicFilterCutoffFormat;
	}
}

#endif
