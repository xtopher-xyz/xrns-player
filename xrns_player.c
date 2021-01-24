#include "xrns_player.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "effects/dsp.c"
#include "effects/westverb.c"

#include "stddef.h"

#ifdef INLCUDE_FLATBUFFER_INTERFACE
#include "XRNSSchema_builder.h"
#include "flatcc/flatcc_builder.h"
#endif

#define MA_LOG_LEVEL MA_LOG_LEVEL_VERBOSE
#define MA_DEBUG_OUTPUT
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"

#include "tracy/TracyC.h"

#include "miniz/miniz.c"

/* Return Codes!
 */
#define XRNS_WOULD_WRAP_ROW           ( 1)
#define XRNS_SUCCESS                  ( 0)
#define XRNS_ERR_INVALID_INPUT_PARAM  (-1)
#define XRNS_ERR_NULL_STATE           (-2)
#define XRNS_ERR_WRONG_INPUT_SIZE     (-3)
#define XRNS_ERR_OUT_OF_SAMPLES       (-4)
#define XRNS_ERR_INVALID_TRACK_NAME   (-5) 
#define XRNS_ERR_TRACK_NOT_FOUND      (-6) 
#define XRNS_ERR_PARSING_FAIL         (-7) 

/* Renoise defines this to be 12. Basically this is the polyphony 
 * limit for a single note column within a track. Because notes
 * have a short fadeout time no matter what, even trivial 
 * XRNS files want this to be at least 2.
 */
#define XRNS_MAX_SAMPLERS_PER_COLUMN (12)

#define XRNS_16BIT_CONVERTION        (3.0517578125e-5f)

/* This affects how many samplers we can tell Unity about.
 */
#define XRNS_ACTIVE_SAMPLER_COUNT (128)

#define XRNS_NOTE_BLANK  (0xFF)
#define XRNS_NOTE_OFF    (0xFE)
#define XRNS_NOTE_EFFECT (0xFD)
#define XRNS_NOTE_REAL   (0xFC)

#define XRNS_MISSING_VALUE (0xFFFF)

#define XRNS_PANNING_LERP (0.96f)

#define XRNS_EFFECT_FILTER (0)
#define XRNS_EFFECT_REVERB (1)

#define XRNS_XFADE_MS (1)

#define XRNS_MAX_COLUMNS_PER_TRACK (16)

#define XRNS_STORED_EFFECT_DU  (0)
#define XRNS_STORED_EFFECT_IO  (1)
#define XRNS_STORED_EFFECT_xG  (2)
#define XRNS_STORED_EFFECT_xV  (3)
#define XRNS_STORED_EFFECT_xT  (4)
#define XRNS_STORED_EFFECT_xN  (5)
#define XRNS_STORED_EFFECT_VOL (6)
#define XRNS_STORED_EFFECT_PAN (7)
#define XRNS_STORED_EFFECT_NUM (8)

#define XRNS_MAX_NESTING_DEPTH (6)

#define XRNS_INTERPOLATION_NONE   (0)
#define XRNS_INTERPOLATION_LINEAR (1)
#define XRNS_INTERPOLATION_CUBIC  (2)

#define XRNS_LOOP_MODE_OFF        (0)
#define XRNS_LOOP_MODE_FORWARD    (1)
#define XRNS_LOOP_MODE_BACKWARD   (2)
#define XRNS_LOOP_MODE_PINGPONG   (3)

#define XRNS_FORWARD              (0)
#define XRNS_BACKWARD             (1)

#define XRNS_MAX_NAME   (512)

#define XRNS_NNA_CUT      (0)
#define XRNS_NNA_NOTEOFF  (1)
#define XRNS_NNA_CONTINUE (2)

#define XRNS_CURVE_TYPE_POINTS (0)
#define XRNS_CURVE_TYPE_LINES  (1)
#define XRNS_CURVE_TYPE_CURVES (2)

#define XRNS_ENVELOPE_UNITS_BEATS (0)
#define XRNS_ENVELOPE_UNITS_MS    (1)
#define XRNS_ENVELOPE_UNITS_LINES (2)

#define XRNS_MODULATION_TARGET_VOLUME  (0)
#define XRNS_MODULATION_TARGET_PANNING (1)
#define XRNS_MODULATION_TARGET_PITCH   (2)

#define XRNS_UNIPOLAR (0)
#define XRNS_BIPOLAR  (1)

#define Kilobytes(_amount) (_amount * 1024L)
#define Megabytes(_amount) (Kilobytes(_amount) * 1024L)
#define Gigabytes(_amount) (Megabytes(_amount) * 1024L)

#define EFFECT_ID_0D     (0)
#define EFFECT_ID_0U     (1)
#define EFFECT_ID_0V     (2)
#define EFFECT_ID_0A     (3)
#define EFFECT_ID_0G     (4)
#define EFFECT_ID_0I     (5)
#define EFFECT_ID_0O     (6)
#define EFFECT_ID_0T     (7)
#define EFFECT_ID_0C     (8)
#define EFFECT_ID_0M     (9)
#define EFFECT_ID_0L    (10)
#define EFFECT_ID_0S    (11)
#define EFFECT_ID_0B    (12)
#define EFFECT_ID_0E    (13)
#define EFFECT_ID_0Q    (14)
#define EFFECT_ID_0R    (15)
#define EFFECT_ID_0Y    (16)
#define EFFECT_ID_0N    (17)
#define EFFECT_ID_0P    (18)
#define EFFECT_ID_0W    (19)
#define EFFECT_ID_0X    (20)
#define EFFECT_ID_0Z    (21)
#define EFFECT_ID_0J    (22)
#define EFFECT_ID_ZT    (23)
#define EFFECT_ID_ZL    (24)
#define EFFECT_ID_ZK    (25)
#define EFFECT_ID_ZG    (26)
#define EFFECT_ID_ZB    (27)
#define EFFECT_ID_ZD    (28)
#define EFFECT_ID_0K    (29)
#define EFFECT_ID_COUNT (30)

#define XRNS_MAX_NUM_INSTRUMENTS (255)
#define XRNS_MAX_NUM_TRACKS      (255)

#define GAMBIT_XML_EVENT_NOTHING               (0)
#define GAMBIT_XML_EVENT_ELEMENT_START         (1)
#define GAMBIT_XML_EVENT_ELEMENT_END           (2)
#define GAMBIT_XML_EVENT_ELEMENT_SELFCLOSING   (3)
#define GAMBIT_XML_EVENT_ATTR_NAME             (4)
#define GAMBIT_XML_EVENT_ATTR_VALUE            (5)
#define GAMBIT_XML_EVENT_EOF                   (6)
#define GAMBIT_XML_EVENT_BROKEN                (7)

/* Pass the XML document in as a NULL terminated string at init, it must persist for
 * the duration of the parsing.
 */

/* char *xml;
 * gambit_xml x;
 * gambit_xml_init(&x, xml);
 *
 * xml_res r;
 * while (1) {
 *   r = gambit_xml_eat_char(&x);
 *   if (r.event_type == GAMBIT_XML_EOF) break;
 *   switch(r.event_type) {
 *     case (GAMBIT_XML_???) : {}
 *   }
 *   xml++;
 * }
 */

/* A single event is returned per call to gambit_xml_eat_char, and events are returned once.
 * (GAMBIT_XML_EVENT_NOTHING) is returned if no event occured.
 */

/*        +-> GAMBIT_XML_EVENT_ELEMENT_START (name => "Burger")
 *        |
 *        |
 *        |          +-> GAMBIT_XML_EVENT_ELEMENT_END (value => "0.99", name => "Price")
 *        |          |
 * <Burger>          |
 * <Price>0.99</Price>
 *       |           
 *       |           
 *       |
 *       +-> GAMBIT_XML_EVENT_ELEMENT_START (xml_res->name => "Price")
 *
 * </Burger>
 *         |
 *         |
 *         +-> GAMBIT_XML_EVENT_ELEMENT_END (value => "<Price>0.99</Price>", name => "Burger")
 *
 *                  +-> GAMBIT_XML_EVENT_ATTR_VALUE (value => "Large")
 *                  |
 *                  |
 * <Meal Size="Large">
 *           |
 *           |
 *           +-> GAMBIT_XML_EVENT_ATTR_NAME (xml_res->name => "Size")
 *
 * <SingleThingy/>
 *               |
 *               |
 *               +-> GAMBIT_XML_EVENT_ELEMENT_SELFCLOSING (xml_res->name => "SingleThingy")
 */

typedef struct
{
    int   event_type;
    char *value;
    char *name;
} xml_res;

typedef struct
{
    xml_res       r;
    char          attribute_side;
    char         *xml;
    char         *stack;
    char         *stack_val;
    char         *attribute;
    char         *value;
    char          b_parsing_attribute;
} gambit_xml;

void gambit_xml_init(gambit_xml *g, char *xml)
{
    int i;
    for (i = 0; i < sizeof(gambit_xml); i++)
        ((char *)g)[i] = 0;

    g->xml = xml;
}

xml_res gambit_xml_parse_one_char(gambit_xml *g)
{
    char *xmlptr = g->xml;

    g->r.event_type = GAMBIT_XML_EVENT_NOTHING;

    while (g->r.event_type == GAMBIT_XML_EVENT_NOTHING)
    {   
        char c = *xmlptr;

        switch (c)
        {
            case '\0':
            {
                g->r.event_type = GAMBIT_XML_EVENT_EOF;
                goto xmlexit;           
            }
            case '<':
            {
                g->stack = xmlptr + 1;

                if (xmlptr[1] == '?' || xmlptr[1] == '!') {xmlptr = strstr(xmlptr, ">"); goto xmlexit;}
                if (xmlptr[1] == '/')
                {
                    xmlptr = strstr(xmlptr, ">");
                    goto stupid;
                }

                while (xmlptr[1] != ' ' && xmlptr[1] != '/' && xmlptr[1] != '>')
                    xmlptr++;

                g->b_parsing_attribute  = 0;
                g->attribute_side       = 0;

                break;
            }
            case '>':
            {
                if (xmlptr[-1] == '/')
                {
                    g->r.event_type = GAMBIT_XML_EVENT_ELEMENT_SELFCLOSING;
                    g->r.name       = g->stack;
                    g->r.value      = 0;
                }
                else
                {
                    if (g->stack[0] == '/')
                    { 
                        g->r.event_type = GAMBIT_XML_EVENT_ELEMENT_END;
                        g->r.name       = g->stack + 1;
                        g->r.value      = g->stack_val;
                    }
                    else 
                    {
                        g->r.event_type = GAMBIT_XML_EVENT_ELEMENT_START;
                        g->r.name       = g->stack;
                        g->stack_val    = xmlptr + 1;
                    }
                }

                g->b_parsing_attribute = 0;

                xmlptr = strstr(xmlptr, "<");

                if (!xmlptr)
                {
                    g->r.event_type = GAMBIT_XML_EVENT_EOF;
                    goto xmlexit;
                }

                goto stupid;
                break;
            }
            case '&':
            {
                break;
            }
            case '\'':
            case '"':
            {
                if (g->attribute_side == 1)
                {
                    g->attribute_side = 2;
                    g->b_parsing_attribute = 0;
                }
                else if (g->attribute_side == 2)
                {
                    g->r.event_type   = GAMBIT_XML_EVENT_ATTR_VALUE;
                    g->r.value        = g->attribute;
                    g->attribute_side = 0;
                }

                break;
            }
            case '=':
            {
                if (g->b_parsing_attribute)
                {
                    g->r.event_type = GAMBIT_XML_EVENT_ATTR_NAME;
                    g->r.name       = g->attribute;
                    g->b_parsing_attribute = 0;
                    g->attribute_side = 1;
                }
                break;
            }
            default:
            {
                if (c == ' ')
                {
                    g->r.event_type = GAMBIT_XML_EVENT_ELEMENT_START;
                    g->r.name       = g->stack;
                }
                else if (!g->b_parsing_attribute)
                {
                    g->b_parsing_attribute = 1;
                    g->attribute           = xmlptr;
                }
                break;
            }
        }

    xmlexit:
        xmlptr++;
    stupid:
        (void) 0;
    }

    g->xml = xmlptr;
    return g->r;
}

typedef void * (*xrns_worker_fcn) (void *);

typedef struct
{
    xrns_worker_fcn  WorkFunction;
    void            *Data;
    void            *FreeData;
    void            *Result;
    int              bInProgress;
    int              bCompleted;
} xrns_job;

typedef struct
{
    int              NumJobs;
    xrns_job        *Jobs;
    ma_mutex         Lock;
    int              bFinished;
} work_table;

typedef struct
{
    int              NumThreads;
    ma_thread       *Threads;
    ma_semaphore    *Work;
    ma_semaphore    *Relax;
    void           **States;
    work_table      *WorkTable;
    int              bActive;
} pooled_threads_ctx;

work_table *CreateWorkTable(int NumJobs)
{
    work_table *WorkTable = malloc(sizeof(work_table));
    memset(WorkTable, 0, sizeof(work_table));

    if (NumJobs)
    {
        WorkTable->Jobs = malloc(sizeof(xrns_job) * NumJobs);
        memset(WorkTable->Jobs, 0, sizeof(xrns_job) * NumJobs);
    }

    WorkTable->NumJobs   = NumJobs;
    WorkTable->bFinished = 0;
    ma_mutex_init(&WorkTable->Lock);

    return WorkTable;
}

void AddToWorkTable(work_table *WorkTable, xrns_job Job)
{
    WorkTable->NumJobs++;
    if (!WorkTable->Jobs)
    {
        WorkTable->Jobs = malloc(WorkTable->NumJobs * sizeof(xrns_job));
    }
    else
    {
        WorkTable->Jobs = realloc(WorkTable->Jobs, WorkTable->NumJobs * sizeof(xrns_job));
    }

    Job.bInProgress = 0;
    Job.bCompleted  = 0;
    WorkTable->Jobs[WorkTable->NumJobs - 1] = Job;
}

void FreeWorkTable(work_table *WorkTable)
{
    for (int i = 0; i < WorkTable->NumJobs; i++)
    {
        if (WorkTable->Jobs[i].FreeData)
            free(WorkTable->Jobs[i].FreeData);
    }

    ma_mutex_uninit(&WorkTable->Lock);
    free(WorkTable->Jobs);
    free(WorkTable);
}

ma_thread_result pooled_worker_fcn(pooled_threads_ctx *PooledThreads)
{
#ifdef TRACY_ENABLE
    ___tracy_init_thread();
#endif

    while (PooledThreads->bActive)
    {
        ma_semaphore_wait(PooledThreads->Work);
        if (!PooledThreads->bActive) break;

        xrns_job *Job = NULL;

        do
        {
            if (!PooledThreads->WorkTable) break;

            ma_mutex_lock(&PooledThreads->WorkTable->Lock);
            for (int w = 0; w < PooledThreads->WorkTable->NumJobs; w++)
            {
                Job = &PooledThreads->WorkTable->Jobs[w];
                if (Job && !Job->bInProgress && !Job->bCompleted)
                {
                    Job->bInProgress = 1;
                    break;
                }
                else
                {
                    Job = NULL;
                }
            }
            ma_mutex_unlock(&PooledThreads->WorkTable->Lock);

            if (!Job) break;

            Job->Result = Job->WorkFunction(Job->Data);

            ma_mutex_lock(&PooledThreads->WorkTable->Lock);
            Job->bCompleted  = 1;
            Job->bInProgress = 0;
            ma_mutex_unlock(&PooledThreads->WorkTable->Lock);

        } while (Job);

        ma_semaphore_release(PooledThreads->Relax);
    }

    return 0;
}

pooled_threads_ctx *CreatePooledThreads(int NumThreads)
{
    int i;
    pooled_threads_ctx *PooledThreads = malloc(sizeof(pooled_threads_ctx));

    PooledThreads->NumThreads = NumThreads; 
    PooledThreads->Threads    = malloc(sizeof(ma_thread) * NumThreads);
    PooledThreads->Work       = malloc(sizeof(ma_semaphore));
    PooledThreads->Relax      = malloc(sizeof(ma_semaphore));
    PooledThreads->WorkTable  = NULL;
    PooledThreads->bActive    = 1;

    ma_semaphore_init(0, PooledThreads->Work);
    ma_semaphore_init(0, PooledThreads->Relax);

    for (i = 0; i < NumThreads; i++)
        ma_thread_create(&PooledThreads->Threads[i], ma_thread_priority_normal, 0, pooled_worker_fcn, PooledThreads);

    return PooledThreads;
}

void FarmPooledThreads(pooled_threads_ctx *PooledThreads, work_table *WorkTable)
{
    int i;

    PooledThreads->WorkTable = WorkTable;
    for (i = 0; i < PooledThreads->NumThreads; i++) ma_semaphore_release(PooledThreads->Work);
    for (i = 0; i < PooledThreads->NumThreads; i++) ma_semaphore_wait(PooledThreads->Relax);
}

void FreePooledThreads(pooled_threads_ctx *PooledThreads)
{
    int i;

    PooledThreads->bActive = 0;

    for (i = 0; i < PooledThreads->NumThreads; i++)
        ma_semaphore_release(PooledThreads->Work);
    for (i = 0; i < PooledThreads->NumThreads; i++)
        ma_thread_wait(&PooledThreads->Threads[i]);

    ma_semaphore_uninit(PooledThreads->Relax);
    ma_semaphore_uninit(PooledThreads->Work);

    free(PooledThreads->Work);
    free(PooledThreads->Relax);
    free(PooledThreads->Threads);
    free(PooledThreads);
}

typedef struct
{
    void    *Memory;
    size_t   CurrentSize;
    size_t   MaxSize;
} xrns_growing_buffer;

void xrns_growing_buffer_init(xrns_growing_buffer *Buf, size_t Sz)
{
    Buf->MaxSize     = Sz;
    Buf->CurrentSize = 0u;
    Buf->Memory      = malloc(Sz);
}
    
void xrns_growing_buffer_append(xrns_growing_buffer *Buf, void *Mem, size_t Sz)
{
    while (Buf->MaxSize < Buf->CurrentSize + Sz)
    {
        Buf->MaxSize *= 2;
        Buf->Memory = realloc(Buf->Memory, Buf->MaxSize);
    }

    memcpy((char *) Buf->Memory + Buf->CurrentSize, Mem, Sz);
    Buf->CurrentSize += Sz;
}

void xrns_magic_write_int(xrns_growing_buffer *Buf, int Value, int Offset)
{
    while (Offset * sizeof(int) >= Buf->MaxSize)
    {
        Buf->MaxSize *= 2;
        Buf->Memory = realloc(Buf->Memory, Buf->MaxSize);
    }

    ((int*)Buf->Memory)[Offset] = Value;
}

void xrns_growing_buffer_free(xrns_growing_buffer *Buf)
{
    free(Buf->Memory);
}

typedef struct 
{
    xrns_growing_buffer EnvelopesPerTrackPerPattern;
    int          RenoiseVersion;
    unsigned int NumTracks;
    unsigned int NumInstruments;
    unsigned int PatternSequenceLength;
    unsigned int NumPatterns;
    unsigned int NumSamplesPerInstrument[XRNS_MAX_NUM_INSTRUMENTS];
    unsigned int NumSampleSplitMapsPerInstrument[XRNS_MAX_NUM_INSTRUMENTS];
    unsigned int NumModulationSetsPerInstrument[XRNS_MAX_NUM_INSTRUMENTS];
    unsigned int NumSliceRegionsPerInstrument[XRNS_MAX_NUM_INSTRUMENTS];
    unsigned int NumEffectUnitsPerTrack[XRNS_MAX_NUM_TRACKS];
    unsigned int NumEnvelopesPerTrack[XRNS_MAX_NUM_TRACKS];
} xrns_file_counts;

#define ZIP_LOCAL_FILE_HEADER_SIG        (0x04034b50)
#define ZIP_CENTRAL_DIRECTORY_HEADER_SIG (0x02014b50)
#define ZIP_END_OF_CENTRAL_DIRECTORY_SIG (0x06054b50)

#define ZIP_DATA_DESCRIPTOR_SZ (12 + 4)

// @Optimization: Remove this conditional.
#define XRNS_ACCESS_STEREO_SAMPLE(x) ((x >= 0 && x < MaxLengthSamples*2) ? pcm[x] : 0)
#define XRNS_ACCESS_MONO_SAMPLE(x) ((x >= 0 && x < MaxLengthSamples) ? pcm[x] : 0)

typedef struct
{
    float Left;
    float Right;
} xrns_panning_gains;

typedef struct
{
    float Val;
    float Target;
    float alpha;
} LerpFloat;

void ResetLerp(LerpFloat *L, float Val, float alpha)
{
    L->Val    = Val;
    L->Target = Val;
    L->alpha  = alpha;
}

void RunLerp(LerpFloat *L)
{
    if (fabsf(L->Val - L->Target) < 1.0e-6)
    {
        L->Val = L->Target;
    }

    L->Val = L->Val * L->alpha + L->Target * (1.0f - L->alpha);
}

/* Panning law for the panning column is log: 3.0 + 10.0 * log10(xx/128)
 * where xx goes from 0 to 128 (all Left to all Right).
 *
 * Special case for xx == 64, dead center, we force the gains to be 1.0 and 1.0.
 * 
 */
xrns_panning_gains PanningGainFromZeroToOne(float step)
{
    xrns_panning_gains g;
    if (step < 1e-6f)
    {
        g.Left  = 1.4125f;
        g.Right = 0.0f;
        return g;
    }
    else if (fabsf(step - 0.5f) < 1e-6f)
    {
        g.Left  = 1.0f;
        g.Right = 1.0f;
        return g;
    }
    else if (fabsf(step - 1.0f) < 1e-6f)
    {
        g.Left  = 0.0f;
        g.Right = 1.4125f;
        return g;
    }
    else
    {
        g.Left  = 3.0f + 10.0f * log10f(1.0f - step);
        g.Right = 3.0f + 10.0f * log10f(step);
        g.Left  = powf(10.0f, g.Left/20.0f);
        g.Right = powf(10.0f, g.Right/20.0f);
        return g;
    }
}

/* 0x00 = Left 
 * 0x40 = Center
 * 0x80 = Right
 */
xrns_panning_gains PanningGainFromColNumber(float step)
{
    return PanningGainFromZeroToOne(step/128.0f);
}

xrns_panning_gains PanningGainFromNeg1To1(float step)
{
    return PanningGainFromZeroToOne((step + 1.0f)/2.0f);
}

int EffectTypeIdxFromEffectType(char *c)
{
    switch (c[0])
    {
        case '0':
        {
            switch (c[1])
            {
                case 'D': return EFFECT_ID_0D;
                case 'U': return EFFECT_ID_0U;
                case 'V': return EFFECT_ID_0V;
                case 'A': return EFFECT_ID_0A;
                case 'G': return EFFECT_ID_0G;
                case 'I': return EFFECT_ID_0I;
                case 'O': return EFFECT_ID_0O;
                case 'T': return EFFECT_ID_0T;
                case 'C': return EFFECT_ID_0C;
                case 'M': return EFFECT_ID_0M;
                case 'L': return EFFECT_ID_0L;
                case 'S': return EFFECT_ID_0S;
                case 'B': return EFFECT_ID_0B;
                case 'E': return EFFECT_ID_0E;
                case 'Q': return EFFECT_ID_0Q;
                case 'R': return EFFECT_ID_0R;
                case 'Y': return EFFECT_ID_0Y;
                case 'N': return EFFECT_ID_0N;
                case 'P': return EFFECT_ID_0P;
                case 'W': return EFFECT_ID_0W;
                case 'X': return EFFECT_ID_0X;
                case 'Z': return EFFECT_ID_0Z;
                case 'J': return EFFECT_ID_0J;
                case 'K': return EFFECT_ID_0K;
                default:  return XRNS_MISSING_VALUE;
            }
        }

        case 'Z':
        {
            switch (c[1])
            {
                case 'T': return EFFECT_ID_ZT;
                case 'L': return EFFECT_ID_ZL;
                case 'K': return EFFECT_ID_ZK;
                case 'G': return EFFECT_ID_ZG;
                case 'B': return EFFECT_ID_ZB;
                case 'D': return EFFECT_ID_ZD;
                default:  return XRNS_MISSING_VALUE;
            }
        }

        default: return XRNS_MISSING_VALUE;
    }
}

static void *xrns_read_entire_file(char *p_filename, long *p_file_size)
{
    TracyCZoneN(ctx, "Read Entire File", 1);
    FILE *F = fopen(p_filename, "rb");

    if (!F)
    {
        printf("Failed to open %s", p_filename);
        abort();
        return NULL;
    }

    fseek(F, 0, SEEK_END);
    long file_size = ftell(F);
    fseek(F, 0, SEEK_SET);

    void *file_mem = malloc(file_size + 32);
    if (!file_mem)
    {
        printf("Failed to malloc %ld bytes for %s", file_size, p_filename);
        fclose(F);
        return NULL;
    }

    if (p_file_size) *p_file_size = file_size;

    if (fread(file_mem, 1, file_size, F) != file_size)
    {
        printf("Couldn't read all the bytes for %s\n", p_filename);
        fclose(F);
        return NULL;
    }

    fclose(F);

    TracyCZoneEnd(ctx);

    return file_mem;
}

#define PITCHING_TABLE_LENGTH (193)
static const double PitchingTable[PITCHING_TABLE_LENGTH] = {0.00390625, 0.00413852771234, 0.00438461737621, 0.00464534029298, 0.00492156660115, 0.00521421818035, 0.00552427172802, 0.00585276201905, 0.00620078535925, 0.00656950324417, 0.00696014623547, 0.00737401806783,    0.0078125, 0.00827705542468, 0.00876923475242, 0.00929068058596, 0.0098431332023, 0.0104284363607, 0.011048543456, 0.0117055240381, 0.0124015707185, 0.0131390064883, 0.0139202924709, 0.0147480361357,     0.015625, 0.0165541108494, 0.0175384695048, 0.0185813611719, 0.0196862664046, 0.0208568727214, 0.0220970869121, 0.0234110480762, 0.024803141437, 0.0262780129767, 0.0278405849419, 0.0294960722713,      0.03125, 0.0331082216987, 0.0350769390097, 0.0371627223438, 0.0393725328092, 0.0417137454428, 0.0441941738242, 0.0468220961524, 0.049606282874, 0.0525560259534, 0.0556811698838, 0.0589921445426,       0.0625, 0.0662164433975, 0.0701538780193, 0.0743254446877, 0.0787450656184, 0.0834274908856, 0.0883883476483, 0.0936441923048, 0.099212565748, 0.105112051907, 0.111362339768, 0.117984289085,        0.125, 0.132432886795, 0.140307756039, 0.148650889375, 0.157490131237, 0.166854981771, 0.176776695297, 0.18728838461, 0.198425131496, 0.210224103813, 0.222724679535, 0.23596857817,         0.25, 0.26486577359, 0.280615512077, 0.297301778751, 0.314980262474, 0.333709963543, 0.353553390593, 0.374576769219, 0.396850262992, 0.420448207627, 0.44544935907, 0.471937156341,          0.5, 0.52973154718, 0.561231024155, 0.594603557501, 0.629960524947, 0.667419927085, 0.707106781187, 0.749153538438, 0.793700525984, 0.840896415254, 0.89089871814, 0.943874312682,            1.0, 1.05946309436, 1.12246204831,  1.189207115, 1.25992104989, 1.33483985417, 1.41421356237, 1.49830707688, 1.58740105197, 1.68179283051, 1.78179743628, 1.88774862536,            2.0, 2.11892618872, 2.24492409662, 2.37841423001, 2.51984209979, 2.66967970834, 2.82842712475, 2.99661415375, 3.17480210394, 3.36358566101, 3.56359487256, 3.77549725073,            4.0, 4.23785237744, 4.48984819324, 4.75682846001, 5.03968419958, 5.33935941668, 5.65685424949, 5.99322830751, 6.34960420787, 6.72717132203, 7.12718974512, 7.55099450145,            8.0, 8.47570475487, 8.97969638647, 9.51365692002, 10.0793683992, 10.6787188334, 11.313708499, 11.986456615, 12.6992084157, 13.4543426441, 14.2543794902, 15.1019890029,           16.0, 16.9514095097, 17.9593927729,  19.02731384, 20.1587367983, 21.3574376667, 22.627416998,  23.97291323, 25.3984168315, 26.9086852881, 28.5087589805, 30.2039780058,           32.0, 33.9028190195, 35.9187855459, 38.0546276801, 40.3174735966, 42.7148753334, 45.2548339959, 47.9458264601, 50.796833663, 53.8173705762, 57.017517961, 60.4079560116,           64.0, 67.805638039, 71.8375710918, 76.1092553602, 80.6349471933, 85.4297506669, 90.5096679919, 95.8916529201, 101.593667326, 107.634741152, 114.035035922, 120.815912023,          128.0, 135.611276078, 143.675142184, 152.21851072, 161.269894387, 170.859501334, 181.019335984, 191.78330584, 203.187334652, 215.269482305, 228.070071844, 241.631824047, 256.0};
static const float  PianoPitches[120] = {25.9565f, 27.5000f, 29.1352f, 30.8677f, 32.7032f, 34.6478f, 36.7081f, 38.8909f, 41.2034f, 43.6535f, 46.2493f, 48.9994f, 51.9131f, 55.0000f, 58.2705f, 61.7354f, 65.4064f, 69.2957f, 73.4162f, 77.7817f, 82.4069f, 87.3071f, 92.4986f, 97.9989f, 103.8262f, 110.0000f, 116.5409f, 123.4708f, 130.8128f, 138.5913f, 146.8324f, 155.5635f, 164.8138f, 174.6141f, 184.9972f, 195.9977f, 207.6523f, 220.0000f, 233.0819f, 246.9417f, 261.6256f, 277.1826f, 293.6648f, 311.1270f, 329.6276f, 349.2282f, 369.9944f, 391.9954f, 415.3047f, 440.0000f, 466.1638f, 493.8833f, 523.2511f, 554.3653f, 587.3295f, 622.2540f, 659.2551f, 698.4565f, 739.9888f, 783.9909f, 830.6094f, 880.0000f, 932.3275f, 987.7666f, 1046.5023f, 1108.7305f, 1174.6591f, 1244.5079f, 1318.5102f, 1396.9129f, 1479.9777f, 1567.9817f, 1661.2188f, 1760.0000f, 1864.6550f, 1975.5332f, 2093.0045f, 2217.4610f, 2349.3181f, 2489.0159f, 2637.0205f, 2793.8259f, 2959.9554f, 3135.9635f, 3322.4376f, 3520.0000f, 3729.3101f, 3951.0664f, 4186.0090f, 4434.9221f, 4698.6363f, 4978.0317f, 5274.0409f, 5587.6517f, 5919.9108f, 6271.9270f, 6644.8752f, 7040.0000f, 7458.6202f, 7902.1328f, 8372.0181f, 8869.8442f, 9397.2726f, 9956.0635f, 10548.0818f, 11175.3034f, 11839.8215f, 12543.8540f, 13289.7503f, 14080.0000f, 14917.2404f, 15804.2656f, 16744.0362f, 17739.6884f, 18794.5451f, 19912.1270f, 21096.1636f, 22350.6068f, 23679.6431f, 25087.7079f};

unsigned int NoteToHzAssumingA440(int note_id)
{
    if (note_id > 0 && note_id < 120)
    {
        return PianoPitches[note_id];
    }

    return 0;
}

unsigned char XRNSOctavelessNoteValue(unsigned char Note, unsigned char Sharp)
{
    if (Sharp == '#')
    {
        switch (Note)
        {
            case 'C':
                return 1;
            case 'D':
                return 3;
            case 'F':
                return 6;
            case 'G':
                return 8;
            case 'A':
                return 10;
            default:
                return 1;
        }
    }
    else
    {
        switch (Note)
        {
            case 'C':
                return 0;
            case 'D':
                return 2;
            case 'E':
                return 4;                
            case 'F':
                return 5;
            case 'G':
                return 7;
            case 'A':
                return 9;
            case 'B':
                return 11;
            default:
                return 0;
        }
    }

    return 0;
}

unsigned int ParseXRNSNoteFromXML(char *XML)
{
    unsigned char Note  = XML[0];
    unsigned char Sharp = XML[1];
    unsigned char Octa  = XML[2];

    if (Note == '-' || Octa == '-')
        return XRNS_NOTE_BLANK;
    if (Note == 'O' || Octa == 'F')
        return XRNS_NOTE_OFF;

    return XRNSOctavelessNoteValue(Note, Sharp) + (Octa - '0') * 12;
}

unsigned int Hex2Int(char h)
{
    if (h >= '0' && h <= '9')
        return h - '0';
    else if (h >= 'a' && h <= 'f')
        return h - 'a' + 10;
    else if (h >= 'A' && h <= 'F')
        return h - 'A' + 10;
    else
        return 0;
    return 0;
}

unsigned int Parse2DigitHexFromXML(char *XML)
{
    return 16 * Hex2Int(XML[0]) + Hex2Int(XML[1]);
}

unsigned int Parse2DigitHexOrDotsFromXML(char *XML)
{
    if (XML[0] == '.' && XML[1] == '.')
    {
        return XRNS_MISSING_VALUE;
    }

    return Parse2DigitHexFromXML(XML);
}

/* These are the little 2-digit columns for delay, panning, volume.
 * There are small number of little effects that can be enabled here.
 * Otherwise, regular hex values between [0, 80] 
 * correspond to the column's intended effect.
 */
unsigned int Parse2DigitEffectColumnOrDotsFromXML(char *XML)
{
    switch(XML[0])
    {
        /* regular hex numbers, column acting normal. */
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
            return Parse2DigitHexOrDotsFromXML(XML);
        default:
            return XRNS_MISSING_VALUE;
    }
}

float ParseFloatFromXML(char *XML)
{
    return atof(XML);
}

int ParseIntegerFromXML(char *XML)
{
    return atoi(XML);
}

int ParseIntegerOrDotsFromXML(char *XML)
{
    if (XML[0] == '.' && XML[1] == '.')
    {
        return XRNS_MISSING_VALUE;
    }

    return atoi(XML);
}

int MatchCharsToString(char *Chars, char *String)
{
    int i;
    unsigned int n = strlen(String);
    for (i = 0; i < n; i++)
    {
        if (!Chars) return 0;
        if (!String) return 0;

        if (*Chars++ != *String++)
            return 0;
    }

    return 1;    
}

void CopyBytes(void *dst, void *src, size_t len)
{
    char *sdst = (char *) dst;
    char *ssrc = (char *) src;
    while (len)
    {
        *sdst++ = *ssrc++;
        len--;
    }
}

void CopyBytesAppendingNULL(void *dst, void *src, size_t len)
{
    CopyBytes(dst, src, len);
    ((char *)dst)[len] = 0;
}

unsigned int XMLTagLength(char *xml)
{
    unsigned int len = 0;
    while (xml && *xml != '<')
        {xml++; len++;}
    return len;
}

int xmltagmatch(char *a, char *b)
{
    while (*b)
    {
        if (*a++ != *b++)
            return 0;
    }

    if (*a != ' ' && *a != '>' && *a != '/')
        return 0;

    return 1;
}

int xmlcopy(char *a, char *b)
{
    while (*b)
    {
        if (*a++ != *b++)
            return 0;
    }

    if (*a != ' ' && *a != '>' && *a != '/')
        return 0;

    return 1;
}

#ifndef GAMBIT_CRC
#define GAMBIT_CRC

uint32_t crc_table[256];

uint32_t crc(unsigned char *p, unsigned long len)
{
    int i;
    uint8_t k;
    uint32_t u;
    /* compute the table, dump this to an array in the future */
    /* x^32+x^26+x^23+x^22+x^16+x^12+x^11+x^10+x^8+x^7+x^5+x^4+x^2+x+1. */
    int poly_bits[] = {0,1,2,4,5,7,8,10,11,12,16,22,23,26};
    uint32_t poly = 0u;
    for (i = 0; i < sizeof(poly_bits)/sizeof(poly_bits[0]); i++)
        poly |= (1UL << (31 - poly_bits[i]));

    for (u = 0; u < 256; u++)
    {
        uint32_t c = u;
        for (k = 0; k < 8; k++)
        {
            if (c & 1)
            {
                c  = (c >> 1) ^ (poly);
            }
            else
            {
                c  = (c >> 1);
            }
            
        }
        crc_table[u] = c;
    }

    uint32_t r = 0xffffffffUL;
    while (len)
    {
        r = ((r >> 8)) ^ crc_table[((r) ^ *p++) & 0xFF];
        len--;
    }

    return r ^ 0xffffffffUL;
}

#endif

#pragma pack(push, 1)
typedef struct
{
    uint32_t Track;
    uint32_t Col;
    uint32_t NoteID;
    uint32_t NoteHz;
    uint32_t InstrumentID;
    uint32_t SampleID;
    uint32_t PlaybackDirection;
    float    SecondsPlayingFor;
    float    ActualNoteHz;
} xrns_sampler_info;
#pragma pack(pop)

typedef struct
{
    int16_t     *PCM;

    char        *Name;
    float        Volume;
    float        Panning;
    int          Transpose;
    int          Finetune;
    int          NewNoteAction;
    int          InterpolationMode;
    int          BeatSyncIsActive;
    unsigned int BeatSyncLines;

    int          LoopMode;
    int          LoopRelease;
    unsigned int LoopStart;
    unsigned int LoopEnd;

    unsigned int LengthSamples;
    unsigned int NumChannels;
    double       SampleRateHz;
    int          ModulationSetIndex;

    char         bIsAlisedSample;
    unsigned int SampleStart;

    // @Optimization: We should use a new data structure for this.
    int          InstrumentNumber;
    int          SampleNumber;
    // Added these for the instrument writer.
    int          BaseNote;
    int          NoteStart;
    int          NoteEnd;  
} xrns_sample;

typedef struct
{
    unsigned int Pos;
    float        Val;
    float        Bez;
} xrns_point;

typedef struct 
{
    unsigned int  NumPoints;
    xrns_point   *Points;
    int           LoopMode;
    unsigned int  LoopStart;
    unsigned int  LoopEnd;
    char          bSustainOn;
    unsigned int  SustainPosition;
    int           CurveType;
    unsigned int  Duration;
    unsigned int  ReleaseValue;
    int           Units;
    int           Polarity;
    int           DeviceIndex;
    int           ParameterIndex;
} xrns_envelope;

void ParsePointFromTriple(xrns_point *Point, char *XML)
{   
    Point->Pos = atoi(XML);
    XML = strstr(XML, ",") + 1;
    Point->Val = atof(XML);
    XML = strstr(XML, ",") + 1;
    Point->Bez = atof(XML);
}

typedef struct
{
    int     Type;
    int     Enabled;
    int     NumParameters;
    float   Parameters[32];
} dsp_effect_desc;

typedef struct
{
    int           PitchModulationRange;

    xrns_envelope Volume;
    xrns_envelope Panning;
    xrns_envelope Pitch;

    char          bVolumeEnvelopePresent;
    char          bPanningEnvelopePresent;
    char          bPitchEnvelopePresent;

} xrns_modulation_set;

typedef struct
{
    unsigned int SampleIndex;
    int          UseEnvelopes;
    int          MapVelocityToVolume;
    unsigned int BaseNote;
    unsigned int NoteStart;
    unsigned int NoteEnd;
    unsigned int VelocityStart;
    unsigned int VelocityEnd;
    char         bIsNoteOn; 
} xrns_ssm;  /* TODO: seems like this should just belong to the sample data struct */

typedef struct
{
    char                *Name;
    unsigned int         NumSamples;
    xrns_sample         *Samples;
    unsigned int         NumSampleSplitMaps;
    xrns_ssm            *SampleSplitMaps;

    char                 bIsSliced;
    unsigned int         NumSliceRegions;
    unsigned int        *SliceRegions;

    /* In Renoise 2.x, this can be at most 1. */
    unsigned int         NumModulationSets;
    xrns_modulation_set *ModulationSets;
} xrns_instrument;

typedef struct
{
    /* Must be either 
     *   XRNS_NOTE_EFFECT
     *   XRNS_NOTE_REAL
     */
    unsigned int  Type;
    
    /* only applies to XRNS_NOTE_REAL
     * may be C0 through to B9, so 0 through to 119 inclusive.
     * may also be XRNS_NOTE_BLANK
     *             XRNS_NOTE_OFF
     */
    unsigned int  Note;            
    unsigned int  Column;          /* ignored by XRNS_NOTE_EFFECT */
    unsigned int  Line;
    unsigned int  Instrument;      /* permitted to be XRNS_MISSING_VALUE */
    unsigned int  Volume;          /* permitted to be XRNS_MISSING_VALUE */ 
    unsigned int  Panning;         /* permitted to be XRNS_MISSING_VALUE */
    unsigned int  Delay;           /* permitted to be XRNS_MISSING_VALUE */

    char VolumeEffect[2];          /* permitted to be \0\0 */
    char PanningEffect[2];         /* permitted to be \0\0 */

    /* If Type is XRNS_NOTE_EFFECT, these will be populated and valid.
     * Otherwise, these will both be populated and set if a Renoise 3.x file
     * was loaded and sample-effects were found. Otherwise these will be
     * XRNS_MISSING_VALUE.
     */
    unsigned int  EffectTypeIdx;
    unsigned int  EffectValue;

    char EffectTypeC[2];

} xrns_note;

void InitNote(xrns_note *Note)
{
    Note->Type          = XRNS_NOTE_REAL;
    Note->Note          = XRNS_NOTE_BLANK;
    Note->Column        = 0;
    Note->Line          = 0;
    Note->Instrument    = XRNS_MISSING_VALUE;
    Note->Volume        = XRNS_MISSING_VALUE;
    Note->Panning       = XRNS_MISSING_VALUE;
    Note->Delay         = XRNS_MISSING_VALUE;
    Note->EffectTypeIdx = XRNS_MISSING_VALUE;
    Note->EffectValue   = XRNS_MISSING_VALUE;
}

typedef struct
{
    unsigned int   NumNotes;
    char           bIsAlias;
    unsigned int   AliasIdx;
    xrns_note     *Notes;
    unsigned int   NumEnvelopes;
    xrns_envelope *Envelopes;
} xrns_track;

typedef struct
{
    unsigned int     NumColumns;
    unsigned int     NumEffectColumns;
    char             bIsGroup;
    unsigned int     WrapsNPreviousTracks;
    unsigned int     Depth;
    char            *Name;
    float            InitialPreVolume;
    float            InitialPostVolume;
    float            InitialPanning;
    float            PostPanning;
    float            InitialWidth;
    unsigned int     NumDSPEffectUnits;
    dsp_effect_desc *DSPEffectDescs;
} xrns_track_desc;

typedef struct
{
    char          *Name;
    unsigned int   NumberOfLines;
    xrns_track    *Tracks;
} xrns_pattern;

typedef struct 
{
    unsigned int  PatternIdx;
    char         *SectionName;
    char          bIsSectionStart;
    unsigned int  NumMutedTracks;
    unsigned int *MutedTracks;
} xrns_pattern_sequence_entry;

typedef struct
{
    int                           RenoiseVersion;
    float                         BeatsPerMin;
    int                           LinesPerBeat;
    unsigned int                  TicksPerLine;
    char                         *SongName; 
    char                         *Artist;
    unsigned int                  NumInstruments;
    xrns_instrument              *Instruments;
    unsigned int                  NumPatterns;
    xrns_pattern                 *PatternPool;
    unsigned int                  PatternSequenceLength;
    xrns_pattern_sequence_entry  *PatternSequence;
    unsigned int                  NumTracks;
    xrns_track_desc              *Tracks;
    unsigned int                  TotalColumns;
} xrns_document;

#pragma pack(push, 1)
typedef struct
{
    uint32_t HeaderSignaturelocal;
    uint16_t VersionNeededToExtract;
    uint16_t GeneralPurposeBitFlag; 
    uint16_t CompressionMethod;     
    uint16_t LastModFileTime;       
    uint16_t LastModFileDate;       
    uint32_t CRC32;                 
    uint32_t CompressedSize;        
    uint32_t UncompressedSize;      
    uint16_t FileNameLength;        
    uint16_t ExtraFieldLength;      
} zip_local_file_header;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct
{
    uint32_t HeaderSignature;
    uint16_t VersionMadeBy; 
    uint16_t VersionNeededToExtract; 
    uint16_t GeneralPurposeBitFlag;
    uint16_t CompressionMethod;
    uint16_t LastModeFileTime;
    uint16_t LastModFileDate;
    uint32_t CRC32;
    uint32_t CompressedSize;
    uint32_t UncompressedSize;
    uint16_t FileNameLength;
    uint16_t ExtraFieldLength;
    uint16_t FileCommentLength;
    uint16_t DiskNumberStart;
    uint16_t InternalFileAttributes;
    uint32_t ExternalFileAttributes;
    uint32_t RelativeOffsetOfLocalHeader;
    /*
    file name (variable size)
    extra field (variable size)
    file comment (variable size)
    */
} zip_central_directory_header;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct
{
    uint32_t Signature;
    uint16_t NumberOfThisDisk;
    uint16_t NumberOfThisDiskWStart;
    uint16_t TotalEntriesOnThisDisk;
    uint16_t TotalEntriesOnInCD;
    uint32_t SizeOfCentralDirectory;
    uint32_t OffsetOfStartOfCentralEtcEtc;
    uint16_t ZIPFileCommentLength;
      /*uint16_t .ZIP file comment       (variable size)*/
} zip_end_of_central_directory_record;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct
{
    uint32_t Signature;
    uint32_t CRC32;
    uint32_t CompressedSize;
    uint32_t UncompressedSize;
} zip_data_descriptor;
#pragma pack(pop)

size_t zip_end_of_central_directory_record_size(uint8_t *p)
{
    zip_end_of_central_directory_record *z = (zip_end_of_central_directory_record *) p;
    return sizeof(zip_end_of_central_directory_record) + z->ZIPFileCommentLength;
}

size_t zip_local_file_header_size(uint8_t *p, char *c)
{
    int i;
    zip_local_file_header *z = (zip_local_file_header *) p;    
    p += sizeof(zip_local_file_header);
    for (i = 0; i < z->FileNameLength && (i < (XRNS_MAX_NAME-1)); i++)
        c[i] = p[i];
    c[i] = 0;
    return sizeof(zip_local_file_header) + z->FileNameLength + z->ExtraFieldLength;
}

size_t zip_central_directory_header_size(uint8_t *p)
{
    zip_central_directory_header *z = (zip_central_directory_header *) p;
    return sizeof(zip_central_directory_header) + z->FileNameLength + z->ExtraFieldLength + z->FileCommentLength;
}

static inline uint32_t zip_sig(uint8_t *z)
{
    return *((uint32_t *) z);
}

uint8_t *gambit_unzip_single_file(uint8_t *p_deflate_stream, uint8_t *p_dest, mz_ulong *compressed_size, mz_ulong *uncompressed_size)
{
    if (MZ_OK != mz_uncompress_skip_header(p_dest, uncompressed_size, p_deflate_stream, *compressed_size))
    {
        printf("Can't uncompress this shit.\n");
    }
    return p_dest;
}

uint8_t *gambit_unzip_single_file_allocate(uint8_t *p_deflate_stream, mz_ulong *compressed_size, mz_ulong *uncompressed_size)
{
    void *p_out_mem = malloc(*uncompressed_size);
    if (MZ_OK != mz_uncompress_skip_header(p_out_mem, uncompressed_size, p_deflate_stream, *compressed_size))
    {
        printf("Can't uncompress this shit.\n");
    }
    p_out_mem = realloc(p_out_mem, *uncompressed_size);
    return p_out_mem;
}

typedef struct
{
    zip_local_file_header Header;
    char                  FileName[XRNS_MAX_NAME];
    char                 *p_mem;
    char                  b_filename_matched;
} gambit_zip_entry;

typedef struct
{
    uint8_t *locals;
    size_t   locals_len;
    uint8_t *centrals;
    size_t   centrals_len;    
    int      total_num_files;
} zip_file_write_context;

void gambit_zip_start_writing(zip_file_write_context *ctx)
{
    ctx->locals          = 0;
    ctx->locals_len      = 0;
    ctx->centrals        = 0;
    ctx->centrals_len    = 0;
    ctx->total_num_files = 0;
}

void write_entire_file(char *p_filename, void *data, long file_size)
{
    FILE *F = fopen(p_filename, "wb");
    fwrite(data, 1, file_size, F);
    fclose(F);
}

void write_n_grow(uint8_t **p, size_t *sz, uint8_t *bytesToWrite, size_t numBytes)
{
    *sz += numBytes;
    if (*p)
        *p = realloc(*p, *sz);
    else
        *p = malloc(*sz);
    
    memcpy(*p + *sz - numBytes, bytesToWrite, numBytes);
}

void gambit_zip_write_file(zip_file_write_context *ctx, uint8_t *mem, size_t sz, int b_compress, char *path)
{
    zip_local_file_header        localheader;
    zip_central_directory_header centralheader;

    localheader.HeaderSignaturelocal = ZIP_LOCAL_FILE_HEADER_SIG;
    localheader.VersionNeededToExtract = 20;
    localheader.GeneralPurposeBitFlag = 0;
    localheader.LastModFileTime = 0;
    localheader.LastModFileDate = 0;
    localheader.UncompressedSize = sz;
    localheader.FileNameLength = strlen(path);
    localheader.ExtraFieldLength = 0;
    
    centralheader.HeaderSignature = ZIP_CENTRAL_DIRECTORY_HEADER_SIG;
    centralheader.VersionMadeBy = 20;
    centralheader.VersionNeededToExtract = 20;
    centralheader.GeneralPurposeBitFlag = 0;
    centralheader.LastModeFileTime = 0;
    centralheader.LastModFileDate = 0;
    centralheader.UncompressedSize = sz;
    centralheader.FileNameLength = strlen(path);
    centralheader.ExtraFieldLength = 0;
    centralheader.FileCommentLength = 0;
    centralheader.DiskNumberStart = 0;
    centralheader.InternalFileAttributes = 0;
    centralheader.ExternalFileAttributes = 0;

    localheader.CRC32   = crc(mem, sz); 
    centralheader.CRC32 = localheader.CRC32;
    ctx->total_num_files++;

    centralheader.RelativeOffsetOfLocalHeader = ctx->locals_len;

    if (b_compress)
    {
        unsigned long compressed_sz;
        uint8_t *Compressed;

        compressed_sz = compressBound(sz);
        Compressed = malloc(compressed_sz);
        compress(Compressed, &compressed_sz, mem, sz);
        localheader.CompressionMethod = 8;                  centralheader.CompressionMethod = localheader.CompressionMethod;
        /* take off the annoying header and the checksum at the end */
        localheader.CompressedSize = compressed_sz - 2 - 4; centralheader.CompressedSize = localheader.CompressedSize;
        
        write_n_grow(&ctx->locals, &ctx->locals_len, (uint8_t *) &localheader,   sizeof(zip_local_file_header));
        write_n_grow(&ctx->locals, &ctx->locals_len, (uint8_t *) path,           strlen(path));
        write_n_grow(&ctx->locals, &ctx->locals_len, (uint8_t *) Compressed + 2, localheader.CompressedSize);

        free(Compressed);
    }
    else
    {
        localheader.CompressionMethod = 0; centralheader.CompressionMethod = localheader.CompressionMethod;
        localheader.CompressedSize = sz;   centralheader.CompressedSize = localheader.CompressedSize;

        write_n_grow(&ctx->locals, &ctx->locals_len, (uint8_t *) &localheader,   sizeof(zip_local_file_header));
        write_n_grow(&ctx->locals, &ctx->locals_len, (uint8_t *) path,           strlen(path));
        write_n_grow(&ctx->locals, &ctx->locals_len, (uint8_t *) mem,            sz);
    }

    write_n_grow(&ctx->centrals, &ctx->centrals_len, (uint8_t *) &centralheader, sizeof(zip_central_directory_header));
    write_n_grow(&ctx->centrals, &ctx->centrals_len, (uint8_t *) path,           strlen(path));
}

void gambit_zip_finish_writing_and_save(zip_file_write_context *ctx, char *p_filename)
{
    uint8_t *mem = 0;
    size_t   len = 0;

    write_n_grow(&mem, &len, ctx->locals,   ctx->locals_len);
    write_n_grow(&mem, &len, ctx->centrals, ctx->centrals_len);

    zip_end_of_central_directory_record footer;
    footer.Signature = ZIP_END_OF_CENTRAL_DIRECTORY_SIG;
    footer.NumberOfThisDisk = 0;
    footer.NumberOfThisDiskWStart = 0;
    footer.TotalEntriesOnThisDisk = ctx->total_num_files;
    footer.TotalEntriesOnInCD = ctx->total_num_files;
    footer.SizeOfCentralDirectory = ctx->centrals_len;
    footer.OffsetOfStartOfCentralEtcEtc = ctx->locals_len;
    footer.ZIPFileCommentLength = 0;

    write_n_grow(&mem, &len, (uint8_t *) &footer, sizeof(zip_end_of_central_directory_record));
    write_entire_file(p_filename, mem, len);
    free(ctx->locals);
    free(ctx->centrals);
    free(mem);
}

typedef struct
{
    uint8_t *base_zip;
    uint8_t *pzip;
    size_t   zip_sz;
} gambit_zip_parsing_state;

void gambit_start_parsing(gambit_zip_parsing_state *zs, uint8_t *p_zip, size_t zip_sz)
{
    zs->base_zip = p_zip;
    zs->pzip     = p_zip;
    zs->zip_sz   = zip_sz;
}

/* Return the file in a buffer, decompressing if nessescary.
 * Caller should free the p_mem pointer when finished.
 * p_mem is 0 if anything went wrong.
 */
gambit_zip_entry gambit_parse_next_file(gambit_zip_parsing_state *zs, char *only_unpack_this_filename)
{
    gambit_zip_entry z;
    z.p_mem = 0;
    z.b_filename_matched = 0;

    int b_done = 0;

    while (!b_done)
    {
        uint8_t *p_out_mem = 0;
        char c[XRNS_MAX_NAME];

        switch(zip_sig(zs->pzip)) {
        case ZIP_LOCAL_FILE_HEADER_SIG:
        {
            zip_local_file_header lfh = *((zip_local_file_header *) zs->pzip);
            zs->pzip += zip_local_file_header_size(zs->pzip, c);

            if (!lfh.CompressedSize && !lfh.CompressionMethod && !(lfh.GeneralPurposeBitFlag & (1u << 3)))
            {
                /* totally blank thing */
                continue;
            }

            if (lfh.CompressionMethod == 0)
            {
                /* These will be the samples, FLAC already compressed them
                p_out_mem = malloc(lfh.UncompressedSize);
                memcpy(p_out_mem, zs->pzip, lfh.UncompressedSize);
                 */
                p_out_mem = zs->pzip;
            }

            if (lfh.CompressionMethod == 8)
            {
                /* Almost sure to be the Song.xml file. */

                /* If we don't have the size, we have to unzip it to
                 * reliably skip through the file. If we *do* have the
                 * size, then we see if the filename filter can save us
                 * the hassle.
                 */
                if (lfh.CompressedSize == 0 || lfh.UncompressedSize == 0)
                {
                    z.p_mem = 0;
                    return z;
                }

                if (!only_unpack_this_filename || !strcmp(only_unpack_this_filename, c))
                {
                    mz_ulong CompressedSize = 0;
                    mz_ulong UncompressedSize = 0;

                    CompressedSize = (mz_ulong) lfh.CompressedSize;
                    UncompressedSize = (mz_ulong) lfh.UncompressedSize;

                    p_out_mem =
                    gambit_unzip_single_file_allocate(
                        zs->pzip,
                        &CompressedSize,
                        &UncompressedSize
                        );

                    lfh.CompressedSize = (uint32_t) CompressedSize;
                    lfh.UncompressedSize = (uint32_t) UncompressedSize;

                    z.b_filename_matched = !!(only_unpack_this_filename);
                }
            }

            zs->pzip += lfh.CompressedSize;

            if (lfh.GeneralPurposeBitFlag & (1u << 3))
            {
                zip_data_descriptor zdd = *((zip_data_descriptor *) zs->pzip);
                zs->pzip += ZIP_DATA_DESCRIPTOR_SZ;
            }

            z.p_mem = p_out_mem;
            z.Header = lfh;
            strcpy(z.FileName, c);
            return z;
        }
        case ZIP_CENTRAL_DIRECTORY_HEADER_SIG:
        {
            zip_central_directory_header cdh = *((zip_central_directory_header *) zs->pzip);
            zs->pzip += zip_central_directory_header_size(zs->pzip);
            break;
        }
        case ZIP_END_OF_CENTRAL_DIRECTORY_SIG:
        {
            zip_end_of_central_directory_record eofcdr = *((zip_end_of_central_directory_record *) zs->pzip);
            zs->pzip += zip_end_of_central_directory_record_size(zs->pzip);
            break;
        }
        default:
        {
            printf("Found something unknown in the ZIP file (sig 0x%x).\n", zip_sig(zs->pzip));
            zs->pzip++; /* not ideal, but occasionally I see ZIP files with completely random empty padding... */
            break;
        }
        }

        if (zs->pzip - zs->base_zip >= zs->zip_sz)
            b_done = 1;
    }

    z.p_mem = 0;
    return z;
}

gambit_zip_entry gambit_fetch_zipped_file_by_name(void *p_mem, size_t zip_sz, char *p_filename)
{
    gambit_zip_entry z; z.p_mem = 0;
    gambit_zip_parsing_state zs;
    gambit_start_parsing(&zs, p_mem, zip_sz);
    char c[2048];
    int i;

    do
    {
        z = gambit_parse_next_file(&zs, p_filename);
        if (!z.p_mem) break;
        for (i = 0; i < z.Header.FileNameLength; i++)
            c[i] = z.FileName[i];
        c[i] = 0;
        if (!strncmp(c, p_filename, XRNS_MAX_NAME))
        {
            printf("found and pulled %s\n", p_filename);
            return z;
        }
        else
        {
            free(z.p_mem);
        }

    } while (z.p_mem);
    z.p_mem = 0;
    return z;
}

#define XRNS_TAGS XRNS_KERNAL(AliasPatternIndex)\
XRNS_KERNAL(AudioPluginDevice)\
XRNS_KERNAL(Automations)\
XRNS_KERNAL(Envelope)\
XRNS_KERNAL(Envelopes)\
XRNS_KERNAL(FilterDevices)\
XRNS_KERNAL(GlobalProperties)\
XRNS_KERNAL(GroupTrackMixerDevice)\
XRNS_KERNAL(Instrument)\
XRNS_KERNAL(Instruments)\
XRNS_KERNAL(IsActive)\
XRNS_KERNAL(Mapping)\
XRNS_KERNAL(MasterTrackMixerDevice)\
XRNS_KERNAL(ModulationSet)\
XRNS_KERNAL(ModulationSets)\
XRNS_KERNAL(MutedTracks)\
XRNS_KERNAL(NoteOnMapping)\
XRNS_KERNAL(Panning)\
XRNS_KERNAL(Parameter)\
XRNS_KERNAL(Parameters)\
XRNS_KERNAL(Pattern)\
XRNS_KERNAL(PatternGroupTrack)\
XRNS_KERNAL(PatternMasterTrack)\
XRNS_KERNAL(Patterns)\
XRNS_KERNAL(PatternSequence)\
XRNS_KERNAL(PatternTrack)\
XRNS_KERNAL(PhraseGenerator)\
XRNS_KERNAL(PluginGenerator)\
XRNS_KERNAL(PluginProperties)\
XRNS_KERNAL(PostPanning)\
XRNS_KERNAL(PostVolume)\
XRNS_KERNAL(Sample)\
XRNS_KERNAL(SampleEnvelopeModulationDevice)\
XRNS_KERNAL(SampleEnvelopes)\
XRNS_KERNAL(Samples)\
XRNS_KERNAL(SampleSplitMap)\
XRNS_KERNAL(SequenceEntries)\
XRNS_KERNAL(SequenceEntry)\
XRNS_KERNAL(SequencerGroupTrack)\
XRNS_KERNAL(SequencerMasterTrack)\
XRNS_KERNAL(SequencerMasterTrackDevice)\
XRNS_KERNAL(SequencerSendTrack)\
XRNS_KERNAL(SequencerTrack)\
XRNS_KERNAL(SequencerTrackDevice)\
XRNS_KERNAL(SliceMarker)\
XRNS_KERNAL(SliceMarkers)\
XRNS_KERNAL(Surround)\
XRNS_KERNAL(TrackMixerDevice)\
XRNS_KERNAL(Tracks)\
XRNS_KERNAL(Volume)

#define XRNS_SUBSET_TAGS XRNS_KERNAL(Instruments)\
XRNS_KERNAL(Sample)\
XRNS_KERNAL(Samples)\
XRNS_KERNAL(PhraseGenerator)\
XRNS_KERNAL(ModulationSet)\
XRNS_KERNAL(SliceMarkers)\
XRNS_KERNAL(GlobalProperties)\
XRNS_KERNAL(Instruments)\
XRNS_KERNAL(Patterns)\
XRNS_KERNAL(Automations)\
XRNS_KERNAL(Envelopes)\
XRNS_KERNAL(Envelope)\
XRNS_KERNAL(SequenceEntries)\
XRNS_KERNAL(FilterDevices)\
XRNS_KERNAL(Tracks)

#define XRNS_TRACK_TAGS XRNS_KERNAL(SequencerSendTrack)\
XRNS_KERNAL(SequencerMasterTrack)\
XRNS_KERNAL(SequencerGroupTrack)\
XRNS_KERNAL(TrackMixerDevice)\
XRNS_KERNAL(MasterTrackMixerDevice)\
XRNS_KERNAL(SequencerMasterTrackDevice)\
XRNS_KERNAL(SequencerTrackDevice)\
XRNS_KERNAL(GroupTrackMixerDevice)\
XRNS_KERNAL(Volume)\
XRNS_KERNAL(PostVolume)\
XRNS_KERNAL(Surround)\
XRNS_KERNAL(Panning)\
XRNS_KERNAL(PostPanning)\
XRNS_KERNAL(FilterDevices)\
XRNS_KERNAL(AudioPluginDevice)\
XRNS_KERNAL(IsActive)\
XRNS_KERNAL(Parameters)\
XRNS_KERNAL(Parameter)

#define XRNS_INSTRUMENT_TAGS XRNS_KERNAL(Instruments)\
XRNS_KERNAL(Instrument)\
XRNS_KERNAL(Samples)\
XRNS_KERNAL(Sample)\
XRNS_KERNAL(PluginProperties)\
XRNS_KERNAL(SampleSplitMap)\
XRNS_KERNAL(SampleEnvelopes)\
XRNS_KERNAL(NoteOnMapping)\
XRNS_KERNAL(ModulationSets)\
XRNS_KERNAL(PluginGenerator)\
XRNS_KERNAL(GlobalProperties)\
XRNS_KERNAL(Mapping)\
XRNS_KERNAL(SampleEnvelopeModulationDevice)\
XRNS_KERNAL(ModulationSet)\
XRNS_KERNAL(SliceMarkers)\
XRNS_KERNAL(SliceMarker)\
XRNS_KERNAL(PhraseGenerator)\
XRNS_KERNAL(Automations)\
XRNS_KERNAL(Envelopes)\
XRNS_KERNAL(Envelope)

#define XRNS_TOPLEVEL_TAGS XRNS_KERNAL(Instruments)\
XRNS_KERNAL(Patterns)\
XRNS_KERNAL(Pattern)\
XRNS_KERNAL(PatternTrack)\
XRNS_KERNAL(PatternMasterTrack)\
XRNS_KERNAL(PatternGroupTrack)\
XRNS_KERNAL(AliasPatternIndex)\
XRNS_KERNAL(Tracks)\
XRNS_KERNAL(Patterns)\
XRNS_KERNAL(SequencerGroupTrack)\
XRNS_KERNAL(SequencerTrack)\
XRNS_KERNAL(SequencerMasterTrack)\
XRNS_KERNAL(PatternSequence)\
XRNS_KERNAL(SequenceEntries)\
XRNS_KERNAL(SequenceEntry)\
XRNS_KERNAL(Automations)\
XRNS_KERNAL(Envelopes)\
XRNS_KERNAL(Envelope)\
XRNS_KERNAL(MutedTracks)

#define XRNS_KERNAL(_x) char _x;
typedef struct
{
    XRNS_TAGS
} xrns_tag_set;
#undef XRNS_KERNAL 

#define XRNS_KERNAL(_x) if (xmltagmatch(Tag, #_x))\
{\
    t->_x = BeginOtherwiseEnd;\
    return;\
}

void UpdateXMLCountingTags(xrns_tag_set *t, char *Tag, int BeginOtherwiseEnd)
{
    XRNS_SUBSET_TAGS
}

void UpdateXMLTrackTags(xrns_tag_set *t, char *Tag, int BeginOtherwiseEnd)
{
    XRNS_TRACK_TAGS
}

void UpdateXMLTopLevelTags(xrns_tag_set *t, char *Tag, int BeginOtherwiseEnd)
{
    XRNS_TOPLEVEL_TAGS
}

void UpdateXMLInstrumentTags(xrns_tag_set *t, char *Tag, int BeginOtherwiseEnd)
{
    XRNS_INSTRUMENT_TAGS
}

#undef XRNS_KERNAL

#define GZEROED(_type, _name) _type _name; memset(&_name, 0, sizeof(_type));

/* galloc for persistent allocations
 */
typedef struct
{
    char   *BaseAddress;
    char   *CurrentAddress;
    /* for debugging only */
    size_t  MaximumSizeBytes;
} galloc_ctx;

void *galloc(galloc_ctx *g, size_t Bytes)
{
    if (g->CurrentAddress - g->BaseAddress > g->MaximumSizeBytes)
        return g->BaseAddress;

    char *OutPtr = g->CurrentAddress;
    g->CurrentAddress += Bytes;
    return OutPtr;
}

void *galloc_aligned(galloc_ctx *g, size_t Bytes, int AlignmentBytes)
{
    char *OutPtr;
    while ((unsigned long long) g->CurrentAddress % AlignmentBytes)
    {
        g->CurrentAddress++;
    }

    OutPtr = g->CurrentAddress;
    g->CurrentAddress += Bytes;
    
    return OutPtr;
}

void *GallocStringFromXML(galloc_ctx *g, xml_res *r)
{
    unsigned int Length = XMLTagLength(r->value);
    void *Dest = galloc(g, Length + 1);
    CopyBytesAppendingNULL(Dest, r->value, Length);
    return Dest;
}

void XRNSGetCounts(char *xml, size_t xml_length, xrns_file_counts *Counts)
{
    TracyCZoneN(ctx, "XRNS Get Counts", 1);

    gambit_xml x;
    gambit_xml_init(&x, xml);

    GZEROED(xrns_tag_set, t);
    GZEROED(xml_res, r);

    int bTracksCounted = 0;
    int bCountedModulationDevice = 0;

    int NumTrackEnvelopes = 0;

    unsigned int xx = 0;

    do
    {
        r = gambit_xml_parse_one_char(&x);
        if (r.event_type == GAMBIT_XML_EVENT_ELEMENT_END)
        {
            if (t.Instruments && t.Samples && xmltagmatch(r.name, "Sample"))
            {
                Counts->NumSamplesPerInstrument[Counts->NumInstruments]++;
            }

            if (Counts->RenoiseVersion == 3)
            {
                if (t.Instruments && !t.PhraseGenerator && t.ModulationSet && !bCountedModulationDevice && xmltagmatch(r.name, "SampleEnvelopeModulationDevice"))
                {
                    Counts->NumModulationSetsPerInstrument[Counts->NumInstruments]++;
                    bCountedModulationDevice = 1;
                }
            }

            if (t.Instruments && t.SliceMarkers && !t.PhraseGenerator)
            {
                if (xmltagmatch(r.name, "SamplePosition"))
                {
                    Counts->NumSliceRegionsPerInstrument[Counts->NumInstruments]++;
                }
            }

            if (   (Counts->RenoiseVersion == 2 && (t.Instruments && !t.Samples && xmltagmatch(r.name, "NoteOnMapping")))
                || (Counts->RenoiseVersion == 3 && (t.Sample && xmltagmatch(r.name, "Mapping") && !t.GlobalProperties)))
            {
                Counts->NumSampleSplitMapsPerInstrument[Counts->NumInstruments]++;
            }

            if (t.Instruments && xmltagmatch(r.name, "Instrument") && !t.PhraseGenerator)
            {
                Counts->NumInstruments++;
            }

            if (t.Patterns && xmltagmatch(r.name, "Pattern"))
            {
                Counts->NumPatterns++;
                bTracksCounted = 1;
            }

            if (t.Automations && t.Envelopes)
            {
                if (xmltagmatch(r.name, "Envelope") && !t.Envelope)
                {
                    NumTrackEnvelopes++;
                }
            }

            if (t.SequenceEntries && xmltagmatch(r.name, "SequenceEntry"))
            {
                Counts->PatternSequenceLength++;
            }

            if (t.FilterDevices && xmltagmatch(r.name, "AudioPluginDevice") && !t.PhraseGenerator)
            {
                Counts->NumEffectUnitsPerTrack[Counts->NumTracks]++;
            }

            if (   !bTracksCounted
                && t.Tracks
                && (xmltagmatch(r.name, "SequencerTrack") || xmltagmatch(r.name, "SequencerGroupTrack") || xmltagmatch(r.name, "SequencerMasterTrack"))
                && !t.PhraseGenerator
                )
            {
                Counts->NumTracks++;
            }

            if (xmltagmatch(r.name, "PatternTrack") || xmltagmatch(r.name, "PatternGroupTrack") || xmltagmatch(r.name, "PatternMasterTrack"))
            {
                xrns_magic_write_int(&Counts->EnvelopesPerTrackPerPattern, NumTrackEnvelopes, xx);
                xx++;
                NumTrackEnvelopes = 0;
            }

            UpdateXMLCountingTags(&t, r.name, 0);
        }
        else if (r.event_type == GAMBIT_XML_EVENT_ELEMENT_START)
        {
            if (xmltagmatch(r.name, "ModulationSet"))
            {
                bCountedModulationDevice = 0;
            }

            if (xmltagmatch(r.name, "Lines"))
            {
                x.xml = strstr(x.xml, "</Lines>");
            }

            UpdateXMLCountingTags(&t, r.name, 1);
        }
        else if (r.event_type == GAMBIT_XML_EVENT_ATTR_VALUE)
        {
            if (MatchCharsToString(r.name, "doc_version"))
            {
                Counts->RenoiseVersion = 2;
                int Version = ParseIntegerFromXML(r.value);
                if (Version >= 54)
                {
                    Counts->RenoiseVersion = 3;
                }
            }
        }
    }
    while (r.event_type != GAMBIT_XML_EVENT_EOF);

    TracyCZoneEnd(ctx);
}

int ParseBoolStringFromXML(char *boolstr)
{
    if (MatchCharsToString(boolstr, "true"))
    {
        return 1;
    }
    else return 0;
}

xrns_envelope *GetModulationPointer(xrns_modulation_set *ModulationSet, int ModulationTarget)
{
    if (ModulationTarget == XRNS_MODULATION_TARGET_VOLUME)
        return &ModulationSet->Volume;
    else if (ModulationTarget == XRNS_MODULATION_TARGET_PANNING)
        return &ModulationSet->Panning;
    else if (ModulationTarget == XRNS_MODULATION_TARGET_PITCH)
        return &ModulationSet->Pitch;
    return 0;
}

void ParseEnvelope(xrns_envelope *Envelope, gambit_xml *x, galloc_ctx *g)
{
    if (!Envelope) return;

    TracyCZoneN(ctx, "Parse Envelope", 1);

    xml_res r;

    int bInPoints = 0;
    int bInDecay  = 0;

    do
    {
        r = gambit_xml_parse_one_char(x);

        if (r.event_type == GAMBIT_XML_EVENT_ELEMENT_END)
        {
            if (xmltagmatch(r.name, "SustainIsActive"))
            {   
                Envelope->bSustainOn = ParseBoolStringFromXML(r.value);
            }
            else if (xmltagmatch(r.name, "SustainPos"))
            {
                Envelope->SustainPosition = ParseIntegerFromXML(r.value);
            }
            else if (xmltagmatch(r.name, "LoopMode"))
            {
                if (MatchCharsToString(r.value, "Off"))
                    Envelope->LoopMode = XRNS_LOOP_MODE_OFF;
                else if (MatchCharsToString(r.value, "Forward"))
                    Envelope->LoopMode = XRNS_LOOP_MODE_FORWARD;
                else if (MatchCharsToString(r.value, "Backward"))
                    Envelope->LoopMode = XRNS_LOOP_MODE_BACKWARD;
                else if (MatchCharsToString(r.value, "PingPong"))
                    Envelope->LoopMode = XRNS_LOOP_MODE_PINGPONG;
            }
            else if (xmltagmatch(r.name, "LoopStart"))
            {
                Envelope->LoopStart = ParseIntegerFromXML(r.value);
            }
            else if (xmltagmatch(r.name, "LoopEnd"))
            {
                Envelope->LoopEnd = ParseIntegerFromXML(r.value);
            }
            else if (xmltagmatch(r.name, "PlayMode"))
            {
                if (MatchCharsToString(r.value, "Points"))
                    Envelope->CurveType = XRNS_CURVE_TYPE_POINTS;
                else if (MatchCharsToString(r.value, "Lines"))
                    Envelope->CurveType = XRNS_CURVE_TYPE_LINES;
                else if (MatchCharsToString(r.value, "Curves"))
                    Envelope->CurveType = XRNS_CURVE_TYPE_CURVES;
            }
            else if (xmltagmatch(r.name, "Length"))
            {
                Envelope->Duration = ParseIntegerFromXML(r.value);
            }
            else if (bInDecay && xmltagmatch(r.name, "Value"))
            {
                Envelope->ReleaseValue = ParseFloatFromXML(r.value);
            }
            else if (xmltagmatch(r.name, "TempoSynced"))
            {
                if (ParseBoolStringFromXML(r.value))
                {
                    Envelope->Units = XRNS_ENVELOPE_UNITS_BEATS;
                }
                else
                {
                    Envelope->Units = XRNS_ENVELOPE_UNITS_MS;
                }
            }
            else if (bInPoints)
            {
                TracyCZoneN(ctx2, "Parsing Points", 1);
                if (xmltagmatch(r.name, "Point"))
                {
                    if (Envelope->NumPoints == 0)
                    {
                        Envelope->Points = galloc(g, sizeof(xrns_point));
                    }
                    else
                    {
                        galloc(g, sizeof(xrns_point));
                    }

                    Envelope->NumPoints++;
                    xrns_point *Point = &Envelope->Points[Envelope->NumPoints - 1];
                    ParsePointFromTriple(Point, r.value);
                }
                TracyCZoneEnd(ctx2);
            }
            else if (xmltagmatch(r.name, "Polarity"))
            {
                if (MatchCharsToString(r.value, "Unipolar"))
                {
                    Envelope->Polarity = XRNS_UNIPOLAR;
                }
                else
                {
                    Envelope->Polarity = XRNS_BIPOLAR;
                }
            }
            else if (xmltagmatch(r.name, "DeviceIndex"))
            {
                Envelope->DeviceIndex = ParseIntegerFromXML(r.value);
            }
            else if (xmltagmatch(r.name, "ParameterIndex"))
            {
                Envelope->ParameterIndex = ParseIntegerFromXML(r.value);
            }

            if (xmltagmatch(r.name, "SampleEnvelopeModulationDevice"))
                break;

            if (xmltagmatch(r.name, "Envelope"))
                break;

            if (xmltagmatch(r.name, "Points"))
                bInPoints = 0;
            if (xmltagmatch(r.name, "Decay"))
                bInDecay = 0;
        }
        else if (r.event_type == GAMBIT_XML_EVENT_ELEMENT_START)
        {
            if (xmltagmatch(r.name, "Points"))
                bInPoints = 1;
            if (xmltagmatch(r.name, "Decay"))
                bInDecay = 1;
        }
    } while (r.event_type != GAMBIT_XML_EVENT_EOF);

    TracyCZoneEnd(ctx);
}

void ParseInstruments
    (galloc_ctx    *g
    ,xrns_document *xdoc
    ,int            PatternIdx
    ,int            TrackIdx
    ,gambit_xml    *x
    ,xrns_tag_set  *t
    )
{
    TracyCZoneN(ctx, "Parse Instruments", 1);

    int InstrumentIdx = 0;
    int SampleIdx = 0;
    int SampleSplitIdx = 0;
    int ModulationSetIdx = 0;
    int ModulationTarget = 0;
    int SliceRegionIdx = 0;

    int bParsingTrackEnvelope = 0;

    xml_res r;

    xrns_envelope *Envelope = NULL;

    do
    {
        r = gambit_xml_parse_one_char(x);

        if (r.event_type == GAMBIT_XML_EVENT_ELEMENT_END)
        {
            if (!xdoc->Instruments[InstrumentIdx].Name && !t->Samples && t->Instrument && xmltagmatch(r.name, "Name") && !t->PluginProperties && !t->GlobalProperties && !t->ModulationSets && !t->PluginGenerator)
            {
                xdoc->Instruments[InstrumentIdx].Name = GallocStringFromXML(g, &r);
            } 
            else if (t->Samples && t->Sample && !t->SampleEnvelopes && !t->PluginProperties)
            {
                xrns_sample *Sample = &xdoc->Instruments[InstrumentIdx].Samples[SampleIdx];
                if (xmltagmatch(r.name, "Name"))
                {
                    Sample->Name = GallocStringFromXML(g, &r);

                    xrns_instrument *Instrument = &xdoc->Instruments[InstrumentIdx];

                    if (Instrument->bIsSliced && SampleIdx)
                    {
                        Sample->SampleStart = Instrument->SliceRegions[SampleIdx - 1];
                        Sample->bIsAlisedSample = 1;
                    }
                    else
                    {
                        Sample->SampleStart = 0;
                        Sample->bIsAlisedSample = 0;
                    }
                }
                else if (xmltagmatch(r.name, "Volume"))
                {
                    Sample->Volume = ParseFloatFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "Panning"))
                {
                    Sample->Panning = ParseFloatFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "Transpose"))
                {
                    Sample->Transpose = ParseIntegerFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "Finetune"))
                {
                    Sample->Finetune = ParseIntegerFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "BeatSyncIsActive"))
                {
                    Sample->BeatSyncIsActive = ParseBoolStringFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "BeatSyncLines"))
                {
                    Sample->BeatSyncLines = ParseIntegerFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "InterpolationMode"))
                {
                    if (MatchCharsToString(r.value, "None"))
                        Sample->InterpolationMode = XRNS_INTERPOLATION_NONE;
                    else if (MatchCharsToString(r.value, "Linear"))
                        Sample->InterpolationMode = XRNS_INTERPOLATION_LINEAR;
                    else
                        Sample->InterpolationMode = XRNS_INTERPOLATION_CUBIC;
                }
                else if (xmltagmatch(r.name, "LoopMode"))
                {
                    if (MatchCharsToString(r.value, "Off"))
                        Sample->LoopMode = XRNS_LOOP_MODE_OFF;
                    else if (MatchCharsToString(r.value, "Forward"))
                        Sample->LoopMode = XRNS_LOOP_MODE_FORWARD;
                    else if (MatchCharsToString(r.value, "Backward"))
                        Sample->LoopMode = XRNS_LOOP_MODE_BACKWARD;
                    else if (MatchCharsToString(r.value, "PingPong"))
                        Sample->LoopMode = XRNS_LOOP_MODE_PINGPONG;
                }
                else if (xmltagmatch(r.name, "LoopStart"))
                {
                    Sample->LoopStart = ParseIntegerFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "LoopEnd"))
                {
                    Sample->LoopEnd = ParseIntegerFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "LoopRelease"))
                {
                    Sample->LoopRelease = ParseBoolStringFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "ModulationSetIndex"))
                {
                    Sample->ModulationSetIndex = ParseIntegerFromXML(r.value);
                    if (Sample->ModulationSetIndex != -1)
                        Sample->ModulationSetIndex = 0;
                }
                else if (xmltagmatch(r.name, "NewNoteAction"))
                {
                    if (MatchCharsToString(r.value, "Cut"))
                        Sample->NewNoteAction = XRNS_NNA_CUT;
                    else if (MatchCharsToString(r.value, "NoteOff"))
                        Sample->NewNoteAction = XRNS_NNA_NOTEOFF;
                    else if (MatchCharsToString(r.value, "Continue") || MatchCharsToString(r.value, "None"))
                        Sample->NewNoteAction = XRNS_NNA_CONTINUE;
                }
                else if (t->SliceMarkers && t->SliceMarker && xmltagmatch(r.name, "SamplePosition"))
                {
                    xrns_instrument *Instrument = &xdoc->Instruments[InstrumentIdx];
                    Instrument->bIsSliced = 1;

                    Instrument->SliceRegions[SliceRegionIdx] = ParseIntegerFromXML(r.value);
                    SliceRegionIdx++;
                }
            }
            if (  (xdoc->RenoiseVersion == 2 && (t->SampleSplitMap && t->NoteOnMapping && !t->SampleEnvelopes && !t->PluginProperties))
                ||(xdoc->RenoiseVersion == 3 && (t->Sample && t->Mapping && !t->GlobalProperties)))
            {
                xrns_ssm *SampleSM = &xdoc->Instruments[InstrumentIdx].SampleSplitMaps[SampleSplitIdx];

                if (xmltagmatch(r.name, "SampleIndex"))
                {
                    SampleSM->SampleIndex = ParseIntegerFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "UseEnvelopes"))
                {
                    SampleSM->UseEnvelopes = ParseBoolStringFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "MapVelocityToVolume"))
                {
                    SampleSM->MapVelocityToVolume = ParseBoolStringFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "BaseNote"))
                {
                    SampleSM->BaseNote = ParseIntegerFromXML(r.value);
                    if (xdoc->RenoiseVersion == 3)
                        SampleSM->SampleIndex = SampleIdx;
                }
                else if (xmltagmatch(r.name, "NoteStart"))
                {
                    SampleSM->NoteStart = ParseIntegerFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "NoteEnd"))
                {
                    SampleSM->NoteEnd = ParseIntegerFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "VelocityStart"))
                {
                    SampleSM->VelocityStart = ParseIntegerFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "VelocityEnd"))
                {
                    SampleSM->VelocityEnd = ParseIntegerFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "Layer"))
                {
                    SampleSM->bIsNoteOn = MatchCharsToString(r.value, "Note-On Layer");
                }
            }
            if (xdoc->RenoiseVersion == 3)
            {
                if (xmltagmatch(r.name, "ModulationSet"))
                {
                    ModulationSetIdx++;
                }

                if (xdoc->Instruments[InstrumentIdx].NumModulationSets && xmltagmatch(r.name, "PitchModulationRange"))
                {
                    xrns_modulation_set *ModulationSet = &xdoc->Instruments[InstrumentIdx].ModulationSets[ModulationSetIdx];
                    ModulationSet->PitchModulationRange = ParseIntegerFromXML(r.value);
                }

                if (t->SampleEnvelopeModulationDevice)
                {
                    bParsingTrackEnvelope = 0;
                    xrns_modulation_set *ModulationSet = &xdoc->Instruments[InstrumentIdx].ModulationSets[ModulationSetIdx];

                    if (xmltagmatch(r.name, "Target"))
                    {
                        if (MatchCharsToString(r.value, "Volume"))
                        {
                            ModulationTarget = XRNS_MODULATION_TARGET_VOLUME;
                            ModulationSet->bVolumeEnvelopePresent = 1;
                        }
                        else if (MatchCharsToString(r.value, "Panning"))
                        {
                            ModulationTarget = XRNS_MODULATION_TARGET_PANNING;
                            ModulationSet->bPanningEnvelopePresent = 1;
                        }
                        else if (MatchCharsToString(r.value, "Pitch"))
                        {
                            ModulationTarget = XRNS_MODULATION_TARGET_PITCH;
                            ModulationSet->bPitchEnvelopePresent = 1;
                        }
                        else
                        {
                            /* Drive, cutoff, etc... */
                            ModulationTarget = 0xDEADBEEF;
                        }

                        Envelope = GetModulationPointer(ModulationSet, ModulationTarget);
                    }
                    else if (Envelope)
                    {
                        ParseEnvelope(Envelope, x, g);
                        t->SampleEnvelopeModulationDevice = 0;
                        Envelope = NULL;
                    }
                }
            }
            
            if (xmltagmatch(r.name, "Instrument") && !t->PhraseGenerator)
            {
                InstrumentIdx++;
                SliceRegionIdx = 0;
                SampleIdx = 0;
                SampleSplitIdx = 0;
                ModulationSetIdx = 0;
            }
            else if (xmltagmatch(r.name, "Sample"))
            {
                SampleIdx++;
            }
            else if (  (xdoc->RenoiseVersion == 2 && xmltagmatch(r.name, "NoteOnMapping"))
                    || (xdoc->RenoiseVersion == 3 && xmltagmatch(r.name, "Mapping") && !t->GlobalProperties && !t->PhraseGenerator)
                    )
            {
                SampleSplitIdx++;
            }

            if (xmltagmatch(r.name, "Instruments"))
            {
                UpdateXMLInstrumentTags(t, r.name, 0);
                break;    
            }

            UpdateXMLInstrumentTags(t, r.name, 0);
        }
        else if (r.event_type == GAMBIT_XML_EVENT_ELEMENT_START)
        {
            UpdateXMLInstrumentTags(t, r.name, 1);
        }
    
    } while (r.event_type != GAMBIT_XML_EVENT_EOF);

    TracyCZoneEnd(ctx);
}

void ParseLines
    (galloc_ctx    *g
    ,xrns_document *xdoc
    ,int            PatternIdx
    ,int            TrackIdx
    ,gambit_xml    *x
    ,xrns_tag_set  *t
    )
{
    TracyCZoneN(ctx, "Parse Lines", 1);

    int ColumnCounter    = 0;
    int CurrentLineIndex = 0;
    int bInNoteColumn    = 0;
    int bInEffectColumn  = 0;

    xrns_track *Track = &xdoc->PatternPool[PatternIdx].Tracks[TrackIdx];

    xml_res r;

    do
    {
        r = gambit_xml_parse_one_char(x);

        int bIsCol = 0;
        int bIsNoteCol = 0;
        int bIsEffectCol = 0;

        if (r.event_type > GAMBIT_XML_EVENT_NOTHING && r.event_type <= GAMBIT_XML_EVENT_ATTR_VALUE)
        {
            if (!(bIsNoteCol = xmltagmatch(r.name, "NoteColumn")))
            {
                bIsEffectCol = xmltagmatch(r.name, "EffectColumn");
            }
            bIsCol = (((TrackIdx != xdoc->NumTracks - 1) && bIsNoteCol) || bIsEffectCol);
        }

        switch (r.event_type)
        {
            case GAMBIT_XML_EVENT_ELEMENT_SELFCLOSING:
            {
                if (bIsCol) ColumnCounter++;
                break;
            }
            case GAMBIT_XML_EVENT_ELEMENT_START:
            {
                if (bIsCol)
                {
                    if (Track->NumNotes == 0)
                    {
                        Track->Notes = galloc(g, sizeof(xrns_note));
                    }
                    else
                    {
                        galloc(g, sizeof(xrns_note));
                    }

                    Track->NumNotes++;

                    xrns_note *NewNote = &Track->Notes[Track->NumNotes - 1];

                    InitNote(NewNote);
                    NewNote->Column = ColumnCounter++;
                    NewNote->Line = CurrentLineIndex;

                    if (bIsNoteCol)
                    {
                        NewNote->Type = XRNS_NOTE_REAL;
                    }
                    else
                    {
                        NewNote->Type = XRNS_NOTE_EFFECT;
                    }
                }

                if (bIsNoteCol)
                    bInNoteColumn = 1;

                if (bIsEffectCol)
                    bInEffectColumn = 1;

                break;
            }
            case GAMBIT_XML_EVENT_ATTR_VALUE:
            {
                if (MatchCharsToString(r.name, "index"))
                {
                    CurrentLineIndex = ParseIntegerFromXML(r.value);
                    ColumnCounter = 0;
                }
                break;
            }
            case GAMBIT_XML_EVENT_ELEMENT_END:
            {
                if ((t->PatternTrack || t->PatternMasterTrack || t->PatternGroupTrack) && bInNoteColumn)
                {
                    if (Track->NumNotes > 0)
                    {
                        xrns_note *Note = &Track->Notes[Track->NumNotes - 1];
                        if (xmltagmatch(r.name, "Note"))
                        {
                            Note->Note = ParseXRNSNoteFromXML(r.value);
                        }
                        else if (xmltagmatch(r.name, "Instrument"))
                        {
                            Note->Instrument = Parse2DigitHexOrDotsFromXML(r.value);
                        }
                        else if (xmltagmatch(r.name, "Volume"))
                        {
                            Note->Volume = Parse2DigitEffectColumnOrDotsFromXML(r.value);
                            Note->VolumeEffect[0] = r.value[0];
                            Note->VolumeEffect[1] = r.value[1];
                        }
                        else if (xmltagmatch(r.name, "Delay"))
                        {
                            Note->Delay = Parse2DigitHexOrDotsFromXML(r.value);
                        }
                        else if (xmltagmatch(r.name, "Panning"))
                        {
                            Note->Panning = Parse2DigitEffectColumnOrDotsFromXML(r.value);
                            Note->PanningEffect[0] = r.value[0];
                            Note->PanningEffect[1] = r.value[1];
                        }
                        else if (xmltagmatch(r.name, "EffectNumber"))
                        {         
                            Note->EffectTypeIdx = EffectTypeIdxFromEffectType(r.value);
                        }
                        else if (xmltagmatch(r.name, "EffectValue"))
                        {
                            Note->EffectValue = Parse2DigitHexFromXML(r.value);
                        }
                    }
                }
                else if (bInEffectColumn)
                {
                    if (Track->NumNotes > 0)
                    {
                        xrns_note *Note = &Track->Notes[Track->NumNotes - 1];
                        if (xmltagmatch(r.name, "Value"))
                        {
                            Note->EffectValue = Parse2DigitHexFromXML(r.value);
                        } else if (xmltagmatch(r.name, "Number"))
                        {
                            Note->EffectTypeIdx = EffectTypeIdxFromEffectType(r.value);    
                            Note->EffectTypeC[0] = r.value[0];
                            Note->EffectTypeC[1] = r.value[1];
                        }
                    }
                }

                if (bIsNoteCol)
                    bInNoteColumn = 0;

                if (bIsEffectCol)
                    bInEffectColumn = 0;

                if (xmltagmatch(r.name, "Lines"))
                {
                    TracyCZoneEnd(ctx);
                    return;
                }
                break;
            }
            default: {}
        }

    } while (r.event_type != GAMBIT_XML_EVENT_EOF);

    TracyCZoneEnd(ctx);
}

void ParseTracks
    (galloc_ctx    *g
    ,xrns_document *xdoc
    ,gambit_xml    *x
    ,xrns_tag_set  *t
    )
{
    TracyCZoneN(ctx, "Parse Tracks", 1);

    int SequenceIdx = 0;
    int EffectNumber = 0;
    int ParameterIdx = 0;
    int TrackStackCounts[16] = {0};

    xml_res r;

    do
    {
        r = gambit_xml_parse_one_char(x);

        if (r.event_type == GAMBIT_XML_EVENT_ELEMENT_END)
        {
            /* This is where the info about tracks is stored, their name, colour and 
             * grouping.
             */
            if (t->SequencerSendTrack)
            {
                UpdateXMLTrackTags(t, r.name, 0);
                continue;
            }

            if (xmltagmatch(r.name, "Name"))
            {
                xdoc->Tracks[SequenceIdx].Name = GallocStringFromXML(g, &r);
                EffectNumber = 0;
                ParameterIdx = 0;
            }
            else if (xmltagmatch(r.name, "GroupNestingLevel"))
            {
                int d;
                int depth = ParseIntegerFromXML(r.value);

                for (d = depth - 1; d >= 0; d--)
                    TrackStackCounts[d]++;

                if (t->SequencerMasterTrack)
                {
                    /* The master track magically wraps all the tracks in the song!
                     */
                    xdoc->Tracks[SequenceIdx].bIsGroup = 1;
                    xdoc->Tracks[SequenceIdx].Depth    = 0;
                    xdoc->Tracks[SequenceIdx].WrapsNPreviousTracks = SequenceIdx;
                }
                else
                {
                    xdoc->Tracks[SequenceIdx].bIsGroup = (t->SequencerGroupTrack);
                    xdoc->Tracks[SequenceIdx].Depth    = depth;
                    xdoc->Tracks[SequenceIdx].WrapsNPreviousTracks = TrackStackCounts[depth];
                }

                TrackStackCounts[depth] = 0;
            }
            else if (xmltagmatch(r.name, "NumberOfVisibleNoteColumns"))
            {
                xdoc->Tracks[SequenceIdx].NumColumns = ParseIntegerFromXML(r.value);
            }
            else if (xmltagmatch(r.name, "NumberOfVisibleEffectColumns"))
            {
                xdoc->Tracks[SequenceIdx].NumEffectColumns = ParseIntegerFromXML(r.value);
            }
            else if (t->TrackMixerDevice || t->MasterTrackMixerDevice || t->SequencerTrackDevice || t->SequencerMasterTrackDevice || t->GroupTrackMixerDevice)
            {
                if (t->Volume)
                {
                    if (xmltagmatch(r.name, "Value"))
                    {
                        xdoc->Tracks[SequenceIdx].InitialPreVolume = ParseFloatFromXML(r.value);
                        /* This is the "pre volume" */
                    }
                }
                else if (t->PostVolume)
                {
                    if (xmltagmatch(r.name, "Value"))
                    {
                        /* this is the post volume */
                        xdoc->Tracks[SequenceIdx].InitialPostVolume = ParseFloatFromXML(r.value);
                    }
                }
                else if (t->Surround)
                {
                    if (xmltagmatch(r.name, "Value"))
                    {
                        xdoc->Tracks[SequenceIdx].InitialWidth = ParseFloatFromXML(r.value);
                    }
                }
                else if (t->Panning)
                {
                    if (xmltagmatch(r.name, "Value"))
                    {
                        xdoc->Tracks[SequenceIdx].InitialPanning = ParseFloatFromXML(r.value);
                    }
                }
                else if (t->PostPanning)
                {
                    if (xmltagmatch(r.name, "Value"))
                    {
                        xdoc->Tracks[SequenceIdx].PostPanning = ParseFloatFromXML(r.value);
                    }
                }                    
            }
            else if (t->FilterDevices)
            {
                if (t->AudioPluginDevice)
                {
                    dsp_effect_desc *EffectDesc = &xdoc->Tracks[SequenceIdx].DSPEffectDescs[EffectNumber - 1];

                    if (t->IsActive && xmltagmatch(r.name, "Value"))
                    {
                        EffectDesc->Enabled = (ParseFloatFromXML(r.value) != 0.0f);
                    }

                    if (xmltagmatch(r.name, "PluginIdentifier"))
                    {
                        if (MatchCharsToString(r.value, "WestVerb"))
                        {
                            EffectDesc->Type = XRNS_EFFECT_REVERB;
                        }
                        else if (MatchCharsToString(r.value, "BasicFilter"))
                        {
                            EffectDesc->Type = XRNS_EFFECT_FILTER;
                        }

                        ParameterIdx = 0;
                    }

                    if (t->Parameters && t->Parameter)
                    {
                        if (xmltagmatch(r.name, "Value"))
                        {
                            EffectDesc->Parameters[ParameterIdx] = ParseFloatFromXML(r.value);
                            ParameterIdx++;
                            EffectDesc->NumParameters++;
                        }
                    }
                }
            }

            if (xmltagmatch(r.name, "SequencerTrack") || xmltagmatch(r.name, "SequencerGroupTrack"))
                SequenceIdx++;

            if (xmltagmatch(r.name, "Tracks"))
            {
                UpdateXMLTrackTags(t, r.name, 0);
                break;
            }

            UpdateXMLTrackTags(t, r.name, 0);

        }
        else if (r.event_type == GAMBIT_XML_EVENT_ELEMENT_START || r.event_type == GAMBIT_XML_EVENT_ELEMENT_SELFCLOSING)
        {
            if (!t->AudioPluginDevice && xmltagmatch(r.name, "AudioPluginDevice"))
            {
                EffectNumber++;
            }

            UpdateXMLTrackTags(t, r.name, (r.event_type == GAMBIT_XML_EVENT_ELEMENT_START));
        }

    } while (r.event_type != GAMBIT_XML_EVENT_EOF);

    TracyCZoneEnd(ctx);
}

typedef struct
{
    galloc_ctx    *g;
    char          *xml;
    size_t         xml_length;
    xrns_document *xdoc;
} xrns_xml_parse_desc;

int populateInstrumentsAndNotes(xrns_xml_parse_desc *ParseDesc)
{
    int i, k;

    galloc_ctx    *g          = ParseDesc->g;
    char          *xml        = ParseDesc->xml;
    size_t         xml_length = ParseDesc->xml_length;
    xrns_document *xdoc       = ParseDesc->xdoc;

    char s = xml[xml_length];

    xml[xml_length] = '\0';

    xrns_file_counts Counts;
    memset(&Counts, 0, sizeof(xrns_file_counts));
    xrns_growing_buffer_init(&Counts.EnvelopesPerTrackPerPattern, Kilobytes(8));

    XRNSGetCounts(xml, xml_length, &Counts);

    xdoc->RenoiseVersion        = Counts.RenoiseVersion;
    xdoc->NumInstruments        = Counts.NumInstruments;
    xdoc->NumTracks             = Counts.NumTracks;
    xdoc->NumPatterns           = Counts.NumPatterns;
    xdoc->PatternSequenceLength = Counts.PatternSequenceLength;

    xdoc->Instruments     = galloc(g, sizeof(xrns_instrument) * xdoc->NumInstruments);
    xdoc->PatternPool     = galloc(g, sizeof(xrns_pattern) * xdoc->NumPatterns);
    xdoc->PatternSequence = galloc(g, sizeof(xrns_pattern_sequence_entry) * xdoc->PatternSequenceLength);

    for (i = 0; i < xdoc->NumInstruments; i++)
    {
        xrns_instrument *Instrument    = &xdoc->Instruments[i];
        Instrument->NumSamples         = Counts.NumSamplesPerInstrument[i];
        Instrument->Samples            = galloc(g, sizeof(xrns_sample) * Counts.NumSamplesPerInstrument[i]);
        Instrument->NumSampleSplitMaps = Counts.NumSampleSplitMapsPerInstrument[i];
        Instrument->SampleSplitMaps    = galloc(g, sizeof(xrns_ssm) * Counts.NumSampleSplitMapsPerInstrument[i]);
        Instrument->NumModulationSets  = Counts.NumModulationSetsPerInstrument[i];
        Instrument->ModulationSets     = galloc(g, sizeof(xrns_modulation_set) * Counts.NumModulationSetsPerInstrument[i]);

        Instrument->NumSliceRegions    = Counts.NumSliceRegionsPerInstrument[i];
        Instrument->SliceRegions       = galloc(g, sizeof(unsigned int) * Counts.NumSliceRegionsPerInstrument[i]);
    }

    xdoc->Tracks = galloc(g, sizeof(xrns_track_desc) * xdoc->NumTracks);

    for (i = 0; i < xdoc->NumTracks; i++)
    {
        xdoc->Tracks[i].DSPEffectDescs    = galloc(g, sizeof(dsp_effect_desc) * Counts.NumEffectUnitsPerTrack[i]);
        xdoc->Tracks[i].NumDSPEffectUnits = Counts.NumEffectUnitsPerTrack[i];
    }

    for (i = 0; i < xdoc->NumPatterns; i++)
    {
        xrns_pattern *Pattern = &xdoc->PatternPool[i];
        Pattern->Tracks       = galloc(g, sizeof(xrns_track) * xdoc->NumTracks);
    }

    for (i = 0; i < xdoc->NumPatterns; i++)
    {
        xrns_pattern *Pattern = &xdoc->PatternPool[i];
        for (int xx = 0; xx < xdoc->NumTracks; xx++)
        {
            Pattern->Tracks[xx].NumEnvelopes = ((int*)Counts.EnvelopesPerTrackPerPattern.Memory)[i*xdoc->NumTracks + xx];
            Pattern->Tracks[xx].Envelopes    = galloc(g, sizeof(xrns_envelope) * Pattern->Tracks[xx].NumEnvelopes);
        }
    }

    gambit_xml x;
    gambit_xml_init(&x, xml);

    GZEROED(xrns_tag_set, t);
    GZEROED(xml_res, r);

    int PatternIdx = 0;
    int SequenceIdx = 0; /* these count as tracks */
    int TrackIdx = 0;
    int NoteIdx = 0;
    int EffectIdx = 0;
    int ColumnIdx = 0;
    int EffectColumnIdx = 0;
    int PatternSequenceIdx = 0;

    int EffectNumber = 0;
    int ParameterIdx = 0;

    int CurrentLineIndex = 0;
    int ColumnCounter = 0;
  
    int CurrentTrackEnvelope = 0;


    int bParsingTrackEnvelope = 0;

    float InitialBPMFromZTorZLCommand = -1.0f;

    xrns_envelope *TrackEnvelope = NULL;

    TracyCZoneN(ctx, "Top-Level Parse", 1);

    do
    {
        r = gambit_xml_parse_one_char(&x);
        if (r.event_type == GAMBIT_XML_EVENT_ELEMENT_END)
        {
            if (xmltagmatch(r.name, "SongName"))
            {
                xdoc->SongName = GallocStringFromXML(g, &r);
            }
            else if (xmltagmatch(r.name, "Artist"))
            {
                xdoc->Artist = GallocStringFromXML(g, &r);
            }
            else if (xmltagmatch(r.name, "BeatsPerMin"))
            {
                xdoc->BeatsPerMin = ParseFloatFromXML(r.value);
            }
            else if (xmltagmatch(r.name, "LinesPerBeat"))
            {
                xdoc->LinesPerBeat = ParseIntegerFromXML(r.value);
            }
            else if (xmltagmatch(r.name, "TicksPerLine"))
            {
                xdoc->TicksPerLine = ParseIntegerFromXML(r.value);
            }
            
            if (t.Instruments)
            {
                ParseInstruments(g, xdoc, PatternIdx, TrackIdx, &x, &t);
                continue;
            }

            if (t.Patterns)
            {
                xrns_track *Track = &xdoc->PatternPool[PatternIdx].Tracks[TrackIdx];
                if (xmltagmatch(r.name, "PatternTrack") || xmltagmatch(r.name, "PatternMasterTrack") || xmltagmatch(r.name, "PatternGroupTrack"))
                {
                    TrackIdx++;
                    ColumnIdx = 0;
                    EffectIdx = 0;
                    EffectColumnIdx = 0;
                    CurrentTrackEnvelope = 0;
                }
                else if (t.Pattern && xmltagmatch(r.name, "NumberOfLines"))
                {
                    xdoc->PatternPool[PatternIdx].NumberOfLines = ParseIntegerFromXML(r.value);
                }
                else if (t.Pattern && xmltagmatch(r.name, "Name"))
                {
                    xdoc->PatternPool[PatternIdx].Name = GallocStringFromXML(g, &r);
                }
                else if (xmltagmatch(r.name, "Pattern"))
                {
                    TrackIdx = 0;
                    PatternIdx++;
                    CurrentTrackEnvelope = 0;
                }

                if ((t.PatternTrack || t.PatternMasterTrack || t.PatternGroupTrack))
                {
                    if (t.AliasPatternIndex)
                    {
                        int Alias = ParseIntegerFromXML(r.value);
                        if (Alias == -1)
                        {
                            Track->bIsAlias = 0;
                        }
                        else
                        {
                            Track->bIsAlias = 1;
                            Track->AliasIdx = Alias;
                        }
                    }
                }
            }
            else if (t.Tracks && !t.Patterns && (t.SequencerGroupTrack || t.SequencerTrack || t.SequencerMasterTrack))
            {
                // DERP!
            }
            else if (t.PatternSequence && t.SequenceEntries && t.SequenceEntry)
            {
                xrns_pattern_sequence_entry *PatternSeq = &xdoc->PatternSequence[PatternSequenceIdx];

                if (xmltagmatch(r.name, "IsSectionStart"))
                {
                    PatternSeq->bIsSectionStart = ParseBoolStringFromXML(r.value);
                }
                else if (xmltagmatch(r.name, "SectionName"))
                {
                    PatternSeq->SectionName = GallocStringFromXML(g, &r);
                }
                else if (xmltagmatch(r.name, "Pattern"))
                {
                    PatternSeq->PatternIdx = ParseIntegerFromXML(r.value);
                }
                else if (t.MutedTracks)
                {
                    if (xmltagmatch(r.name, "MutedTrack"))
                    {
                        PatternSeq->MutedTracks[PatternSeq->NumMutedTracks - 1] = ParseIntegerFromXML(r.value); 
                    }
                }
            }

            if (xmltagmatch(r.name, "SequenceEntry"))
            {
                PatternSequenceIdx++;
            }

            UpdateXMLTopLevelTags(&t, r.name, 0);
        }

        if ((r.event_type == GAMBIT_XML_EVENT_ELEMENT_START) && xmltagmatch(r.name, "Tracks") && !t.Patterns)
        {
            ParseTracks(g, xdoc, &x, &t);
            continue;
        }

        if ((r.event_type == GAMBIT_XML_EVENT_ELEMENT_START) && xmltagmatch(r.name, "Lines"))
        {
            ParseLines(g, xdoc, PatternIdx, TrackIdx, &x, &t);
            continue;
        }
        
        if (r.event_type == GAMBIT_XML_EVENT_ELEMENT_START || r.event_type == GAMBIT_XML_EVENT_ELEMENT_SELFCLOSING)
        {
            xrns_track *Track = &xdoc->PatternPool[PatternIdx].Tracks[TrackIdx];
            xrns_pattern_sequence_entry *PatternSeq = &xdoc->PatternSequence[PatternSequenceIdx];

            if (t.Automations && t.Envelopes)
            {
                if (xmltagmatch(r.name, "Envelope") && !t.Envelope)
                {
                    CurrentTrackEnvelope++;
                    TrackEnvelope = &Track->Envelopes[CurrentTrackEnvelope - 1];
                    ParseEnvelope(TrackEnvelope, &x, g);
                }
            }
            else
            {
                bParsingTrackEnvelope = 0;
            }

            if (xmltagmatch(r.name, "MutedTrack"))
            {
                if (PatternSeq->NumMutedTracks == 0)
                {
                    PatternSeq->MutedTracks = galloc(g, sizeof(unsigned int));
                }
                else
                {
                    galloc(g, sizeof(unsigned int));
                }
                PatternSeq->NumMutedTracks++;
            }
            UpdateXMLTopLevelTags(&t, r.name, (r.event_type == GAMBIT_XML_EVENT_ELEMENT_START));
        }
        else if (r.event_type == GAMBIT_XML_EVENT_ATTR_VALUE)
        {
            if (MatchCharsToString(r.name, "doc_version"))
            {
                xdoc->RenoiseVersion = 2;
                int Version = ParseIntegerFromXML(r.value);

                if (Version < 37)
                {
                    /* We don't support XRNS files older than this. */
                    return 0;
                }

                if (Version >= 54)
                {
                    xdoc->RenoiseVersion = 3;
                }
            }
        }
    }
    while (r.event_type != GAMBIT_XML_EVENT_EOF);

    TracyCZoneEnd(ctx);

    int bFoundStartingZT = 0;
    int bFoundStartingZL = 0;
    int bFoundStartingZK = 0;
    float StartingZT = -1.0f;
    float StartingZL = -1.0f;
    float StartingZK = -1.0f;

    for (int PatternSeqIdx = 0; PatternSeqIdx < xdoc->PatternSequenceLength; PatternSeqIdx++)
    {
        unsigned int Pattern = xdoc->PatternSequence[PatternSeqIdx].PatternIdx;

        for (int TrackIdx = 0; TrackIdx < xdoc->NumTracks; TrackIdx++)
        {
            xrns_track *Track = &xdoc->PatternPool[Pattern].Tracks[TrackIdx];
            if (Track->bIsAlias && Track->AliasIdx < xdoc->NumPatterns)
            {
                Track = &xdoc->PatternPool[Track->AliasIdx].Tracks[TrackIdx];
            }

            for (int Note = 0; Note < Track->NumNotes; Note++)
            {
                xrns_note *Effect = &Track->Notes[Note];

                if (Effect->Note != XRNS_NOTE_EFFECT)
                    continue;

                if (!bFoundStartingZT && (Effect->EffectTypeIdx == EFFECT_ID_ZT))
                {
                    bFoundStartingZT = 1;
                    StartingZT = Effect->Volume;
                }

                if (!bFoundStartingZL && (Effect->EffectTypeIdx == EFFECT_ID_ZL))
                {
                    bFoundStartingZL = 1;
                    StartingZL = Effect->Volume;
                }

                if (!bFoundStartingZK && (Effect->EffectTypeIdx == EFFECT_ID_ZK))
                {
                    bFoundStartingZK = 1;
                    StartingZK = Effect->Volume;
                }
            }
        }
    }

    if (bFoundStartingZL)
        xdoc->LinesPerBeat = StartingZL;
    if (bFoundStartingZT)
        xdoc->BeatsPerMin = StartingZT;
    if (bFoundStartingZK)
        xdoc->TicksPerLine = StartingZK;

    xrns_growing_buffer_free(&Counts.EnvelopesPerTrackPerPattern);
    xml[xml_length] = s;

    return 1;

}

typedef struct
{
    gambit_zip_entry z;
    char             zipped_filename[2048];
    xrns_document   *xdoc;
} populate_instrument_desc;

xrns_sample *populateInstrumentSample(populate_instrument_desc *InstrumentDesc)
{
    TracyCZoneN(ctx, "Populate Instrument Sample", 1);

    gambit_zip_entry *z                = &InstrumentDesc->z;
    char             *zipped_filename  = InstrumentDesc->zipped_filename;
    xrns_document    *xdoc             = InstrumentDesc->xdoc;

    unsigned int InstrumentNumber = atoi(zipped_filename + strlen("SampleData/Instrument"));

    char *s = strstr(zipped_filename, ")/Sample");
    s += strlen(")/Sample");

    unsigned int SampleNumber = atoi(s);

    int16_t   *PCM;
    ma_uint64  FrameCountOut;
    ma_decoder_config Config = ma_decoder_config_init(ma_format_s16, 0, 0);

    ma_result ret = 
    ma_decode_memory
        (z->p_mem
        ,z->Header.CompressedSize
        ,&Config
        ,&FrameCountOut
        ,(void **) &PCM
        );

    if (ret != MA_SUCCESS)
    {
        if (ret == MA_INVALID_ARGS)
        {
            printf("MA_INVALID_ARGS\n");
        }

        if (ret == MA_TOO_BIG)
        {
            printf("MA_TOO_BIG\n");
        }

        if (ret == MA_OUT_OF_MEMORY)
        {
            printf("MA_OUT_OF_MEMORY\n");
        }
    }

    ma_convert_pcm_frames_format
        (PCM
        ,ma_format_s16
        ,PCM
        ,Config.format
        ,FrameCountOut
        ,Config.channels
        ,ma_dither_mode_none
        );

    xrns_sample *Sample = malloc(sizeof(xrns_sample));//&xdoc->Instruments[InstrumentNumber].Samples[SampleNumber];
    memset(Sample, 0, sizeof(xrns_sample));

    Sample->PCM           = PCM;
    Sample->SampleRateHz  = Config.sampleRate;
    Sample->NumChannels   = Config.channels;
    Sample->LengthSamples = FrameCountOut;
    Sample->InstrumentNumber = InstrumentNumber;
    Sample->SampleNumber  = SampleNumber;

    TracyCZoneEnd(ctx);

    return Sample;
}

int populateXRNSDocument(galloc_ctx *g, void *mem, size_t mem_sz, xrns_document *xdoc, pooled_threads_ctx *Workers)
{
    char c[2048];
    int i;

    TracyCZoneN(main_ctx, "Parse ZIP", 1);

    work_table *Decoding = CreateWorkTable(0);

    gambit_zip_parsing_state zs;
    gambit_start_parsing(&zs, mem, mem_sz);

    xrns_xml_parse_desc ParseDesc;

    do
    {
        gambit_zip_entry z = gambit_parse_next_file(&zs, 0);
        if (!z.p_mem) break;
        for (i = 0; i < z.Header.FileNameLength; i++) c[i] = z.FileName[i];
        c[i] = 0;

        if (!strcmp(c, "Song.xml"))
        {
            xrns_job Job;
            Job.WorkFunction = (xrns_worker_fcn) populateInstrumentsAndNotes;
            Job.Data         = &ParseDesc;
            Job.FreeData     = NULL;

            ParseDesc.g          = g;
            ParseDesc.xml        = z.p_mem;
            ParseDesc.xml_length = z.Header.UncompressedSize;
            ParseDesc.xdoc       = xdoc;

            AddToWorkTable(Decoding, Job);
        }
        else
        {
            xrns_job Job;
            populate_instrument_desc *SampleDesc = malloc(sizeof(populate_instrument_desc));
            SampleDesc->z    = z;
            SampleDesc->xdoc = xdoc;
            strncpy(SampleDesc->zipped_filename, c, 2048);
            Job.FreeData     = SampleDesc;
            Job.Data         = SampleDesc;
            Job.WorkFunction = (xrns_worker_fcn) populateInstrumentSample;

            AddToWorkTable(Decoding, Job);
        }

        // free(z.p_mem);

    } while (1);

    TracyCZoneEnd(main_ctx);
    TracyCZoneN(ctx, "Farm Work", 1);
    
    FarmPooledThreads(Workers, Decoding);

    //&
    for (i = 0; i < Decoding->NumJobs; i++)
    {
        xrns_job *Job = &Decoding->Jobs[i];
        if (Job->FreeData)
        {
            xrns_sample *Sample = (xrns_sample *) Job->Result;
            xdoc->Instruments[Sample->InstrumentNumber].Samples[Sample->SampleNumber].PCM           = Sample->PCM;
            xdoc->Instruments[Sample->InstrumentNumber].Samples[Sample->SampleNumber].SampleRateHz  = Sample->SampleRateHz;
            xdoc->Instruments[Sample->InstrumentNumber].Samples[Sample->SampleNumber].NumChannels   = Sample->NumChannels;
            xdoc->Instruments[Sample->InstrumentNumber].Samples[Sample->SampleNumber].LengthSamples = Sample->LengthSamples;
            free(Sample);
        }
    }

    FreeWorkTable(Decoding);
    free(ParseDesc.xml);

    TracyCZoneEnd(ctx);

    return 1;
}

typedef struct
{
    double        p;
    unsigned int  PrevPointIdx;
    char          PlaybackDirection;
    char          bSustaining;
} xrns_envelope_playback_state;

/* Instruments can have a maximum of 12 samples playing back at once.
 */
#define XRNS_MAX_INSTRUMENT_SAMPLES_PLAYING (12)

typedef struct
{
    /* Envelope Positions */
    xrns_envelope_playback_state VolumeEnvelope;
    xrns_envelope_playback_state PanningEnvelope;
    xrns_envelope_playback_state PitchEnvelope;

    char         Active;
    char         bPlaying;

    char         bMapped;

    /* used to trigger the stuff before the 1st slice, if S00 is used on
     * the "root" sample of a sliced instrument.
     */
    char         bPlay0Slice;

    double       PlaybackPosition;

    unsigned int CurrentSample;
    unsigned int CurrentBaseNote;
    unsigned int SamplesPlayedFor;
    char         PlaybackDirection;

    double       SxxStartPosition;
    char         SxxPlaybackDirection;

    /* Set the bounds on a sample,
     * the most interesting case is when a slice of
     * a non-aliased sliced instrument (the base sample)
     * is triggered. There are actually three different
     * points the sample may end at depending on the playback
     * direction.
     */
    float        BackPosition0;
    float        BackPosition1;
    float        FrontPosition0;

    int          CrossFade;
    int          CrossFadeDuration;
    int          IntroCrossFade;
    int          IntroCrossFadeDuration;

    char         bIntroIsCrossFading;
    char         bIsCrossFading;

} xrns_sample_playback_state;

typedef struct
{
    xrns_sample_playback_state
        PlaybackStates[XRNS_MAX_INSTRUMENT_SAMPLES_PLAYING];

    char         Active;
    char         bPlaying;
    
    unsigned int CurrentNote;
    unsigned int CurrentInstrument;
    LerpFloat    CurrentVolume;

    xrns_note    OriginalNote;

    float        SavedPitchMod;

    char         bStillOnHomeRow;

    LerpFloat    CurrentPanning;
    char         bPanningSlide;
    float        PanningSlideAmount;
  
    /* Qx commands in their various forms will cause samplers
     * to delay their start. 
     */
    int          QxValue;
    int          QCounter;
    int          FracDelay;
    char         bQPrepped;
    char         bQReadyForCalc;

    /* Pattern commands that effect the active samplers:
     *
     * D & U: These change the slide destination (a relative offset).
     * V: Sets the vibrato settings (additive, resets every row)
     *
     */

    char  bIsSliding;
    float CurrentSlideOffset;
    float CurrentSlideStart;
    float CurrentSlideDest;

    /* Volume slide.
     */
    char  bVolumeIsSliding;
    float VolumeSlideOffset;
    float VolumeSlideStart;
    int   VolumeSlideDest;
    char  bVolumeSlideStartedWithActiveNote;

    char  SlideTick;

    int   CurrentVibratoSpeed;
    int   CurrentVibratoDepth;
    float VibratoTick;
    float VibratoOffset;

    char  bIsNoteOff;

    /* Cx command cuts the volume to Volume on Tick Offset
     */
    int           CxVolume;
    int           CxOffset;
    char          bCxKill;

    /* Re-triggers.
     */
    int           RetriggerRate;
    int           RetriggerVolumeIsAdditive;
    float         RetriggerVolume;
    unsigned int  WetVolume;
    char          bRetriggerOnZero;

    /* Tremolo
     */
    int           CurrentTremoloSpeed;
    int           CurrentTremoloDepth;
    float         TremoloTick;
    float         TremoloAmount;

    /* Pitch gliding.
     */
    char          bInstantSlide;
    int           PitchSlideSpeed;
    unsigned int  PitchGoal16th;
    char          bIsPitchGliding;
    float         GlideNote; /* units of 16th semitones */
    char          bHitWithGCommand;

    /* Temporary storage for the correct computation of
     * the Bxx and Sxx commands.
     */
    int           BxxValue;
    int           SxxValue;

} xrns_sampler;

typedef struct
{
    int          MostRecentlyAllocatedSampler;
    int          MostRecentlyPlayingSampler;
    int          PitchToGlideTo;
    unsigned int NumSamplesOfXFade;
    xrns_sampler Samplers[XRNS_MAX_SAMPLERS_PER_COLUMN];
} xrns_sampler_bank;

void InitialiseSampler(xrns_sampler *Sampler)
{
    memset(Sampler, 0, sizeof(xrns_sampler));
    Sampler->CxVolume            = 0xF;
    Sampler->CxOffset            = -1;
    Sampler->bCxKill             =  0;
    Sampler->BxxValue            = -1;
    Sampler->SxxValue            = -1;
    for (int j = 0; j < XRNS_MAX_INSTRUMENT_SAMPLES_PLAYING; j++)
    {
        xrns_sample_playback_state *PlaybackState = &Sampler->PlaybackStates[j];
        PlaybackState->CurrentSample = -1;
        PlaybackState->bMapped = 0;
        PlaybackState->BackPosition0  = 0.0f;
        PlaybackState->BackPosition1  = 0.0f;
    }
    ResetLerp(&Sampler->CurrentPanning, 0x40,  XRNS_PANNING_LERP);
    ResetLerp(&Sampler->CurrentVolume,  0x100, XRNS_PANNING_LERP);
}

typedef struct
{
    float       *OutputRingBuffer;
    int          RingBufferReadPtr;
    int          RingBufferWritePtr;
    int          RingBufferFreeSamples;
    int          RingBufferSz;
} xrns_ringbuffer;

void InitRingBuffer(xrns_ringbuffer *Ringbuffer)
{
    Ringbuffer->RingBufferSz          = 1<<13;
    Ringbuffer->RingBufferWritePtr    = 0;
    Ringbuffer->RingBufferReadPtr     = 0;
    Ringbuffer->RingBufferFreeSamples = Ringbuffer->RingBufferSz;
    Ringbuffer->OutputRingBuffer      = malloc(Ringbuffer->RingBufferSz * 2 * sizeof(float));
}

void FreeRingBuffer(xrns_ringbuffer *Ringbuffer)
{
    free(Ringbuffer->OutputRingBuffer);
}

void PushRingBuffer(xrns_ringbuffer *Ringbuffer, float *Samples, unsigned int NumSamples)
{   
    int i = 0;
    while (i < NumSamples)
    {
        float *p_track_samples = &Ringbuffer->OutputRingBuffer[2 * Ringbuffer->RingBufferWritePtr];

        p_track_samples[0] = Samples[2*i + 0];
        p_track_samples[1] = Samples[2*i + 1];

        Ringbuffer->RingBufferWritePtr = (Ringbuffer->RingBufferWritePtr + 1) % Ringbuffer->RingBufferSz;
        Ringbuffer->RingBufferFreeSamples--;
        i++;
    }
}

typedef struct 
{
    LerpFloat        CurrentPreVolume;
    LerpFloat        CurrentGamePostVolume;
    LerpFloat        CurrentPanning;
    float            CurrentPostVolume;
    float            CurrentWidth;
    char             bIsMuted;
    int              CurrentNoteIndex;
    unsigned int     LastInstrumentToBeUsed[XRNS_MAX_COLUMNS_PER_TRACK];
    unsigned int     LastEffectToBeUsed[XRNS_MAX_COLUMNS_PER_TRACK + 1][XRNS_STORED_EFFECT_NUM];
    unsigned int     LastVolumeToBeUsed[XRNS_MAX_COLUMNS_PER_TRACK];
    unsigned int     LastPanningToBeUsed[XRNS_MAX_COLUMNS_PER_TRACK];
    int             *DSPEffectEnableFlags;
    dsp_effect      *DSPEffects;
    xrns_ringbuffer  RawAudio;
} xrns_track_playback_state;

#pragma pack(push, 1) 
typedef struct
{
    int32_t Value;
    int32_t Octave;
    int32_t Row;
    int32_t Column;
    int32_t Instrument;
    int32_t Track;
} xrns_note_from_caller;
#pragma pack(pop)

struct _XRNSPlaybackState
{
    char         bSongStopped;
    double       OutputSampleRate;

    float        CurrentBPMAugmentation;

    /* Caller can redirect flow of the song by setting the next
     * pattern index to something.
     */
    int          PatternHasBeenCued;
    unsigned int CuedPatternIndex;

    /* These are inclusive, and are used to restrict the flow
     * through the pattern sequence.
     */
    unsigned int PatternSequenceLoopStart;
    unsigned int PatternSequenceLoopEnd;

    /* These "Next" things are for someone wanting to know what the next row is, so
     * it can be mutated beforehand.
     */
    unsigned int NextPatternIndex;
    unsigned int NextRowIndex;

    unsigned int CurrentPatternIndex;
    unsigned int CurrentRow;

    unsigned int CurrentTick;
    float        CurrentBPM;
    unsigned int CurrentLinesPerBeat;
    unsigned int CurrentTicksPerLine;

    double       CurrentTickDuration;
    double       CurrentLineDuration;

    unsigned int CurrentSample;

    char         bFirstPlay;
    char         bEvenEarlierFirstPlay;

    /* this is an offset in fractional samples that aligns
     * the next pattern of notes.
     */
    double       XRNSGridOffset;

    double       LocationOfNextTick;
    double       LocationOfNextLine;

    double       BaseOfCurrentlyPlayingLine;

    xrns_document *xdoc;

    /* Each column in each track will need some state for what is currently playing.
     * We will use a sampler per column per track. 
     *       SamplerBanks[Track] -> (SamplerBank x NumCols)
     */
    xrns_sampler_bank **SamplerBanks;

    unsigned int NumSamplesOfXFade;
    xrns_track_playback_state **TrackStates;

    galloc_ctx *g;

    unsigned int SerializedSongLengthInBytes;
    char         bSongHasBeenSerialized;
    void        *SerializedSongMemory;

    xrns_ringbuffer Output;

    xrns_note_from_caller *CallerNotes;
    unsigned int           NumCallerNotes;

    xrns_sampler_info ActiveSamplers[XRNS_ACTIVE_SAMPLER_COUNT];
    unsigned int      NumActiveSamplers;

    void *ScratchMemory;

    pooled_threads_ctx *Workers;

    int bStopAtEndOfSong;

};

static double PresentLineDuration(XRNSPlaybackState *xstate)
{
    if (!xstate->CurrentLinesPerBeat)    return 1.0f;
    if (!xstate->CurrentBPM)             return 1.0f;
    if (!xstate->CurrentBPMAugmentation) return 1.0f;

    return xstate->OutputSampleRate * (60.0 / ((double) xstate->CurrentLinesPerBeat * (double) xstate->CurrentBPM * (xstate->CurrentBPMAugmentation / 100.0f)));
}

static double PresentTickDuration(XRNSPlaybackState *xstate)
{
    if (!xstate->CurrentLinesPerBeat)    return 1.0f;
    if (!xstate->CurrentBPM)             return 1.0f;
    if (!xstate->CurrentBPMAugmentation) return 1.0f;

    return xstate->OutputSampleRate * (60.0 / ((double) xstate->CurrentLinesPerBeat * (double) xstate->CurrentBPM * (double) xstate->CurrentTicksPerLine * (xstate->CurrentBPMAugmentation / 100.0f)));
}

void RecomputeDurations(XRNSPlaybackState *xstate)
{
    xstate->CurrentTickDuration = PresentTickDuration(xstate);
    xstate->CurrentLineDuration = PresentLineDuration(xstate);
}

unsigned int NextEnvelopeSample(xrns_envelope *Desc, xrns_envelope_playback_state *Envelope)
{
    if (Envelope->PlaybackDirection == XRNS_FORWARD)
    {
        if (Envelope->PrevPointIdx == Desc->NumPoints - 1)
        {
            /* already on the last point */
            return Envelope->PrevPointIdx;
        }
        else
        {
            return Envelope->PrevPointIdx + 1;
        }
    }
    else
    {
        if (Envelope->PrevPointIdx == 0)
        {
            /* already on the last point */
            return 0;
        }        
        else
        {
            return Envelope->PrevPointIdx - 1;
        }
    }
}


int OnLastEnvelopePoint(xrns_envelope *Desc, xrns_envelope_playback_state *Envelope)
{
    if (Envelope->PlaybackDirection == XRNS_FORWARD)
    {
        if (Envelope->PrevPointIdx == Desc->NumPoints - 1)
        {
            return (Desc->LoopMode == XRNS_LOOP_MODE_OFF);
        }
    }

    return 0;
}

unsigned int FindIndexBeforeValue(xrns_envelope *Desc, float Value)
{
    int i = 0;
    unsigned int idx = 0;
    for (i = 0; i < Desc->NumPoints; i++)
    {
        if (Desc->Points[i].Pos > Value)
        {
            break;
        }

        idx = i;
    }
    return idx;
}

unsigned int FindIndexAfterValue(xrns_envelope *Desc, float Value)
{
    int i = 0;
    unsigned int idx = 0;
    for (i = Desc->NumPoints - 1; i >= 0; i++)
    {
        if (Desc->Points[i].Pos < Value)
        {
            break;
        }

        idx = i;
    }
    return idx;
}

double CurveSample
    (xrns_envelope *Desc
    ,double         ProposedNewPosition
    ,float          Bez
    ,float          Val0
    ,float          Val1
    ,float          Pos0
    ,float          Pos1
    )
{
    double InterpolatedVal = 0.0;

    TracyCZoneN(ctx, "CurveSample", 1);

    if (Desc->CurveType == XRNS_CURVE_TYPE_POINTS)
    {
        InterpolatedVal = Val0;
    }
    else if (Desc->CurveType == XRNS_CURVE_TYPE_LINES)
    {
        if (Pos0 == Pos1)
        {
            InterpolatedVal = Val0;
        }
        else
        {
            /* Lines can have a crazy exponential term if the value is anything other than 0.0.
             * x^(1 + 16 * Bez ^ (4/3))
             */

            float a0;
            float a1;
            float b0;
            float b1;

            float B;
            float H;

            int bOrientation = (Val0 > Val1);
            int bNegBez      = (Bez < 0.0f);

            if (bOrientation)
            {
                B = Val1;
                H = Val0 - Val1;                
            }
            else
            {
                B = Val0;
                H = Val1 - Val0;                
            }

            if (bNegBez)
            {
                a0 =  1.0f;
                a1 = -1.0f;
            }
            else
            {
                a0 =  0.0f;
                a1 =  1.0f;
            }

            if (bNegBez != bOrientation)
            {
                b0 =  1.0f;
                b1 = -1.0f;                
            }
            else
            {
                b0 =  0.0f;
                b1 =  1.0f;                
            }

            InterpolatedVal = B + H * (b0 + b1 * pow((a0 + a1 * (ProposedNewPosition - Pos0) / (Pos1 - Pos0)), 1.0f + 16.0f * pow(fabsf(Bez), 1.333f)));
        }
    }
    else if (Desc->CurveType == XRNS_CURVE_TYPE_CURVES)
    {
        if (Pos0 == Pos1)
        {
            InterpolatedVal = Val0;
        }
        else
        {
            /* Cubic Bezier with four points (A, B, C, D).
             * A & D are the start and end points, the curve goes through these.
             * B & C are the control points, they are constrained to lie at fixed positions
             * mid = (Ax + Dx)/2
             * B = (mid, Ay) and C = (mid, Dy)
             * 
             * This creates a pleasant evenly shaped curve.
             * The parametric formula for the curve is....
             *
             * P(t) = A(1-t)^3 + 3B(1-t)^2t + 3C(1-t)t^2 + Dt^3
             *
             * This does us a fat lot of good for determining the Y value at a point X.
             * Instead we plug in X and solve for t, and then sub t into the same expression for Y.
             *
             * Letting a, b, c, d be the x values of A, B, C, D. XDes is our desired X.
             *
             * (d - a)t^3 + 1.5(a - d)t^2 + 1.5(d - a)t + (a - XDes) = 0
             *
             * We require a != 0, in other words, the start and end points of the curve must not be the same.
             *
             * Because we know this cubic has only one real solution at XDes, we can directly  
             * compute the solution using Cardano's formula, assuming one real solution and two
             * complex conjugated solutions that we ignore.
             *
             */

            float Q, R, S, T;
            float a, b, c, d;
            float descrim;

            float alpha = 0.5f * (1.0f + Bez);

            if (Desc->CurveType == XRNS_CURVE_TYPE_LINES)
            {
                float bx = Pos1 * alpha + Pos0 * (1.0f - alpha);
                a = Pos1 - Pos0;
                b = 3.0f * (Pos0 - bx);
                c = 3.0f * (bx - Pos0);
                d = (Pos0 - ProposedNewPosition);
            }
            else
            {
                a = Pos1 - Pos0;
                b = 1.5f * (Pos0 - Pos1);
                c = 1.5f * (Pos1 - Pos0);
                d = (Pos0 - ProposedNewPosition);                
            }

            Q = (3.0f*a*c - b*b)/(9.0f*a*a);
            R = (9.0f*a*b*c - 27.0f*a*a*d - 2.0f*b*b*b)/(54.0f*a*a*a);

            descrim = sqrt(Q*Q*Q + R*R);

            S = cbrt(R + descrim);
            T = cbrt(R - descrim);

            float t = S + T - b/(3.0f * a);

            if (Desc->CurveType == XRNS_CURVE_TYPE_LINES)
            {
                float by = Val0 * alpha + Val1 * (1.0f - alpha);
                a = Val1 - Val0;
                b = 3.0f * (Val0 - by);
                c = 3.0f * (by - Val0);
                d = Val0;
            }
            else
            {
                a = 2.0f * (Val0 - Val1);
                b = 3.0f * (Val1 - Val0);
                c = 0;
                d = Val0;
            }

            float y = a*t*t*t + b*t*t + c*t + d;

            InterpolatedVal = y;
        }
    }

    TracyCZoneEnd(ctx);

    return InterpolatedVal;
}

double WalkEnvelopeStateless(xrns_envelope *Desc, double Position)
{
    float Bez;
    float Val0;
    float Val1;
    float Pos0;
    float Pos1;

    int PrevPoint = 0;
    int NextPoint = 0;

    for (int p = 0; p < Desc->NumPoints; p++)
    {
        if (Desc->Points[p].Pos <= Position)
        {
            PrevPoint = p;
        }
    }

    if (PrevPoint < Desc->NumPoints - 1)
    {
        NextPoint = PrevPoint + 1;
    }
    else
    {
        NextPoint = PrevPoint;
    }

    Bez  = Desc->Points[PrevPoint].Bez;
    Val0 = Desc->Points[PrevPoint].Val;
    Val1 = Desc->Points[NextPoint].Val;
    Pos0 = Desc->Points[PrevPoint].Pos;
    Pos1 = Desc->Points[NextPoint].Pos;

    return CurveSample
        (Desc
        ,Position
        ,Bez
        ,Val0
        ,Val1
        ,Pos0
        ,Pos1
        );    
}

/* dt_in_seconds will be 1/fs if this is called every sample
 */
double WalkEnvelope(XRNSPlaybackState *xstate,
                    xrns_envelope *Desc,
                    xrns_envelope_playback_state *Envelope,
                    double dt_in_seconds,
                    int bIgnoreSustain)
{
    TracyCZoneN(ctx, "WalkEnvelope", 1);

    int NextPointIdx = NextEnvelopeSample(Desc, Envelope);

    /* walk the cursor by dt in our units */
    double dt_env = dt_in_seconds;
    if (Desc->Units == XRNS_ENVELOPE_UNITS_MS)
    {
        /* Renoise 3.x allows the units to correspond to milliseconds.
         */
        dt_env = dt_in_seconds * 1000.0;
    }
    else if (Desc->Units == XRNS_ENVELOPE_UNITS_BEATS)
    {
        /* Renoise 3.x changed the units from lines to 1/256th beats.
         */
        dt_env = dt_in_seconds * (xstate->CurrentBPM / 60.0) * 256.0;
    }
    else if (Desc->Units == XRNS_ENVELOPE_UNITS_LINES)
    {
        /* Renoise 2.x, always uses lines.
         */
        dt_env = dt_in_seconds / xstate->CurrentLineDuration;
    }
    else
    {
        /* Pattern automation curves are always in 1/256th of
         * a line. e.g. 64 line pattern has a length of 64*256 = 16,384.
         */
    }

    if (Envelope->bSustaining && !bIgnoreSustain)
        dt_env = 0;

    int bWrappedNextPoint = 0;
    int bWrappedSustainPosition = 0;
    int bWrappedLoopPosition = 0;

    double ProposedNewPosition;
    if (Envelope->PlaybackDirection == XRNS_FORWARD)
    {
        ProposedNewPosition = Envelope->p + dt_env;
        if (ProposedNewPosition >= Desc->Points[NextPointIdx].Pos)
        {
            bWrappedNextPoint = 1;
        }

        if (ProposedNewPosition >= Desc->SustainPosition)
        {
            bWrappedSustainPosition = 1;
        }

        if (ProposedNewPosition >= Desc->LoopEnd)
        {
            bWrappedLoopPosition = 1;
        }
    }
    else
    {
        ProposedNewPosition = Envelope->p - dt_env;
        if (ProposedNewPosition <= Desc->Points[NextPointIdx].Pos)
        {
            bWrappedNextPoint = 1;
        }

        if (ProposedNewPosition <= Desc->SustainPosition)
        {
            bWrappedSustainPosition = 1;
        }

        if (ProposedNewPosition <= Desc->LoopStart)
        {
            bWrappedLoopPosition = 1;
        }
    }

    if (bWrappedNextPoint)
    {
        Envelope->PrevPointIdx = NextPointIdx;
        NextPointIdx = NextEnvelopeSample(Desc, Envelope);
    }

    if (Desc->bSustainOn && bWrappedSustainPosition && !bIgnoreSustain)
    {
        Envelope->bSustaining = 1;
        ProposedNewPosition = Desc->SustainPosition;
    }

    if (bWrappedLoopPosition)
    {
        if (Envelope->PlaybackDirection == XRNS_FORWARD)
        {
            double OverHang = ProposedNewPosition - Desc->LoopEnd;

            if (Desc->LoopMode == XRNS_LOOP_MODE_FORWARD)
            {
                ProposedNewPosition = Desc->LoopStart + OverHang;

                /* jumped thanks to loop, recompute Prev and Next */
                Envelope->PrevPointIdx = FindIndexBeforeValue(Desc,  ProposedNewPosition);
                NextPointIdx = NextEnvelopeSample(Desc, Envelope);

            } else if (Desc->LoopMode == XRNS_LOOP_MODE_BACKWARD)
            {
                ProposedNewPosition = Desc->LoopEnd - OverHang;
                Envelope->PlaybackDirection = XRNS_BACKWARD;
                /* flip indicies */
                unsigned int Temp = Envelope->PrevPointIdx;
                Envelope->PrevPointIdx = NextPointIdx;
                NextPointIdx = Temp;
            } else if (Desc->LoopMode == XRNS_LOOP_MODE_PINGPONG)
            {
                ProposedNewPosition = Desc->LoopEnd - OverHang;
                Envelope->PlaybackDirection = XRNS_BACKWARD;
                /* flip indicies */
                unsigned int Temp = Envelope->PrevPointIdx;
                Envelope->PrevPointIdx = NextPointIdx;
                NextPointIdx = Temp;
            }
        }
        else if (Envelope->PlaybackDirection == XRNS_BACKWARD)
        {
            double OverHang = Desc->LoopStart - ProposedNewPosition;

            if (Desc->LoopMode == XRNS_LOOP_MODE_BACKWARD)
            {
                ProposedNewPosition = Desc->LoopEnd - OverHang;

                /* jumped thanks to loop, recompute Prev and Next */
                Envelope->PrevPointIdx = FindIndexAfterValue(Desc,  ProposedNewPosition);
                NextPointIdx = NextEnvelopeSample(Desc, Envelope);
            } else if (Desc->LoopMode == XRNS_LOOP_MODE_PINGPONG)
            {
                ProposedNewPosition = Desc->LoopStart + OverHang;
                Envelope->PlaybackDirection = XRNS_FORWARD;
                /* flip indicies */
                unsigned int Temp = Envelope->PrevPointIdx;
                Envelope->PrevPointIdx = NextPointIdx;
                NextPointIdx = Temp;
            }
        }
    }

    float Bez;
    float Val0;
    float Val1;
    float Pos0;
    float Pos1;

    if (Envelope->PlaybackDirection == XRNS_FORWARD)
    {
        Bez  = Desc->Points[Envelope->PrevPointIdx].Bez;
        Val0 = Desc->Points[Envelope->PrevPointIdx].Val;
        Val1 = Desc->Points[NextPointIdx].Val;
        Pos0 = Desc->Points[Envelope->PrevPointIdx].Pos;
        Pos1 = Desc->Points[NextPointIdx].Pos;        
    }
    else 
    {
        Bez  = Desc->Points[NextPointIdx].Bez;
        Val0 = Desc->Points[NextPointIdx].Val;
        Val1 = Desc->Points[Envelope->PrevPointIdx].Val;
        Pos0 = Desc->Points[NextPointIdx].Pos;
        Pos1 = Desc->Points[Envelope->PrevPointIdx].Pos;        
    }

    /* Sample from the curves.
     */
    float InterpolatedVal = CurveSample
        (Desc
        ,ProposedNewPosition
        ,Bez
        ,Val0
        ,Val1
        ,Pos0
        ,Pos1
        );

    Envelope->p = ProposedNewPosition;

    TracyCZoneEnd(ctx);

    return InterpolatedVal;
}

void PerformNewNoteActionOnSamplerBank(XRNSPlaybackState *xstate, xrns_document *xdoc, xrns_sampler_bank *SamplerBank, int bForceNoteOff)
{
    /* a new note was hit on this column, so go and perform the correct
     * new note events on the active samplers.
     */
    int j, s;

    for (s = 0; s < XRNS_MAX_SAMPLERS_PER_COLUMN; s++)
    {
        xrns_sampler *Sampler = &SamplerBank->Samplers[s];

        if (!Sampler->Active) continue;

        for (j = 0; j < XRNS_MAX_INSTRUMENT_SAMPLES_PLAYING; j++)
        {
            xrns_sample_playback_state *PlaybackState = &Sampler->PlaybackStates[j];

            if (PlaybackState->bIsCrossFading) continue;

            int CurrentSample = PlaybackState->CurrentSample;
            if (CurrentSample == -1) continue;

            xrns_sample *Sample = &xdoc->Instruments[Sampler->CurrentInstrument].Samples[CurrentSample];

            if (bForceNoteOff || (Sample->NewNoteAction == XRNS_NNA_NOTEOFF))
            {
                PlaybackState->bIsCrossFading = 1;

                /* the release period for samples ALSO applies to the sample data itself
                 * the loop region is completely ignored if LoopRelease is set true,
                 * and the sample continues to play, in whatever direction it was in
                 */
                if (Sample->LoopRelease)
                {
                    PlaybackState->CrossFade = (xstate->OutputSampleRate * 240) / 1;
                    PlaybackState->CrossFadeDuration = -1;
                }
                else
                {
                    PlaybackState->CrossFade = SamplerBank->NumSamplesOfXFade;
                    PlaybackState->CrossFadeDuration = PlaybackState->CrossFade;
                }

                /* note-off events end the sustain and start the release period.
                 * during the release period, sustain is totally ignored, and the
                 * envelopes continue to loop, whilst the release fadeout is applied.
                 */

                if (   xdoc->Instruments[Sampler->CurrentInstrument].NumModulationSets
                    && (Sample->ModulationSetIndex != -1))
                {
                    xrns_modulation_set *ModulationSet = &xdoc->Instruments[Sampler->CurrentInstrument].ModulationSets[Sample->ModulationSetIndex];

                    if (ModulationSet->bVolumeEnvelopePresent)
                    {
                        /* If ReleaseValue is 0, the release time is infinite.
                         * ReleaseValue of 1 will give 240 seconds of release time, or 4 minutes.
                         * ReleaseValue of 2 is 2 minutes, following the relationship.
                         *
                         * DurationSamples = Fs * 240 / ReleaseValue;
                         */

                        if (ModulationSet->Volume.ReleaseValue != 0)
                        {
                            PlaybackState->CrossFade = (32/xstate->CurrentBPM) * (xstate->OutputSampleRate * 240) / ModulationSet->Volume.ReleaseValue;
                            PlaybackState->CrossFadeDuration = PlaybackState->CrossFade;
                        }
                        else
                        {
                            PlaybackState->CrossFade = (xstate->OutputSampleRate * 240) / 1;
                            PlaybackState->CrossFadeDuration = -1;
                        }
                    }
                }
            }
            else if (Sample->NewNoteAction == XRNS_NNA_CUT)
            {
                /* cut events force the sampler into the x-fade out, no release
                 * envelopes or anything fancy.
                 */
                PlaybackState->CrossFade = SamplerBank->NumSamplesOfXFade;
                PlaybackState->CrossFadeDuration = PlaybackState->CrossFade;
                PlaybackState->bIsCrossFading = 1;
            }
            else
            {
                /* continue actually just implies that there is nothing to do, 
                 * just keep playing until pattern commands stop things, or you
                 * run out of polyphony....
                 */
            }
        }
    }
}

void SamplerBlankTriggerNewNote(XRNSPlaybackState *xstate, 
                                xrns_sampler_bank *SamplerBank,
                                xrns_note          OriginalNote, 
                                unsigned int       TrackIndex,
                                xrns_sampler      *DerivedSampler)
{
    int c, j;
    int i = SamplerBank->MostRecentlyAllocatedSampler;

    unsigned int TheNote = OriginalNote.Note; 
    unsigned int TheInstrument = OriginalNote.Instrument; 
    unsigned int TheVolume = OriginalNote.Volume; 
    unsigned int ThePanning = OriginalNote.Panning; 
    unsigned int TheFineDelay = OriginalNote.Delay;
    unsigned int ColumnIndex = OriginalNote.Column;

    int RetriggerRate = 0;
    float RetriggerVolume = 0; 
    int RetriggerVolumeIsAdditive = 0;
    unsigned int WetVolume = 0;

    /* useful when cloning the sampler for retriggers, more things might go in here one day
     * if interesting interactions between the retrigger command the other commands are possible.
     */
    if (DerivedSampler)
    {
        RetriggerRate = DerivedSampler->RetriggerRate;
        RetriggerVolumeIsAdditive = DerivedSampler->RetriggerVolumeIsAdditive;
        RetriggerVolume = DerivedSampler->RetriggerVolume;
        WetVolume = DerivedSampler->WetVolume;
    }

    if (TheNote == XRNS_NOTE_OFF)
    {
        i = (i + 1) % XRNS_MAX_SAMPLERS_PER_COLUMN;
        SamplerBank->MostRecentlyAllocatedSampler = i;

        xrns_sampler *Sampler = &SamplerBank->Samplers[i];

        InitialiseSampler(Sampler);
        Sampler->bQReadyForCalc    = 1;
        Sampler->Active            = 0;
        Sampler->bPlaying          = 0;
        Sampler->bIsNoteOff        = (TheNote == XRNS_NOTE_OFF);
        Sampler->QxValue           = 0;
        Sampler->FracDelay         = (TheFineDelay == XRNS_MISSING_VALUE) ? 0 : TheFineDelay;
        return;
    }

    if (TheVolume == XRNS_MISSING_VALUE)
    {
        if (TheInstrument == XRNS_MISSING_VALUE)
        {
            TheVolume = xstate->TrackStates[TrackIndex]->LastVolumeToBeUsed[ColumnIndex];    
        }
        else
        {
            TheVolume = 0x80;
            xstate->TrackStates[TrackIndex]->LastVolumeToBeUsed[ColumnIndex] = 0x80;
        }
    }
    else
    {
        xstate->TrackStates[TrackIndex]->LastVolumeToBeUsed[ColumnIndex] = TheVolume;
    }

    if (ThePanning == XRNS_MISSING_VALUE)
    {
        if (TheInstrument == XRNS_MISSING_VALUE)
        {
            ThePanning = xstate->TrackStates[TrackIndex]->LastPanningToBeUsed[ColumnIndex];
        }
        else
        {
            ThePanning = 0x40;
            xstate->TrackStates[TrackIndex]->LastPanningToBeUsed[ColumnIndex] = ThePanning;
        }
    }
    else
    {
        xstate->TrackStates[TrackIndex]->LastPanningToBeUsed[ColumnIndex] = ThePanning;
    }    

    if (TheInstrument == XRNS_MISSING_VALUE)
    {
        TheInstrument = xstate->TrackStates[TrackIndex]->LastInstrumentToBeUsed[ColumnIndex];
    }
    else
    {
        xstate->TrackStates[TrackIndex]->LastInstrumentToBeUsed[ColumnIndex] = TheInstrument;
    }

    /* Here is where we need to see which samples are actually triggered by the note.
     * Checking the Note and the Velocity.
     */

    xrns_ssm *SampleSplitMap;
    int bDudNote  = 0;
    int bFoundSSM = 0; 

    if (TheInstrument >= xstate->xdoc->NumInstruments)
    {
        bDudNote = 1;
    }

    i = (i + 1) % XRNS_MAX_SAMPLERS_PER_COLUMN;
    SamplerBank->MostRecentlyAllocatedSampler = i;
    xrns_sampler *Sampler = &SamplerBank->Samplers[i];

    j = 0;

    InitialiseSampler(Sampler);

    if (!bDudNote)
    {
        xrns_instrument *Instrument = &xstate->xdoc->Instruments[TheInstrument];
        for (c = 0; c < Instrument->NumSampleSplitMaps; c++)
        {
            SampleSplitMap = &Instrument->SampleSplitMaps[c];

            /* Increase the velocity end region, because for some reason 
             * a volume of 0x80 triggers 
             */
            if ((SampleSplitMap->NoteStart <= TheNote &&
                SampleSplitMap->NoteEnd >= TheNote) &&
                (SampleSplitMap->VelocityStart <= TheVolume &&
                (SampleSplitMap->VelocityEnd + 1) >= TheVolume)
                && SampleSplitMap->bIsNoteOn)
            {
                bFoundSSM = 1;

                xrns_sample_playback_state *PlaybackState = &Sampler->PlaybackStates[j];
                xrns_sample *Sample = &Instrument->Samples[SampleSplitMap->SampleIndex];

                PlaybackState->Active      = 0;
                PlaybackState->bPlaying    = 0;
                PlaybackState->bMapped     = 1;
                PlaybackState->bPlay0Slice = 0;

                Sampler->bQReadyForCalc    = 1;
                Sampler->Active            = 0;
                Sampler->bPlaying          = 0;
                Sampler->bIsNoteOff        = (TheNote == XRNS_NOTE_OFF);

                PlaybackState->SamplesPlayedFor  = 0;
                PlaybackState->PlaybackPosition  = 0.0;
                PlaybackState->PlaybackDirection = XRNS_FORWARD;
                PlaybackState->CrossFade         = 0;
                PlaybackState->bIsCrossFading    = 0;
                PlaybackState->CurrentSample     = SampleSplitMap->SampleIndex;
                PlaybackState->CurrentBaseNote   = SampleSplitMap->BaseNote;

                /* this doesn't work for sliced samples, but the front and back
                 * positions will be fixed during the Sxx sample code later.
                 */
                PlaybackState->FrontPosition0 = Sample->LengthSamples;

                PlaybackState->IntroCrossFadeDuration = 80;
                PlaybackState->IntroCrossFade = 0;
                PlaybackState->bIntroIsCrossFading = 1;

                Sampler->RetriggerRate = RetriggerRate;
                Sampler->RetriggerVolume = RetriggerVolume;
                Sampler->RetriggerVolumeIsAdditive = RetriggerVolumeIsAdditive;

                Sampler->OriginalNote          = OriginalNote;
                Sampler->CurrentNote           = TheNote;
                Sampler->CurrentInstrument     = TheInstrument;

                if (RetriggerRate && DerivedSampler)
                {
                    Sampler->WetVolume     = WetVolume;
                    Sampler->CurrentVolume.Val = Sampler->WetVolume * 2u;
                    Sampler->CurrentVolume.Target = Sampler->WetVolume * 2u;

                    Sampler->SxxValue = DerivedSampler->SxxValue;
                    Sampler->BxxValue = DerivedSampler->BxxValue;

                    xrns_sample_playback_state *DerivedPlaybackState = &DerivedSampler->PlaybackStates[j];

                    PlaybackState->PlaybackPosition     = DerivedPlaybackState->SxxStartPosition;
                    PlaybackState->SxxStartPosition     = DerivedPlaybackState->SxxStartPosition;
                    PlaybackState->PlaybackDirection    = DerivedPlaybackState->SxxPlaybackDirection;
                    PlaybackState->SxxPlaybackDirection = DerivedPlaybackState->SxxPlaybackDirection;
                    PlaybackState->BackPosition0        = DerivedPlaybackState->BackPosition0;
                    PlaybackState->BackPosition1        = DerivedPlaybackState->BackPosition1;
                    PlaybackState->FrontPosition0       = DerivedPlaybackState->FrontPosition0;
                }
                else
                {
                    Sampler->CurrentVolume.Val = TheVolume * 2u;
                    Sampler->CurrentVolume.Target = TheVolume * 2u;
                }
                
                Sampler->CurrentPanning.Val    = ThePanning;
                Sampler->CurrentPanning.Target = ThePanning;

                Sampler->QxValue           = 0;
                Sampler->FracDelay         = (TheFineDelay == XRNS_MISSING_VALUE) ? 0 : TheFineDelay;

                Sampler->bHitWithGCommand  = 0;

                SamplerBank->PitchToGlideTo = Sampler->CurrentNote;

                Sampler->bStillOnHomeRow   = 1;
                Sampler->TremoloAmount     = 1.0f;

                j = (j + 1) % XRNS_MAX_INSTRUMENT_SAMPLES_PLAYING;
            }
        }

        if (!bFoundSSM)
        {
            bDudNote = 1;
        }
    }

    if (bDudNote)
    {
        xrns_sample_playback_state *PlaybackState = &Sampler->PlaybackStates[j];
        PlaybackState->CurrentSample = -1;
        Sampler->bQReadyForCalc    = 1;
        Sampler->QxValue           = 0;
        Sampler->FracDelay         = (TheFineDelay == XRNS_MISSING_VALUE) ? 0 : TheFineDelay;
        Sampler->Active            = 0;
        Sampler->bPlaying          = 0;
        Sampler->OriginalNote      = OriginalNote;
    }
}

void InitialiseTrackState(xrns_track_playback_state *TrackState, xrns_track_desc *TrackDesc)
{
    ResetLerp(&TrackState->CurrentPreVolume, TrackDesc->InitialPreVolume, 0.997f);
    ResetLerp(&TrackState->CurrentGamePostVolume, 1.0f, 0.997f);
    ResetLerp(&TrackState->CurrentPanning, 0.0f, 0.997f);

    TrackState->CurrentPostVolume = TrackDesc->InitialPostVolume;
    TrackState->CurrentWidth = 0.0f; 
    TrackState->bIsMuted = 0;
}

void CreateXRNSPlaybackState(galloc_ctx *g, XRNSPlaybackState *xstate, xrns_document *xdoc, float Fs)
{
    int i, j, k, TotalColumns = 0;
    xstate->xdoc = xdoc;
    xstate->bFirstPlay = 1;
    xstate->bEvenEarlierFirstPlay = 1;
    xstate->OutputSampleRate = Fs;
    xstate->NumSamplesOfXFade = floor(Fs * XRNS_XFADE_MS / (1000.0));

    xstate->SamplerBanks = galloc(g, sizeof(xrns_sampler_bank *) * xdoc->NumTracks);
    xstate->TrackStates = galloc(g, sizeof(xrns_track_playback_state *) * xdoc->NumTracks);

    for (i = 0; i < xdoc->NumTracks; i++)
    {
        xstate->SamplerBanks[i] = galloc(g, sizeof(xrns_sampler_bank) * xdoc->Tracks[i].NumColumns);
        TotalColumns += xdoc->Tracks[i].NumColumns + xdoc->Tracks[i].NumEffectColumns;

        for (j = 0; j < xdoc->Tracks[i].NumColumns; j++)
        {
            /* @Optimization: Why do the sampler banks need this? */
            xstate->SamplerBanks[i][j].NumSamplesOfXFade = xstate->NumSamplesOfXFade;
            for (k = 0; k < XRNS_MAX_SAMPLERS_PER_COLUMN; k++)
            {
                InitialiseSampler(&xstate->SamplerBanks[i][j].Samplers[k]);
            }
        }

        xstate->TrackStates[i] = galloc(g, sizeof(xrns_track_playback_state));
        InitialiseTrackState(xstate->TrackStates[i], &xdoc->Tracks[i]);

        InitRingBuffer(&xstate->TrackStates[i]->RawAudio);

        xstate->TrackStates[i]->DSPEffects = galloc(g, sizeof(dsp_effect) * xdoc->Tracks[i].NumDSPEffectUnits);
        xstate->TrackStates[i]->DSPEffectEnableFlags = galloc(g, sizeof(int *) * xdoc->Tracks[i].NumDSPEffectUnits);

        for (j = 0; j < xdoc->Tracks[i].NumDSPEffectUnits; j++)
        {
            dsp_effect_desc *EffectDesc = &xdoc->Tracks[i].DSPEffectDescs[j];
            dsp_effect             *DSP = &xstate->TrackStates[i]->DSPEffects[j];

            switch (EffectDesc->Type)
            {
                case XRNS_EFFECT_FILTER:
                {
                    BasicFilterPopulateDSPStruct(DSP);
                    break;
                } 
                case XRNS_EFFECT_REVERB:
                {
                    WestVerbPopulateDSPStruct(DSP);
                    break;
                }
            }

            xstate->TrackStates[i]->DSPEffectEnableFlags[j] = EffectDesc->Enabled;

            DSP->State = DSP->Open();
            DSP->SetSampleRate(DSP->State, xstate->OutputSampleRate);
            EffectDesc->NumParameters = DSP->NumParameters;

            /* copy all the initial parameters over. */
            for (int k = 0; k < EffectDesc->NumParameters; k++)
            {
                DSP->SetParameter(DSP->State, k, EffectDesc->Parameters[k]);
            }
        }
    }

    InitRingBuffer(&xstate->Output);

    xstate->xdoc->TotalColumns = TotalColumns;
    xstate->CallerNotes = galloc(g, TotalColumns * sizeof(xrns_note_from_caller));
    xstate->ScratchMemory = galloc(g, TotalColumns * sizeof(xrns_note));

    xstate->CurrentBPMAugmentation = 100.0f;

    xstate->CurrentBPM          = xstate->xdoc->BeatsPerMin;
    xstate->CurrentLinesPerBeat = xstate->xdoc->LinesPerBeat; 
    xstate->CurrentTicksPerLine = xstate->xdoc->TicksPerLine;

    RecomputeDurations(xstate);

    xstate->bSongHasBeenSerialized = 0;
    xstate->SerializedSongLengthInBytes = 0;
    xstate->SerializedSongMemory = 0;

    xstate->CurrentRow = 0;
    xstate->CurrentPatternIndex = 0;

    /* Yes, these are technically correct, because the song hasn't started playing yet.
     * In basically every other circumstance, GetNextPatternAndRowIndex() would need to be called instead.
     */
    xstate->NextRowIndex = 0;
    xstate->NextPatternIndex = 0;

    xstate->PatternSequenceLoopStart = 0;
    xstate->PatternSequenceLoopEnd = xdoc->PatternSequenceLength - 1;
    xstate->PatternHasBeenCued = 0;
    xstate->CuedPatternIndex = 0;
}

/* returns true if a pattern cue was spent */
int GetNextPatternAndRowIndex(XRNSPlaybackState *xstate, unsigned int *NextPatternIndex, unsigned int *NextRowIndex, int *bEndOfSong)
{
    int PatternCueObeyed = 0;
    unsigned int patternIndex = xstate->xdoc->PatternSequence[xstate->CurrentPatternIndex].PatternIdx;
    int numberOfLines = xstate->xdoc->PatternPool[patternIndex].NumberOfLines;

    if (bEndOfSong) *bEndOfSong = 0;

    if (xstate->CurrentRow == numberOfLines - 1)
    {
        if (xstate->PatternHasBeenCued && (xstate->CuedPatternIndex < xstate->xdoc->PatternSequenceLength))
        {
            *NextPatternIndex = xstate->CuedPatternIndex;
            PatternCueObeyed = 1;
        }
        else
        {
            unsigned int NextIndexInSeq = (xstate->CurrentPatternIndex + 1) % xstate->xdoc->PatternSequenceLength;

            if (xstate->CurrentPatternIndex + 1 == xstate->xdoc->PatternSequenceLength)
            {
                if (bEndOfSong) *bEndOfSong = 1;
            }

            if (xstate->CurrentPatternIndex == xstate->PatternSequenceLoopEnd)
            {
                NextIndexInSeq = xstate->PatternSequenceLoopStart;
            }

            *NextPatternIndex = NextIndexInSeq;
        }

        *NextRowIndex = 0;
    }
    else
    {
        *NextPatternIndex = xstate->CurrentPatternIndex;
        *NextRowIndex = xstate->CurrentRow + 1;
    }

    return PatternCueObeyed;
}


void xrns_perform_tick_processing(XRNSPlaybackState *xstate)
{
    int track, col, s;

    if (!xstate->CurrentTicksPerLine) return;
    if (!xstate->CurrentLinesPerBeat) return;
    if (!xstate->CurrentBPM)          return;

    for (track = 0; track < xstate->xdoc->NumTracks; track++)
    {
        for (col = 0; col < xstate->xdoc->Tracks[track].NumColumns; col++)
        {
            xrns_sampler_bank *SamplerBank = &xstate->SamplerBanks[track][col];

            for (s = 0; s < XRNS_MAX_SAMPLERS_PER_COLUMN; s++)
            {
                xrns_sampler *Sampler = &SamplerBank->Samplers[s];

                if (Sampler->bIsPitchGliding && Sampler->bPlaying)
                {
                    /* glide towards the destination note at the present speed, but divide the speed up
                     * by the ticks.
                     */
                    int PitchDifference = (16 * (SamplerBank->PitchToGlideTo - Sampler->CurrentNote)) + (int)Sampler->CurrentSlideDest;
                    int WiggleDifference = PitchDifference - Sampler->GlideNote;

                    if (!Sampler->bInstantSlide)
                    {
                        if (WiggleDifference > 0)
                        {
                            Sampler->GlideNote += (float) Sampler->PitchSlideSpeed / ((float) xstate->CurrentTicksPerLine);
                            if (Sampler->GlideNote > PitchDifference)
                            {
                                Sampler->GlideNote = PitchDifference;
                            }
                        }
                        else
                        {
                            Sampler->GlideNote -= (float) Sampler->PitchSlideSpeed / ((float) xstate->CurrentTicksPerLine);
                            if (Sampler->GlideNote < PitchDifference)
                            {
                                Sampler->GlideNote = PitchDifference;
                            }
                        }
                    }
                    else
                    {
                        Sampler->GlideNote = PitchDifference;
                    }
                }

                if (Sampler->bIsSliding)
                {
                    if (xstate->CurrentTicksPerLine == 1)
                    {
                        Sampler->CurrentSlideOffset += Sampler->CurrentSlideDest;
                    }
                    else if (xstate->CurrentTick)
                    {
                        /* Technically this is a little bit suspect, due to floating point precision. The alpha mixing
                         * method ensures the slide finished on the right thing at least.
                         */
                        Sampler->CurrentSlideOffset += Sampler->CurrentSlideDest / ((float) xstate->CurrentTicksPerLine - 1.0f);
                        Sampler->SlideTick++;
                    }
                }

                if (!Sampler->Active) continue;

                if (Sampler->bPanningSlide)
                {
                    Sampler->CurrentPanning.Target += Sampler->PanningSlideAmount * 8.0f / ((float) xstate->CurrentTicksPerLine);

                    if (Sampler->CurrentPanning.Target < 0.0f)
                    {
                        Sampler->CurrentPanning.Target = 0.0f;
                    }

                    if (Sampler->CurrentPanning.Target > 128.0f)
                    {
                        Sampler->CurrentPanning.Target = 128.0f;
                    }   
                }

                if (Sampler->bVolumeIsSliding /*&& xstate->CurrentTick*/)
                {
                    int NewVolumeGoal = Sampler->VolumeSlideStart + Sampler->VolumeSlideDest;

                    if (NewVolumeGoal < 0)   NewVolumeGoal = 0;
                    if (NewVolumeGoal > 256) NewVolumeGoal = 256;

                    if (Sampler->bVolumeSlideStartedWithActiveNote)
                    {
                        /* If the slide hit an already-playing note, 
                         * the first tick will slide up/down, and we will
                         * actually make it here when current_tick == 0.
                         */
                        float al = ((float) xstate->CurrentTick + 1.0f) / ((float) xstate->CurrentTicksPerLine - 0.0f);
                        Sampler->CurrentVolume.Target = al * NewVolumeGoal + (1.0f - al) * (Sampler->VolumeSlideStart);
                    }
                    else
                    {
                        /* Otherwise, the slide happened on the same row
                         * as the note, meaning the first tick should not
                         * recieve any slide.
                         */
                        float al = ((float) xstate->CurrentTick + 0.0f) / ((float) xstate->CurrentTicksPerLine - 1.0f);
                        Sampler->CurrentVolume.Target = al * NewVolumeGoal + (1.0f - al) * (Sampler->VolumeSlideStart);
                    }
                }

                /* Attempt to provide Vibrato.... */
                if (Sampler->CurrentVibratoDepth != 0)
                {
                    float m = -1.0f * sin(2.0f * (3.14159265f) * Sampler->CurrentVibratoSpeed * (Sampler->VibratoTick) / ((float) xstate->CurrentTicksPerLine * 12.0f));
                    Sampler->VibratoOffset = m * (200.0f * Sampler->CurrentVibratoDepth) / 16.0f;
                    Sampler->VibratoTick += 1.0f;
                }

                /* Tremolo is kind of the same? */
                if (Sampler->CurrentTremoloDepth)
                {
                    float m = cos(2.0f * (3.14159265f) * Sampler->CurrentTremoloSpeed * (Sampler->TremoloTick + 0.5f) / ((float) (xstate->CurrentTicksPerLine) * 24.0f + 1.0f));
                    Sampler->TremoloAmount = ((0xF - Sampler->CurrentTremoloDepth)/((float) 0xF)) + m*m * (Sampler->CurrentTremoloDepth/((float) 0xF));
                    Sampler->TremoloTick += 1.0f;
                }

                if (Sampler->CxOffset != -1)
                {
                    if (Sampler->bCxKill && (xstate->CurrentTick == Sampler->CxOffset))
                    {
                        for (int j = 0; j < XRNS_MAX_INSTRUMENT_SAMPLES_PLAYING; j++)
                        {
                            xrns_sample_playback_state *PlaybackState = &Sampler->PlaybackStates[j];
                            if (PlaybackState->bIsCrossFading) continue;
                            PlaybackState->CrossFade = xstate->NumSamplesOfXFade;
                            PlaybackState->CrossFadeDuration = PlaybackState->CrossFade;
                            PlaybackState->bIsCrossFading = 1;
                        }
                    }
                    else
                    {
                        /* Cxx command is annoying. 
                         *
                         * If it's placed in the panning/volume columns as Cx, it *cuts*
                         * the note, we call this CxKill. It happens on the tick you would expect.
                         *
                         * For some reason, the Cxx command doesn't happen on the tick you expect. A note with
                         * a C00 command on it will sound for one tick. This leads me to believe that 0 => 1. 
                         * A C00 command will turn on a silent note!
                         *
                         */
                        if (xstate->CurrentTick == 0)
                        {
                            /* check if this is a Cxx command.....
                             *
                             * The 0CXY effect ramps up the volume on the first tick on purpose, 
                             * then ramps down the volume as specified. It also doesn’t stop the sample, 
                             * it only ramps down the volume to zero.
                             *
                             * - from the Renoise forums.
                             *
                             *                              
                             */
                            if (!Sampler->bCxKill)
                            {
                                /* ramp up on the 0th tick */
                                Sampler->CurrentVolume.Target = 0x100;
                            }
                        }

                        int ActualOffset = Sampler->CxOffset;
                        if (ActualOffset == 0 )ActualOffset = 1;

                        if (xstate->CurrentTick == ActualOffset)
                        {
                            Sampler->CurrentVolume.Target = 0x100 * ((float) Sampler->CxVolume / ((float) 0xF));
                        }
                    }
                }

                /* A retrigger event may appear on its own, and put playing samplers into retrigger mode
                 * Regardless, the samplers will be given a limit on how many retriggers they may fire.
                 *
                 * If the Rxx command and the note appear together, Ticks - 1 retriggers are needed
                 * because the original note counts as one. If they appear apart, Ticks retriggers are needed.
                 *
                 * To avoid double-triggering when they appear together, we use the bStillOnHomeRow flag.
                 */

                if (   Sampler->RetriggerRate
                    && ((xstate->CurrentTick % Sampler->RetriggerRate) == 0)
                    && (((xstate->CurrentTick == 0) && Sampler->bRetriggerOnZero) || (xstate->CurrentTick))
                    && (SamplerBank->MostRecentlyAllocatedSampler == s)
                   )
                {
                    if (Sampler->WetVolume == XRNS_MISSING_VALUE)
                    {
                        Sampler->WetVolume = 0x80;
                    }

                    if (!Sampler->RetriggerVolumeIsAdditive)
                    {
                        Sampler->WetVolume = round(Sampler->WetVolume * Sampler->RetriggerVolume);
                        if (Sampler->WetVolume > 0x80)
                            Sampler->WetVolume = 0x80;
                    }
                    else
                    {
                        int Adj = round(0x80 * Sampler->RetriggerVolume);

                        if (Adj < 0)
                        {
                            if (Sampler->WetVolume < -Adj)
                                Sampler->WetVolume = 0u;
                            else
                                Sampler->WetVolume += Adj;
                        }
                        else
                        {
                            if (Sampler->WetVolume + Adj > 0x80)
                                Sampler->WetVolume = 0x80;                            
                            else
                                Sampler->WetVolume += Adj;
                        }
                    }
                    
                    SamplerBlankTriggerNewNote(xstate, SamplerBank, Sampler->OriginalNote, track, Sampler);
                }
            }
        }
    }

    /* Post-pass over the notes to start the delay timers.
     */
    for (track = 0; track < xstate->xdoc->NumTracks; track++)
    {
        xrns_track_desc *TrackDesc = &xstate->xdoc->Tracks[track];
        for (col = 0; col < TrackDesc->NumColumns; col++)
        {
            xrns_sampler_bank *SamplerBank = &xstate->SamplerBanks[track][col];

            for (int i = 0; i < XRNS_MAX_SAMPLERS_PER_COLUMN; i++)
            {
                xrns_sampler *Sampler = &SamplerBank->Samplers[i];

                if (Sampler->bQReadyForCalc)
                {
                    Sampler->bQReadyForCalc = 0;
                    Sampler->bQPrepped      = 1;
                    Sampler->QCounter       = Sampler->QxValue   * xstate->CurrentTickDuration
                                            + Sampler->FracDelay * xstate->CurrentLineDuration / 256.0f;
                }
            }
        }
    }

}

void ResetEffectStatesOnColumnSamplers(XRNSPlaybackState *xstate, int track_idx, int col_idx)
{
    int i;

    xrns_sampler_bank *SamplerBank = &xstate->SamplerBanks[track_idx][col_idx];
    xrns_track_playback_state *Track = xstate->TrackStates[track_idx];

    for (i = 0; i < XRNS_MAX_SAMPLERS_PER_COLUMN; i++)
    {
        xrns_sampler *Sampler = &SamplerBank->Samplers[i];
        Sampler->bIsSliding          = 0;
        Sampler->bVolumeIsSliding    = 0;
        Sampler->bVolumeSlideStartedWithActiveNote = 0;
        Sampler->VolumeSlideDest     = 0.0f;
        Sampler->CurrentSlideDest    = 0;
        Sampler->CurrentVibratoDepth = 0;
        Sampler->CurrentVibratoSpeed = 0;
        Sampler->CurrentTremoloDepth = 0;
        Sampler->CurrentTremoloSpeed = 0;
        Sampler->SlideTick           = 0;
        Sampler->bPanningSlide       = 0;
        Sampler->bStillOnHomeRow     = 0;
        Sampler->bIsPitchGliding     = 0;
        Sampler->PitchSlideSpeed     = 0;
        Sampler->RetriggerRate       = 0;
        Sampler->CxOffset            = -1;
        Sampler->BxxValue            = -1;
        Sampler->SxxValue            = -1;
    }    
}

int HandleMissingLastEffect(int *track_value, int value)
{
    if (value != XRNS_MISSING_VALUE)
    {
        *track_value = value;
    }
    else
    {
        value = *track_value;
    }

    return value;
}

unsigned int Handle00LastEffect(unsigned int *track_value, unsigned int value)
{
    if (value != 0)
    {
        *track_value = value;
    }
    else
    {
        value = *track_value;
    }

    return value;
}

unsigned int HandleMissingAnd00LastEffect(unsigned int *track_value, unsigned int value)
{
    if (value != XRNS_MISSING_VALUE && value != 0)
    {
        *track_value = value;
    }
    else
    {
        value = *track_value;
    }

    return value;
}

/* Three cases: (Only Slice, Only Backwards, Slice & Backwards)
 */
void FinaliseEffectCommands(XRNSPlaybackState *xstate, int track_idx)
{
    int col, s, j;
    xrns_track_desc *TrackDesc = &xstate->xdoc->Tracks[track_idx];
    
    for (col = 0; col < TrackDesc->NumColumns; col++)
    {
        xrns_sampler_bank *SamplerBank = &xstate->SamplerBanks[track_idx][col];
        for (s = 0; s < XRNS_MAX_SAMPLERS_PER_COLUMN; s++)
        {
            xrns_sampler *Sampler = &SamplerBank->Samplers[s];

            int bBackwardSampleFlip = 0;
            int DigitalPercent = 0;

            if (Sampler->CxOffset != -1)
            {
                if (Sampler->bCxKill && Sampler->CxOffset == 0)
                {
                    for (j = 0; j < XRNS_MAX_INSTRUMENT_SAMPLES_PLAYING; j++)
                    {
                        xrns_sample_playback_state *PlaybackState = &Sampler->PlaybackStates[j];
                        if (Sampler->bStillOnHomeRow)
                        {
                            PlaybackState->CurrentSample = -1;    
                        }
                        else
                        {
                            if (PlaybackState->bIsCrossFading) continue;
                            PlaybackState->CrossFade = SamplerBank->NumSamplesOfXFade;
                            PlaybackState->CrossFadeDuration = PlaybackState->CrossFade;
                            PlaybackState->bIsCrossFading = 1;   
                        }   
                    }
                }
            }

            /* determine a new playback position, playback bounds, and playback direction */
            for (j = 0; j < XRNS_MAX_INSTRUMENT_SAMPLES_PLAYING; j++)
            {
                xrns_sample_playback_state *PlaybackState = &Sampler->PlaybackStates[j];

                if (!Sampler->BxxValue || (Sampler->BxxValue == XRNS_MISSING_VALUE))
                {
                    PlaybackState->PlaybackDirection = XRNS_BACKWARD;
                    bBackwardSampleFlip = 1;
                    if (PlaybackState->bPlay0Slice)
                    {
                        /* don't ask me why Renoise kills notes like this,
                         * seems like a strange edge-case. 00'th slice being hit with
                         * a reverse, even after it's started to play out.
                         */
                        PlaybackState->bPlaying = 0;
                        PlaybackState->Active = 0;
                    }
                }
                else if (Sampler->BxxValue == 1)
                {
                    PlaybackState->PlaybackDirection = XRNS_FORWARD;
                }
            }

            if (!Sampler->bStillOnHomeRow) continue;

            if (Sampler->SxxValue == XRNS_MISSING_VALUE)
                Sampler->SxxValue = 0;

            if (Sampler->SxxValue != -1)
            DigitalPercent = Sampler->SxxValue;

            if (bBackwardSampleFlip)
            {
                DigitalPercent = 0xFF - DigitalPercent;
            }

            xrns_instrument *Instrument = &xstate->xdoc->Instruments[Sampler->CurrentInstrument];
            for (j = 0; j < XRNS_MAX_INSTRUMENT_SAMPLES_PLAYING; j++)
            {
                xrns_sample_playback_state *PlaybackState = &Sampler->PlaybackStates[j];

                int CurrentSample = PlaybackState->CurrentSample;
                if (CurrentSample == -1) continue;

                xrns_sample *Sample = &Instrument->Samples[CurrentSample];

                if (Instrument->bIsSliced)
                {
                    if (Sample->bIsAlisedSample)
                    {
                        unsigned int LengthSamples;
                        xrns_sample  *BaseSample = &Instrument->Samples[0];

                        if (PlaybackState->CurrentSample == Instrument->NumSamples - 1)
                        {
                            /* take the length of the full sample */
                            LengthSamples = BaseSample->LengthSamples - Sample->SampleStart;
                        }
                        else
                        {
                            LengthSamples = Instrument->Samples[PlaybackState->CurrentSample + 1].SampleStart - Sample->SampleStart;
                        }

                        PlaybackState->PlaybackPosition = round(LengthSamples * DigitalPercent/(256.0f));

                        PlaybackState->BackPosition0  = 0.0f;
                        PlaybackState->FrontPosition0 = LengthSamples;
                    }
                    else if (Sampler->SxxValue != -1)
                    {
                        if (Sampler->SxxValue == Instrument->NumSamples)
                        {
                            DigitalPercent = 0;
                            if (bBackwardSampleFlip)
                            {
                                DigitalPercent = 0xFF - DigitalPercent;
                            }
                        }

                        /* we get here when it's the base sample, which can
                         * no longer be sliced with 0xFF steps, rather it must be
                         * the actual sample, or a slice of the sample.
                         */
                        if (Sampler->SxxValue >= Instrument->NumSamples)
                        {
                            /* renoise handles this by just pretending the Sxx
                             * wasn't specified at all.
                             */
                            unsigned int len = Sample->LengthSamples;
                            PlaybackState->PlaybackPosition = round(len * DigitalPercent/(256.0f));
                        }
                        else if (Sampler->SxxValue > 0)
                        {
                            /* this conditional is catching an odd Renoise bug whereby the only slice
                             * in a one-slice instrument won't have the right backwards end points....
                             * getting pretty obscure now!
                             */
                            if (Instrument->NumSamples != 2)
                            {
                                PlaybackState->BackPosition0 = Instrument->Samples[Sampler->SxxValue].SampleStart;    
                            }
                            
                            PlaybackState->BackPosition1 = Instrument->Samples[Sampler->SxxValue - 1].SampleStart;

                            if (Sampler->SxxValue != Instrument->NumSamples - 1)
                            {
                                PlaybackState->FrontPosition0 = Instrument->Samples[Sampler->SxxValue + 1].SampleStart;
                            }
                            
                            PlaybackState->PlaybackPosition = Instrument->Samples[Sampler->SxxValue].SampleStart;
                        }
                        else
                        {
                            PlaybackState->bPlay0Slice = 1;
                        }
                    }
                }
                else
                {
                    unsigned int len = Sample->LengthSamples;
                    PlaybackState->PlaybackPosition = round(len * DigitalPercent/(256.0f));
                }

                PlaybackState->SxxPlaybackDirection = PlaybackState->PlaybackDirection;
                PlaybackState->SxxStartPosition     = PlaybackState->PlaybackPosition;               

            }
        }
    }
}

void SetEffectCommandOnColumnSamplers(XRNSPlaybackState *xstate, int track_idx, int col_idx, xrns_note *Effect, int bMainEffectColumn)
{
    int i, j;
    int cid;

    int bCxKill = 0;

    xrns_sampler_bank *SamplerBank = &xstate->SamplerBanks[track_idx][col_idx];
    xrns_track_playback_state *Track = xstate->TrackStates[track_idx];
    
    for (cid = 0; cid <= 2; cid++)
    {
        int   EffectTypeIdx;
        int   value;

        char  bIsEffectsColumn = (cid == 2);

        unsigned int  *LastEffectValues;

        if (bIsEffectsColumn)
        {
            /* Handle regular effects in the effect columns. */
            EffectTypeIdx = Effect->EffectTypeIdx;
            value         = Effect->EffectValue;

            if (bMainEffectColumn)
            {
                LastEffectValues = &Track->LastEffectToBeUsed[XRNS_MAX_COLUMNS_PER_TRACK][0];
            }
            else
            {
                LastEffectValues = &Track->LastEffectToBeUsed[col_idx][0];
            }
        }
        else
        {
            /* Handle effects that are placed in the volume and panning columns. */
            char *FieldToLookAt;

            if (cid == 0)
            {
                FieldToLookAt    = &Effect->VolumeEffect[0]; 
                LastEffectValues = &Track->LastEffectToBeUsed[col_idx][XRNS_STORED_EFFECT_VOL];
            }
            else
            {
                FieldToLookAt    = &Effect->PanningEffect[0];
                LastEffectValues = &Track->LastEffectToBeUsed[col_idx][XRNS_STORED_EFFECT_PAN];
            }

            switch (FieldToLookAt[0])
            {
                case 'U':
                {
                    EffectTypeIdx = EFFECT_ID_0U;
                    value         = 16 * Hex2Int(FieldToLookAt[1]);
                    break;
                }

                case 'D':
                {
                    EffectTypeIdx = EFFECT_ID_0D;
                    value         = 16 * Hex2Int(FieldToLookAt[1]);
                    break;
                }

                case 'C':
                {
                    EffectTypeIdx = EFFECT_ID_0C;
                    value = Hex2Int(FieldToLookAt[1]); /* Cx in the vol/pan columns cut instantly */
                    bCxKill = 1;
                    break;
                }

                case 'J':
                {
                    EffectTypeIdx = EFFECT_ID_0J;
                    value = Hex2Int(FieldToLookAt[1]);
                    break;
                }

                case 'K':
                {
                    EffectTypeIdx = EFFECT_ID_0K;
                    value = Hex2Int(FieldToLookAt[1]);
                    break;
                }

                case 'B':
                {
                    EffectTypeIdx = EFFECT_ID_0B;
                    value = Hex2Int(FieldToLookAt[1]);
                    break;
                }

                case 'Q':
                {
                    EffectTypeIdx = EFFECT_ID_0Q;
                    value = Hex2Int(FieldToLookAt[1]);
                    break;
                }

                case 'R':
                {
                    EffectTypeIdx = EFFECT_ID_0R;
                    value = 16*8 + Hex2Int(FieldToLookAt[1]);
                    break;
                }

                case 'O':
                {
                    EffectTypeIdx = EFFECT_ID_0O;
                    value = 16*Hex2Int(FieldToLookAt[1]);
                    break;
                }

                case 'I':
                {
                    EffectTypeIdx = EFFECT_ID_0I;
                    value = 16*Hex2Int(FieldToLookAt[1]);
                    break;
                }

                case 'G':
                {
                    EffectTypeIdx = EFFECT_ID_0G;
                    value = 16*Hex2Int(FieldToLookAt[1]);

                    /* treat GF in the panning/volume columns as an instant slide */
                    if (value == 0xF0)
                    {
                        value = 0xFF;
                    }

                    break;
                }

                default:
                {
                    continue;
                    break;
                }
            }
        }

        /* Commands that store their values in the Effects column:
         * U, D, G, V, I, O, T and N => 6 values
         * 
         * Commands that store their values in the Volume column:
         * I, O, U, D, G
         *
         * Commands that store their values in the Panning column:
         * J, K, U, D, G
         *
         * U and D commands share storage. I and O commands share storage.
         * 
         * Volume command effects all share the same storage.
         * Panning command effects all share the same storage.
         */

        for (i = 0; i < XRNS_MAX_SAMPLERS_PER_COLUMN; i++)
        {
            xrns_sampler *Sampler = &SamplerBank->Samplers[i];

            switch (EffectTypeIdx)
            {
                case EFFECT_ID_0G:
                {
                    unsigned int *TrackLast = LastEffectValues;
                    if (bIsEffectsColumn)
                    {
                        TrackLast = &LastEffectValues[XRNS_STORED_EFFECT_xG];
                    }

                    value = HandleMissingAnd00LastEffect(TrackLast, value);
                    xrns_sampler *PreviousSampler = &SamplerBank->Samplers[SamplerBank->MostRecentlyPlayingSampler];

                    if ((PreviousSampler->CurrentInstrument == Sampler->CurrentInstrument) &&
                       (PreviousSampler->Active))
                    {
                        if (value == 0xFF)
                        {
                            Sampler->bInstantSlide = 1;
                        }

                        Sampler->PitchSlideSpeed += value;
                        Sampler->bIsPitchGliding = 1;

                        if (Sampler->bStillOnHomeRow)
                        {
                            Sampler->bHitWithGCommand = 1;
                        }
                    }

                    break;
                }

                case EFFECT_ID_0D:
                {
                    unsigned int *TrackLast = LastEffectValues;
                    if (bIsEffectsColumn)
                    {
                        TrackLast = &LastEffectValues[XRNS_STORED_EFFECT_DU];
                    }
                    value = HandleMissingAnd00LastEffect(TrackLast, value);
                    Sampler->bIsSliding = 1;
                    Sampler->CurrentSlideDest -= value;
                    break;
                }

                case EFFECT_ID_0U:
                {
                    unsigned int *TrackLast = LastEffectValues;
                    if (bIsEffectsColumn)
                    {
                        TrackLast = &LastEffectValues[XRNS_STORED_EFFECT_DU];
                    }
                    value = HandleMissingAnd00LastEffect(TrackLast, value);
                    Sampler->bIsSliding = 1;
                    Sampler->CurrentSlideDest += value;
                    break;
                }

                case EFFECT_ID_0O:
                {
                    unsigned int *TrackLast = LastEffectValues;
                    if (bIsEffectsColumn)
                    {
                        TrackLast = &LastEffectValues[XRNS_STORED_EFFECT_IO];
                    }
                    value = HandleMissingAnd00LastEffect(TrackLast, value);

                    Sampler->VolumeSlideDest -= value;
                    Sampler->VolumeSlideStart = Sampler->CurrentVolume.Target;
                    Sampler->bVolumeIsSliding = 1;

                    Sampler->bVolumeSlideStartedWithActiveNote = Sampler->Active;

                    break;
                }

                case EFFECT_ID_0I:
                {
                    unsigned int *TrackLast = LastEffectValues;
                    if (bIsEffectsColumn)
                    {
                        TrackLast = &LastEffectValues[XRNS_STORED_EFFECT_IO];
                    }

                    value = HandleMissingAnd00LastEffect(TrackLast, value);

                    Sampler->VolumeSlideDest += value;
                    Sampler->VolumeSlideStart = Sampler->CurrentVolume.Target;
                    Sampler->bVolumeIsSliding = 1;

                    Sampler->bVolumeSlideStartedWithActiveNote = Sampler->Active;

                    break;
                }

                case EFFECT_ID_0V:
                {
                    unsigned int *TrackLast = LastEffectValues;
                    if (bIsEffectsColumn)
                    {
                        TrackLast = &LastEffectValues[XRNS_STORED_EFFECT_xV];
                    }

                    int speed = value / 16;
                    int depth = value % 16;

                    if ((XRNS_MISSING_VALUE == value) || !speed) speed = (*TrackLast) / 16;
                    if ((XRNS_MISSING_VALUE == value) || !depth) depth = (*TrackLast) % 16;

                    *TrackLast = depth + 16*speed;

                    Sampler->CurrentVibratoSpeed += speed;
                    Sampler->CurrentVibratoDepth += depth;
                    break;
                }

                case EFFECT_ID_0T:
                {
                    unsigned int *TrackLast = LastEffectValues;
                    if (bIsEffectsColumn)
                    {
                        TrackLast = &LastEffectValues[XRNS_STORED_EFFECT_xT];
                    }

                    int speed = value / 16;
                    int depth = value % 16;

                    if ((XRNS_MISSING_VALUE == value) || !speed) speed = (*TrackLast) / 16;
                    if ((XRNS_MISSING_VALUE == value) || !depth) depth = (*TrackLast) % 16;

                    *TrackLast = depth + 16*speed;

                    Sampler->CurrentTremoloSpeed += speed;
                    Sampler->CurrentTremoloDepth += depth;

                    break;
                }

                case EFFECT_ID_0Q:
                {
                    Sampler->QxValue = value;
                    break;
                }

                case EFFECT_ID_0C:
                {
                    if (value == XRNS_MISSING_VALUE)
                    {
                        Sampler->CxVolume = 0;
                        Sampler->CxOffset = 0;
                    }
                    else
                    {
                        Sampler->CxVolume = value / 16;
                        Sampler->CxOffset = value % 16;                        
                    }

                    Sampler->bCxKill = bCxKill;

                    break;
                }

                case EFFECT_ID_0P:
                {
                    if (value == XRNS_MISSING_VALUE)
                    {
                        /* 00 is omitted, and it corresponds to hard pan left */
                        Track->CurrentPanning.Target = -1.0f;
                    }
                    else if (value == 0x80)
                    {
                        Track->CurrentPanning.Target = 0.0f;
                    }
                    else
                    {
                        Track->CurrentPanning.Target = value / ((float) 0xFF);
                    }

                    break;
                }

                case EFFECT_ID_0J:
                {
                    unsigned int *TrackLast = LastEffectValues;
                    value = Handle00LastEffect(TrackLast, value);
                    Sampler->bPanningSlide = 1;
                    Sampler->PanningSlideAmount = -1.0f * value;
                    break;
                }

                case EFFECT_ID_0K:
                {
                    unsigned int *TrackLast = LastEffectValues;
                    TrackLast = &Track->LastEffectToBeUsed[col_idx][XRNS_STORED_EFFECT_PAN];
                    value = Handle00LastEffect(TrackLast, value);
                    Sampler->bPanningSlide = 1;
                    Sampler->PanningSlideAmount = value;
                    break;
                }

                case EFFECT_ID_0B:
                {
                    Sampler->BxxValue = value;
                    break;
                }

                case EFFECT_ID_0L:
                {
                    if (value == 0 || (value == XRNS_MISSING_VALUE))
                    {
                        Track->CurrentPreVolume.Target = 0.0f;
                    }
                    else
                    {
                        float VolumedB = 0.0f;

                        if (value >= 0xC0)
                        {
                            /* 0.07357f = 3.0 / (63.0 ^ 0.895) */
                            VolumedB = powf((float)value - 192.0f, 0.895f) * 0.07357f;
                        }
                        else
                        {
                            VolumedB = -21.6034f + 10.0f * log10f((float)value * (float)value / (255.0f));
                        }

                        Track->CurrentPreVolume.Target = powf(10.0f, VolumedB / 20.0f);
                    }

                    break;
                }

                case EFFECT_ID_0R:
                {
                    Sampler->RetriggerRate = value % 16;
                    int RetriggerVolumeFactor = value / 16;

                    int NumRetrigs = ((xstate->CurrentTicksPerLine + Sampler->RetriggerRate - 1)/Sampler->RetriggerRate);

                    Sampler->WetVolume = Sampler->OriginalNote.Volume;

                    if (Sampler->bStillOnHomeRow)
                    {
                        Sampler->bRetriggerOnZero = 0;
                    }
                    else
                    {
                        Sampler->bRetriggerOnZero = 1;
                    }

                    switch (RetriggerVolumeFactor)
                    {
                        case 0x0:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 0;
                            Sampler->RetriggerVolume           = 1.0f;
                            break;
                        }
                        case 0x1:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 1;
                            Sampler->RetriggerVolume           = -1.0f/32.0f;
                            break;
                        }
                        case 0x2:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 1;
                            Sampler->RetriggerVolume           = -1.0f/16.0f;
                            break;
                        }
                        case 0x3:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 1;
                            Sampler->RetriggerVolume           = -1.0f/8.0f;
                            break;
                        }
                        case 0x4:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 1;
                            Sampler->RetriggerVolume           = -1.0f/4.0f;
                            break;
                        }
                        case 0x5:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 1;
                            Sampler->RetriggerVolume           = -1.0f/2.0f;
                            break;
                        }
                        case 0x6:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 0;
                            Sampler->RetriggerVolume           = 2.0f/3.0f;
                            break;
                        }
                        case 0x7:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 0;
                            Sampler->RetriggerVolume           = 1.0f/2.0f;
                            break;
                        }
                        case 0x8:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 0;
                            Sampler->RetriggerVolume           = 1.0f;
                            break;
                        }
                        case 0x9:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 1;
                            Sampler->RetriggerVolume           = 1.0f/32.0f;
                            break;
                        }
                        case 0xA:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 1;
                            Sampler->RetriggerVolume           = 1.0f/16.0f;
                            break;
                        }
                        case 0xB:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 1;
                            Sampler->RetriggerVolume           = 1.0f/8.0f;
                            break;
                        }
                        case 0xC:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 1;
                            Sampler->RetriggerVolume           = 1.0f/4.0f;
                            break;
                        }
                        case 0xD:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 1;
                            Sampler->RetriggerVolume           = 1.0f/2.0f;
                            break;
                        }
                        case 0xE:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 0;
                            Sampler->RetriggerVolume           = 3.0f/2.0f;
                            break;
                        }
                        case 0xF:
                        {
                            Sampler->RetriggerVolumeIsAdditive = 0;
                            Sampler->RetriggerVolume           = 2.0f;
                            break;
                        }
                        default:
                        {
                            break;
                        }
                    }
                   
                    break;
                }

                case EFFECT_ID_0M:
                {
                    if (value == XRNS_MISSING_VALUE) value = 0;
                    Sampler->CurrentVolume.Target = value;
                    break;
                }

                case EFFECT_ID_0S:
                {
                    Sampler->SxxValue = value;
                    break;
                }

                default:
                {
                    break;
                }
            }
        }
    }
}

/* This function should look at the CurrentRow and trigger all of the samples/effects
 * in the song data. It must also merge any caller notes that were added dynamically.
 */
void xrns_update_notes_and_effects(XRNSPlaybackState *xstate, int bFreshPattern)
{
    int track, m, i, j, col, a;
    unsigned int PatternIdx = xstate->xdoc->PatternSequence[xstate->CurrentPatternIndex].PatternIdx;
    xrns_pattern *Pattern = &xstate->xdoc->PatternPool[PatternIdx];
    xrns_note *UnifiedNotes = (xrns_note *) xstate->ScratchMemory;
    unsigned int NumUnifiedNotes = 0;

    /* Most effects that operate on samplers reset on new rows.
     * For instance the Vibrato command drops unless the effect
     * is re-applied on subsequent rows (often with V00 to repeat values).
     */
    for (track = 0; track < xstate->xdoc->NumTracks; track++)
    {
        xrns_track_desc *TrackDesc = &xstate->xdoc->Tracks[track];
        for (col = 0; col < TrackDesc->NumColumns; col++)
        {
            ResetEffectStatesOnColumnSamplers(xstate, track, col);
        }
    }

    for (track = 0; track < xstate->xdoc->NumTracks; track++)
    {
        NumUnifiedNotes = 0;

        xrns_track *Track = &Pattern->Tracks[track];
        xrns_track_playback_state *TrackState = xstate->TrackStates[track];

        if (Track->bIsAlias && Track->AliasIdx < xstate->xdoc->NumPatterns)
        {
            Track = &xstate->xdoc->PatternPool[Track->AliasIdx].Tracks[track];
        }
        xrns_track_desc *TrackDesc = &xstate->xdoc->Tracks[track];

        if (bFreshPattern)
        {
            /* we hit row 0 of a new pattern. */
            TrackState->CurrentNoteIndex = 0;

            /* Tracks can be muted depending on where we are in the sequence. */
            xstate->TrackStates[track]->bIsMuted = 0;
            for (m = 0; m < xstate->xdoc->PatternSequence[xstate->CurrentPatternIndex].NumMutedTracks; m++)
            {
                TrackState->bIsMuted |= (xstate->xdoc->PatternSequence[xstate->CurrentPatternIndex].MutedTracks[m] == (unsigned int) track);
            }
        }

        /* Here's where we need to use the RingIdx notes, they take priority.
         *
         * The original XRNS notes are sorted by row, but the notes from the caller may not be.
         * 
         * Build a list of notes for this row, on this track, replacing/booting
         * notes if they appear in the caller's delta.
         *
         */
        unsigned int idx = TrackState->CurrentNoteIndex;

        /* Add the original notes from the XRNS, with effects coming first. 
         */
        if ((idx != Track->NumNotes) && (Track->NumNotes != 0))
        {
            while (idx < Track->NumNotes && Track->Notes[idx].Line == xstate->CurrentRow)
            {
                xrns_note *Note = &Track->Notes[idx];
                UnifiedNotes[NumUnifiedNotes] = *Note;
                NumUnifiedNotes++;
                idx++;
            }

            TrackState->CurrentNoteIndex = idx;
        }

        /* Now sweep through with the delta notes, replacing clashes. */
        unsigned int NumOriginalNotes = NumUnifiedNotes;
#ifdef INLCUDE_FLATBUFFER_INTERFACE
        for (i = 0; i < xstate->NumCallerNotes; i++)
        {
            xrns_note_from_caller *CallerNote = &xstate->CallerNotes[i];

            if (CallerNote->Track != track)
            {
                continue;
            }

            int bNoteReplaced = 0;

            for (j = 0; j < NumOriginalNotes; j++)
            {
                if ((CallerNote->Column == UnifiedNotes[j].Column)
                    && (CallerNote->Row == UnifiedNotes[j].Line)
                    && (UnifiedNotes[j].Type != XRNS_NOTE_EFFECT))
                {
                    /* Replace. */
                    UnifiedNotes[j].Instrument = CallerNote->Instrument;

                    if (CallerNote->Value == XRNSSong_NoteValue_NOTE_BLANK)
                    {
                        UnifiedNotes[j].Note = XRNS_NOTE_BLANK;
                    }
                    else if (CallerNote->Value == XRNSSong_NoteValue_NOTE_OFF)
                    {
                        UnifiedNotes[j].Note = XRNS_NOTE_OFF;
                    }
                    else
                    {
                        UnifiedNotes[j].Note = CallerNote->Value + 12*CallerNote->Octave;    
                    }                    

                    bNoteReplaced = 1;
                    break;
                }
            }

            if (!bNoteReplaced)
            {
                xrns_note NewNote;
                InitNote(&NewNote);

                NewNote.Type       = XRNS_NOTE_REAL;
                NewNote.Column     = CallerNote->Column;
                NewNote.Line       = CallerNote->Row;
                NewNote.Instrument = CallerNote->Instrument;

                if (CallerNote->Value == XRNSSong_NoteValue_NOTE_BLANK)
                {
                    NewNote.Note = XRNS_NOTE_BLANK;
                }
                else if (CallerNote->Value == XRNSSong_NoteValue_NOTE_OFF)
                {
                    NewNote.Note = XRNS_NOTE_OFF;
                }
                else
                {
                    NewNote.Note = CallerNote->Value + 12*CallerNote->Octave;    
                }

                UnifiedNotes[NumUnifiedNotes] = NewNote;
                NumUnifiedNotes++;
            }
        }
#endif

        for (i = 0; i < NumUnifiedNotes; i++)
        {
            xrns_note *Note = &UnifiedNotes[i];

            if (Note->Type == XRNS_NOTE_EFFECT)
            {
                /* Certain effects apply to the track, DSPs, or global state.
                 * We handle these effects here.
                 *
                 * Effects that modify the playback of the notes in the particular track
                 * are skipped here, but will be accumulated across all the relevant 
                 * columns/track groups and applied at once.
                 */

                if (Note->EffectTypeIdx == EFFECT_ID_ZT)
                {
                    if (Note->EffectValue == XRNS_MISSING_VALUE)
                    {
                        xstate->CurrentBPM = 0;
                        xstate->bSongStopped = 1;
                    }
                    else
                    {
                        xstate->CurrentBPM = Note->EffectValue;
                    }

                    RecomputeDurations(xstate);
                }

                if (Note->EffectTypeIdx == EFFECT_ID_ZL)
                {
                    xstate->CurrentLinesPerBeat = Note->EffectValue;

                    if (xstate->CurrentLinesPerBeat == XRNS_MISSING_VALUE)
                        xstate->CurrentLinesPerBeat = 0;

                    if (!xstate->CurrentLinesPerBeat)
                        xstate->bSongStopped = 1;

                    RecomputeDurations(xstate);
                }
                if (Note->EffectTypeIdx == EFFECT_ID_ZK)
                {
                    if (Note->EffectValue && Note->EffectValue != XRNS_MISSING_VALUE)
                        xstate->CurrentTicksPerLine = Note->EffectValue;

                    RecomputeDurations(xstate);
                }

                /* Certain effects apply to the active samplers contained within the
                 * scope of this effect. Track effects hit all the active samplers on the track,
                 * and track effects on a track group will hit all the tracks contained.
                 * 
                 * In Renoise 3.x, notes can have effects paired with them directly. 
                 * These are handled as the note is parsed.
                 *
                 */

                if (Note->Type == XRNS_NOTE_EFFECT)
                {
                    /* Check for and handle DSP parameter changes, they have 
                     * commands that look like 1103 => DSP 1, Param 1, Value
                     */

                    if (Note->EffectTypeC[0] != '0' && Note->EffectTypeC[0] != 'Z')
                    {
                        int DSPIndex = Note->EffectTypeC[0] - '1';
                        int DSPParam = Note->EffectTypeC[1] - '0';
                        int ActualEffectValue = (Note->EffectValue == XRNS_MISSING_VALUE) ? 0 : Note->EffectValue;

                        if (DSPParam == 0)
                        {
                            /* turns the DSP effect on and off */
                            xstate->TrackStates[track]->DSPEffectEnableFlags[DSPIndex] = !!(ActualEffectValue);
                        }
                        else if (DSPIndex < xstate->xdoc->Tracks[track].NumDSPEffectUnits)
                        {
                            dsp_effect_desc *EffectDesc = &xstate->xdoc->Tracks[track].DSPEffectDescs[DSPIndex];
                            dsp_effect             *DSP = &xstate->TrackStates[track]->DSPEffects[DSPIndex];

                            if ((DSPParam - 1) < DSP->NumParameters)
                            {
                                DSP->SetParameter(DSP->State, DSPParam - 1, ActualEffectValue / 255.0f);
                            }
                        }
                    }
                    else
                    {
                        for (a = track; a >= 0 && a >= track - TrackDesc->WrapsNPreviousTracks; a--)
                        {
                            xrns_track_desc *TrackDesc = &xstate->xdoc->Tracks[a];
                            for (col = 0; col < TrackDesc->NumColumns; col++)
                            {
                                SetEffectCommandOnColumnSamplers(xstate, a, col, Note, 1);
                            }
                        }
                    }
                }
            }
            else
            {
                xrns_sampler_bank *SamplerBank = &xstate->SamplerBanks[track][Note->Column];
                if (Note->Note == XRNS_NOTE_BLANK || Note->Note == XRNS_MISSING_VALUE)
                {
                    /* Commands in the volume column behave differently depending on
                     * whether they are paired with a note or not. Volume commands 
                     * attached to a note only affect that note. Otherwise, they address
                     * _all_ the live samples in the column.
                     *
                     * If the delay column has a value inside, the vol/pan stuff is also
                     * delayed.
                     */
                    if (Note->Delay != XRNS_MISSING_VALUE && Note->Delay)
                    {
                        SamplerBlankTriggerNewNote(xstate, SamplerBank, *Note, track, NULL);
                        continue;
                    }
                    else
                    {
                        if (Note->Volume <= 0x80)
                        {
                            int s;
                            for (s = 0; s < XRNS_MAX_SAMPLERS_PER_COLUMN; s++)
                            {
                                xrns_sampler *Sampler = &SamplerBank->Samplers[s];
                                if (Sampler->Active)
                                {
                                    Sampler->CurrentVolume.Target = Note->Volume * 2u;
                                }
                            }
                        }

                        if (Note->Panning <= 0x80)
                        {
                            int s;
                            for (s = 0; s < XRNS_MAX_SAMPLERS_PER_COLUMN; s++)
                            {
                                xrns_sampler *Sampler = &SamplerBank->Samplers[s];
                                if (Sampler->Active)
                                {
                                    Sampler->CurrentPanning.Target = Note->Panning;
                                }
                            }
                        }

                        SetEffectCommandOnColumnSamplers(xstate, track, Note->Column, Note, 0);
                        continue;
                    }
                }

                SamplerBlankTriggerNewNote(xstate, SamplerBank, *Note, track, NULL);
                SetEffectCommandOnColumnSamplers(xstate, track, Note->Column, Note, 0);
            }
        }
    }

    /* some effects need to be calculated with the benefit of knowing ALL the effects */
    for (track = 0; track < xstate->xdoc->NumTracks; track++)
    {
        FinaliseEffectCommands(xstate, track);
    }

    xstate->NumCallerNotes = 0;
}

static inline double SampleLoopWrapping(xrns_sample *Sample, xrns_sample_playback_state *PlaybackState, double ProposedNewPosition)
{
    int bFullyUnwound = 0;

    while (!bFullyUnwound)
    {
        int bWrappedLoopPosition = 0;
        
        if ((PlaybackState->PlaybackDirection == XRNS_FORWARD) && (ProposedNewPosition >= Sample->LoopEnd))
        {
            bWrappedLoopPosition = 1;
        }
        else if ((PlaybackState->PlaybackDirection == XRNS_BACKWARD) && (ProposedNewPosition < Sample->LoopStart))
        {
            bWrappedLoopPosition = 1;
        }

        if (bWrappedLoopPosition)
        {
            if (PlaybackState->PlaybackDirection == XRNS_FORWARD)
            {
                double OverHang = ProposedNewPosition - Sample->LoopEnd;

                if (Sample->LoopMode == XRNS_LOOP_MODE_FORWARD)
                {
                    ProposedNewPosition = Sample->LoopStart + OverHang;
                } else if (Sample->LoopMode == XRNS_LOOP_MODE_BACKWARD)
                {
                    ProposedNewPosition = Sample->LoopEnd - OverHang;
                    PlaybackState->PlaybackDirection = XRNS_BACKWARD;
                } else if (Sample->LoopMode == XRNS_LOOP_MODE_PINGPONG)
                {
                    ProposedNewPosition = Sample->LoopEnd - OverHang;
                    PlaybackState->PlaybackDirection = XRNS_BACKWARD;
                }
            }
            else if (PlaybackState->PlaybackDirection == XRNS_BACKWARD)
            {
                double OverHang = Sample->LoopStart - ProposedNewPosition;

                if (Sample->LoopMode == XRNS_LOOP_MODE_BACKWARD)
                {
                    ProposedNewPosition = Sample->LoopEnd - OverHang;
                } else if (Sample->LoopMode == XRNS_LOOP_MODE_PINGPONG)
                {
                    ProposedNewPosition = Sample->LoopStart + OverHang;
                    PlaybackState->PlaybackDirection = XRNS_FORWARD;
                } else if (Sample->LoopMode == XRNS_LOOP_MODE_FORWARD)
                {
                    ProposedNewPosition = Sample->LoopStart + OverHang;
                    PlaybackState->PlaybackDirection = XRNS_FORWARD;
                }
            }
        }
        else
        {
            bFullyUnwound = 1;
        }
    }

    return ProposedNewPosition;
}

XRNS_DLL_EXPORT int xrns_cue_section_by_name(XRNSPlaybackState *xstate, char *SectionName)
{
    int i;

    if (!xstate) return XRNS_ERR_NULL_STATE;

    for (i = 0; i < xstate->xdoc->PatternSequenceLength; i++)
    {
        xrns_pattern_sequence_entry *Seq = &xstate->xdoc->PatternSequence[i];
        if (Seq->bIsSectionStart
            && Seq->SectionName
            && SectionName
            && !strcmp(SectionName, Seq->SectionName))
        {
            xstate->PatternHasBeenCued = 1;
            xstate->CuedPatternIndex = i;
        }
    }

    return XRNS_SUCCESS;
}

XRNS_DLL_EXPORT int xrns_cue_pattern_by_name(XRNSPlaybackState *xstate, char *PatternName)
{
    int i;

    if (!xstate) return XRNS_ERR_NULL_STATE;
    if (!PatternName) return XRNS_ERR_INVALID_TRACK_NAME;

    for (i = 0; i < xstate->xdoc->PatternSequenceLength; i++)
    {
        xrns_pattern_sequence_entry *Seq = &xstate->xdoc->PatternSequence[i];
        xrns_pattern            *Pattern = &xstate->xdoc->PatternPool[Seq->PatternIdx];

        if (Pattern->Name && !strcmp(PatternName, Pattern->Name))
        {
            xstate->PatternHasBeenCued = 1;
            xstate->CuedPatternIndex = i;
        }
    }

    return XRNS_SUCCESS;
}

XRNS_DLL_EXPORT void xrns_set_section_loop_by_name(XRNSPlaybackState *xstate, char *SectionName)
{
    int i, bFoundStart = 0;
    for (i = 0; i < xstate->xdoc->PatternSequenceLength; i++)
    {
        xrns_pattern_sequence_entry *Seq = &xstate->xdoc->PatternSequence[i];
        if (Seq->bIsSectionStart
            && Seq->SectionName
            && SectionName)
        {
            if (!strcmp(SectionName, Seq->SectionName))
            {
                xstate->PatternSequenceLoopEnd = xstate->xdoc->PatternSequenceLength - 1;
                xstate->PatternSequenceLoopStart = i;
                bFoundStart = 1;
            }
            else if (bFoundStart)
            {
                xstate->PatternSequenceLoopEnd = i - 1;
                break;
            }
        }
    }
}

XRNS_DLL_EXPORT void xrns_set_section_loop_by_pattern_names(XRNSPlaybackState *xstate, char *StartName, char *EndName)
{
    if (!xstate) return;
    if (!StartName) return;

    if (!EndName) EndName = StartName;

    int i, bFoundStart = 0;
    for (i = 0; i < xstate->xdoc->PatternSequenceLength; i++)
    {
        xrns_pattern_sequence_entry *Seq = &xstate->xdoc->PatternSequence[i];
        xrns_pattern            *Pattern = &xstate->xdoc->PatternPool[Seq->PatternIdx];

        if (Pattern->Name && !strcmp(StartName, Pattern->Name))
        {
            xstate->PatternSequenceLoopStart = i;
        }

        if (Pattern->Name && !strcmp(EndName, Pattern->Name))
        {
            xstate->PatternSequenceLoopEnd = i;
        }
    }
}

XRNS_DLL_EXPORT int32_t xrns_jump_to_pattern_by_name(XRNSPlaybackState *xstate, char *Name)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;
    if (!Name) return   XRNS_ERR_INVALID_INPUT_PARAM;

    int i;

    for (i = 0; i < xstate->xdoc->PatternSequenceLength; i++)
    {
        xrns_pattern_sequence_entry *Seq = &xstate->xdoc->PatternSequence[i];
        xrns_pattern            *Pattern = &xstate->xdoc->PatternPool[Seq->PatternIdx];

        if (Pattern->Name && !strcmp(Name, Pattern->Name))
        {
            xstate->CurrentPatternIndex = i;
            xstate->CurrentRow = 0;
            xstate->CurrentTick = 0;
            return XRNS_SUCCESS;
        }
    }

    return XRNS_ERR_TRACK_NOT_FOUND;
}

XRNS_DLL_EXPORT void xrns_set_bpm_augmentation(XRNSPlaybackState *xstate, float BPMAugmentation)
{
    if (!xstate) return;
    if (isnan(BPMAugmentation)) return;

    if (BPMAugmentation < 1.0f)  BPMAugmentation = 1.0f;
    if (BPMAugmentation > 500.0f) BPMAugmentation = 500.0f;

    xstate->CurrentBPMAugmentation = BPMAugmentation;
    RecomputeDurations(xstate);
}

XRNS_DLL_EXPORT void xrns_set_song_loop(XRNSPlaybackState *xstate, int bLoopSong)
{
    if (!xstate) return;
    xstate->bStopAtEndOfSong = !!(bLoopSong);
}

XRNS_DLL_EXPORT void xrns_free_playback_state(XRNSPlaybackState *xstate)
{
    int h, j = 0;

    FreePooledThreads(xstate->Workers);

    /* free everything created by the FLAC decoder */
    for (h = 0; h < xstate->xdoc->NumInstruments; h++)
    {
        xrns_instrument *Instrument = &xstate->xdoc->Instruments[h];
        for (j = 0; j < Instrument->NumSamples; j++)
        {
            xrns_sample *Sample = &Instrument->Samples[j];
            if (Sample->PCM && !Sample->bIsAlisedSample) ma_free(Sample->PCM, NULL);
        }
    }

    /* free the galloc'd memory */
    // VirtualFree(xstate->g->BaseAddress, 0, MEM_RELEASE);
    free(xstate->g->BaseAddress);

    /* free the galloc context itself */
    free(xstate->g);

    /* free the xrns_document */
    free(xstate->xdoc);

#ifdef INLCUDE_FLATBUFFER_INTERFACE
    /* if we serialized stuff out, free that up now */
    if (xstate->bSongHasBeenSerialized && xstate->SerializedSongMemory)
    {
        flatcc_builder_free(xstate->SerializedSongMemory);
        //free(xstate->SerializedSongMemory);

    }
#endif
    /* free output ring */
    // free(xstate->Output);
    FreeRingBuffer(&xstate->Output);

    /* free the XRNSPlaybackState */
    free(xstate);

}

XRNS_DLL_EXPORT int xrns_set_output_sample_rate(XRNSPlaybackState *xstate, float Fs)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;

    if (Fs < 8000.0f || Fs > 192000.0f)
    {
        return XRNS_ERR_INVALID_INPUT_PARAM;
    }

    xstate->OutputSampleRate  = Fs;
    xstate->NumSamplesOfXFade = floor(Fs * XRNS_XFADE_MS / (1000.0));

    return XRNS_SUCCESS;
}

void print_galloc_bytes_used(galloc_ctx *g);

XRNS_DLL_EXPORT XRNSPlaybackState * xrns_create_playback_state_from_bytes(void *p_bytes, uint32_t num_bytes)
{
    TracyCZoneN(ctx, "Create Playback", 1);

    pooled_threads_ctx *Workers = CreatePooledThreads(8);

    galloc_ctx *galloc_context = malloc(sizeof(galloc_ctx));

    if (!galloc_context)
    {
        return 0;
    }

    galloc_context->MaximumSizeBytes = Megabytes(8);
    galloc_context->BaseAddress = malloc(galloc_context->MaximumSizeBytes);
    memset(galloc_context->BaseAddress, 0, galloc_context->MaximumSizeBytes);

    galloc_context->CurrentAddress = galloc_context->BaseAddress;

    if (!galloc_context->BaseAddress)
    {
        return 0;
    }

    xrns_document *Master = malloc(sizeof(xrns_document));
    memset(Master, 0, sizeof(xrns_document));

    if (!populateXRNSDocument(galloc_context, p_bytes, (size_t) num_bytes, Master, Workers))
    {
        free(galloc_context->BaseAddress);
        free(galloc_context);
        free(Master);
        return NULL;
    }

    XRNSPlaybackState *xplay = malloc(sizeof(XRNSPlaybackState));
    memset(xplay, 0, sizeof(XRNSPlaybackState));

    CreateXRNSPlaybackState(galloc_context, xplay, Master, 48000.0f);

    xplay->Workers = Workers;
    xplay->g = galloc_context;

    TracyCZoneEnd(ctx);

    unsigned long long BytesUsed = galloc_context->CurrentAddress - galloc_context->BaseAddress;
    printf("GALLOC Using %llu Bytes (%.2f MBytes), the maximum is %.2f\n", BytesUsed, BytesUsed / ((float) Megabytes(1)), galloc_context->MaximumSizeBytes / ((float) Megabytes(1)));
    if (BytesUsed > galloc_context->MaximumSizeBytes)
    {
        return NULL;
    }   

    return xplay;
}

XRNS_DLL_EXPORT XRNSPlaybackState * xrns_create_playback_state(char *p_filename)
{
    TracyCZoneN(ctx, "Create Playback From File", 1);
    void *masterXRNS;
    long masterXRNSSize;
    masterXRNS = xrns_read_entire_file(p_filename, &masterXRNSSize);
    XRNSPlaybackState *RetState = xrns_create_playback_state_from_bytes(masterXRNS, masterXRNSSize);
    free(masterXRNS);
    TracyCZoneEnd(ctx);
    return RetState;
}

int run_engine(XRNSPlaybackState *xstate, int bExitingAfterTick, int bExitingAfterLine, int bExitingBeforeLine, int MaximumSamples)
{
    TracyCZoneN(main_ctx, "Run Engine", 1);

    int return_code = XRNS_SUCCESS;

    int bTimeToExit = 0;
    int SamplesGenerated = 0;

    if (xstate->Output.RingBufferFreeSamples == 0) bTimeToExit = 1;

    if (xstate->bSongStopped)
    {
        float __x = 0.0f;
        for (int x = 0; x < MaximumSamples; x++)
            PushRingBuffer(&xstate->Output, &__x, 1);
        return return_code;
    }

    if (xstate->bEvenEarlierFirstPlay)
    {
        if (xstate->PatternHasBeenCued)
        {
            /* if a pattern was cued before playing began, we should jump there first. */
            xstate->CurrentPatternIndex = xstate->CuedPatternIndex;
            xstate->PatternHasBeenCued = 0;
        }

        xstate->CurrentBPM          = xstate->xdoc->BeatsPerMin;
        xstate->CurrentLinesPerBeat = xstate->xdoc->LinesPerBeat;
        xstate->CurrentTicksPerLine = xstate->xdoc->TicksPerLine;
        xstate->CurrentSample       = 0;
        xstate->BaseOfCurrentlyPlayingLine = 0.0;
        xstate->bEvenEarlierFirstPlay = 0;

        RecomputeDurations(xstate);

        if (bExitingBeforeLine)
        {
            bTimeToExit = 1;
            return XRNS_WOULD_WRAP_ROW;
        }
    }

    if (xstate->bFirstPlay)
    {
        /* update notes, instruments, etc .. */
        /* evaluate all effect changes, including tempo! */
        xrns_update_notes_and_effects(xstate, 1);
        xrns_perform_tick_processing(xstate); /* always a tick on a line */

        RecomputeDurations(xstate);

        double DurationOfThisLine  = xstate->CurrentLineDuration;
        xstate->LocationOfNextLine = xstate->XRNSGridOffset + DurationOfThisLine;
        xstate->LocationOfNextTick = xstate->CurrentTickDuration;
        xstate->bFirstPlay = 0;

        if (bExitingAfterLine || bExitingAfterTick)
        {
            bTimeToExit = 1;
        }
    }

    while(!bTimeToExit)
    {
        int i;
        float *p_samples = &xstate->Output.OutputRingBuffer[2 * xstate->Output.RingBufferWritePtr];

        p_samples[0] = 0.0f;
        p_samples[1] = 0.0f;

        int CurrentLevel = xstate->xdoc->Tracks[0].Depth;

        float TempBuffers[XRNS_MAX_NESTING_DEPTH][2] = {0};

        /* Generate a sample into the output buffer. */
        for (int track = 0; track < xstate->xdoc->NumTracks; track++)
        {   
            float *tsample;
            float  Dry[2];

            TracyCZoneN(ctx, "Track Preamble", 1);

            xrns_track_playback_state *Track = xstate->TrackStates[track];
            xrns_track_desc      *TrackDesc2 = &xstate->xdoc->Tracks[track];
            unsigned int PatternIdx = xstate->xdoc->PatternSequence[xstate->CurrentPatternIndex].PatternIdx;
            xrns_pattern *Pattern = &xstate->xdoc->PatternPool[PatternIdx];
            xrns_track *TrackData = &Pattern->Tracks[track];

            Dry[0] = 0.0f;
            Dry[1] = 0.0f;

            double DurationOfThisLine2 = xstate->CurrentLineDuration;
            float ProgressThroughPatternIn256thRows = xstate->CurrentRow * 256;
            ProgressThroughPatternIn256thRows += 256.0 - (256 * (xstate->LocationOfNextLine - xstate->CurrentSample) / DurationOfThisLine2);

            /* handle the track automation curves. */
            for (i = 0; i < TrackData->NumEnvelopes; i++)
            {
                xrns_envelope *Envelope = &TrackData->Envelopes[i];

                if (Envelope->NumPoints == 0) continue;

                if (Envelope->DeviceIndex == 0)
                {
                    /* this is the standard Renoise device */
                    switch (Envelope->ParameterIndex)
                    {
                        case 1: /* panning */
                        {
                            double v = WalkEnvelopeStateless(Envelope, ProgressThroughPatternIn256thRows);
                            Track->CurrentPanning.Target = 2.0 * v - 1.0;
                            break;
                        }
                        case 2: /* volume */
                        {
                            double v = WalkEnvelopeStateless(Envelope, ProgressThroughPatternIn256thRows) * 1.414f;
                            Track->CurrentPreVolume.Target = v;
                            break;
                        }
                        case 3: /* width (ignored) */
                        {
                            break;
                        }
                    }
                }
                else
                {
                    int DSPIndex = Envelope->DeviceIndex - 1;
                    /* this will be a track FX unit */
                    if (DSPIndex >= 0 && DSPIndex < TrackDesc2->NumDSPEffectUnits)
                    {
                        dsp_effect_desc *EffectDesc = &xstate->xdoc->Tracks[track].DSPEffectDescs[DSPIndex];
                        dsp_effect             *DSP = &xstate->TrackStates[track]->DSPEffects[DSPIndex];

                        if (Envelope->ParameterIndex - 1 < DSP->NumParameters)
                        {
                            double v = WalkEnvelopeStateless(Envelope, ProgressThroughPatternIn256thRows);
                            DSP->SetParameter(DSP->State, Envelope->ParameterIndex - 1, v);
                        }
                    }
                }
            }

            RunLerp(&Track->CurrentPanning);
            RunLerp(&Track->CurrentPreVolume);
            RunLerp(&Track->CurrentGamePostVolume);

            if (CurrentLevel < xstate->xdoc->Tracks[track].Depth)
            {
                /* Clear into the next depth down, ready for future summing...
                 */
                CurrentLevel = xstate->xdoc->Tracks[track].Depth;
                TempBuffers[CurrentLevel][0] = 0.0f;
                TempBuffers[CurrentLevel][1] = 0.0f;
            }
            else if (CurrentLevel > xstate->xdoc->Tracks[track].Depth)
            {
                /* When ascending up one level, sum the result from the 
                 * current depth up to the new depth.
                 */
                TempBuffers[xstate->xdoc->Tracks[track].Depth][0] += TempBuffers[CurrentLevel][0];
                TempBuffers[xstate->xdoc->Tracks[track].Depth][1] += TempBuffers[CurrentLevel][1];
                CurrentLevel = xstate->xdoc->Tracks[track].Depth;
            }

            tsample = TempBuffers[CurrentLevel];

            TracyCZoneEnd(ctx);

            for (int col = 0; col < xstate->xdoc->Tracks[track].NumColumns; col++)
            {
                int SampleIndex = -1;
                int BaseNote = -1;

                xrns_sampler_bank *SamplerBank = &xstate->SamplerBanks[track][col];

                for (int s = 0; s < XRNS_MAX_SAMPLERS_PER_COLUMN; s++)
                {
                    xrns_sampler *Sampler = &SamplerBank->Samplers[s];

                    if (Sampler->QCounter)
                    {
                        Sampler->QCounter--;
                    }

                    if (!Sampler->QCounter && Sampler->bQPrepped)
                    {
                        Sampler->bQPrepped = 0;

                        if (Sampler->OriginalNote.Note == XRNS_NOTE_BLANK || Sampler->OriginalNote.Note == XRNS_MISSING_VALUE)
                        {
                            if (Sampler->OriginalNote.Volume <= 0x80)
                            {
                                int s;
                                for (s = 0; s < XRNS_MAX_SAMPLERS_PER_COLUMN; s++)
                                {
                                    xrns_sampler *Sampler2 = &SamplerBank->Samplers[s];
                                    if (Sampler2->Active)
                                    {
                                        Sampler2->CurrentVolume.Target = Sampler->OriginalNote.Volume * 2u;
                                    }
                                }
                            }

                            if (Sampler->OriginalNote.Panning <= 0x80)
                            {
                                int s;
                                for (s = 0; s < XRNS_MAX_SAMPLERS_PER_COLUMN; s++)
                                {
                                    xrns_sampler *Sampler2 = &SamplerBank->Samplers[s];
                                    if (Sampler2->Active)
                                    {
                                        Sampler2->CurrentPanning.Target = Sampler->OriginalNote.Panning;
                                    }
                                }
                            }

                            SetEffectCommandOnColumnSamplers(xstate, track, Sampler->OriginalNote.Column, &Sampler->OriginalNote, 0);
                        }
                        else
                        {
                            if (Sampler->bHitWithGCommand)
                            {
                                SamplerBank->Samplers[SamplerBank->MostRecentlyPlayingSampler].CurrentVolume.Target = Sampler->CurrentVolume.Target;
                                SamplerBank->Samplers[SamplerBank->MostRecentlyPlayingSampler].CurrentPanning.Target = Sampler->CurrentPanning.Target;
                            }
                            else
                            {
                                PerformNewNoteActionOnSamplerBank(xstate, xstate->xdoc, SamplerBank, Sampler->bIsNoteOff);

                                if (Sampler->PlaybackStates[0].CurrentSample != -1)
                                {
                                    Sampler->Active   = 1;
                                    Sampler->bPlaying = (!Sampler->bIsNoteOff);
                                    for (int j = 0; j < XRNS_MAX_INSTRUMENT_SAMPLES_PLAYING; j++)
                                    {
                                        xrns_sample_playback_state *PlaybackState = &Sampler->PlaybackStates[j];
                                        if (PlaybackState->bMapped)
                                        {
                                            PlaybackState->Active = Sampler->Active;
                                            PlaybackState->bPlaying = Sampler->bPlaying;
                                        }
                                    }
                                }

                                SamplerBank->MostRecentlyPlayingSampler = s;
                            }
                        }
                    }

                    if (!Sampler->bPlaying) continue;

                    int j;
                    for (j = 0; j < XRNS_MAX_INSTRUMENT_SAMPLES_PLAYING; j++)
                    {
                        xrns_sample_playback_state *PlaybackState = &Sampler->PlaybackStates[j];
                        if (!PlaybackState->bPlaying) continue;
                        SampleIndex = PlaybackState->CurrentSample;
                        if (SampleIndex == -1) continue; /* in this condition, the note simply isn't mapped to any sample. */

                        xrns_instrument *Instrument = &xstate->xdoc->Instruments[Sampler->CurrentInstrument];
                        BaseNote = PlaybackState->CurrentBaseNote;

                        PlaybackState->SamplesPlayedFor++;

                        xrns_sample * Sample = &Instrument->Samples[SampleIndex];

                        int16_t *pcm               = Sample->PCM;
                        int SampleRateHz           = Sample->SampleRateHz;
                        int NumChannels            = Sample->NumChannels;
                        int LengthSamples          = Sample->LengthSamples;

                        /* supposed to be the length of the whole sample, if it's a sliced sample, 
                         * it should be the length of the base sample.
                         */
                        int MaxLengthSamples       = Sample->LengthSamples;

                        int bSampleIsPlayingLoopRelease = (PlaybackState->bIsCrossFading && Sample->LoopRelease);

                        if (Sample->bIsAlisedSample || PlaybackState->bPlay0Slice)
                        {
                            pcm           = Instrument->Samples[0].PCM;
                            SampleRateHz  = Instrument->Samples[0].SampleRateHz;
                            NumChannels   = Instrument->Samples[0].NumChannels;

                            MaxLengthSamples = Instrument->Samples[0].LengthSamples;

                            if (SampleIndex == Instrument->NumSamples - 1)
                            {
                                /* take the length of the full sample */
                                LengthSamples = MaxLengthSamples - Sample->SampleStart;
                            }
                            else
                            {
                                LengthSamples = Instrument->Samples[SampleIndex + 1].SampleStart - Sample->SampleStart;
                            }
                        }

                        if (!pcm) continue; /* no actual PCM data was loaded for this instrument ... */

                        TracyCZoneN(ctxx, "Sampler Playback", 1);

                        /* Every sample can have a modulation set attached.
                         */

                        /* Volume envelope. @Optimization
                         *       Points on the automation curves can be on 1/256ths of a beat, or on 1ms time.
                         *       Also, according to the plots, the actual curves are only sampled at that
                         *       resolution as well. Linear interpolation appears to be used inbetween.
                         *       So we have to at least call this every sample, to get the linearly interpolated values,
                         *       however the actual curve evaluations don't need to happen that often.
                         */
                        float VolumePercent = 1.0f;
                        int bOnLastEnvelopePoint = 0;

                        if (Instrument->NumModulationSets && (Sample->ModulationSetIndex != -1))
                        {
                            xrns_modulation_set *ModulationSet = &Instrument->ModulationSets[Sample->ModulationSetIndex];

                            xrns_envelope *Envelope = &ModulationSet->Volume;
                            if (Envelope && ModulationSet->bVolumeEnvelopePresent)
                            {
                                VolumePercent = WalkEnvelope(xstate, Envelope, &PlaybackState->VolumeEnvelope, 1.0/(xstate->OutputSampleRate), PlaybackState->bIsCrossFading);
                                bOnLastEnvelopePoint = OnLastEnvelopePoint(Envelope, &PlaybackState->VolumeEnvelope);
                            }
                        }

                        // @Optimization: I've stuck the scaling factor for int to float here.
                        const float RenoiseOutputGain = 0.5011872336272722f * XRNS_16BIT_CONVERTION;
                        float CrossFadeLevel = RenoiseOutputGain
                                             * xstate->TrackStates[xstate->xdoc->NumTracks-1]->CurrentPreVolume.Val
                                             * VolumePercent;

                        if (Sampler->CurrentTremoloDepth)
                        {
                            CrossFadeLevel *= fabs(Sampler->TremoloAmount);
                        }
                        
                        float IntroCrossFadeLevel = 1.0f;

                        if (PlaybackState->bIsCrossFading)
                        {
                            if (PlaybackState->CrossFadeDuration == 0)
                            {
                                CrossFadeLevel *= 0.0f;
                            }
                            else if (PlaybackState->CrossFadeDuration == -1)
                            {
                                /* infinite */
                                CrossFadeLevel *= 1.0f;
                            }
                            else
                            {
                                CrossFadeLevel *= PlaybackState->CrossFade / ((float) PlaybackState->CrossFadeDuration);
                            }

                            /* If we are out of envelope data, and the crossfade gets really low, 
                             * we can just chop the note.
                             */
                            if (bOnLastEnvelopePoint && CrossFadeLevel < 1e-9f)
                            {
                                PlaybackState->CrossFade = 0;
                            }
                        }

                        if (PlaybackState->bIntroIsCrossFading)
                        {
                            if (PlaybackState->IntroCrossFadeDuration == 0)
                            {
                                IntroCrossFadeLevel *= 0.0f;
                            }
                            else
                            {
                                IntroCrossFadeLevel *= PlaybackState->IntroCrossFade / ((float) PlaybackState->IntroCrossFadeDuration);
                            }
                        }                    

                        CrossFadeLevel *= IntroCrossFadeLevel;

                        CrossFadeLevel *= xstate->TrackStates[track]->CurrentPreVolume.Val;

                        if (xstate->TrackStates[track]->bIsMuted)
                            CrossFadeLevel = 0.0f;

                        CrossFadeLevel *= Sample->Volume;

                        /* Various panning gains are possible to see here.
                         *
                         * 1. Samples may have a static pan value (-50 <-> 50) set. (values in the XRNS file will be -1.0 vs. 1.0)
                         * 2. Samples may have a panning modulation curve that maps into the range
                         *    (-50 <-> 50).
                         * 3. Samplers may have a current panning effect applied in the pattern data. This
                         *    may only be set in the panning column, and can have set/adjust. These are set in 
                         *    steps of 64 (0x00 <-> 0x80).
                         * 4. Tracks may have a panning value set pre-DSP chain. This is automated with xJxx. 
                         *    This is also from (-50 <-> 50).
                         * 5. Tracks may also have a post panning value. (-50 <-> 50)
                         *
                         * These all map to gains of +3dB to -inf, and stack multiplicatively.
                         */

                        /* Handle panning from sources 1, 2, 3, 4 */
                        // @Optimization: Maybe don't do these every sample?
                        xrns_panning_gains SamplePan     = PanningGainFromZeroToOne(Sample->Panning);
                        xrns_panning_gains ModulationPan = {1.0f, 1.0f};
                        xrns_panning_gains SamplerPan    = PanningGainFromColNumber(Sampler->CurrentPanning.Val);
                        xrns_panning_gains TrackPan      = PanningGainFromNeg1To1(xstate->TrackStates[track]->CurrentPanning.Val);

                        RunLerp(&Sampler->CurrentPanning);
                        RunLerp(&Sampler->CurrentVolume);

                        if (Instrument->NumModulationSets && (Sample->ModulationSetIndex != -1))
                        {
                            xrns_modulation_set *ModulationSet = &Instrument->ModulationSets[Sample->ModulationSetIndex];
                            xrns_envelope *Envelope = &ModulationSet->Panning;

                            if (Envelope && ModulationSet->bPanningEnvelopePresent)
                            {
                                float PanningEnvelopeValue = WalkEnvelope(xstate, Envelope, &PlaybackState->PanningEnvelope, 1.0/(xstate->OutputSampleRate), PlaybackState->bIsCrossFading);
                                ModulationPan = PanningGainFromZeroToOne(PanningEnvelopeValue);
                            }
                        }

                        float LeftPanGain = SamplePan.Left * ModulationPan.Left * SamplerPan.Left * TrackPan.Left;
                        float RightPanGain = SamplePan.Right * ModulationPan.Right * SamplerPan.Right * TrackPan.Right;

                        if (Sample->InterpolationMode == XRNS_INTERPOLATION_NONE)
                        {
                            unsigned int pbsample = round(PlaybackState->PlaybackPosition);

                            if (NumChannels == 2)
                            {
                                Dry[0] += LeftPanGain * (XRNS_ACCESS_STEREO_SAMPLE(2 * pbsample + 0) * ((CrossFadeLevel * ((float) Sampler->CurrentVolume.Val)) / 255.0f));
                                Dry[1] += RightPanGain * (XRNS_ACCESS_STEREO_SAMPLE(2 * pbsample + 1) * ((CrossFadeLevel * ((float) Sampler->CurrentVolume.Val)) / 255.0f));                    
                            }
                            else
                            {
                                Dry[0] += LeftPanGain * (XRNS_ACCESS_MONO_SAMPLE(pbsample) * ((CrossFadeLevel * ((float) Sampler->CurrentVolume.Val)) / 255.0f));
                                Dry[1] += RightPanGain * (XRNS_ACCESS_MONO_SAMPLE(pbsample) * ((CrossFadeLevel * ((float) Sampler->CurrentVolume.Val)) / 255.0f));
                            }
                        } 
                        else if (   Sample->InterpolationMode == XRNS_INTERPOLATION_LINEAR
                                 || Sample->InterpolationMode == XRNS_INTERPOLATION_CUBIC)
                        {
                            float BaseSampleL;
                            float BaseSampleR;
                            float NextSampleL;
                            float NextSampleR;

                            unsigned int OffsetIntoSample = Instrument->Samples[PlaybackState->CurrentSample].SampleStart;

                            float base_sample_floating;
                            unsigned int base_sample;
                            float alpha;

                            if (PlaybackState->PlaybackDirection == XRNS_FORWARD)
                            {
                                base_sample_floating = floor(PlaybackState->PlaybackPosition);
                                base_sample = (unsigned int) base_sample_floating;
                                alpha = PlaybackState->PlaybackPosition - base_sample_floating;
                            }
                            else
                            {
                                base_sample_floating = ceil(PlaybackState->PlaybackPosition);
                                base_sample = (unsigned int) base_sample_floating;
                                alpha = base_sample_floating - PlaybackState->PlaybackPosition;
                            }

                            unsigned int next_sample;

                            if (bSampleIsPlayingLoopRelease || Sample->LoopMode == XRNS_LOOP_MODE_OFF)
                            {
                                if (PlaybackState->PlaybackDirection == XRNS_FORWARD)
                                {
                                    if (base_sample == LengthSamples - 1)
                                        next_sample = base_sample;
                                    else 
                                        next_sample = base_sample + 1;                                
                                }
                                else
                                {
                                    if (base_sample == 0)
                                        next_sample = base_sample;
                                    else 
                                        next_sample = base_sample - 1;
                                }
                            }
                            else
                            {
                                if (PlaybackState->PlaybackDirection == XRNS_FORWARD)
                                {
                                    next_sample = base_sample + 1;
                                }
                                else
                                {
                                    next_sample = base_sample - 1;
                                }

                                next_sample = (unsigned int) SampleLoopWrapping(Sample, PlaybackState, next_sample);
                            }                            
                            
                            if (NumChannels == 2)
                            {
                                BaseSampleL = XRNS_ACCESS_STEREO_SAMPLE(2 * (OffsetIntoSample + base_sample) + 0);
                                BaseSampleR = XRNS_ACCESS_STEREO_SAMPLE(2 * (OffsetIntoSample + base_sample) + 1);

                                if (next_sample >= LengthSamples)
                                {
                                    NextSampleL = 0.0f;
                                    NextSampleR = 0.0f;
                                }
                                else
                                {
                                    NextSampleL = XRNS_ACCESS_STEREO_SAMPLE(2 * (OffsetIntoSample + next_sample) + 0);
                                    NextSampleR = XRNS_ACCESS_STEREO_SAMPLE(2 * (OffsetIntoSample + next_sample) + 1);
                                }                        
                            }
                            else
                            {
                                BaseSampleL = XRNS_ACCESS_MONO_SAMPLE(OffsetIntoSample + base_sample);

                                if (next_sample >= LengthSamples)
                                {
                                    NextSampleL = 0.0f;
                                }
                                else
                                {
                                    NextSampleL = XRNS_ACCESS_MONO_SAMPLE(OffsetIntoSample + next_sample);
                                }
                            }

                            if (NumChannels == 2)
                            {
                                Dry[0] += LeftPanGain * ((BaseSampleL * (1.0f - alpha) + NextSampleL * alpha) * ((CrossFadeLevel * ((float) Sampler->CurrentVolume.Val)) / 255.0f));
                                Dry[1] += RightPanGain * ((BaseSampleR * (1.0f - alpha) + NextSampleR * alpha) * ((CrossFadeLevel * ((float) Sampler->CurrentVolume.Val)) / 255.0f));                    
                            }
                            else
                            {
                                float v = ((BaseSampleL * (1.0f - alpha) + NextSampleL * alpha) * ((CrossFadeLevel * ((float) Sampler->CurrentVolume.Val)) / 255.0f));
                                Dry[0] += LeftPanGain * v;
                                Dry[1] += RightPanGain * v;
                            }
                        }

                        /* Note - BaseNote = the relative pitch to apply to the sample
                         * Instead of stepping by 1 sample, we need to step by the tuning..
                         *
                         * 2.0^((AbsoluatePitchOnPiano - DesiredTone)/12);
                         *
                         * Since the sample is "at" the AbsoluatePitchOnPiano, and the BaseNote is set
                         * to this number, and we know DesiredTone, we just need to look this function up
                         * in a pitching table. 
                         *
                         */

                        /* Piggyback on this for the different sample rates.
                         */

                        /* Fine pitch is effected by a combination of note effects, modulation curves, 
                         * and instrument settings.
                         *
                         * If the sum of these pitch adjustments is larger than one semitone, we adjust our 
                         * index into the pitch table. The fractional remainder is then lerped between
                         * adjacent values in the table.
                         */

                        int IntegerPartOfPitch = (Sampler->CurrentNote - BaseNote + Sample->Transpose);
                        double PitchAdjustment = (Sampler->GlideNote / 16.0) + (Sampler->CurrentSlideOffset / 16.0);

                        /* Pitch envelopes also apply.
                         */
                        double PitchEnvelopeValue = 1.0f;

                        if (Instrument->NumModulationSets && (Sample->ModulationSetIndex != -1))
                        {
                            xrns_modulation_set *ModulationSet = &Instrument->ModulationSets[Sample->ModulationSetIndex];
                            xrns_envelope *Envelope = &ModulationSet->Pitch;

                            if (Envelope && ModulationSet->bPitchEnvelopePresent)
                            {
                                PitchEnvelopeValue = ((double) ModulationSet->PitchModulationRange)
                                                   * (2.0f * WalkEnvelope(xstate, Envelope, &PlaybackState->PitchEnvelope, 1.0/(xstate->OutputSampleRate), PlaybackState->bIsCrossFading) - 1.0f);
                                PitchAdjustment   += PitchEnvelopeValue;
                            }
                        }

                        if (!Sample->BeatSyncIsActive)
                        {
                            PitchAdjustment += (Sample->Finetune / 128.0);
                        }

                        if (Sampler->CurrentVibratoDepth != 0)
                        {
                            PitchAdjustment += (Sampler->VibratoOffset / 100.0f);
                        }

                        /* Get the Hz of the sample if it is played back as the basenote.
                         * Apply the key, transpose, and everything else ...
                         */

                        float OriginalHz = NoteToHzAssumingA440(BaseNote);

                        if (PitchAdjustment > 1.0)
                        {
                            IntegerPartOfPitch += floor(PitchAdjustment);
                            PitchAdjustment    -= floor(PitchAdjustment);
                        }
                        else if (PitchAdjustment < -1.0)
                        {
                            IntegerPartOfPitch += -floor(-PitchAdjustment);
                            PitchAdjustment    +=  floor(-PitchAdjustment);
                        }

                        int PitchTableIdx = 96 + IntegerPartOfPitch;

                        if (PitchTableIdx < 0)                       
                            PitchTableIdx = 0;
                        if (PitchTableIdx >= PITCHING_TABLE_LENGTH)
                            PitchTableIdx = PITCHING_TABLE_LENGTH - 1;

                        double KeyPitch = PitchingTable[PitchTableIdx];

                        if (Sample->BeatSyncIsActive)
                        {
                            /* Work out the re-pitch....  */
                            double DurationOfBeatSyncLines = Sample->BeatSyncLines * xstate->CurrentLineDuration / xstate->OutputSampleRate;
                            KeyPitch = ((double) LengthSamples / (DurationOfBeatSyncLines * ((double)SampleRateHz)));
                        }

                        if (PitchAdjustment > 0 && PitchTableIdx < PITCHING_TABLE_LENGTH - 1)
                        {
                            KeyPitch = (KeyPitch * (1.0 - PitchAdjustment)) + (PitchAdjustment) * PitchingTable[PitchTableIdx + 1];
                        }
                        else if (PitchAdjustment < 0 && PitchTableIdx > 0)
                        {
                            KeyPitch = (KeyPitch * (1.0 + PitchAdjustment)) + (-PitchAdjustment) * PitchingTable[PitchTableIdx - 1];
                        }                        

                        double dt = KeyPitch * ((double)SampleRateHz / xstate->OutputSampleRate);

                        Sampler->SavedPitchMod = OriginalHz * dt;

                        double PrevPos = PlaybackState->PlaybackPosition;

                        if (PlaybackState->PlaybackDirection == XRNS_FORWARD)
                        {
                            PlaybackState->PlaybackPosition += dt;
                        }
                        else
                        {
                            PlaybackState->PlaybackPosition -= dt;
                        }

                        if (!bSampleIsPlayingLoopRelease && Sample->LoopMode != XRNS_LOOP_MODE_OFF)
                        {
                            PlaybackState->PlaybackPosition = SampleLoopWrapping(Sample, PlaybackState, PlaybackState->PlaybackPosition);
                        }

                        /* exiting the sample boundaries always ends the note, sample looping
                         * will keep the playhead in-bounds.
                         */
                        if (bSampleIsPlayingLoopRelease || Sample->LoopMode == XRNS_LOOP_MODE_OFF)
                        {
                            if (PlaybackState->PlaybackDirection == XRNS_FORWARD)
                            {
                                if (PrevPos < PlaybackState->FrontPosition0 && PlaybackState->PlaybackPosition >= PlaybackState->FrontPosition0)
                                {
                                    PlaybackState->bPlaying = 0;
                                    PlaybackState->Active = 0;
                                }
                            } else if (PlaybackState->PlaybackDirection == XRNS_BACKWARD)
                            {
                                if (PrevPos > PlaybackState->BackPosition0 && PlaybackState->PlaybackPosition <= PlaybackState->BackPosition0)
                                {
                                    PlaybackState->bPlaying = 0;
                                    PlaybackState->Active = 0;
                                }
                                if (PrevPos > PlaybackState->BackPosition1 && PlaybackState->PlaybackPosition <= PlaybackState->BackPosition1)
                                {
                                    PlaybackState->bPlaying = 0;
                                    PlaybackState->Active = 0;
                                }
                            }
                        }

                        if (PlaybackState->bIsCrossFading)
                        {
                            if (PlaybackState->CrossFade > 0)
                            {
                                PlaybackState->CrossFade--;
                            }
                            else
                            {
                                PlaybackState->bPlaying = 0;
                                PlaybackState->bIsCrossFading = 0;
                                PlaybackState->Active = 0;
                            }
                        }

                        if (PlaybackState->bIntroIsCrossFading)
                        {
                            if (PlaybackState->IntroCrossFade < PlaybackState->IntroCrossFadeDuration)
                            {
                                PlaybackState->IntroCrossFade++;
                            }
                            else
                            {
                                PlaybackState->bIntroIsCrossFading = 0;
                            }
                        }

                        int bAllDone = 1;
                        for (int jj = 0; jj < XRNS_MAX_INSTRUMENT_SAMPLES_PLAYING; jj++)
                        {
                            if (Sampler->PlaybackStates[jj].bPlaying || Sampler->PlaybackStates[jj].Active)
                                bAllDone = 0;
                        }
                        if (bAllDone)
                        {
                            Sampler->bPlaying = 0;
                            Sampler->Active = 0;
                        }

                        TracyCZoneEnd(ctxx);
                    }                    
                }
            }

            xrns_track_desc *TrackDesc = &xstate->xdoc->Tracks[track];
            xrns_panning_gains PostTrackPan = PanningGainFromZeroToOne(TrackDesc->PostPanning);
            float                PostVolume = xstate->TrackStates[track]->CurrentGamePostVolume.Val
                                            * xstate->TrackStates[track]->CurrentPostVolume;
            
            /* sum all the nested tracks together for this group track */
            if (TrackDesc->bIsGroup)
            {
                int _trackIndex;
                Dry[0] = 0.0f;
                Dry[1] = 0.0f;
                
                for (_trackIndex = 1; _trackIndex <= TrackDesc->WrapsNPreviousTracks; _trackIndex++)
                {
                    if (track - _trackIndex < 0) continue;

                    xrns_track_desc       *PrevTrackDesc = &xstate->xdoc->Tracks[track - _trackIndex];
                    xrns_track_playback_state *PrevTrack = xstate->TrackStates[track - _trackIndex];

                    int PrevIdx = PrevTrack->RawAudio.RingBufferWritePtr;
                    if (!PrevIdx)
                    {
                        PrevIdx = PrevTrack->RawAudio.RingBufferSz - 1;
                    }
                    else
                    {
                        PrevIdx--;   
                    }

                    Dry[0] += PrevTrack->RawAudio.OutputRingBuffer[2*PrevIdx + 0];
                    Dry[1] += PrevTrack->RawAudio.OutputRingBuffer[2*PrevIdx + 1];

                    if (PrevTrackDesc->bIsGroup)
                    {
                        /* group already captured all the sub-track's audio */
                        _trackIndex += PrevTrackDesc->WrapsNPreviousTracks;
                    }
                }
            }

            /* Now that all the columns have summed their stuff into tsample, we run it through the effect chain
             * before summing it into the output.
             */
            for (int effect = 0; effect < xstate->xdoc->Tracks[track].NumDSPEffectUnits; effect++)
            {
                if (!xstate->TrackStates[track]->DSPEffectEnableFlags[effect])
                {
                    continue;
                }

                float *temp[2];
                temp[0] = &Dry[0];
                temp[1] = &Dry[1];

                dsp_effect *DSPEffect = &xstate->TrackStates[track]->DSPEffects[effect];
                DSPEffect->Process(DSPEffect->State, &temp[0], &temp[0], 1);
            }

            Dry[0] *= PostTrackPan.Left  * PostVolume;
            Dry[1] *= PostTrackPan.Right * PostVolume;

            /* For this track, commit the samples into the ringbuffer.
             */
            PushRingBuffer(&xstate->TrackStates[track]->RawAudio, Dry, 1);
        }

        xrns_track_playback_state *MasterTrack = xstate->TrackStates[xstate->xdoc->NumTracks-1];

        int PrevIdx2 = MasterTrack->RawAudio.RingBufferWritePtr;
        if (!PrevIdx2)
        {
            PrevIdx2 = MasterTrack->RawAudio.RingBufferSz - 1;
        }
        else
        {
            PrevIdx2--;
        }

        /* limiter here! */
        xstate->CurrentSample++;

        PushRingBuffer(&xstate->Output, &MasterTrack->RawAudio.OutputRingBuffer[2*PrevIdx2 + 0], 1);

        /*
        * [xxxxxxxxxxxx][xxxxxxxxxxx]
        *             |..| 
        *  1. ask what row needs data
        *  2. provide notes
        *  3. synth last sample of previous row (meaning we have to keep the previous frame of custom notes alive)
        *  4. start synthing from the 1st sample of the next row, using the new notes, stopping on the 2nd last sample
        *     of the row.
        */

        TracyCZoneN(time_ctx, "Time Walking", 1);

        unsigned int PatternIdx              = xstate->xdoc->PatternSequence[xstate->CurrentPatternIndex].PatternIdx;
        int bOnLastRow                       = (xstate->CurrentRow == xstate->xdoc->PatternPool[PatternIdx].NumberOfLines - 1);
        int bSampleIncrementWouldWrapPattern = ((xstate->XRNSGridOffset >= 0 && (xstate->CurrentSample >= xstate->LocationOfNextLine)) || (xstate->XRNSGridOffset <  0 && (xstate->CurrentSample >= xstate->LocationOfNextLine - 1.0)));
        int bSampleIncrementWouldWrapLine    = (xstate->CurrentSample >= xstate->LocationOfNextLine);
        int bSampleIncrementWouldWrapTick    = (xstate->CurrentSample >= xstate->LocationOfNextTick);

        if (bExitingBeforeLine && !bSampleIncrementWouldWrapLine)
        {
            /* Exit before the **next** sample (next time we get here) would trigger a new row. 
             * We exit in time to give the caller the chance to update the notes.
             */
            int bNextSampleIncrementWouldWrapLine = ((xstate->CurrentSample + 1) >= xstate->LocationOfNextLine);
            if (bNextSampleIncrementWouldWrapLine)
            {
                bTimeToExit = 1;
                return_code = XRNS_WOULD_WRAP_ROW;
            }
        }

        if (bOnLastRow && bSampleIncrementWouldWrapPattern)
        {
            int bEndOfSong;

            /* we are now on the next pattern! */    
            if (GetNextPatternAndRowIndex(xstate, &xstate->CurrentPatternIndex, &xstate->CurrentRow, &bEndOfSong))
            {
                xstate->PatternHasBeenCued = 0;
            }

            if (bEndOfSong && xstate->bStopAtEndOfSong)
            {
                xstate->bSongStopped = 1;
            }
            GetNextPatternAndRowIndex(xstate, &xstate->NextPatternIndex, &xstate->NextRowIndex, NULL);
            xstate->CurrentTick = 0;

            /* update notes, instruments, etc .. */
            /* evaluate all effect changes, including tempo! */
            xrns_update_notes_and_effects(xstate, bOnLastRow && bSampleIncrementWouldWrapPattern);
            xrns_perform_tick_processing(xstate); /* There is always a tick on a line */

            xstate->XRNSGridOffset += (xstate->LocationOfNextLine - xstate->CurrentSample);
            xstate->CurrentSample = 0;

            xstate->LocationOfNextTick = xstate->XRNSGridOffset + xstate->CurrentTickDuration;
            xstate->LocationOfNextLine = xstate->XRNSGridOffset + xstate->CurrentLineDuration;
            xstate->BaseOfCurrentlyPlayingLine = xstate->XRNSGridOffset;
            if (bExitingAfterTick || bExitingAfterLine)
            {
                bTimeToExit = 1;
            }      
        }
        else if (!bOnLastRow && bSampleIncrementWouldWrapLine)
        {
            int bEndOfSong;

            /* we are now on the next line */
            GetNextPatternAndRowIndex(xstate, &xstate->CurrentPatternIndex, &xstate->CurrentRow, &bEndOfSong);

            if (bEndOfSong && xstate->bStopAtEndOfSong)
            {
                xstate->bSongStopped = 1;
            }

            GetNextPatternAndRowIndex(xstate, &xstate->NextPatternIndex, &xstate->NextRowIndex, NULL);
            xstate->CurrentTick = 0;

            /* update notes, instruments, etc .. */
            /* evaluate all effect changes, including tempo! */
            xrns_update_notes_and_effects(xstate, bOnLastRow && bSampleIncrementWouldWrapPattern);
            xrns_perform_tick_processing(xstate);

            xstate->LocationOfNextTick = xstate->LocationOfNextLine + xstate->CurrentTickDuration;
            xstate->BaseOfCurrentlyPlayingLine = xstate->LocationOfNextLine;

            xstate->LocationOfNextLine = xstate->LocationOfNextLine + xstate->CurrentLineDuration;
            if (bExitingAfterTick || bExitingAfterLine)
            {
                bTimeToExit = 1;
            }            
        } else if (bSampleIncrementWouldWrapTick)
        {
            xstate->CurrentTick++;
            xrns_perform_tick_processing(xstate);
            xstate->LocationOfNextTick = xstate->BaseOfCurrentlyPlayingLine + (xstate->CurrentTick + 1) * xstate->CurrentTickDuration;
            if (bExitingAfterTick)
            {
                bTimeToExit = 1;
            }
        }
        SamplesGenerated++;
        if (SamplesGenerated >= MaximumSamples)
        {
            bTimeToExit = 1;
        }

        TracyCZoneEnd(time_ctx);
    }

    TracyCZoneEnd(main_ctx);

    return return_code;
}

/* DLL Functions
 * =============================================================================
 * =============================================================================
 * =============================================================================
 * =============================================================================
 */

/* Returns an index greater than or equal to 0 on success, corresponding to the pattern index
 * of the pattern that will play on the next row. This index should be used to index the global
 * pattern pool.
 *
 * Return Codes:
 *              XRNS_ERR_NULL_STATE
 */
XRNS_DLL_EXPORT int32_t xrns_get_pattern_index_of_next_row(XRNSPlaybackState *xstate)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;
    return xstate->NextPatternIndex;
}

/* Returns an index greater than or equal to 0 on success, corresponding to the pattern index
 * of the pattern that is playing now. This index should be used to index the global
 * pattern pool.
 *
 * Return Codes:
 *              XRNS_ERR_NULL_STATE
 */
XRNS_DLL_EXPORT int32_t xrns_get_current_pattern_index(XRNSPlaybackState *xstate)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;
    return xstate->CurrentPatternIndex;
}

/* Returns an index greater than or equal to 0 on success, corresponding to the row
 * that will play after the current row. It can be 0 if we are on the last row
 * of a pattern, and can be something "unexpected" if the ZBxx command is used.
 *
 * Return Codes:
 *              XRNS_ERR_NULL_STATE
 */
XRNS_DLL_EXPORT int32_t xrns_get_next_row_index(XRNSPlaybackState *xstate)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;
    return xstate->NextRowIndex;
}

/* Returns an index greater than or equal to 0 on success, corresponding to the row
 * currently playing.
 *
 * Return Codes:
 *              XRNS_ERR_NULL_STATE
 */
XRNS_DLL_EXPORT int32_t xrns_get_current_row_index(XRNSPlaybackState *xstate)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;
    return xstate->CurrentRow;
}

/* Returns an index greater than or equal to 0 on success, corresponding to the tick
 * currently playing.
 *
 * Return Codes:
 *              XRNS_ERR_NULL_STATE
 */
XRNS_DLL_EXPORT int32_t xrns_get_current_tick_index(XRNSPlaybackState *xstate)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;
    return xstate->CurrentTick;
}

/* Returns a count of the samples available in the outgoing ringbuffer greater than or equal to
 * zero. 
 *
 * Return Codes:
 *              XRNS_ERR_NULL_STATE
 */
XRNS_DLL_EXPORT int32_t xrns_query_available_samples(XRNSPlaybackState *xstate)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;
    return (xstate->Output.RingBufferSz - xstate->Output.RingBufferFreeSamples);
}

/* Generates samples into the outgoing ringbuffer until a new tick is reached, or the ringbuffer 
 * fills up. 
 *
 * Returns the number of samples available in the ringbuffer. 
 *
 * Return Codes:
 *              XRNS_ERR_NULL_STATE
 *
 */
XRNS_DLL_EXPORT uint32_t xrns_do_one_tick(XRNSPlaybackState *xstate)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;
    run_engine(xstate, 1, 1, 0, xstate->Output.RingBufferFreeSamples);
    return xrns_query_available_samples(xstate);
}

/* Generates samples into the outgoing ringbuffer until a new row is reached, or the ringbuffer 
 * fills up. 
 *
 * Returns the number of samples available in the ringbuffer. 
 *
 * Return Codes:
 *              XRNS_ERR_NULL_STATE
 *
 */
XRNS_DLL_EXPORT int32_t xrns_do_one_row(XRNSPlaybackState *xstate)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;
    run_engine(xstate, 0, 1, 0, xstate->Output.RingBufferFreeSamples);
    return xrns_query_available_samples(xstate);
}

/* Generates the requested number of samples into the outgoing ringbuffer stopping if the ringbuffer 
 * fills up. Note that this will freely render over multiple row boundaries without stopping.
 *
 * Returns error codes, XRNS_WOULD_WRAP_ROW (only possible if bStopBeforeRows is true) or XRNS_SUCCESS.
 *
 * Return Codes:
 *              XRNS_SUCCESS
 *              XRNS_ERR_NULL_STATE
 *              XRNS_ERR_OUT_OF_SAMPLES
 *              XRNS_WOULD_WRAP_ROW
 *
 */
XRNS_DLL_EXPORT int32_t xrns_do_n_samples(XRNSPlaybackState *xstate, int32_t n, int32_t bStopBeforeRows)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;
    if (n > xstate->Output.RingBufferFreeSamples) return XRNS_ERR_OUT_OF_SAMPLES;
    int MinSamples = xstate->Output.RingBufferFreeSamples;
    if (MinSamples > n) MinSamples = n;
    return run_engine(xstate, 0, 0, (!!bStopBeforeRows), MinSamples);
}


/* Set track post-volume by name, performs sub-string matching and takes the first hit.
 * The volume is set in dB, with a maximum of 3.0f and no minimum. Returns XRNS_SUCCESS
 * on success.
 *
 * Return Codes:
 *
 * XRNS_SUCCESS
 * XRNS_ERR_NULL_STATE
 * XRNS_ERR_INVALID_TRACK_NAME
 * XRNS_ERR_TRACK_NOT_FOUND
 * XRNS_ERR_INVALID_INPUT_PARAM
 */
XRNS_DLL_EXPORT int32_t xrns_set_track_volume_by_name(XRNSPlaybackState *xstate, const char *TrackName, float VolumedB)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;

    if (!TrackName)
    {
        return XRNS_ERR_INVALID_TRACK_NAME;
    }

    unsigned int StrLen = strlen(TrackName);

    if (!StrLen || StrLen > XRNS_MAX_NAME)
    {
        return XRNS_ERR_INVALID_TRACK_NAME;
    }

    if (isnan(VolumedB) || VolumedB > 3.0f) return XRNS_ERR_INVALID_INPUT_PARAM;

    for (int track = 0; track < xstate->xdoc->NumTracks; track++)
    {
        xrns_track_desc *TrackDesc = &xstate->xdoc->Tracks[track];
        if (!TrackDesc->Name) continue;

        if (strstr(TrackDesc->Name, TrackName))
        {
            xrns_track_playback_state *Track = xstate->TrackStates[track];
            if (VolumedB < -96.0f)
            {
                Track->CurrentGamePostVolume.Target = 0.0f;
            }
            else

            {
                Track->CurrentGamePostVolume.Target = powf(10.0f, VolumedB/20.0f);
            }

            return XRNS_SUCCESS;
        }
    }

    return XRNS_ERR_TRACK_NOT_FOUND;
}

/* Write out num_samples worth of stereo audio data into the p_samples buffer. Samples will be
 * written interleaved. If more samples are requested than the amount available, 
 * XRNS_ERR_OUT_OF_SAMPLES is returned. xrns_done_producing_samples() should be called once
 * all the calls to xrns_produce_samples* functions with the same value of num_samples as was used
 * in this function.
 *
 * Return Codes:
 *
 * XRNS_SUCCESS
 * XRNS_ERR_NULL_STATE
 * XRNS_ERR_OUT_OF_SAMPLES
 */
XRNS_DLL_EXPORT int32_t xrns_produce_samples(XRNSPlaybackState *xstate, unsigned int num_samples, float *p_samples)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;

    if (xstate->Output.RingBufferSz - xstate->Output.RingBufferFreeSamples < num_samples)
    {
        return XRNS_ERR_OUT_OF_SAMPLES;
    }

    for (int s = 0, r = xstate->Output.RingBufferReadPtr; s < num_samples; s++)
    {
        p_samples[2*s + 0] = xstate->Output.OutputRingBuffer[2*r + 0];
        p_samples[2*s + 1] = xstate->Output.OutputRingBuffer[2*r + 1];
        r = (r + 1) % xstate->Output.RingBufferSz;
    }

    return XRNS_SUCCESS;
}

/* Write out num_samples worth of stereo audio data into the p_samples buffer. Samples will be
 * written interleaved. If more samples are requested than the amount available, 
 * XRNS_ERR_OUT_OF_SAMPLES is returned. xrns_done_producing_samples() should be called once
 * all the calls to xrns_produce_samples* functions with the same value of num_samples as was used
 * in this function.
 *
 * pp_track_names should point to num_track_names C-style strings. Each of these track names will
 * be tested against the XRNS tracks from left to right, and matching tracks will be summed into
 * the output. No matching tracks will result in silence.
 *
 * Return Codes:
 *
 * XRNS_SUCCESS
 * XRNS_ERR_NULL_STATE
 * XRNS_ERR_OUT_OF_SAMPLES
 * XRNS_ERR_INVALID_TRACK_NAME
 * XRNS_ERR_INVALID_INPUT_PARAM
 */
XRNS_DLL_EXPORT int32_t xrns_produce_samples_submix(XRNSPlaybackState *xstate, unsigned int num_samples, float *p_samples, char **pp_track_names, int num_track_names)
{
    int ch = 0;

    if (!xstate) return XRNS_ERR_NULL_STATE;

    if (!pp_track_names || !num_track_names)
    {
        return XRNS_ERR_INVALID_INPUT_PARAM;
    }

    if (num_track_names > xstate->xdoc->NumTracks) return XRNS_ERR_INVALID_INPUT_PARAM;

    for (ch = 0; ch < num_track_names; ch++)
    {
        if (!pp_track_names[ch]) return XRNS_ERR_INVALID_TRACK_NAME;
    }

    memset(p_samples, 0, sizeof(float) * 2 * num_samples);

    if (xstate->Output.RingBufferSz - xstate->Output.RingBufferFreeSamples < num_samples)
    {
        /* caller should make more calls to xrns_do_one_row/xrns_do_one_tick to produce more samples */
        return XRNS_ERR_OUT_OF_SAMPLES;
    }

    for (ch = 0; ch < num_track_names; ch++)
    {
        for (int track = 0; track < xstate->xdoc->NumTracks; track++)
        {
            xrns_track_desc *TrackDesc = &xstate->xdoc->Tracks[track];
            xrns_track_playback_state *Track = xstate->TrackStates[track];
            if (!TrackDesc->Name) continue;
            if (strstr(TrackDesc->Name, pp_track_names[ch]))
            {
                for (int s = 0, r = Track->RawAudio.RingBufferReadPtr; s < num_samples; s++)
                {
                    p_samples[2*s + 0] += Track->RawAudio.OutputRingBuffer[2*r + 0];
                    p_samples[2*s + 1] += Track->RawAudio.OutputRingBuffer[2*r + 1];
                    r = (r + 1) % Track->RawAudio.RingBufferSz;
                }
                continue;
            }
        }
    }

    return XRNS_SUCCESS;
}

/* Call this once all the calls to xrns_produce_samples* functions are done. Use the same value 
 * of num_samples as was used for those functions. XRNS_ERR_OUT_OF_SAMPLES can be returned
 * if there aren't enough samples to claim as consumed.
 *
 * Return Codes:
 *
 * XRNS_SUCCESS
 * XRNS_ERR_NULL_STATE
 * XRNS_ERR_OUT_OF_SAMPLES
 */
XRNS_DLL_EXPORT int32_t xrns_done_producing_samples(XRNSPlaybackState *xstate, unsigned int num_samples)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;

    if (xstate->Output.RingBufferSz - xstate->Output.RingBufferFreeSamples < num_samples)
    {
        return XRNS_ERR_OUT_OF_SAMPLES;
    }

    for (int t = 0; t < xstate->xdoc->NumTracks; t++)
    {
        xrns_track_playback_state *Track = xstate->TrackStates[t];

        for (int s = 0; s < num_samples; s++)
        {
            Track->RawAudio.RingBufferReadPtr = (Track->RawAudio.RingBufferReadPtr + 1) % Track->RawAudio.RingBufferSz;
            Track->RawAudio.RingBufferFreeSamples++;
        }        
    }

    for (int s = 0; s < num_samples; s++)
    {
        xstate->Output.RingBufferReadPtr = (xstate->Output.RingBufferReadPtr + 1) % xstate->Output.RingBufferSz;
        xstate->Output.RingBufferFreeSamples++;
    }

    return XRNS_SUCCESS;
}

/* Return Codes:
 *
 * XRNS_SUCCESS
 * XRNS_ERR_INVALID_INPUT_PARAM
 * XRNS_ERR_NULL_STATE
 * XRNS_ERR_WRONG_INPUT_SIZE
 */
XRNS_DLL_EXPORT int32_t xrns_provide_notes(XRNSPlaybackState *xstate, uint32_t num_notes, uint32_t num_bytes, void *p_notes)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;
    if (num_notes == 0 || num_bytes == 0 || !p_notes) return XRNS_ERR_INVALID_INPUT_PARAM;
    if ((num_bytes / num_notes) != sizeof(xrns_note_from_caller)) return XRNS_ERR_WRONG_INPUT_SIZE;

    for (int i = 0; i < num_notes; i++)
    {
        xstate->CallerNotes[i] = ((xrns_note_from_caller *) p_notes)[i];
    }

    xstate->NumCallerNotes = num_notes;

    return XRNS_SUCCESS;
}

XRNS_DLL_EXPORT int32_t xrns_prepare_active_notes(XRNSPlaybackState *xstate)
{
    if (!xstate) return XRNS_ERR_NULL_STATE;

    xrns_document *xdoc = xstate->xdoc;

    int track, col, s;
    int32_t NumOutgoingNotes = 0;

    for (track = 0; track < xdoc->NumTracks; track++)
    {
        xrns_track_desc *TrackDesc = &xdoc->Tracks[track];
        xrns_track_playback_state *Track = xstate->TrackStates[track];

        for (col = 0; col < TrackDesc->NumColumns; col++)
        {
            xrns_sampler_bank *SamplerBank = &xstate->SamplerBanks[track][col];

            for (s = 0; s < XRNS_MAX_SAMPLERS_PER_COLUMN; s++)
            {
                xrns_sampler *Sampler = &SamplerBank->Samplers[s];

                if (Sampler->bPlaying && Sampler->Active && NumOutgoingNotes < XRNS_ACTIVE_SAMPLER_COUNT)
                {
                    if (Sampler->CurrentInstrument == -1) continue;

                    xrns_instrument *Instrument = &xdoc->Instruments[Sampler->CurrentInstrument];
                    xrns_sampler_info *Info     = &xstate->ActiveSamplers[NumOutgoingNotes];

                    int CurrentSample = Sampler->PlaybackStates[0].CurrentSample;

                    if (CurrentSample == -1) continue;

                    int Transpose = Instrument->Samples[CurrentSample].Transpose;
                    int BaseNote  = Sampler->PlaybackStates[0].CurrentBaseNote;

                    Info->Track             = track;
                    Info->Col               = col;
                    Info->NoteID            = Sampler->CurrentNote; 
                    Info->NoteHz            = NoteToHzAssumingA440(Info->NoteID - BaseNote + Transpose);
                    Info->InstrumentID      = Sampler->CurrentInstrument;
                    Info->SampleID          = Sampler->PlaybackStates[0].CurrentSample;
                    Info->PlaybackDirection = Sampler->PlaybackStates[0].PlaybackDirection;
                    Info->SecondsPlayingFor = (float) Sampler->PlaybackStates[0].SamplesPlayedFor / xstate->OutputSampleRate;
                    Info->ActualNoteHz      = Sampler->SavedPitchMod;
                    NumOutgoingNotes++;
                }
            }
        }
    }

    xstate->NumActiveSamplers = NumOutgoingNotes;

    return NumOutgoingNotes;
}

XRNS_DLL_EXPORT void *xrns_pull_active_notes(XRNSPlaybackState *xstate)
{
    if (!xstate) return NULL;
    if (xstate->NumActiveSamplers)
        return (void *) &xstate->ActiveSamplers[0];
    return NULL;
}

#ifdef INLCUDE_FLATBUFFER_INTERFACE

XRNS_DLL_EXPORT int32_t xrns_copy_out_song_serialized(XRNSPlaybackState *xstate, char *p_bytes)
{
    if (xstate->bSongHasBeenSerialized)
    {
        memcpy(p_bytes, xstate->SerializedSongMemory, xstate->SerializedSongLengthInBytes);
        return 1;
    }
    else
    {
        return 0;
    }
}

XRNS_DLL_EXPORT uint32_t xrns_prepare_song_serialized(XRNSPlaybackState *xstate)
{
    int p, t, n, k;
    size_t size;
    flatcc_builder_t builder, *B;

    if (xstate->bSongHasBeenSerialized) return 0;

    xrns_document *xdoc = xstate->xdoc;

    B = &builder;

    flatcc_builder_init(B);

    XRNSSong_Song_start_as_root(B);

    XRNSSong_Song_Name_create_str(B, xdoc->SongName ? xdoc->SongName : "Unknown Song");
    XRNSSong_Song_Artist_create_str(B, xdoc->Artist ? xdoc->Artist   : "Unknown Artist");

    XRNSSong_Song_Instruments_start(B);
    for (t = 0; t < xdoc->NumInstruments; t++)
    {
        xrns_instrument *Instrument = &xdoc->Instruments[t];
        XRNSSong_Song_Instruments_push_start(B);
        XRNSSong_Instrument_Name_create_str(B, Instrument->Name ? Instrument->Name : "Unknown");
        XRNSSong_Instrument_bIsSliced_add(B, Instrument->bIsSliced);
        XRNSSong_Instrument_Samples_start(B);
        for (k = 0; k < Instrument->NumSamples; k++)
        {
            xrns_sample *Sample = &Instrument->Samples[k];
            XRNSSong_Sample_start(B);
            XRNSSong_Sample_Name_create_str(B, Sample->Name ? Sample->Name : "Unknown");
            XRNSSong_Sample_Volume_add(B, Sample->Volume);
            XRNSSong_Sample_Panning_add(B, Sample->Panning);
            XRNSSong_Sample_Transpose_add(B, Sample->Transpose);
            XRNSSong_Sample_end(B);
        }
        XRNSSong_Instrument_Samples_end(B);

        XRNSSong_Song_Instruments_push_end(B);
    }
    XRNSSong_Song_Instruments_end(B);

    XRNSSong_Song_Tracks_start(B);
    for (t = 0; t < xdoc->NumTracks; t++)
    {
        xrns_track_desc *TrackDesc = &xdoc->Tracks[t];
        XRNSSong_Song_Tracks_push_start(B);
            XRNSSong_TrackInfo_Name_create_str(B, TrackDesc->Name ? TrackDesc->Name : "Untitled Track");
            XRNSSong_TrackInfo_NumColumns_add(B, TrackDesc->NumColumns);
            XRNSSong_TrackInfo_NumEffectColumns_add(B, TrackDesc->NumEffectColumns);
            XRNSSong_TrackInfo_bIsGroup_add(B, TrackDesc->bIsGroup);
            XRNSSong_TrackInfo_WrapsNPreviousTracks_add(B, TrackDesc->WrapsNPreviousTracks);
        XRNSSong_Song_Tracks_push_end(B);
    }
    XRNSSong_Song_Tracks_end(B);

    XRNSSong_Song_Patterns_start(B);

    for (p = 0; p < xdoc->PatternSequenceLength; p++)
    {
        XRNSSong_Song_Patterns_push_start(B);

        xrns_pattern_sequence_entry *pse = &xdoc->PatternSequence[p];
        int PoolIdx = pse->PatternIdx;
        xrns_pattern *Pattern = &xdoc->PatternPool[PoolIdx];

        if (Pattern->Name)
            XRNSSong_Pattern_Name_create_str(B, Pattern->Name);

        XRNSSong_Pattern_bIsSectionStart_add(B, pse->bIsSectionStart);

        if (pse->bIsSectionStart)
            XRNSSong_Pattern_NameOfSection_create_str(B, pse->SectionName ? pse->SectionName : "Untitled Section");

        XRNSSong_Pattern_NumRows_add(B, Pattern->NumberOfLines);

        XRNSSong_Pattern_Tracks_start(B);
        for (t = 0; t < xdoc->NumTracks; t++)
        {
            /* Tracks are serialized linearly, from left to right.
             * A separate structure describing how these tracks are 
             * actually nested logically will be provided for anyone
             * who cares about that.
             */

            XRNSSong_Pattern_Tracks_push_start(B);
            
            int SkipTrackDueToMuted = 0;

            for (int m = 0; m < pse->NumMutedTracks; m++)
            {
                if (t == pse->MutedTracks[m])
                {
                    SkipTrackDueToMuted = 1;
                    break;
                }
            }

            xrns_track *Track = &xdoc->PatternPool[PoolIdx].Tracks[t];
            if (Track->bIsAlias && Track->AliasIdx < xdoc->NumPatterns)
            {
                Track = &xdoc->PatternPool[Track->AliasIdx].Tracks[t];
            }

            /* Now just go ham and loop over all the notes, then over all the effects. */
            XRNSSong_Track_Notes_start(B);

            if (SkipTrackDueToMuted) 
            {
                XRNSSong_Track_Notes_end(B);
                XRNSSong_Pattern_Tracks_push_end(B);
                continue;
            }

            for (n = 0; n < Track->NumNotes; n++)
            {
                xrns_note *Note = &Track->Notes[n];
                if (   Note->Type != XRNS_NOTE_EFFECT
                    && Note->Note != XRNS_NOTE_BLANK)
                {
                    /* Avoid sending blanks until we make more progress later. */
                    unsigned char TheNote;
                    if (Note->Note == XRNS_NOTE_OFF) TheNote = XRNSSong_NoteValue_NOTE_OFF;
                    else if (Note->Note == XRNS_NOTE_BLANK) TheNote = XRNSSong_NoteValue_NOTE_BLANK;
                    else TheNote = Note->Note % 12;

                    XRNSSong_Note_vec_push_create(B,
                        TheNote,
                        Note->Note / 12,
                        Note->Line,
                        Note->Column,
                        Note->Instrument,
                        0, /* this is inline effect */
                        0
                        );
                }
            }
            XRNSSong_Track_Notes_end(B);

            /* EFFECTS */
            XRNSSong_Pattern_Tracks_push_end(B);

        }
        XRNSSong_Pattern_Tracks_end(B);

        XRNSSong_Song_Patterns_push_end(B);
    }

    XRNSSong_Song_Patterns_end(B);  
    
    XRNSSong_Song_end_as_root(B);

    xstate->SerializedSongMemory = flatcc_builder_finalize_buffer(B, &size);
    xstate->SerializedSongLengthInBytes = (unsigned int) size;

    flatcc_builder_clear(B);

    if (size != 0)
    {
        xstate->bSongHasBeenSerialized = 1;
    }

    return (uint32_t) xstate->SerializedSongLengthInBytes;
}
#endif

void xrns_produce_samples_int16(XRNSPlaybackState *xstate, unsigned int num_samples, int16_t *p_samples)
{
    int i;
    float *dumb = malloc(num_samples * 2 * sizeof(float));
    
    int samples_produced = 0;
    int blk = 1024;
    while (samples_produced < num_samples)
    {
        int samples_to_produce = num_samples - samples_produced;
        if (samples_to_produce > blk)
            samples_to_produce = blk;

        TracyCFrameMark;
        run_engine(xstate, 0, 0, 0, samples_to_produce);
        int samples_available = xrns_query_available_samples(xstate);
        xrns_produce_samples(xstate, samples_available, dumb + 2*samples_produced);
        xrns_done_producing_samples(xstate, samples_available);
        samples_produced += samples_available;
    }

    for (i = 0; i < num_samples * 2; i++)
    {
        p_samples[i] = (int16_t) (32768 * dumb[i]);
    }

    free(dumb);
}
