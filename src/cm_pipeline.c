/* Infernal's accelerated seq/profile comparison pipeline
 *  
 * Contents:
 *   1. CM_PIPELINE: allocation, initialization, destruction
 *   2. Pipeline API
 *   3. Example 1: search mode (in a sequence db)
 *   TODO: 4. Example 2: scan mode (in an HMM db)
 *   5. Copyright and license information
 * 
 * SRE, Fri Dec  5 10:09:39 2008 [Janelia] [BSG3, Bear McCreary]
 * SVN $Id: p7_pipeline.c 3352 2010-08-24 20:56:01Z eddys $
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
#include "funcs.h"
#include "structs.h"

#define DOPRINT 1

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
 *            The configuration <go> must include settings for the 
 *            following options:
 *            
 *            || option      ||            description                     || usually  ||
 *            | --noali      |  don't output alignments (smaller output)    |   FALSE   |
 *            | -E           |  report hits <= this E-value threshold       |    10.0   |
 *            | -T           |  report hits >= this bit score threshold     |    NULL   |
 *            | -Z           |  set initial hit search space size           |    NULL   |
 *            | --incE       |  include hits <= this E-value threshold      |    0.01   |
 *            | --incT       |  include hits >= this bit score threshold    |    NULL   |
 *            | --cut_ga     |  model-specific thresholding using GA        |   FALSE   |
 *            | --cut_nc     |  model-specific thresholding using NC        |   FALSE   |
 *            | --cut_tc     |  model-specific thresholding using TC        |   FALSE   |
 *            | --max        |  turn all heuristic filters off              |   FALSE   |
 *            | --F1         |  Stage 1 (MSV) thresh: promote hits P <= F1  |    0.35   |
 *            | --F2         |  Stage 2 (Vit) thresh: promote hits P <= F2  |    0.10   |
 *            | --F3         |  Stage 3 (Fwd) thresh: promote hits P <= F3  |    0.02   |
 *            | --nonull2    |  turn OFF biased comp score correction       |    TRUE   |
 *            | --seed       |  RNG seed (0=use arbitrary seed)             |      42   |
 *            | --acc        |  prefer accessions over names in output      |   FALSE   |
 * *** opts below this line are unique to cm_pipeline.c ***
 *            | --nonull3    |  turn off NULL3 correction                   |   FALSE   |
 *            | -g           |  configure the CM for glocal alignment       |   FALSE   |
 *            | --dF3        |  Stage 3 (Fwd) per-domain thresh             |   0.01    |
 *            | --domsvbias  |  turn ON composition bias filter HMM for MSV |   FALSE   |
 *            | --novitbias  |  turn OFF composition bias filter HMM for Vit |   FALSE   |
 *            | --nofwdbias  |  turn OFF composition bias filter HMM for Fwd |   FALSE   |
 *            | --nodombias  |  turn OFF composition bias filter HMM for ddef|   FALSE   |
 *            | --F1b        |  Stage 1 (MSV) bias filter thresh            |   OFF     |
 *            | --F2b        |  Stage 2 (Vit) bias filter thresh            |   OFF     |
 *            | --F3b        |  Stage 3 (Fwd) bias filter thresh            |   OFF     |
 *            | --dF3b       |  Stage 3 (Fwd) domain bias filter thresh     |   OFF     |
 *            | --dtF3       |  Stage 3 (Fwd) per-domain bit sc thresh      |   NULL    |
 *            | --F4         |  Stage 4 (CYK) thresh: promote hits P <= F4  |   5e-4    |
 *            | --E4         |  Stage 4 (CYK) thres: promote hits E <= E4   |   NULL    |
 *            | --fast       |  set filters at strict-level                 |   FALSE   |
 *            | --mid        |  set filters at mid-level                    |   FALSE   |
 *            | --cyk        |  set final search stage as CYK, not inside   |   FALSE   |
 *            | --hmm        |  don't use CM at all, HMM only               |   FALSE   |
 *            | --noddef     |  don't domainize windows before CM stages    |   FALSE   |
 *            | --nomsv      |  skip MSV filter stage                       |   FALSE   |
 *            | --novit      |  skip Viterbi filter stage                   |   FALSE   |
 *            | --nofwd      |  skip Forward filter stage                   |   FALSE   |
 *            | --nohmm      |  skip all HMM filter stages                  |   FALSE   |
 *            | --nocyk      |  don't filter with CYK, skip to final stage  |   FALSE   |
 *            | --fnoqdb     |  don't use QDBs in CYK filter                |   FALSE   |
 *            | --fbeta      |  set beta for QDBs in CYK filter to this     |   1e-9    |
 *            | --fhbanded   |  use HMM bands for CYK filter                |   FALSE   |
 *            | --ftau       |  set tau for HMM bands in CYK filter to this |   5e-6    |
 *            | --fsums      |  use sums to get CYK filter HMM bands        |   FALSE   |
 *            | --tau        |  set tau for final HMM bands to this         |   FALSE   |
 *            | --sums       |  use sums to get final HMM bands             |   FALSE   |
 *            | --qdb        |  use QDBs, not HMM bands in final stage      |   FALSE   |
 *            | --beta       |  set beta for QDBs in final stage            |   1e-15   |
 *            | --nonbanded  |  don't use QDBs or HMM bands in final stage  |   FALSE   |
 *            | --rt1        |  set domain def rt1 parameter as <x>         |   0.25    |
 *            | --rt2        |  set domain def rt2 parameter as <x>         |   0.10    |
 *            | --rt3        |  set domain def rt3 parameter as <x>         |   0.20    |
 *            | --localweak  |  rescore low-scoring domains in local mode   |   FALSE   |
 *            | --glocaldom  |  define domains in glocal mode               |   FALSE   |
 *            | --glocalp    |  use glocal P values for domains             |   FALSE   |
 *            | --ns         |  set number of samples for domain traceback  |   1000    |
 *            | --wsplit     |  split windows after MSV                     |   FALSE   |
 *            | --wmult      |  multiplier for splitting windows if --wsplit|   3.0     |
 * Returns:   ptr to new <cm_PIPELINE> object on success. Caller frees this
 *            with <cm_pipeline_Destroy()>.
 *
 * Throws:    <NULL> on allocation failure.
 */
CM_PIPELINE *
cm_pipeline_Create(ESL_GETOPTS *go, int clen_hint, int L_hint, enum cm_pipemodes_e mode)
{
  CM_PIPELINE *pli  = NULL;
  int          seed = esl_opt_GetInteger(go, "--seed");
  int          status;

  ESL_ALLOC(pli, sizeof(CM_PIPELINE));

  if ((pli->fwd  = p7_omx_Create(clen_hint, L_hint, L_hint)) == NULL) goto ERROR;
  if ((pli->bck  = p7_omx_Create(clen_hint, L_hint, L_hint)) == NULL) goto ERROR;
  if ((pli->oxf  = p7_omx_Create(clen_hint, 0,      L_hint)) == NULL) goto ERROR;
  if ((pli->oxb  = p7_omx_Create(clen_hint, 0,      L_hint)) == NULL) goto ERROR;     
  if ((pli->gfwd = p7_gmx_Create(clen_hint, L_hint))         == NULL) goto ERROR;
  if ((pli->gbck = p7_gmx_Create(clen_hint, L_hint))         == NULL) goto ERROR;
  if ((pli->gxf  = p7_gmx_Create(clen_hint, L_hint))         == NULL) goto ERROR;
  if ((pli->gxb  = p7_gmx_Create(clen_hint, L_hint))         == NULL) goto ERROR;     
  pli->fsmx = NULL; /* can't allocate this until we know dmin/dmax/W etc. */
  pli->smx  = NULL; /* can't allocate this until we know dmin/dmax/W etc. */

  /* Normally, we reinitialize the RNG to the original seed every time we're
   * about to collect a stochastic trace ensemble. This eliminates run-to-run
   * variability. As a special case, if seed==0, we choose an arbitrary one-time 
   * seed: time() sets the seed, and we turn off the reinitialization.
   */
  pli->r                  =  esl_randomness_CreateFast(seed);
  pli->do_reseeding       = (seed == 0) ? FALSE : TRUE;
  pli->ddef               = p7_domaindef_Create(pli->r);
  pli->ddef->do_reseeding = pli->do_reseeding;

  /* Configure reporting thresholds */
  pli->by_E            = TRUE;
  pli->E               = esl_opt_GetReal(go, "-E");
  pli->T               = 0.0;
  pli->use_bit_cutoffs = FALSE;
  if (esl_opt_IsOn(go, "-T")) 
    {
      pli->T    = esl_opt_GetReal(go, "-T"); 
      pli->by_E = FALSE;
    } 

  /* Configure inclusion thresholds */
  pli->inc_by_E           = TRUE;
  pli->incE               = esl_opt_GetReal(go, "--incE");
  pli->incT               = 0.0;
  if (esl_opt_IsOn(go, "--incT")) 
    {
      pli->incT     = esl_opt_GetReal(go, "--incT"); 
      pli->inc_by_E = FALSE;
    } 

  /* Configure for one of the model-specific thresholding options */
  if (esl_opt_GetBoolean(go, "--cut_ga"))
    {
      pli->T        = 0.0;
      pli->by_E     = FALSE;
      pli->incT     = 0.0;
      pli->inc_by_E = FALSE;
      pli->use_bit_cutoffs = CMH_GA;
    }
  if (esl_opt_GetBoolean(go, "--cut_nc"))
    {
      pli->T        = 0.0;
      pli->by_E     = FALSE;
      pli->incT     = 0.0;
      pli->inc_by_E = FALSE;
      pli->use_bit_cutoffs = CMH_NC;
    }
  if (esl_opt_GetBoolean(go, "--cut_tc"))
    {
      pli->T        = 0.0;
      pli->by_E     = FALSE;
      pli->incT     = 0.0;
      pli->inc_by_E = FALSE;
      pli->use_bit_cutoffs = CMH_TC;
    }

  /* Configure search space sizes for E value calculations  */
  pli->Z       = 0.0;
  pli->Z_setby = CM_ZSETBY_DBSIZE;
  if (esl_opt_IsOn(go, "-Z")) 
    {
      pli->Z_setby = CM_ZSETBY_OPTION;
      pli->Z       = esl_opt_GetReal(go, "-Z");
    }

  /* Configure domain definition parameters */
  pli->rt1 = esl_opt_GetReal(go, "--rt1");
  pli->rt2 = esl_opt_GetReal(go, "--rt2");
  pli->rt3 = esl_opt_GetReal(go, "--rt3");
  pli->ns  = esl_opt_GetInteger(go, "--ns");
  pli->ddef->rt1 = pli->rt1;
  pli->ddef->rt2 = pli->rt2;
  pli->ddef->rt3 = pli->rt3;
  pli->ddef->nsamples = pli->ns;
  pli->do_localdoms     = esl_opt_GetBoolean(go, "--localdom");
  pli->do_wsplit        = (esl_opt_GetBoolean(go, "--wnosplit")) ? FALSE : TRUE;
  pli->wmult            = esl_opt_GetReal(go, "--wmult");
  pli->do_wcorr         = esl_opt_GetBoolean(go, "--wcorr");

  /* Configure acceleration pipeline thresholds */
  pli->do_cm         = TRUE;
  pli->do_hmm        = TRUE;
  pli->do_max        = FALSE;
  pli->do_mid        = FALSE;
  pli->do_fast       = FALSE;
  pli->do_domainize  = TRUE;
  pli->do_pad        = esl_opt_GetBoolean(go, "--pad");
  pli->do_msv        = TRUE;
  pli->do_msvbias    = FALSE;
  pli->do_msvnull3   = FALSE;
  pli->do_vit        = TRUE;
  pli->do_vitbias    = TRUE;
  pli->do_vitnull3   = FALSE;
  pli->do_fwd        = TRUE;
  pli->do_fwdbias    = TRUE;
  pli->do_fwdnull3   = FALSE;
  pli->do_dombias    = TRUE;
  pli->do_domnull3   = FALSE;
  pli->do_cyk        = TRUE;
  pli->do_null2      = TRUE;
  pli->do_null3      = TRUE;
  pli->p7_n3omega    = esl_opt_GetReal(go, "--p7n3omega");
  pli->F1     = ESL_MIN(1.0, esl_opt_GetReal(go, "--F1"));
  pli->F2     = ESL_MIN(1.0, esl_opt_GetReal(go, "--F2"));
  pli->F3     = ESL_MIN(1.0, esl_opt_GetReal(go, "--F3"));
  pli->dF3    = ESL_MIN(1.0, esl_opt_GetReal(go, "--dF3"));
  pli->F1b    = ESL_MIN(1.0, esl_opt_GetReal(go, "--F1b"));
  pli->F1n3   = ESL_MIN(1.0, esl_opt_GetReal(go, "--F1n3"));
  pli->F2b    = ESL_MIN(1.0, esl_opt_GetReal(go, "--F2b"));
  pli->F2n3   = ESL_MIN(1.0, esl_opt_GetReal(go, "--F2n3"));
  pli->F3b    = ESL_MIN(1.0, esl_opt_GetReal(go, "--F3b"));
  pli->F3n3   = ESL_MIN(1.0, esl_opt_GetReal(go, "--F3n3"));
  pli->dF3b   = ESL_MIN(1.0, esl_opt_GetReal(go, "--dF3b"));
  pli->dF3n3  = ESL_MIN(1.0, esl_opt_GetReal(go, "--dF3n3"));
  pli->dtF3   = 0.;
  pli->use_dtF3 = FALSE;
  if (esl_opt_IsOn(go, "--dtF3")) { 
    pli->use_dtF3 = TRUE; 
    pli->dtF3   = esl_opt_GetReal(go, "--dtF3");
  }
  pli->F4     = ESL_MIN(1.0, esl_opt_GetReal(go, "--F4"));
  pli->E4     = 1.;   
  pli->use_E4 = FALSE;
  if (esl_opt_IsUsed(go, "--E4")) { 
    pli->E4 = esl_opt_GetReal(go, "--E4");
    pli->use_E4 = TRUE; 
  }
  if(esl_opt_GetBoolean(go, "--nomsv"))    pli->do_msv        = FALSE; 
  if(esl_opt_GetBoolean(go, "--novit"))    pli->do_vit        = FALSE; 
  if(esl_opt_GetBoolean(go, "--nofwd"))    pli->do_fwd        = FALSE; 
  if(esl_opt_GetBoolean(go, "--nocyk"))    pli->do_cyk        = FALSE; 
  if(esl_opt_GetBoolean(go, "--noddef"))   pli->do_domainize  = FALSE; 
  if(esl_opt_GetBoolean(go, "--domsvbias"))pli->do_msvbias    = TRUE;
  if(esl_opt_GetBoolean(go, "--novitbias"))pli->do_vitbias    = FALSE;
  if(esl_opt_GetBoolean(go, "--nofwdbias"))pli->do_fwdbias    = FALSE;
  if(esl_opt_GetBoolean(go, "--domsvnull3"))pli->do_msvnull3  = TRUE;
  if(esl_opt_GetBoolean(go, "--dovitnull3"))pli->do_vitnull3  = TRUE;
  if(esl_opt_GetBoolean(go, "--dofwdnull3"))pli->do_fwdnull3  = TRUE;
  if(esl_opt_GetBoolean(go, "--dodomnull3"))pli->do_domnull3  = TRUE;
  if(esl_opt_GetBoolean(go, "--nodombias"))pli->do_dombias    = FALSE;
  if(esl_opt_GetBoolean(go, "--hmm")) { 
    pli->do_cm  = FALSE;
    pli->do_cyk = FALSE; 
  }
  if(esl_opt_GetBoolean(go, "--nohmm")) { 
    pli->do_hmm        = FALSE; 
    pli->do_msv        = FALSE; 
    pli->do_vit        = FALSE;
    pli->do_fwd        = FALSE; 
    pli->do_domainize  = FALSE; 
    pli->do_msvbias    = FALSE;
    pli->do_vitbias    = FALSE;
    pli->do_fwdbias    = FALSE;
    pli->do_dombias    = FALSE;
  }
  if(esl_opt_GetBoolean(go, "--max")) { /* turn off all filters */
    pli->do_max = TRUE;
    pli->do_hmm = pli->do_msv = pli->do_vit = pli->do_fwd = pli->do_cyk = FALSE; 
    pli->do_msvbias = pli->do_vitbias = pli->do_fwdbias = pli->do_dombias = FALSE;
    pli->F1 = pli->F2 = pli->F3 = pli->dF3 = pli->F4 = 1.0;
  }
  if(esl_opt_GetBoolean(go, "--mid")) { /* set up mid-level filtering */
    pli->do_mid = TRUE;
    pli->do_msv = pli->do_msvbias = pli->do_vitbias = pli->do_fwdbias = pli->do_dombias = FALSE;
    pli->F2 = 0.1;
    pli->F3 = 0.05;
    pli->dF3 = 0.1;
    pli->F4 = 0.001;
  }
  if(esl_opt_GetBoolean(go, "--fast")) { /* set up strict-level filtering */
    /* these are set as nhmmer defaults for HMM filters, default for CYK */
    pli->do_fast = TRUE;
    pli->F1 = 0.02;
    pli->F2 = 0.001;
    pli->F3 = 0.00001;
    pli->dF3 = 0.0001;
  }

  pli->do_time_F1   = esl_opt_GetBoolean(go, "--time-F1");
  pli->do_time_F2   = esl_opt_GetBoolean(go, "--time-F2");
  pli->do_time_F3   = esl_opt_GetBoolean(go, "--time-F3");
  pli->do_time_dF3  = esl_opt_GetBoolean(go, "--time-dF3");
  pli->do_time_bfil = esl_opt_GetBoolean(go, "--time-bfil");
  pli->do_time_F4   = esl_opt_GetBoolean(go, "--time-F4");
  pli->do_time_F5   = esl_opt_GetBoolean(go, "--time-F5");

  if (esl_opt_GetBoolean(go, "--nonull2")) pli->do_null2      = FALSE;
  if (esl_opt_GetBoolean(go, "--nonull3")) pli->do_null3      = FALSE;

  /* Configure options for the CM stages */
  pli->fcyk_cm_search_opts  = 0;
  pli->final_cm_search_opts = 0;
  pli->fcyk_beta = esl_opt_GetReal(go, "--fbeta");
  pli->fcyk_tau  = esl_opt_GetReal(go, "--ftau");
  pli->final_beta = esl_opt_GetReal(go, "--beta");
  pli->final_tau  = esl_opt_GetReal(go, "--tau");

  if(! esl_opt_GetBoolean(go, "--hmm")) { 
    /* set up filter round parameters */

    if(  esl_opt_GetBoolean(go, "--fnoqdb"))      pli->fcyk_cm_search_opts  |= CM_SEARCH_NOQDB;
    if(  esl_opt_GetBoolean(go, "--fhbanded"))    pli->fcyk_cm_search_opts  |= CM_SEARCH_HBANDED;
    if(  esl_opt_GetBoolean(go, "--fsums"))       pli->fcyk_cm_search_opts  |= CM_SEARCH_SUMS;
    if(! esl_opt_GetBoolean(go, "--nonull3"))     pli->fcyk_cm_search_opts  |= CM_SEARCH_NULL3;

    /* set up final round parameters */
    if(! esl_opt_GetBoolean(go, "--cyk"))         pli->final_cm_search_opts |= CM_SEARCH_INSIDE;
    if(! esl_opt_GetBoolean(go, "--qdb")) { 
      pli->final_cm_search_opts |= CM_SEARCH_NOQDB;
    }
    if(! esl_opt_GetBoolean(go, "--nonbanded")) { 
      pli->final_cm_search_opts |= CM_SEARCH_HBANDED;
    }
    /* note that --nonbanded and --qdb are exclusive */
    if(  esl_opt_GetBoolean(go, "--sums"))        pli->final_cm_search_opts |= CM_SEARCH_SUMS;
    if(! esl_opt_GetBoolean(go, "--nonull3"))     pli->final_cm_search_opts |= CM_SEARCH_NULL3;
  }

  /* Determine statistics modes for CM stages */
  pli->fcyk_cm_exp_mode  = (esl_opt_GetBoolean(go, "-g")) ? EXP_CM_GC : EXP_CM_LC;
  if(pli->final_cm_search_opts & CM_SEARCH_INSIDE) { 
    pli->final_cm_exp_mode = (esl_opt_GetBoolean(go, "-g")) ? EXP_CM_GI : EXP_CM_LI;
  }
  else {
    pli->final_cm_exp_mode = (esl_opt_GetBoolean(go, "-g")) ? EXP_CM_GC : EXP_CM_LC;
  }

  /* Accounting as we collect results */
  pli->nmodels         = 0;
  pli->nseqs           = 0;
  pli->nres            = 0;
  pli->nnodes          = 0;
  pli->n_past_msv      = 0;
  pli->n_past_vit      = 0;
  pli->n_past_fwd      = 0;
  pli->n_past_ddef     = 0;
  pli->n_past_cyk      = 0;
  pli->n_past_ins      = 0;
  pli->n_output        = 0;
  pli->n_past_msvbias  = 0;
  pli->n_past_vitbias  = 0;
  pli->n_past_fwdbias  = 0;
  pli->n_past_dombias  = 0;
  pli->pos_past_msv    = 0;
  pli->pos_past_vit    = 0;
  pli->pos_past_fwd    = 0;
  pli->pos_past_ddef   = 0;
  pli->pos_past_cyk    = 0;
  pli->pos_past_ins    = 0;
  pli->pos_output      = 0;
  pli->pos_past_msvbias= 0;
  pli->pos_past_vitbias= 0;
  pli->pos_past_fwdbias= 0;
  pli->pos_past_dombias= 0;
  pli->mode            = mode;
  pli->show_accessions = (esl_opt_GetBoolean(go, "--acc")   ? TRUE  : FALSE);
  pli->show_alignments = (esl_opt_GetBoolean(go, "--noali") ? FALSE : TRUE);
  pli->cmfp            = NULL;
  pli->errbuf[0]       = '\0';

  /* EPN TEMP: remove once I get a data structure that holds CM parsetrees */
  pli->show_alignments = FALSE;
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
  if(pli->fsmx != NULL && cm != NULL) cm_FreeScanMatrix(cm, pli->fsmx);
  if(pli->smx != NULL && cm != NULL)  cm_FreeScanMatrix(cm, pli->smx);
  free(pli);
}
/*---------------- end, CM_PIPELINE object ----------------------*/


/*****************************************************************
 * 2. The pipeline API.
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
 * Purpose:   Caller has a new model <cm>, and optimized HMM matrix <om>. 
 *            Prepare the pipeline <pli> to receive this model as either 
 *            a query or a target.
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
cm_pli_NewModel(CM_PIPELINE *pli, CM_t *cm, int *fcyk_dmin, int *fcyk_dmax, int *final_dmin, int *final_dmax, const P7_OPROFILE *om, P7_BG *bg)
{
  int status = eslOK;

  pli->nmodels++;
  pli->nnodes += om->M;

  if(pli->fsmx != NULL) cm_FreeScanMatrix(cm, pli->fsmx);
  if(pli->smx != NULL)  cm_FreeScanMatrix(cm, pli->smx);

  pli->fsmx = cm_CreateScanMatrix(cm, cm->W, fcyk_dmin, fcyk_dmax, 
				  ((fcyk_dmin == NULL && fcyk_dmax == NULL) ? cm->W : fcyk_dmax[0]),
				  pli->fcyk_beta, 
				  ((fcyk_dmin == NULL && fcyk_dmax == NULL) ? FALSE : TRUE),
				  TRUE,    /* do     allocate float matrices for CYK filter round */
				  FALSE);  /* do not allocate int   matrices for CYK filter round  */

  pli->smx  = cm_CreateScanMatrix(cm, cm->W, final_dmin, final_dmax, 
				  ((final_dmin == NULL && final_dmax == NULL) ? cm->W : final_dmax[0]),
				  pli->final_beta, 
				  ((final_dmin == NULL && final_dmax == NULL) ? FALSE : TRUE),
		 		  FALSE,  /* do not allocate float matrices for Inside round */
			 	  TRUE);  /* do     allocate int   matrices for Inside round */

  
  if (pli->Z_setby == p7_ZSETBY_NTARGETS && pli->mode == p7_SCAN_MODELS) pli->Z = pli->nmodels;

  if (pli->do_msvbias || pli->do_vitbias || pli->do_fwdbias || pli->do_dombias) p7_bg_SetFilter(bg, om->M, om->compo);

  pli->W    = cm->W;
  pli->clen = cm->clen;
  esl_vec_FCopy(cm->p7_evparam, CM_p7_NEVPARAM, pli->p7_evparam);

  return status;
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
cm_pli_NewSeq(CM_PIPELINE *pli, CM_t *cm, const ESL_SQ *sq)
{
  int status;
  float T;
  pli->nres += sq->n;
  if (pli->Z_setby == CM_ZSETBY_DBSIZE && pli->mode == CM_SEARCH_SEQS) pli->Z = pli->nres;

  if((status = UpdateExpsForDBSize(cm, NULL, (long) pli->nres))          != eslOK) return status;
  if((status = E2MinScore(cm, NULL, pli->final_cm_exp_mode, pli->E, &T)) != eslOK) return status;
  pli->T = (double) T;

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
  if (p1->mode == CM_SEARCH_SEQS)
    {
      p1->nseqs   += p2->nseqs;
      p1->nres    += p2->nres;
    }
  else
    {
      p1->nmodels += p2->nmodels;
      p1->nnodes  += p2->nnodes;
    }

  p1->n_past_msv  += p2->n_past_msv;
  p1->n_past_vit  += p2->n_past_vit;
  p1->n_past_fwd  += p2->n_past_fwd;
  p1->n_past_ddef += p2->n_past_ddef;
  p1->n_past_cyk  += p2->n_past_cyk;
  p1->n_past_ins  += p2->n_past_ins;
  p1->n_output    += p2->n_output;

  p1->n_past_msvbias += p2->n_past_msvbias;
  p1->n_past_vitbias += p2->n_past_vitbias;
  p1->n_past_fwdbias += p2->n_past_fwdbias;
  p1->n_past_dombias += p2->n_past_dombias;

  p1->pos_past_msv  += p2->pos_past_msv;
  p1->pos_past_vit  += p2->pos_past_vit;
  p1->pos_past_fwd  += p2->pos_past_fwd;
  p1->pos_past_ddef += p2->pos_past_ddef;
  p1->pos_past_cyk  += p2->pos_past_cyk;
  p1->pos_past_ins  += p2->pos_past_ins;
  p1->pos_output    += p2->pos_output;

  p1->pos_past_msvbias += p2->pos_past_msvbias;
  p1->pos_past_vitbias += p2->pos_past_vitbias;
  p1->pos_past_fwdbias += p2->pos_past_fwdbias;
  p1->pos_past_dombias += p2->pos_past_dombias;

  if (p1->Z_setby == p7_ZSETBY_NTARGETS)
    {
      p1->Z += (p1->mode == p7_SCAN_MODELS) ? p2->nmodels : p2->nseqs;
    }
  else
    {
      p1->Z = p2->Z;
    }

  return eslOK;
}

/* Function:  cm_pli_p7Filter()
 * Synopsis:  The accelerated p7 comparison pipeline: MSV through Forward filter.
 * Incept:    EPN, Wed Nov 24 13:07:02 2010
 *            TJW, Fri Feb 26 10:17:53 2018 [Janelia] (p7_Pipeline_Longtargets())
 *
 * Purpose:   Run the accelerated pipeline to compare profile <om>
 *            against sequence <sq>. Some combination of the MSV,
 *            Viterbi and Forward algorithms are used, based on 
 *            option flags set in <pli>. 
 *
 *            In a normal pipeline run, this function call should be
 *            followed by a call to cm_pipeline_p7BoundaryDef().
 *
 * Returns:   <eslOK> on success. For the <ret_nwin> windows that
 *            survive all filters, the start and end positions of the
 *            windows are stored and * returned in <ret_ws> and
 *            <ret_we> respectively.

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
cm_pli_p7Filter(CM_PIPELINE *pli, CM_t *cm, P7_OPROFILE *om, P7_PROFILE *gm, P7_BG *bg, const ESL_SQ *sq, int64_t **ret_ws, int64_t **ret_we, int *ret_nwin)
{
  int              status;
  float            usc, vfsc, fwdsc; /* filter scores                           */
  float            filtersc;         /* HMM null filter score                   */
  float            null3sc;          /* null3 scores                   */
  int              have_filtersc;    /* TRUE if filtersc has been calc'ed for current window */
  int              have_null3sc;     /* TRUE if null3sc has been calc'ed for current window */
  float            nullsc;           /* null model score */
  float            wsc;              /* the corrected bit score for a window */
  double           P;                /* P-value of a hit */
  int              i, i2;            /* counters */
  int              wlen;             /* length of current window */
  void            *p;                /* for ESL_RALLOC */
  int             *useme = NULL;     /* used when merging overlapping windows */
  int              overlap;          /* number of overlapping positions b/t 2 adjacent windows */
  float            wcorr;            /* bit sc correction for a window */
  int            **survAA = NULL;    /* [0..s..Np7_SURV-1][0..i..nwin-1] TRUE if window i survived stage s */
  int              nalloc;           /* currently allocated size for ws, we */
  int64_t         *ws;               /* [0..nwin-1] window start positions */
  int64_t         *we;               /* [0..nwin-1] window end   positions */
  int              nwin;             /* number of windows */
  int64_t         *new_ws;           /* used when copying/modifying ws */
  int64_t         *new_we;           /* used when copying/modifying we */
  int              nsurv_fwd;        /* number of windows that survive fwd filter */
  int              new_nsurv_fwd;    /* used when merging fwd survivors */
  ESL_DSQ         *subdsq;           /* a ptr to the first position of a window */

  if (sq->n == 0) return eslOK;    /* silently skip length 0 seqs; they'd cause us all sorts of weird problems */
  p7_omx_GrowTo(pli->oxf, om->M, 0, sq->n);    /* expand the one-row omx if needed */

  /* Set false target length. This is a conservative estimate of the length of window that'll
   * soon be passed on to later phases of the pipeline;  used to recover some bits of the score
   * that we would miss if we left length parameters set to the full target length */
  p7_oprofile_ReconfigMSVLength(om, om->max_length);

  printf("sq: %s\nsq->n: %" PRId64 " cm->W:  %d  pli->W: %d\n", sq->name, sq->n, cm->W, pli->W);

  /* initializations */
  nsurv_fwd = 0;

  /***************************************************/
  /* Filter 1: MSV, long target-variant, with p7 HMM */
  if(pli->do_msv) { 
    /* ws_int, we_int: workaround for p7_MSVFIlter_longtarget() taking integers, instead of int64_t */
    int *ws_int;
    int *we_int;
    p7_MSVFilter_longtarget(sq->dsq, sq->n, om, pli->oxf, bg, pli->F1, &ws_int, &we_int, &nwin);
    if (nwin == 0 ) return eslOK;
    ESL_ALLOC(ws, sizeof(int64_t) * nwin);
    ESL_ALLOC(we, sizeof(int64_t) * nwin);
    for(i = 0; i < nwin; i++) { ws[i] = ws_int[i]; we[i] = we_int[i]; }
    free(ws_int); free(we_int);

    if(pli->do_wsplit) { 
      /* split up windows > (pli->wmult * W) into length 2W-1, with W-1 overlapping residues */
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
	if(wlen > (pli->wmult * pli->W)) { 
	  /* split this window */
	  new_ws[i2] = ws[i]; 
	  new_we[i2] = ESL_MIN((new_ws[i2] + (2 * pli->W) - 1), we[i]);
	  while(new_we[i2] < we[i]) { 
	    i2++;
	    if((i2+1) == nalloc) { 
	      nalloc += 100;
	      ESL_RALLOC(new_ws, p, sizeof(int64_t) * nalloc);
	      ESL_RALLOC(new_we,   p, sizeof(int64_t) * nalloc);
	    }
	    new_ws[i2] = ESL_MIN(new_ws[i2-1] + pli->W, we[i]);
	    new_we[i2] = ESL_MIN(new_we[i2-1] + pli->W, we[i]);
	  }	    
	}
	else { /* do not split this window */
	  new_ws[i2] = ws[i]; 
	  new_we[i2] = we[i];
	}
      }
      free(ws);
      free(we);
      ws = new_ws;
      we = new_we;
      nwin = i2;
    }
  }
  else { /* all windows automatically pass MSV, divide up into windows of 2*W, overlapping by W-1 residues */
    nwin = 1; /* first window */
    if(sq->n > (2 * pli->W)) { 
      nwin += (int) (sq->n - (2 * pli->W)) / ((2 * pli->W) - (pli->W - 1));
      /*            (L     -  first window)/(number of unique residues per window) */
      if(((sq->n - (2 * pli->W)) % ((2 * pli->W) - (pli->W - 1))) > 0) { 
	nwin++; /* if the (int) cast in previous line removed any fraction of a window, we add it back here */
      }
    }
    ESL_ALLOC(ws, sizeof(int) * nwin);
    ESL_ALLOC(we, sizeof(int) * nwin);
    for(i = 0; i < nwin; i++) { 
      ws[i] = 1 + (i * (pli->W + 1));
      we[i] = ESL_MIN((ws[i] + (2*pli->W) - 1), sq->n);
      /*printf("window %5d/%5d  %10d..%10d (L=%10" PRId64 ")\n", i+1, nwin, ws[i], we[i], sq->n);*/
    }
  }      
  pli->n_past_msv += nwin;

  if(pli->do_time_F1) return eslOK;

  /*********************************************/
  /* allocate and initialize survAA, which will keep track of number of windows surviving each stage */
  ESL_ALLOC(survAA, sizeof(int *) * Np7_SURV);
  for (i = 0; i < Np7_SURV; i++) { 
    ESL_ALLOC(survAA[i], sizeof(int) * nwin);
    esl_vec_ISet(survAA[i], nwin, FALSE);
  }
    
  for (i = 0; i < nwin; i++) {
#if DOPRINT
    printf("\n\nWindow %5d [%10d..%10d] survived MSV.\n", i, ws[i], we[i]);
#endif
    wlen = we[i] - ws[i] + 1;
    survAA[p7_SURV_F1][i] = TRUE;
    
    subdsq = sq->dsq + ws[i] - 1;
    have_filtersc = have_null3sc = FALSE;
    
    if(pli->do_wcorr) { 
      wcorr = (2 * log(2. / (pli->W+2))) - (2. * log(2. / (wlen+2)));
      p7_bg_SetLength(bg, pli->W);
      p7_bg_NullOne  (bg, subdsq, pli->W, &nullsc);
    }
    else { 
      wcorr = 0.;
      p7_bg_SetLength(bg, wlen);
      p7_bg_NullOne  (bg, subdsq, wlen, &nullsc);
    }

    if (pli->do_msv && pli->do_msvbias) {
      /******************************************************************************/
      /* Filter 1B: Bias filter with p7 HMM */
      /* Have to run msv again, to get the full score for the window.
	 (using the standard "per-sequence" msv filter this time). */
      p7_oprofile_ReconfigMSVLength(om, wlen);
      p7_MSVFilter(subdsq, wlen, om, pli->oxf, &usc);
      if(pli->do_wcorr) p7_bg_FilterScore(bg, subdsq, pli->W,     &filtersc);
      else              p7_bg_FilterScore(bg, subdsq, wlen, &filtersc);
      have_filtersc = TRUE;
      
      wsc = (usc + wcorr - filtersc) / eslCONST_LOG2;
      P   = esl_gumbel_surv(wsc,  pli->p7_evparam[CM_p7_LMMU],  pli->p7_evparam[CM_p7_LMLAMBDA]);

      if (P > pli->F1b) continue;
      /******************************************************************************/
    }
    if (pli->do_msv && pli->do_msvnull3) { 
      p7_oprofile_ReconfigMSVLength(om, wlen);
      p7_MSVFilter(subdsq, wlen, om, pli->oxf, &usc);
      if (! have_null3sc) { 
	ScoreCorrectionNull3CompUnknown(cm->abc, cm->null, subdsq, 1, wlen, pli->p7_n3omega, &null3sc);
#if DOPRINT
	printf("null3sc: %20.18f\n", null3sc);
#endif
	null3sc *= ((float) pli->clen/ (float) wlen); /* a hit is more like clen, not window len */
      }
      have_null3sc = TRUE;
      wsc = ((usc + wcorr - nullsc) / eslCONST_LOG2) - null3sc;
      P   = esl_exp_surv(wsc,  pli->p7_evparam[CM_p7_LMMU],  pli->p7_evparam[CM_p7_LMLAMBDA]);

      if (P > pli->F1n3) continue;
    }
    pli->n_past_msvbias++;
    survAA[p7_SURV_F1b][i] = TRUE;

#if DOPRINT
    printf("Window %5d [%10d..%10d] survived MSV Bias.\n", i, ws[i], we[i]);
#endif      
    
    if(pli->do_msvbias) { /* we already called p7_oprofile_ReconfigMSVLength() above */
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
      wsc = (vfsc + wcorr - nullsc) / eslCONST_LOG2; 
      P   = esl_gumbel_surv(wsc,  pli->p7_evparam[CM_p7_LVMU],  pli->p7_evparam[CM_p7_LVLAMBDA]);
#if DOPRINT
      printf("vit sc: %8.2f P: %g\n", wsc, P);
      printf("IMPT: vfsc:  %8.2f   (log2: %8.2f)  wcorr: %8.2f  (log2: %8.2f)  nullsc: %8.2f  (log2: %8.2f)\n", vfsc, vfsc/eslCONST_LOG2, wcorr, wcorr/eslCONST_LOG2, nullsc, nullsc/eslCONST_LOG2);
#endif
      if (P > pli->F2) continue;
    }
    pli->n_past_vit++;
    survAA[p7_SURV_F2][i] = TRUE;

#if DOPRINT
    printf("Window %5d [%10d..%10d] survived Vit.\n", i, ws[i], we[i]);
#endif

    if(pli->do_time_F2) continue; 

    /********************************************/
    if (pli->do_vit && pli->do_vitbias) { 
      if(! have_filtersc) { 
	if(pli->do_wcorr) p7_bg_FilterScore(bg, subdsq, pli->W,     &filtersc);
	else              p7_bg_FilterScore(bg, subdsq, wlen, &filtersc);
      }
      have_filtersc = TRUE;
      wsc = (vfsc + wcorr - filtersc) / eslCONST_LOG2;
      P = esl_gumbel_surv(wsc,  pli->p7_evparam[CM_p7_LVMU],  pli->p7_evparam[CM_p7_LVLAMBDA]);
#if DOPRINT
      printf("vit-bias sc: %8.2f P: %g\n", wsc, P);
#endif
      if (P > pli->F2b) continue;
      /******************************************************************************/
    }
    if (pli->do_vit && pli->do_vitnull3) { 
      if (! have_null3sc) { 
	ScoreCorrectionNull3CompUnknown(cm->abc, cm->null, subdsq, 1, wlen, pli->p7_n3omega, &null3sc);
	printf("null3sc: %20.18f\n", null3sc);
	null3sc *= ((float) pli->clen/ (float) wlen); /* a hit is more like clen, not window len */
      }
      have_null3sc = TRUE;
      wsc = ((vfsc + wcorr - nullsc) / eslCONST_LOG2) - null3sc;
      P = esl_exp_surv(wsc,  pli->p7_evparam[CM_p7_LVMU],  pli->p7_evparam[CM_p7_LVLAMBDA]);

      if (P > pli->F1n3) continue;
    }
    pli->n_past_vitbias++;
    survAA[p7_SURV_F2b][i] = TRUE;

#if DOPRINT
    printf("Window %5d [%10d..%10d] survived Vit-Bias.\n", i, ws[i], we[i]);
#endif

    /********************************************/

    if(pli->do_fwd) { 
      /******************************************************************************/
      /* Filter 3: Forward with p7 HMM */
      /* Parse it with Forward and obtain its real Forward score. */
      p7_ForwardParser(subdsq, wlen, om, pli->oxf, &fwdsc);
      wsc = (fwdsc + wcorr - nullsc) / eslCONST_LOG2; 
      P = esl_exp_surv(wsc,  pli->p7_evparam[CM_p7_LFTAU],  pli->p7_evparam[CM_p7_LFLAMBDA]);
#if DOPRINT
      printf("fwd sc: %8.2f P: %g\n", wsc, P);
      printf("IMPT: fwdsc:  %8.2f (log2: %8.2f)  wcorr:  %8.2f  (log2: %8.2f)  nullsc: %8.2f  (log2: %8.2f)\n", fwdsc, fwdsc/eslCONST_LOG2, wcorr, wcorr/eslCONST_LOG2, nullsc, nullsc/eslCONST_LOG2);
#endif
      if (P > pli->F3) continue;
    }
    /******************************************************************************/
    pli->n_past_fwd++;
    survAA[p7_SURV_F3][i] = TRUE;

#if DOPRINT
    printf("Window %5d [%10d..%10d] survived Fwd (sc: %6.2f bits;  P: %g).\n", i, ws[i], we[i], wsc, P);
#endif

    if(pli->do_time_F3) continue;

    if (pli->do_fwd && pli->do_fwdbias) { 
      if (! have_filtersc) { 
	if(pli->do_wcorr) p7_bg_FilterScore(bg, subdsq, pli->W,     &filtersc);
	else              p7_bg_FilterScore(bg, subdsq, wlen, &filtersc);
      }
      have_filtersc = TRUE;
      wsc = (fwdsc + wcorr - filtersc) / eslCONST_LOG2;
      P = esl_exp_surv(wsc,  pli->p7_evparam[CM_p7_LFTAU],  pli->p7_evparam[CM_p7_LFLAMBDA]);
      if (P > pli->F3b) continue;
      /******************************************************************************/
    }

    if (pli->do_fwd && pli->do_fwdnull3) { 
      if (! have_null3sc) { 
	ScoreCorrectionNull3CompUnknown(cm->abc, cm->null, subdsq, 1, wlen, pli->p7_n3omega, &null3sc);
	printf("null3sc: %20.18f\n", null3sc);
	null3sc *= ((float) pli->clen/ (float) wlen); /* a hit is more like clen, not window len */
      }
      have_null3sc = TRUE;
      wsc = ((fwdsc + wcorr - nullsc) / eslCONST_LOG2) - null3sc;
      P = esl_exp_surv(wsc,  pli->p7_evparam[CM_p7_LFTAU],  pli->p7_evparam[CM_p7_LFLAMBDA]);
#if DOPRINT
      printf("Window %5d [%10d..%10d] Fwd null3 sc: %10.8f bits (sc: %6.2f bits;  P: %g).\n", i, ws[i], we[i], null3sc, wsc, P);
#endif
      if (P > pli->F3n3) continue;
      /******************************************************************************/
    }
    pli->n_past_fwdbias++;
    nsurv_fwd++;
    /* Replaced with survAA: pli->pos_past_fwdbias += wlen;*/
    survAA[p7_SURV_F3b][i] = TRUE;

#if DOPRINT
    printf("Window %5d [%10d..%10d] survived Fwd-Bias.\n", i, ws[i], we[i]);
#endif
  }

  /* Go back through all windows, and tally up total number of residues
   * that survived each stage, without double-counting overlapping residues.
   * Note, based on way windows were split, we know that any overlapping 
   * residues must occur in adjacent windows and we exploit that here. 
   */
  for(i = 0; i < nwin; i++) {
    wlen = we[i] - ws[i] + 1;
    
    if(survAA[p7_SURV_F1][i])  pli->pos_past_msv     += wlen; 
    if(survAA[p7_SURV_F1b][i]) pli->pos_past_msvbias += wlen; 
    if(survAA[p7_SURV_F2][i])  pli->pos_past_vit     += wlen; 
    if(survAA[p7_SURV_F2b][i]) pli->pos_past_vitbias += wlen; 
    if(survAA[p7_SURV_F3][i])  pli->pos_past_fwd     += wlen; 
    if(survAA[p7_SURV_F3b][i]) pli->pos_past_fwdbias += wlen; 

    /* now subtract residues we've double counted */
    if(i > 0) { 
      overlap = we[i-1] - ws[i] + 1;
      if(overlap > 0) { 
	if(survAA[p7_SURV_F1][i]  && survAA[p7_SURV_F1][i-1])  pli->pos_past_msv     -= overlap;
	if(survAA[p7_SURV_F1b][i] && survAA[p7_SURV_F1b][i-1]) pli->pos_past_msvbias -= overlap;
	if(survAA[p7_SURV_F2][i]  && survAA[p7_SURV_F2][i-1])  pli->pos_past_vit     -= overlap;
	if(survAA[p7_SURV_F2b][i] && survAA[p7_SURV_F2b][i-1]) pli->pos_past_vitbias -= overlap;
	if(survAA[p7_SURV_F3][i]  && survAA[p7_SURV_F3][i-1])  pli->pos_past_fwd     -= overlap;
	if(survAA[p7_SURV_F3b][i] && survAA[p7_SURV_F3b][i-1]) pli->pos_past_fwdbias -= overlap;
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
	/*printf("window %5d  %10d..%10d\n", i2+1, new_ws[i2], new_we[i2]);*/
	i2++;
      }
    }
    if(pli->do_wsplit || (! pli->do_msv)) { 
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
    }
    free(ws);
    free(we);
    ws = new_ws;
    we = new_we;
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


/* Function:  cm_pli_p7EnvelopeDef()
 * Synopsis:  Envelope definition of hits surviving Forward, prior to passing to CYK.
 * Incept:    EPN, Wed Nov 24 13:18:54 2010
 *
 * Purpose:   For each window x, from <ws[x]>..<we[x]>, determine
 *            the envelope boundaries for any hits within it using 
 *            a glocally configured multihit p7 profile. If the P-value
 *            of the envelope is sufficiently low, remove it (i.e. 
 *            boundary def acts as a filter too).. 
 *
 *            In a normal pipeline run, this function call should be
 *            just after a call to cm_pipeline_p7Filter() and just
 *            before a call to cm_pipeline_CMStage().
 *
 * Returns:   <eslOK on success. For all <ret_nenv> envelopes not
 *            filtered out, return their envelope boundaries in
 *            <ret_es> and <ret_ee>.
 *
 * Throws:    <eslEMEM> on allocation failure.
 */
int
cm_pli_p7EnvelopeDef(CM_PIPELINE *pli, CM_t *cm, P7_OPROFILE *om, P7_PROFILE *gm, P7_BG *bg, const ESL_SQ *sq, int64_t *ws, int64_t *we, int nwin, int64_t **ret_es, int64_t **ret_ee, int *ret_nenv)
{
  int              status;                     
  float            env_filtersc;               /* HMM null filter score                   */
  float            env_null3sc;                /* null3 scores                   */
  double           P;                          /* P-value of a hit */
  int              d, i;                       /* counters */
  void            *p;                          /* for ESL_RALLOC */
  int              ali_len, env_len, env_wlen; /* lengths of alignment, envelope, envelope window */
  float            env_sc, env_nullsc;         /* envelope bit score, and domain null1 score */
  float            env_sc_for_pvalue;          /* corrected score for enveloped, used for calc'ing P value */
  int64_t          wlen;             /* window length of current window */
  int64_t         *es;               /* [0..nenv-1] envelope start positions */
  int64_t         *ee;               /* [0..nenv-1] envelope end   positions */
  int              nenv;             /* number of surviving envelopes */
  int              nenv_alloc;       /* current size of es, ee */
  ESL_DSQ         *subdsq;           /* a ptr to the first position of a window */
  ESL_SQ          *seq = NULL;       /* a copy of a window */
  int64_t          estart, eend;     /* envelope start/end positions */

  if (sq->n == 0) return eslOK;    /* silently skip length 0 seqs; they'd cause us all sorts of weird problems */
  if (nwin == 0)  return eslOK;    /* if there's no windows to define envelopes in, return */

  p7_omx_GrowTo(pli->oxf, om->M, 0, sq->n);    /* expand the one-row omx if needed */

  nenv_alloc = nwin;
  ESL_ALLOC(es, sizeof(int64_t) * nenv_alloc);
  ESL_ALLOC(ee, sizeof(int64_t) * nenv_alloc);
  nenv = 0;
  seq = esl_sq_CreateDigital(sq->abc);

  printf("p7ED: sq: %s\nsq->n: %" PRId64 " cm->W:  %d  pli->W: %d\n", sq->name, sq->n, cm->W, pli->W);

  for (i = 0; i < nwin; i++) {
#if DOPRINT
    printf("\n\nWindow %5d/%5d [%10d..%10d] in cm_pipeline_p7EnvelopeDef()\n", i, nwin, ws[i], we[i]);
#endif
    wlen = we[i] - ws[i] + 1;
    subdsq = sq->dsq + ws[i] - 1;

    /* set up seq object for domaindef function */
    esl_sq_GrowTo(seq, wlen);
    memcpy((void*)(seq->dsq), subdsq, (wlen+1) * sizeof(uint8_t)); 
    seq->dsq[0] = seq->dsq[wlen+1] = eslDSQ_SENTINEL;
    seq->n = wlen;

    if(pli->do_localdoms) { /* we can use optimized matrices and, consequently, p7_domaindef_ByPosteriorHeuristics */
      p7_oprofile_ReconfigLength(om, wlen);
      p7_ForwardParser(seq->dsq, wlen, om, pli->oxf, NULL);
      p7_omx_GrowTo(pli->oxb, om->M, 0, wlen);
      p7_BackwardParser(seq->dsq, wlen, om, pli->oxf, pli->oxb, NULL);
      status = p7_domaindef_ByPosteriorHeuristics (seq, om, pli->oxf, pli->oxb, pli->fwd,  pli->bck,  pli->ddef); 
    }
    else { /* we're defining domains in glocal mode, so we need to fill 
	    * generic fwd/bck matrices and pass them to p7_domaindef_GlocalByPosteriorHeuristics() */
      p7_ReconfigLength(gm, wlen);
      p7_gmx_GrowTo(pli->gxf, gm->M, wlen);
      p7_GForward (seq->dsq, wlen, gm, pli->gxf, NULL);
      p7_gmx_GrowTo(pli->gxb, gm->M, wlen);
      p7_GBackward(seq->dsq, wlen, gm, pli->gxb, NULL);
      status = p7_domaindef_GlocalByPosteriorHeuristics(seq, gm, pli->gxf, pli->gxb, pli->gfwd, pli->gbck, pli->ddef); 
    }

    if (status != eslOK) ESL_FAIL(status, pli->errbuf, "domain definition workflow failure"); /* eslERANGE can happen */
    if (pli->ddef->nregions   == 0)  continue; /* score passed threshold but there's no discrete domains here       */
    if (pli->ddef->nenvelopes == 0)  continue; /* rarer: region was found, stochastic clustered, no envelopes found */

    /* For each domain found in the p7_domaindef_*() function, determine if it passes our criteria */
    for(d = 0; d < pli->ddef->ndom; d++) { 
      /* TODO: EPN, Sun Nov 28 13:16:39 2010 revisit this correction, understand it, and write comments explaining it */

      /***************************************************/
      /* Modify the bit score using Travis' corrections originally from p7_pipeline.c::p7_Pipeline_LongTarget() 
       * (these corrections are also included below for hmmonly hit reporting) */
      env_len = pli->ddef->dcl[d].jenv - pli->ddef->dcl[d].ienv +1;
      ali_len = pli->ddef->dcl[d].jali - pli->ddef->dcl[d].iali +1;
      env_wlen = ESL_MIN(pli->W, wlen); //see notes, ~/notebook/20100716_hmmer_score_v_eval_bug/, end of Thu Jul 22 13:36:49 EDT 2010
      env_sc  = pli->ddef->dcl[d].envsc;
      /* For these modifications, see notes, ~/notebook/20100716_hmmer_score_v_eval_bug/, end of Thu Jul 22 13:36:49 EDT 2010 */
      env_sc -= 2 * log(2. / (wlen+2)) +   (env_len-ali_len) * log((float) wlen / (wlen+2));
      env_sc += 2 * log(2. / (env_wlen+2)) ;
      /* handle extremely rare case that the env_len is actually larger than om->max_length */
      /* (I don't think its very rare for dcmsearch b/c MSV P value threshold is so high) */
      env_sc +=  (ESL_MAX(env_wlen, env_len) - ali_len) * log((float) env_wlen / (float) (env_wlen+2));

      env_nullsc = (float) env_wlen * log((float)env_wlen/(env_wlen+1)) + log(1./(env_wlen+1));
      env_sc_for_pvalue = (env_sc - env_nullsc) / eslCONST_LOG2;
      if(pli->do_localdoms) P = esl_exp_surv (env_sc_for_pvalue,  pli->p7_evparam[CM_p7_LFTAU], pli->p7_evparam[CM_p7_LFLAMBDA]);
      else                  P = esl_exp_surv (env_sc_for_pvalue,  pli->p7_evparam[CM_p7_GFMU],  pli->p7_evparam[CM_p7_GFLAMBDA]);
      /***************************************************/

      /* check if we can skip this domain based on its P-value or bit-score */
#if DOPRINT
	printf("\t\tdomain %5d  env bits: %6.2f P: %g\n", d+1, env_sc_for_pvalue, P);
#endif
      if(((! pli->use_dtF3) && (P                 > pli->dF3)) || 
	 ((  pli->use_dtF3) && (env_sc_for_pvalue < pli->dtF3))) {
	continue;
      }
	
      /* Define envelope to search with CM:
       *
       * if (! pli->do_pad): just pass existing envelope
       * from the p7DomainDef() function.
       *
       * if (pli->do_pad):
       * 
       *                domain/hit
       *   ----------xxxxxxxxxxxxxxxx---------
       *   |<-----------------------|
       *                W
       *             |---------------------->|
       *                         W
       *
       * On rare occasions, the envelope size may exceed W, in that case we
       * define dstarts-W-1..dend+W-1 as the envelope.
       *
       */
      if(! pli->do_pad) { 
	estart = pli->ddef->dcl[d].ienv;
	eend   = pli->ddef->dcl[d].jenv;
      }
      else { /* pli->do_pad is TRUE */
	if(env_len <= pli->W) { /* envelope size is less than or equal to W */
	  estart = ESL_MAX(1,    pli->ddef->dcl[d].jenv - (pli->W-1));
	  eend   = ESL_MIN(wlen, pli->ddef->dcl[d].ienv + (pli->W-1));
	}
	else { /* envelope size > W, pad W-1 residues to each side */
	  estart = ESL_MAX(1,    pli->ddef->dcl[d].ienv - (int) ((pli->W - 1)));
	  eend   = ESL_MIN(wlen, pli->ddef->dcl[d].jenv + (int) ((pli->W - 1)));
	}
      }
#if DOPRINT
      printf("Domain [%10d..%10d] survived dF3.\n", pli->ddef->dcl[d].ienv + ws[i] - 1, pli->ddef->dcl[d].jenv + ws[i] - 1);
#endif
      pli->n_past_ddef++;
      pli->pos_past_ddef += env_len;

      if(pli->do_time_dF3) { continue; }

      /* if we're doing a bias filter on domains - check if we skip domain due to that */
      if(pli->do_dombias) {
	p7_bg_FilterScore(bg, seq->dsq + pli->ddef->dcl[d].ienv - 1, env_len, &env_filtersc);
	env_sc_for_pvalue = (env_sc - env_filtersc) / eslCONST_LOG2;
	if(pli->do_localdoms) P = esl_exp_surv (env_sc_for_pvalue,  pli->p7_evparam[CM_p7_LFTAU], pli->p7_evparam[CM_p7_LFLAMBDA]);
	else                  P = esl_exp_surv (env_sc_for_pvalue,  pli->p7_evparam[CM_p7_GFMU],  pli->p7_evparam[CM_p7_GFLAMBDA]);
#if DOPRINT
	printf("\t\tdomain %5d  env bits w/bias : %6.2f P: %g\n", d+1, env_sc_for_pvalue, P);
#endif
	if (P > pli->dF3b) { continue; }
      }
#if DOPRINT
      printf("Domain [%10d..%10d] survived dF3b.\n", pli->ddef->dcl[d].ienv + ws[i] - 1, pli->ddef->dcl[d].jenv + ws[i] - 1);
#endif
	
      /* if we're doing a null3 bias filter on domains - check if we skip domain due to that */
      if(pli->do_domnull3) {
	ScoreCorrectionNull3CompUnknown(cm->abc, cm->null, seq->dsq + pli->ddef->dcl[d].ienv - 1, 1, env_len, pli->p7_n3omega, &env_null3sc);
	printf("dom null3sc: %20.18f\n", env_null3sc);
	env_sc_for_pvalue = ((env_sc - env_nullsc) / eslCONST_LOG2) - env_null3sc;
	if(pli->do_localdoms) P = esl_exp_surv (env_sc_for_pvalue,  pli->p7_evparam[CM_p7_LFTAU], pli->p7_evparam[CM_p7_LFLAMBDA]);
	else                  P = esl_exp_surv (env_sc_for_pvalue,  pli->p7_evparam[CM_p7_GFMU],  pli->p7_evparam[CM_p7_GFLAMBDA]);
#if DOPRINT
	printf("\t\tdomain %5d  env bits w/null3: %6.2f P: %g\n", d+1, env_sc_for_pvalue, P);
#endif
	if (P > pli->dF3n3) { continue; }
      }
#if DOPRINT
      printf("Domain [%10d..%10d] survived dF3n3.\n", pli->ddef->dcl[d].ienv + ws[i] - 1, pli->ddef->dcl[d].jenv + ws[i] - 1);
#endif

      pli->n_past_dombias++;
      pli->pos_past_dombias += env_len;
	
      /* if we get here, the 'domain' has survived, add it to the growing list */
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

  if(seq != NULL) esl_sq_Destroy(seq);

  *ret_es = es;
  *ret_ee = ee;
  *ret_nenv = nenv;

  return eslOK;

 ERROR:
  ESL_EXCEPTION(eslEMEM, "Error: out of memory");
}


/* Function:  cm_pli_CMStage()
 * Synopsis:  Final stage of pipeline, CYK filter, then final scoring with Inside.
 * Incept:    EPN, Sun Nov 28 13:47:31 2010
 *
 * Purpose:   For each envelope x, from <es[x]>..<ee[x]>, run 
 *            CYK to see if any hits above threshold exist, and
 *            if so pass to Inside for final hit definition.
 *
 *            In a normal pipeline run, this function call should be
 *            just after a call to cm_pipeline_p7EnvelopeDef().
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
cm_pli_CMStage(CM_PIPELINE *pli, CM_t *cm, P7_OPROFILE *om, P7_PROFILE *gm, P7_BG *bg, const ESL_SQ *sq, int64_t *es, int64_t *ee, int nenv, P7_TOPHITS *hitlist)
{
  int              status;
  char             errbuf[cmERRBUFSIZE];  /* for error messages */
  P7_HIT          *hit     = NULL;        /* ptr to the current hit output data   */
  float            cyksc, inssc, finalsc; /* bit scores                           */
  int              have_hmmbands;         /* TRUE if HMM bands have been calc'ed for current hit */
  double           P;                     /* P-value of a hit */
  double           E;                     /* E-value of a hit */
  int              i, h;                  /* counters */
  search_results_t *results;              /* results data structure CM search functions report hits to */
  int              do_hbanded_filter_scan, do_hbanded_final_scan; 
  int              do_qdb_or_nonbanded_filter_scan, do_qdb_or_nonbanded_final_scan;
  int              nhit;                  /* number of hits reported */
  int              env_len;               /* length of an envelope */
  double           save_tau = cm->tau;    /* CM's tau upon entering function */

  if (sq->n == 0) return eslOK;    /* silently skip length 0 seqs; they'd cause us all sorts of weird problems */
  if (nenv == 0)  return eslOK;    /* if there's no envelopes to search in, return */

  results = CreateResults(INIT_RESULTS);
  nhit = 0;

  printf("CMST: sq: %s\nsq->n: %" PRId64 " cm->W:  %d  pli->W: %d\n", sq->name, sq->n, cm->W, pli->W);

  for (i = 0; i < nenv; i++) {
    env_len = ee[i] - es[i] + 1;
    cm->tau = save_tau;
#if DOPRINT
    printf("\nEnvelope %5d [%10d..%10d] being passed to CYK.\n", i, es[i], ee[i]);
#endif

    do_hbanded_filter_scan          = (pli->fcyk_cm_search_opts  & CM_SEARCH_HBANDED) ? TRUE  : FALSE;
    do_qdb_or_nonbanded_filter_scan = (pli->fcyk_cm_search_opts  & CM_SEARCH_HBANDED) ? FALSE : TRUE;
    do_hbanded_final_scan           = (pli->final_cm_search_opts & CM_SEARCH_HBANDED) ? TRUE  : FALSE;
    do_qdb_or_nonbanded_final_scan  = (pli->final_cm_search_opts & CM_SEARCH_HBANDED) ? FALSE : TRUE;
    have_hmmbands = FALSE;

    if(pli->do_cyk) { 
      /******************************************************************************/
      /* Filter 4: CYK with CM */
      cm->search_opts  = pli->fcyk_cm_search_opts;
      cm->tau          = pli->fcyk_tau;
      
      if(do_hbanded_filter_scan) { /* get HMM bands */
	/* put up CM_SEARCH_HBANDED flag temporarily */
	cm->search_opts |= CM_SEARCH_HBANDED;
	if((status = cp9_Seq2Bands(cm, errbuf, cm->cp9_mx, cm->cp9_bmx, cm->cp9_bmx, sq->dsq, es[i], ee[i], cm->cp9b, TRUE, 0)) != eslOK) { 
	  printf("ERROR: %s\n", errbuf); return status; }
	/* reset search opts */
	cm->search_opts  = pli->fcyk_cm_search_opts;
	have_hmmbands = TRUE;

	status = FastCYKScanHB(cm, errbuf, sq->dsq, es[i], ee[i], 
			       0.,            /* minimum score to report, irrelevant */
			       NULL,          /* results to add to, NULL in this case */
			       pli->do_null3, /* do the NULL3 correction? */
			       cm->hbmx,      /* the HMM banded matrix */
			       1024.,         /* upper limit for size of DP matrix, 1 Gb */
			       &cyksc);       /* best score, irrelevant here */
	if     (status == eslERANGE) { do_qdb_or_nonbanded_filter_scan = TRUE; }
	else if(status != eslOK)     { printf("ERROR: %s\n", errbuf); return status; }
      }
      if(do_qdb_or_nonbanded_filter_scan) { /* careful, different from just an 'else', b/c we may have just set this as true if status == eslERANGE */
	/*printf("Running CYK on window %d\n", i);*/
	if((status = FastCYKScan(cm, errbuf, pli->fsmx, sq->dsq, es[i], ee[i],
				 0.,            /* minimum score to report, irrelevant */
				 NULL,          /* results to add to, NULL in this case */
				 pli->do_null3, /* do the NULL3 correction? */
				 NULL,          /* ret_vsc, irrelevant here */
				 &cyksc)) != eslOK) { 
	  printf("ERROR: %s\n", errbuf); return status;  }
      }
      P = esl_exp_surv(cyksc, cm->stats->expAA[pli->fcyk_cm_exp_mode][0]->mu_extrap, cm->stats->expAA[pli->fcyk_cm_exp_mode][0]->lambda);
      E = P * cm->stats->expAA[pli->fcyk_cm_exp_mode][0]->cur_eff_dbsize;
#if DOPRINT
      printf("\t\t\tCYK      %7.2f bits  E: %g  P: %g\n", cyksc, E, P);
#endif
      if ((!pli->use_E4) && (P > pli->F4)) continue;
      if (( pli->use_E4) && (E > pli->E4)) continue;
      /******************************************************************************/
    }	
    pli->n_past_cyk++;
    pli->pos_past_cyk += env_len;

#if DOPRINT
    printf("Envelope %5d [%10d..%10d] survived CYK.\n", i, es[i], ee[i]);
#endif
    if(pli->do_time_F4) continue; 
    
    /******************************************************************************/
    /* Final stage: Inside/CYK with CM, report hits to a search_results_t data structure. */
    cm->search_opts  = pli->final_cm_search_opts;
    cm->tau          = pli->final_tau;
    
    /*******************************************************************
     * Determine if we're doing a HMM banded scan, if so, we may already have HMM bands 
     * if our CYK filter also used them. 
     *******************************************************************/
    if(do_hbanded_final_scan) { /* use HMM bands */
      if(! have_hmmbands || (esl_DCompare(cm->tau, pli->fcyk_tau, 1E-30) != eslOK)) { 
	if((status = cp9_Seq2Bands(cm, errbuf, cm->cp9_mx, cm->cp9_bmx, cm->cp9_bmx, sq->dsq, es[i], ee[i], cm->cp9b, TRUE, 0)) != eslOK) { 
	  printf("ERROR: %s\n", errbuf); return status; }
	have_hmmbands = TRUE;
      }
      if(cm->search_opts & CM_SEARCH_INSIDE) { /* final algorithm is HMM banded Inside */
	status = FastFInsideScanHB(cm, errbuf, sq->dsq, es[i], ee[i], 
				   pli->T,            /* minimum score to report */
				   results,           /* our results data structure that will store hit(s) */
				   pli->do_null3,     /* do the NULL3 correction? */
				   cm->hbmx,          /* the HMM banded matrix */
				   1024.,             /* upper limit for size of DP matrix, 1 Gb */
				   &inssc);           /* best score, irrelevant here */
	/* if status == eslERANGE: HMM banded scan was skipped b/c mx needed to be too large, 
	 * we'll repeat the scan with QDBs or without bands below */
	if     (status == eslERANGE) { do_qdb_or_nonbanded_final_scan = TRUE; }
	else if(status != eslOK)     { printf("ERROR: %s\n", errbuf); return status; }
      }
      else { /* final algorithm is HMM banded CYK */
	/*printf("calling HMM banded CYK scan\n");*/
	status = FastCYKScanHB(cm, errbuf, sq->dsq, es[i], ee[i], 
			       pli->T,            /* minimum score to report */
			       results,           /* our results data structure that will store hit(s) */
			       pli->do_null3,     /* do the NULL3 correction? */
			       cm->hbmx,          /* the HMM banded matrix */
			       1024.,             /* upper limit for size of DP matrix, 1 Gb */
			       &cyksc);            /* best score, irrelevant here */
	/* if status == eslERANGE: HMM banded scan was skipped b/c mx needed to be too large, 
	 * we'll repeat the scan with QDBs or without bands below */
	if     (status == eslERANGE) { do_qdb_or_nonbanded_final_scan = TRUE; }
	else if(status != eslOK)     { printf("ERROR: %s\n", errbuf); return status; }
      }
    }
    if(do_qdb_or_nonbanded_final_scan) { /* careful, different from just an 'else', b/c we may have just set this as true if status == eslERANGE */
      /*******************************************************************
       * Run non-HMM banded (probably qdb) version of CYK or Inside *
       *******************************************************************/
      if(cm->search_opts & CM_SEARCH_INSIDE) { /* final algorithm is Inside */
	/*printf("calling non-HMM banded Inside scan\n");*/
	if((status = FastIInsideScan(cm, errbuf, pli->smx, sq->dsq, es[i], ee[i],
				     pli->T,            /* minimum score to report */
				     results,           /* our results data structure that will store hit(s) */
				     pli->do_null3,     /* apply the null3 correction? */
				     NULL,              /* ret_vsc, irrelevant here */
				     &finalsc)) != eslOK) { /* best score, irrelevant here */
	  printf("ERROR: %s\n", errbuf); return status; }
      }
      else { /* final algorithm is CYK */
	/*printf("calling non-HMM banded CYK scan\n");*/
	if((status = FastCYKScan(cm, errbuf, pli->fsmx, sq->dsq, es[i], ee[i],
				 pli->T,            /* minimum score to report */
				 results,           /* our results data structure that will store hit(s) */
				 pli->do_null3,     /* apply the null3 correction? */
				 NULL,              /* ret_vsc, irrelevant here */
				 &finalsc)) != eslOK) { /* best score, irrelevant here */
	  printf("ERROR: %s\n", errbuf); return status; }
      }
    }
    if(pli->do_time_F5) continue;

    /* add each hit to the hitlist */
    for (h = nhit; h < results->num_results; h++) { 
      p7_tophits_CreateNextHit(hitlist, &hit);
      hit->ndom        = 1;
      hit->best_domain = 0;
      /*hit->window_length = ESL_MIN(om->max_length, window_len); //see notes, ~/notebook/20100716_hmmer_score_v_eval_bug/, end of Thu Jul 22 13:36:49 EDT 2010*/
      hit->subseq_start = sq->start;
      
      ESL_ALLOC(hit->dcl, sizeof(P7_DOMAIN));
      
      hit->dcl[0].ienv = hit->dcl[0].iali = results->data[h].start;
      hit->dcl[0].jenv = hit->dcl[0].jali = results->data[h].stop;
      hit->dcl[0].bitscore = hit->dcl[0].envsc = results->data[h].score;
      hit->dcl[0].pvalue   = esl_exp_surv(results->data[h].score, cm->stats->expAA[pli->final_cm_exp_mode][0]->mu_extrap, cm->stats->expAA[pli->final_cm_exp_mode][0]->lambda);
      /* initialize remaining values we don't know yet */
      hit->dcl[0].domcorrection = 0.;
      hit->dcl[0].dombias  = 0.;
      hit->dcl[0].oasc     = 0.;
      hit->dcl[0].is_reported = hit->dcl[0].is_included = FALSE;
      hit->dcl[0].ad = NULL;
	  
      hit->pre_score  = hit->sum_score  = hit->score  = hit->dcl[0].bitscore;
      hit->pre_pvalue = hit->sum_pvalue = hit->pvalue = hit->dcl[0].pvalue;
	  
      if (pli->mode == CM_SEARCH_SEQS) { 
	if (                       (status  = esl_strdup(sq->name, -1, &(hit->name)))  != eslOK) ESL_EXCEPTION(eslEMEM, "allocation failure");
	if (sq->acc[0]  != '\0' && (status  = esl_strdup(sq->acc,  -1, &(hit->acc)))   != eslOK) ESL_EXCEPTION(eslEMEM, "allocation failure");
      } 
      else {
	if ((status  = esl_strdup(cm->name, -1, &(hit->name)))  != eslOK) esl_fatal("allocation failure");
	if ((status  = esl_strdup(cm->acc,  -1, &(hit->acc)))   != eslOK) esl_fatal("allocation failure");
      }
#if DOPRINT
      printf("\t\t\tIns h: %2d  [%7d..%7d]  %7.2f bits  E: %g\n", h+1, hit->dcl[0].ienv, hit->dcl[0].jenv, hit->dcl[0].bitscore, hit->dcl[0].pvalue);
#endif
    }
    nhit = results->num_results;
  }
  FreeResults(results);
  cm->tau = save_tau;
  return eslOK;

 ERROR:
  ESL_EXCEPTION(eslEMEM, "Error allocating memory for hit list in pipeline\n");
}


/* Function:  cm_Pipeline()
 * Synopsis:  The accelerated seq/profile comparison pipeline using HMMER3 scanning 
 * Incept:    EPN, Fri Sep 24 16:42:21 2010
 *            TJW, Fri Feb 26 10:17:53 2018 [Janelia] (p7_Pipeline_Longtargets())
 *
 * Purpose:   Run the accelerated pipeline to compare profile <om>
 *            against sequence <sq>. This function calls three other
 *            functions: cm_pli_p7Filter(), cm_pli_p7EnvelopeDef, and
 *            cm_pli_CMStage().  This organization allows us to
 *            use multiple p7 HMMs as filters.
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
cm_Pipeline(CM_PIPELINE *pli, CM_t *cm, P7_OPROFILE *om, P7_PROFILE *gm, P7_BG *bg, const ESL_SQ *sq, P7_TOPHITS *hitlist)
{
  int status;
  int nwin;            /* number of windows surviving MSV & Vit & Fwd, filled by cm_pli_p7Filter */
  int64_t *ws = NULL;  /* [0..nwin-1] window start positions, filled by cm_pli_p7Filter() */
  int64_t *we = NULL;  /* [0..nwin-1] window end   positions, filled by cm_pli_p7Filter() */
  int nenv;            /* number of envelopes surviving MSV & Vit & Fwd & DomainDef, filled by cm_pli_p7EnvelopeDef */
  int64_t *es = NULL;  /* [0..nenv-1] envelope start positions, filled by cm_pli_p7EnvelopeDef() */
  int64_t *ee = NULL;  /* [0..nenv-1] envelope end   positions, filled by cm_pli_p7EnvelopeDef() */

  if (sq->n == 0) return eslOK;    /* silently skip length 0 seqs; they'd cause us all sorts of weird problems */

  if((status = cm_pli_p7Filter     (pli, cm, om, gm, bg, sq, &ws, &we, &nwin))                           != eslOK) return status;
  if((status = cm_pli_p7EnvelopeDef(pli, cm, om, gm, bg, sq,  ws,  we,  nwin, &es, &ee, &nenv))          != eslOK) return status;
  if((status = cm_pli_CMStage      (pli, cm, om, gm, bg, sq,                   es,  ee,  nenv, hitlist)) != eslOK) return status;

  if(ws != NULL) free(ws);
  if(we != NULL) free(we);
  if(es != NULL) free(ee);
  if(ee != NULL) free(es);

  return eslOK;
}
  

/* Function:  cm_pli_Statistics()
 * Synopsis:  Final statistics output from a processing pipeline.
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
cm_pli_Statistics(FILE *ofp, CM_PIPELINE *pli, ESL_STOPWATCH *w)
{
  double ntargets; 

  fprintf(ofp, "Internal pipeline statistics summary:\n");
  fprintf(ofp, "-------------------------------------\n");
  if (pli->mode == CM_SEARCH_SEQS) {
    fprintf(ofp,   "Query model(s):               %15" PRId64 "  (%" PRId64 " consensus positions)\n",     pli->nmodels, pli->nnodes);
    fprintf(ofp,   "Target sequences:             %15" PRId64 "  (%" PRId64 " residues)\n",  pli->nseqs,   pli->nres);
    ntargets = pli->nseqs;
  } else {
    fprintf(ofp, "Query sequence(s):                    %15" PRId64 "  (%" PRId64 " residues)\n",  pli->nseqs,   pli->nres);
    fprintf(ofp, "Target model(s):                      %15" PRId64 "  (%" PRId64 " nodes)\n",     pli->nmodels, pli->nnodes);
    ntargets = pli->nmodels;
  }

  if(pli->do_msv) { 
    fprintf(ofp, "Windows passing MSV filter:   %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_msv,
	    (double)pli->pos_past_msv / pli->nres ,
	    pli->F1);
  }
  else { 
    fprintf(ofp, "Windows passing MSV filter:   %15s  (off)\n", "");
  }

  if(pli->do_msvbias) { 
    fprintf(ofp, "Windows passing MSV bias fil: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_msvbias,
	    (double)pli->pos_past_msvbias / pli->nres ,
	    pli->F1b);
  }
  else { 
    fprintf(ofp, "Windows passing MSV bias fil: %15s  (off)\n", "");
  }

  if(pli->do_vit) { 
    fprintf(ofp, "Windows passing Vit filter:   %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_vit,
	    (double)pli->pos_past_vit / pli->nres ,
	    pli->F2);
  }
  else { 
    fprintf(ofp, "Windows passing Vit filter:   %15s  (off)\n", "");
  }

  if(pli->do_vitbias) { 
    fprintf(ofp, "Windows passing Vit bias fil: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_vitbias,
	    (double)pli->pos_past_vitbias / pli->nres ,
	    pli->F2b);
  }
  else { 
    fprintf(ofp, "Windows passing Vit bias fil: %15s  (off)\n", "");
  }

  if(pli->do_fwd) { 
    fprintf(ofp, "Windows passing Fwd filter:   %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_fwd,
	    (double)pli->pos_past_fwd / pli->nres ,
	    pli->F3);
  }
  else { 
    fprintf(ofp, "Windows passing Fwd filter:   %15s  (off)\n", "");
  }

  if(pli->do_fwdbias) { 
    fprintf(ofp, "Windows passing Fwd bias fil: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_fwdbias,
	    (double)pli->pos_past_fwdbias / pli->nres ,
	    pli->F3b);
  }
  else { 
    fprintf(ofp, "Windows passing Fwd bias fil: %15s  (off)\n", "");
  }

  if(pli->do_domainize) { 
    fprintf(ofp, "Windows passing domain dfn:   %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_ddef,
	    (double)pli->pos_past_ddef / pli->nres ,
	    pli->dF3);
  }
  else { 
    fprintf(ofp, "Windows passing domain dfn:  %15s  (off)\n", "");
  }

  if(pli->do_dombias) { 
    fprintf(ofp, "Windows passing dom bias fil: %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_dombias,
	    (double)pli->pos_past_dombias / pli->nres ,
	    pli->dF3b);
  }
  else { 
    fprintf(ofp, "Windows passing dom bias fil: %15s  (off)\n", "");
  }

  if(pli->do_cyk) { 
    fprintf(ofp, "Windows passing CYK filter:   %15" PRId64 "  (%.4g); expected (%.4g)\n",
	    pli->n_past_cyk,
	    (double)pli->pos_past_cyk / pli->nres ,
	    pli->F4);
  }
  else { 
    fprintf(ofp, "Windows passing CYK filter:   %15s  (off)\n", "");
  }

  fprintf(ofp, "Total hits:                   %15d  (%.4g)\n",
          (int)pli->n_output,
          (double)pli->pos_output / pli->nres );

  if (w != NULL) {
    esl_stopwatch_Display(ofp, w, "# CPU time: ");
    fprintf(ofp, "# Mc/sec: %.2f\n", 
        (double) pli->nres * (double) pli->nnodes / (w->elapsed * 1.0e6));
  }

  return eslOK;
}
/*------------------- end, pipeline API -------------------------*/


/*****************************************************************
 * 3. Example 1: "search mode" in a sequence db
 *****************************************************************/

#ifdef p7PIPELINE_EXAMPLE
/* gcc -o pipeline_example -g -Wall -I../easel -L../easel -I. -L. -Dp7PIPELINE_EXAMPLE p7_pipeline.c -lhmmer -leasel -lm
 * ./pipeline_example <hmmfile> <sqfile>
 */

#include "p7_config.h"

#include "easel.h"
#include "esl_getopts.h"
#include "esl_sqio.h"

#include "hmmer.h"

static ESL_OPTIONS options[] = {
  /* name           type         default   env  range   toggles   reqs   incomp                             help                                                  docgroup*/
  { "-h",           eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  NULL,                          "show brief help on version and usage",                         0 },
  { "-E",           eslARG_REAL,  "10.0", NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "E-value cutoff for reporting significant sequence hits",       0 },
  { "-T",           eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "bit score cutoff for reporting significant sequence hits",     0 },
  { "-Z",           eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  NULL,                          "set # of comparisons done, for E-value calculation",           0 },
  { "--domE",       eslARG_REAL,"1000.0", NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "E-value cutoff for reporting individual domains",              0 },
  { "--domT",       eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "bit score cutoff for reporting individual domains",            0 },
  { "--domZ",       eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  NULL,                          "set # of significant seqs, for domain E-value calculation",    0 },
  { "--cut_ga",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  "--seqE,--seqT,--domE,--domT", "use GA gathering threshold bit score cutoffs in <hmmfile>",    0 },
  { "--cut_nc",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  "--seqE,--seqT,--domE,--domT", "use NC noise threshold bit score cutoffs in <hmmfile>",        0 },
  { "--cut_tc",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  "--seqE,--seqT,--domE,--domT", "use TC trusted threshold bit score cutoffs in <hmmfile>",      0 },
  { "--max",        eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL, "--F1,--F2,--F3",               "Turn all heuristic filters off (less speed, more power)",      0 },
  { "--F1",         eslARG_REAL,  "0.02", NULL, NULL,      NULL,  NULL, "--max",                        "Stage 1 (MSV) threshold: promote hits w/ P <= F1",             0 },
  { "--F2",         eslARG_REAL,  "1e-3", NULL, NULL,      NULL,  NULL, "--max",                        "Stage 2 (Vit) threshold: promote hits w/ P <= F2",             0 },
  { "--F3",         eslARG_REAL,  "1e-5", NULL, NULL,      NULL,  NULL, "--max",                        "Stage 3 (Fwd) threshold: promote hits w/ P <= F3",             0 },
  { "--nobias",     eslARG_NONE,   NULL,  NULL, NULL,      NULL,  NULL, "--max",                        "turn off composition bias filter",                             0 },
  { "--nonull2",    eslARG_NONE,   NULL,  NULL, NULL,      NULL,  NULL,  NULL,                          "turn off biased composition score corrections",                0 },
  { "--seed",       eslARG_INT,    "42",  NULL, "n>=0",    NULL,  NULL,  NULL,                          "set RNG seed to <n> (if 0: one-time arbitrary seed)",          0 },
  { "--acc",        eslARG_NONE,  FALSE,  NULL, NULL,      NULL,  NULL,  NULL,                          "output target accessions instead of names if possible",        0 },
 {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static char usage[]  = "[-options] <hmmfile> <seqdb>";
static char banner[] = "example of using acceleration pipeline in search mode (seq targets)";

int
main(int argc, char **argv)
{
  ESL_GETOPTS  *go      = esl_getopts_CreateDefaultApp(options, 2, argc, argv, banner, usage);
  char         *hmmfile = esl_opt_GetArg(go, 1);
  char         *seqfile = esl_opt_GetArg(go, 2);
  int           format  = eslSQFILE_FASTA;
  P7_HMMFILE   *hfp     = NULL;
  ESL_ALPHABET *abc     = NULL;
  P7_BG        *bg      = NULL;
  P7_HMM       *hmm     = NULL;
  P7_PROFILE   *gm      = NULL;
  P7_OPROFILE  *om      = NULL;
  ESL_SQFILE   *sqfp    = NULL;
  ESL_SQ       *sq      = NULL;
  CM_PIPELINE  *pli     = NULL;
  P7_TOPHITS   *hitlist = NULL;
  int           h,d,namew;

  /* Don't forget this. Null2 corrections need FLogsum() */
  p7_FLogsumInit();

  /* Read in one HMM */
  if (p7_hmmfile_Open(hmmfile, NULL, &hfp) != eslOK) p7_Fail("Failed to open HMM file %s", hmmfile);
  if (p7_hmmfile_Read(hfp, &abc, &hmm)     != eslOK) p7_Fail("Failed to read HMM");
  p7_hmmfile_Close(hfp);

  /* Open a sequence file */
  if (esl_sqfile_OpenDigital(abc, seqfile, format, NULL, &sqfp) != eslOK) p7_Fail("Failed to open sequence file %s\n", seqfile);
  sq = esl_sq_CreateDigital(abc);

  /* Create a pipeline and a top hits list */
  pli     = p7_pipeline_Create(go, hmm->M, 400, FALSE, p7_SEARCH_SEQS);
  hitlist = p7_tophits_Create();

  /* Configure a profile from the HMM */
  bg = p7_bg_Create(abc);
  gm = p7_profile_Create(hmm->M, abc);
  om = p7_oprofile_Create(hmm->M, abc);
  p7_ProfileConfig(hmm, bg, gm, 400, p7_LOCAL);
  p7_oprofile_Convert(gm, om);     /* <om> is now p7_LOCAL, multihit */
  p7_pli_NewModel(pli, om, bg);

  /* Run each target sequence through the pipeline */
  while (esl_sqio_Read(sqfp, sq) == eslOK)
    { 
      p7_pli_NewSeq(pli, sq);
      p7_bg_SetLength(bg, sq->n);
      p7_oprofile_ReconfigLength(om, sq->n);
  
      p7_Pipeline(pli, om, bg, sq, hitlist);

      esl_sq_Reuse(sq);
      p7_pipeline_Reuse(pli);
    }

  /* Print the results. 
   * This example is a stripped version of hmmsearch's tabular output.
   */
  p7_tophits_Sort(hitlist);
  namew = ESL_MAX(8, p7_tophits_GetMaxNameLength(hitlist));
  for (h = 0; h < hitlist->N; h++)
    {
      d    = hitlist->hit[h]->best_domain;

      printf("%10.2g %7.1f %6.1f  %7.1f %6.1f %10.2g  %6.1f %5d  %-*s %s\n",
         hitlist->hit[h]->pvalue * (double) pli->Z,
         hitlist->hit[h]->score,
         hitlist->hit[h]->pre_score - hitlist->hit[h]->score, /* bias correction */
         hitlist->hit[h]->dcl[d].bitscore,
         eslCONST_LOG2R * p7_FLogsum(0.0, log(bg->omega) + hitlist->hit[h]->dcl[d].domcorrection), /* print in units of bits */
         hitlist->hit[h]->dcl[d].pvalue * (double) pli->Z,
         hitlist->hit[h]->nexpected,
         hitlist->hit[h]->nreported,
         namew,
         hitlist->hit[h]->name,
         hitlist->hit[h]->desc);
    }

  /* Done. */
  p7_tophits_Destroy(hitlist);
  p7_pipeline_Destroy(pli);
  esl_sq_Destroy(sq);
  esl_sqfile_Close(sqfp);
  p7_oprofile_Destroy(om);
  p7_profile_Destroy(gm);
  p7_hmm_Destroy(hmm);
  p7_bg_Destroy(bg);
  esl_alphabet_Destroy(abc);
  esl_getopts_Destroy(go);
  return 0;
}
#endif /*p7PIPELINE_EXAMPLE*/
/*----------- end, search mode (seq db) example -----------------*/




/*****************************************************************
 * 4. Example 2: "scan mode" in an HMM db
 *****************************************************************/
#ifdef p7PIPELINE_EXAMPLE2
/* gcc -o pipeline_example2 -g -Wall -I../easel -L../easel -I. -L. -Dp7PIPELINE_EXAMPLE2 p7_pipeline.c -lhmmer -leasel -lm
 * ./pipeline_example2 <hmmdb> <sqfile>
 */

#include "p7_config.h"

#include "easel.h"
#include "esl_getopts.h"
#include "esl_sqio.h"

#include "hmmer.h"

static ESL_OPTIONS options[] = {
  /* name           type         default   env  range   toggles   reqs   incomp                             help                                                  docgroup*/
  { "-h",           eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  NULL,                          "show brief help on version and usage",                         0 },
  { "-E",           eslARG_REAL,  "10.0", NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "E-value cutoff for reporting significant sequence hits",       0 },
  { "-T",           eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "bit score cutoff for reporting significant sequence hits",     0 },
  { "-Z",           eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  NULL,                          "set # of comparisons done, for E-value calculation",           0 },
  { "--domE",       eslARG_REAL,"1000.0", NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "E-value cutoff for reporting individual domains",              0 },
  { "--domT",       eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  "--cut_ga,--cut_nc,--cut_tc",  "bit score cutoff for reporting individual domains",            0 },
  { "--domZ",       eslARG_REAL,   FALSE, NULL, "x>0",     NULL,  NULL,  NULL,                          "set # of significant seqs, for domain E-value calculation",    0 },
  { "--cut_ga",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  "--seqE,--seqT,--domE,--domT", "use GA gathering threshold bit score cutoffs in <hmmfile>",    0 },
  { "--cut_nc",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  "--seqE,--seqT,--domE,--domT", "use NC noise threshold bit score cutoffs in <hmmfile>",        0 },
  { "--cut_tc",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  "--seqE,--seqT,--domE,--domT", "use TC trusted threshold bit score cutoffs in <hmmfile>",      0 },
  { "--max",        eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL, "--F1,--F2,--F3",               "Turn all heuristic filters off (less speed, more power)",      0 },
  { "--F1",         eslARG_REAL,  "0.02", NULL, NULL,      NULL,  NULL, "--max",                        "Stage 1 (MSV) threshold: promote hits w/ P <= F1",             0 },
  { "--F2",         eslARG_REAL,  "1e-3", NULL, NULL,      NULL,  NULL, "--max",                        "Stage 2 (Vit) threshold: promote hits w/ P <= F2",             0 },
  { "--F3",         eslARG_REAL,  "1e-5", NULL, NULL,      NULL,  NULL, "--max",                        "Stage 3 (Fwd) threshold: promote hits w/ P <= F3",             0 },
  { "--nobias",     eslARG_NONE,   NULL,  NULL, NULL,      NULL,  NULL, "--max",                        "turn off composition bias filter",                             0 },
  { "--nonull2",    eslARG_NONE,   NULL,  NULL, NULL,      NULL,  NULL,  NULL,                          "turn off biased composition score corrections",                0 },
  { "--seed",       eslARG_INT,    "42",  NULL, "n>=0",    NULL,  NULL,  NULL,                          "set RNG seed to <n> (if 0: one-time arbitrary seed)",          0 },
  { "--acc",        eslARG_NONE,  FALSE,  NULL, NULL,      NULL,  NULL,  NULL,                          "output target accessions instead of names if possible",        0 },
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static char usage[]  = "[-options] <hmmfile> <seqfile>";
static char banner[] = "example of using acceleration pipeline in scan mode (HMM targets)";

int
main(int argc, char **argv)
{
  ESL_GETOPTS  *go      = esl_getopts_CreateDefaultApp(options, 2, argc, argv, banner, usage);
  char         *hmmfile = esl_opt_GetArg(go, 1);
  char         *seqfile = esl_opt_GetArg(go, 2);
  int           format  = eslSQFILE_FASTA;
  P7_HMMFILE   *hfp     = NULL;
  ESL_ALPHABET *abc     = NULL;
  P7_BG        *bg      = NULL;
  P7_OPROFILE  *om      = NULL;
  ESL_SQFILE   *sqfp    = NULL;
  ESL_SQ       *sq      = NULL;
  CM_PIPELINE  *pli     = NULL;
  P7_TOPHITS   *hitlist = p7_tophits_Create();
  int           h,d,namew;

  /* Don't forget this. Null2 corrections need FLogsum() */
  p7_FLogsumInit();

  /* Open a sequence file, read one seq from it.
   * Convert to digital later, after 1st HMM is input and abc becomes known 
   */
  sq = esl_sq_Create();
  if (esl_sqfile_Open(seqfile, format, NULL, &sqfp) != eslOK) p7_Fail("Failed to open sequence file %s\n", seqfile);
  if (esl_sqio_Read(sqfp, sq)                       != eslOK) p7_Fail("Failed to read sequence from %s\n", seqfile);
  esl_sqfile_Close(sqfp);

  /* Open the HMM db */
  if (p7_hmmfile_Open(hmmfile, NULL, &hfp) != eslOK) p7_Fail("Failed to open HMM file %s", hmmfile);

  /* Create a pipeline for the query sequence in scan mode */
  pli      = p7_pipeline_Create(go, 100, sq->n, FALSE, p7_SCAN_MODELS);
  p7_pli_NewSeq(pli, sq);
   
  /* Some additional config of the pipeline specific to scan mode */
  pli->hfp = hfp;
  if (! pli->Z_is_fixed && hfp->is_pressed) { pli->Z_is_fixed = TRUE; pli->Z = hfp->ssi->nprimary; }

  /* Read (partial) of each HMM in file */
  while (p7_oprofile_ReadMSV(hfp, &abc, &om) == eslOK) 
    {
      /* One time only initialization after abc becomes known */
      if (bg == NULL) 
    {
      bg = p7_bg_Create(abc);
      if (esl_sq_Digitize(abc, sq) != eslOK) p7_Die("alphabet mismatch");
      p7_bg_SetLength(bg, sq->n);
    }
      p7_pli_NewModel(pli, om, bg);
      p7_oprofile_ReconfigLength(om, sq->n);

      p7_Pipeline(pli, om, bg, sq, hitlist);
      
      p7_oprofile_Destroy(om);
      p7_pipeline_Reuse(pli);
    } 

  /* Print the results. 
   * This example is a stripped version of hmmsearch's tabular output.
   */
  p7_tophits_Sort(hitlist);
  namew = ESL_MAX(8, p7_tophits_GetMaxNameLength(hitlist));
  for (h = 0; h < hitlist->N; h++)
    {
      d    = hitlist->hit[h]->best_domain;

      printf("%10.2g %7.1f %6.1f  %7.1f %6.1f %10.2g  %6.1f %5d  %-*s %s\n",
         hitlist->hit[h]->pvalue * (double) pli->Z,
         hitlist->hit[h]->score,
         hitlist->hit[h]->pre_score - hitlist->hit[h]->score, /* bias correction */
         hitlist->hit[h]->dcl[d].bitscore,
         eslCONST_LOG2R * p7_FLogsum(0.0, log(bg->omega) + hitlist->hit[h]->dcl[d].domcorrection), /* print in units of BITS */
         hitlist->hit[h]->dcl[d].pvalue * (double) pli->Z,
         hitlist->hit[h]->nexpected,
         hitlist->hit[h]->nreported,
         namew,
         hitlist->hit[h]->name,
         hitlist->hit[h]->desc);
    }

  /* Done. */
  p7_tophits_Destroy(hitlist);
  p7_pipeline_Destroy(pli);
  esl_sq_Destroy(sq);
  p7_hmmfile_Close(hfp);
  p7_bg_Destroy(bg);
  esl_alphabet_Destroy(abc);
  esl_getopts_Destroy(go);
  return 0;
}
#endif /*p7PIPELINE_EXAMPLE2*/
/*--------------- end, scan mode (HMM db) example ---------------*/



/*****************************************************************
 * @LICENSE@
 *****************************************************************/
