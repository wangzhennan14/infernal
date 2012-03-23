/* Infernal's accelerated seq/profile comparison pipeline
 *  
 * Contents:
 *   1. CM_PIPELINE: allocation, initialization, destruction
 *   2. Pipeline API
 *   3. Non-API filter stage search functions.
 *   4. Copyright and license information
 */
#include "esl_config.h"
#include "p7_config.h"
#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h> 

#include "easel.h"
#include "esl_exponential.h"
#include "esl_getopts.h"
#include "esl_gumbel.h"
#include "esl_vectorops.h"

#include "hmmer.h"

#include "infernal.h"

#define DEBUGPIPELINE  0
#define DEBUGMSVMERGE  0

static int  pli_p7_filter         (CM_PIPELINE *pli, P7_OPROFILE *om, P7_BG *bg, float *p7_evparam, P7_MSVDATA *msvdata, const ESL_SQ *sq, int64_t **ret_ws, int64_t **ret_we, int *ret_nwin);
static int  pli_p7_env_def        (CM_PIPELINE *pli, P7_OPROFILE *om, P7_BG *bg, float *p7_evparam, const ESL_SQ *sq, int64_t *ws, int64_t *we, int nwin, P7_HMM **opt_hmm, P7_PROFILE **opt_gm, P7_PROFILE **opt_Rgm, P7_PROFILE **opt_Lgm, P7_PROFILE **opt_Tgm, int64_t **ret_es, int64_t **ret_ee, int *ret_nenv);
static int  pli_cyk_env_filter    (CM_PIPELINE *pli, off_t cm_offset, const ESL_SQ *sq, int64_t *p7es, int64_t *p7ee, int np7env, CM_t **opt_cm, int64_t **ret_es, int64_t **ret_ee, int *ret_nenv);
static int  pli_cyk_seq_filter    (CM_PIPELINE *pli, off_t cm_offset, const ESL_SQ *sq, CM_t **opt_cm, int64_t **ret_ws, int64_t **ret_we, int *ret_nwin);
static int  pli_final_stage       (CM_PIPELINE *pli, off_t cm_offset, const ESL_SQ *sq, int64_t *es, int64_t *ee, int nenv, CM_TOPHITS *hitlist, CM_t **opt_cm);
static int  pli_dispatch_cm_search(CM_PIPELINE *pli, CM_t *cm, ESL_DSQ *dsq, int64_t start, int64_t stop, CM_TOPHITS *hitlist, float cutoff, float env_cutoff, int qdbidx, float *ret_sc, int *opt_used_hb, int64_t *opt_envi, int64_t *opt_envj);
static int  pli_align_hit         (CM_PIPELINE *pli, CM_t *cm, const ESL_SQ *sq, CM_HIT *hit, int cp9b_valid);
static int  pli_scan_mode_read_cm (CM_PIPELINE *pli, off_t cm_offset, CM_t **ret_cm);
static void copy_subseq           (const ESL_SQ *src_sq, ESL_SQ *dest_sq, int64_t i, int64_t L);

/*****************************************************************
 * 1. The CM_PIPELINE object: allocation, initialization, destruction.
 *****************************************************************/

/* Function:  cm_pipeline_Create()
 * Synopsis:  Create a new accelerated comparison pipeline.
 * Incept:    EPN, Fri Sep 24 16:14:39 2010
 *            SRE, Fri Dec  5 10:11:31 2008 [Janelia] (p7_pipeline_Create())
 *
 * Purpose:   Given an application configuration structure <go>
 *            containing certain standardized options (described
 *            below), some initial guesses at the model size <M_hint>
 *            and sequence length <L_hint> that will be processed,
 *            and a <mode> that can be either <cm_SCAN_MODELS> or
 *            <cm_SEARCH_SEQS> depending on whether we're searching one sequence
 *            against a model database (cmscan mode) or one model
 *            against a sequence database (cmsearch mode); create new
 *            pipeline object.
 *
 *            In search mode, we would generally know the length of
 *            our query profile exactly, and would pass <cm->clen> as <M_hint>;
 *            in scan mode, we generally know the length of our query
 *            sequence exactly, and would pass <sq->n> as <L_hint>.
 *            Targets will come in various sizes as we read them,
 *            and the pipeline will resize any necessary objects as
 *            needed, so the other (unknown) length is only an
 *            initial allocation.
 *
 *            <Z> is passed as the database size, in residues, if
 *            known. If unknown, 0 should be passed as <Z>.
 *            
 *            The configuration <go> must include settings for the 
 *            following options:
 *            
 *            || option      ||            description                     || usually  ||
 *            | -g           |  configure CM for glocal alignment           |   FALSE   |
 *            | -Z           |  database size in Mb                         |    NULL   |
 *            | --allstats   |  verbose statistics output mode              |   FALSE   |
 *            | --acc        |  prefer accessions over names in output      |   FALSE   |
 *            | --noali      |  don't output alignments (smaller output)    |   FALSE   |
 *            | -E           |  report hits <= this E-value threshold       |    10.0   |
 *            | -T           |  report hits >= this bit score threshold     |    NULL   |
 *            | --incE       |  include hits <= this E-value threshold      |    0.01   |
 *            | --incT       |  include hits >= this bit score threshold    |    NULL   |
 *            | --cut_ga     |  model-specific thresholding using GA        |   FALSE   |
 *            | --cut_nc     |  model-specific thresholding using NC        |   FALSE   |
 *            | --cut_tc     |  model-specific thresholding using TC        |   FALSE   |
 *            | --max        |  turn all heuristic filters off              |   FALSE   |
 *            | --nohmm      |  turn all HMM filters off                    |   FALSE   |
 *            | --mid        |  turn off MSV and Viterbi filters            |   FALSE   |
 *            | --rfam       |  set filters to strict Rfam settings         |   FALSE   |
 *            | --FZ <x>     |  set filter thr as if dbsize were <x> Mb     |    NULL   |
 *            | --Fmid <x>   |  with --mid, set fwd filter thresholds to <x>|    NULL   |
 *            | --notrunc    |  turn off truncated hit detection            |   FALSE   |
 *            | --anytrunc   |  allow truncated hits anywhere in the seq    |   FALSE   |
 *            | --nonull3    |  turn off NULL3 correction                   |   FALSE   |
 *            | --mxsize <x> |  set max allowed HMM banded DP mx size to <x>|    128 Mb |
 *            | --cyk        |  set final search stage as CYK, not Inside   |   FALSE   |
 *            | --aln-cyk    |  align hits with CYK, not optimal accuracy   |   FALSE   |
 *            | --toponly    |  only search top strand                      |   FALSE   |
 *            | --bottomonly |  only search bottom strand                   |   FALSE   |
 * *** all opts below this line are 'developer' options, only visible in cmsearch/cmscan via --devhelp 
 *            | --noF1       |  turn off MSV filter stage                   |   FALSE   |
 *            | --noF2       |  turn off Viterbi filter stage               |   FALSE   |
 *            | --noF3       |  turn off HMM local forward stage            |   FALSE   |
 *            | --noF4       |  turn off HMM glocal forward stage           |   FALSE   |
 *            | --noF6       |  turn off CYK filter stage                   |   FALSE   |
 *            | --doF1b      |  turn on  MSV composition bias filter        |   FALSE   |
 *            | --noF2b      |  turn off Viterbi composition bias filter    |   FALSE   |
 *            | --noF3b      |  turn off local forward bias filter          |   FALSE   |
 *            | --noF4b      |  turn off glocal forward bias filter         |   FALSE   |
 *            | --doF5b      |  turn on  per-envelope bias filter           |   TRUE    |
 * *** options for defining filter thresholds, usually NULL bc set in DB-size dependent manner
 *            | --F1         |  Stage 1  (MSV)         P value threshold    |    NULL   |
 *            | --F1b        |  Stage 1b (MSV bias)    P value threshold    |    NULL   |
 *            | --F2         |  Stage 2  (Vit)         P value threshold    |    NULL   |
 *            | --F2b        |  Stage 2b (Vit bias)    P value threshold    |    NULL   |
 *            | --F3         |  Stage 3  (lFwd)        P value threshold    |    NULL   |
 *            | --F3b        |  Stage 3b (lFwd bias)   P value threshold    |    NULL   |
 *            | --F4         |  Stage 4  (gFwd)        P value threshold    |    NULL   |
 *            | --F4b        |  Stage 4b (gFwd bias)   P value threshold    |    NULL   |
 *            | --F5         |  Stage 5  (envdef)      P value threshold    |    NULL   |
 *            | --F5b        |  Stage 5b (envdef bias) P value threshold    |    NULL   |
 *            | --F6         |  Stage 6  (CYK)         P value threshold    |    NULL   |
 *            | --ftau       |  HMM band tail loss prob for CYK filter      |    1e-4   |
 *            | --fsums      |  use sums to get CYK filter HMM bands        |   FALSE   |
 *            | --fbeta      |  beta for QDBs in CYK filter                 |    1e-7   |
 *            | --fnonbanded |  run CYK filter without bands                |   FALSE   |
 *            | --nocykenv   |  do not redefine envelopes using CYK         |   FALSE   |
 *            | --cykenvx    |  P-value multiplier for CYK envelope redefn  |    NULL   |
 *            | --tau        |  HMM band tail loss prob for final round     |    5e-6   |
 *            | --sums       |  use sums to get final round HMM bands       |   FALSE   |
 *            | --beta       |  beta for QDBs in final round                |   1e-15   |
 *            | --nonbanded  |  run CYK filter without bands                |   FALSE   |
 *            | --timeF1     |  abort after F1b stage, for timing expts     |   FALSE   | 
 *            | --timeF2     |  abort after F2b stage, for timing expts     |   FALSE   | 
 *            | --timeF3     |  abort after F3b stage, for timing expts     |   FALSE   | 
 *            | --timeF4     |  abort after F4b stage, for timing expts     |   FALSE   | 
 *            | --timeF5     |  abort after F5b stage, for timing expts     |   FALSE   | 
 *            | --timeF6     |  abort after F6  stage, for timing expts     |   FALSE   | 
 *            | --rt1        |  P7_DOMAINDEF rt1 parameter                  |    0.25   |
 *            | --rt2        |  P7_DOMAINDEF rt2 parameter                  |    0.10   |
 *            | --rt3        |  P7_DOMAINDEF rt3 parameter                  |    0.20   |
 *            | --ns         |  number of domain/envelope tracebacks        |     200   |
 *            | --anonbanded |  do not use bands when aligning hits         |   FALSE   |
 *            | --anewbands  |  calculate new bands for hit alignment       |   FALSE   |
 *            | --nogreedy   |  use optimal CM hit resolution, not greedy   |   FALSE   |
 *            | --filcmW     |  use CM's W not HMM's for all filter stages  |   FALSE   |
 *            | --cp9noel    |  turn off EL state in CP9 HMM                |   FALSE   |
 *            | --cp9gloc    |  configure CP9 HMM in glocal mode            |   FALSE   |
 *            | --null2      |  turn on null2 biased composition model      |   FALSE   |
 *            | --xtau       |  tau multiplier during band tightening       |     2.0   |
 *            | --maxtau     |  max tau during band tightening              |    0.01   |
 *            | --seed       |  RNG seed (0=use arbitrary seed)             |     181   |
 *
 * Returns:   ptr to new <cm_PIPELINE> object on success. Caller frees this
 *            with <cm_pipeline_Destroy()>.
 *
 * Throws:    <NULL> on allocation failure.
 */
CM_PIPELINE *
cm_pipeline_Create(ESL_GETOPTS *go, ESL_ALPHABET *abc, int clen_hint, int L_hint, int64_t Z, enum cm_zsetby_e Z_setby, enum cm_pipemodes_e mode)
{
  CM_PIPELINE *pli  = NULL;
  int          seed = esl_opt_GetInteger(go, "--seed");
  int          status;
  double       Z_Mb; /* database size in Mb */
  int          pass_idx; /* counter over passes */

  ESL_ALLOC(pli, sizeof(CM_PIPELINE));

  /* allocate matrices */
  if ((pli->fwd  = p7_omx_Create(clen_hint, L_hint, L_hint)) == NULL) goto ERROR;
  if ((pli->bck  = p7_omx_Create(clen_hint, L_hint, L_hint)) == NULL) goto ERROR;
  if ((pli->oxf  = p7_omx_Create(clen_hint, 0,      L_hint)) == NULL) goto ERROR;
  if ((pli->oxb  = p7_omx_Create(clen_hint, 0,      L_hint)) == NULL) goto ERROR;     
  if ((pli->gfwd = p7_gmx_Create(clen_hint, L_hint))         == NULL) goto ERROR;
  if ((pli->gbck = p7_gmx_Create(clen_hint, L_hint))         == NULL) goto ERROR;
  if ((pli->gxf  = p7_gmx_Create(clen_hint, L_hint))         == NULL) goto ERROR;
  if ((pli->gxb  = p7_gmx_Create(clen_hint, L_hint))         == NULL) goto ERROR;     

  /* Initializations */
  pli->mode         = mode;
  pli->abc          = abc;  
  pli->errbuf[0]    = '\0'; 
  pli->maxW         = 0;    /* model-dependent, invalid until cm_pli_NewModel() is called */
  pli->cmW          = 0;    /* model-dependent, invalid until cm_pli_NewModel() is called */
  pli->clen         = 0;    /* model-dependent, invalid until cm_pli_NewModel() is called */
  pli->cur_cm_idx   = -1;   /* model-dependent, invalid until cm_pli_NewModel() is called */
  pli->cur_seq_idx  = -1;   /* sequence-dependent, invalid until cm_pli_NewSeq() is called */
  pli->cur_pass_idx = -1;   /* pipeline-pass-dependent, updated in cm_Pipeline() */
  pli->cmfp         = NULL; /* set by caller only if we're a scan pipeline (i.e. set in cmscan) */

  /* Accounting, as we collect results */
  pli->nmodels = 0;
  pli->nseqs   = 0;
  pli->nnodes  = 0;
  for(pass_idx = 0; pass_idx < NPLI_PASSES; pass_idx++) { 
    cm_pli_ZeroAccounting(&(pli->acct[pass_idx]));
  }

  /* Normally, we reinitialize the RNG to the original seed every time we're
   * about to collect a stochastic trace ensemble. This eliminates run-to-run
   * variability. As a special case, if seed==0, we choose an arbitrary one-time 
   * seed: time() sets the seed, and we turn off the reinitialization.
   */
  pli->r                  =  esl_randomness_CreateFast(seed);
  pli->do_reseeding       = (seed == 0) ? FALSE : TRUE;
  pli->ddef               = p7_domaindef_Create(pli->r);
  pli->ddef->do_reseeding = pli->do_reseeding;

  /* Miscellaneous parameters */
  pli->hb_size_limit   = esl_opt_GetReal   (go, "--mxsize");
  pli->do_top          = esl_opt_GetBoolean(go, "--bottomonly") ? FALSE : TRUE;
  pli->do_bot          = esl_opt_GetBoolean(go, "--toponly")    ? FALSE : TRUE;
  pli->do_allstats     = esl_opt_GetBoolean(go, "--allstats")   ? TRUE  : FALSE;
  pli->show_accessions = esl_opt_GetBoolean(go, "--acc")        ? TRUE  : FALSE;
  pli->show_alignments = esl_opt_GetBoolean(go, "--noali")      ? FALSE : TRUE;
  pli->do_hb_recalc    = esl_opt_GetBoolean(go, "--anewbands")  ? TRUE  : FALSE;
  pli->xtau            = esl_opt_GetReal(go, "--xtau");
  pli->maxtau          = esl_opt_GetReal(go, "--maxtau");
  pli->do_time_F1      = esl_opt_GetBoolean(go, "--timeF1")     ? TRUE  : FALSE;
  pli->do_time_F2      = esl_opt_GetBoolean(go, "--timeF2")     ? TRUE  : FALSE;
  pli->do_time_F3      = esl_opt_GetBoolean(go, "--timeF3")     ? TRUE  : FALSE;
  pli->do_time_F4      = esl_opt_GetBoolean(go, "--timeF4")     ? TRUE  : FALSE;
  pli->do_time_F5      = esl_opt_GetBoolean(go, "--timeF5")     ? TRUE  : FALSE;
  pli->do_time_F6      = esl_opt_GetBoolean(go, "--timeF6")     ? TRUE  : FALSE;

  /* hard-coded miscellaneous parameters that were command-line
   * settable in past testing, and could be in future testing.
   */
  pli->smult           = 2.0;
  pli->wmult           = 1.0;
  pli->cmult           = 1.25;
  pli->mlmult          = 0.1;
  
  /* Configure reporting thresholds */
  pli->by_E            = TRUE;
  pli->E               = esl_opt_GetReal(go, "-E");
  pli->T               = 0.0;
  pli->use_bit_cutoffs = FALSE;
  if (esl_opt_IsOn(go, "-T")) { 
    pli->T    = esl_opt_GetReal(go, "-T"); 
    pli->by_E = FALSE;
  } 

  /* Configure inclusion thresholds */
  pli->inc_by_E           = TRUE;
  pli->incE               = esl_opt_GetReal(go, "--incE");
  pli->incT               = 0.0;
  if (esl_opt_IsOn(go, "--incT")) { 
    pli->incT     = esl_opt_GetReal(go, "--incT"); 
    pli->inc_by_E = FALSE;
  } 

  /* Configure for one of the model-specific thresholding options */
  if (esl_opt_GetBoolean(go, "--cut_ga")) {
    pli->T        = 0.0;
    pli->by_E     = FALSE;
    pli->incT     = 0.0;
    pli->inc_by_E = FALSE;
    pli->use_bit_cutoffs = CMH_GA;
  }
  if (esl_opt_GetBoolean(go, "--cut_nc")) { 
    pli->T        = 0.0;
    pli->by_E     = FALSE;
    pli->incT     = 0.0;
    pli->inc_by_E = FALSE;
    pli->use_bit_cutoffs = CMH_NC;
  }
  if (esl_opt_GetBoolean(go, "--cut_tc")) { 
    pli->T        = 0.0;
    pli->by_E     = FALSE;
    pli->incT     = 0.0;
    pli->inc_by_E = FALSE;
    pli->use_bit_cutoffs = CMH_TC;
  }

  /* Configure envelope definition parameters */
  pli->rt1            = esl_opt_GetReal(go, "--rt1");
  pli->rt2            = esl_opt_GetReal(go, "--rt2");
  pli->rt3            = esl_opt_GetReal(go, "--rt3");
  pli->ns             = esl_opt_GetInteger(go, "--ns");
  pli->ddef->rt1      = pli->rt1;
  pli->ddef->rt2      = pli->rt2;
  pli->ddef->rt3      = pli->rt3;
  pli->ddef->nsamples = pli->ns;

  /* configure truncation hit allowance parameters */
  if(esl_opt_GetBoolean(go, "--anytrunc")) { 
    pli->do_trunc_ends = FALSE;
    pli->do_trunc_any  = TRUE;
  }
  else if(esl_opt_GetBoolean(go, "--notrunc")) { 
    pli->do_trunc_ends = FALSE;
    pli->do_trunc_any  = FALSE;
  }
  else { /* default */
    pli->do_trunc_ends = TRUE;
    pli->do_trunc_any  = FALSE;
  }

  /* Set Z, the search space size. This is used for E-value
   * calculations and for setting filter thresholds by default
   * (i.e. if none of --max, --nohmm, --mid, --rfam are used) which is
   * why we do this here, before setting filter thresholds.  The
   * database size was passed in, if -Z <x> enabled, we overwrite the
   * passed in value with <x>.
   */
  if (esl_opt_IsOn(go, "-Z")) { 
      pli->Z_setby = CM_ZSETBY_OPTION;
      pli->Z       = (int64_t) (esl_opt_GetReal(go, "-Z") * 1000000.); 
  }
  else { 
    pli->Z       = Z;       /* Z is an input variable to this function */
    pli->Z_setby = Z_setby; /* so is Z_setby */
  }

  /********************************************************************************/
  /* Configure acceleration pipeline by setting filter on/off parameters
   * and filter thresholds. 
   *
   * Two steps:
   * 1. Set filter parameters based on which of the five filtering strategies 
   *    we're using.
   * 2. Overwrite any filter parameters set on the command-line.
   *
   * The five exclusive filtering strategies: 
   * 1. --max:     turn off all filters
   * 2. --nohmm:   turn off all HMM filters
   * 3. --mid:     turn off MSV/Viterbi HMM filters
   * 4. default:   use all filters with DB-size dependent thresholds
   * 5. --rfam:    use all filters with strict thresholds (as if DB was size of RFAMSEQ)
   *
   * strategy       F1?*  F2/F2b?  F3/F3b?  F4/F4b?    F5?**      F6?
   * --------    -------  -------  -------  -------  -------  -------  
   * --max           off      off      off      off      off      off
   * --nohmm         off      off      off      off      off       on
   * --mid           off      off       on       on       on       on
   * default          on       on       on       on       on       on
   * --rfam           on       on       on       on       on       on
   * 
   *  * By default, F1b is always off.
   * ** By default, F5b is always off.
   *
   * First set defaults, then make nec changes if --max, --nohmm, --mid or 
   * --rfam 
   */
  pli->do_max        = FALSE;
  pli->do_nohmm      = FALSE;
  pli->do_mid        = FALSE;
  pli->do_rfam       = FALSE;
  pli->do_msv        = TRUE;
  pli->do_msvbias    = FALSE;
  pli->do_vit        = TRUE;
  pli->do_vitbias    = TRUE;
  pli->do_fwd        = TRUE;
  pli->do_fwdbias    = TRUE;
  pli->do_gfwd       = TRUE;
  pli->do_gfwdbias   = TRUE;
  pli->do_edef       = TRUE;
  pli->do_edefbias   = FALSE;
  pli->do_fcyk       = TRUE;

  if(esl_opt_GetBoolean(go, "--max")) { 
    pli->do_max = TRUE;
    pli->do_msv     = pli->do_vit     = pli->do_fwd     = pli->do_gfwd     = pli->do_edef     = pli->do_fcyk    = FALSE; 
    pli->do_msvbias = pli->do_vitbias = pli->do_fwdbias = pli->do_gfwdbias = pli->do_edefbias = pli->do_fcykenv = FALSE;
    pli->F1  = pli->F2  = pli->F3  = pli->F4  = pli->F5  = pli->F6 = 1.0;
    pli->F1b = pli->F2b = pli->F3b = pli->F4b = pli->F5b = pli->F6 = 1.0;
    /* D&C truncated alignment is not robust, so we don't allow it */
    pli->do_trunc_ends = FALSE;
    pli->do_trunc_any  = FALSE;
  }
  else if(esl_opt_GetBoolean(go, "--nohmm")) { 
    pli->do_nohmm  = TRUE;
    pli->do_msv     = pli->do_vit     = pli->do_fwd     = pli->do_gfwd     = pli->do_edef     = FALSE;
    pli->do_msvbias = pli->do_vitbias = pli->do_fwdbias = pli->do_gfwdbias = pli->do_edefbias = FALSE;
    pli->F1  = pli->F2  = pli->F3  = pli->F4  = pli->F5  = 1.0;
    pli->F1b = pli->F2b = pli->F3b = pli->F4b = pli->F5b = 1.0;
    /* D&C truncated alignment is not robust, so we don't allow it */
    pli->do_trunc_ends = FALSE;
    pli->do_trunc_any  = FALSE;
  }
  else if(esl_opt_GetBoolean(go, "--mid")) { 
    pli->do_mid  = TRUE;
    pli->do_msv     = pli->do_vit     = FALSE;
    pli->do_msvbias = pli->do_vitbias = FALSE;
    pli->F1  = pli->F2  = 1.0;
    pli->F1b = pli->F2b = 1.0;
    pli->F3  = pli->F3b = pli->F4 = pli->F4b = pli->F5 = pli->F5b = esl_opt_GetReal(go, "--Fmid");
  }
  else if(esl_opt_GetBoolean(go, "--rfam")) { 
    pli->do_rfam = TRUE;
    pli->F1 = pli->F1b = 0.05;
    pli->F2 = pli->F2b = 0.04;
    pli->F3 = pli->F3b = 0.0004;
    pli->F4 = pli->F4b = 0.0004;
    pli->F5 = pli->F5b = 0.0004;
    pli->F6 = 0.0001;
    /* these are the same as the defaults for a 100 Gb database or larger */
  }
  else { 
    /* None of --max, --nohmm, --mid, --rfam enabled, use default
     * strategy, set filter thresholds dependent on Z, which was set
     * above. These default thresholds are hard-coded and were determined
     * by a systematic search over possible filter threshold combinations.
     * xref: ~nawrockie/notebook/11_0513_inf_dcmsearch_thresholds/00LOG
     */
    Z_Mb = esl_opt_IsOn(go, "--FZ") ? esl_opt_GetReal(go, "--FZ") : pli->Z / 1000000.;
    if(Z_Mb >= (100000. - eslSMALLX1)) { /* Z >= 100 Gb */
      pli->F1 = pli->F1b = 0.05;
      pli->F2 = pli->F2b = 0.04;
      pli->F3 = pli->F3b = 0.0004;
      pli->F4 = pli->F4b = 0.0004;
      pli->F5 = pli->F5b = 0.0004;
      pli->F6 = 0.0001;
    }
    else if(Z_Mb >= (10000. - eslSMALLX1)) { /* 100 Gb > Z >= 10 Gb */
      pli->F1 = pli->F1b = 0.06;
      pli->F2 = pli->F2b = 0.05;
      pli->F3 = pli->F3b = 0.0005;
      pli->F4 = pli->F4b = 0.0005;
      pli->F5 = pli->F5b = 0.0005;
      pli->F6 = 0.0001;
    }
    else if(Z_Mb >= (1000. - eslSMALLX1)) { /* 10 Gb > Z >= 1 Gb */
      pli->F1 = pli->F1b = 0.06;
      pli->F2 = pli->F2b = 0.15;
      pli->F3 = pli->F3b = 0.0005;
      pli->F4 = pli->F4b = 0.0005;
      pli->F5 = pli->F5b = 0.0005;
      pli->F6 = 0.0001;
    }
    else if(Z_Mb >= (100. - eslSMALLX1)) { /* 1 Gb  > Z >= 100 Mb */
      pli->F1 = pli->F1b = 0.30;
      pli->F2 = pli->F2b = 0.15;
      pli->F3 = pli->F3b = 0.002;
      pli->F4 = pli->F4b = 0.002;
      pli->F5 = pli->F5b = 0.002;
      pli->F6 = 0.0001;
    }
    else if(Z_Mb >= (10. - eslSMALLX1)) { /* 100 Mb  > Z >= 10 Mb */
      pli->F1 = pli->F1b = 0.35;
      pli->F2 = pli->F2b = 0.20;
      pli->F3 = pli->F3b = 0.003;
      pli->F4 = pli->F4b = 0.003;
      pli->F5 = pli->F5b = 0.003;
      pli->F6 = 0.0001;
    }
    else if(Z_Mb >= (1. - eslSMALLX1)) { /* 10 Mb  > Z >= 1 Mb */
      pli->F1 = pli->F1b = 0.35;
      pli->F2 = pli->F2b = 0.20;
      pli->F3 = pli->F3b = 0.015;
      pli->F4 = pli->F4b = 0.015;
      pli->F5 = pli->F5b = 0.015;
      pli->F6 = 0.0001;
    }
    else { /* 1 Mb  > Z */
      pli->do_msv = FALSE;
      pli->F1 = pli->F1b = 1.00; /* this is irrelevant */
      pli->F2 = pli->F2b = 0.25;
      pli->F3 = pli->F3b = 0.02;
      pli->F4 = pli->F4b = 0.02;
      pli->F5 = pli->F5b = 0.02;
      pli->F6 = 0.0001;
    }
  } /* end of 'else' entered if none of --max, --nohmm, --mid, --rfam used */

  /* Filter on/off parameters and thresholds are now completely set
   * based on filtering strategy. Final step is to overwrite any that
   * the user set on the command-line. (Only expert users should be
   * doing this.) 
   * 
   * We have to be careful here to not turn on filters that our
   * strategy disallows. The definition of ESL_GETOPTS go should
   * enforce that incompatible options cause a failure (and thus will
   * never exist in go), but we do a second check here for some
   * combinations.
   */
  if((! pli->do_max) && (! pli->do_nohmm) && (! pli->do_mid)) { 
    if(esl_opt_IsOn(go, "--F1"))  { pli->do_msv      = TRUE; pli->F1  = esl_opt_GetReal(go, "--F1");  }
    if(esl_opt_IsOn(go, "--F1b")) { pli->do_msvbias  = TRUE; pli->F1b = esl_opt_GetReal(go, "--F1b"); }
    if(esl_opt_IsOn(go, "--F2"))  { pli->do_vit      = TRUE; pli->F2  = esl_opt_GetReal(go, "--F2");  }
    if(esl_opt_IsOn(go, "--F2b")) { pli->do_vitbias  = TRUE; pli->F2b = esl_opt_GetReal(go, "--F2b"); }
  }
  if((! pli->do_max) && (! pli->do_nohmm)) { 
    if(esl_opt_IsOn(go, "--F3"))  { pli->do_fwd        = TRUE; pli->F3  = esl_opt_GetReal(go, "--F3");  }
    if(esl_opt_IsOn(go, "--F3b")) { pli->do_fwdbias    = TRUE; pli->F3b = esl_opt_GetReal(go, "--F3b"); }
    if(esl_opt_IsOn(go, "--F4"))  { pli->do_gfwd       = TRUE; pli->F4  = esl_opt_GetReal(go, "--F4");  }
    if(esl_opt_IsOn(go, "--F4b")) { pli->do_gfwdbias   = TRUE; pli->F4b = esl_opt_GetReal(go, "--F4b"); }
    if(esl_opt_IsOn(go, "--F5"))  { pli->do_edef       = TRUE; pli->F5  = esl_opt_GetReal(go, "--F5");  }
    if(esl_opt_IsOn(go, "--F5b")) { pli->do_edefbias   = TRUE; pli->F5b = esl_opt_GetReal(go, "--F5b"); }
  }
  if(! pli->do_max) { 
    if(esl_opt_IsOn(go, "--F6"))  { pli->do_fcyk     = TRUE; pli->F6  = esl_opt_GetReal(go, "--F6");  }
  }
  
  if(esl_opt_GetBoolean(go, "--noF1"))     pli->do_msv  = FALSE; 
  if(esl_opt_GetBoolean(go, "--noF2"))     pli->do_vit  = FALSE; 
  if(esl_opt_GetBoolean(go, "--noF3"))     pli->do_fwd  = FALSE; 
  if(esl_opt_GetBoolean(go, "--noF4"))     pli->do_gfwd = FALSE; 
  if(esl_opt_GetBoolean(go, "--noF6"))     pli->do_fcyk = FALSE; 

  if((! pli->do_max) && (! pli->do_nohmm) && (! pli->do_mid)) { 
    if(esl_opt_GetBoolean(go, "--doF1b"))    pli->do_msvbias    = TRUE;
  }
  if(esl_opt_GetBoolean(go, "--noF2b"))    pli->do_vitbias  = FALSE;
  if(esl_opt_GetBoolean(go, "--noF3b"))    pli->do_fwdbias  = FALSE;
  if(esl_opt_GetBoolean(go, "--noF4b"))    pli->do_gfwdbias = FALSE;
  if(esl_opt_GetBoolean(go, "--doF5b"))    pli->do_edefbias = TRUE;

  /* Finished setting filter stage on/off parameters and thresholds */
  /********************************************************************************/

  /********************************************************************************/
  /* Configure options for the CM stages */
  pli->do_null2   = esl_opt_GetBoolean(go, "--null2")   ? TRUE  : FALSE;
  pli->do_null3   = esl_opt_GetBoolean(go, "--nonull3") ? FALSE : TRUE;

  pli->fcyk_cm_search_opts  = 0;
  pli->final_cm_search_opts = 0;
  pli->fcyk_beta  = esl_opt_GetReal(go, "--fbeta");
  pli->fcyk_tau   = esl_opt_GetReal(go, "--ftau");
  pli->do_fcykenv = esl_opt_GetBoolean(go, "--nocykenv") ? FALSE : TRUE;
  /* important to set F6env after F6 is set to final value */
  pli->F6env       = ESL_MIN(1.0, pli->F6 * (float) esl_opt_GetInteger(go, "--cykenvx")); 

  pli->final_beta = esl_opt_GetReal(go, "--beta");
  pli->final_tau  = esl_opt_GetReal(go, "--tau");

  /* There are 3 options for banding in CYK filter and final round.
   * Choice of the 3 varies depending on if pli->do_max, pli->do_nohmm
   * or neither.
   * 
   * if(do_max) {
   *   filter CYK is off. 
   *   final round: --qdb: use QDBs, else non-banded
   * }
   * else if(do_nohmm) {
   *   filter CYK : --fnonbanded: no bands, else use QDBs 
   *   final round: --nonbanded:  no bands, else use QDBs 
   * }
   * else {  normal case 
   *   filter CYK : --fnonbanded: no bands, --fqdb: use QDBs, else use HMM bands
   *   final round: --nonbanded:  no bands, --qdb:  use QDBs, else use HMM bands
   * }
   *
   * In all cases, if QDBs used for filter CYK beta is from --fbeta
   * <x>, final beta is from --beta <x>.  
   * 
   * In all cases, if HMM bands used for filter CYK tau is from --ftau
   * <x>, final tau is from --tau <x>.
   */

  /* CYK filter settings, only set these if do_fcyk 
   */
  if(pli->do_fcyk) { 
    if(pli->do_nohmm) { 
      /* special case: default behavior for fcyk is to do QDB, HMM banded is not allowed.
       */
      if(esl_opt_GetBoolean(go, "--fnonbanded"))       pli->fcyk_cm_search_opts  |= CM_SEARCH_NONBANDED;
      else                                             pli->fcyk_cm_search_opts  |= CM_SEARCH_QDB;
    }
    else { 
      if     (esl_opt_GetBoolean(go, "--fnonbanded"))  pli->fcyk_cm_search_opts  |= CM_SEARCH_NONBANDED;
      else if(esl_opt_GetBoolean(go, "--fqdb"))        pli->fcyk_cm_search_opts  |= CM_SEARCH_QDB;
      else                                             pli->fcyk_cm_search_opts  |= CM_SEARCH_HBANDED;
    }
    if(  esl_opt_GetBoolean(go, "--fsums"))            pli->fcyk_cm_search_opts  |= CM_SEARCH_SUMS;
    if(! esl_opt_GetBoolean(go, "--nonull3"))          pli->fcyk_cm_search_opts  |= CM_SEARCH_NULL3;
  }

  /* set up final round parameters, always set these (we always do the final CM round) */
  if(! esl_opt_GetBoolean(go, "--cyk"))                pli->final_cm_search_opts |= CM_SEARCH_INSIDE;
  if     (pli->do_max)   { /* special case, default behavior in final round is to do non-banded, HMM banded is not allowed */
    if(esl_opt_GetBoolean(go, "--qdb"))                pli->final_cm_search_opts |= CM_SEARCH_QDB;
    else                                               pli->final_cm_search_opts |= CM_SEARCH_NONBANDED;
  }
  else if(pli->do_nohmm) { /* special case, default behavior in final round is to do QDB, HMM banded is not allowed */
    if(esl_opt_GetBoolean(go, "--nonbanded"))          pli->final_cm_search_opts |= CM_SEARCH_NONBANDED;
    else                                               pli->final_cm_search_opts |= CM_SEARCH_QDB;
  }
  else { /* normal case, default is HMM banded */
    if     (esl_opt_GetBoolean(go, "--nonbanded"))     pli->final_cm_search_opts |= CM_SEARCH_NONBANDED;
    else if(esl_opt_GetBoolean(go, "--qdb"))           pli->final_cm_search_opts |= CM_SEARCH_QDB;
    else                                               pli->final_cm_search_opts |= CM_SEARCH_HBANDED;
  }
  if(esl_opt_GetBoolean(go, "--sums"))                 pli->final_cm_search_opts |= CM_SEARCH_SUMS;
  if(esl_opt_GetBoolean(go, "--nogreedy"))             pli->final_cm_search_opts |= CM_SEARCH_CMNOTGREEDY;

  /* Determine cm->config_opts and cm->align_opts we'll use to
   * configure CMs after reading within a SCAN pipeline. Search
   * options will change for the CYK filter and final stage, so
   * these are stored in fcyk_cm_search_opts and final_cm_search_opts
   * determined above.
  */
  pli->cm_config_opts = 0;
  pli->cm_align_opts = 0;
  /* should we configure CM/CP9 into local mode? */
  if(! esl_opt_GetBoolean(go, "-g")) { 
    pli->cm_config_opts |= CM_CONFIG_LOCAL;
    if(! esl_opt_GetBoolean(go, "--cp9gloc")) { 
      pli->cm_config_opts |= CM_CONFIG_HMMLOCAL;
      if(! esl_opt_GetBoolean(go, "--cp9noel")) pli->cm_config_opts |= CM_CONFIG_HMMEL; 
    }
  }
  /* should we setup for truncated alignments? */
  if(pli->do_trunc_ends || pli->do_trunc_any) pli->cm_config_opts |= CM_CONFIG_TRUNC; 

  /* will we be requiring a CM_SCAN_MX? a CM_TR_SCAN_MX? */
  if(pli->do_max   ||                    /* max mode, no filters */
     pli->do_nohmm ||                    /* nohmm mode, no HMM filters */
     esl_opt_GetBoolean(go, "--fqdb") || /* user specified to use --fqdb, do it */
     esl_opt_GetBoolean(go, "--qdb")) {  /* user specified to use --qdb,  do it */
    pli->cm_config_opts |= CM_CONFIG_SCANMX;
    if(pli->do_trunc_ends || pli->do_trunc_any) pli->cm_config_opts |= CM_CONFIG_TRSCANMX;
  }
  /* will we be requiring non-banded alignment matrices? */
  if(esl_opt_GetBoolean(go, "--anonbanded") || pli->do_max || pli->do_nohmm) { 
    pli->cm_config_opts |= CM_CONFIG_NONBANDEDMX;
    pli->cm_align_opts |= CM_ALIGN_NONBANDED;
    pli->cm_align_opts |= CM_ALIGN_SMALL;
    pli->cm_align_opts |= CM_ALIGN_CYK;
    /* D&C truncated alignment is not robust, so we don't allow it */
    pli->do_trunc_ends = FALSE;
    pli->do_trunc_any  = FALSE;
  }
  else { 
    pli->cm_align_opts |= CM_ALIGN_HBANDED;
    pli->cm_align_opts |= CM_ALIGN_POST; 
  }
  if(esl_opt_GetBoolean(go, "--acyk")) { 
    pli->cm_align_opts |= CM_ALIGN_CYK;
  }
  else { 
    pli->cm_align_opts |= CM_ALIGN_OPTACC;
  }

  /* Determine statistics modes for CM stages */
  pli->do_glocal_cm_stages = (esl_opt_GetBoolean(go, "-g")) ? TRUE : FALSE;
  pli->fcyk_cm_exp_mode    = pli->do_glocal_cm_stages ? EXP_CM_GC : EXP_CM_LC;
  if(pli->final_cm_search_opts & CM_SEARCH_INSIDE) { 
    pli->final_cm_exp_mode = pli->do_glocal_cm_stages ? EXP_CM_GI : EXP_CM_LI;
  }
  else {
    pli->final_cm_exp_mode = pli->do_glocal_cm_stages ? EXP_CM_GC : EXP_CM_LC;
  }
  /* finished setting up parameters for CM stages */
  /********************************************************************************/

  return pli;

 ERROR:
  cm_pipeline_Destroy(pli, NULL);
  return NULL;
}

/* Function:  cm_pipeline_Reuse()
 * Synopsis:  Reuse a pipeline for next target.
 * Incept:    EPN, Fri Sep 24 16:28:55 2010
 *            SRE, Fri Dec  5 10:31:36 2008 [Janelia] (p7_pipeline_Reuse())
 *
 * Purpose:   Reuse <pli> for next target sequence (search mode)
 *            or model (scan mode). 
 *            
 *            May eventually need to distinguish from reusing pipeline
 *            for next query, but we're not really focused on multiquery
 *            use of hmmscan/hmmsearch/phmmer for the moment.
 */
int
cm_pipeline_Reuse(CM_PIPELINE *pli)
{
  p7_omx_Reuse(pli->oxf);
  p7_omx_Reuse(pli->oxb);
  p7_omx_Reuse(pli->fwd);
  p7_omx_Reuse(pli->bck);
  p7_domaindef_Reuse(pli->ddef);
  /* TO DO, write scanmatrixreuse */
  return eslOK;
}


/* Function:  cm_pipeline_Destroy()
 * Synopsis:  Free a <CM_PIPELINE> object.
 * Incept:    EPN, Fri Sep 24 16:29:34 2010
 *            SRE, Fri Dec  5 10:30:23 2008 [Janelia] (p7_pipeline_Destroy())
 *
 * Purpose:   Free a <CM_PIPELINE> object. Requires a cm (sigh) to free the scan matrix.
 */
void
cm_pipeline_Destroy(CM_PIPELINE *pli, CM_t *cm)
{
  if (pli == NULL) return;
  
  p7_omx_Destroy(pli->oxf);
  p7_omx_Destroy(pli->oxb);
  p7_omx_Destroy(pli->fwd);
  p7_omx_Destroy(pli->bck);
  p7_gmx_Destroy(pli->gfwd);
  p7_gmx_Destroy(pli->gbck);
  p7_gmx_Destroy(pli->gxf);
  p7_gmx_Destroy(pli->gxb);
  esl_randomness_Destroy(pli->r);
  p7_domaindef_Destroy(pli->ddef);
  free(pli);
}
/*---------------- end, CM_PIPELINE object ----------------------*/


/*****************************************************************
 * 2. Pipeline API.
 *****************************************************************/

/* Function:  cm_pli_TargetReportable
 * Synopsis:  Returns TRUE if target score meets reporting threshold.
 * Incept:    EPN, Fri Sep 24 16:34:37 2010
 *            SRE, Tue Dec  9 08:57:26 2008 [Janelia] (p7_pli_TargetReportable())
 *
 * Purpose:   Returns <TRUE> if the bit score <score> and/or 
 *            E-value <Eval> meets per-target reporting thresholds 
 *            for the processing pipeline.
 */
int
cm_pli_TargetReportable(CM_PIPELINE *pli, float score, double Eval)
{
  if      (  pli->by_E && Eval  <= pli->E) return TRUE;
  else if (! pli->by_E && score >= pli->T) return TRUE;
  return FALSE;
}

/* Function:  cm_pli_TargetIncludable()
 * Synopsis:  Returns TRUE if target score meets inclusion threshold.
 * Incept:    EPN, Fri Sep 24 16:32:43 2010
 *            SRE, Fri Jan 16 11:18:08 2009 [Janelia] (p7_pli_TargetIncludable())
 */
int
cm_pli_TargetIncludable(CM_PIPELINE *pli, float score, double Eval)
{
  if      (  pli->by_E && Eval  <= pli->incE) return TRUE;
  else if (! pli->by_E && score >= pli->incT) return TRUE;

  return FALSE;
}

/* Function:  cm_pli_NewModel()
 * Synopsis:  Prepare pipeline for a new CM/HMM
 * Incept:    EPN, Fri Sep 24 16:35:35 2010
 *            SRE, Fri Dec  5 10:35:37 2008 [Janelia] (p7_pli_NewModel())
 *
 * Purpose:   Caller has a new model.
 *            Prepare the pipeline <pli> to receive this model as either 
 *            a query or a target.
 *
 *            The information of the model we may receive varies, as
 *            indicated by <modmode> and <pli->mode>. This is enforced 
 *            by a contract check upon entrance, failure causes immediate
 *            return of eslEINCOMPAT.
 *
 *      case  <pli->mode>     <modmode>         <cm>      <om> and <bg>
 *      ----  --------------  ---------------   --------  -------------
 *         1  CM_SEARCH_SEQS  CM_NEWMODEL_CM    non-null  non-null     
 *         2  CM_SCAN_SEQS    CM_NEWMODEL_MSV   NULL      non-null     
 *         3  CM_SCAN_SEQS    CM_NEWMODEL_CM    non-null  NULL         
 *
 *            <cm_clen> and <cm_W> are always valid, but are only 
 *            necessary for case 2.
 *
 *            Note that if we're in SEARCH mode, <modmode> will 
 *            always be CM_NEWMODEL_CM. In SCAN mode, we may only
 *            call this function once with <modmode> == CM_NEWMODEL_MSV
 *            (case 2 above, this happens if no hit from the query
 *            sequence survives to the CM stage). Also, if we
 *            are in SCAN mode with modmode==CM_NEWMODEL_CM we must
 *            have entered this function previously for the same
 *            'model' with modmode==CM_NEWMODEL_MSV.
 *
 *            The pipeline may alter the null model in <bg> in a
 *            model-specific way (if we're using composition bias
 *            filter HMMs in the pipeline).
 *
 * Returns:   <eslOK> on success.
 *
 *            <eslEINCOMPAT> in contract is not met.
 * 
 *            <eslEINVAL> if pipeline expects to be able to use a
 *            model's bit score thresholds, but this model does not
 *            have the appropriate ones set.
 */
int
cm_pli_NewModel(CM_PIPELINE *pli, int modmode, CM_t *cm, int cm_clen, int cm_W, P7_OPROFILE *om, P7_BG *bg, int64_t cur_cm_idx)
{
  int status = eslOK;
  float T;

  /* check contract */
  if(pli->mode == CM_SEARCH_SEQS) { /* case 1 */
    if(modmode != CM_NEWMODEL_CM) 
    if(cm == NULL)                 ESL_FAIL(eslEINCOMPAT, pli->errbuf, "cm_pli_NewModel(), contract violated, SEARCH mode and CM is NULL"); 
    if(cm->clen != cm_clen)        ESL_FAIL(eslEINCOMPAT, pli->errbuf, "cm_pli_NewModel(), contract violated, cm->clen != cm_clen"); 
    if(cm->W    != cm_W)           ESL_FAIL(eslEINCOMPAT, pli->errbuf, "cm_pli_NewModel(), contract violated, cm->W != cm_W"); 
    if(om == NULL)                 ESL_FAIL(eslEINCOMPAT, pli->errbuf, "cm_pli_NewModel(), contract violated, SEARCH mode and om is NULL"); 
    if(bg == NULL)                 ESL_FAIL(eslEINCOMPAT, pli->errbuf, "cm_pli_NewModel(), contract violated, SEARCH mode and bg is NULL"); 
  }
  else if(pli->mode == CM_SCAN_MODELS) { 
    if(modmode == CM_NEWMODEL_MSV) { /* case 2 */
      if(cm != NULL)              ESL_FAIL(eslEINCOMPAT, pli->errbuf, "cm_pli_NewModel(), contract violated, SCAN/MSV mode, and CM is non-NULL"); 
      if(om == NULL)              ESL_FAIL(eslEINCOMPAT, pli->errbuf, "cm_pli_NewModel(), contract violated, SCAN/MSV mode, and om is NULL"); 
      if(bg == NULL)              ESL_FAIL(eslEINCOMPAT, pli->errbuf, "cm_pli_NewModel(), contract violated, SCAN/MSV mode, and bg is NULL"); 
    }
    else if(modmode == CM_NEWMODEL_CM) { /* case 3 */
      if(cm == NULL)              ESL_FAIL(eslEINCOMPAT, pli->errbuf, "cm_pli_NewModel(), contract violated, SCAN/CM mode, and CM is NULL"); 
      if(cm->clen != cm_clen)     ESL_FAIL(eslEINCOMPAT, pli->errbuf, "cm_pli_NewModel(), contract violated, cm->clen != cm_clen");
      if(cm->W    != cm_W)        ESL_FAIL(eslEINCOMPAT, pli->errbuf, "cm_pli_NewModel(), contract violated, cm->W != cm_W");
      if(om != NULL)              ESL_FAIL(eslEINCOMPAT, pli->errbuf, "cm_pli_NewModel(), contract violated, SCAN/CM mode, and om is non-NULL"); 
      if(bg != NULL)              ESL_FAIL(eslEINCOMPAT, pli->errbuf, "cm_pli_NewModel(), contract violated, SCAN/CM mode, and bg is non-NULL"); 
    }
  }

  pli->cur_cm_idx = cur_cm_idx;

  /* Two sets (A and B) of value updates: 
   * case 1: we do both sets 
   * case 2: we do set A only
   * case 3: we do set B only
   */
  if(pli->mode == CM_SEARCH_SEQS || modmode == CM_NEWMODEL_MSV) { 
    /* set A updates: case 1 and 2 do these */
    pli->nmodels++;
    pli->nnodes += cm_clen;

    if (pli->do_msvbias || pli->do_vitbias || pli->do_fwdbias || pli->do_gfwdbias || pli->do_edefbias) { 
      p7_bg_SetFilter(bg, om->M, om->compo);
    }
    /* copy some values from the model */
    pli->cmW  = cm_W;
    pli->clen = cm_clen;
    /* determine pli->maxW, this will be the number of residues that
     * must overlap between adjacent windows on a single sequence,
     * this is MAX of cm->W and pli->cmult * cm->clen.
     */
    pli->maxW = ESL_MAX(pli->wmult * cm_W, pli->cmult * cm_clen);
  }
  if(pli->mode == CM_SEARCH_SEQS || modmode == CM_NEWMODEL_CM) { 
    /* set B updates: case 1 and 3 do these (they require a valid CM) */

    /* Update the current effective database size so it pertains to the
     * new model. Also, If we're using an E-value threshold determine
     * the bit score for this model that pertains to that E-value.
     */
    if((status = UpdateExpsForDBSize(cm, pli->errbuf, (long) pli->Z)) != eslOK) return status;
    if(pli->by_E) { 
      if((status = E2ScoreGivenExpInfo(cm->expA[pli->final_cm_exp_mode], pli->errbuf, pli->E, &T)) != eslOK) ESL_FAIL(status, pli->errbuf, "problem determining min score for E-value %6g for model %s\n", pli->E, cm->name);
      pli->T = (double) T;
    }

    /* if we're using Rfam GA, NC, or TC cutoffs, update them for this model */
    if (pli->use_bit_cutoffs) { 
      if((status = cm_pli_NewModelThresholds(pli, cm)) != eslOK) return status;
    }
  }
  return eslOK;
}

/* Function:  cm_pli_NewModelThresholds()
 * Synopsis:  Set reporting and inclusion bit score thresholds on a new model.
 * Incept:    EPN, Wed Jun 15 14:40:46 2011
 *            SRE, Sat Oct 17 12:07:43 2009 [Janelia] (p7_pli_NewModelThresholds()
 *
 * Purpose:   Set the bit score thresholds on a new model, if we're 
 *            either using Rfam GA, TC, or NC cutoffs for reporting or
 *            inclusion, and/or if we already know the total 
 *            database size.
 *            
 *            In a "search" pipeline, this only needs to be done once
 *            per query model, so <cm_pli_NewModelThresholds()> gets 
 *            called by <cm_pli_NewModel()>.
 *            
 *            In a "scan" pipeline, this needs to be called for each
 *            target model.
 *
 * Returns:   <eslOK> on success. 
 *            
 *            <eslEINVAL> if pipeline expects to be able to use a
 *            model's bit score thresholds, but this model does not
 *            have the appropriate ones set.
 *
 * Xref:      Written to fix bug #h60 (p7_pli_NewModelThreshold())
 */
int
cm_pli_NewModelThresholds(CM_PIPELINE *pli, CM_t *cm)
{

  if (pli->use_bit_cutoffs) { 
    if(pli->use_bit_cutoffs == CMH_GA) {
      if (! (cm->flags & CMH_GA)) ESL_FAIL(eslEINVAL, pli->errbuf, "GA bit threshold unavailable for model %s\n", cm->name);
      pli->T = pli->incT = cm->ga;
    }
    else if(pli->use_bit_cutoffs == CMH_TC) {
      if (! (cm->flags & CMH_TC)) ESL_FAIL(eslEINVAL, pli->errbuf, "TC bit threshold unavailable for model %s\n", cm->name);
      pli->T = pli->incT = cm->tc;
    }
    else if(pli->use_bit_cutoffs == CMH_NC) {
      if (! (cm->flags & CMH_NC)) ESL_FAIL(eslEINVAL, pli->errbuf, "NC bit threshold unavailable for model %s\n", cm->name);
      pli->T = pli->incT = cm->nc;
    }
  }
  return eslOK;
}

/* Function:  cm_pli_NewSeq()
 * Synopsis:  Prepare pipeline for a new sequence (target or query)
 * Incept:    EPN, Fri Sep 24 16:39:52 2010
 *            SRE, Fri Dec  5 10:57:15 2008 [Janelia] (p7_pli_NewSeq())
 *
 * Purpose:   Caller has a new sequence <sq>. Prepare the pipeline <pli>
 *            to receive this model as either a query or a target.
 *
 * Returns:   <eslOK> on success.
 */
 int
 cm_pli_NewSeq(CM_PIPELINE *pli, const ESL_SQ *sq, int64_t cur_seq_idx)
{
  /* Update number of residues read/searched in standard pipeline passes */
  pli->acct[PLI_PASS_STD_ANY].nres += sq->n;

  /* Update cur_seq_idx, which is a unique identifier for the sequence, so 
   * we can reliably remove overlaps. This index is copied to all the 
   * hit objects for all hits found by the pipeline when searching this sequence. */
  pli->cur_seq_idx = cur_seq_idx;

  /* Note that we don't update pli->Z, that must be set at beginning
   * of a search, this differs from hmmsearch and nhmmer, which by
   * default update Z as sequences are read.
   */
  return eslOK;
}

/* Function:  cm_pipeline_Merge()
 * Synopsis:  Merge the pipeline statistics
 * Incept:    EPN, Fri Sep 24 16:41:17 2010   
 *
 * Purpose:   Caller has a new model <om>. Prepare the pipeline <pli>
 *            to receive this model as either a query or a target.
 *
 *            The pipeline may alter the null model <bg> in a model-specific
 *            way (if we're using a composition bias filter HMM in the
 *            pipeline).
 *
 * Returns:   <eslOK> on success.
 * 
 *            <eslEINVAL> if pipeline expects to be able to use a
 *            model's bit score thresholds, but this model does not
 *            have the appropriate ones set.
 */
int
cm_pipeline_Merge(CM_PIPELINE *p1, CM_PIPELINE *p2)
{
  /* if we are searching a sequence database, we need to keep track of the
   * number of sequences and residues processed.
   */
  int p; /* counter over pipeline passes */

  if (p1->mode == CM_SEARCH_SEQS)
    {
      p1->nseqs   += p2->nseqs;
      for(p = 0; p < NPLI_PASSES; p++) p1->acct[p].nres += p2->acct[p].nres;
    }
  else
    {
      p1->nmodels += p2->nmodels;
      p1->nnodes  += p2->nnodes;
    }

  for(p = 0; p < NPLI_PASSES; p++) { 
    p1->acct[p].n_past_msv  += p2->acct[p].n_past_msv;
    p1->acct[p].n_past_vit  += p2->acct[p].n_past_vit;
    p1->acct[p].n_past_fwd  += p2->acct[p].n_past_fwd;
    p1->acct[p].n_past_gfwd += p2->acct[p].n_past_gfwd;
    p1->acct[p].n_past_edef += p2->acct[p].n_past_edef;
    p1->acct[p].n_past_cyk  += p2->acct[p].n_past_cyk;
    p1->acct[p].n_past_ins  += p2->acct[p].n_past_ins;
    p1->acct[p].n_output    += p2->acct[p].n_output;

    p1->acct[p].n_past_msvbias += p2->acct[p].n_past_msvbias;
    p1->acct[p].n_past_vitbias += p2->acct[p].n_past_vitbias;
    p1->acct[p].n_past_fwdbias += p2->acct[p].n_past_fwdbias;
    p1->acct[p].n_past_gfwdbias+= p2->acct[p].n_past_gfwdbias;
    p1->acct[p].n_past_edefbias += p2->acct[p].n_past_edefbias;

    p1->acct[p].pos_past_msv  += p2->acct[p].pos_past_msv;
    p1->acct[p].pos_past_vit  += p2->acct[p].pos_past_vit;
    p1->acct[p].pos_past_fwd  += p2->acct[p].pos_past_fwd;
    p1->acct[p].pos_past_gfwd += p2->acct[p].pos_past_gfwd;
    p1->acct[p].pos_past_edef += p2->acct[p].pos_past_edef;
    p1->acct[p].pos_past_cyk  += p2->acct[p].pos_past_cyk;
    p1->acct[p].pos_past_ins  += p2->acct[p].pos_past_ins;
    p1->acct[p].pos_output    += p2->acct[p].pos_output;

    p1->acct[p].pos_past_msvbias += p2->acct[p].pos_past_msvbias;
    p1->acct[p].pos_past_vitbias += p2->acct[p].pos_past_vitbias;
    p1->acct[p].pos_past_fwdbias += p2->acct[p].pos_past_fwdbias;
    p1->acct[p].pos_past_gfwdbias+= p2->acct[p].pos_past_gfwdbias;
    p1->acct[p].pos_past_edefbias += p2->acct[p].pos_past_edefbias;

    p1->acct[p].n_overflow_fcyk  += p2->acct[p].n_overflow_fcyk;
    p1->acct[p].n_overflow_final += p2->acct[p].n_overflow_final;
    p1->acct[p].n_aln_hb         += p2->acct[p].n_aln_hb;
    p1->acct[p].n_aln_dccyk      += p2->acct[p].n_aln_dccyk;
  }

  return eslOK;
}

/* Function:  cm_Pipeline()
 * Synopsis:  The accelerated seq/profile comparison pipeline using HMMER3 scanning.
 * Incept:    EPN, Fri Sep 24 16:42:21 2010
 *            TJW, Fri Feb 26 10:17:53 2018 [Janelia] (p7_Pipeline_Longtargets())
 *
 * Purpose:   Run the accelerated pipeline to compare profile <om>
 *            against sequence <sq>. This function calls other
 *            functions specific to each stage of the pipeline: 
 *            pli_p7_filter(), pli_p7_env_def, pli_cyk_env_filter(), 
 *            pli_cyk_seq_filter(), pli_final_stage(). 
 *
 * Returns:   <eslOK> on success. If a significant hit is obtained,
 *            its information is added to the growing <hitlist>.
 *
 *            <eslEINVAL> if (in a scan pipeline) we're supposed to
 *            set GA/TC/NC bit score thresholds but the model doesn't
 *            have any.
 *
 *            <eslERANGE> on numerical overflow errors in the
 *            optimized vector implementations; particularly in
 *            posterior decoding. I don't believe this is possible for
 *            multihit local models, but I'm set up to catch it
 *            anyway. We may emit a warning to the user, but cleanly
 *            skip the problematic sequence and continue.
 *
 * Throws:    <eslEMEM> on allocation failure.
 *
 * Xref:      J4/25.
 */
int
cm_Pipeline(CM_PIPELINE *pli, off_t cm_offset, P7_OPROFILE *om, P7_BG *bg, float *p7_evparam, P7_MSVDATA *msvdata, ESL_SQ *sq, CM_TOPHITS *hitlist, P7_HMM **opt_hmm, P7_PROFILE **opt_gm, P7_PROFILE **opt_Rgm, P7_PROFILE **opt_Lgm, P7_PROFILE **opt_Tgm, CM_t **opt_cm)
{
  int       status;
  int       nwin = 0;     /* number of windows surviving MSV & Vit & lFwd, filled by pli_p7_filter() */
  int64_t  *ws = NULL;    /* [0..i..nwin-1] window start positions, filled by pli_p7_filter() */
  int64_t  *we = NULL;    /* [0..i..nwin-1] window end   positions, filled by pli_p7_filter() */
  int       np7env = 0;   /* number of envelopes surviving MSV & Vit & lFwd & gFwd & EnvDef, filled by pli_p7_env_def */
  int64_t  *p7es  = NULL; /* [0..i..np7env-1] window start positions, filled by pli_p7_env_def() */
  int64_t  *p7ee  = NULL; /* [0..i..np7env-1] window end   positions, filled by pli_p7_env_def() */
  int       nenv = 0;     /* number of envelopes surviving CYK filter, filled by pli_cyk_env_filter() or pli_cyk_seq_filter() */
  int64_t  *es  = NULL;   /* [0..i..nenv-1] window start positions, filled by pli_cyk_env_filter() or pli_cyk_seq_filter() */
  int64_t  *ee  = NULL;   /* [0..i..nenv-1] window end   positions, filled by pli_cyk_env_filter() or pli_cyk_seq_filter() */
  int       i;            /* counter over envelopes */

  /* variables necessary for re-searching sequence ends */
  ESL_SQ   *sq2search = NULL;  /* a pointer to the sequence to search on current pass */
  ESL_SQ   *term5sq   = NULL;  /* a copy of the 5'-most pli->maxW residues from sq */
  ESL_SQ   *term3sq   = NULL;  /* a copy of the 3'-most pli->maxW residues from sq */
  int       p;                 /* counter over passes */
  int       nwin_pass_std_any = 0; /* number of windows that survived pass 1 (PLI_PASS_STD_ANY) */
  int       do_pass_5p_only_force;   /* should we do pass 2 (PLI_PASS_5P_ONLY_FORCE)? re-search the 5'-most pli->maxW residues */
  int       do_pass_3p_only_force;   /* should we do pass 3 (PLI_PASS_3P_ONLY_FORCE)? re-search the 3'-most pli->maxW residues */
  int       do_pass_5p_and_3p_force; /* should we do pass 4 (PLI_PASS_5P_AND_3P_FORCE)? re-search the full sequence allowing 5' and 3' truncated hits? */
  int       do_pass_5p_and_3p_any;   /* should we do pass 5 (PLI_PASS_5P_AND_3P_ANY)?   re-search the full sequence allowing for all truncated hits? */
  int       have5term;         /* TRUE if sq contains the 5'-most pli->maxW residues of its source sequence */
  int       have3term;         /* TRUE if sq contains the 3'-most pli->maxW residues of its source sequence */
  int       h;                 /* counter over hits */
  int       prv_ntophits;      /* number of hits */
  int64_t   start_offset;      /* offset to add to start/stop coordinates of hits found in pass 3, in which we re-search the 3' terminus */

  if (sq->n == 0) return eslOK;    /* silently skip length 0 seqs; they'd cause us all sorts of weird problems */

  /* Determine if we have the 5' and/or 3' terminii. We can do this
   * because sq->L should always be valid. (Caller should enforce
   * this, but it takes some effort if caller is potentially reading
   * subsequences of large sequences. For example, cmsearch does an
   * initial readthrough of the entire target database file storing
   * the sequence lengths prior to doing any cm_Pipeline() calls (or
   * it uses sequence length info in an SSI index).
   */
  if(sq->start <= sq->end) { /* not in reverse complement (or 1 residue sequence, in revcomp) */
    have5term = (sq->start == 1)     ? TRUE : FALSE;
    have3term = (sq->end   == sq->L) ? TRUE : FALSE;
  }
  else { /* in reverse complement */
    have5term = (sq->start == sq->L) ? TRUE : FALSE;
    have3term = (sq->end   == 1)     ? TRUE : FALSE;
  }

#if DEBUGPIPELINE
  /*printf("\nPIPELINE ENTRANCE %s  %s  %" PRId64 " residues (pli->maxW: %d om->max_length: %d cm->W: %d)\n", sq->name, sq->desc, sq->n, pli->maxW, om->max_length, (*opt_cm)->W);*/
  printf("\nPIPELINE ENTRANCE %-15s  (n: %6" PRId64 " start: %6" PRId64 " end: %6" PRId64 " C: %6" PRId64 " W: %6" PRId64 " L: %6" PRId64 " have5term: %d have3term: %d)\n",
	 sq->name, sq->n, sq->start, sq->end, sq->C, sq->W, sq->L, have5term, have3term);
#endif

  /* determine which passes (besides the mandatory 1st standard pass
   * PLI_PASS_STD_ANY) we'll need to do for this sequence. The 'do_'
   * variables indicated which type of truncations are allowed in each
   * pass; e.g. do_pass_5p_only_force: only 5' truncations are allowed in
   * that pass - we do this pass if <do_trunc_ends> is TRUE and
   * have5term is TRUE.
   */
  if(pli->do_trunc_ends) { 
    do_pass_5p_only_force   = have5term ? TRUE : FALSE;
    do_pass_3p_only_force   = have3term ? TRUE : FALSE;
    do_pass_5p_and_3p_force = (have5term && have3term && sq->n <= pli->maxW) ? TRUE : FALSE;
    do_pass_5p_and_3p_any   = FALSE;
  }
  else if(pli->do_trunc_any) { /* we allow any truncated hit, in a special pass PLI_PASS_5P_AND_3P_ANY */
    do_pass_5p_and_3p_any = TRUE;
    do_pass_5p_only_force = do_pass_3p_only_force = do_pass_5p_and_3p_force = FALSE;
  }
  else { /* we're not allowing any truncated hits */
    do_pass_5p_only_force = do_pass_3p_only_force = do_pass_5p_and_3p_force = do_pass_5p_and_3p_any = FALSE;
  }

  for(p = PLI_PASS_STD_ANY; p < NPLI_PASSES; p++) { /* p will go from 1..5 */
    if(p == PLI_PASS_5P_ONLY_FORCE   && (! do_pass_5p_only_force))   continue;
    if(p == PLI_PASS_3P_ONLY_FORCE   && (! do_pass_3p_only_force))   continue;
    if(p == PLI_PASS_5P_AND_3P_FORCE && (! do_pass_5p_and_3p_force)) continue;
    if(p == PLI_PASS_5P_AND_3P_ANY   && (! do_pass_5p_and_3p_any))   continue;

    /* Update nres for non-standard pass (this is done by caller via
     * cm_pli_NewSeq() for pass PLI_PASS_STD_ANY) It's important to do
     * this precisely here, in between the 'continue's above, but
     * prior the one below that 'continue's only because we know no
     * windows pass local Fwd (F3).
     */
    if     (p == PLI_PASS_5P_AND_3P_ANY) pli->acct[p].nres += sq->n;
    else if(p != PLI_PASS_STD_ANY)       pli->acct[p].nres += ESL_MIN(pli->maxW, sq->n);

    /* if we know there's no windows that pass local Fwd (F3), our terminal (or full) seqs won't have any either, continue */
    if(p != PLI_PASS_STD_ANY && nwin_pass_std_any == 0 && (! pli->do_max)) continue; 
    
    /* set sq2search and remember start_offset */
    start_offset = 0;
    if(p == PLI_PASS_STD_ANY || p == PLI_PASS_5P_AND_3P_FORCE || p == PLI_PASS_5P_AND_3P_ANY || sq->n <= pli->maxW) { 
      sq2search = sq;
    }
    else if(p == PLI_PASS_5P_ONLY_FORCE) { /* research first (5') pli->maxW residues */
      term5sq = esl_sq_CreateDigital(bg->abc);
      copy_subseq(sq, term5sq, 1, pli->maxW);
      sq2search = term5sq;
    }
    else if(p == PLI_PASS_3P_ONLY_FORCE) { /* research first (5') pli->maxW residues */
      term3sq = esl_sq_CreateDigital(bg->abc);
      copy_subseq(sq, term3sq, sq->n - pli->maxW + 1, pli->maxW);
      sq2search = term3sq;
      start_offset = sq->n - pli->maxW;
    }
    pli->cur_pass_idx = p;

    /********************************************************************************************************/
    /* Execute the filter pipeline:
     * Goal of filters is to define envelopes in 1 of 3 ways: 
     * 1. using a p7 hmm (if pli->do_edef == TRUE) 
     * 2. using CYK      (if pli->do_edef == FALSE && pli->fcyk = TRUE)
     * 3. each full seq is an envelope (no filters, if pli->do_edef == FALSE && pli->fcyk = FALSE)
     */

    /********************************************************************************************************/
    /* 1. using a p7 hmm (if pli->do_edef == TRUE) */
    if(pli->do_edef) { 
      /* Defining envelopes with p7 HMM: 
       * A. pli_p7_filter(): MSV, Viterbi, local Forward filters 
       * B. pli_p7_env_def(): glocal Forward, and glocal (usually) HMM envelope definition, then
       * C. pli_cyk_env_filter():  CYK filter, run on each envelope defined by the HMM 
       */
#if DEBUGPIPELINE
      printf("\nPIPELINE calling p7_filter() %s  %" PRId64 " residues (pass: %d)\n", sq2search->name, sq2search->n, p);
#endif
      if((status = pli_p7_filter(pli, om, bg, p7_evparam, msvdata, sq2search, &ws, &we, &nwin)) != eslOK) return status;
      if(p == PLI_PASS_STD_ANY) nwin_pass_std_any = nwin;
      if(pli->do_time_F1 || pli->do_time_F2 || pli->do_time_F3) return status;
#if DEBUGPIPELINE
      printf("\nPIPELINE calling p7_env_def() %s  %" PRId64 " residues (pass: %d)\n", sq2search->name, sq2search->n, p);
#endif
      if((status = pli_p7_env_def(pli, om, bg, p7_evparam, sq2search, ws, we, nwin, opt_hmm, opt_gm, opt_Rgm, opt_Lgm, opt_Tgm, &p7es, &p7ee, &np7env)) != eslOK) return status;
      if(pli->do_time_F1 || pli->do_time_F2 || pli->do_time_F3) return status;

      if(pli->do_fcyk) { 
#if DEBUGPIPELINE
	printf("\nPIPELINE calling pli_cyk_env_filter() %s  %" PRId64 " residues (pass: %d)\n", sq2search->name, sq2search->n, p);
#endif
	if((status = pli_cyk_env_filter(pli, cm_offset, sq2search, p7es, p7ee, np7env, opt_cm, &es, &ee, &nenv)) != eslOK) return status;
	if(pli->do_time_F4 || pli->do_time_F5) return status;
      }
      else { /* defined envelopes with HMM, but CYK filter is off: act as if p7-defined envelopes survived CYK */
	ESL_ALLOC(es, sizeof(int64_t) * np7env);
	ESL_ALLOC(ee, sizeof(int64_t) * np7env);
	for(i = 0; i < np7env; i++) { es[i] = p7es[i]; ee[i] = p7ee[i]; } 
	nenv = np7env;
      }
    }
    /* end of '1. using a p7 hmm (if pli->do_edef == TRUE)' */
    /********************************************************************************************************/
    /* 2. using CYK (if pli->do_edef == FALSE && pli->fcyk = TRUE) */
    else if(pli->do_fcyk) { 
      /* Defining envelopes with CYK */
#if DEBUGPIPELINE
      printf("\nPIPELINE calling pli_cyk_seq_filterf() %s  %" PRId64 " residues (pass: %d)\n", sq2search->name, sq2search->n, p);
#endif
      if((status = pli_cyk_seq_filter(pli, cm_offset, sq2search, opt_cm, &es, &ee, &nenv)) != eslOK) return status;
    }
    /* end of '2. using CYK (if pli->do_edef == FALSE && pli->fcyk = TRUE)' */
    /********************************************************************************************************/
    /* 3. each full seq is an envelope (no filters, if pli->do_edef == FALSE && pli->fcyk = FALSE) */
    else { 
      /* No filters, (pli->do_edef and pli->do_fcyk are both FALSE)
       *  each full sequence becomes an 'envelope' to be searched in
       *  the final stage.
       */
      ESL_ALLOC(es, sizeof(int64_t) * 1);
      ESL_ALLOC(ee, sizeof(int64_t) * 1);
      nenv = 1; es[0] = 1; ee[0] = sq2search->n;
    }
    /* end of '3. each full seq is an envelope (no filters, if pli->do_edef == FALSE && pli->fcyk = FALSE)' */
    /********************************************************************************************************/
    if(pli->do_time_F6) return status;
      
    /* Filters are finished. Final stage of pipeline (always run).
     */
#if DEBUGPIPELINE
    printf("\nPIPELINE calling FinalStage() %s  %" PRId64 " residues (pass: %d)\n", sq2search->name, sq2search->n, p);
#endif
    prv_ntophits = hitlist->N;
    if((status = pli_final_stage(pli, cm_offset, sq2search, es, ee, nenv, hitlist, opt_cm)) != eslOK) return status;

    /* if we're researching a 3' terminus, adjust the start/stop
     * positions so they are relative to the actual 5' start 
     */
    if(hitlist->N > prv_ntophits) { 
      if(start_offset != 0) { /* this will only be non-zero if we're in pass PLI_PASS_3P_ONLY_FORCE */
	for(h = prv_ntophits; h < hitlist->N; h++) { 
	  hitlist->unsrt[h].start += start_offset;
	  hitlist->unsrt[h].stop  += start_offset;
	  if(hitlist->unsrt[h].ad != NULL) { 
	    hitlist->unsrt[h].ad->sqfrom += start_offset;
	    hitlist->unsrt[h].ad->sqto   += start_offset;
	  }
	}
      }
    }

    if(ws   != NULL) { free(ws);   ws   = NULL; }
    if(we   != NULL) { free(we);   we   = NULL; }
    if(p7es != NULL) { free(p7es); p7es = NULL; }
    if(p7ee != NULL) { free(p7ee); p7ee = NULL; }
    if(es   != NULL) { free(es);   es   = NULL; }
    if(ee   != NULL) { free(ee);   ee   = NULL; }
    nwin = np7env = nenv = 0;
  }
  
  if(term5sq != NULL) esl_sq_Destroy(term5sq);
  if(term3sq != NULL) esl_sq_Destroy(term3sq);

  return eslOK;

 ERROR: 
  ESL_FAIL(eslEMEM, pli->errbuf, "out of memory");
}
  
/* Function:  cm_pli_Statistics()
 * Synopsis:  Final stats output for all passes of a pipeline.
 * Incept:    EPN, Thu Feb 16 15:19:35 2012
 *
 * Purpose:   Print a standardized report of the internal statistics of
 *            a finished processing pipeline <pli> to stream <ofp>.
 *            If pli->do_allstats, print statistics for each pass, 
 *            else only print statistics for the standard pass.
 *
 *            Actual work is done by repeated calls to 
 *            cm_pli_PassStatistics().
 *            
 * Returns:   <eslOK> on success.
 */
int
cm_pli_Statistics(FILE *ofp, CM_PIPELINE *pli, ESL_STOPWATCH *w)
{
  if(! pli->do_allstats) { /* not in verbose mode, only print standard pass statistics */
    cm_pli_PassStatistics(ofp, pli, PLI_PASS_STD_ANY, w); fprintf(ofp, "//\n");
  }
  else { 
    /* compute summed statistics for all passes */
    cm_pli_SumStatistics(pli);
    /* There's 3 possible sets of passes that we've performed: */
    if(pli->do_trunc_ends) { 
      cm_pli_PassStatistics(ofp, pli, PLI_PASS_SUMMED,          NULL); fprintf(ofp, "\n");
      cm_pli_PassStatistics(ofp, pli, PLI_PASS_STD_ANY,         NULL); fprintf(ofp, "\n");
      cm_pli_PassStatistics(ofp, pli, PLI_PASS_5P_ONLY_FORCE,   NULL); fprintf(ofp, "\n");
      cm_pli_PassStatistics(ofp, pli, PLI_PASS_3P_ONLY_FORCE,   NULL); fprintf(ofp, "\n");
      cm_pli_PassStatistics(ofp, pli, PLI_PASS_5P_AND_3P_FORCE, w);    fprintf(ofp, "//\n");
    }
    else if(pli->do_trunc_any) { 
      cm_pli_PassStatistics(ofp, pli, PLI_PASS_SUMMED,          NULL); fprintf(ofp, "\n");
      cm_pli_PassStatistics(ofp, pli, PLI_PASS_STD_ANY,         NULL); fprintf(ofp, "\n");
      cm_pli_PassStatistics(ofp, pli, PLI_PASS_5P_AND_3P_ANY,   w);    fprintf(ofp, "//\n");
    }
    else { 
      cm_pli_PassStatistics(ofp, pli, PLI_PASS_STD_ANY,         w);    fprintf(ofp, "//\n");
    }
  }
  return eslOK;
}

/* Function:  cm_pli_PassStatistics()
 * Synopsis:  Final stats output for one pass of a pipeline.
 * Incept:    EPN, Fri Sep 24 16:48:06 2010 
 *            SRE, Tue Dec  9 10:19:45 2008 [Janelia] (p7_pli_Statistics())
 *
 * Purpose:   Print a standardized report of the internal statistics of
 *            a finished processing pipeline <pli> to stream <ofp>.
 *            
 *            If stopped, non-<NULL> stopwatch <w> is provided for a
 *            stopwatch that was timing the pipeline, then the report
 *            includes timing information.
 *
 * Returns:   <eslOK> on success.
 */
int
cm_pli_PassStatistics(FILE *ofp, CM_PIPELINE *pli, int pass_idx, ESL_STOPWATCH *w)
{
  double  ntargets; 
  int64_t nwin_fcyk  = 0;   /* number of windows CYK filter evaluated */
  int64_t nwin_final = 0;   /* number of windows final stage evaluated */
  int64_t n_output_trunc;   /* number of truncated hits */
  int64_t pos_output_trunc; /* number of residues in truncated hits */

  CM_PLI_ACCT *pli_acct = &(pli->acct[pass_idx]);

  if(pli->do_allstats) { 
    fprintf(ofp, "Internal pipeline statistics summary: %s\n", cm_pli_DescribePass(pass_idx));
  }
  else { 
    fprintf(ofp, "Internal pipeline statistics summary:\n");
  }
  fprintf(ofp, "-------------------------------------\n");
  if (pli->mode == CM_SEARCH_SEQS) {
    fprintf(ofp,   "Query model(s):                                    %15" PRId64 "  (%" PRId64 " consensus positions)\n",     
	    pli->nmodels, pli->nnodes);
    if(pass_idx == PLI_PASS_STD_ANY || pass_idx == PLI_PASS_SUMMED) { 
      fprintf(ofp,   "Target sequences:                                  %15" PRId64 "  (%" PRId64 " residues searched)\n",  
	      pli->nseqs, pli->acct[PLI_PASS_STD_ANY].nres);
    }
    if(pass_idx != PLI_PASS_STD_ANY) { 
      fprintf(ofp,   "Target sequences reexamined for truncated hits:    %15" PRId64 "  (%" PRId64 " residues reexamined)\n",  
	      (pli->do_trunc_ends || pli->do_trunc_any) ? pli->nseqs : 0, 
	      (pass_idx == PLI_PASS_SUMMED) ? pli_acct->nres - pli_acct[PLI_PASS_STD_ANY].nres : pli_acct->nres);
    }
    ntargets = pli->nseqs;
  } else {
    if(pass_idx == PLI_PASS_STD_ANY || pass_idx == PLI_PASS_SUMMED) { 
      fprintf(ofp,   "Query sequence(s):                                 %15" PRId64 "  (%" PRId64 " residues searched)\n",  
	      pli->nseqs,   pli->acct[PLI_PASS_STD_ANY].nres);
    }
    if(pass_idx != PLI_PASS_STD_ANY) { 
      fprintf(ofp,   "Query sequences examined for truncated hits:       %15" PRId64 "  (%" PRId64 " residues searched)\n", 
	      (pli->do_trunc_ends || pli->do_trunc_any) ? pli->nseqs : 0, 
	      (pass_idx == PLI_PASS_SUMMED) ? pli_acct->nres - pli_acct[PLI_PASS_STD_ANY].nres : pli_acct->nres); 
    }
    fprintf(ofp,   "Target model(s):                                   %15" PRId64 "  (%" PRId64 " consensus positions)\n",     
	    pli->nmodels, pli->nnodes);
    ntargets = pli->nmodels;
  }

  if(pli->do_msv) { 
    fprintf(ofp, "Windows   passing  local HMM MSV           filter: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli_acct->n_past_msv,
	    (double) pli_acct->pos_past_msv / pli_acct->nres,
	    pli->F1 * pli->nmodels);
    nwin_fcyk = nwin_final = pli_acct->n_past_msv;
  }
  else { 
    fprintf(ofp, "Windows   passing  local HMM MSV           filter: %15s  (off)\n", "");
  }

  if(pli->do_msvbias) { 
    fprintf(ofp, "Windows   passing  local HMM MSV      bias filter: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli_acct->n_past_msvbias,
	    (double) pli_acct->pos_past_msvbias / pli_acct->nres,
	    pli->F1b * pli->nmodels);
    nwin_fcyk = nwin_final = pli_acct->n_past_msvbias;
  }
  /* msv bias is off by default, so don't output anything if it's off */

  if(pli->do_vit) { 
    fprintf(ofp, "Windows   passing  local HMM Viterbi       filter: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli_acct->n_past_vit,
	    (double) pli_acct->pos_past_vit / pli_acct->nres,
	    pli->F2 * pli->nmodels);
    nwin_fcyk = nwin_final = pli_acct->n_past_vit;
  }
  else { 
    fprintf(ofp, "Windows   passing  local HMM Viterbi       filter: %15s  (off)\n", "");
  }

  if(pli->do_vitbias) { 
    fprintf(ofp, "Windows   passing  local HMM Viterbi  bias filter: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli_acct->n_past_vitbias,
	    (double) pli_acct->pos_past_vitbias / pli_acct->nres,
	    pli->F2b * pli->nmodels);
    nwin_fcyk = nwin_final = pli_acct->n_past_vitbias;
  }
  else { 
    fprintf(ofp, "Windows   passing  local HMM Viterbi  bias filter: %15s  (off)\n", "");
  }

  if(pli->do_fwd) { 
    fprintf(ofp, "Windows   passing  local HMM Forward       filter: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli_acct->n_past_fwd,
	    (double) pli_acct->pos_past_fwd / pli_acct->nres,
	    pli->F3 * pli->nmodels);
    nwin_fcyk = nwin_final = pli_acct->n_past_fwd;
  }
  else { 
    fprintf(ofp, "Windows   passing  local HMM Forward       filter: %15s  (off)\n", "");
  }

  if(pli->do_fwdbias) { 
    fprintf(ofp, "Windows   passing  local HMM Forward  bias filter: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli_acct->n_past_fwdbias,
	    (double) pli_acct->pos_past_fwdbias / pli_acct->nres,
	    pli->F3b * pli->nmodels);
    nwin_fcyk = nwin_final = pli_acct->n_past_fwdbias;
  }
  else { 
    fprintf(ofp, "Windows   passing  local HMM Forward  bias filter: %15s  (off)\n", "");
  }

  if(pli->do_gfwd) { 
    fprintf(ofp, "Windows   passing glocal HMM Forward       filter: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli_acct->n_past_gfwd,
	    (double) pli_acct->pos_past_gfwd / pli_acct->nres,
	    pli->F4 * pli->nmodels);
    nwin_fcyk = nwin_final = pli_acct->n_past_gfwd;
  }
  else { 
    fprintf(ofp, "Windows   passing glocal HMM Forward       filter: %15s  (off)\n", "");
  }
  if(pli->do_gfwdbias) { 
    fprintf(ofp, "Windows   passing glocal HMM Forward  bias filter: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli_acct->n_past_gfwdbias,
	    (double) pli_acct->pos_past_gfwdbias / pli_acct->nres,
	    pli->F4b * pli->nmodels);
    nwin_fcyk = nwin_final = pli_acct->n_past_gfwdbias;
  }
  else { 
    fprintf(ofp, "Windows   passing glocal HMM Forward  bias filter: %15s  (off)\n", "");
  }

  if(pli->do_edef) { 
    fprintf(ofp, "Envelopes passing glocal HMM envelope defn filter: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli_acct->n_past_edef,
	    (double) pli_acct->pos_past_edef / pli_acct->nres,
	    pli->F5 * pli->nmodels);
    nwin_fcyk = nwin_final = pli_acct->n_past_edef;
  }
  else { 
    fprintf(ofp, "Envelopes passing glocal HMM envelope defn filter: %15s  (off)\n", "");
  }

  if(pli->do_edefbias) { 
    fprintf(ofp, "Envelopes passing glocal HMM envelope bias filter: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli_acct->n_past_edefbias,
	    (double) pli_acct->pos_past_edefbias / pli_acct->nres,
	    pli->F5b * pli->nmodels);
    nwin_fcyk = nwin_final = pli_acct->n_past_edefbias;
  }
  /* edef bias is off by default, so don't output anything if it's off */

  if(pli->do_fcyk) { 
    fprintf(ofp, "Envelopes passing %6s CM  CYK           filter: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    (pli->do_glocal_cm_stages) ? "glocal" : "local",
	    pli_acct->n_past_cyk,
	    (double) pli_acct->pos_past_cyk / pli_acct->nres,
	    pli->F6 * pli->nmodels);
    nwin_final = pli_acct->n_past_cyk;
  }
  else { 
    fprintf(ofp, "Envelopes passing %6s CM  CYK           filter: %15s  (off)\n", 
	    (pli->do_glocal_cm_stages) ? "glocal" : "local",
	    "");
  }

  if(pass_idx == PLI_PASS_STD_ANY && (! pli->do_allstats)) { 
    if(pli->do_trunc_ends) { 
      n_output_trunc   = pli->acct[PLI_PASS_5P_ONLY_FORCE].n_output   + pli->acct[PLI_PASS_3P_ONLY_FORCE].n_output   + pli->acct[PLI_PASS_5P_AND_3P_FORCE].n_output;
      pos_output_trunc = pli->acct[PLI_PASS_5P_ONLY_FORCE].pos_output + pli->acct[PLI_PASS_3P_ONLY_FORCE].pos_output + pli->acct[PLI_PASS_5P_AND_3P_FORCE].pos_output;
    }
    else if(pli->do_trunc_any) { 
      n_output_trunc   = pli->acct[PLI_PASS_5P_AND_3P_ANY].n_output;
      pos_output_trunc = pli->acct[PLI_PASS_5P_AND_3P_ANY].pos_output;
    }
    else { /* no truncated hits were allowed */
      n_output_trunc   = 0;
      pos_output_trunc = 0;
    }
    fprintf(ofp, "Total hits reported:                               %15d  (%.4g) [includes %d truncated hit%s]\n",
	    (int) (pli_acct->n_output + n_output_trunc),
	    (double) (pli_acct->pos_output + pos_output_trunc) / pli_acct->nres, 
	    (int) n_output_trunc, (n_output_trunc == 1) ? "" : "s");
  }
  else { 
    fprintf(ofp, "Total hits reported:                               %15d  (%.4g)\n",
	    (int) pli_acct->n_output,
	    (double) pli_acct->pos_output / pli_acct->nres );
  }


  if(pli->do_allstats) { 
     fprintf(ofp, "\n");
     if(nwin_fcyk > 0) { 
       fprintf(ofp, "%-6s filter stage scan matrix overflows:         %15" PRId64 "  (%.4g)\n", 
	       "CYK", 
	       pli_acct->n_overflow_fcyk,
	       (double) pli_acct->n_overflow_fcyk / (double) nwin_fcyk);
     }
     else { 
       fprintf(ofp, "%-6s filter stage scan matrix overflows:         %15d  (%.4g)\n", 
	       "CYK", 
	       0, 0.);
     }
     if(nwin_final > 0) { 
       fprintf(ofp, "%-6s final  stage scan matrix overflows:         %15" PRId64 "  (%.4g)\n", 
	       (pli->final_cm_search_opts & CM_SEARCH_INSIDE) ? "Inside" : "CYK",
	       pli_acct->n_overflow_final,
	       (double) pli_acct->n_overflow_final / (double) nwin_final);
     }
     else { 
       fprintf(ofp, "%-6s final  stage scan matrix overflows:         %15d  (%.4g)\n", 
	       (pli->final_cm_search_opts & CM_SEARCH_INSIDE) ? "Inside" : "CYK",
	       0, 0.);
     }
  }
  if (w != NULL) { 
    esl_stopwatch_Display(ofp, w, "# CPU time: ");
    fprintf(ofp, "# Mc/sec: %.2f\n", 
	    (double) pli_acct->nres * (double) pli->nnodes / (w->elapsed * 1.0e6));
  }

  return eslOK;
}

/* Function:  cm_pli_SumStatistics()
 * Synopsis:  Sum up pipeline statistics for all passes of a pipeline.
 * Incept:    EPN, Thu Feb 23 11:00:39 2012
 *
 * Purpose:   Sum stats for all relevant passes in a pipeline
 *            and store them in pli->acct[PLI_PASS_SUMMED].
 *
 * Returns:   <eslOK> on success.
 */
int
cm_pli_SumStatistics(CM_PIPELINE *pli)
{
  int p; /* counter over passes */

  /* first zero out pli->acct[PLI_PASS_SUMMED] */
  cm_pli_ZeroAccounting(&(pli->acct[PLI_PASS_SUMMED]));

  /* now tally up counts, but only for passes we actually performed
   * (the others should be zero..., but this way is safest).
   */
  for(p = 1; p < NPLI_PASSES; p++) { 
    if((p == PLI_PASS_STD_ANY) || /* we always do the standard pass */
       (pli->do_trunc_ends && p != PLI_PASS_5P_AND_3P_ANY) ||   /* do_trunc_ends: do all passes except PLI_PASS_5P_AND_3P_ANY */
       (pli->do_trunc_any  && p == PLI_PASS_5P_AND_3P_ANY)) /* do_trunc_any:  do PLI_PASS_STD_ANY and PLI_PASS_5P_AND_3P_ANY */
      { 
	pli->acct[PLI_PASS_SUMMED].nres              += pli->acct[p].nres;
	pli->acct[PLI_PASS_SUMMED].n_past_msv        += pli->acct[p].n_past_msv;
	pli->acct[PLI_PASS_SUMMED].n_past_vit        += pli->acct[p].n_past_vit;
	pli->acct[PLI_PASS_SUMMED].n_past_fwd        += pli->acct[p].n_past_fwd;
	pli->acct[PLI_PASS_SUMMED].n_past_gfwd       += pli->acct[p].n_past_gfwd;
	pli->acct[PLI_PASS_SUMMED].n_past_edef       += pli->acct[p].n_past_edef;
	pli->acct[PLI_PASS_SUMMED].n_past_cyk        += pli->acct[p].n_past_cyk;
	pli->acct[PLI_PASS_SUMMED].n_past_ins        += pli->acct[p].n_past_ins;
	pli->acct[PLI_PASS_SUMMED].n_output          += pli->acct[p].n_output;
	pli->acct[PLI_PASS_SUMMED].n_past_msvbias    += pli->acct[p].n_past_msvbias;
	pli->acct[PLI_PASS_SUMMED].n_past_vitbias    += pli->acct[p].n_past_vitbias;
	pli->acct[PLI_PASS_SUMMED].n_past_fwdbias    += pli->acct[p].n_past_fwdbias;
	pli->acct[PLI_PASS_SUMMED].n_past_gfwdbias   += pli->acct[p].n_past_gfwdbias;
	pli->acct[PLI_PASS_SUMMED].n_past_edefbias   += pli->acct[p].n_past_edefbias;
	pli->acct[PLI_PASS_SUMMED].pos_past_msv      += pli->acct[p].pos_past_msv;
	pli->acct[PLI_PASS_SUMMED].pos_past_vit      += pli->acct[p].pos_past_vit;
	pli->acct[PLI_PASS_SUMMED].pos_past_fwd      += pli->acct[p].pos_past_fwd;
	pli->acct[PLI_PASS_SUMMED].pos_past_gfwd     += pli->acct[p].pos_past_gfwd;
	pli->acct[PLI_PASS_SUMMED].pos_past_edef     += pli->acct[p].pos_past_edef;
	pli->acct[PLI_PASS_SUMMED].pos_past_cyk      += pli->acct[p].pos_past_cyk;
	pli->acct[PLI_PASS_SUMMED].pos_past_ins      += pli->acct[p].pos_past_ins;
	pli->acct[PLI_PASS_SUMMED].pos_output        += pli->acct[p].pos_output;
	pli->acct[PLI_PASS_SUMMED].pos_past_msvbias  += pli->acct[p].pos_past_msvbias;
	pli->acct[PLI_PASS_SUMMED].pos_past_vitbias  += pli->acct[p].pos_past_vitbias;
	pli->acct[PLI_PASS_SUMMED].pos_past_fwdbias  += pli->acct[p].pos_past_fwdbias;
	pli->acct[PLI_PASS_SUMMED].pos_past_gfwdbias += pli->acct[p].pos_past_gfwdbias;
	pli->acct[PLI_PASS_SUMMED].pos_past_edefbias += pli->acct[p].pos_past_edefbias;
	
	pli->acct[PLI_PASS_SUMMED].n_overflow_fcyk   += pli->acct[p].n_overflow_fcyk;
	pli->acct[PLI_PASS_SUMMED].n_overflow_final  += pli->acct[p].n_overflow_final;
	pli->acct[PLI_PASS_SUMMED].n_aln_hb          += pli->acct[p].n_aln_hb;
	pli->acct[PLI_PASS_SUMMED].n_aln_dccyk       += pli->acct[p].n_aln_dccyk;
      }
  }
  return eslOK;
}

/* Function:  cm_pli_ZeroAccounting()
 * Synopsis:  Zero a set of pipeline accounting statistics.
 * Incept:    EPN, Mon Nov 28 18:40:00 2011
 *
 * Returns:   <eslOK> on success.
 */
int
cm_pli_ZeroAccounting(CM_PLI_ACCT *pli_acct)
{
  pli_acct->nres              = 0;
  pli_acct->n_past_msv        = 0;
  pli_acct->n_past_vit        = 0;
  pli_acct->n_past_fwd        = 0;
  pli_acct->n_past_gfwd       = 0;
  pli_acct->n_past_edef       = 0;
  pli_acct->n_past_cyk        = 0;
  pli_acct->n_past_ins        = 0;
  pli_acct->n_output          = 0;
  pli_acct->n_past_msvbias    = 0;
  pli_acct->n_past_vitbias    = 0;
  pli_acct->n_past_fwdbias    = 0;
  pli_acct->n_past_gfwdbias   = 0;
  pli_acct->n_past_edefbias   = 0;
  pli_acct->pos_past_msv      = 0;
  pli_acct->pos_past_vit      = 0;
  pli_acct->pos_past_fwd      = 0;
  pli_acct->pos_past_gfwd     = 0;
  pli_acct->pos_past_edef     = 0;
  pli_acct->pos_past_cyk      = 0;
  pli_acct->pos_past_ins      = 0;      
  pli_acct->pos_output        = 0;
  pli_acct->pos_past_msvbias  = 0;
  pli_acct->pos_past_vitbias  = 0;
  pli_acct->pos_past_fwdbias  = 0;
  pli_acct->pos_past_gfwdbias = 0;
  pli_acct->pos_past_edefbias = 0;
  
  pli_acct->n_overflow_fcyk   = 0;
  pli_acct->n_overflow_final  = 0;
  pli_acct->n_aln_hb          = 0;
  pli_acct->n_aln_dccyk       = 0;

  return eslOK;
}


/* Function:  cm_pli_DescribePass()
 * Date:      EPN, Tue Nov 29 04:39:38 2011
 *
 * Purpose:   Translate internal flags for pipeline pass index
 *            into human-readable strings, for clearer output.
 * 
 * Args:      pass_idx - a pipeline pass index
 *                       PLI_PASS_SUMMED, PLI_PASS_STD_ANY, PLI_PASS_5P_ONLY_FORCE, PLI_PASS_3P_ONLY_FORCE, PLI_PASS_5P_AND_3P_FORCE
 *
 * Returns:   the appropriate string
 */
char *
cm_pli_DescribePass(int pass_idx) 
{
  switch (pass_idx) {
  case PLI_PASS_SUMMED:          return "(standard and truncated passes)"; break;
  case PLI_PASS_STD_ANY:         return "(full sequences)";                break;
  case PLI_PASS_5P_ONLY_FORCE:   return "(5' terminal sequence regions)";  break;
  case PLI_PASS_3P_ONLY_FORCE:   return "(3' terminal sequence regions)";  break;
  case PLI_PASS_5P_AND_3P_FORCE: return "(full sequences short enough to contain a 5' and 3' truncated hit)"; break;
  case PLI_PASS_5P_AND_3P_ANY:   return "(full sequences, allowing truncated hits)"; break;
  default: cm_Fail("bogus pipeline pass index %d\n", pass_idx); break;
  }
  return "";
}

/* Function:  cm_pli_PassEnforcesFirstRes()
 * Date:      EPN, Thu Feb 16 07:42:59 2012
 *
 * Purpose:   Return TRUE if the pipeline pass indicated by
 *            <pass_idx> forces the inclusion of i0 (first
 *            residue in the sequence) in any valid 
 *            parsetree/alignment. Else returns FALSE.
 * 
 * Args:      pass_idx - a pipeline pass index
 *                       PLI_PASS_SUMMED, PLI_PASS_STD_ANY, PLI_PASS_5P_ONLY_FORCE, PLI_PASS_3P_ONLY_FORCE, PLI_PASS_5P_AND_3P_FORCE
 *
 * Returns:   TRUE if i0 inclusion is forced, FALSE if not
 */
int
cm_pli_PassEnforcesFirstRes(int pass_idx) 
{
  switch (pass_idx) {
  case PLI_PASS_STD_ANY:         return FALSE; break;
  case PLI_PASS_5P_ONLY_FORCE:   return TRUE;  break;
  case PLI_PASS_3P_ONLY_FORCE:   return FALSE; break;
  case PLI_PASS_5P_AND_3P_FORCE: return TRUE;  break;
  case PLI_PASS_5P_AND_3P_ANY:   return FALSE; break;
  default:                       return FALSE; break;
  }
  return FALSE;
}

/* Function:  cm_pli_PassEnforcesFinalRes()
 * Date:      EPN, Thu Feb 16 07:46:10 2012
 *
 * Purpose:   Return TRUE if the pipeline pass indicated by
 *            <pass_idx> forces the inclusion of j0 (final
 *            residue in the sequence) in any valid 
 *            parsetree/alignment. Else returns FALSE.
 * 
 * Args:      pass_idx - a pipeline pass index
 *                       PLI_PASS_SUMMED, PLI_PASS_STD_ANY, PLI_PASS_5P_ONLY_FORCE, PLI_PASS_3P_ONLY_FORCE, PLI_PASS_5P_AND_3P_FORCE
 *
 * Returns:   TRUE if j0 inclusion is forced, FALSE if not
 */
int
cm_pli_PassEnforcesFinalRes(int pass_idx) 
{
  switch (pass_idx) {
  case PLI_PASS_STD_ANY:         return FALSE; break;
  case PLI_PASS_5P_ONLY_FORCE:   return FALSE; break;
  case PLI_PASS_3P_ONLY_FORCE:   return TRUE;  break;
  case PLI_PASS_5P_AND_3P_FORCE: return TRUE;  break;
  case PLI_PASS_5P_AND_3P_ANY:   return FALSE; break;
  default:                       return FALSE; break;
  }
  return FALSE;
}

/* Function:  cm_pli_PassAllowsTruncation()
 * Date:      EPN, Wed Mar 14 13:53:30 2012
 *
 * Purpose:   Return TRUE if the pipeline pass indicated by <pass_idx>
 *            allows some type of truncated alignment, else return
 *            FALSE.  In other words, if we can safely call a standard
 *            (non-truncated) DP alignment/search function for this
 *            pass then return FALSE, else return TRUE.
 * 
 * Args:      pass_idx - a pipeline pass index
 *                       PLI_PASS_SUMMED, PLI_PASS_STD_ANY, PLI_PASS_5P_ONLY_FORCE, PLI_PASS_3P_ONLY_FORCE, PLI_PASS_5P_AND_3P_FORCE
 *
 * Returns:   TRUE if j0 inclusion is forced, FALSE if not
 */
int
cm_pli_PassAllowsTruncation(int pass_idx) 
{
  switch (pass_idx) {
  case PLI_PASS_STD_ANY:         return FALSE; break;
  case PLI_PASS_5P_ONLY_FORCE:   return TRUE;  break;
  case PLI_PASS_3P_ONLY_FORCE:   return TRUE;  break;
  case PLI_PASS_5P_AND_3P_FORCE: return TRUE;  break;
  case PLI_PASS_5P_AND_3P_ANY:   return TRUE;  break;
  default:                       return FALSE; break;
  }
  return FALSE;
}

/*------------------- end, pipeline API -------------------------*/
 

/*****************************************************************
 * 3. Non-API filter stage search and other functions
 *****************************************************************/


/* Function:  pli_p7_filter()
 * Synopsis:  The accelerated p7 comparison pipeline: MSV through Forward filter.
 * Incept:    EPN, Wed Nov 24 13:07:02 2010
 *            TJW, Fri Feb 26 10:17:53 2018 [Janelia] (p7_Pipeline_Longtargets())
 *
 * Purpose:   Run the accelerated pipeline to compare profile <om>
 *            against sequence <sq>. Some combination of the MSV,
 *            Viterbi and Forward algorithms are used, based on 
*             option flags set in <pli>. 
 *
 *            In a normal pipeline run, this function call is the
 *            first search function used and should be followed by a
 *            call to pli_p7_env_def().
 *
 * Returns:   <eslOK> on success. For the <ret_nwin> windows that
 *            survive all filters, the start and end positions of the
 *            windows are stored and returned in <ret_ws> and
 *            <ret_we> respectively.
 *
 *            <eslEINVAL> if (in a scan pipeline) we're supposed to
 *            set GA/TC/NC bit score thresholds but the model doesn't
 *            have any.
 *
 *            <eslERANGE> on numerical overflow errors in the
 *            optimized vector implementations; particularly in
 *            posterior decoding. I don't believe this is possible for
 *            multihit local models, but I'm set up to catch it
 *            anyway. We may emit a warning to the user, but cleanly
 *            skip the problematic sequence and continue.
 *
 * Throws:    <eslEMEM> on allocation failure.
 *
 * Xref:      J4/25.
 */
int
pli_p7_filter(CM_PIPELINE *pli, P7_OPROFILE *om, P7_BG *bg, float *p7_evparam, P7_MSVDATA *msvdata, const ESL_SQ *sq, int64_t **ret_ws, int64_t **ret_we, int *ret_nwin)
{
  int              status;
  float            mfsc, vfsc, fwdsc;/* filter scores          */
  float            filtersc;         /* HMM null filter score  */
  int              have_filtersc;    /* TRUE if filtersc has been calc'ed for current window */
  float            nullsc;           /* null model score */
  float            wsc;              /* the corrected bit score for a window */
  double           P;                /* P-value of a hit */
  int              i, i2;            /* counters */
  int              wlen;             /* length of current window */
  void            *p;                /* for ESL_RALLOC */
  int             *useme = NULL;     /* used when merging overlapping windows */
  int              overlap;          /* number of overlapping positions b/t 2 adjacent windows */
  int            **survAA = NULL;    /* [0..s..Np7_SURV-1][0..i..nwin-1] TRUE if window i survived stage s */
  int              nalloc;           /* currently allocated size for ws, we */
  int64_t         *ws = NULL;        /* [0..nwin-1] window start positions */
  int64_t         *we = NULL;        /* [0..nwin-1] window end   positions */
  double          *wp = NULL;        /* [0..nwin-1] window P-values, P-value of furthest-reached filter algorithm */
  int              nwin;             /* number of windows */
  int64_t         *new_ws = NULL;    /* used when copying/modifying ws */
  int64_t         *new_we = NULL;    /* used when copying/modifying we */
  int              nsurv_fwd;        /* number of windows that survive fwd filter */
  int              new_nsurv_fwd;    /* used when merging fwd survivors */
  ESL_DSQ         *subdsq;           /* a ptr to the first position of a window */
  int              have_rest;        /* do we have the full <om> read in? */
  FM_WINDOWLIST    wlist;            /* list of windows, structure taken by p7_MSVFilter_longtarget() */

  if (sq->n == 0) return eslOK;    /* silently skip length 0 seqs; they'd cause us all sorts of weird problems */
  p7_omx_GrowTo(pli->oxf, om->M, 0, sq->n);    /* expand the one-row omx if needed */
  have_rest = (om->mode == p7_NO_MODE) ? FALSE : TRUE; /* we use om->mode as a flag to tell us whether we already have read the full <om> from disk or not */

  /* Set false target length. This is a conservative estimate of the length of window that'll
   * soon be passed on to later phases of the pipeline;  used to recover some bits of the score
   * that we would miss if we left length parameters set to the full target length */

  /* Set MSV length as pli->maxW/
   * Note: this differs from nhmmer, which uses om->max_length
   */
  p7_oprofile_ReconfigMSVLength(om, pli->maxW);
  om->max_length = pli->maxW;

  #if DEBUGPIPELINE
  printf("\nPIPELINE p7Filter() %s  %" PRId64 " residues\n", sq->name, sq->n);
  #endif

  /* initializations */
  nsurv_fwd = 0;
  nwin = 0;

  /***************************************************/
  /* Filter 1: MSV, long target-variant, with p7 HMM */
  if(pli->do_msv) { 
    fm_initWindows(&wlist);
    status = p7_MSVFilter_longtarget(sq->dsq, sq->n, om, pli->oxf, msvdata, bg, pli->F1, &wlist, TRUE); /* TRUE: force SSV, not MSV */

    if(wlist.count > 0) { 
      /* In scan mode, if at least one window passes the MSV filter, read the rest of the profile */
      if (pli->mode == CM_SCAN_MODELS && (! have_rest)) {
	if (pli->cmfp) p7_oprofile_ReadRest(pli->cmfp->hfp, om);
	/* Note: we don't call cm_pli_NewModelThresholds() yet (as p7_pipeline() 
	 * does at this point), because we don't yet have the CM */
	have_rest = TRUE;
      }
      p7_hmm_MSVDataComputeRest(om, msvdata);
      p7_pli_ExtendAndMergeWindows(om, msvdata, &wlist, sq->n);
    }
    ESL_ALLOC(ws, sizeof(int64_t) * wlist.count);
    ESL_ALLOC(we, sizeof(int64_t) * wlist.count);
    nwin = wlist.count;
    for(i = 0; i < nwin; i++) { 
      ws[i] =         wlist.windows[i].n;
      we[i] = ws[i] + wlist.windows[i].length - 1;
    }
    /* split up windows > (pli->wmult * pli->cmW) into length 2W,
     * with W-1 overlapping residues, pli->wmult is 2.0. (So yes, 
     * if window is 2*W+1 residues we search all but one residue
     * twice.)
     */
    nalloc = nwin + 100;
    ESL_ALLOC(new_ws, sizeof(int64_t) * nalloc);
    ESL_ALLOC(new_we, sizeof(int64_t) * nalloc);
    for (i = 0, i2 = 0; i < nwin; i++, i2++) {
      wlen = we[i] - ws[i] + 1;
      if((i2+1) == nalloc) { 
	nalloc += 100;
	ESL_RALLOC(new_ws, p, sizeof(int64_t) * nalloc);
	ESL_RALLOC(new_we, p, sizeof(int64_t) * nalloc);
      }
      if(wlen > (pli->wmult * pli->cmW)) { 
	/* split this window */
	new_ws[i2]   = ws[i]; 
	new_we[i2]   = ESL_MIN((new_ws[i2] + (2 * pli->cmW) - 1), we[i]);
	while(new_we[i2] < we[i]) { 
	  i2++;
	  if((i2+1) == nalloc) { 
	    nalloc += 100;
	    ESL_RALLOC(new_ws, p, sizeof(int64_t) * nalloc);
	    ESL_RALLOC(new_we, p, sizeof(int64_t) * nalloc);
	  }
	  new_ws[i2]   = ESL_MIN(new_ws[i2-1] + pli->cmW, we[i]);
	  new_we[i2]   = ESL_MIN(new_we[i2-1] + pli->cmW, we[i]);
	}	    
      }
      else { /* do not split this window */
	new_ws[i2] = ws[i]; 
	new_we[i2] = we[i];
      }
    }
    free(wlist.windows);
    free(ws);
    free(we);
    ws = new_ws;
    we = new_we;
    nwin = i2;
  }
  else { /* do_msv is FALSE */
    nwin = 1; /* first window */
    if(sq->n > (2 * pli->maxW)) { 
      nwin += (int) (sq->n - (2 * pli->maxW)) / ((2 * pli->maxW) - (pli->maxW - 1));
      /*            (L     -  first window)/(number of unique residues per window) */
      if(((sq->n - (2 * pli->maxW)) % ((2 * pli->maxW) - (pli->maxW - 1))) > 0) { 
	nwin++; /* if the (int) cast in previous line removed any fraction of a window, we add it back here */
      }
    }
    ESL_ALLOC(ws,     sizeof(int64_t) * nwin);
    ESL_ALLOC(we,     sizeof(int64_t) * nwin);
    for(i = 0; i < nwin; i++) { 
      ws[i] = 1 + (i * (pli->maxW + 1));
      we[i] = ESL_MIN((ws[i] + (2*pli->maxW) - 1), sq->n);
      /*printf("window %5d/%5d  %10" PRId64 "..%10" PRId64 " (L=%10" PRId64 ")\n", i+1, nwin, ws[i], we[i], sq->n);*/
    }
  }     
  pli->acct[pli->cur_pass_idx].n_past_msv += nwin;
  
  ESL_ALLOC(wp, sizeof(double) * nwin);
  for(i = 0; i < nwin; i++) { wp[i] = pli->F1; } /* TEMP (?) p7_MSVFilter_longtarget() does not return P-values */

  /*********************************************/
  /* allocate and initialize survAA, which will keep track of number of windows surviving each stage */
  ESL_ALLOC(survAA, sizeof(int *) * Np7_SURV);
  for (i = 0; i < Np7_SURV; i++) { 
    ESL_ALLOC(survAA[i], sizeof(int) * nwin);
    esl_vec_ISet(survAA[i], nwin, FALSE);
  }
    
  for (i = 0; i < nwin; i++) {
    subdsq = sq->dsq + ws[i] - 1;
    have_filtersc = FALSE;
    wlen = we[i] - ws[i] + 1;

    p7_bg_SetLength(bg, wlen);
    p7_bg_NullOne  (bg, subdsq, wlen, &nullsc);

#if DEBUGPIPELINE
    if(pli->do_msv) printf("SURVIVOR window %5d [%10" PRId64 "..%10" PRId64 "] survived MSV       %6.2f bits  P %g\n", i, ws[i], we[i], 0., wp[i]);
#endif
    survAA[p7_SURV_F1][i] = TRUE;
    
    if (pli->do_msv && pli->do_msvbias) {
      /******************************************************************************/
      /* Filter 1B: Bias filter with p7 HMM 
       * Have to run msv again, to get the full score for the window.
       * (using the standard "per-sequence" msv filter this time). 
       */
      p7_oprofile_ReconfigMSVLength(om, wlen);
      p7_MSVFilter(subdsq, wlen, om, pli->oxf, &mfsc);
      p7_bg_FilterScore(bg, subdsq, wlen, &filtersc);
      have_filtersc = TRUE;
      
      wsc = (mfsc - filtersc) / eslCONST_LOG2;
      P   = esl_gumbel_surv(wsc,  p7_evparam[CM_p7_LMMU],  p7_evparam[CM_p7_LMLAMBDA]);
      wp[i] = P;

      if (P > pli->F1b) continue;

      /******************************************************************************/
    }
    pli->acct[pli->cur_pass_idx].n_past_msvbias++;
    survAA[p7_SURV_F1b][i] = TRUE;

#if DEBUGPIPELINE
    if(pli->do_msv && pli->do_msvbias) printf("SURVIVOR window %5d [%10" PRId64 "..%10" PRId64 "] survived MSV-Bias  %6.2f bits  P %g\n", i, ws[i], we[i], 0., wp[i]);
#endif      
    if(pli->do_time_F1) return eslOK;
    
    /* In scan mode, we may get to this point and not yet have read the rest of 
     * the profile if the msvfilter is off, if so read the rest of the profile.
     */
    if (pli->mode == CM_SCAN_MODELS && (! have_rest)) {
      if (pli->cmfp) p7_oprofile_ReadRest(pli->cmfp->hfp, om);
      /* Note: we don't call cm_pli_NewModelThresholds() yet (as p7_pipeline() 
       * does at this point), because we don't yet have the CM */
      have_rest = TRUE;
    }
    if(pli->do_msv && pli->do_msvbias) { /* we already called p7_oprofile_ReconfigMSVLength() above */
      p7_oprofile_ReconfigRestLength(om, wlen);
    }
    else { /* we did not call p7_oprofile_ReconfigMSVLength() above */
      p7_oprofile_ReconfigLength(om, wlen);
    }

    if (pli->do_vit) { 
      /******************************************************************************/
      /* Filter 2: Viterbi with p7 HMM */
      /* Second level filter: ViterbiFilter(), multihit with <om> */
      p7_ViterbiFilter(subdsq, wlen, om, pli->oxf, &vfsc);
      wsc = (vfsc - nullsc) / eslCONST_LOG2; 
      P   = esl_gumbel_surv(wsc,  p7_evparam[CM_p7_LVMU],  p7_evparam[CM_p7_LVLAMBDA]);
      wp[i] = P;
      if (P > pli->F2) continue;
    }
    pli->acct[pli->cur_pass_idx].n_past_vit++;
    survAA[p7_SURV_F2][i] = TRUE;

#if DEBUGPIPELINE
    if (pli->do_vit) printf("SURVIVOR window %5d [%10" PRId64 "..%10" PRId64 "] survived Vit       %6.2f bits  P %g\n", i, ws[i], we[i], wsc, wp[i]);
#endif

    /********************************************/
    if (pli->do_vit && pli->do_vitbias) { 
      if(! have_filtersc) { 
	p7_bg_FilterScore(bg, subdsq, wlen, &filtersc);
      }
      have_filtersc = TRUE;
      wsc = (vfsc - filtersc) / eslCONST_LOG2;
      P = esl_gumbel_surv(wsc,  p7_evparam[CM_p7_LVMU],  p7_evparam[CM_p7_LVLAMBDA]);
      wp[i] = P;
      if (P > pli->F2b) continue;
      /******************************************************************************/
    }
    pli->acct[pli->cur_pass_idx].n_past_vitbias++;
    survAA[p7_SURV_F2b][i] = TRUE;

#if DEBUGPIPELINE
    if (pli->do_vit && pli->do_vitbias) printf("SURVIVOR window %5d [%10" PRId64 "..%10" PRId64 "] survived Vit-Bias  %6.2f bits  P %g\n", i, ws[i], we[i], wsc, wp[i]);
#endif
    if(pli->do_time_F2) continue; 
    /********************************************/

    if(pli->do_fwd) { 
      /******************************************************************************/
      /* Filter 3: Forward with p7 HMM */
      /* Parse it with Forward and obtain its real Forward score. */
      p7_ForwardParser(subdsq, wlen, om, pli->oxf, &fwdsc);
      wsc = (fwdsc - nullsc) / eslCONST_LOG2; 
      P = esl_exp_surv(wsc,  p7_evparam[CM_p7_LFTAU],  p7_evparam[CM_p7_LFLAMBDA]);
      wp[i] = P;
      if (P > pli->F3) continue;
    }
    /******************************************************************************/
    pli->acct[pli->cur_pass_idx].n_past_fwd++;
    survAA[p7_SURV_F3][i] = TRUE;

#if DEBUGPIPELINE
    if(pli->do_fwd) printf("SURVIVOR window %5d [%10" PRId64 "..%10" PRId64 "] survived Fwd       %6.2f bits  P %g\n", i, ws[i], we[i], wsc, wp[i]);
#endif

    if (pli->do_fwd && pli->do_fwdbias) { 
      if (! have_filtersc) { 
	p7_bg_FilterScore(bg, subdsq, wlen,     &filtersc);
      }
      have_filtersc = TRUE;
      wsc = (fwdsc - filtersc) / eslCONST_LOG2;
      P = esl_exp_surv(wsc,  p7_evparam[CM_p7_LFTAU],  p7_evparam[CM_p7_LFLAMBDA]);
      wp[i] = P;
      if (P > pli->F3b) continue;
      /******************************************************************************/
    }
    pli->acct[pli->cur_pass_idx].n_past_fwdbias++;
    nsurv_fwd++;
    survAA[p7_SURV_F3b][i] = TRUE;

#if DEBUGPIPELINE
    if(pli->do_fwd && pli->do_fwdbias) printf("SURVIVOR window %5d [%10" PRId64 "..%10" PRId64 "] survived Fwd-Bias  %6.2f bits  P %g\n", i, ws[i], we[i], wsc, wp[i]);
#endif
    if(pli->do_time_F3) continue;
  }

  /* Go back through all windows, and tally up total number of
   * residues that survived each stage, without double-counting
   * overlapping residues. Based on the way the windows were split, we
   * know that any overlapping residues must occur in adjacent windows
   * and we exploit that here.
   */
  for(i = 0; i < nwin; i++) {
    wlen = we[i] - ws[i] + 1;
    
    if(survAA[p7_SURV_F1][i])  pli->acct[pli->cur_pass_idx].pos_past_msv     += wlen; 
    if(survAA[p7_SURV_F1b][i]) pli->acct[pli->cur_pass_idx].pos_past_msvbias += wlen; 
    if(survAA[p7_SURV_F2][i])  pli->acct[pli->cur_pass_idx].pos_past_vit     += wlen; 
    if(survAA[p7_SURV_F2b][i]) pli->acct[pli->cur_pass_idx].pos_past_vitbias += wlen; 
    if(survAA[p7_SURV_F3][i])  pli->acct[pli->cur_pass_idx].pos_past_fwd     += wlen; 
    if(survAA[p7_SURV_F3b][i]) pli->acct[pli->cur_pass_idx].pos_past_fwdbias += wlen; 

    /* now subtract residues we've double counted */
    if(i > 0) { 
      overlap = we[i-1] - ws[i] + 1;
      if(overlap > 0) { 
	if(survAA[p7_SURV_F1][i]  && survAA[p7_SURV_F1][i-1])  pli->acct[pli->cur_pass_idx].pos_past_msv     -= overlap;
	if(survAA[p7_SURV_F1b][i] && survAA[p7_SURV_F1b][i-1]) pli->acct[pli->cur_pass_idx].pos_past_msvbias -= overlap;
	if(survAA[p7_SURV_F2][i]  && survAA[p7_SURV_F2][i-1])  pli->acct[pli->cur_pass_idx].pos_past_vit     -= overlap;
	if(survAA[p7_SURV_F2b][i] && survAA[p7_SURV_F2b][i-1]) pli->acct[pli->cur_pass_idx].pos_past_vitbias -= overlap;
	if(survAA[p7_SURV_F3][i]  && survAA[p7_SURV_F3][i-1])  pli->acct[pli->cur_pass_idx].pos_past_fwd     -= overlap;
	if(survAA[p7_SURV_F3b][i] && survAA[p7_SURV_F3b][i-1]) pli->acct[pli->cur_pass_idx].pos_past_fwdbias -= overlap;
      }
    }
  }
 
  /* Finally, create list of just those that survived fwd, and merge any overlapping windows together */
  if(nsurv_fwd > 0) { 
    ESL_ALLOC(new_ws, sizeof(int64_t) * nsurv_fwd);
    ESL_ALLOC(new_we, sizeof(int64_t) * nsurv_fwd);
    for (i = 0, i2 = 0; i < nwin; i++) { 
      if(survAA[p7_SURV_F3b][i]) { 
	new_ws[i2] = ws[i];
	new_we[i2] = we[i];
	i2++;
      }
    }
    /* we could have overlapping windows, merge those that do overlap */
    new_nsurv_fwd = 0;
    ESL_ALLOC(useme, sizeof(int) * nsurv_fwd);
    esl_vec_ISet(useme, nsurv_fwd, FALSE);
    i2 = 0;
    for(i = 0, i2 = 0; i < nsurv_fwd; i++) { 
      useme[i] = TRUE;
      i2 = i+1;
      while((i2 < nsurv_fwd) && ((new_we[i]+1) >= (new_ws[i2]))) { 
	useme[i2] = FALSE;
	new_we[i] = new_we[i2]; /* merged i with i2, rewrite end for i */
	i2++;
      }
      i = i2-1;
    }
    i2 = 0;
    for(i = 0; i < nsurv_fwd; i++) { 
      if(useme[i]) { 
	new_ws[i2] = new_ws[i];
	new_we[i2] = new_we[i];
	i2++;
      }
    }
    nsurv_fwd = i2;
    free(useme);
    free(ws); ws = NULL;
    free(we); we = NULL;
    free(wp); wp = NULL;
    ws = new_ws;
    we = new_we;
  }    
  else { 
    if(ws != NULL) free(ws); ws = NULL;
    if(we != NULL) free(we); we = NULL;
    if(wp != NULL) free(wp); wp = NULL;
  }

  if(survAA != NULL) { 
    for (i = 0; i < Np7_SURV; i++) free(survAA[i]);
    free(survAA);
  }

  *ret_ws   = ws;
  *ret_we   = we;
  *ret_nwin = nsurv_fwd;

  return eslOK;

 ERROR:
  ESL_EXCEPTION(eslEMEM, "Error allocating memory for hit list in pipeline\n");

}


/* Function:  pli_p7_env_def()
 * Synopsis:  Envelope definition of hits surviving Forward, prior to passing to CYK.
 * Incept:    EPN, Wed Nov 24 13:18:54 2010
 *
 * Purpose:   For each window x, from <ws[x]>..<we[x]>, determine
 *            the envelope boundaries for any hits within it using 
 *            a p7 profile. 
 *
 *            In the SCAN pipeline we may enter this function with
 *            *opt_gm == NULL because we haven't yet read it from the
 *            HMM file. In that case, read it and return it in
 *            <*opt_gm>.  Otherwise, <*opt_gm> is valid upon entering.
 *
 *            If the P-value of any detected envelopes is sufficiently
 *            high (above pli->F5), we skip them (i.e. envelope defn
 *            acts as a filter too). Further, in glocal mode
 *            (<do_glocal>==TRUE) we skip any window for which the
 *            glocal Forward P-value is too high (above pli->F4).
 *
 *            In a normal pipeline run, this function call should be
 *            just after a call to pli_p7_filter() and just
 *            before a call to pli_cyk_env_filter().
 *
 * Returns:   <eslOK on success. For all <ret_nenv> envelopes not
 *            filtered out, return their envelope boundaries in
 *            <ret_es> and <ret_ee>.
 *
 * Throws:    <eslEMEM> on allocation failure.
 *            <eslENOTFOUND> if we need but don't have an HMM file to read
 *            <eslESYS> on failure of system call when reading HMM
 */
int
pli_p7_env_def(CM_PIPELINE *pli, P7_OPROFILE *om, P7_BG *bg, float *p7_evparam, const ESL_SQ *sq, int64_t *ws, int64_t *we, int nwin, P7_HMM **opt_hmm, P7_PROFILE **opt_gm, P7_PROFILE **opt_Rgm, P7_PROFILE **opt_Lgm, P7_PROFILE **opt_Tgm, int64_t **ret_es, int64_t **ret_ee, int *ret_nenv)
{
  int              status;                     
  double           P;                 /* P-value of a hit */
  int              d, i;              /* counters */
  void            *p;                 /* for ESL_RALLOC */
  int              env_len;           /* envelope length */
  float            env_sc;            /* envelope bit score, and null1 score */
  float            sc_for_pvalue;     /* score for window, used for calc'ing P value */
  float            env_edefbias;      /* null2 correction for envelope */
  float            env_sc_for_pvalue; /* corrected score for envelope, used for calc'ing P value */
  int64_t          wlen;              /* window length of current window */
  int64_t         *es = NULL;         /* [0..nenv-1] envelope start positions */
  int64_t         *ee = NULL;         /* [0..nenv-1] envelope end   positions */
  int              nenv;              /* number of surviving envelopes */
  int              nenv_alloc;        /* current size of es, ee */
  ESL_DSQ         *subdsq;            /* a ptr to the first position of a window */
  ESL_SQ          *seq = NULL;        /* a copy of a window */
  int64_t          estart, eend;      /* envelope start/end positions */
  float            nullsc, filtersc, fwdsc, bcksc;
  P7_PROFILE      *gm  = NULL;        /* a ptr to *opt_gm, for convenience */
  int              do_local_envdef;   /* TRUE if we define envelopes with p7 in local mode, FALSE for glocal */

  /* variables related to forcing first and/or final residue within truncated hits */
  P7_PROFILE      *Rgm = NULL;        /* a ptr to *Ropt_gm, for convenience */
  P7_PROFILE      *Lgm = NULL;        /* a ptr to *Lopt_gm, for convenience */
  P7_PROFILE      *Tgm = NULL;        /* a ptr to *Topt_gm, for convenience */
  float            safe_lfwdsc;       /* a score <= local Forward score, determined via a correction to a Forward score with Rgm or Lgm */
  int              use_gm, use_Rgm, use_Lgm, use_Tgm; /* only one of these can be TRUE, should we use the standard profile for non
						       * truncated hits or the specially configured T, R, or L profiles because 
						       * we're defining envelopes for hits possibly truncated 5', 3' or both 5' and 3'? 
						       */
  float            Rgm_correction;    /* nat score correction for windows and envelopes defined with Rgm */
  float            Lgm_correction;    /* nat score correction for windows and envelopes defined with Lgm */

  if (sq->n == 0) return eslOK;    /* silently skip length 0 seqs; they'd cause us all sorts of weird problems */
  if (nwin == 0) { 
    *ret_es = NULL;
    *ret_ee = NULL;
    *ret_nenv = 0;
    return eslOK;    /* if there's no windows to search in, return */
  }

  /* Will we use local envelope definition? Only if we're in the
   * special pipeline pass where we allow any truncated hits (only
   * possibly true if pli->do_trunc_any is TRUE).
   */
  do_local_envdef = (pli->cur_pass_idx == PLI_PASS_5P_AND_3P_ANY) ? TRUE : FALSE;

  nenv_alloc = nwin;
  ESL_ALLOC(es, sizeof(int64_t) * nenv_alloc);
  ESL_ALLOC(ee, sizeof(int64_t) * nenv_alloc);
  nenv = 0;
  seq = esl_sq_CreateDigital(sq->abc);

#if DEBUGPIPELINE
  printf("\nPIPELINE p7EnvelopeDef() %s  %" PRId64 " residues\n", sq->name, sq->n);
#endif

  /* determine which generic model we'll need to use based on which pass we're in */
  use_gm = use_Tgm = use_Rgm = use_Lgm = FALSE; /* one of these is set to TRUE below if nec */
  if(! do_local_envdef) { 
    switch(pli->cur_pass_idx) { 
    case PLI_PASS_STD_ANY:          use_gm = TRUE; break;
    case PLI_PASS_5P_ONLY_FORCE:   use_Rgm = TRUE; break;
    case PLI_PASS_3P_ONLY_FORCE:   use_Lgm = TRUE; break;
    case PLI_PASS_5P_AND_3P_FORCE: use_Tgm = TRUE; break;
    default: ESL_FAIL(eslEINVAL, pli->errbuf, "pli_p7_env_def() invalid pass index");
    }
  }

  /* If we're in SCAN mode and we don't yet have the generic model we need, read the 
   * HMM and create it. This could be optimized if we kept the hmm around when we
   * read the generic profile in pass 1 (PLI_PASS_STD_ANY), that way we wouldn't need
   * to read the file each time, we could just use the *opt_gm and the hmm to 
   * create Lgm, Rgm or Tgm as needed.
   */
  if (pli->mode == CM_SCAN_MODELS && 
      ((use_gm  == TRUE && (*opt_gm)  == NULL) || 
       (use_Rgm == TRUE && (*opt_Rgm) == NULL) || 
       (use_Lgm == TRUE && (*opt_Lgm) == NULL) || 
       (use_Tgm == TRUE && (*opt_Tgm) == NULL))) { 
    if((*opt_hmm) == NULL) { 
      /* read the HMM from the file */
      if (pli->cmfp      == NULL) ESL_FAIL(eslENOTFOUND, pli->errbuf, "No file available to read HMM from in pli_p7_env_def()");
      if (pli->cmfp->hfp == NULL) ESL_FAIL(eslENOTFOUND, pli->errbuf, "No file available to read HMM from in pli_p7_env_def()");
      if((status = cm_p7_hmmfile_Read(pli->cmfp, pli->abc, om->offs[p7_MOFFSET], opt_hmm)) != eslOK) ESL_FAIL(status, pli->errbuf, "%s", pli->cmfp->errbuf);
    }

    if((*opt_gm) == NULL) { /* we need gm to create Lgm, Rgm or Tgm */
      *opt_gm = p7_profile_Create((*opt_hmm)->M, pli->abc);
      p7_ProfileConfig(*opt_hmm, bg, *opt_gm, 100, p7_GLOCAL);
    }
    if(use_Rgm && (*opt_Rgm == NULL)) { 
      *opt_Rgm = p7_profile_Clone(*opt_gm);
      p7_ProfileConfig5PrimeTrunc(*opt_Rgm, 100);
    }
    if(use_Lgm && (*opt_Lgm == NULL)) { 
      *opt_Lgm = p7_profile_Clone(*opt_gm);
      p7_ProfileConfig3PrimeTrunc(*opt_hmm, *opt_Lgm, 100);
    }
    if(use_Tgm && (*opt_Tgm == NULL)) { 
      *opt_Tgm = p7_profile_Clone(*opt_gm);
      p7_ProfileConfig(*opt_hmm, bg, *opt_Tgm, 100, p7_LOCAL);
      p7_ProfileConfig5PrimeAnd3PrimeTrunc(*opt_Tgm, 100);
    }
  }
  gm  = *opt_gm;
  Rgm = *opt_Rgm;
  Lgm = *opt_Lgm;
  Tgm = *opt_Tgm;
  
  for (i = 0; i < nwin; i++) {
#if DEBUGPIPELINE    
    printf("p7 envdef win: %4d of %4d [%6" PRId64 "..%6" PRId64 "] pass: %" PRId64 "\n", i, nwin, ws[i], we[i], pli->cur_pass_idx);
#endif
    /* if we require first or final residue, and don't have it, then
     * this window doesn't survive.
     */
    if(cm_pli_PassEnforcesFirstRes(pli->cur_pass_idx) && ws[i] != 1)     continue;
    if(cm_pli_PassEnforcesFinalRes(pli->cur_pass_idx) && we[i] != sq->n) continue; 

    wlen   = we[i]   - ws[i] + 1;
    subdsq = sq->dsq + ws[i] - 1;
    
    /* set up seq object for domaindef function */
    esl_sq_GrowTo(seq, wlen);
    memcpy((void*)(seq->dsq), subdsq, (wlen+1) * sizeof(uint8_t)); 
    seq->dsq[0] = seq->dsq[wlen+1] = eslDSQ_SENTINEL;
    seq->n = wlen;

    p7_bg_SetLength(bg, wlen);
    p7_bg_NullOne(bg, seq->dsq, wlen, &nullsc);

    if(do_local_envdef) { 
      /* Local envelope defn: we can use optimized matrices and,
       * consequently, p7_domaindef_ByPosteriorHeuristics().
       */
      p7_oprofile_ReconfigLength(om, wlen);
      p7_ForwardParser(seq->dsq, wlen, om, pli->oxf, NULL);
      p7_omx_GrowTo(pli->oxb, om->M, 0, wlen);
      p7_BackwardParser(seq->dsq, wlen, om, pli->oxf, pli->oxb, NULL);
      status = p7_domaindef_ByPosteriorHeuristics (seq, om, pli->oxf, pli->oxb, pli->fwd, pli->bck, pli->ddef, NULL, bg, FALSE); 
    }
    else { 
      /* We're defining envelopes in glocal mode, so we need to fill
       * generic fwd/bck matrices and pass them to
       * p7_domaindef_GlocalByPosteriorHeuristics(), but we have to do
       * this differently depending on which pass we're in 
       * (i.e. which type of *gm we're using). 
       */
      if(use_Tgm) { 
	/* no length reconfiguration necessary */
	p7_gmx_GrowTo(pli->gxf, Tgm->M, wlen);
	p7_GForward (seq->dsq, wlen, Tgm, pli->gxf, &fwdsc);
	/*printf("Tfwdsc: %.4f\n", fwdsc);*/
	/* We use local Fwd statistics to determine statistical
	 * significance of this score, it has already had basically a
	 * 1/log(M*(M+1)) penalty for equiprobable local begins and
	 * ends */
	sc_for_pvalue = (fwdsc - nullsc) / eslCONST_LOG2;
	P = esl_exp_surv (sc_for_pvalue,  p7_evparam[CM_p7_LFTAU],  p7_evparam[CM_p7_LFLAMBDA]);
      }
      else if(use_Rgm) { 
	p7_ReconfigLength5PrimeTrunc(Rgm, wlen);
	p7_gmx_GrowTo(pli->gxf, Rgm->M, wlen);
	p7_GForward (seq->dsq, wlen, Rgm, pli->gxf, &fwdsc);
	/*printf("Rfwdsc: %.4f\n", fwdsc);*/
	/* We use local Fwd statistics to determine significance of
	 * the score. GForward penalized 0. for ends and log(1/Rgm->M)
	 * for begins into any state. No further correction is
	 * required because GForward already properly accounted for
	 * equiprobable begins and fixed ends out of node M.
	 */
	Rgm_correction = 0.;
	safe_lfwdsc = fwdsc + Rgm_correction;
	sc_for_pvalue = (safe_lfwdsc - nullsc) / eslCONST_LOG2;
	P = esl_exp_surv (sc_for_pvalue,  p7_evparam[CM_p7_LFTAU],  p7_evparam[CM_p7_LFLAMBDA]);
      }
      else if(use_Lgm) { 
	p7_ReconfigLength3PrimeTrunc(Lgm, wlen);
	p7_gmx_GrowTo(pli->gxf, Lgm->M, wlen);
	p7_GForward (seq->dsq, wlen, Lgm, pli->gxf, &fwdsc);
	/*printf("Lfwdsc: %.4f\n", fwdsc);*/
	/* We use local Fwd statistics to determine significance of
	 * the score, but we need to correct for lack of equiprobable
	 * begins and ends in fwdsc. We correct for fact that local
	 * begins should be equiprobable.
	 */
	Lgm_correction = log(1./Lgm->M); 
	safe_lfwdsc = fwdsc + Lgm_correction; 
	sc_for_pvalue = (safe_lfwdsc - nullsc) / eslCONST_LOG2;
	P = esl_exp_surv (sc_for_pvalue,  p7_evparam[CM_p7_LFTAU],  p7_evparam[CM_p7_LFLAMBDA]);
      }
      else if(use_gm) { /* normal case, not looking for truncated hits */
	p7_ReconfigLength(gm, wlen);
	p7_gmx_GrowTo(pli->gxf, gm->M, wlen);
	p7_GForward (seq->dsq, wlen, gm, pli->gxf, &fwdsc);
	/*printf(" fwdsc: %.4f\n", fwdsc);*/
	sc_for_pvalue = (fwdsc - nullsc) / eslCONST_LOG2;
	P = esl_exp_surv (sc_for_pvalue,  p7_evparam[CM_p7_GFMU],  p7_evparam[CM_p7_GFLAMBDA]);
      }

#if DEBUGPIPELINE	
      if(P > pli->F4) { 
	printf("KILLED   window %5d [%10" PRId64 "..%10" PRId64 "]          gFwd      %6.2f bits  P %g\n", i, ws[i], we[i], sc_for_pvalue, P);
      }
#endif      
	/* Does this score exceed our glocal forward filter threshold? If not, move on to next seq */
      if(P > pli->F4) continue;

      pli->acct[pli->cur_pass_idx].n_past_gfwd++;
      pli->acct[pli->cur_pass_idx].pos_past_gfwd += wlen;

#if DEBUGPIPELINE	
      printf("SURVIVOR window %5d [%10" PRId64 "..%10" PRId64 "] survived gFwd      %6.2f bits  P %g\n", i, ws[i], we[i], sc_for_pvalue, P);
#endif      

      if(pli->do_gfwdbias) {
	/* calculate bias filter score for entire window */
	p7_bg_FilterScore(bg, seq->dsq, wlen, &filtersc);
	/* Once again, score and P-value determination depends on which 
	 * *gm we're using (see F4 code block above for comments).
	 */
	if(use_Tgm) { 
	  sc_for_pvalue = (fwdsc - filtersc) / eslCONST_LOG2;
	  P = esl_exp_surv (sc_for_pvalue,  p7_evparam[CM_p7_LFTAU],  p7_evparam[CM_p7_LFLAMBDA]);
	}
	else if(use_Rgm || use_Lgm) { 
	  sc_for_pvalue = (safe_lfwdsc - nullsc) / eslCONST_LOG2;
	  P = esl_exp_surv (sc_for_pvalue,  p7_evparam[CM_p7_LFTAU],  p7_evparam[CM_p7_LFLAMBDA]);
	}
	else if(use_gm) { /* normal case */
	  sc_for_pvalue = (fwdsc - filtersc) / eslCONST_LOG2;
	  P = esl_exp_surv (sc_for_pvalue,  p7_evparam[CM_p7_GFMU],  p7_evparam[CM_p7_GFLAMBDA]);
	}
	if(P > pli->F4b) continue;
#if DEBUGPIPELINE	
	printf("SURVIVOR window %5d [%10" PRId64 "..%10" PRId64 "] survived gFwdBias  %6.2f bits  P %g\n", i, ws[i], we[i], sc_for_pvalue, P);
#endif 
	pli->acct[pli->cur_pass_idx].n_past_gfwdbias++;
	pli->acct[pli->cur_pass_idx].pos_past_gfwdbias += wlen;
      }
      if(pli->do_time_F4) continue; 

      /* this block needs to match up with if..else if...else if...else block calling p7_GForward above */
      if(use_Tgm) { 
	/* no length reconfiguration necessary */
	p7_gmx_GrowTo(pli->gxb, Tgm->M, wlen);
	p7_GBackward(seq->dsq, wlen, Tgm, pli->gxb, &bcksc);
	if((status = p7_domaindef_GlocalByPosteriorHeuristics(seq, Tgm, pli->gxf, pli->gxb, pli->gfwd, pli->gbck, pli->ddef, pli->do_null2)) != eslOK) ESL_FAIL(status, pli->errbuf, "unexpected failure during glocal envelope defn"); 
	/*printf("Tbcksc: %.4f\n", bcksc);*/
      }
      else if(use_Rgm) { 
	p7_gmx_GrowTo(pli->gxb, Rgm->M, wlen);
	p7_GBackward(seq->dsq, wlen, Rgm, pli->gxb, &bcksc);
	if((status = p7_domaindef_GlocalByPosteriorHeuristics(seq, Rgm, pli->gxf, pli->gxb, pli->gfwd, pli->gbck, pli->ddef, pli->do_null2)) != eslOK) ESL_FAIL(status, pli->errbuf, "unexpected failure during glocal envelope defn");; 
	/*printf("Rbcksc: %.4f\n", bcksc);*/
      }
      else if(use_Lgm) { 
	p7_gmx_GrowTo(pli->gxb, Lgm->M, wlen);
	p7_GBackward(seq->dsq, wlen, Lgm, pli->gxb, &bcksc);
	if((status = p7_domaindef_GlocalByPosteriorHeuristics(seq, Lgm, pli->gxf, pli->gxb, pli->gfwd, pli->gbck, pli->ddef, pli->do_null2)) != eslOK) ESL_FAIL(status, pli->errbuf, "unexpected failure during glocal envelope defn");
	/*printf("Lbcksc: %.4f\n", bcksc);*/
      }
      else { /* normal case, not looking for truncated hits */
	p7_gmx_GrowTo(pli->gxb, gm->M, wlen);
	p7_GBackward(seq->dsq, wlen, gm, pli->gxb, &bcksc);
	if((status = p7_domaindef_GlocalByPosteriorHeuristics(seq, gm, pli->gxf, pli->gxb, pli->gfwd, pli->gbck, pli->ddef, pli->do_null2)) != eslOK) ESL_FAIL(status, pli->errbuf, "unexpected failure during glocal envelope defn");
	/*printf(" bcksc: %.4f\n", bcksc);*/
      }
    }
    
    if (status != eslOK) ESL_FAIL(status, pli->errbuf, "envelope definition workflow failure"); /* eslERANGE can happen */
    if (pli->ddef->nregions   == 0)  continue; /* score passed threshold but there's no discrete domains here       */
    if (pli->ddef->nenvelopes == 0)  continue; /* rarer: region was found, stochastic clustered, no envelopes found */
    
    /* For each domain found in the p7_domaindef_*() function, determine if it passes our criteria */
    for(d = 0; d < pli->ddef->ndom; d++) { 
      
      if(do_local_envdef) { /* we called p7_domaindef_ByPosteriorHeuristics() above, which fills pli->ddef->dcl[d].ad, but we don't need it */
	p7_alidisplay_Destroy(pli->ddef->dcl[d].ad);
      }

      env_len = pli->ddef->dcl[d].jenv - pli->ddef->dcl[d].ienv +1;
      env_sc  = pli->ddef->dcl[d].envsc;
      
      /* Make a correction to the score based on the fact that our envsc,
       * from hmmsearch's p7_pipeline():
       * here is the p7_pipeline code, verbatim: 
       *  Ld = hit->dcl[d].jenv - hit->dcl[d].ienv + 1;
       *  hit->dcl[d].bitscore = hit->dcl[d].envsc + (sq->n-Ld) * log((float) sq->n / (float) (sq->n+3)); 
       *  hit->dcl[d].dombias  = (pli->do_null2 ? p7_FLogsum(0.0, log(bg->omega) + hit->dcl[d].domcorrection) : 0.0); 
       *  hit->dcl[d].bitscore = (hit->dcl[d].bitscore - (nullsc + hit->dcl[d].dombias)) / eslCONST_LOG2; 
       *  hit->dcl[d].pvalue   = esl_exp_surv (hit->dcl[d].bitscore,  om->evparam[p7_FTAU], om->evparam[p7_FLAMBDA]);
       *  
       *  and here is the same code, simplified with our different names for the variables, etc 
       * (we don't use hit->dcl the way p7_pipeline does after this):  */
      env_sc            = env_sc + (wlen - env_len) * log((float) wlen / (float) (wlen+3)); /* NATS, for the moment... */
      env_edefbias      = (pli->do_null2 ? p7_FLogsum(0.0, log(bg->omega) + pli->ddef->dcl[d].domcorrection) : 0.0); /* NATS, and will stay so */
      env_sc_for_pvalue = (env_sc - (nullsc + env_edefbias)) / eslCONST_LOG2; /* now BITS, as it should be */

      if(use_Rgm) env_sc_for_pvalue += Rgm_correction / eslCONST_LOG2; /* glocal env def penalized 0. for ends and log(1/Lgm->M) for begins into any state */
      if(use_Lgm) env_sc_for_pvalue += Lgm_correction / eslCONST_LOG2; /* glocal env def penalized 0. for ends and 0. for begins into M1 */

      if(do_local_envdef || use_Tgm || use_Rgm || use_Lgm) P = esl_exp_surv (env_sc_for_pvalue,  p7_evparam[CM_p7_LFTAU], p7_evparam[CM_p7_LFLAMBDA]);
      else                                                 P = esl_exp_surv (env_sc_for_pvalue,  p7_evparam[CM_p7_GFMU],  p7_evparam[CM_p7_GFLAMBDA]);
      /***************************************************/
      
      /* check if we can skip this envelope based on its P-value or bit-score */
      if(P > pli->F5) continue;
    
      /* Define envelope to search with CM */
      estart = pli->ddef->dcl[d].ienv;
      eend   = pli->ddef->dcl[d].jenv;

#if DEBUGPIPELINE
      printf("SURVIVOR envelope     [%10" PRId64 "..%10" PRId64 "] survived F5       %6.2f bits  P %g\n", pli->ddef->dcl[d].ienv + ws[i] - 1, pli->ddef->dcl[d].jenv + ws[i] - 1, env_sc_for_pvalue, P);
#endif
      pli->acct[pli->cur_pass_idx].n_past_edef++;
      pli->acct[pli->cur_pass_idx].pos_past_edef += env_len;

      /* if we're doing a bias filter on envelopes - check if we skip envelope due to that */
      if(pli->do_edefbias) {
	/* calculate bias filter score for entire window 
	 * may want to test alternative strategies in the future.
	 */
	p7_bg_FilterScore(bg, seq->dsq, wlen, &filtersc);
	env_sc_for_pvalue = (env_sc - filtersc) / eslCONST_LOG2;
	
	if(use_Rgm) env_sc_for_pvalue += Rgm_correction / eslCONST_LOG2; /* glocal env def penalized 0. for ends and log(1/Lgm->M) for begins into any state */
	if(use_Lgm) env_sc_for_pvalue += Lgm_correction / eslCONST_LOG2; /* glocal env def penalized 0. for ends and 0. for begins into M1 */

	if(do_local_envdef || use_Tgm || use_Rgm || use_Lgm) P = esl_exp_surv (env_sc_for_pvalue,  p7_evparam[CM_p7_LFTAU], p7_evparam[CM_p7_LFLAMBDA]);
	else                                                 P = esl_exp_surv (env_sc_for_pvalue,  p7_evparam[CM_p7_GFMU],  p7_evparam[CM_p7_GFLAMBDA]);
	if(P > pli->F5b) continue;
      }
#if DEBUGPIPELINE
      printf("SURVIVOR envelope     [%10" PRId64 "..%10" PRId64 "] survived F5-bias  %6.2f bits  P %g\n", pli->ddef->dcl[d].ienv + ws[i] - 1, pli->ddef->dcl[d].jenv + ws[i] - 1, env_sc_for_pvalue, P);
#endif
      pli->acct[pli->cur_pass_idx].n_past_edefbias++;
      pli->acct[pli->cur_pass_idx].pos_past_edefbias += env_len;
	
      if(pli->do_time_F5) { continue; }

      /* if we get here, the envelope has survived, add it to the growing list */
      if((nenv+1) == nenv_alloc) { 
	nenv_alloc *= 2;
	ESL_RALLOC(es, p, sizeof(int64_t) * nenv_alloc);
	ESL_RALLOC(ee, p, sizeof(int64_t) * nenv_alloc);
      }
      es[nenv] = estart + ws[i] - 1;
      ee[nenv] = eend   + ws[i] - 1;
      nenv++;
    }
    pli->ddef->ndom = 0; /* reset for next use */
  }

  /* clean up, set return variables, and return */
  if(seq != NULL) esl_sq_Destroy(seq);

  *ret_es = es;
  *ret_ee = ee;
  *ret_nenv = nenv;

  return eslOK;

 ERROR:
  ESL_EXCEPTION(eslEMEM, "Error: out of memory");
}

/* Function:  pli_cyk_env_filter()
 * Synopsis:  Given envelopes defined by an HMM, use CYK as a filter.
 * Incept:    EPN, Thu Mar  1 12:03:09 2012
 *
 * Purpose:   For each envelope x, from <es[x]>..<ee[x]>, run 
 *            CYK to see if any hits above threshold exist, if so
 *            the hit will survive the filter. 
 *
 *            In a normal pipeline run, this function call should be
 *            just after a call to pli_p7_env_def().
 *
 *            This function is similar to pli_cyk_seq_filter(), but
 *            differs in that it takes as input envelope boundaries
 *            defined by an HMM as input, while cyk_seq_filter() takes
 *            as input full length sequences that have not been
 *            analyzed with an HMM.
 *
 *            If pli->mode is CM_SCAN_MODELS, it's possible that we
 *            haven't yet read our CM from the file. This is true when
 *            (*opt_cm == NULL). If so, we read the CM from the file
 *            after positioning it to position <cm_offset> and
 *            configure the CM after setting cm->config_opts to
 *            <pli->cm_config_opts>. 
 *
 * Returns:   <eslOK> on success. If a significant hit is obtained,
 *            its information is added to the growing <hitlist>.
 *
 *            <eslEINVAL> if (in a scan pipeline) we're supposed to
 *            set GA/TC/NC bit score thresholds but the model doesn't
 *            have any.
 *
 * Throws:    <eslEMEM> on allocation failure.
 */
int
pli_cyk_env_filter(CM_PIPELINE *pli, off_t cm_offset, const ESL_SQ *sq, int64_t *p7es, int64_t *p7ee, int np7env, CM_t **opt_cm, 
		    int64_t **ret_es, int64_t **ret_ee, int *ret_nenv)
{
  int              status;
  float            sc;                     /* bit score */
  double           P;                      /* P-value of a hit */
  int              i, si;                  /* counters */
  double           save_tau;               /* CM's tau upon entering function */
  int64_t          cyk_envi, cyk_envj;     /* cyk_envi..cyk_envj is new envelope as defined by CYK hits */
  float            cyk_env_cutoff;         /* bit score cutoff for envelope redefinition */
  CM_t            *cm = NULL;              /* ptr to *opt_cm, for convenience only */
  int              qdbidx;                 /* scan matrix qdb idx, defined differently for filter and final round */

  int             *i_surv = NULL;          /* [0..i..np7env-1], TRUE if hit i survived CYK filter, FALSE if not */
  int64_t          nenv = 0;               /* number of hits that survived CYK filter */
  int64_t         *es = NULL;              /* [0..si..nenv-1] start posn of surviving envelope si */
  int64_t         *ee = NULL;              /* [0..si..nenv-1] end   posn of surviving envelope si */

  if (sq->n == 0)  return eslOK;    /* silently skip length 0 seqs; they'd cause us all sorts of weird problems */
  if (np7env == 0) return eslOK;    /* if there's no envelopes to search in, return */

  ESL_ALLOC(i_surv, sizeof(int) * np7env); 
  esl_vec_ISet(i_surv, np7env, FALSE);

  /* if we're in SCAN mode, and we don't yet have a CM, read it and configure it */
  if (pli->mode == CM_SCAN_MODELS && (*opt_cm == NULL)) { 
    if((status = pli_scan_mode_read_cm(pli, cm_offset, opt_cm)) != eslOK) return status;
  }
  else { /* *opt_cm should be valid */
    if(opt_cm == NULL || *opt_cm == NULL) ESL_FAIL(eslEINCOMPAT, pli->errbuf, "Entered pli_final_stage() with invalid CM"); 
  }
  cm = *opt_cm;
  save_tau = cm->tau;

  /* Determine bit score cutoff for CYK envelope redefinition, any
   * residue that exists in a CYK hit that reaches this threshold will
   * be included in the redefined envelope, any that doesn't will not
   * be.
   */
  cyk_env_cutoff = cm->expA[pli->fcyk_cm_exp_mode]->mu_extrap + (log(pli->F6env) / (-1 * cm->expA[pli->fcyk_cm_exp_mode]->lambda));

#if DEBUGPIPELINE
  printf("\nPIPELINE EnvCYKFilter() %s  %" PRId64 " residues\n", sq->name, sq->n);
#endif

  for (i = 0; i < np7env; i++) {
#if DEBUGPIPELINE
    printf("\nSURVIVOR Envelope %5d [%10ld..%10ld] being passed to EnvCYKFilter   pass: %" PRId64 "\n", i, p7es[i], p7ee[i], pli->cur_pass_idx);
#endif
    cm->search_opts  = pli->fcyk_cm_search_opts;
    cm->tau          = pli->fcyk_tau;
    qdbidx           = (cm->search_opts & CM_SEARCH_NONBANDED) ? SMX_NOQDB : SMX_QDB1_TIGHT;
    status = pli_dispatch_cm_search(pli, cm, sq->dsq, p7es[i], p7ee[i], NULL, 0., cyk_env_cutoff, qdbidx, &sc, NULL,
				       (pli->do_fcykenv) ? &cyk_envi : NULL, 
				       (pli->do_fcykenv) ? &cyk_envj : NULL);

    if(status == eslERANGE) {
      pli->acct[pli->cur_pass_idx].n_overflow_fcyk++;
      continue; /* skip envelopes that would require too big of a HMM banded matrix */
    }
    else if(status != eslOK) return status;
    
    P = esl_exp_surv(sc, cm->expA[pli->fcyk_cm_exp_mode]->mu_extrap, cm->expA[pli->fcyk_cm_exp_mode]->lambda);

    if (P > pli->F6) continue;
    
    i_surv[i] = TRUE;
    nenv++;
    /* update envelope boundaries, if nec */
    if(pli->do_fcykenv && (cyk_envi != -1 && cyk_envj != -1)) { 
      p7es[i] = cyk_envi;
      p7ee[i] = cyk_envj;
    }

#if DEBUGPIPELINE
    printf("SURVIVOR envelope     [%10" PRId64 "..%10" PRId64 "] survived EnvCYKFilter       %6.2f bits  P %g\n", p7es[i], p7ee[i], sc, P);
#endif
  }
  /* create list of surviving envelopes */
  if(nenv > 0) { 
    ESL_ALLOC(es, sizeof(int64_t) * nenv);
    ESL_ALLOC(ee, sizeof(int64_t) * nenv);
    si = 0;
    for(i = 0; i < np7env; i++) { 
      if(i_surv[i]) { 
	es[si] = p7es[i];
	ee[si] = p7ee[i];
	pli->acct[pli->cur_pass_idx].n_past_cyk++;
	pli->acct[pli->cur_pass_idx].pos_past_cyk += ee[si] - es[si] + 1;
	si++;
      }
    }
    free(i_surv);
  }
  cm->tau = save_tau;
  *ret_es   = es;
  *ret_ee   = ee;
  *ret_nenv = nenv;

  return eslOK;

 ERROR: 
  cm->tau = save_tau;
  if(i_surv != NULL) free(i_surv);
  if(es     != NULL) free(es);
  if(ee     != NULL) free(ee);
  *ret_es   = NULL;
  *ret_ee   = NULL;
  *ret_nenv = 0;
  ESL_FAIL(status, pli->errbuf, "out of memory");
}


/* Function:  pli_cyk_seq_filter()
 * Synopsis:  Given a sequence, use CYK as a filter and to define
 *            surviving windows.
 *
 * Incept:    EPN, Thu Mar  1 12:03:09 2012
 *
 * Purpose:   Run scanning CYK to see if any hits in <dsq> above 
 *            threshold exist. Then append adjacent residues to
 *            all such hits, merge those that overlap, and return
 *            information on the number of resulting windows and
 *            the locations of those windows in <ret_nwin>, <ret_ws>
 *            and <ret_we>.
 *
 *            This function is only called in a pipeline run if
 *            HMMs were not used to define envelopes, so when used
 *            it is the first stage of the pipeline. 
 *
 *            This function is similar to pli_cyk_env_filter(), but
 *            differs in that it takes as input a single full length
 *            sequences, while EnvCYKFilter takes as input envelopes
 *            defined by an HMM filter.
 *
 *            If pli->mode is CM_SCAN_MODELS, it's possible that we
 *            haven't yet read our CM from the file. This is true when
 *            (*opt_cm == NULL). If so, we read the CM from the file
 *            after positioning it to position <cm_offset> and
 *            configure the CM after setting cm->config_opts to
 *            <pli->cm_config_opts>. 
 *
 * Returns:   <eslOK> on success. If a significant hit is obtained,
 *            its information is added to the growing <hitlist>.
 *
 *            <eslEINVAL> if (in a scan pipeline) we're supposed to
 *            set GA/TC/NC bit score thresholds but the model doesn't
 *            have any.
 *
 * Throws:    <eslEMEM> on allocation failure.
 */
int
pli_cyk_seq_filter(CM_PIPELINE *pli, off_t cm_offset, const ESL_SQ *sq, CM_t **opt_cm, int64_t **ret_ws, int64_t **ret_we, int *ret_nwin)
{
  int              status;
  float            sc;                     /* bit score */
  double           save_tau;               /* CM's tau upon entering function */
  float            cutoff;                 /* CYK bit score cutoff, anything above this survives */
  CM_t            *cm = NULL;              /* ptr to *opt_cm, for convenience only */
  int              qdbidx;                 /* scan matrix qdb idx, defined differently for filter and final round */
  CM_TOPHITS      *sq_hitlist = NULL;      /* hits found in sq, local to this function */
  int              do_merge;               /* TRUE to merge overlapping windows at end of function */
  int              h;                      /* counter over hits */

  int64_t          nwin = 0;               /* number of windows that survived CYK filter */
  int64_t         *ws = NULL;              /* [0..i..nwin-1] start posn of surviving window i */
  int64_t         *we = NULL;              /* [0..i..nwin-1] end   posn of surviving window i */
  int              alloc_size = 1000;      /* chunk size for allocating ws, we */
  int              nwin_alloc = 0;         /* current size of ws, we */
  int64_t          iwin, jwin;             /* start, stop positions of a window */
  int64_t          next_iwin;              /* start position of next window */

  if (sq->n == 0) return eslOK;    /* silently skip length 0 seqs; they'd cause us all sorts of weird problems */

  if(pli->fcyk_cm_search_opts & CM_SEARCH_HBANDED) ESL_FAIL(eslEINCOMPAT, pli->errbuf, "pli_cyk_seq_filter() trying to use HMM bands");

  /* if we're in SCAN mode, and we don't yet have a CM, read it and configure it */
  if (pli->mode == CM_SCAN_MODELS && (*opt_cm == NULL)) { 
    if((status = pli_scan_mode_read_cm(pli, cm_offset, opt_cm)) != eslOK) return status;
  }
  else { /* *opt_cm should be valid */
    if(opt_cm == NULL || *opt_cm == NULL) ESL_FAIL(eslEINCOMPAT, pli->errbuf, "Entered pli_final_stage() with invalid CM"); 
  }
  cm = *opt_cm;

  cm->search_opts = pli->fcyk_cm_search_opts;
  save_tau        = cm->tau;
  cm->tau         = pli->fcyk_tau;
  qdbidx          = (cm->search_opts & CM_SEARCH_NONBANDED) ? SMX_NOQDB : SMX_QDB1_TIGHT;
  cutoff          = cm->expA[pli->fcyk_cm_exp_mode]->mu_extrap + (log(pli->F6) / (-1 * cm->expA[pli->fcyk_cm_exp_mode]->lambda));
  sq_hitlist      = cm_tophits_Create();
  status = pli_dispatch_cm_search(pli, cm, sq->dsq, 1, sq->n, sq_hitlist, cutoff, 0., qdbidx, &sc, NULL, NULL, NULL);
  if(status == eslERANGE) ESL_FAIL(status, pli->errbuf, "pli_cyk_seq_filter(), internal error, trying to use a HMM banded matrix");
  else if(status != eslOK) return status;

  /* To be safe, we only trust that start..stop of our filter-passing
   * hit is within the real hit, so we add (W-1) to start point i and
   * subtract (W-1) from j, and treat this region j-(W-1)..i+(W-1) as
   * having survived the filter.  And (unless we're using HMM bands in
   * the final stage) we merge overlapping hits following this
   * addition of residues.
   */
  
  do_merge = (pli->final_cm_search_opts & CM_SEARCH_HBANDED) ? FALSE : TRUE;
  if(do_merge) { 
    /* sort hits by position, so we can merge them after padding out */
    cm_tophits_SortByPosition(sq_hitlist);
    /* any hits in sq_hitlist will be sorted by increasing end point j */
    /* cm_tophits_Dump(stdout, sq_hitlist); */
  }

  for(h = 0; h < sq_hitlist->N; h++) { 
    if(sq_hitlist->hit[h]->stop < sq_hitlist->hit[h]->start) ESL_FAIL(eslEINVAL, pli->errbuf, "pli_cyk_seq_filter() internal error: hit is in revcomp");    

    iwin = ESL_MAX(1,     sq_hitlist->hit[h]->stop  - (cm->W-1));
    jwin = ESL_MIN(sq->n, sq_hitlist->hit[h]->start + (cm->W-1));

#if DEBUGPIPELINE
    double P = esl_exp_surv(sq_hitlist->hit[h]->score, cm->expA[pli->fcyk_cm_exp_mode]->mu_extrap, cm->expA[pli->fcyk_cm_exp_mode]->lambda);
    printf("SURVIVOR window       [%10" PRId64 "..%10" PRId64 "] survived SeqCYKFilter   %6.2f bits  P %g\n", iwin, jwin, sq_hitlist->hit[h]->score, P);
#endif

    if(do_merge) { 
      if((h+1) < sq_hitlist->N) { 
	next_iwin = ((h+1) < sq_hitlist->N) ? ESL_MAX(1, sq_hitlist->hit[h+1]->stop - (cm->W-1)) : sq->n+1;
	while(next_iwin <= jwin) { /* merge hit h and h+1 */
	  h++;
	  jwin = ESL_MIN(sq->n, sq_hitlist->hit[h]->start + (cm->W-1));
	  /* printf("\t merging with hit %" PRId64 "..%" PRId64 "\n", next_iwin, jwin); */
	  next_iwin = ((h+1) < sq_hitlist->N) ? ESL_MAX(1, sq_hitlist->hit[h+1]->stop - (cm->W-1)) : sq->n+1;
	  /* if (h == tophits->N-1) (last hit) next_i will be set as sq->n+1, breaking the while() */
	}
      }
    }

    if(nwin == nwin_alloc) { 
      nwin_alloc += alloc_size;
      ESL_REALLOC(ws, sizeof(int64_t) * nwin_alloc);
      ESL_REALLOC(we, sizeof(int64_t) * nwin_alloc);
    }
    ws[nwin] = iwin;
    we[nwin] = jwin;
    nwin++;

    pli->acct[pli->cur_pass_idx].n_past_cyk++;
    pli->acct[pli->cur_pass_idx].pos_past_cyk += jwin-iwin+1;
  }
  cm->tau = save_tau;

  if(sq_hitlist != NULL) cm_tophits_Destroy(sq_hitlist);
  *ret_ws   = ws;
  *ret_we   = we;
  *ret_nwin = nwin;

  return eslOK;

 ERROR: 
  if(sq_hitlist != NULL) cm_tophits_Destroy(sq_hitlist);
  cm->tau = save_tau;
  *ret_ws   = NULL;
  *ret_we   = NULL;
  *ret_nwin = 0;
  ESL_FAIL(status, pli->errbuf, "out of memory");
}

/* Function:  pli_final_stage()
 * Synopsis:  Final stage of pipeline: Inside or CYK.
 * Incept:    EPN, Sun Nov 28 13:47:31 2010
 *
 * Purpose:   For each envelope x, from <es[x]>..<ee[x]>, run 
 *            Inside or CYK for final hit definition.
 *
 *            In a normal pipeline run, this function call should be
 *            just after a call to pli_cyk_env_filter().
 *
 *            If pli->mode is CM_SCAN_MODELS, it's possible that we
 *            haven't yet read our CM from the file. This is true when
 *            (*opt_cm == NULL). If so, we read the CM from the file
 *            after positioning it to position <cm_offset> and
 *            configure the CM after setting cm->config_opts to
 *            <pli->cm_config_opts>. 
 *
 * Returns:   <eslOK> on success. If a significant hit is obtained,
 *            its information is added to the growing <hitlist>.
 *
 *            <eslEINVAL> if (in a scan pipeline) we're supposed to
 *            set GA/TC/NC bit score thresholds but the model doesn't
 *            have any.
 *
 * Throws:    <eslEMEM> on allocation failure.
 */
int
pli_final_stage(CM_PIPELINE *pli, off_t cm_offset, const ESL_SQ *sq, int64_t *es, int64_t *ee, int nenv, CM_TOPHITS *hitlist, CM_t **opt_cm)
{
  int              status;
  CM_HIT          *hit = NULL;        /* ptr to the current hit output data   */
  float            sc;                /* bit score */
  int              i, h;              /* counters */
  int              nhit;              /* number of hits reported */
  double           save_tau;          /* CM's tau upon entering function */
  CM_t            *cm = NULL;         /* ptr to *opt_cm, for convenience only */
  CP9Bands_t      *scan_cp9b = NULL;  /* a copy of the HMM bands derived in the final CM search stage, if its HMM banded */
  int              qdbidx;            /* scan matrix qdb idx, defined differently for filter and final round */
  int              used_hb;           /* TRUE if HMM bands used to scan current envelope (cm->cp9b valid) */
  if (sq->n == 0) return eslOK;    /* silently skip length 0 seqs; they'd cause us all sorts of weird problems */
  if (nenv == 0)  return eslOK;    /* if there's no envelopes to search in, return */

  /* if we're in SCAN mode, and we don't yet have a CM, read it and configure it */
  if (pli->mode == CM_SCAN_MODELS && (*opt_cm == NULL)) { 
    if((status = pli_scan_mode_read_cm(pli, cm_offset, opt_cm)) != eslOK) return status;
  }
  else { /* *opt_cm should be valid */
    if(opt_cm == NULL || *opt_cm == NULL) ESL_FAIL(eslEINCOMPAT, pli->errbuf, "Entered pli_final_stage() with invalid CM"); 
  }
  cm = *opt_cm;
  save_tau = cm->tau;

  for (i = 0; i < nenv; i++) {
#if DEBUGPIPELINE
    printf("\nSURVIVOR Envelope %5d [%10ld..%10ld] being passed to Final stage   pass: %" PRId64 "\n", i, es[i], ee[i], pli->cur_pass_idx);
#endif
    nhit             = hitlist->N;
    cm->search_opts  = pli->final_cm_search_opts;
    cm->tau          = pli->final_tau;
    qdbidx           = (cm->search_opts & CM_SEARCH_NONBANDED) ? SMX_NOQDB : SMX_QDB2_LOOSE;
    status = pli_dispatch_cm_search(pli, cm, sq->dsq, es[i], ee[i], hitlist, pli->T, 0., qdbidx, &sc, &used_hb, NULL, NULL);
    if(status == eslERANGE) {
      pli->acct[pli->cur_pass_idx].n_overflow_final++;
      continue; /* skip envelopes that would require too big a HMM banded matrix */
    }
    else if(status != eslOK) return status;

    /* save a copy of the bands we calculated for the final search stage */
    if(used_hb && (! pli->do_hb_recalc)) { 
      scan_cp9b = cp9_CloneBands(cm->cp9b, pli->errbuf);
      if(scan_cp9b == NULL) return eslEMEM;
#if eslDEBUGLEVEL >= 1
      if((status = cp9_ValidateBands(cm, pli->errbuf, cm->cp9b, es[i], ee[i])) != eslOK) return status;
      ESL_DPRINTF1(("original bands validated.\n"));
      if((status = cp9_ValidateBands(cm, pli->errbuf, scan_cp9b, es[i], ee[i])) != eslOK) return status;
      ESL_DPRINTF1(("cloned bands validated.\n"));
#endif
    }
    else { 
      scan_cp9b = NULL;
    }
  
    /* add info to each hit DP scanning functions didn't have access to, and align the hits if nec */
    for (h = nhit; h < hitlist->N; h++) { 
      hit = &(hitlist->unsrt[h]);
      hit->cm_idx   = pli->cur_cm_idx;
      hit->seq_idx  = pli->cur_seq_idx;
      hit->pass_idx = pli->cur_pass_idx;
      hit->pvalue   = esl_exp_surv(hit->score, cm->expA[pli->final_cm_exp_mode]->mu_extrap, cm->expA[pli->final_cm_exp_mode]->lambda);
      hit->srcL     = sq->L; /* this may be -1, in which case it will be updated by caller (cmsearch or cmscan) when full length is known */

      /* initialize remaining values we don't know yet */
      hit->evalue  = 0.;
      hit->ad      = NULL;
	  
      if (pli->mode == CM_SEARCH_SEQS) { 
	if (                       (status  = esl_strdup(sq->name, -1, &(hit->name)))  != eslOK) ESL_FAIL(eslEMEM, pli->errbuf, "allocation failure");
	if (sq->acc[0]  != '\0' && (status  = esl_strdup(sq->acc,  -1, &(hit->acc)))   != eslOK) ESL_FAIL(eslEMEM, pli->errbuf, "allocation failure");
        if (sq->desc[0] != '\0' && (status  = esl_strdup(sq->desc, -1, &(hit->desc)))  != eslOK) ESL_FAIL(eslEMEM, pli->errbuf, "allocation failure");
      } 
      else {
	if ((status  = esl_strdup(cm->name, -1, &(hit->name)))  != eslOK) ESL_FAIL(eslEMEM, pli->errbuf, "allocation failure");
	if ((status  = esl_strdup(cm->acc,  -1, &(hit->acc)))   != eslOK) ESL_FAIL(eslEMEM, pli->errbuf, "allocation failure");
        if ((status  = esl_strdup(cm->desc, -1, &(hit->desc)))  != eslOK) ESL_FAIL(eslEMEM, pli->errbuf, "allocation failure");
      }
#if DEBUGPIPELINE
      printf("SURVIVOR envelope     [%10ld..%10ld] survived Inside    %6.2f bits  P %g\n", hit->start, hit->stop, hit->score, hit->pvalue);
#endif
      /* Get an alignment of the hit. 
       */
      /* check if we need to overwrite cm->cp9b with scan_cp9b
       * because alignment of previous hit modified them in previous 
       * call to pli_align_hit(). 
       */
      if(h > nhit && scan_cp9b != NULL) { 
	if(cm->cp9b != NULL) FreeCP9Bands(cm->cp9b);
	cm->cp9b = cp9_CloneBands(scan_cp9b, pli->errbuf);
	if(cm->cp9b == NULL) return status;
      }
      /*cm_hit_Dump(stdout, hit);*/
      if((status = pli_align_hit(pli, cm, sq, hit, used_hb)) != eslOK) return status;
      
      /* Finally, if we're using model-specific bit score thresholds,
       * determine if the significance of the hit (is it reported
       * and/or included?)  Adapted from Sean's comments at an
       * analogous point in p7_pipeline():
       *
       * If we're using model-specific bit score thresholds (GA | TC | NC)
       * and we're in a cmscan pipeline (mode = CM_SCAN_MODELS), then we
       * *must* apply those reporting or inclusion thresholds now, because
       * this model is about to go away; we won't have its thresholds
       * after all targets have been processed.
       * 
       * If we're using E-value thresholds and we don't know the
       * search space size (Z_setby == CM_ZSETBY_NTARGETS), we 
       * *cannot* apply those thresholds now, and we *must* wait 
       * until all targets have been processed (see cm_tophits_Threshold()).
       * 
       * For any other thresholding, it doesn't matter whether we do
       * it here (model-specifically) or at the end (in
       * cm_tophits_Threshold()). 
       * 
       * What we actually do, then, is to set the flags if we're using
       * model-specific score thresholds (regardless of whether we're
       * in a scan or a search pipeline); otherwise we leave it to 
       * cm_tophits_Threshold(). cm_tophits_Threshold() is always
       * responsible for *counting* the reported, included sequences.
       * 
       * [xref J5/92]
       */
      
      if (pli->use_bit_cutoffs) { 
	if (cm_pli_TargetReportable(pli, hit->score, hit->evalue)) { /* evalue is invalid, but irrelevant if pli->use_bit_cutoffs */
	  hit->flags |= CM_HIT_IS_REPORTED;
	  if (cm_pli_TargetIncludable(pli, hit->score, hit->evalue)) /* ditto */
	    hit->flags |= CM_HIT_IS_INCLUDED;
	}
      }
    } /* end of 'for(h = nhit'... */
    if(scan_cp9b != NULL) { 
      FreeCP9Bands(scan_cp9b);
      scan_cp9b = NULL;
    }
  } /* end of for each envelope loop */
  cm->tau = save_tau;
  /* free the scan matrices if we just allocated them */
  if(scan_cp9b != NULL) FreeCP9Bands(scan_cp9b);

  return eslOK;
}

/* Function:  pli_dispatch_cm_search()
 * Synopsis:  Search a sequence from <start> to <end> with a CM.
 * Incept:    EPN, Thu Mar  1 10:29:53 2012
 *
 * Purpose:   Use a CM scanning DP algorithm to scan <dsq> from
 *            <start> to <stop>. The specific algorithm to use
 *            is specified by cm->search_opts and pli->cur_pass_idx.
 * 
 * Args:      pli         - the pipeline
 *            cm          - the CM
 *            dsq         - sequence to search
 *            start       - first position of dsq to search
 *            stop        - final position of dsq to search
 *            hitlist     - CM_TOPHITS to add to, can be NULL
 *            cutoff      - min bit score to report to hitlist, 
 *                          irrelevant if hitlist is NULL
 *            env_cutoff  - min bit score for env redefn, 
 *                          irrelevant if opt_envi, opt_envj are NULL
 *            qdbidx      - index 
 *            ret_sc      - RETURN: score returned by scanner
 *            opt_used_hb - OPT RETURN: TRUE if HMM banded scanner used, FALSE if not
 *            opt_envi    - OPT RETURN: redefined envelope start (can be NULL)
 *            opt_envj    - OPT RETURN: redefined envelope stop  (can be NULL)
 *
 * Returns: eslOK on success.
 *          eslERANGE if we wanted to do HMM banded, but couldn't.
 */
int pli_dispatch_cm_search(CM_PIPELINE *pli, CM_t *cm, ESL_DSQ *dsq, int64_t start, int64_t stop, CM_TOPHITS *hitlist, float cutoff, 
			   float env_cutoff, int qdbidx, float *ret_sc, int *opt_used_hb, int64_t *opt_envi, int64_t *opt_envj)
{
  int status;   
  int do_trunc            = cm_pli_PassAllowsTruncation(pli->cur_pass_idx);
  int do_inside           = (cm->search_opts & CM_SEARCH_INSIDE) ? TRUE : FALSE;
  int do_hbanded          = (cm->search_opts & CM_SEARCH_HBANDED) ? TRUE : FALSE;
  int do_qdb_or_nonbanded = (do_hbanded) ? FALSE : TRUE; /* will get set to TRUE later if do_hbanded and mx too big */
  double save_tau         = cm->tau;
  float  hbmx_Mb = 0.;     /* approximate size in Mb for HMM banded matrix for this sequence */
  float  sc;               /* score returned from DP scanner */
  int    used_hb = FALSE;  /* TRUE if HMM banded scanner was used, FALSE if not */

  /*printf("in pli_dispatch_cm_search(): do_trunc: %d do_inside: %d cutoff: %.1f env_cutoff: %.1f do_hbanded: %d hitlist?: %d opt_envi/j?: %d start: %" PRId64 " stop: %" PRId64 "\n", 
    do_trunc, do_inside, cutoff, env_cutoff, do_hbanded, (hitlist == NULL) ? 0 : 1, (opt_envi == NULL && opt_envj == NULL) ? 0 : 1, start, stop); */

  if(do_hbanded) { 
    status = cp9_IterateSeq2Bands(cm, pli->errbuf, dsq, start, stop, pli->cur_pass_idx, pli->hb_size_limit, TRUE, pli->maxtau, pli->xtau, &hbmx_Mb);
    if(status == eslOK) { /* bands imply a matrix or size pli->hb_size_limit or smaller with tau == cm->tau <= pli->maxtau */
      if(do_trunc) { /* HMM banded, truncated */
	if(do_inside) { 
	  status = FTrInsideScanHB(cm, pli->errbuf, cm->trhb_mx, pli->hb_size_limit, pli->cur_pass_idx, dsq, start, stop,
				   cutoff, hitlist, pli->do_null3, env_cutoff, opt_envi, opt_envj, NULL, &sc);
	}
	else { 
	  status = TrCYKScanHB(cm, pli->errbuf, cm->trhb_mx, pli->hb_size_limit, pli->cur_pass_idx, dsq, start, stop,
			       cutoff, hitlist, pli->do_null3, env_cutoff, opt_envi, opt_envj, NULL, &sc);
	}
      }
      else { /* HMM banded, not truncated */
	if(do_inside) { 
	  status = FastFInsideScanHB(cm, pli->errbuf, cm->hb_mx, pli->hb_size_limit, dsq, start, stop,
				     cutoff, hitlist, pli->do_null3, env_cutoff, opt_envi, opt_envj, &sc);
	}
	else { 
	  status = FastCYKScanHB(cm, pli->errbuf, cm->hb_mx, pli->hb_size_limit, dsq, start, stop,
				 cutoff, hitlist, pli->do_null3, env_cutoff, opt_envi, opt_envj, &sc);
	}
      }
      used_hb = TRUE;
    }
    if     (status == eslERANGE) { do_qdb_or_nonbanded = TRUE; }
    else if(status != eslOK)     { printf("pli_dispatch_cm_search(), error: %s\n", pli->errbuf); goto ERROR; }
  }
  
  if(do_qdb_or_nonbanded) { /* careful, different from just an 'else', b/c we may have just set this as true if status == eslERANGE */
    if(do_trunc) { 
      if(cm->trsmx == NULL) { printf("FIX ME! round, cm->trsmx is NULL, probably overflow sized hb mx (do_inside: %d, tau: %g, hbmx_Mb: %g Mb \n", do_inside, cm->tau, hbmx_Mb); goto ERROR; } 
      if(do_inside) { /* not HMM banded, truncated */
	status = RefITrInsideScan(cm, pli->errbuf, cm->trsmx, qdbidx, pli->cur_pass_idx, dsq, start, stop, 
				  cutoff, hitlist, pli->do_null3, env_cutoff, opt_envi, opt_envj, NULL, NULL, &sc);
      }
      else { 
	status = RefTrCYKScan(cm, pli->errbuf, cm->trsmx, qdbidx, pli->cur_pass_idx, dsq, start, stop, 
			      cutoff, hitlist, pli->do_null3, env_cutoff, opt_envi, opt_envj, NULL, NULL, &sc);
      }
    }
    else { /* not HMM banded, not truncated */
      if(cm->smx == NULL) { printf("FIX ME! round, cm->smx is NULL, probably overflow sized hb mx (do_inside: %d, tau: %g, hbmx_Mb: %g Mb)\n", do_inside, cm->tau, hbmx_Mb); goto ERROR; } 
      if(do_inside) { 
	status = FastIInsideScan(cm, pli->errbuf, cm->smx, qdbidx, dsq, start, stop, 
				 cutoff, hitlist, pli->do_null3, env_cutoff, opt_envi, opt_envj, NULL, &sc);
      }
      else { 
	status = FastCYKScan(cm, pli->errbuf, cm->smx, qdbidx, dsq, start, stop, 
			     cutoff, hitlist, pli->do_null3, env_cutoff, opt_envi, opt_envj, NULL, &sc);
      }
    }
    if(status != eslOK) { printf("cm_pli_Dispatch_SqCMSearch(), error: %s\n", pli->errbuf); goto ERROR; }
    used_hb = FALSE;
  }

  /* revert to original parameters */
  cm->tau = save_tau;

  *ret_sc = sc;
  if(opt_used_hb != NULL) *opt_used_hb = used_hb;

  return eslOK;
  
 ERROR: 
  cm->tau = save_tau;
  *ret_sc      = IMPOSSIBLE;
  if(opt_used_hb != NULL) *opt_used_hb = FALSE;
  if(opt_envi    != NULL) *opt_envi = start;
  if(opt_envj    != NULL) *opt_envi = stop;
  return eslERANGE;
}


/* Function:  pli_align_hit()
 * Synopsis:  Align a hit that survives all stages of the pipeline to a CM.
 * Incept:    EPN, Mon Aug  8 10:46:21 2011
 *
 * Purpose:   For a given hit <hit> in sequence <sq> spanning
 *            <hit->start> to <hit->stop>, align it to a CM and create a
 *            CM_ALIDISPLAY object and store it in <hit->ad>. 
 *
 *            The algorithm used is dictated by pli->cm_align_opts.
 *            But if we wanted HMM banded alignment but it required
 *            too much memory, we failover to small D&C CYK.
 *
 * Returns: eslOK on success, alidisplay in hit->ad.
 *          ! eslOK on an error, pli->errbuf is filled, hit->ad is NULL.
 */
int
pli_align_hit(CM_PIPELINE *pli, CM_t *cm, const ESL_SQ *sq, CM_HIT *hit, int cp9b_valid)
{
  int            status;           /* Easel status code */
  CM_ALNDATA    *adata  = NULL;    /* alignment data */
  ESL_SQ        *sq2aln = NULL;    /* copy of the hit, req'd by DispatchSqAlignment() */
  ESL_STOPWATCH *watch  = NULL;    /* stopwatch for timing alignment step */
  float          null3_correction; /* null 3 bit score penalty, for CYK score */
  float          hbmx_Mb;          /* size of required HB mx */
  int            used_hbands;      /* TRUE if HMM bands used to compute aln */

  if(cm->cmcons == NULL) ESL_FAIL(eslEINCOMPAT, pli->errbuf, "pli_align_hit() cm->cmcons is NULL");

  if((watch = esl_stopwatch_Create()) == NULL) ESL_FAIL(eslEMEM, pli->errbuf, "out of memory");
  esl_stopwatch_Start(watch);  

  /* make new sq object, b/c DispatchSqAlignment() requires one */
  if((sq2aln = esl_sq_CreateDigitalFrom(cm->abc, "seq", sq->dsq + hit->start - 1, hit->stop - hit->start + 1, NULL, NULL, NULL)) == NULL) goto ERROR;

  cm->align_opts = pli->cm_align_opts;
  if(pli->cur_pass_idx != PLI_PASS_STD_ANY) cm->align_opts |= CM_ALIGN_TRUNC;

  /* 1. Do HMM banded alignment, if we want one.  
   * 2. Do D&C CYK alignment, if we want one or we wanted HMM banded
   *    alignment but it wasn't possible b/c the mx was too big.
   */
  if(cm->align_opts & CM_ALIGN_HBANDED) { 
    /* Align with HMM bands, if we can do it in the allowed amount of memory */
    if((! cp9b_valid) || pli->do_hb_recalc) { 
      /* Calculate HMM bands. We'll increase tau and recalculate bands until 
       * the resulting HMM banded matrix is under our size limit or we
       * reach maximum allowed tau (pli->maxtau).
       */
      cm->tau = pli->final_tau;
      status = cp9_IterateSeq2Bands(cm, pli->errbuf, sq2aln->dsq, 1, sq2aln->L, pli->cur_pass_idx, pli->hb_size_limit, FALSE, pli->maxtau, pli->xtau, &hbmx_Mb);
      if(status != eslOK && status != eslERANGE) goto ERROR;
      /* eslERANGE is okay, mx is too big, we'll failover to D&C aln
       * below.  But only after we redetermine mx size in
       * DispatchSqAlignment(), which is slightly wasteful...
       */
    }
    else { 
      /* we have existing CP9 HMM bands from the final search stage,
       * shift them by a fixed offset, this guarantees our alignment
       * will be the same hit our search found. (After this cm->cp9b
       * bands would fail a cp9_ValidateBands() check..., but they'll
       * work for our purposes here.
       */
      cp9_ShiftCMBands(cm, hit->start, hit->stop, (cm->align_opts & CM_ALIGN_TRUNC) ? TRUE : FALSE);
    }
  
    /* compute the HMM banded alignment */
    status = DispatchSqAlignment(cm, pli->errbuf, sq2aln, -1, pli->hb_size_limit, hit->mode, pli->cur_pass_idx,
				 TRUE, /* TRUE: cp9b bands are valid, don't recalc them */
				 NULL, NULL, NULL, &adata);
    if(status == eslOK) {
      pli->acct[pli->cur_pass_idx].n_aln_hb++;
      used_hbands = TRUE;
    }
    else if(status == eslERANGE) { 
      /* matrix was too big, alignment not computed, failover to small CYK */
      cm->align_opts &= ~CM_ALIGN_HBANDED;
      cm->align_opts &= ~CM_ALIGN_OPTACC;
      cm->align_opts &= ~CM_ALIGN_POST;
      cm->align_opts |= CM_ALIGN_NONBANDED;
      cm->align_opts |= CM_ALIGN_SMALL;
      cm->align_opts |= CM_ALIGN_CYK;
    }
    else goto ERROR;
    esl_stopwatch_Stop(watch); /* we started it above before we calc'ed the CP9 bands */ 
  }

  if(! (cm->align_opts & CM_ALIGN_HBANDED)) { 
    /* careful, not just an else, we may have just turned off CM_ALIGN_HBANDED bc mx was too big */
    esl_stopwatch_Start(watch);
    if((status = DispatchSqAlignment(cm, pli->errbuf, sq2aln, -1, pli->hb_size_limit, hit->mode, pli->cur_pass_idx,
				     FALSE, NULL, NULL, NULL, &adata)) != eslOK) goto ERROR;
    pli->acct[pli->cur_pass_idx].n_aln_dccyk++;
    used_hbands = FALSE;
    esl_stopwatch_Stop(watch);
  }
  /* ParsetreeDump(stdout, tr, cm, sq2aln->dsq); */
    
  /* add null3 correction to sc if nec */
  if(pli->do_null3) { 
    ScoreCorrectionNull3CompUnknown(cm->abc, cm->null, sq2aln->dsq, 1, sq2aln->L, cm->null3_omega, &null3_correction);
    adata->sc  -= null3_correction;
    hit->n3corr = null3_correction;
  }

  /* create the CM_ALIDISPLAY object */
  if((status = cm_alidisplay_Create(cm, pli->errbuf, adata, sq, hit->start, used_hbands, watch->elapsed, &(hit->ad))) != eslOK) goto ERROR;

  /* clean up and return */
  cm->align_opts = pli->cm_align_opts; /* restore these */
  if(adata  != NULL) cm_alndata_Destroy(adata, FALSE); /* FALSE: don't free adata->sqp (sq2aln) */
  if(sq2aln != NULL) esl_sq_Destroy(sq2aln); 
  if(watch  != NULL) esl_stopwatch_Destroy(watch); 

  return eslOK;

 ERROR:
  cm->align_opts = pli->cm_align_opts; /* restore these */
  if(adata  != NULL) cm_alndata_Destroy(adata, FALSE); /* FALSE: don't free adata->sqp (sq2aln) */
  if(sq2aln != NULL) esl_sq_Destroy(sq2aln); 
  if(watch  != NULL) esl_stopwatch_Destroy(watch);
  hit->ad = NULL;
  return status;
}

/* Function:  pli_scan_mode_read_cm()
 * Synopsis:  Read a CM from the CM file, mid-pipeline.
 * Incept:    EPN, Thu Mar  1 15:00:27 2012
 *
 * Purpose:   When in scan mode, we don't read the CM until 
 *            we know we're going to need it, i.e. at least
 *            one envelope has survived all HMM filters (or
 *            HMM filters are turned off). Here, we read
 *            the CM from the file, configure it and return 
 *            it in <ret_cm>. We also update the pipeline 
 *            regarding the CM we just read.
 *
 * Returns:   <eslOK> on success. <ret_cm> contains the CM.
 *
 * Throws:    <eslEMEM> on allocation failure
 *            <eslEINCOMPAT> on contract violation.
 *            In both case, *ret_cm set to NULL, pli->errbuf filled.
 */
int
pli_scan_mode_read_cm(CM_PIPELINE *pli, off_t cm_offset, CM_t **ret_cm)
{
  int status; 
  CM_t *cm = NULL;
  int  check_fcyk_beta;  /* TRUE if we just read a CM and we need to check if its beta1 == pli->fcyk_beta */
  int  check_final_beta; /* TRUE if we just read a CM and we need to check if its beta2 == pli->final_beta */

  if (pli->mode != CM_SCAN_MODELS) ESL_FAIL(eslEINCOMPAT, pli->errbuf, "pli_scan_mode_read_cm(), pipeline isn't in SCAN mode");
  if (*ret_cm != NULL) ESL_FAIL(eslEINCOMPAT, pli->errbuf, "pli_scan_mode_read_cm(), *ret_cm != NULL");

#ifdef HMMER_THREADS
  /* lock the mutex to prevent other threads from reading the file at the same time */
  if (pli->cmfp->syncRead) { 
    if (pthread_mutex_lock (&pli->cmfp->readMutex) != 0) ESL_FAIL(eslESYS, pli->errbuf, "mutex lock failed");
  }
#endif
  cm_file_Position(pli->cmfp, cm_offset);
  if((status = cm_file_Read(pli->cmfp, FALSE, &(pli->abc), &cm)) != eslOK) ESL_FAIL(status, pli->errbuf, "%s", pli->cmfp->errbuf);
#ifdef HMMER_THREADS
  if (pli->cmfp->syncRead) { 
    if (pthread_mutex_unlock (&pli->cmfp->readMutex) != 0) ESL_EXCEPTION(eslESYS, "mutex unlock failed");
  }
#endif    
  cm->config_opts = pli->cm_config_opts;
  cm->align_opts  = pli->cm_align_opts;
  /* check if we need to recalculate QDBs prior to building the scan matrix in cm_Configure() 
   * (we couldn't do this until we read the CM file to find out what cm->qdbinfo->beta1/beta2 were 
   */
  check_fcyk_beta  = (pli->fcyk_cm_search_opts  & CM_SEARCH_QDB) ? TRUE : FALSE;
  check_final_beta = (pli->final_cm_search_opts & CM_SEARCH_QDB) ? TRUE : FALSE;
  if((status = CheckCMQDBInfo(cm->qdbinfo, pli->fcyk_beta, check_fcyk_beta, pli->final_beta, check_final_beta)) == eslFAIL) { 
    cm->config_opts   |= CM_CONFIG_QDB;
    cm->qdbinfo->beta1 = pli->fcyk_beta;
    cm->qdbinfo->beta2 = pli->final_beta;
  }
  /* else we don't have to change cm->qdbinfo->beta1/beta2 */
  
  if((status = cm_Configure(cm, pli->errbuf, -1)) != eslOK) goto ERROR;
  /* update the pipeline about the model */
  if((status = cm_pli_NewModel(pli, CM_NEWMODEL_CM, cm, cm->clen, cm->W, 
			       NULL, NULL, pli->cur_cm_idx)) /* om, bg */
     != eslOK) goto ERROR;
  
  *ret_cm = cm;
  return eslOK;

 ERROR: 
  if(cm != NULL) FreeCM(cm);
  *ret_cm = NULL;
  return status;
}

/* Function:  copy_subseq()
 * Incept:    EPN, Tue Aug  9 11:13:02 2011
 *
 * Purpose: Copy a subsequence of an existing sequence <src_sq>
 *           starting at position <i>, of length <L> to another
 *           sequence object <dest_sq>. Copy only residues from
 *           <i>..<i>+<L>-1. <dest_sq> must be pre-allocated.
 *
 * Returns: eslOK on success. 
 */
void
copy_subseq(const ESL_SQ *src_sq, ESL_SQ *dest_sq, int64_t i, int64_t L)
{ 
  /*Printf("entering copy_subseq i: %" PRId64 " j: %" PRId64 " L: %" PRId64 " start-end: %" PRId64 "... %" PRId64 "\n",
    i, i+L-1, L, src_sq->start, src_sq->end);
    fflush(stdout);*/

  esl_sq_Reuse(dest_sq);
  esl_sq_GrowTo(dest_sq, L);
  memcpy((void*)(dest_sq->dsq+1), src_sq->dsq+i, L * sizeof(ESL_DSQ));
  dest_sq->dsq[0] = dest_sq->dsq[L+1] = eslDSQ_SENTINEL;
  dest_sq->n      = L;
  dest_sq->L      = src_sq->L;

  if(src_sq->start <= src_sq->end) { 
    ESL_DASSERT1((L <= (src_sq->end - src_sq->start + 1)));
    /*assert(L <= (src_sq->end - src_sq->start + 1));*/
    dest_sq->start = src_sq->start  + i - 1;
    dest_sq->end   = dest_sq->start + L - 1;
  }
  else { 
    ESL_DASSERT1((L <= (src_sq->start - src_sq->end + 1)));
    /*assert(L <= (src_sq->start - src_sq->end + 1));*/
    dest_sq->start = src_sq->end    + L - 1;
    dest_sq->end   = dest_sq->start - L + 1;
  }
  /*printf("leaving copy_subseq dest_sq->start..end: %" PRId64 " %" PRId64 " n: %" PRId64 "\n",
    dest_sq->start, dest_sq->end, dest_sq->n);*/

  esl_sq_SetName     (dest_sq, src_sq->name);
  esl_sq_SetAccession(dest_sq, src_sq->acc);
  esl_sq_SetDesc     (dest_sq, src_sq->desc);

  return;
}

#if 0
/* EPN, Fri Mar 2 13:46:18 2012 
 * This function was developed when I was experimenting with using
 * multiple HMM filters, which was later scrapped. It's left here for
 * reference.
 */

/* Function:  merge_windows_from_two_lists()
 * Synopsis:  Merge two sorted lists of windows into one, collapsing overlapping windows together. 
 * Incept:    EPN, Mon Nov 29 14:47:40 2010
 *
 * Purpose:   Given two lists of windows, merge them into one list
 *            by collapsing any overlapping windows into a single window. 
 *
 *            Each window in list 1 is defined as:
 *            <ws1[i]>..we1[i] for i=0..<nwin1-1>
 *
 *            Each window in list 2 is defined as:
 *            <ws2[i]>..we2[i] for i=0..<nwin2-1>
 *
 *            The windows within each list must be sorted in
 *            increasing order and be non-overlapping, i.e. the
 *            following must hold:
 *
 *            ws1[i] <  ws1[j] for all i < j.
 *            ws1[i] <= we1[i] for all i.
 *
 *            ws2[i] <  ws2[j] for all i < j.
 *            ws2[i] <= we2[i] for all i.
 *
 *            Note, we check that these conditions hold at the
 *            beginning of the function, and return eslEINVAL if not.
 *
 *            A new list is created and returned, as <mws>,
 *            <mwe>, and <nmwin>, start, end positions and 
 *            number of windows respectively. 
 *
 *            The lowest P-value of any of the merged windows is also
 *            kept, and returned in <ret_mwp>, <ret_mwp[i]> is the
 *            lowest P-value of any of the P-values from <wp1>/<wp2>
 *            for windows that were merged together to create
 *            <mws>..<mwe>. If the P-value was from a window in list 1
 *            (<wp1>), <ret_mwl[i]> is set as 1, if it is from a
 *            window from list 2 (<wp2>), <ret_mwl[i]> is set as 2.
 * 
 * Returns:   New merged list in <ret_mws>, <ret_mwe>, <ret_mwp>,
 *            <ret_mwl>, <ret_nmwin>, storing start position, end
 *            position, P-value and list origin of each of the
 *            <ret_nmwin> windows in the merged list.
 */
int
merge_windows_from_two_lists(int64_t *ws1, int64_t *we1, double *wp1, int *wl1, int nwin1, int64_t *ws2, int64_t *we2, double *wp2, int *wl2, int nwin2, int64_t **ret_mws, int64_t **ret_mwe, double **ret_mwp, int **ret_mwl, int *ret_nmwin)
{
  int status;
  int64_t *mws = NULL;
  int64_t *mwe = NULL;
  double  *mwp = NULL;
  int     *mwl = NULL;
  int nmwin;
  int mi, i1, i2;
  int nalloc;

  /* check that our required conditions hold */
  for(i1 = 0; i1 < nwin1-1; i1++) if(ws1[i1] >= ws1[i1+1]) return eslEINVAL;
  for(i2 = 0; i2 < nwin2-1; i2++) if(ws2[i2] >= ws2[i2+1]) return eslEINVAL;
  for(i1 = 0; i1 < nwin1; i1++) if(ws1[i1] > we1[i1]) return eslEINVAL;
  for(i2 = 0; i2 < nwin2; i2++) if(ws2[i2] > we2[i2]) return eslEINVAL;

  nalloc = nwin1 + nwin2; /* we'll never exceed this */
  ESL_ALLOC(mws, sizeof(int64_t) * nalloc);
  ESL_ALLOC(mwe, sizeof(int64_t) * nalloc);
  ESL_ALLOC(mwp, sizeof(double)  * nalloc);
  ESL_ALLOC(mwl, sizeof(int)     * nalloc);

  i1 = 0;
  i2 = 0;
  mi = 0;
  while(i1 < nwin1 || i2 < nwin2) { 
    if((i1 < nwin1) && ((i2 == nwin2) || (ws1[i1] < ws2[i2]))) {
      /**************************************************
       * case 1: our next hit, in order is from list 1 *
       **************************************************/
      /* initialize merged hit as copy of hit from list 1 */
      mws[mi] = ws1[i1]; /* our merged window begins at ws1[i1], this won't change */
      mwp[mi] = wp1[i1]; /* for now, our best P-value is wp1[i1] */
      mwl[mi] = wl1[i1]; /* for now, our best P-value comes from list 1 */
      mwe[mi] = we1[i1]; /* for now, our merged window ends at we1[i1] */
      /* merge with all overlapping windows in list 2 */
      while((i2 < nwin2) && (mwe[mi] >= ws2[i2])) { /* check all windows in list 2 that overlap with our current list 1 window */
	mwe[mi] = ESL_MAX(mwe[mi], we2[i2]); /* our merged window now ends at larger of mwe[me] and we2[i2] */
	if(wp2[i2] < mwp[mi]) { 
	  mwp[mi] = wp2[i2];  /* our best P-value now is wp2[i2] */
	  mwl[mi] = wl2[i2];  /* our best P-value now comes from list 2 */
	}
	i2++;
      }
      i1++;
      /* finally, if we merged any windows from list 2, our window is possibly larger, 
       * so we merge with all now-overlapping windows in list 1 */
      while((i1 < nwin1) && (mwe[mi] >= ws1[i1])) { 
	mwe[mi] = ESL_MAX(mwe[mi], we1[i1]); /* our merged window now ends at larger of mwe[me] and we2[i2] */
	if(wp1[i1] < mwp[mi]) { 
	  mwp[mi] = wp1[i1];  /* our best P-value now is wp1[i1] */
	  mwl[mi] = wl1[i1];  /* our best P-value now comes from list 1 */
	}
	i1++;
      }
      mi++;
    }
    else { 
      /****************************************************************************************************************
       * case 2: our next hit, in order is from list 2, same code as case 1, with list 1 and list 2 variables inverted *
       ****************************************************************************************************************/
      /* initialize merged hit as copy of hit from list 2 */
      mws[mi] = ws2[i2]; /* our merged window begins at ws1[i2], this won't change */
      mwp[mi] = wp2[i2]; /* for now, our best P-value is wp2[i2] */
      mwl[mi] = wl2[i2]; /* for now, our best P-value comes from list 2 */
      mwe[mi] = we2[i2]; /* for now, our merged window ends at we2[i2] */
      /* merge with all overlapping windows in list 1 */
      while((i1 < nwin1) && (mwe[mi] >= ws1[i1])) { /* check all windows in list 1 that overlap with our current list 2 window */
	mwe[mi] = ESL_MAX(mwe[mi], we1[i1]); /* our merged window now ends at larger of mwe[me] and we1[i1] */
	if(wp1[i1] < mwp[mi]) { 
	  mwp[mi] = wp1[i1];  /* our best P-value now is wp1[i1] */
	  mwl[mi] = wl1[i1];  /* our best P-value now comes from list 1 */
	}
	i1++;
      }
      i2++;
      /* finally, if we merged any windows from list 1, our window is possibly larger, 
       * so we merge with all now-overlapping windows in list 2 */
      while((i2 < nwin2) && (mwe[mi] >= ws2[i2])) { /* check all windows in list 2 that overlap with our current list 1 window */
	mwe[mi] = ESL_MAX(mwe[mi], we2[i2]); /* our merged window now ends at larger of mwe[me] and we2[i2] */
	if(wp2[i2] < mwp[mi]) { 
	  mwp[mi] = wp2[i2];  /* our best P-value now is wp2[i2] */
	  mwl[mi] = wl2[i2];  /* our best P-value now comes from list 2 */
	}
	i2++;
      }
      mi++;
    }
  }
  nmwin = mi;

  /*
  for(i1 = 0; i1 < nwin1; i1++) printf("list1 win %5d  %10" PRId64 "..%10" PRId64 " P: %10g\n", i1, ws1[i1], we1[i1], wp1[i1]);
  printf("\n");
  for(i2 = 0; i2 < nwin2; i2++) printf("list2 win %5d  %10" PRId64 "..%10" PRId64 " P: %10g\n", i2, ws2[i2], we2[i2], wp2[i2]);
  printf("\n");
  for(mi = 0; mi < nmwin; mi++) printf("mlist win %5d  %10" PRId64 "..%10" PRId64 " P: %10g  %d\n", mi, mws[mi], mwe[mi], mwp[mi], mwl[mi]);
  printf("\n");
  */

  *ret_mws = mws;
  *ret_mwe = mwe;
  *ret_mwp = mwp;
  *ret_mwl = mwl;
  *ret_nmwin = nmwin;

  return eslOK;

 ERROR:
  return status;
}
#endif

/*****************************************************************
 * 3. Example 1: "search mode" in a sequence db
 *****************************************************************/
/*****************************************************************
 * 4. Example 2: "scan mode" in an HMM db
 *****************************************************************/
/*****************************************************************
 * @LICENSE@
 *****************************************************************/
