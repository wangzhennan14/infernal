/* cm_dpsearch_trun.c
 *
 * DP functions for truncated CYK and Inside CM similarity search,
 * includes fast (optimized) and reference versions.
 * 
 * EPN, Tue Aug 16 04:15:32 2011
 *****************************************************************
 * @LICENSE@
 *****************************************************************  
 */

#include "esl_config.h"
#include "p7_config.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include "easel.h"
#include "esl_sqio.h"
#include "esl_stack.h"
#include "esl_vectorops.h"

#include "funcs.h"
#include "structs.h"


/* Function: RefTrCYKScan()
 * Date:     EPN, Tue Aug 16 04:16:03 2011
 *
 * Purpose:  Scan a sequence for matches to a covariance model, using
 *           a reference trCYK scanning algorithm. Query-dependent 
 *           bands are used or not used as specified in ScanMatrix_t <si>.
 *
 *           This function is slower, but easier to understand than the
 *           FastTrCYKScan() version.
 *
 * Args:     cm              - the covariance model
 *           errbuf          - char buffer for reporting errors
 *           smx             - ScanMatrix_t for this search w/this model (incl. DP matrix, qdbands etc.) 
 *           dsq             - the digitized sequence
 *           i0              - start of target subsequence (1 for full seq)
 *           j0              - end of target subsequence (L for full seq)
 *           cutoff          - minimum score to report
 *           results         - search_results_t to add to; if NULL, don't add to it
 *           do_null3        - TRUE to do NULL3 score correction, FALSE not to
 *           env_cutoff      - ret_envi..ret_envj will include all hits that exceed this bit sc
 *           ret_envi        - min position in any hit w/sc >= env_cutoff, set to -1 if no such hits exist, NULL if not wanted
 *           ret_envj        - max position in any hit w/sc >= env_cutoff, set to -1 if no such hits exist, NULL if not wanted 
 *           ret_vsc         - RETURN: [0..v..M-1] best score at each state v, NULL if not-wanted
 *           ret_sc          - RETURN: score of best overall hit (vsc[0])
 *
 * Note:     This function is heavily synchronized with RefIInsideScan() and RefCYKScan()
 *           any change to this function should be mirrored in those functions. 
 *
 * Returns:  eslOK on succes;
 *           <ret_sc> is score of best overall hit (vsc[0]). Information on hits added to <results>.
 *           <ret_vsc> is filled with an array of the best hit to each state v (if non-NULL).
 *           Dies immediately if some error occurs.
 */
int
RefTrCYKScan(CM_t *cm, char *errbuf, TrScanMatrix_t *trsmx, ESL_DSQ *dsq, int i0, int j0, float cutoff, search_results_t *results, 
	     int do_null3, float env_cutoff, int64_t *ret_envi, int64_t *ret_envj, float **ret_vsc, float *ret_sc)
{
  int       status;
  GammaHitMx_t *gamma;       /* semi-HMM for hit resoultion */
  float    *vsc;                /* best score for each state (float) */
  float     vsc_root;           /* best overall score (score at ROOT_S) */
  int       yoffset;		/* offset to a child state */
  int       i,j;		/* index of start/end positions in sequence, 0..L */
  int       d;			/* a subsequence length, 0..W */
  int       k;			/* used in bifurc calculations: length of right subseq */
  int       prv, cur;		/* previous, current j row (0 or 1) */
  int       v, w, y;            /* state indices */
  int       jp_v;  	        /* offset j for state v */
  int       jp_y;  	        /* offset j for state y */
  int       jq_y;  	        /* offset j for state y plus 1 (if jp_y is prv, jq_y is cur, and vice versa) */
  int       jp_g;               /* offset j for gamma (j-i0+1) */
  int       kmin, kmax;         /* for B_st's, min/max value consistent with bands*/
  int       L;                  /* length of the subsequence (j0-i0+1) */
  int       W;                  /* max d; max size of a hit, this is min(L, smx->W) */
  int       sd;                 /* StateDelta(cm->sttype[v]), # emissions from v */
  int       do_banded = FALSE;  /* TRUE: use QDBs, FALSE: don't   */
  int      *dnA, *dxA;          /* tmp ptr to 1 row of dnAA, dxAA */
  int       kn, kx;             /* minimum/maximum valid k for current d in B_st recursion */
  int       cnum;               /* number of children for current state */
  int      *jp_wA;              /* rolling pointer index for B states, gets precalc'ed */
  float   **init_scAA;          /* [0..v..cm->M-1][0..d..W] initial score for each v, d for all j */
  double  **act;                /* [0..j..W-1][0..a..abc->K-1], alphabet count, count of residue a in dsq from 1..jp where j = jp%(W+1) */
  int       do_env_defn;        /* TRUE to calculate envi, envj, FALSE not to (TRUE if ret_envi != NULL or ret_envj != NULL */
  int64_t   envi, envj;         /* min/max positions that exist in any hit with sc >= env_cutoff */

  /* Contract check */
  if(! cm->flags & CMH_BITS)               ESL_FAIL(eslEINCOMPAT, errbuf, "RefCYKScan, CMH_BITS flag is not raised.\n");
  if(j0 < i0)                              ESL_FAIL(eslEINCOMPAT, errbuf, "RefCYKScan, i0: %d j0: %d\n", i0, j0);
  if(dsq == NULL)                          ESL_FAIL(eslEINCOMPAT, errbuf, "RefCYKScan, dsq is NULL\n");
  if(trsmx == NULL)                        ESL_FAIL(eslEINCOMPAT, errbuf, "RefCYKScan, trsmx == NULL\n");
  if(cm->search_opts & CM_SEARCH_INSIDE)   ESL_FAIL(eslEINCOMPAT, errbuf, "RefCYKScan, CM_SEARCH_INSIDE flag raised");
  if(! (trsmx->flags & cmTRSMX_HAS_FLOAT)) ESL_FAIL(eslEINCOMPAT, errbuf, "RefCYKScan, ScanMatrix's cmTRSMX_HAS_FLOAT flag is not raised");

  /* make pointers to the ScanMatrix/CM data for convenience */
  float ***Jalpha      = trsmx->fJalpha;      /* [0..j..1][0..v..cm->M-1][0..d..W] Jalpha DP matrix, NULL for v == BEGL_S */
  float ***Jalpha_begl = trsmx->fJalpha_begl; /* [0..j..W][0..v..cm->M-1][0..d..W] Jalpha DP matrix, NULL for v != BEGL_S */
  float ***Lalpha      = trsmx->fLalpha;      /* [0..j..1][0..v..cm->M-1][0..d..W] Lalpha DP matrix, NULL for v == BEGL_S */
  float ***Lalpha_begl = trsmx->fLalpha_begl; /* [0..j..W][0..v..cm->M-1][0..d..W] Lalpha DP matrix, NULL for v != BEGL_S */
  float ***Ralpha      = trsmx->fRalpha;      /* [0..j..1][0..v..cm->M-1][0..d..W] Ralpha DP matrix, NULL for v == BEGL_S */
  float ***Ralpha_begl = trsmx->fRalpha_begl; /* [0..j..W][0..v..cm->M-1][0..d..W] Ralpha DP matrix, NULL for v != BEGL_S */
  float ***Talpha      = trsmx->fTalpha;      /* [0..j..1][0..v..cm->M-1][0..d..W] Talpha DP matrix, NULL for v != BIF_B  */
  int   **dnAA         = trsmx->dnAA;         /* [0..v..cm->M-1][0..j..W] minimum d for v, j (for j > W use [v][W]) */
  int   **dxAA         = trsmx->dxAA;         /* [0..v..cm->M-1][0..j..W] maximum d for v, j (for j > W use [v][W]) */
  int    *bestr        = trsmx->bestr;        /* [0..d..W] best root state (for local begins or 0) for this d */
  int    *dmax         = trsmx->dmax;         /* [0..v..cm->M-1] maximum d allowed for this state */
  float **esc_vAA      = cm->oesc;            /* [0..v..cm->M-1][0..a..(cm->abc->Kp | cm->abc->Kp**2)] optimized emission scores for v 
 					       * and all possible emissions a (including ambiguities) */
  float **lmesc_vAA    = cm->lmesc;           /* [0..v..cm->M-1][0..a..(cm->abc->Kp-1)] left  marginal emission scores for v */
  float **rmesc_vAA    = cm->rmesc;           /* [0..v..cm->M-1][0..a..(cm->abc->Kp-1)] right marginal emission scores for v */

  /* determine if we're doing banded/non-banded */
  if(trsmx->dmax != NULL) do_banded = TRUE;
  
  L = j0-i0+1;
  W = trsmx->W;
  if (W > L) W = L; 

  /* set vsc array */
  vsc = NULL;
  ESL_ALLOC(vsc, sizeof(float) * cm->M);
  esl_vec_FSet(vsc, cm->M, IMPOSSIBLE);
  vsc_root = IMPOSSIBLE;

  /* gamma allocation and initialization.
   * This is a little SHMM that finds an optimal scoring parse
   * of multiple nonoverlapping hits. */
  if(results != NULL) gamma = CreateGammaHitMx(L, i0, (cm->search_opts & CM_SEARCH_CMGREEDY), cutoff, FALSE);
  else                gamma = NULL;

  /* allocate array for precalc'ed rolling ptrs into BEGL deck, filled inside 'for(j...' loop */
  ESL_ALLOC(jp_wA, sizeof(float) * (W+1));

  /* precalculate the initial scores for all cells */
  init_scAA = FCalcInitDPScores(cm);

  /* if do_null3: allocate and initialize act vector */
  if(do_null3) { 
    ESL_ALLOC(act, sizeof(double *) * (W+1));
    for(i = 0; i <= W; i++) { 
      ESL_ALLOC(act[i], sizeof(double) * cm->abc->K);
      esl_vec_DSet(act[i], cm->abc->K, 0.);
    }
  }
  else act = NULL;

  /* initialize envelope boundary variables */
  do_env_defn = (ret_envi != NULL || ret_envj != NULL) ? TRUE : FALSE;
  envi = j0+1;
  envj = i0-1;

  /* The main loop: scan the sequence from position i0 to j0.
   */
  for (j = i0; j <= j0; j++) 
    {
      float Jsc, Lsc, Rsc, Tsc;
      jp_g = j-i0+1; /* j is actual index in dsq, jp_g is offset j relative to start i0 (index in gamma* data structures) */
      cur  = j%2;
      prv  = (j-1)%2;
      if(jp_g >= W) { dnA = dnAA[W];     dxA = dxAA[W];    }
      else          { dnA = dnAA[jp_g];  dxA = dxAA[jp_g]; }
      /* precalcuate all possible rolling ptrs into the BEGL deck, so we don't wastefully recalc them inside inner DP loop */
      for(d = 0; d <= W; d++) jp_wA[d] = (j-d)%(W+1);

      /* if do_null3 (act != NULL), update act */
      if(act != NULL) { 
	esl_vec_DCopy(act[(jp_g-1)%(W+1)], cm->abc->K, act[jp_g%(W+1)]);
	esl_abc_DCount(cm->abc, act[jp_g%(W+1)], dsq[j], 1.);
	/*printf("j: %3d jp_g: %3d jp_g/W: %3d act[0]: %.3f act[1]: %.3f act[2]: %.3f act[3]: %.3f\n", j, jp_g, jp_g%(W+1), act[jp_g%(W+1)][0], act[jp_g%(W+1)][1], act[jp_g%(W+1)][2], act[jp_g%(W+1)][3]);*/
      }

      for (v = cm->M-1; v > 0; v--) /* ...almost to ROOT; we handle ROOT specially... */
	{
	  /* printf("dnA[v:%d]: %d\ndxA[v:%d]: %d\n", v, dnA[v], v, dxA[v]); */
	  if(cm->sttype[v] == E_st) continue;
	  float const *esc_v = esc_vAA[v]; 
	  float const *tsc_v = cm->tsc[v];
	  float const *lmesc_v = lmesc_vAA[v]; 
	  float const *rmesc_v = rmesc_vAA[v]; 
	  int emitmode = Emitmode(cm->sttype[v]);

	  /* float sc; */
	  jp_v  = (cm->stid[v] == BEGL_S) ? (j % (W+1)) : cur;
	  jp_y  = (StateRightDelta(cm->sttype[v]) > 0) ? prv : cur;
	  jq_y = (StateRightDelta(cm->sttype[v]) > 0) ? cur : prv;
	  sd    = StateDelta(cm->sttype[v]);
	  cnum  = cm->cnum[v];
	  /* if we emit right, precalc score of emitting res j from state v */
	  float   esc_j = IMPOSSIBLE;
	  float rmesc_j = IMPOSSIBLE;
	  if(cm->sttype[v] == IR_st || cm->sttype[v] == MR_st) { 
	    esc_j   =   esc_v[dsq[j]];
	    rmesc_j = rmesc_v[dsq[j]];
	  }
	  if(cm->sttype[v] == MP_st) { 
	    rmesc_j = rmesc_v[dsq[j]];
	  }

	  if(cm->sttype[v] == B_st) {
	    w = cm->cfirst[v]; /* BEGL_S */
	    y = cm->cnum[v];   /* BEGR_S */
	    for (d = dnA[v]; d <= dxA[v]; d++) {
	      /* k is the length of the right fragment */
	      /* Careful, make sure k is consistent with bands in state w and state y. */
	      if(do_banded) {
		kmin = ESL_MAX(0, (d-dmax[w]));
		kmin = ESL_MAX(kmin, 0);
		kmax = ESL_MIN(dmax[y], d);
	      }
	      else { kmin = 0; kmax = d; }

	      Jsc = init_scAA[v][d-sd]; /* state delta (sd) is 0 for B_st */
	      Lsc = IMPOSSIBLE;
	      Rsc = IMPOSSIBLE;
	      Tsc = IMPOSSIBLE;

	      /* Careful with Tsc, it isn't updated for k == 0 or  k == d, 
	       * but Jsc, Lsc, Rsc, are all updated for k == 0 and k == d */
	      for (k = kmin; k <= kmax; k++) {
		Jsc = ESL_MAX(Jsc, (Jalpha_begl[jp_wA[k]][w][d-k] + Jalpha[jp_y][y][k]));
		Lsc = ESL_MAX(Lsc, (Jalpha_begl[jp_wA[k]][w][d-k] + Lalpha[jp_y][y][k]));
		Rsc = ESL_MAX(Rsc, (Ralpha_begl[jp_wA[k]][w][d-k] + Jalpha[jp_y][y][k]));
	      }		
	      kn = ESL_MAX(1,   kmin);
	      kx = ESL_MIN(d-1, kmax);
	      for (k = kn; k <= kx; k++) {
		Tsc = ESL_MAX(Tsc, (Ralpha_begl[jp_wA[k]][w][d-k] + Lalpha[jp_y][y][k]));
	      }

	      Jalpha[jp_v][v][d] = Jsc;
	      Talpha[jp_v][v][d] = Tsc;
	      if(kmin == 0) { 
		Lalpha[jp_v][v][d] = ESL_MAX(Lsc, ESL_MAX(Jalpha_begl[jp_wA[0]][w][d], Lalpha_begl[jp_wA[0]][w][d])); 
		Ralpha[jp_v][v][d] = ESL_MAX(Rsc, ESL_MAX(Jalpha[jp_y][y][d], Ralpha[jp_y][y][d]));
	      }
	      else { 
		Lalpha[jp_v][v][d] = Lsc;
		Ralpha[jp_v][v][d] = Rsc;
	      }

	      /* careful: scores for w, the BEGL_S child of v, are in alpha_begl, not alpha */
	    }
	  }
	  else if (emitmode == EMITLEFT) {
	    y = cm->cfirst[v]; 
	    i = j - dnA[v] + 1;
	    assert(dnA[v] == 1);
	    for (d = dnA[v]; d <= dxA[v]; d++) {
	      Jsc = init_scAA[v][d-sd]; 
	      Lsc = IMPOSSIBLE;
	      Rsc = IMPOSSIBLE;
	      Ralpha[jp_y][v][d] = Rsc; /* this is important b/c if we're an IL, we'll access this cell in the recursion below for Ralpha */

	      /* We need to do separate 'for (yoffset...' loops for J
	       * and R matrices, because jp_v == jp_y for all states
	       * here, and for IL states, v can equal y+yoffset (when
	       * yoffset==0).  This means we have to fully calculate
	       * the Jalpha[jp_v][y+yoffset][d] cell (which is
	       * Jalpha[jp_v][v][d]) before we can start to calculate
	       * Ralpha[jp_v][v][d]. 
	       */
	      for (yoffset = 0; yoffset < cm->cnum[v]; yoffset++) {
		Jsc = ESL_MAX(Jsc,         Jalpha[jp_y][y+yoffset][d - sd] + tsc_v[yoffset]);
		Lsc = ESL_MAX(Lsc,         Lalpha[jp_y][y+yoffset][d - sd] + tsc_v[yoffset]);
	      }
	      Jalpha[jp_v][v][d] = Jsc + esc_v[dsq[i]];
	      Lalpha[jp_v][v][d] = (d >= 2) ? Lsc + esc_v[dsq[i]] : esc_v[dsq[i]];
	      
	      for (yoffset = 0; yoffset < cm->cnum[v]; yoffset++) {
		Rsc = ESL_MAX(Rsc, ESL_MAX(Jalpha[jp_y][y+yoffset][d]      + tsc_v[yoffset],
					   Ralpha[jp_y][y+yoffset][d]      + tsc_v[yoffset]));
	      }
	      Ralpha[jp_v][v][d] = Rsc;
	      i--;
	    }
	  }
	  else if (emitmode == EMITRIGHT) { 
	    y = cm->cfirst[v]; 
	    assert(dnA[v] == 1);
	    for (d = dnA[v]; d <= dxA[v]; d++) {
	      Jsc = init_scAA[v][d-sd]; 
	      Lsc = IMPOSSIBLE;
	      Rsc = IMPOSSIBLE;
	      Lalpha[jp_y][v][d] = Lsc; /* this is important b/c if we're an IR, we'll access this cell in the recursion below for Lalpha */
	      
	      /* We need to do separate 'for (yoffset...' loops for J
	       * and L matrices, because jp_v == jq_y for all states
	       * here, and for IR states, v can equal y+yoffset (when
	       * yoffset==0).  This means we have to fully calculate
	       * the Jalpha[jq_y][y+yoffset][d] cell (which is
	       * Jalpha[jp_v][v][d]) before we can start to calculate
	       * Lalpha[jp_v][v][d]. 
	       */
	      for (yoffset = 0; yoffset < cm->cnum[v]; yoffset++) { 
		Jsc = ESL_MAX(Jsc,         Jalpha[jp_y][y+yoffset][d - sd] + tsc_v[yoffset]);
		Rsc = ESL_MAX(Rsc,         Ralpha[jp_y][y+yoffset][d - sd] + tsc_v[yoffset]);
	      }
	      Jalpha[jp_v][v][d] = Jsc + esc_j;
	      Ralpha[jp_v][v][d] = (d >= 2) ? Rsc + esc_j : esc_j;

	      for (yoffset = 0; yoffset < cm->cnum[v]; yoffset++) { 
		Lsc = ESL_MAX(Lsc, ESL_MAX(Jalpha[jq_y][y+yoffset][d]     + tsc_v[yoffset],
					   Lalpha[jq_y][y+yoffset][d]     + tsc_v[yoffset]));
	      }
	      Lalpha[jp_v][v][d] = Lsc;
	    }
	  }
	  else if (emitmode == EMITPAIR) { 
	    y = cm->cfirst[v]; 
	    i = j - dnA[v] + 1;
	    assert(dnA[v] == 1);
	    for (d = dnA[v]; d <= dxA[v]; d++) {
	      Jsc = init_scAA[v][d-sd]; 
	      Lsc = IMPOSSIBLE;
	      Rsc = IMPOSSIBLE;
	      for (yoffset = 0; yoffset < cm->cnum[v]; yoffset++) { 
		Jsc = ESL_MAX(Jsc,         Jalpha[jp_y][y+yoffset][d - 2] + tsc_v[yoffset]);
		Lsc = ESL_MAX(Lsc, ESL_MAX(Jalpha[jq_y][y+yoffset][d - 1] + tsc_v[yoffset],
					   Lalpha[jq_y][y+yoffset][d - 1] + tsc_v[yoffset])),
		Rsc = ESL_MAX(Rsc, ESL_MAX(Jalpha[jp_y][y+yoffset][d - 1] + tsc_v[yoffset],
					   Ralpha[jp_y][y+yoffset][d - 1] + tsc_v[yoffset]));
	      }
	      Jalpha[jp_v][v][d] = (d >= 2) ? Jsc + esc_v[dsq[i]*cm->abc->Kp+dsq[j]] : IMPOSSIBLE;
	      Lalpha[jp_v][v][d] = (d >= 2) ? Lsc + lmesc_v[dsq[i]]                  : lmesc_v[dsq[i]];
	      Ralpha[jp_v][v][d] = (d >= 2) ? Rsc + rmesc_j                          : rmesc_j;
	      i--;
	    }
	  }
	  else if (cm->stid[v] == BEGL_S) {
	    y = cm->cfirst[v]; 
	    for (d = dnA[v]; d <= dxA[v]; d++) {
	      Jsc = init_scAA[v][d-sd]; /* state delta (sd) is 0 for BEGL_S st */
	      Lsc = IMPOSSIBLE;
	      Rsc = IMPOSSIBLE;
	      for (yoffset = 0; yoffset < cm->cnum[v]; yoffset++) { 
		Jsc = ESL_MAX(Jsc, Jalpha[jp_y][y+yoffset][d - sd] + tsc_v[yoffset]);
		Lsc = ESL_MAX(Lsc, Lalpha[jp_y][y+yoffset][d - sd] + tsc_v[yoffset]);
		Rsc = ESL_MAX(Rsc, Ralpha[jp_y][y+yoffset][d - sd] + tsc_v[yoffset]);
	      }
	      Jalpha_begl[jp_v][v][d] = Jsc;
	      Lalpha_begl[jp_v][v][d] = Lsc;
	      Ralpha_begl[jp_v][v][d] = Rsc;
	      /* careful: y is in alpha (all children of a BEGL_S must be non BEGL_S) */
	    }
	  }
	  else { /* ! B_st && ! BEGL_S st && ! L_st && ! R_st && ! P_st (emitmode == EMITNONE) */
	    y = cm->cfirst[v]; 
	    for (d = dnA[v]; d <= dxA[v]; d++) {
	      Jsc = init_scAA[v][d-sd]; 
	      Lsc = IMPOSSIBLE;
	      Rsc = IMPOSSIBLE;
	      for (yoffset = 0; yoffset < cm->cnum[v]; yoffset++) { 
		Jsc = ESL_MAX(Jsc, Jalpha[jp_y][y+yoffset][d - sd] + tsc_v[yoffset]);
		Lsc = ESL_MAX(Lsc, Lalpha[jp_y][y+yoffset][d - sd] + tsc_v[yoffset]);
		Rsc = ESL_MAX(Rsc, Ralpha[jp_y][y+yoffset][d - sd] + tsc_v[yoffset]);
	      }
	      Jalpha[jp_v][v][d] = Jsc;
	      Lalpha[jp_v][v][d] = Lsc;
	      Ralpha[jp_v][v][d] = Rsc;
	    }
	  }
	  if(vsc != NULL) {
	    if(cm->stid[v] == BIF_B) { 
	      for (d = dnA[v]; d <= dxA[v]; d++) { 
		vsc[v] = ESL_MAX(vsc[v], 
				 ESL_MAX(Jalpha[jp_v][v][d], 
					 ESL_MAX(Lalpha[jp_v][v][d], 
						 ESL_MAX(Ralpha[jp_v][v][d], Talpha[jp_v][v][d]))));
	      }
	    }
	    else if(cm->stid[v] == BEGL_S) { 
	      for (d = dnA[v]; d <= dxA[v]; d++) { 
		vsc[v] = ESL_MAX(vsc[v], 
				 ESL_MAX(Jalpha_begl[jp_v][v][d], 
					 ESL_MAX(Lalpha_begl[jp_v][v][d], Ralpha_begl[jp_v][v][d])));
	      }
	    }
	    else {
	      for (d = dnA[v]; d <= dxA[v]; d++) { 
		vsc[v] = ESL_MAX(vsc[v], 
				 ESL_MAX(Jalpha[jp_v][v][d], 
					 ESL_MAX(Lalpha[jp_v][v][d], Ralpha[jp_v][v][d])));
	      }
	    }
	  }
#if 0
	  if(cm->stid[v] == BIF_B) { 
	    for(d = dnA[v]; d <= dxA[v]; d++) { 
	      printf("M j: %3d  v: %3d  d: %3d\n", j, v, d);
	      printf("M j: %3d  v: %3d  d: %3d   J: %10.4f  L: %10.4f  R: %10.4f  T: %10.4f\n", 
		     j, v, d, 
		     NOT_IMPOSSIBLE(Jalpha[jp_v][v][d]) ? Jalpha[jp_v][v][d] : -9999.9,
		     NOT_IMPOSSIBLE(Lalpha[jp_v][v][d]) ? Lalpha[jp_v][v][d] : -9999.9,
		     NOT_IMPOSSIBLE(Ralpha[jp_v][v][d]) ? Ralpha[jp_v][v][d] : -9999.9,
		     NOT_IMPOSSIBLE(Talpha[jp_v][v][d]) ? Talpha[jp_v][v][d] : -9999.9);
	    }
	  }
	  else if(cm->stid[v] == BEGL_S) { 
	    for(d = dnA[v]; d <= dxA[v]; d++) { 
	      printf("M j: %3d  v: %3d  d: %3d\n", j, v, d);
	      printf("M j: %3d  v: %3d  d: %3d   J: %10.4f  L: %10.4f  R: %10.4f  T: %10.4f\n", 
		     j, v, d, 
		     NOT_IMPOSSIBLE(Jalpha_begl[jp_v][v][d]) ? Jalpha_begl[jp_v][v][d] : -9999.9,
		     NOT_IMPOSSIBLE(Lalpha_begl[jp_v][v][d]) ? Lalpha_begl[jp_v][v][d] : -9999.9,
		     NOT_IMPOSSIBLE(Ralpha_begl[jp_v][v][d]) ? Ralpha_begl[jp_v][v][d] : -9999.9, 
		     -9999.9);
	    }
	  }
	  else {
	    for(d = dnA[v]; d <= dxA[v]; d++) { 
	      printf("M j: %3d  v: %3d  d: %3d\n", j, v, d);
	      printf("M j: %3d  v: %3d  d: %3d   J: %10.4f  L: %10.4f  R: %10.4f  T: %10.4f\n", 
		     j, v, d, 
		     NOT_IMPOSSIBLE(Jalpha[jp_v][v][d]) ? Jalpha[jp_v][v][d] : -9999.9,
		     NOT_IMPOSSIBLE(Lalpha[jp_v][v][d]) ? Lalpha[jp_v][v][d] : -9999.9,
		     NOT_IMPOSSIBLE(Ralpha[jp_v][v][d]) ? Ralpha[jp_v][v][d] : -9999.9,
		     -9999.9);
	    }
	  }
	  printf("\n");
#endif
	}  /*loop over decks v>0 */
      
      /* Finish up with the ROOT_S, state v=0; and deal w/ local begins.
       * 
       * If local begins are off, the hit must be rooted at v=0.
       * With local begins on, the hit is rooted at the second state in
       * the traceback (e.g. after 0), the internal entry point. Divide & conquer
       * can only handle this if it's a non-insert state; this is guaranteed
       * by the way local alignment is parameterized (other transitions are
       * -INFTY), which is probably a little too fragile of a method. 
       */

      float const *tsc_v = cm->tsc[0];
      /* determine min/max d we're allowing for the root state and this position j */
      jp_v = cur;
      for (d = dnA[0]; d <= dxA[0]; d++) {
	bestr[d] = 0;	/* root of the traceback = root state 0 */
	y = cm->cfirst[0];
	Jalpha[jp_v][0][d] = ESL_MAX(IMPOSSIBLE, Jalpha[cur][y][d] + tsc_v[0]);
	Lalpha[jp_v][0][d] = IMPOSSIBLE;
	Ralpha[jp_v][0][d] = IMPOSSIBLE;
	for (yoffset = 1; yoffset < cm->cnum[0]; yoffset++) {
	  Jalpha[jp_v][0][d] = ESL_MAX (Jalpha[jp_v][0][d], (Jalpha[cur][y+yoffset][d] + tsc_v[yoffset]));
	  Lalpha[jp_v][0][d] = ESL_MAX (Lalpha[jp_v][0][d], (Lalpha[cur][y+yoffset][d] + tsc_v[yoffset]));
	  Ralpha[jp_v][0][d] = ESL_MAX (Ralpha[jp_v][0][d], (Ralpha[cur][y+yoffset][d] + tsc_v[yoffset]));
	}
      }
	
      if (cm->flags & CMH_LOCAL_BEGIN) {
	for (y = 1; y < cm->M; y++) {
	  if(NOT_IMPOSSIBLE(cm->beginsc[y])) {
	    if(cm->stid[y] == BEGL_S)
	      {
		jp_y = j % (W+1);
		for (d = dnA[y]; d <= dxA[y]; d++) {
		  /* Is this more efficient:? 
		     bestr[d]          = (alpha[jp_v][0][d] > (alpha_begl[jp_y][y][d] + cm->beginsc[y])) ? bestr[d] : y;
		     alpha[jp_v][0][d] = ESL_MAX(alpha[jp_v][0][d], alpha_begl[jp_y][y][d] + cm->beginsc[y]); */
		  if(Jalpha[jp_v][0][d] < (Jalpha_begl[jp_y][y][d] + cm->beginsc[y])) {
		    Jalpha[jp_v][0][d] = Jalpha_begl[jp_y][y][d] + cm->beginsc[y];
		    bestr[d] = y;
		  }
		}
	      }
	    else { /* y != BEGL_S */
	      jp_y = cur;
	      for (d = dnA[y]; d <= dxA[y]; d++) {
		{
		  /* Is this more efficient:? 
		     bestr[d]          = (alpha[jp_v][0][d] > (alpha[jp_y][y][d] + cm->beginsc[y])) ? bestr[d] : y;
		     alpha[jp_v][0][d] = ESL_MAX(alpha[jp_v][0][d], alpha[jp_y][y][d] + cm->beginsc[y]); */
		  if(Jalpha[jp_v][0][d] < (Jalpha[jp_y][y][d] + cm->beginsc[y])) {
		    Jalpha[jp_v][0][d] = Jalpha[jp_y][y][d] + cm->beginsc[y];
		    bestr[d] = y;
		  }
		}
	      }
	    }
	  }
	}
      }
      /* find the best score in J */
      for (d = dnA[0]; d <= dxA[0]; d++) 
	vsc_root = ESL_MAX(vsc_root, Jalpha[jp_v][0][d]);

      /* update envi, envj, if nec */
      if(do_env_defn) { 
	for (d = dnA[0]; d <= dxA[0]; d++) {
	  if(Jalpha[jp_v][0][d] >= env_cutoff) { 
	    envi = ESL_MIN(envi, j-d+1);
	    envj = ESL_MAX(envj, j);
	  }
	}
      }

      /* update gamma, but only if we're reporting hits to results */
      if(results != NULL) if((status = UpdateGammaHitMxCM(cm, errbuf, gamma, jp_g, Jalpha[jp_v][0], dnA[0], dxA[0], FALSE, trsmx->bestr, results, W, act)) != eslOK) return status;

      /* cm_DumpScanMatrixAlpha(cm, si, j, i0, TRUE); */
    } /* end loop over end positions j */
  if(vsc != NULL) vsc[0] = vsc_root;

  /* find the best score in any matrix at any state */
  float best_tr_sc = IMPOSSIBLE;
  for(v = 0; v < cm->M; v++) { 
    best_tr_sc = ESL_MAX(best_tr_sc, vsc[v]);
  }
  printf("Best truncated score: %.4f (%.4f)\n",
	 best_tr_sc, 
	 best_tr_sc + sreLOG2(2./(cm->clen * (cm->clen+1))));

  /* If recovering hits in a non-greedy manner, do the traceback.
   * If we were greedy, they were reported in UpdateGammaHitMxCM() for each position j */
  if(results != NULL && gamma->iamgreedy == FALSE) 
    TBackGammaHitMxForward(gamma, results, i0, j0);

  /* set envelope return variables if nec */
  if(ret_envi != NULL) { *ret_envi = (envi == j0+1) ? -1 : envi; }
  if(ret_envj != NULL) { *ret_envj = (envj == i0-1) ? -1 : envj; }

  /* clean up and return */
  if(gamma != NULL) FreeGammaHitMx(gamma);
  if (act != NULL) { 
    for(i = 0; i <= W; i++) free(act[i]); 
    free(act);
  }
  free(jp_wA);
  free(init_scAA[0]);
  free(init_scAA);
  if (ret_vsc != NULL) *ret_vsc = vsc;
  else free(vsc);
  if (ret_sc != NULL) *ret_sc = vsc_root;

  ESL_DPRINTF1(("RefTrCYKScan() return score: %10.4f\n", vsc_root)); 
  return eslOK;
  
 ERROR:
  ESL_FAIL(eslEMEM, errbuf, "Memory allocation error.\n");
  return 0.; /* NEVERREACHED */
}

/*****************************************************************
 *   1. TrScanMatrix_t data structure functions,
 *      auxiliary info and matrix of float and/or int scores for 
 *      truncated query dependent banded or non-banded CM DP search 
 *      functions.
 *****************************************************************/

/* Function: cm_CreateTrScanMatrix()
 * Date:     EPN, Tue Aug 16 04:23:41 2011
 *
 * Purpose:  Given relevant info, allocate and initialize
 *           TrScanMatrix_t object.  Note that unlike a ScanMatrix_t,
 *           dmin is not used to set minimum values, even if we're
 *           going to use QDBs, because minimum subtree lengths are
 *           illogical with the truncated version of CYK/Inside, but
 *           maximum lengths are not, so <dmax> is considered here.
 *            
 * Returns:  eslOK on success, dies immediately on some error
 */
TrScanMatrix_t *
cm_CreateTrScanMatrix(CM_t *cm, int W, int *dmax, double beta_W, double beta_qdb, int do_banded, int do_float, int do_int)
{ 
  int status;
  TrScanMatrix_t *trsmx;
  int v,j;

  if((!do_float) && (!do_int)) cm_Fail("cm_CreateScanMatrix(), do_float and do_int both FALSE.\n");
  if(do_banded && dmax == NULL) cm_Fail("cm_CreateScanMatrix(), do_banded is TRUE, but dmax is NULL.\n");

  ESL_ALLOC(trsmx, sizeof(TrScanMatrix_t));

  trsmx->flags    = 0;
  trsmx->cm_M     = cm->M;
  trsmx->W        = W;
  trsmx->dmax     = dmax; /* could be NULL */
  trsmx->beta_W   = beta_W; 
  trsmx->beta_qdb = beta_qdb; 

  /* precalculate minimum and maximum d for each state and each sequence index (1..j..W). 
   * this is not always just 0, dmax, (for ex. if j < W). */
  ESL_ALLOC(trsmx->dnAA, sizeof(int *) * (trsmx->W+1));
  ESL_ALLOC(trsmx->dxAA, sizeof(int *) * (trsmx->W+1));
  trsmx->dnAA[0] = trsmx->dxAA[0] = NULL; /* corresponds to j == 0, which is out of bounds */
  for(j = 1; j <= trsmx->W; j++) {
    ESL_ALLOC(trsmx->dnAA[j], sizeof(int) * cm->M);
    ESL_ALLOC(trsmx->dxAA[j], sizeof(int) * cm->M);
    for(v = 0; v < cm->M; v++) {
      /* dnAA[j][v] is 1 for all states, even MATP, b/c d == 1 is valid for MATP in L,R matrices */
      trsmx->dnAA[j][v] = 1;
      if(do_banded) { 
	trsmx->dxAA[j][v] = ESL_MIN(j, trsmx->dmax[v]); 
	trsmx->dxAA[j][v] = ESL_MIN(trsmx->dxAA[j][v], trsmx->W);
      }
      else { 
	trsmx->dxAA[j][v] = ESL_MIN(j, trsmx->W); 
      }
    }
  }
  /* allocate bestr, which holds best root state at alpha[0][cur][d] */
  ESL_ALLOC(trsmx->bestr, (sizeof(int) * (trsmx->W+1)));

  /* Some info about the falpha/ialpha matrix
   * The alpha matrix holds data for all states EXCEPT BEGL_S states
   * The alpha scanning matrix is indexed [j][v][d]. 
   *    j takes values 0 or 1: only the previous (prv) or current (cur) row
   *    v ranges from 0..M-1 over states in the model.
   *    d ranges from 0..W over subsequence lengths.
   * Note if v is a BEGL_S alpha[j][v] == NULL
   * Note that old convention of sharing E memory is no longer,
   * each E state has it's own deck.
   *
   * alpha_begl matrix holds data for ONLY BEGL_S states
   *    j takes value of 0..W
   *    v ranges from 0..M-1 over states in the model
   *    d ranges from 0..W over subsequence lengths.
   * Note if v is NOT a BEGL_S alpha_begl[j][v] == NULL
   *
   * alpha and alpha_begl are allocated in contiguous blocks
   * of memory in {f,i}alpha_mem and {f,i}alpha_begl_mem
   */

  /* Some info on alpha initialization 
   * We initialize on d=0, subsequences of length 0; these are
   * j-independent. Any generating state (P,L,R) is impossible on d=0.
   * E=0 for d=0. B,S,D must be calculated. 
   * Also, for MP, d=1 is impossible.
   * Also, for E, all d>0 are impossible.
   *
   * and, for banding: any cell outside our bands is impossible.
   * These inits are never changed in the recursion, so even with the
   * rolling, matrix face reuse strategy, this works.
   *
   * The way we initialize is just to set the entire matrix
   * to -INFTY or IMPOSSIBLE (for ints and floats, respectively),
   * and then reset those cells that should not be -INFTY or
   * IMPOSSIBLE as listed above. This way we don't have to
   * step through the bands, setting cells outside them to IMPOSSIBLE
   * or -INFY;
   */

  trsmx->fJalpha          = trsmx->fLalpha          = trsmx->fRalpha          = trsmx->fTalpha          = NULL;
  trsmx->fJalpha_begl     = trsmx->fLalpha_begl     = trsmx->fRalpha_begl     = NULL;
  trsmx->fJalpha_mem      = trsmx->fLalpha_mem      = trsmx->fRalpha_mem      = trsmx->fTalpha_mem      = NULL;
  trsmx->fJalpha_begl_mem = trsmx->fLalpha_begl_mem = trsmx->fRalpha_begl_mem = NULL;

  trsmx->iJalpha          = trsmx->iLalpha          = trsmx->iRalpha          = trsmx->iTalpha          = NULL;
  trsmx->iJalpha_begl     = trsmx->iLalpha_begl     = trsmx->iRalpha_begl     = NULL;
  trsmx->iJalpha_mem      = trsmx->iLalpha_mem      = trsmx->iRalpha_mem      = trsmx->iTalpha_mem      = NULL;
  trsmx->iJalpha_begl_mem = trsmx->iLalpha_begl_mem = trsmx->iRalpha_begl_mem = NULL;

  trsmx->ncells_alpha      = 0;
  trsmx->ncells_alpha_begl = 0;
  trsmx->ncells_Talpha     = 0;

  if(do_float) /* allocate float mx and scores */
    cm_FloatizeTrScanMatrix(cm, trsmx);
  if(do_int)   /* allocate int mx and scores */
    esl_fatal("cm_IntizeTrScanMatrix() not yet implemented");
    //cm_IntizeTrScanMatrix(cm, trsmx);
  return trsmx;

 ERROR:
  cm_Fail("memory allocation error in cm_CreateTrScanMatrix().\n");
  return NULL; /* NEVERREACHED */
}

/* Function: cm_FloatizeTrScanMatrix()
 * Date:     EPN, Tue Aug 16 04:36:29 2011
 *
 * Purpose: Allocate and initialize float data structures in a
 *           TrScanMatrix_t object for <cm>.  This initializes a
 *           scanning float DP matrix for trCYK/trInside, for details
 *           on that matrix see the notes by the
 *           cm_TrFloatizeScanMatrix() function call in
 *           cm_CreateTrScanMatrix().
 *            
 * Returns:  eslOK on success, dies immediately on an error.
 */
int
cm_FloatizeTrScanMatrix(CM_t *cm, TrScanMatrix_t *trsmx)
{
  int status;
  int j, v;
  int y, yoffset, w;
  int use_hmmonly;
  use_hmmonly = ((cm->search_opts & CM_SEARCH_HMMVITERBI) ||  (cm->search_opts & CM_SEARCH_HMMFORWARD)) ? TRUE : FALSE;
  int n_begl, n_bif;
  int n_non_begl;
  int cur_cell;

  /* contract check */
  if(trsmx->flags & cmTRSMX_HAS_FLOAT) cm_Fail("cm_FloatizeScanMatrix(), trsmx's cmTRSMX_HAS_FLOAT flag is already up.");
  if(trsmx->fJalpha != NULL)       cm_Fail("cm_FloatizeScanMatrix(), trsmx->fJalpha is not NULL.");
  if(trsmx->fJalpha_begl != NULL)  cm_Fail("cm_FloatizeScanMatrix(), trsmx->fJalpha_begl is not NULL.");
  if(trsmx->fLalpha != NULL)       cm_Fail("cm_FloatizeScanMatrix(), trsmx->fLalpha is not NULL.");
  if(trsmx->fLalpha_begl != NULL)  cm_Fail("cm_FloatizeScanMatrix(), trsmx->fLalpha_begl is not NULL.");
  if(trsmx->fRalpha != NULL)       cm_Fail("cm_FloatizeScanMatrix(), trsmx->fRalpha is not NULL.");
  if(trsmx->fRalpha_begl != NULL)  cm_Fail("cm_FloatizeScanMatrix(), trsmx->fRalpha_begl is not NULL.");
  if(trsmx->fTalpha != NULL)       cm_Fail("cm_FloatizeScanMatrix(), trsmx->fTalpha is not NULL.");
  
  /* allocate alpha 
   * we allocate only as many cells as necessary,
   * for f{J,L,R,T}alpha,      we only allocate for non-BEGL_S states,
   * for f{J,L,R,T}alpha_begl, we only allocate for     BEGL_S states
   *
   * note: deck for the EL state, cm->M is never used for scanners
   */
  n_begl = 0;
  n_bif  = 0;
  for (v = 0; v < cm->M; v++) if (cm->stid[v] == BEGL_S) n_begl++;
  for (v = 0; v < cm->M; v++) if (cm->stid[v] == BIF_B)  n_bif++;
  n_non_begl = cm->M - n_begl;

  /* allocate f{J,L,R,T}alpha */
  /* j == 0 v == 0 cells, followed by j == 1 v == 0, then j == 0 v == 1 etc.. */
  ESL_ALLOC(trsmx->fJalpha,        sizeof(float **) * 2);
  ESL_ALLOC(trsmx->fJalpha[0],     sizeof(float *) * (cm->M)); /* we still allocate cm->M ptrs, if v == BEGL_S, fJalpha[0][v] will be NULL */
  ESL_ALLOC(trsmx->fJalpha[1],     sizeof(float *) * (cm->M)); /* we still allocate cm->M ptrs, if v == BEGL_S, fJalpha[1][v] will be NULL */
  ESL_ALLOC(trsmx->fJalpha_mem,    sizeof(float) * 2 * n_non_begl * (trsmx->W+1));

  ESL_ALLOC(trsmx->fLalpha,        sizeof(float **) * 2);
  ESL_ALLOC(trsmx->fLalpha[0],     sizeof(float *) * (cm->M)); /* we still allocate cm->M ptrs, if v == BEGL_S, fLalpha[0][v] will be NULL */
  ESL_ALLOC(trsmx->fLalpha[1],     sizeof(float *) * (cm->M)); /* we still allocate cm->M ptrs, if v == BEGL_S, fLalpha[1][v] will be NULL */
  ESL_ALLOC(trsmx->fLalpha_mem,    sizeof(float) * 2 * n_non_begl * (trsmx->W+1));

  ESL_ALLOC(trsmx->fRalpha,        sizeof(float **) * 2);
  ESL_ALLOC(trsmx->fRalpha[0],     sizeof(float *) * (cm->M)); /* we still allocate cm->M ptrs, if v == BEGL_S, fRalpha[0][v] will be NULL */
  ESL_ALLOC(trsmx->fRalpha[1],     sizeof(float *) * (cm->M)); /* we still allocate cm->M ptrs, if v == BEGL_S, fRalpha[1][v] will be NULL */
  ESL_ALLOC(trsmx->fRalpha_mem,    sizeof(float) * 2 * n_non_begl * (trsmx->W+1));

  ESL_ALLOC(trsmx->fTalpha,        sizeof(float **) * 2);
  ESL_ALLOC(trsmx->fTalpha[0],     sizeof(float *) * (cm->M)); /* we still allocate cm->M ptrs, if v != BIF_B, fTalpha[0][v] will be NULL */
  ESL_ALLOC(trsmx->fTalpha[1],     sizeof(float *) * (cm->M)); /* we still allocate cm->M ptrs, if v != BIF_B, fTalpha[1][v] will be NULL */
  ESL_ALLOC(trsmx->fTalpha_mem,    sizeof(float) * 2 * n_non_begl * (trsmx->W+1));

  if((trsmx->flags & cmTRSMX_HAS_INT) && ((2 * n_non_begl * (trsmx->W+1)) != trsmx->ncells_alpha)) 
    cm_Fail("cm_FloatizeScanMatrix(), cmTRSMX_HAS_INT flag raised, but trsmx->ncells_alpha %d != %d (predicted num float cells size)\n", trsmx->ncells_alpha, (2 * n_non_begl * (trsmx->W+1)));
  trsmx->ncells_alpha = 2 * n_non_begl * (trsmx->W+1);

  cur_cell = 0;
  for (v = 0; v < cm->M; v++) {	
    if (cm->stid[v] != BEGL_S) {
      trsmx->fJalpha[0][v] = trsmx->fJalpha_mem + cur_cell;
      trsmx->fLalpha[0][v] = trsmx->fLalpha_mem + cur_cell;
      trsmx->fRalpha[0][v] = trsmx->fRalpha_mem + cur_cell;
      cur_cell += trsmx->W+1;
      trsmx->fJalpha[1][v] = trsmx->fJalpha_mem + cur_cell;
      trsmx->fLalpha[1][v] = trsmx->fLalpha_mem + cur_cell;
      trsmx->fRalpha[1][v] = trsmx->fRalpha_mem + cur_cell;
      cur_cell += trsmx->W+1;
    }
    else { 
      trsmx->fJalpha[0][v] = NULL;
      trsmx->fJalpha[1][v] = NULL;
      trsmx->fLalpha[0][v] = NULL;
      trsmx->fLalpha[1][v] = NULL;
      trsmx->fRalpha[0][v] = NULL;
      trsmx->fRalpha[1][v] = NULL;
    }
  }
  if(cur_cell != trsmx->ncells_alpha) cm_Fail("cm_FloatizeScanMatrix(), error allocating falpha, cell cts differ %d != %d\n", cur_cell, trsmx->ncells_alpha);

  if((trsmx->flags & cmTRSMX_HAS_INT) && ((2 * n_bif * (trsmx->W+1)) != trsmx->ncells_Talpha)) 
    cm_Fail("cm_FloatizeScanMatrix(), cmTRSMX_HAS_INT flag raised, but trsmx->ncells_Talpha %d != %d (predicted num float cells size in Talpha)\n", trsmx->ncells_Talpha, (2 * n_bif * (trsmx->W+1)));
  trsmx->ncells_Talpha = 2 * n_bif * (trsmx->W+1);

  cur_cell = 0;
  for (v = 0; v < cm->M; v++) {	
    if (cm->stid[v] == BIF_B) { 
      trsmx->fTalpha[0][v] = trsmx->fTalpha_mem + cur_cell;
      cur_cell += trsmx->W+1;
      trsmx->fTalpha[1][v] = trsmx->fTalpha_mem + cur_cell;
      cur_cell += trsmx->W+1;
    }
    else { 
      trsmx->fTalpha[0][v] = NULL;
      trsmx->fTalpha[1][v] = NULL;
    }
  }
  if(cur_cell != trsmx->ncells_Talpha) cm_Fail("cm_FloatizeScanMatrix(), error allocating fTalpha, cell cts differ %d != %d\n", cur_cell, trsmx->ncells_Talpha);
  

  /* allocate falpha_begl */
  /* j == d, v == 0 cells, followed by j == d+1, v == 0, etc. */
  ESL_ALLOC(trsmx->fJalpha_begl, sizeof(float **) * (trsmx->W+1));
  ESL_ALLOC(trsmx->fLalpha_begl, sizeof(float **) * (trsmx->W+1));
  ESL_ALLOC(trsmx->fRalpha_begl, sizeof(float **) * (trsmx->W+1));
  for (j = 0; j <= trsmx->W; j++) {
    ESL_ALLOC(trsmx->fJalpha_begl[j],  sizeof(float *) * (cm->M)); /* we still allocate cm->M ptrs, if v != BEGL_S, fJalpha_begl[0][v] will be NULL */
    ESL_ALLOC(trsmx->fLalpha_begl[j],  sizeof(float *) * (cm->M)); /* we still allocate cm->M ptrs, if v != BEGL_S, fLalpha_begl[0][v] will be NULL */
    ESL_ALLOC(trsmx->fRalpha_begl[j],  sizeof(float *) * (cm->M)); /* we still allocate cm->M ptrs, if v != BEGL_S, fRalpha_begl[0][v] will be NULL */
  }
  ESL_ALLOC(trsmx->fJalpha_begl_mem,   sizeof(float) * (trsmx->W+1) * n_begl * (trsmx->W+1));
  ESL_ALLOC(trsmx->fLalpha_begl_mem,   sizeof(float) * (trsmx->W+1) * n_begl * (trsmx->W+1));
  ESL_ALLOC(trsmx->fRalpha_begl_mem,   sizeof(float) * (trsmx->W+1) * n_begl * (trsmx->W+1));
  if((trsmx->flags & cmTRSMX_HAS_INT) && (((trsmx->W+1) * n_begl * (trsmx->W+1)) != trsmx->ncells_alpha_begl)) 
    cm_Fail("cm_IntizeScanMatrix(), cmTRSMX_HAS_FLOAT flag raised, but trsmx->ncells_alpha_begl %d != %d (predicted num float cells size)\n", trsmx->ncells_alpha_begl, ((trsmx->W+1) * n_begl * (trsmx->W+1)));
  trsmx->ncells_alpha_begl = (trsmx->W+1) * n_begl * (trsmx->W+1);

  cur_cell = 0;
  for (v = 0; v < cm->M; v++) {	
    for (j = 0; j <= trsmx->W; j++) { 
      if (cm->stid[v] == BEGL_S) {
	trsmx->fJalpha_begl[j][v] = trsmx->fJalpha_begl_mem + cur_cell;
	trsmx->fLalpha_begl[j][v] = trsmx->fLalpha_begl_mem + cur_cell;
	trsmx->fRalpha_begl[j][v] = trsmx->fRalpha_begl_mem + cur_cell;
	cur_cell += trsmx->W+1;
      }
      else { 
	trsmx->fJalpha_begl[j][v] = NULL;
	trsmx->fLalpha_begl[j][v] = NULL;
	trsmx->fRalpha_begl[j][v] = NULL;
      }
    }
  }
  if(cur_cell != trsmx->ncells_alpha_begl) cm_Fail("cm_FloatizeScanMatrix(), error allocating falpha_begl, cell cts differ %d != %d\n", cur_cell, trsmx->ncells_alpha_begl);

  /* Initialize matrix */
  /* First, init entire matrix to IMPOSSIBLE */
  esl_vec_FSet(trsmx->fJalpha_mem,      trsmx->ncells_alpha,      IMPOSSIBLE);
  esl_vec_FSet(trsmx->fLalpha_mem,      trsmx->ncells_alpha,      IMPOSSIBLE);
  esl_vec_FSet(trsmx->fRalpha_mem,      trsmx->ncells_alpha,      IMPOSSIBLE);
  esl_vec_FSet(trsmx->fTalpha_mem,      trsmx->ncells_Talpha,     IMPOSSIBLE);
  esl_vec_FSet(trsmx->fJalpha_begl_mem, trsmx->ncells_alpha_begl, IMPOSSIBLE);
  esl_vec_FSet(trsmx->fLalpha_begl_mem, trsmx->ncells_alpha_begl, IMPOSSIBLE);
  esl_vec_FSet(trsmx->fRalpha_begl_mem, trsmx->ncells_alpha_begl, IMPOSSIBLE);
  /* Now, initialize cells that should not be IMPOSSIBLE in f{J,L,RT}alpha and f{J,L,R}alpha_begl */
  for(v = cm->M-1; v >= 0; v--) {
    if(cm->stid[v] != BEGL_S) {
      if (cm->sttype[v] == E_st) { 
	trsmx->fJalpha[0][v][0] = trsmx->fJalpha[1][v][0] = 0.;
	trsmx->fLalpha[0][v][0] = trsmx->fLalpha[1][v][0] = 0.;
	trsmx->fRalpha[0][v][0] = trsmx->fRalpha[1][v][0] = 0.;
	/* rest of E deck is IMPOSSIBLE, it's already set */
      }
      else if (cm->sttype[v] == S_st || cm->sttype[v] == D_st) {
	y = cm->cfirst[v];
	trsmx->fJalpha[0][v][0] = cm->endsc[v];
	for (yoffset = 0; yoffset < cm->cnum[v]; yoffset++)
	  trsmx->fJalpha[0][v][0] = ESL_MAX(trsmx->fJalpha[0][v][0], (trsmx->fJalpha[0][y+yoffset][0] + cm->tsc[v][yoffset]));
	trsmx->fJalpha[0][v][0] = ESL_MAX(trsmx->fJalpha[0][v][0], IMPOSSIBLE);
	/* {L,R}alpha[0][v][0] remain IMPOSSIBLE */
      }
      else if (cm->sttype[v] == B_st) {
	w = cm->cfirst[v]; /* BEGL_S, left child state */
	y = cm->cnum[v];
	trsmx->fJalpha[0][v][0] = trsmx->fJalpha_begl[0][w][0] + trsmx->fJalpha[0][y][0]; 
      }
      trsmx->fJalpha[1][v][0] = trsmx->fJalpha[0][v][0];
      /* {L,R,T}alpha[{0,1}][v][0] remain IMPOSSIBLE */
    }
    else { /* v == BEGL_S */
      y = cm->cfirst[v];
      trsmx->fJalpha_begl[0][v][0] = cm->endsc[v];
      for (yoffset = 0; yoffset < cm->cnum[v]; yoffset++)
	trsmx->fJalpha_begl[0][v][0] = ESL_MAX(trsmx->fJalpha_begl[0][v][0], (trsmx->fJalpha[0][y+yoffset][0] + cm->tsc[v][yoffset])); /* careful: y is in trsmx->fJalpha */
      trsmx->fJalpha_begl[0][v][0] = ESL_MAX(trsmx->fJalpha_begl[0][v][0], IMPOSSIBLE);
      for (j = 1; j <= trsmx->W; j++) 
	trsmx->fJalpha_begl[j][v][0] = trsmx->fJalpha_begl[0][v][0];
      /* {L,R}alpha_begl[j][v][0] remain IMPOSSIBLE for all j */
    }
  }
  /* set the flag that tells us we've got valid floats */
  trsmx->flags |= cmTRSMX_HAS_FLOAT;
  return eslOK;

 ERROR: 
  cm_Fail("memory allocation error.");
  return status; /* NEVERREACHED */
}

/* Function: cm_FreeTrScanMatrix()
 * Date:     EPN, Wed Aug 17 14:22:45 2011
 *
 * Purpose:  Free a TrScanMatrix_t object corresponding
 *           to CM <cm>.
 *            
 * Returns:  void
 */
void
cm_FreeTrScanMatrix(CM_t *cm, TrScanMatrix_t *trsmx)
{
  int j;
  //if(! ((cm->flags & CMH_TRSCANMATRIX) && (trsmx == cm->trsmx))) { /* don't free the cm->trsmx's dmax */
  //if(trsmx->dmax != cm->dmax && trsmx->dmax != NULL) { free(trsmx->dmax); trsmx->dmax = NULL; }
  //}

  for(j = 1; j <= trsmx->W; j++) {
    free(trsmx->dnAA[j]);
    free(trsmx->dxAA[j]);
  }
  free(trsmx->dnAA);
  free(trsmx->dxAA);
  free(trsmx->bestr);
  
  if(trsmx->flags & cmTRSMX_HAS_FLOAT) cm_FreeFloatsFromTrScanMatrix(cm, trsmx);
  //if(trsmx->flags & cmTRSMX_HAS_INT)   cm_FreeIntsFromTrScanMatrix(cm, trsmx);
  free(trsmx);
  return;
}

/* Function: cm_FreeFloatsFromTrScanMatrix()
 * Date:     EPN, Wed Aug 17 14:19:21 2011
 *
 * Purpose:  Free float data structures in a TrScanMatrix_t object 
 *           corresponding to <cm>.
 *            
 * Returns:  eslOK on success, dies immediately on an error.
 */
int
cm_FreeFloatsFromTrScanMatrix(CM_t *cm, TrScanMatrix_t *trsmx)
{
  int j;

  /* contract check */
  if(! trsmx->flags & cmTRSMX_HAS_FLOAT)  cm_Fail("cm_FreeFloatsFromScanMatrix(), si's cmTRSMX_HAS_FLOAT flag is down.");
  if(trsmx->fJalpha == NULL)              cm_Fail("cm_FreeFloatsFromScanMatrix(), trsmx->fJalpha is already NULL.");
  if(trsmx->fJalpha_begl == NULL)         cm_Fail("cm_FreeFloatsFromScanMatrix(), trsmx->fJalpha_begl is already NULL.");
  if(trsmx->fLalpha == NULL)              cm_Fail("cm_FreeFloatsFromScanMatrix(), trsmx->fLalpha is already NULL.");
  if(trsmx->fLalpha_begl == NULL)         cm_Fail("cm_FreeFloatsFromScanMatrix(), trsmx->fLalpha_begl is already NULL.");
  if(trsmx->fRalpha == NULL)              cm_Fail("cm_FreeFloatsFromScanMatrix(), trsmx->fRalpha is already NULL.");
  if(trsmx->fRalpha_begl == NULL)         cm_Fail("cm_FreeFloatsFromScanMatrix(), trsmx->fRalpha_begl is already NULL.");
  if(trsmx->fTalpha == NULL)              cm_Fail("cm_FreeFloatsFromScanMatrix(), trsmx->fTalpha is already NULL.");

  free(trsmx->fJalpha_mem);
  free(trsmx->fJalpha[1]);
  free(trsmx->fJalpha[0]);
  free(trsmx->fJalpha);
  trsmx->fJalpha = NULL;

  free(trsmx->fLalpha_mem);
  free(trsmx->fLalpha[1]);
  free(trsmx->fLalpha[0]);
  free(trsmx->fLalpha);
  trsmx->fLalpha = NULL;

  free(trsmx->fRalpha_mem);
  free(trsmx->fRalpha[1]);
  free(trsmx->fRalpha[0]);
  free(trsmx->fRalpha);
  trsmx->fRalpha = NULL;

  free(trsmx->fTalpha_mem);
  free(trsmx->fTalpha[1]);
  free(trsmx->fTalpha[0]);
  free(trsmx->fTalpha);
  trsmx->fTalpha = NULL;

  free(trsmx->fJalpha_begl_mem);
  for (j = 0; j <= trsmx->W; j++) free(trsmx->fJalpha_begl[j]);
  free(trsmx->fJalpha_begl);
  trsmx->fJalpha_begl = NULL;

  free(trsmx->fLalpha_begl_mem);
  for (j = 0; j <= trsmx->W; j++) free(trsmx->fLalpha_begl[j]);
  free(trsmx->fLalpha_begl);
  trsmx->fLalpha_begl = NULL;

  free(trsmx->fRalpha_begl_mem);
  for (j = 0; j <= trsmx->W; j++) free(trsmx->fRalpha_begl[j]);
  free(trsmx->fRalpha_begl);
  trsmx->fRalpha_begl = NULL;

  trsmx->flags &= ~cmTRSMX_HAS_FLOAT;
  return eslOK;
}


/*****************************************************************
 * Benchmark driver
 *****************************************************************/
#ifdef IMPL_TRUNC_SEARCH_BENCHMARK
/* gcc   -o benchmark-trunc-search -std=gnu99 -g -O2 -I. -L. -I../hmmer/src -L../hmmer/src -I../easel -L../easel -DIMPL_TRUNC_SEARCH_BENCHMARK cm_dpsearch_trunc.c -linfernal -lhmmer -leasel -lm
 * gcc   -o benchmark-trunc-search -std=gnu99 -g -Wall -I. -L. -I../hmmer/src -L../hmmer/src -I../easel -L../easel -DIMPL_TRUNC_SEARCH_BENCHMARK cm_dpsearch_trunc.c -linfernal -lhmmer -leasel -lm
 * ./benchmark-trunc-search <cmfile>
 */

#include "esl_config.h"
#include "p7_config.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "easel.h"
#include <esl_getopts.h>
#include <esl_histogram.h>
#include <esl_random.h>
#include <esl_randomseq.h>
#include <esl_sqio.h>
#include <esl_stats.h>
#include <esl_stopwatch.h>
#include <esl_vectorops.h>
#include <esl_wuss.h>

#include "funcs.h"		/* function declarations                */
#include "old_funcs.h"		/* function declarations for 0.81 versions */
#include "structs.h"		/* data structures, macros, #define's   */

static ESL_OPTIONS options[] = {
  /* name           type      default  env  range toggles reqs incomp  help                                       docgroup*/
  { "-h",        eslARG_NONE,    NULL, NULL, NULL,  NULL,  NULL, NULL, "show brief help on version and usage",           0 },
  { "-s",        eslARG_INT,    "181", NULL, NULL,  NULL,  NULL, NULL, "set random number seed to <n>, '0' for one-time arbitrary", 0 },
  { "-e",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "emit sequences from CM, don't randomly create them", 0 },
  { "-g",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "search in glocal mode [default: local]", 0 },
  { "-L",        eslARG_INT,  "10000", NULL, "n>0", NULL,  NULL, NULL, "length of random target seqs",                   0 },
  { "-N",        eslARG_INT,      "1", NULL, "n>0", NULL,  NULL, NULL, "number of random target seqs",                   0 },
  { "--dc",      eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "also search with D&C trCYK",                     0},
  { "--noqdb",   eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "don't use QDBs", 0},
  { "--infile",  eslARG_INFILE,  NULL, NULL, NULL,  NULL,  NULL, "-L,-N,-e", "read sequences to search from file <s>", 2 },
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static char usage[]  = "[-options] <cmfile>";
static char banner[] = "benchmark driver for scanning trCYK implementations";

int 
main(int argc, char **argv)
{
  int             status;
  ESL_GETOPTS    *go      = esl_getopts_CreateDefaultApp(options, 1, argc, argv, banner, usage);
  CM_t           *cm;
  ESL_STOPWATCH  *w       = esl_stopwatch_Create();
  ESL_RANDOMNESS *r       = NULL;
  ESL_ALPHABET   *abc     = NULL;
  int             L       = esl_opt_GetInteger(go, "-L");
  int             N       = esl_opt_GetInteger(go, "-N");
  ESL_DSQ        *dsq;
  int             i;
  float           sc;
  char           *cmfile = esl_opt_GetArg(go, 1);
  CM_FILE        *cmfp;	/* open input CM file stream */
  int            *dmin;
  int            *dmax;
  int             do_random;
  seqs_to_aln_t  *seqs_to_aln;  /* sequences to align, either randomly created, or emitted from CM (if -e) */
  char            errbuf[cmERRBUFSIZE];
  TrScanMatrix_t *trsmx = NULL;
  ESL_SQFILE     *sqfp  = NULL;        /* open sequence input file stream */

  /* setup logsum lookups (could do this only if nec based on options, but this is safer) */
  init_ilogsum();
  FLogsumInit();

  r = esl_randomness_Create(esl_opt_GetInteger(go, "-s"));

  if ((status = cm_file_Open(cmfile, NULL, FALSE, &(cmfp), errbuf)) != eslOK) cm_Fail(errbuf);
  if ((status = cm_file_Read(cmfp, TRUE, &abc, &cm))                != eslOK) cm_Fail(cmfp->errbuf);
  cm_file_Close(cmfp);

  do_random = TRUE;
  if(esl_opt_GetBoolean(go, "-e")) do_random = FALSE; 

  if(! esl_opt_GetBoolean(go, "-g"))     cm->config_opts |= CM_CONFIG_LOCAL;
  if( esl_opt_GetBoolean(go, "--noqdb")) cm->search_opts |= CM_SEARCH_NOQDB;
  else                                   cm->config_opts |= CM_CONFIG_QDB;
  ConfigCM(cm, errbuf, FALSE, NULL, NULL); /* FALSE says: don't calculate W */

  if (esl_opt_GetBoolean(go, "--noqdb")) { 
    if(cm->dmin != NULL) { free(cm->dmin); cm->dmin = NULL; }
    if(cm->dmax != NULL) { free(cm->dmax); cm->dmax = NULL; }
  }
  dmin = cm->dmin; 
  dmax = cm->dmax; 

  cm_CreateScanMatrixForCM(cm, TRUE, TRUE); /* impt to do this after QDBs set up in ConfigCM() */
#if 0
  trsmx = cm_CreateTrScanMatrix   (cm, cm->W, dmax, cm->beta_W, cm->beta_qdb, 
				   (dmin == NULL && dmax == NULL) ? FALSE : TRUE,
				   TRUE, FALSE); /* do_float, do_int */
  if(trsmx == NULL) esl_fatal("Problem creating trsmx");
#endif
  
  /* get sequences */
  if(esl_opt_IsUsed(go, "--infile")) { 
    /* read sequences from a file */
    status = esl_sqfile_OpenDigital(cm->abc, esl_opt_GetString(go, "--infile"), eslSQFILE_UNKNOWN, NULL, &sqfp);
    if (status == eslENOTFOUND)    esl_fatal("File %s doesn't exist or is not readable\n", esl_opt_GetString(go, "--infile"));
    else if (status == eslEFORMAT) esl_fatal("Couldn't determine format of sequence file %s\n", esl_opt_GetString(go, "--infile"));
    else if (status == eslEINVAL)  esl_fatal("Can't autodetect stdin or .gz."); 
    else if (status != eslOK)      esl_fatal("Sequence file open failed with error %d.\n", status);

    seqs_to_aln = CreateSeqsToAln(100, FALSE);
    if((status = ReadSeqsToAln(cm->abc, sqfp, 0, seqs_to_aln, FALSE)) != eslEOF)
      esl_fatal("Error reading sqfile: %s\n", esl_opt_GetString(go, "--infile"));
    esl_sqfile_Close(sqfp);
    N = seqs_to_aln->nseq;
  }
  else if(!esl_opt_IsDefault(go, "-L")) {
     double *dnull;
     ESL_DSQ *randdsq = NULL;
     ESL_ALLOC(randdsq, sizeof(ESL_DSQ)* (L+2));
     ESL_ALLOC(dnull, sizeof(double) * cm->abc->K);
     for(i = 0; i < cm->abc->K; i++) dnull[i] = (double) cm->null[i];
     esl_vec_DNorm(dnull, cm->abc->K);
     seqs_to_aln = CreateSeqsToAln(N, FALSE);

     for (i = 0; i < N; i++) {
       if (esl_rsq_xIID(r, dnull, cm->abc->K, L, randdsq)  != eslOK) cm_Fail("Failure creating random sequence.");
       if((seqs_to_aln->sq[i] = esl_sq_CreateDigitalFrom(abc, NULL, randdsq, L, NULL, NULL, NULL)) == NULL)
         cm_Fail("Failure digitizing/copying random sequence.");
     }
  }
  else if(do_random) {
    double *dnull;
    ESL_ALLOC(dnull, sizeof(double) * cm->abc->K);
    for(i = 0; i < cm->abc->K; i++) dnull[i] = (double) cm->null[i];
    esl_vec_DNorm(dnull, cm->abc->K);
    /* get gamma[0] from the QDB calc alg, which will serve as the length distro for random seqs */
    int safe_windowlen = cm->clen * 2;
    double **gamma = NULL;
    while(!(BandCalculationEngine(cm, safe_windowlen, DEFAULT_BETA, TRUE, NULL, NULL, &(gamma), NULL))) {
      safe_windowlen *= 2;
      /* This is a temporary fix becuase BCE() overwrites gamma, leaking memory
       * Probably better long-term for BCE() to check whether gamma is already allocated
       */
      FreeBandDensities(cm, gamma);
      if(safe_windowlen > (cm->clen * 1000)) cm_Fail("Error trying to get gamma[0], safe_windowlen big: %d\n", safe_windowlen);
    }
    seqs_to_aln = RandomEmitSeqsToAln(r, cm->abc, dnull, 1, N, gamma[0], safe_windowlen, FALSE);
    FreeBandDensities(cm, gamma);
    free(dnull);
  }
  else /* don't randomly generate seqs, emit them from the CM */
    seqs_to_aln = CMEmitSeqsToAln(r, cm, 1, N, FALSE, NULL, FALSE);

  SetMarginalScores(cm);

  for (i = 0; i < N; i++)
    {
      L = seqs_to_aln->sq[i]->n;
      dsq = seqs_to_aln->sq[i]->dsq;
      cm->search_opts &= ~CM_SEARCH_INSIDE;

      trsmx = cm_CreateTrScanMatrix   (cm, cm->W, dmax, cm->beta_W, cm->beta_qdb, 
				       (dmin == NULL && dmax == NULL) ? FALSE : TRUE,
				       TRUE, FALSE); /* do_float, do_int */
      if(trsmx == NULL) esl_fatal("Problem creating trsmx");
      
      esl_stopwatch_Start(w);
      if((status = FastCYKScan(cm, errbuf, cm->smx, dsq, 1, L, 0., NULL, FALSE, 0., NULL, NULL, NULL, &sc)) != eslOK) cm_Fail(errbuf);
      printf("%4d %-30s %10.4f bits ", (i+1), "FastCYKScan(): ", sc);
      esl_stopwatch_Stop(w);
      esl_stopwatch_Display(stdout, w, " CPU time: ");

      esl_stopwatch_Start(w);
      if((status = RefCYKScan(cm, errbuf, cm->smx, dsq, 1, L, 0., NULL, FALSE, 0., NULL, NULL, NULL, &sc)) != eslOK) cm_Fail(errbuf);
      printf("%4d %-30s %10.4f bits ", (i+1), "RefCYKScan(): ", sc);
      esl_stopwatch_Stop(w);
      esl_stopwatch_Display(stdout, w, " CPU time: ");

      esl_stopwatch_Start(w);
      if((status = RefTrCYKScan(cm, errbuf, trsmx, dsq, 1, L, 0., NULL, FALSE, 0., NULL, NULL, NULL, &sc)) != eslOK) cm_Fail(errbuf);
      printf("%4d %-30s %10.4f bits ", (i+1), "RefTrCYKScan(): ", sc);
      esl_stopwatch_Stop(w);
      esl_stopwatch_Display(stdout, w, " CPU time: ");

      esl_stopwatch_Start(w);
      sc = TrCYK_Inside(cm, dsq, L, 0, 1, L, FALSE, NULL);
      printf("%4d %-30s %10.4f bits ", (i+1), "TrCYK_Inside():   ", sc);
      esl_stopwatch_Stop(w);
      esl_stopwatch_Display(stdout, w, " CPU time: ");

      if(esl_opt_GetBoolean(go, "--dc")) { 
	esl_stopwatch_Start(w);
	sc = TrCYK_DnC(cm, dsq, L, 0, 1, L, FALSE);
	printf("%4d %-30s %10.4f bits ", (i+1), "TrCYK_DnC():      ", sc);
	esl_stopwatch_Stop(w);
	esl_stopwatch_Display(stdout, w, " CPU time: ");
      }

      printf("\n");
      cm_FreeTrScanMatrix(cm, trsmx);
    }
  FreeCM(cm);
  FreeSeqsToAln(seqs_to_aln);
  //cm_FreeTrScanMatrix(cm, trsmx);
  esl_alphabet_Destroy(abc);
  esl_stopwatch_Destroy(w);
  esl_randomness_Destroy(r);
  esl_getopts_Destroy(go);
  return 0;

 ERROR:
  cm_Fail("memory allocation error");
  return 0; /* never reached */
}
#endif /*IMPL_TRUNC_SEARCH_BENCHMARK*/

