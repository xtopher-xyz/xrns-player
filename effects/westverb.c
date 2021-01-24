#include <stdlib.h>
#include <stdint.h>

#include "basic_filter.c"

#define WESTVERB_PAR_DRY_GAIN    (0)
#define WESTVERB_PAR_LPF_FC      (1)
#define WESTVERB_PAR_HPF_FC      (2)
#define WESTVERB_PAR_T_FALL      (3)
#define WESTVERB_PAR_ROOM_GAIN   (4)
#define WESTVERB_PAR_LOOP_GAIN   (5)
#define WESTVERB_PAR_DELAY_SCALE (6)
#define WESTVERB_PAR_BRIGHTNESS  (7)
#define WESTVERB_PAR_DURATION    (8)
#define WESTVERB_PAR_EARLYTIME0  (9)
#define WESTVERB_PAR_EARLYTIME1  (10)
#define WESTVERB_PAR_EARLYTIME2  (11)
#define WESTVERB_PAR_EARLYGAIN0  (12)
#define WESTVERB_PAR_EARLYGAIN1  (13)
#define WESTVERB_PAR_EARLYGAIN2  (14)
#define WESTVERB_NUM_PARAMS      (15)

#define WESTVERB_MAX_STAGES      (16)

#define WESTVERB_DELAY_SCALE_MIN (0.1f)
#define WESTVERB_DELAY_SCALE_MAX (2.0f)

#define WESTVERB_EARLY_TIME_MS_MIN  (0)
#define WESTVERB_EARLY_TIME_MS_MAX  (100)

/* TODO: These were tuned to sound good at 48kHz, may want a set of new primes for 44.1kHz.
 * ...... actually these aren't even prime if you scale them by 0.6 as you have been doing.....
 */
int WestVerbPrimeDelays[WESTVERB_MAX_STAGES] = {541, 863, 1223, 1583, 1987, 2357, 2741, 3181, 3571, 3989, 4409, 4831, 5279, 5693, 6133, 6571};

const char *WestVerbName = "WestVerb 1.0";

typedef struct
{
	float   g;
	float   a;
	int     Delay;
	int     MaxDelay;
	biquad  FDF;
	float  *DelayLine;
	float   BranchGain;
	int     idx;
} westverb_allpass;

typedef struct
{
	unsigned int     NumStages;
	westverb_allpass Allpasses[WESTVERB_MAX_STAGES];
	float            FeedbackSample;

	float            Tau;
	float            Norm;

	biquad           LPF;
	biquad           HPF;

	float           *DelayLine;
	int              MaxDelay;
	int              idx;

	float            EarlyGains[3];
	int              EarlyDelays[3];
} westverb_chain;

typedef struct
{
	int              NumInputChannels;
	int              NumOutputChannels;
	float            SampleRate;
	float            Params[WESTVERB_NUM_PARAMS];

	westverb_chain   Tank[2];

	float            DryGain;
	float            LateGain;
	float            EarlyGain;

} westverb_state;

/* Gain used for the mix level on wet, dry, output
 */
float WestVerbParam2LinGain(float P)
{
	return pow(10.0f, (-50.0f + P * 50.0f)/20.0f);
}

float WestVerbParam2LinGain6dBCap(float P)
{
	return pow(10.0f, (-50.0f + P * 44.0f)/20.0f);
}

void WestVerbPrintLinearToDB(float Value, char *str, unsigned int max_str_len)
{
	snprintf(str, max_str_len, "%02.2f dB", -50.0f + Value * 50.0f);
}

void WestVerbPrintLinearToDB6dbCap(float Value, char *str, unsigned int max_str_len)
{
	snprintf(str, max_str_len, "%02.2f dB", -50.0f + Value * 44.0f);
}

void WestVerbLinearToCutoff(float Value, char *str, unsigned int max_str_len)
{
	snprintf(str, max_str_len, "%02.3f kHz", Value);
}

void WestVerbLinearToBounceMs(float Value, char *str, unsigned int max_str_len)
{
	snprintf(str, max_str_len, "%02.1f ms", Value * 100.0f);
}

float ComputeTankNorm(westverb_chain *Tank)
{
	int i, j;
	float LoopGain    = 0.1f; /* Comment this out and allow the loop gain to change. Tank->Tau * Tank->Tau; */
	float ForwardGain = 0.0f;

	float AllPassGains[WESTVERB_MAX_STAGES] = {0.0f};

	for (i = 0; i < Tank->NumStages; i++)
	{
		westverb_allpass *ap = &Tank->Allpasses[i];

		float g = ap->g;
		float a = ap->a;

		AllPassGains[i] = ((a * a) +  (2.0f * g) + (g * g)) / (1.0f + 2.0f * g * a + (g*a)*(g*a));
	}

	for (i = 0; i < Tank->NumStages; i++)
	{
		westverb_allpass *ap = &Tank->Allpasses[i];

		float TGain = 1.0f;

		for (j = 0; j <= i; j++)
		{
			 TGain *= AllPassGains[j];
		}

		ForwardGain += TGain * ap->BranchGain * ap->BranchGain;

		LoopGain *= AllPassGains[i];
	}

    return sqrt((1.0f - LoopGain)/ForwardGain);
}

void *WestVerbOpen(void)
{
	int i, ch, j;
	const int NumStages = 12;

	/* Assume no alignment restrictions, allocate everything
	 * flat into memory.
	 */

	unsigned int MemSz = sizeof(westverb_state);

	/* Every allpass stage requires a delayline, often
	 * fairly long as the prime delay times grow. Allocate
	 * all this memory together, and index the total buffer
	 * for each allpass.
	 *
	 * To avoid re-allocation (aside from sample rate changes), 
	 * we will allocate the worst-case delay lines for each,
	 * given by the maximum value of density * WestVerbPrimeDelays[i].
	 *
	 */

	for (i = 0; i < NumStages; i++)
	{
		MemSz += (2 /* num channels */) * sizeof(float) * ((int) ceil(WESTVERB_DELAY_SCALE_MAX * WestVerbPrimeDelays[i]));
	}

	MemSz += 2 * sizeof(float) * ((int) ceil(WESTVERB_EARLY_TIME_MS_MAX * 48000.0f / 1000.0f));

	char *VerbMem = malloc(MemSz);
	char *DLMem   = VerbMem;
	memset(VerbMem, 0, MemSz);

	westverb_state *W = (westverb_state *) VerbMem;

	DLMem += sizeof(westverb_state);

	W->Params[WESTVERB_PAR_DELAY_SCALE] = 0.5f;

	for (ch = 0; ch < 2; ch++)
	{
		westverb_chain *Tank = &W->Tank[ch];

		Tank->NumStages = NumStages;

		for (i = 0; i < NumStages; i++)
		{
			westverb_allpass *ap = &Tank->Allpasses[i];

			ap->g          = 0.7f;
			ap->a          = 1.0f;
			ap->Delay      = (int) ceil(2.0f * W->Params[WESTVERB_PAR_DELAY_SCALE] * WestVerbPrimeDelays[i]);
			ap->MaxDelay   = (int) ceil(WESTVERB_DELAY_SCALE_MAX * WestVerbPrimeDelays[i]);
			ap->BranchGain = 1.0f / (i + 1.0f);

			BiquadInit(&ap->FDF);
			BiquadComputeNewButterworthLPF(&ap->FDF, 8000.0f, 48000.0f);
			
			ap->DelayLine = (float *) DLMem;
			DLMem += sizeof(float) * ap->MaxDelay;
		}

		Tank->MaxDelay  = ceil(WESTVERB_EARLY_TIME_MS_MAX * 48000.0f / 1000.0f);
		Tank->DelayLine = (float *) DLMem;
		DLMem += sizeof(float) * Tank->MaxDelay;

		Tank->Tau = 0.5f;

		Tank->Norm = ComputeTankNorm(Tank);

		W->Params[WESTVERB_PAR_EARLYGAIN0] = (-6.0f  + 50.0f)/50.0f;
		W->Params[WESTVERB_PAR_EARLYGAIN1] = (-8.0f  + 50.0f)/50.0f;
		W->Params[WESTVERB_PAR_EARLYGAIN2] = (-10.0f + 50.0f)/50.0f;

		Tank->EarlyGains[0]  = WestVerbParam2LinGain(W->Params[WESTVERB_PAR_EARLYGAIN0]);
		Tank->EarlyGains[1]  = WestVerbParam2LinGain(W->Params[WESTVERB_PAR_EARLYGAIN1]);
		Tank->EarlyGains[2]  = WestVerbParam2LinGain(W->Params[WESTVERB_PAR_EARLYGAIN2]);		

		W->Params[WESTVERB_PAR_EARLYTIME0] = 0.03f / 0.1f;
		W->Params[WESTVERB_PAR_EARLYTIME1] = 0.05f / 0.1f;
		W->Params[WESTVERB_PAR_EARLYTIME2] = 0.10f / 0.1f;

		Tank->EarlyDelays[0] = W->Params[WESTVERB_PAR_EARLYTIME0] * 0.1f;
		Tank->EarlyDelays[1] = W->Params[WESTVERB_PAR_EARLYTIME1] * 0.1f;
		Tank->EarlyDelays[2] = W->Params[WESTVERB_PAR_EARLYTIME2] * 0.1f;	

		W->Params[WESTVERB_PAR_LPF_FC] = 1.0f;
		W->Params[WESTVERB_PAR_HPF_FC] = 0.0f;
		BiquadInit(&Tank->LPF);
		BiquadInit(&Tank->HPF);

		BiquadComputeNewButterworthLPF(&Tank->LPF, BasicFilterParamToHz(W->Params[WESTVERB_PAR_LPF_FC]), 48000.0f);
		BiquadComputeNewButterworthHPF(&Tank->HPF, BasicFilterParamToHz(W->Params[WESTVERB_PAR_HPF_FC]), 48000.0f);
	}

	W->Params[WESTVERB_PAR_DRY_GAIN]   = 1.0;
	W->Params[WESTVERB_PAR_ROOM_GAIN]  = 1.0;
	// W->Params[WESTVERB_PAR_EARLY_GAIN] = 1.0;

	W->DryGain   = WestVerbParam2LinGain(W->Params[WESTVERB_PAR_DRY_GAIN]);
	// W->EarlyGain = WestVerbParam2LinGain(W->Params[WESTVERB_PAR_EARLY_GAIN]);
	W->LateGain  = WestVerbParam2LinGain(W->Params[WESTVERB_PAR_ROOM_GAIN]);

	return VerbMem;
}

void WestVerbClose(westverb_state *state)
{
	free(state);
}

float AllpassProcess(westverb_allpass *ap, float In)
{
	float OldSample = ap->DelayLine[(ap->idx - ap->Delay + ap->MaxDelay) % ap->MaxDelay];
	float DecayPath = BiquadProcess(&ap->FDF, OldSample);
	float y = DecayPath * ap->a + ap->g * In;
	ap->DelayLine[ap->idx] = In - ap->g * y;
	ap->idx = (ap->idx + 1) % ap->MaxDelay;

	return y;
}

void westverb_process(westverb_state *state, float **Input, float **Output, int32_t NumSamples)
{
	int i, s, p;
	state->NumInputChannels  = 2;
	state->NumOutputChannels = 2;

	for (i = 0; i < state->NumInputChannels; i++)
	{
		westverb_chain *Tank = &state->Tank[i];

		for (s = 0; s < NumSamples; s++)
		{
			float Room     = 0.0f;
			float In       = Input[i][s] + Tank->FeedbackSample * Tank->Tau;
			float EarlyOut = 0.0f;
			float LateOut  = 0.0f;

			Tank->DelayLine[Tank->idx] = Input[i][s];

			for (p = 0; p < 3; p++)
			{
				int idx = (Tank->idx - Tank->EarlyDelays[p] + Tank->MaxDelay) % Tank->MaxDelay;
				EarlyOut += Tank->EarlyGains[p] * Tank->DelayLine[idx];
			}

			Tank->idx = (Tank->idx + 1) % Tank->MaxDelay;

			for (p = 0; p < Tank->NumStages; p++)
			{
				westverb_allpass *ap = &Tank->Allpasses[p];
				In = AllpassProcess(ap, In);
				LateOut += In * ap->BranchGain;
			}

			Tank->FeedbackSample = In;

			Room = state->LateGain * (LateOut + EarlyOut);
			Room = BiquadProcess(&Tank->LPF, Room);
			Room = BiquadProcess(&Tank->HPF, Room);

			Output[i][s] = state->Params[WESTVERB_PAR_DRY_GAIN] * Input[i][s]
			             + Room * Tank->Norm;
		}
	}
}


float WestVerbGetParameter(westverb_state *state, int32_t index)
{
	if (index >= 0 && index < WESTVERB_NUM_PARAMS)
		return state->Params[index];
	return 0.0f;
}

void WestVerbSetParameter(westverb_state *state, int32_t index, float value)
{
	int ch, i;
	if (index >= 0 && index < WESTVERB_NUM_PARAMS)
	{
		switch (index)
		{
			case WESTVERB_PAR_DRY_GAIN:
			{
				state->DryGain = WestVerbParam2LinGain(value);
				break;
			}
			case WESTVERB_PAR_ROOM_GAIN:
			{
				state->LateGain = WestVerbParam2LinGain(value);
				break;
			}
			// case WESTVERB_PAR_EARLY_GAIN:
			// {
			// 	state->EarlyGain = WestVerbParam2LinGain(value);
			// 	break;
			// }		
			case WESTVERB_PAR_EARLYTIME0:
			{
				state->Tank[0].EarlyDelays[0] = ceil(value * 0.1f * state->SampleRate);
				state->Tank[1].EarlyDelays[0] = ceil(value * 0.1f * state->SampleRate);
				break;
			}
			case WESTVERB_PAR_EARLYTIME1:
			{
				state->Tank[0].EarlyDelays[1] = ceil(value * 0.1f * state->SampleRate);
				state->Tank[1].EarlyDelays[1] = ceil(value * 0.1f * state->SampleRate);
				break;
			}
			case WESTVERB_PAR_EARLYTIME2:
			{
				state->Tank[0].EarlyDelays[2] = ceil(value * 0.1f * state->SampleRate);
				state->Tank[1].EarlyDelays[2] = ceil(value * 0.1f * state->SampleRate);
				break;
			}
			case WESTVERB_PAR_EARLYGAIN0:
			{
				state->Tank[0].EarlyGains[0] = WestVerbParam2LinGain(value);
				state->Tank[1].EarlyGains[0] = WestVerbParam2LinGain(value);
				break;
			}
			case WESTVERB_PAR_EARLYGAIN1:
			{
				state->Tank[0].EarlyGains[1] = WestVerbParam2LinGain(value);
				state->Tank[1].EarlyGains[1] = WestVerbParam2LinGain(value);
				break;
			}
			case WESTVERB_PAR_EARLYGAIN2:
			{
				state->Tank[0].EarlyGains[2] = WestVerbParam2LinGain(value);
				state->Tank[1].EarlyGains[2] = WestVerbParam2LinGain(value);
				break;
			}
			case WESTVERB_PAR_DURATION:
			{
				float m = value;
				if (m > 0.99f) m = 0.99f;

				state->Tank[0].Tau  = m;
				state->Tank[1].Tau  = m;

				state->Tank[0].Norm = ComputeTankNorm(&state->Tank[0]);
				state->Tank[1].Norm = ComputeTankNorm(&state->Tank[1]);
				break;
			}
			case WESTVERB_PAR_T_FALL:
			{
				for (ch = 0; ch < 2; ch++)
				{
					westverb_chain *Tank = &state->Tank[ch];

					for (i = 0; i < Tank->NumStages; i++)
					{
						westverb_allpass *ap = &Tank->Allpasses[i];
						ap->BranchGain = 1.0f / (i*(1.0f - value)*3.0f + 1.0f);
					}
				}

				state->Tank[0].Norm = ComputeTankNorm(&state->Tank[0]);
				state->Tank[1].Norm = ComputeTankNorm(&state->Tank[1]);

				break;
			}
			case WESTVERB_PAR_LPF_FC:
			{
				float Hz = BasicFilterParamToHz(value);
				BiquadComputeNewButterworthLPF(&state->Tank[0].LPF, Hz, state->SampleRate);
				BiquadComputeNewButterworthLPF(&state->Tank[1].LPF, Hz, state->SampleRate);
				break;
			}
			case WESTVERB_PAR_HPF_FC:
			{
				float Hz = BasicFilterParamToHz(value);
				BiquadComputeNewButterworthHPF(&state->Tank[0].HPF, Hz, state->SampleRate);
				BiquadComputeNewButterworthHPF(&state->Tank[1].HPF, Hz, state->SampleRate);
				break;
			}
			case WESTVERB_PAR_DELAY_SCALE:
			{
				for (ch = 0; ch < 2; ch++)
				{
					westverb_chain *Tank = &state->Tank[ch];

					for (i = 0; i < Tank->NumStages; i++)
					{
						westverb_allpass *ap = &Tank->Allpasses[i];
						ap->Delay = (int) ceil(2.0f * value * WestVerbPrimeDelays[i]);
					}
				}

				break;
			}
			case WESTVERB_PAR_BRIGHTNESS:
			{
				for (ch = 0; ch < 2; ch++)
				{
					westverb_chain *Tank = &state->Tank[ch];

					for (i = 0; i < Tank->NumStages; i++)
					{
						westverb_allpass *ap = &Tank->Allpasses[i];
						float Hz = BasicFilterParamToHz(value);
						BiquadComputeNewButterworthLPF(&ap->FDF, Hz, state->SampleRate);
					}
				}

				break;
			}
			case WESTVERB_PAR_LOOP_GAIN:
			{
				for (ch = 0; ch < 2; ch++)
				{
					westverb_chain *Tank = &state->Tank[ch];

					for (i = 0; i < Tank->NumStages; i++)
					{
						westverb_allpass *ap = &Tank->Allpasses[i];
						ap->g = value;
					}
				}

				state->Tank[0].Norm = ComputeTankNorm(&state->Tank[0]);
				state->Tank[1].Norm = ComputeTankNorm(&state->Tank[1]);				

				break;
			}			
		}

		state->Params[index] = value;
	}
}

void WestVerbSetSampleRate(westverb_state *state, float SampleRate)
{
	state->SampleRate = SampleRate;
}

#ifdef DSP_WRAPPERS

void WestVerbPopulateDSPStruct(dsp_effect *DSPEffect)
{
	int p;
	DSPEffect->State         = 0;
	DSPEffect->Process       = westverb_process;
	DSPEffect->Open          = WestVerbOpen;
	DSPEffect->Close         = WestVerbClose;
	DSPEffect->GetParameter  = WestVerbGetParameter;
	DSPEffect->SetParameter  = WestVerbSetParameter;
	DSPEffect->Name          = WestVerbName;
	DSPEffect->SetSampleRate = WestVerbSetSampleRate;
	DSPEffect->NumParameters = WESTVERB_NUM_PARAMS;

	DSPEffect->Unique32BitCode = CCONST('w', 'E', 's', 'V');

	if (DSPEffect->NumParameters)
	{
		DSPEffect->Parameters = malloc(sizeof(dsp_effect_parameter) * DSPEffect->NumParameters);

		#define CreateDSPParam(_tok, _fcn, _name)\
		DSPEffect->Parameters[_tok].Format = _fcn;\
		CopyBytesUpToNAndAppendNULL(DSPEffect->Parameters[_tok].Name, _name, PARAMETER_MAX_NAME);		

		CreateDSPParam(WESTVERB_PAR_DRY_GAIN,    WestVerbPrintLinearToDB,       "Dry Gain");
		CreateDSPParam(WESTVERB_PAR_DURATION,    DSPEffectPrintLinearToLinear,  "Duration");
		CreateDSPParam(WESTVERB_PAR_LOOP_GAIN,   DSPEffectPrintLinearToDB,      "Feedback");
		CreateDSPParam(WESTVERB_PAR_DELAY_SCALE, DSPEffectPrintLinearToPercent, "Density");
		CreateDSPParam(WESTVERB_PAR_BRIGHTNESS,  WestVerbLinearToCutoff,        "Brightness");
		CreateDSPParam(WESTVERB_PAR_T_FALL,      DSPEffectPrintLinearToLinear,  "Liveliness");
		// CreateDSPParam(WESTVERB_PAR_EARLY_GAIN,  WestVerbPrintLinearToDB,       "Early Gain");
		CreateDSPParam(WESTVERB_PAR_ROOM_GAIN,   WestVerbPrintLinearToDB,       "Room Gain");
		CreateDSPParam(WESTVERB_PAR_LPF_FC,      WestVerbLinearToCutoff,        "Low Hz");
		CreateDSPParam(WESTVERB_PAR_HPF_FC,      WestVerbLinearToCutoff,        "High Hz");

		CreateDSPParam(WESTVERB_PAR_EARLYTIME0,  WestVerbLinearToBounceMs,      "Bounce Time 1");
		CreateDSPParam(WESTVERB_PAR_EARLYTIME1,  WestVerbLinearToBounceMs,      "Bounce Time 2");
		CreateDSPParam(WESTVERB_PAR_EARLYTIME2,  WestVerbLinearToBounceMs,      "Bounce Time 3");

		CreateDSPParam(WESTVERB_PAR_EARLYGAIN0,  WestVerbPrintLinearToDB,       "Bounce Gain 1");
		CreateDSPParam(WESTVERB_PAR_EARLYGAIN1,  WestVerbPrintLinearToDB,       "Bounce Gain 2");
		CreateDSPParam(WESTVERB_PAR_EARLYGAIN2,  WestVerbPrintLinearToDB,       "Bounce Gain 3");
	}
}

#endif
