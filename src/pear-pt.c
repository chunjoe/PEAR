#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include "args.h"
#include "emp.h"
#include "reader.h"
#include "async.h"

/** @file pear-pt.c
    @brief Main file containing scoring and assembly related functions (pthreads version)
*/
#define         PHRED_INIT                       33
#define         THREAD_MIN_PACKET_SIZE          100
#define         THREAD_PACKET_SIZE_DELTA         20

static pthread_mutex_t cs_mutex_wnd  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cs_mutex_io   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cs_mutex_cond = PTHREAD_COND_INITIALIZER;

struct thread_global_t thr_global;


//int stat_test (double, double, int, double);
int stat_test2 (double, double, int, double);

double
assemble_overlap (struct read_t * left, struct read_t * right, int base_left, int base_right, int ol_size, struct read_t * ai);

/*
double
assemble_overlap_ef (struct reads_info * left, struct reads_info * right, int base_left, int base_right, int ol_size, struct asm_info * ai, struct emp_freq  * ef);
*/

/* TODO: Dynamically allocate them */
double      sc_eq[256][256];
double     sc_neq[256][256];
double     sc_eqA[256][256];
double     sc_eqC[256][256];
double     sc_eqG[256][256];
double     sc_eqT[256][256];
double   sc_neqAC[256][256];
double   sc_neqAG[256][256];
double   sc_neqAT[256][256];
double   sc_neqCA[256][256];
double   sc_neqCG[256][256];
double   sc_neqCT[256][256];
double   sc_neqGA[256][256];
double   sc_neqGC[256][256];
double   sc_neqGT[256][256];
double   sc_neqTA[256][256];
double   sc_neqTC[256][256];
double   sc_neqTG[256][256];
double     qs_mul[256][256];

int match_score    = 1;
int mismatch_score = 1;

/** @brief Trimming of forward part of unassembled sequence
  *
  * Finds two adjacent quality scores that are smaller than the minimum quality score threshold \a min_quality.
  * It then \e trims the part starting from the rightmost quality score position.
  *
  * @param read
  *   Forward sequence
  *
  * @param min_quality
  *   Minimum quality score threshold
  *
  * @param len
  *   Length of the forward sequence
  *
  * @return
  *   Returns the length of the trimmed sequence
  *
  * @todo
  *  Change the function (and also trim_cpl) so that it does not require the parameter \a len
  */
int
trim (struct read_t * read, int min_quality, int len, double * uncalled)
{
  int                   i;
  // TODO:  Change so that len is not required
  *uncalled = 0;

  for (i = 0; i < len - 1; ++ i)
   {
     if (read->data[i] == 'N' || read->data[i] == 'n') ++ (*uncalled);
     if ( read->qscore[i] - PHRED_INIT < min_quality && read->qscore[i + 1] - PHRED_INIT < min_quality)
      {
        read->qscore[i + 1] = 0;
        read->data[i + 1] = 0;
        *uncalled = (*uncalled) / (i + 1);
        return i + 1;
      }
   }

  if (read->data[len - 1] ==  'N' || read->data[len - 1] == 'n') ++ (*uncalled);
  *uncalled = (*uncalled) / len;
  return (len);
}

int
trim_cpl (struct read_t * read, int min_quality, int len, double * uncalled)
{
  int                   i, j;

  *uncalled = 0;

  for (i = len - 1; i > 0; -- i)
   {
     if (read->qscore[i] - PHRED_INIT < min_quality && read->qscore[i - 1] - PHRED_INIT < min_quality)
      {
        read->qscore[i - 1] = 0;
        read->data[i - 1] = 0;
        memmove (read->data, read->data + i, strlen (read->data + i) + 1);
        memmove (read->qscore, read->qscore + i, strlen (read->qscore + i) + 1);
        for (j = 0; read->data[j]; ++ j)
          if (read->data[j] == 'N' || read->data[j] == 'n') ++ (*uncalled);
        *uncalled = (*uncalled) / j;
        return (len - i);
      }
   }
  for (j = 0; read->data[j]; ++ j)
    if (read->data[j] == 'N' || read->data[j] == 'n') ++ (*uncalled);
  *uncalled = (*uncalled) / j;
  return (len);
}

void init_scores (int match, int mismatch, struct emp_freq * ef)
{
  int           i, j;
  double        ex, ey;
  double        pa2, pc2, pg2, pt2, pagt2, pcgt2, pact2, pacg2, pacg, pact, pagt, pcgt;
  double        pac2, pag2, pat2, pcg2, pct2, pgt2;
  double        p2acg, p2act, p2agt, p2cgt;

  pa2 = ef->pa * ef->pa;
  pc2 = ef->pc * ef->pc;
  pg2 = ef->pg * ef->pg;
  pt2 = ef->pt * ef->pt;

  pacg2 = pa2 + pc2 + pg2;  pacg = ef->pa + ef->pc + ef->pg; p2acg = pacg * pacg;
  pact2 = pa2 + pc2 + pt2;  pact = ef->pa + ef->pc + ef->pt; p2act = pact * pact;
  pagt2 = pa2 + pg2 + pt2;  pagt = ef->pa + ef->pg + ef->pt; p2agt = pagt * pagt;
  pcgt2 = pc2 + pg2 + pt2;  pcgt = ef->pc + ef->pg + ef->pt; p2cgt = pcgt * pcgt;

  pac2 = (ef->pa + ef->pc) * (ef->pa + ef->pc);
  pag2 = (ef->pa + ef->pg) * (ef->pa + ef->pg);
  pat2 = (ef->pa + ef->pt) * (ef->pa + ef->pt);
  pcg2 = (ef->pc + ef->pg) * (ef->pc + ef->pg);
  pct2 = (ef->pc + ef->pt) * (ef->pc + ef->pt);
  pgt2 = (ef->pg + ef->pt) * (ef->pg + ef->pt);


  for (i = 0; i < 256; ++ i)
   {
     for (j = 0; j < 256; ++ j) 
      {
        ex = pow (10.0, - (i - PHRED_INIT) / 10.0);
        ey = pow (10.0, - (j - PHRED_INIT) / 10.0);

        sc_eq[i][j]  =    match * ((1 - ex) * (1 - ey) + (ex * ey) / 3.0);
        sc_neq[i][j] = mismatch * (1 - (1.0 / 3.0) * (1 - ex) * ey - (1.0 / 3.0) * (1 - ey) * ex - (2.0 / 9.0) * ex * ey);
        qs_mul[i][j] = ex * ey;

        sc_eqA[i][j] = match * (1 - ex) * (1 - ey) + (ex * ey) * pcgt2 / p2cgt;
        sc_eqC[i][j] = match * (1 - ex) * (1 - ey) + (ex * ey) * pagt2 / p2agt;
        sc_eqG[i][j] = match * (1 - ex) * (1 - ey) + (ex * ey) * pact2 / p2act;
        sc_eqT[i][j] = match * (1 - ex) * (1 - ey) + (ex * ey) * pacg2 / p2acg;

        sc_neqAC[i][j] = mismatch * (1- (1 - ey) * ex * (ef->pc / pcgt) - (1 - ex) * ey * (ef->pa / pagt) - ex * ey * (pg2 + pt2) / pgt2);
        sc_neqCA[i][j] = mismatch * (1- (1 - ey) * ex * (ef->pa / pagt) - (1 - ex) * ey * (ef->pc / pcgt) - ex * ey * (pg2 + pt2) / pgt2);

        sc_neqAG[i][j] = mismatch * (1- (1 - ey) * ex * (ef->pg / pcgt) - (1 - ex) * ey * (ef->pa / pact) - ex * ey * (pc2 + pt2) / pct2);
        sc_neqGA[i][j] = mismatch * (1- (1 - ey) * ex * (ef->pa / pact) - (1 - ex) * ey * (ef->pg / pcgt) - ex * ey * (pc2 + pt2) / pct2);

        sc_neqAT[i][j] = mismatch * (1- (1 - ey) * ex * (ef->pt / pcgt) - (1 - ex) * ey * (ef->pa / pacg) - ex * ey * (pc2 + pg2) / pcg2);
        sc_neqTA[i][j] = mismatch * (1- (1 - ey) * ex * (ef->pa / pacg) - (1 - ex) * ey * (ef->pt / pcgt) - ex * ey * (pc2 + pg2) / pcg2);

        sc_neqCG[i][j] = mismatch * (1- (1 - ey) * ex * (ef->pg / pagt) - (1 - ex) * ey * (ef->pc / pact) - ex * ey * (pa2 + pt2) / pat2);
        sc_neqGC[i][j] = mismatch * (1- (1 - ey) * ex * (ef->pc / pact) - (1 - ex) * ey * (ef->pg / pagt) - ex * ey * (pa2 + pt2) / pat2);

        sc_neqCT[i][j] = mismatch * (1- (1 - ey) * ex * (ef->pt / pagt) - (1 - ex) * ey * (ef->pc / pacg) - ex * ey * (pa2 + pg2) / pag2);
        sc_neqTC[i][j] = mismatch * (1- (1 - ey) * ex * (ef->pc / pacg) - (1 - ex) * ey * (ef->pt / pagt) - ex * ey * (pa2 + pg2) / pag2);

        sc_neqGT[i][j] = mismatch * (1- (1 - ey) * ex * (ef->pt / pact) - (1 - ex) * ey * (ef->pg / pacg) - ex * ey * (pa2 + pc2) / pac2);
        sc_neqTG[i][j] = mismatch * (1- (1 - ey) * ex * (ef->pg / pacg) - (1 - ex) * ey * (ef->pt / pact) - ex * ey * (pa2 + pc2) / pac2);
     }
   }
}

inline void
scoring_ef (char dleft, char dright, char qleft, char qright, int score_method, double * score, int match, int mismatch, struct emp_freq * ef)
{
  if (dleft == 'N' || dright == 'N')       /* one of them is N */
   {
     switch (score_method)
      {
        case 1:
          *score += (ef->q * match - (1 - ef->q) * mismatch);
          break;
        case 2:
          *score -= (1 - ef->q) * mismatch; 
          break;
        case 3:
          *score -= mismatch;
          break;
      }
   }
  else if (dleft == dright)     /* equal */
   {
     switch (score_method)
      {
        case 1:
          switch (dleft)
           {
             case 'A':
               *score += (sc_eqA[(int)qright][(int)qleft] - (1 - sc_eqA[(int)qright][(int)qleft] / match) * mismatch);
               break;
             case 'C':
               *score += (sc_eqC[(int)qright][(int)qleft] - (1 - sc_eqC[(int)qright][(int)qleft] / match) * mismatch);
               break;
             case 'G':
               *score += (sc_eqG[(int)qright][(int)qleft] - (1 - sc_eqG[(int)qright][(int)qleft] / match) * mismatch);
               break;
             case 'T':
               *score += (sc_eqT[(int)qright][(int)qleft] - (1 - sc_eqT[(int)qright][(int)qleft] / match) * mismatch);
               break;
           }
          break;
        case 2:
          switch (dleft)
           {
             case 'A':
               *score += sc_eqA[(int)qright][(int)qleft];
               break;
             case 'C':
               *score += sc_eqC[(int)qright][(int)qleft];
               break;
             case 'G':
               *score += sc_eqG[(int)qright][(int)qleft];
               break;
             case 'T':
               *score += sc_eqT[(int)qright][(int)qleft];
               break;
           }
          break;
        case 3:
          *score += match;
          break;
      }
   }
  else          /* not equal */
   {
     switch (score_method)
      {
        case 1:
           switch  (dleft)
           {
             case 'A':
               switch (dright)
                {
                  case 'C':
                    *score = *score - (sc_neqAC[(int)qleft][(int)qright] - (1 - sc_neqAC[(int)qleft][(int)qright] / mismatch) * match);
                    break;
                  case 'G':
					*score = *score - (sc_neqAG[(int)qleft][(int)qright] - (1 - sc_neqAG[(int)qleft][(int)qright] / mismatch) * match);
                    break;
                  case 'T':
					*score = *score - (sc_neqAT[(int)qleft][(int)qright] - (1 - sc_neqAT[(int)qleft][(int)qright] / mismatch) * match);
                    break;
                }
               break;
             case 'C':
               switch (dright)
                {
                  case 'A':
					*score = *score - (sc_neqCA[(int)qleft][(int)qright] - (1 - sc_neqCA[(int)qleft][(int)qright] / mismatch) * match);
                    break;
                  case 'G':
					*score = *score - (sc_neqCG[(int)qleft][(int)qright] - (1 - sc_neqCG[(int)qleft][(int)qright] / mismatch) * match);
                    break;
                  case 'T':
					*score = *score - (sc_neqCT[(int)qleft][(int)qright] - (1 - sc_neqCT[(int)qleft][(int)qright] / mismatch) * match);
                    break;
                }
               break;
             case 'G':
               switch (dright)
                {
                  case 'A':
                    *score = *score - (sc_neqGA[(int)qleft][(int)qright] - (1 - sc_neqGA[(int)qleft][(int)qright] / mismatch) * match);
                    break;
                  case 'C':
                    *score = *score - (sc_neqGC[(int)qleft][(int)qright] - (1 - sc_neqGC[(int)qleft][(int)qright] / mismatch) * match);
                    break;
                  case 'T':
					*score = *score - (sc_neqGT[(int)qleft][(int)qright] - (1 - sc_neqGT[(int)qleft][(int)qright] / mismatch) * match);
                    break;
                }
               break;
             case 'T':
               switch (dright)
                {
                  case 'A':
					*score = *score - (sc_neqTA[(int)qleft][(int)qright] - (1 - sc_neqTA[(int)qleft][(int)qright] / mismatch) * match);
                    break;
                  case 'C':
					*score = *score - (sc_neqTC[(int)qleft][(int)qright] - (1 - sc_neqTC[(int)qleft][(int)qright] / mismatch) * match);
                    break;
                  case 'G':
					*score = *score - (sc_neqTG[(int)qleft][(int)qright] - (1 - sc_neqTG[(int)qleft][(int)qright] / mismatch) * match);
                    break;
                }
               break;
           }
          break;
        case 2:
          switch  (dleft)
           {
             case 'A':
               switch (dright)
                {
                  case 'C':
                    *score -= sc_neqAC[(int)qleft][(int)qright];
                    break;
                  case 'G':
                    *score -= sc_neqAG[(int)qleft][(int)qright];
                    break;
                  case 'T':
                    *score -= sc_neqAT[(int)qleft][(int)qright];
                    break;
                }
               break;
             case 'C':
               switch (dright)
                {
                  case 'A':
                    *score -= sc_neqCA[(int)qleft][(int)qright];
                    break;
                  case 'G':
                    *score -= sc_neqCG[(int)qleft][(int)qright];
                    break;
                  case 'T':
                    *score -= sc_neqCT[(int)qleft][(int)qright];
                    break;
                }
               break;
             case 'G':
               switch (dright)
                {
                  case 'A':
                    *score -= sc_neqGA[(int)qleft][(int)qright];
                    break;
                  case 'C':
                    *score -= sc_neqGC[(int)qleft][(int)qright];
                    break;
                  case 'T':
                    *score -= sc_neqGT[(int)qleft][(int)qright];
                    break;
                }
               break;
             case 'T':
               switch (dright)
                {
                  case 'A':
                    *score -= sc_neqTA[(int)qleft][(int)qright];
                    break;
                    break;
                  case 'C':
                    *score -= sc_neqTC[(int)qleft][(int)qright];
                    break;
                  case 'G':
                    *score -= sc_neqTG[(int)qleft][(int)qright];
                    break;
                }
               break;
           }
          break;
        case 3:
          *score -= mismatch;
          break;
      }
   }
}

/* TODO: Remember to speed up this function by doing something with the multiplication and division of match/mismatch */
inline void 
scoring (char dleft, char dright, char qleft, char qright, int score_method, double * score, int match, int mismatch)
{
  if (dleft == 'N' || dright == 'N')       /* one of them is N */
   {
     switch (score_method)
      {
        case 1:
          *score += (0.25 * match - (1 - 0.25) * mismatch);
          break;
        case 2:
          *score -= (1 - 0.25) * mismatch; 
          break;
        case 3:
          *score -= mismatch;
          break;
      }
   }
  else if (dleft == dright)     /* equal */
   {
     switch (score_method)
      {
        case 1:
          *score += (sc_eq[(int)qright][(int)qleft] - (1 - sc_eq[(int)qright][(int)qleft] / match) * mismatch);
          break;
        case 2:
          *score += sc_eq[(int)qright][(int)qleft];
          break;
        case 3:
          *score += match;
          break;
      }
   }
  else          /* not equal */
   {
     switch (score_method)
      {
        case 1:
          *score = *score - (sc_neq[(int)qright][(int)qleft] - (1 - sc_neq[(int)qright][(int)qleft] / mismatch) * match);
          break;
        case 2:
          *score -= sc_neq[(int)qright][(int)qleft];
          break;
        case 3:
          *score -= mismatch;
          break;
      }
   }
}

inline int
assembly_ef (struct read_t * left, struct read_t * right, int match_score, int mismatch_score, struct emp_freq * ef, struct user_args  * sw)
{
  int                   i,j;
  int                   n;
  double                score;
  double                best_score = 0;
  int                   best_overlap = 0;       /* overlap of the best alignment */
  int                   run_through = 0;
  int                   nMatch;
  int                   asm_len = 0;
  int                   st_pass;
  int                   uncalled = 0;
  
  n = strlen (left->data);
     
  /* compute score for every overlap */
  score = 0;
  for (i = 0; i <= n; ++ i)    /* the size of the overlap */
   {     
     nMatch = 0;
     score = 0;
     for (j = 0; j < i; ++ j)
      {
        scoring_ef (left->data[n - i + j], right->data[j], left->qscore[n - i + j], right->qscore[j], sw->score_method, &score, match_score, mismatch_score, ef);
        if (left->data[n - i + j] == right->data[j]) ++nMatch;
      }
     if (score > best_score)
      {
        best_overlap = i;
        best_score   = score;
      }
   }

  /* compute for score for runthrough case */
  for (i = n - 1; i > 0; --i)
   {
     score  = 0;
     nMatch = 0;
     for (j = 0; j < i; ++j)
      {
        scoring_ef (left->data[j], right->data[n - i + j], left->qscore[j], right->qscore[n - i + j], sw->score_method, &score, match_score, mismatch_score, ef);
        if (left->data[n - i + j] == right->data[j]) ++nMatch;
      }

     if (score > best_score)
      {
        run_through  = 1;
        best_overlap = i;
        best_score   = score;
      }
   }


  if (sw->test == 1)
   {
     st_pass = stat_test2 (sw->p_value, best_score, sw->min_overlap, ef->q);
   }
  else
   {
     st_pass = stat_test2 (sw->p_value, best_score, best_overlap, ef->q);
   }

  if (!st_pass) return (0);


  /* do the assembly!!!! */
  if (!run_through)
   {
     if (best_overlap == 0)
      {
        asm_len = 2 * n;

        for (j = 0; j < n; ++ j)
          if (left->data[j] == 'N' || left->data[j] == 'n')  ++uncalled;
        for (j = 0; j < n; ++ j)
          if (right->data[j] == 'N' || right->data[j] == 'n')  ++uncalled;
        uncalled /= asm_len;

        if (2 * n - asm_len >= sw->min_overlap && asm_len >= sw->min_asm_len && asm_len <= sw->max_asm_len && uncalled <= sw->max_uncalled)
         {
           *(left->data - 1) = 1;
         }
        else
         {
           return (0);
         }
      }
     else if (best_overlap == n )
      {
        asm_len         = n;

        for (j = 0; j < asm_len; ++ j)
          if ((left->data[j] == 'N' || left->data[j] == 'n') && (right->data[j] == 'N' || right->data[j] == 'n')) ++uncalled;
        uncalled /= asm_len;

        if (2 * n - asm_len >= sw->min_overlap && asm_len >= sw->min_asm_len && asm_len <= sw->max_asm_len && uncalled <= sw->max_uncalled)
         {
           *(left->data - 1) = 0;
           assemble_overlap (left, right, 0, 0, n, left);
           left->data[n]   = 0;
           left->qscore[n] = 0;
         }
        else
         {
           return (0);
         }
      }
     else
      {
        asm_len = 2 * n - best_overlap;
        for (j = 0; j < n - best_overlap; ++ j)
          if (left->data[j] == 'N' || left->data[j] == 'n')  ++uncalled;
        for (j = n - best_overlap; j < n; ++ j)
          if ((left->data[j] == 'N' || left->data[j] == 'n') && (right->data[j - n + best_overlap] == 'N' || right->data[j - n + best_overlap] == 'n'))  ++uncalled;
        for (j = best_overlap; j < n; ++ j)
          if (right->data[j] == 'N' || right->data[j] == 'n')  ++uncalled;
        uncalled /= asm_len;

        if (2 * n - asm_len >= sw->min_overlap && asm_len >= sw->min_asm_len && asm_len <= sw->max_asm_len && uncalled <= sw->max_uncalled)
         {
           *(left->data - 1) = 1;
           
           assemble_overlap (left, right, n - best_overlap, 0, best_overlap, left);
           memmove (right->data,   right->data   + best_overlap,  n - best_overlap);
           memmove (right->qscore, right->qscore + best_overlap,  n - best_overlap);
           /* THIS IS WRONG */
           //memcpy (right->data,   right->data   + best_overlap,  n - best_overlap);
           //memcpy (right->qscore, right->qscore + best_overlap,  n - best_overlap);

           right->data[n   - best_overlap] = 0;
           right->qscore[n - best_overlap] = 0;
         }
        else
         {
           return (0);
         }
      }
   }     /* run-through case */
  else
   {
     asm_len = best_overlap;
     
     /* compute uncalled */
     for (j = 0; j < asm_len; ++ j)
       if ((left->data[j] == 'N' || left->data[j] == 'n') && (right->data[n - best_overlap + j] == 'N' || right->data[n - best_overlap + j] == 'n')) ++uncalled;
     uncalled /= asm_len;

     if (asm_len >= sw->min_overlap && asm_len >= sw->min_asm_len && asm_len <= sw->max_asm_len && uncalled <= sw->max_uncalled)
      {
        assemble_overlap (left, right, 0, n - best_overlap, best_overlap, left);
        
        left->data[best_overlap]   = 0;
        left->qscore[best_overlap] = 0;
        *(left->data - 1) = 0;   /* flag that it's one piece */
      }
     else
      {
        return (0);
      }
   }

  /* TODO: Remember to reset *(let->data - 1) */

  /* TODO: Optimize this in assemble_overlap? */

  return (1);
}

inline int
assembly (struct read_t * left, struct read_t * right, int match_score, int mismatch_score, struct user_args  * sw)
{
  int                   i,j;
  int                   n;
  double                score;
  double                best_score = 0;
  int                   best_overlap = 0;       /* overlap of the best alignment */
  int                   run_through = 0;
  int                   nMatch;
  int                   asm_len = 0;
  int                   st_pass;
  int                   uncalled = 0;
  
  n = strlen (left->data);
     
  /* compute score for every overlap */
  score = 0;
  for (i = 0; i <= n; ++ i)    /* the size of the overlap */
   {     
     nMatch = 0;
     score = 0;
     for (j = 0; j < i; ++ j)
      {
        scoring (left->data[n - i + j], right->data[j], left->qscore[n - i + j], right->qscore[j], sw->score_method, &score, match_score, mismatch_score);
        if (left->data[n - i + j] == right->data[j]) ++nMatch;
      }
     if (score > best_score)
      {
        best_overlap = i;
        best_score   = score;
      }
   }

  /* compute for score for runthrough case */
  for (i = n - 1; i > 0; --i)
   {
     score  = 0;
     nMatch = 0;
     for (j = 0; j < i; ++j)
      {
        scoring (left->data[j], right->data[n - i + j], left->qscore[j], right->qscore[n - i + j], sw->score_method, &score, match_score, mismatch_score);
        if (left->data[n - i + j] == right->data[j]) ++nMatch;
      }

     if (score > best_score)
      {
        run_through  = 1;
        best_overlap = i;
        best_score   = score;
      }
   }


  if (sw->test == 1)
   {
     st_pass = stat_test2 (sw->p_value, best_score, sw->min_overlap, 0.25);
   }
  else
   {
     st_pass = stat_test2 (sw->p_value, best_score, best_overlap, 0.25);
   }

  if (!st_pass) return (0);


  /* do the assembly!!!! */
  if (!run_through)
   {
     if (best_overlap == 0)
      {
        asm_len = 2 * n;

        for (j = 0; j < n; ++ j)
          if (left->data[j] == 'N' || left->data[j] == 'n')  ++uncalled;
        for (j = 0; j < n; ++ j)
          if (right->data[j] == 'N' || right->data[j] == 'n')  ++uncalled;
        uncalled /= asm_len;

        if (2 * n - asm_len >= sw->min_overlap && asm_len >= sw->min_asm_len && asm_len <= sw->max_asm_len && uncalled <= sw->max_uncalled)
         {
           *(left->data - 1) = 1;
         }
        else
         {
           return (0);
         }
      }
     else if (best_overlap == n )
      {
        asm_len         = n;

        for (j = 0; j < asm_len; ++ j)
          if ((left->data[j] == 'N' || left->data[j] == 'n') && (right->data[j] == 'N' || right->data[j] == 'n')) ++uncalled;
        uncalled /= asm_len;

        if (2 * n - asm_len >= sw->min_overlap && asm_len >= sw->min_asm_len && asm_len <= sw->max_asm_len && uncalled <= sw->max_uncalled)
         {
           *(left->data - 1) = 0;
           assemble_overlap (left, right, 0, 0, n, left);
           left->data[n]   = 0;
           left->qscore[n] = 0;
         }
        else
         {
           return (0);
         }
      }
     else
      {
        asm_len = 2 * n - best_overlap;
        for (j = 0; j < n - best_overlap; ++ j)
          if (left->data[j] == 'N' || left->data[j] == 'n')  ++uncalled;
        for (j = n - best_overlap; j < n; ++ j)
          if ((left->data[j] == 'N' || left->data[j] == 'n') && (right->data[j - n + best_overlap] == 'N' || right->data[j - n + best_overlap] == 'n'))  ++uncalled;
        for (j = best_overlap; j < n; ++ j)
          if (right->data[j] == 'N' || right->data[j] == 'n')  ++uncalled;
        uncalled /= asm_len;

        if (2 * n - asm_len >= sw->min_overlap && asm_len >= sw->min_asm_len && asm_len <= sw->max_asm_len && uncalled <= sw->max_uncalled)
         {
           *(left->data - 1) = 1;
           
           assemble_overlap (left, right, n - best_overlap, 0, best_overlap, left);
           memmove (right->data,   right->data   + best_overlap,  n - best_overlap);
           memmove (right->qscore, right->qscore + best_overlap,  n - best_overlap);
           /* THIS IS WRONG */
           //memcpy (right->data,   right->data   + best_overlap,  n - best_overlap);
           //memcpy (right->qscore, right->qscore + best_overlap,  n - best_overlap);

           right->data[n   - best_overlap] = 0;
           right->qscore[n - best_overlap] = 0;
         }
        else
         {
           return (0);
         }
      }
   }     /* run-through case */
  else
   {
     asm_len = best_overlap;
     
     /* compute uncalled */
     for (j = 0; j < asm_len; ++ j)
       if ((left->data[j] == 'N' || left->data[j] == 'n') && (right->data[n - best_overlap + j] == 'N' || right->data[n - best_overlap + j] == 'n')) ++uncalled;
     uncalled /= asm_len;

     if (asm_len >= sw->min_overlap && asm_len >= sw->min_asm_len && asm_len <= sw->max_asm_len && uncalled <= sw->max_uncalled)
      {
        assemble_overlap (left, right, 0, n - best_overlap, best_overlap, left);
        
        left->data[best_overlap]   = 0;
        left->qscore[best_overlap] = 0;
        *(left->data - 1) = 0;   /* flag that it's one piece */
      }
     else
      {
        return (0);
      }
   }

  /* TODO: Remember to reset *(let->data - 1) */

  /* TODO: Optimize this in assemble_overlap? */

  return (1);
}

double
assemble_overlap (struct read_t * left, struct read_t * right, int base_left, int base_right, int ol_size, struct read_t * ai)
{
  int           i; 
  char          x, y;
  char          qx, qy;
  //double        exp_match  = 0;

  for (i = 0; i < ol_size; ++i)
   {
     x  = left->data[base_left + i]; 
     y  = right->data[base_right + i];
     qx = left->qscore[base_left + i];
     qy = right->qscore[base_right + i];
     if ( (x == 'N' || x == 'n') && (y == 'N' || y == 'n'))
      {
        //exp_match += 0.25; sm_len
        ai->data[base_left + i]          = 'N';
        ai->qscore[base_left + i] = ( qx < qy ) ? qx : qy;
      }
     else if (x == 'N' || x == 'n')
      {
        //exp_match += 0.25; 
        ai->data[base_left + i]          = y;
        ai->qscore[base_left + i] = qy;
      }
     else if (y == 'N' || y == 'n')
      {
        //exp_match += 0.25; 
        ai->data[base_left + i]          = x;
        ai->qscore[base_left + i] = qx;
      }
     else
      {
        if (x == y)
         {
           //exp_match += (sc_eq[(int)qx][(int)qy] / match_score);
           
           ai->data[base_left + i] = x;
           ai->qscore[base_left + i] = (right->qscore[base_right + i] - PHRED_INIT) + (left->qscore[base_left + i] - PHRED_INIT) + PHRED_INIT; //qs_mul[qx][qy];
         }
        else
         {
           //exp_match += (1 - sc_neq[(int)qx][(int)qy] / mismatch_score);
           
           if (qx > qy)
            {
              ai->data[base_left + i]          =  x;
              ai->qscore[base_left + i] = qx;
            }
           else
            {
              ai->data[base_left + i]          =  y;
              ai->qscore[base_left + i] = qy;
            }
         }
      }
   }
  //if (ol_size == 0) return (0);
  //return (exp_match / (double)ol_size);
  return (0);
}

/*
double
assemble_overlap_ef (struct reads_info * left, struct reads_info * right, int base_left, int base_right, int ol_size, struct asm_info * ai, struct emp_freq  * ef)
{
  int           i; 
  char          x, y;
  char          qx, qy;
  double        exp_match  = 0;

  for (i = 0; i < ol_size; ++i)
   {
     x  = left->data[base_left + i]; 
     y  = right->data[base_right + i];
     qx = left->qscore[base_left + i];
     qy = right->qscore[base_right + i];
     if ( (x == 'N' || x == 'n') && (y == 'N' || y == 'n'))
      {
        exp_match += ef->q; 
        ai->data[base_left + i]          = 'N';
        ai->qscore[base_left + i] = ( qx < qy ) ? qx : qy;
      }
     else if (x == 'N' || x == 'n')
      {
        exp_match += ef->q; 
        ai->data[base_left + i]          = y;
        ai->qscore[base_left + i] = qy;
      }
     else if (y == 'N' || y == 'n')
      {
        exp_match += ef->q; 
        ai->data[base_left + i]          = x;
        ai->qscore[base_left + i] = qx;
      }
     else
      {
        if (x == y)
         {
           //exp_match += (sc_eq[(int)qx][(int)qy] / match_score);
           switch (x)
           {
             case 'A':
               exp_match += (sc_eqA[(int)qy][(int)qx] / match_score);
               break;
             case 'C':
               exp_match += (sc_eqC[(int)qy][(int)qx] / match_score);
               break;
             case 'G':
               exp_match += (sc_eqG[(int)qy][(int)qx] / match_score);
               break;
             case 'T':
               exp_match += (sc_eqT[(int)qy][(int)qx] / match_score);
               break;
           }
           ai->data[base_left + i] = x;
           ai->qscore[base_left + i] = (right->qscore[base_right + i] - PHRED_INIT) + (left->qscore[base_left + i] - PHRED_INIT) + PHRED_INIT; //qs_mul[qx][qy];
         }
        else
         {
           //exp_match += (1 - sc_neq[(int)qx][(int)qy] / mismatch_score);
           switch  (x)
           {
             case 'A':
               switch (y)
                {
                  case 'C':
                    exp_match += (1 - sc_neqAC[(int)qx][(int)qy]/mismatch_score);
                    break;
                  case 'G':
                    exp_match += (1 - sc_neqAG[(int)qx][(int)qy]/mismatch_score);
                    break;
                  case 'T':
                    exp_match += (1 - sc_neqAT[(int)qx][(int)qy]/mismatch_score);
                    break;
                }
               break;
             case 'C':
               switch (y)
                {
                  case 'A':
                    exp_match += (1 - sc_neqCA[(int)qx][(int)qy]/mismatch_score);
                    break;
                  case 'G':
                    exp_match += (1 - sc_neqCG[(int)qx][(int)qy]/mismatch_score);
                    break;
                  case 'T':
                    exp_match += (1 - sc_neqCT[(int)qx][(int)qy]/mismatch_score);
                    break;
                }
               break;
             case 'G':
               switch (y)
                {
                  case 'A':
                    exp_match += (1 - sc_neqGA[(int)qx][(int)qy]/mismatch_score);
                    break;
                  case 'C':
                    exp_match += (1 - sc_neqGC[(int)qx][(int)qy]/mismatch_score);
                    break;
                  case 'T':
                    exp_match += (1 - sc_neqGT[(int)qx][(int)qy]/mismatch_score);
                    break;
                }
               break;
             case 'T':
               switch (y)
                {
                  case 'A':
                    exp_match += (1 - sc_neqTA[(int)qx][(int)qy]/mismatch_score);
                    break;
                  case 'C':
                    exp_match += (1 - sc_neqTC[(int)qx][(int)qy]/mismatch_score);
                    break;
                  case 'G':
                    exp_match += (1 - sc_neqTG[(int)qx][(int)qy]/mismatch_score);
                    break;
                }
               break;
           }

           if (qx > qy)
            {
              ai->data[base_left + i]          =  x;
              ai->qscore[base_left + i] = qx;
            }
           else
            {
              ai->data[base_left + i]          =  y;
              ai->qscore[base_left + i] = qy;
            }
         }
      }
   }
  if (ol_size == 0) return (0);
  return (exp_match / (double)ol_size);
}
*/

void mstrrev (char * s)
{
  char * q = s;

  while (q && *q) ++q;

  for (--q; s < q; ++s, --q)
   {
     *s = *s ^ *q;
     *q = *s ^ *q;
     *s = *s ^ *q;
   }
}

void mstrcpl (char * s)
{
  if (!s) return;

  while (*s)
   {
     switch (*s)
      {
        case 'A':
        case 'a':
                  *s = 'T';
                  break;
        case 'C':
        case 'c':
                  *s = 'G';
                  break;
        case 'G':
        case 'g':
                  *s = 'C';
                  break;
        case 'T':
        case 't':
                  *s = 'A';
                  break;
      }
     ++s;
   }
}


int 
validate_input (int nleft, int nright)
{
  if (!nleft || !nright)
   {
     fprintf (stderr, "ERROR: At least one of the input files contains no records.\n");
     return (0);
   }

  if (nleft != nright)
   {
     fprintf (stderr, "ERROR: Number of records in the two input files does not match.\n");
     return (0);
   }

  return (1);
}

char *
makefilename (const char * prefix, const char * suffix)
{
  char * filename;

  filename = (char *) malloc ((strlen(prefix) + strlen(suffix) + 1) * sizeof (char));

  strcpy (filename, prefix);
  strcat (filename, suffix);

  return (filename);
}

void write_data (struct read_t ** fwd, struct read_t ** rev, unsigned int elms, FILE ** fd)
{
  int i;
  char two_piece;
  
  for ( i = 0; i < elms; ++ i)
   {
     two_piece = *(fwd[i]->data - 1);
     *(fwd[i]->data - 1) = 0;

     if (*(fwd[i]->qscore - 1) == 1)   /* assembled */
      {
        *(fwd[i]->qscore - 1) = 0;
        fprintf (fd[0], "%s\n", fwd[i]->header);
        if (!two_piece)
         {
           fprintf (fd[0], "%s\n", fwd[i]->data);
         }
        else
         {
           fprintf (fd[0], "%s",   fwd[i]->data);
           fprintf (fd[0], "%s\n", rev[i]->data);
         }
        fprintf (fd[0], "+\n");

        if (!two_piece)
         {
           fprintf (fd[0], "%s\n", fwd[i]->qscore);
         }
        else
         {
           fprintf (fd[0], "%s",   fwd[i]->qscore);
           fprintf (fd[0], "%s\n", rev[i]->qscore);
         }
      }
     else if (*(fwd[i]->qscore - 1) == 2)                                            /* not assembled */
      {
        *(fwd[i]->qscore - 1) = 0;
           /* discarded reads*/
           /* Maybe consider printing the untrimmed sequences */
           fprintf (fd[3], "%s\n", fwd[i]->header);
           fprintf (fd[3], "%s\n+\n%s\n", fwd[i]->data,  fwd[i]->qscore);
           fprintf (fd[3], "%s\n", rev[i]->header);
           fprintf (fd[3], "%s\n+\n%s\n", rev[i]->data, rev[i]->qscore); /* printing the reverse compliment of the original sequence */
      }
     else   /* unassembled reads*/
      {
        *(fwd[i]->qscore - 1) = 0;
           fprintf (fd[1], "%s\n", fwd[i]->header);
           fprintf (fd[2], "%s\n", rev[i]->header);
           fprintf (fd[1], "%s\n+\n%s\n", fwd[i]->data,  fwd[i]->qscore);
           fprintf (fd[2], "%s\n+\n%s\n", rev[i]->data, rev[i]->qscore); /* printing the reverse compliment of the original sequence */
      }
   }
}

void flip_list (void)
{
  struct blockinfo_t * elm1;
  struct blockinfo_t * elm2;

  elm1 = thr_global.xblock;
  elm2 = thr_global.yblock;

  thr_global.xblock = elm2;
  thr_global.yblock = elm1;
}

inline int assign_reads (struct blockinfo_t * block, struct thread_local_t * thr_data)
{
  int r;

  if (block->reads == block->processed) return (0);

  r = THREAD_MIN_PACKET_SIZE + (rand() % THREAD_MIN_PACKET_SIZE);
  thr_data->block   = block;
  thr_data->start   = block->processed;

  if (block->reads - block->processed <= r + THREAD_PACKET_SIZE_DELTA)
   {
     thr_data->end    = block->reads;
     block->processed = block->reads;
   }
  else
   {
     thr_data->end    = thr_data->start + r;
     block->processed = thr_data->start + r;
   }
  return (1);
}

void * entry_point (void * data)
{
  struct thread_local_t * thr_local;
  int ass, i, sleep, elms;
  double                uncalled_forward, uncalled_reverse;

  /* initialization of values */
  thr_local = (struct thread_local_t *)data;

  
  while (1)
   {
     // TODO: The first condition in the next line is useless?
     if (thr_local->block == thr_global.xblock && thr_global.io_thread == thr_local->id)
      {
        pthread_mutex_lock (&cs_mutex_io);
        write_data (thr_global.xblock->fwd->reads, thr_global.xblock->rev->reads, thr_global.xblock->reads, thr_global.fd);
        // TODO: read_data ();
        elms = db_get_next_reads (thr_global.xblock->fwd, 
                                  thr_global.xblock->rev, 
                                  thr_global.yblock->fwd, 
                                  thr_global.yblock->rev);
          //read_size = strlen (fwd_block.reads[0]->data);
        thr_global.io_thread      = -1;
        pthread_mutex_unlock (&cs_mutex_io);

        pthread_mutex_lock (&cs_mutex_wnd);
        thr_global.xblock->reads     = elms;
        thr_global.xblock->processed = 0;
        if (!elms) thr_global.finish  = 1;
        flip_list ();
        pthread_mutex_unlock (&cs_mutex_wnd);

        // wakeup threads
        pthread_cond_broadcast (&cs_mutex_cond);
      }

     sleep = 1;

     pthread_mutex_lock (&cs_mutex_wnd);
     if (thr_global.xblock->reads == thr_global.xblock->processed)
      {
        if (thr_global.finish)
         {
           pthread_mutex_unlock (&cs_mutex_wnd);
           break;
         }
        /* is this the last thread using the current buffer? */
        if (thr_global.xblock->threads == 0 && thr_global.io_thread != -1)
         {
           thr_global.io_thread = thr_local->id;
           sleep = 0;
         }
        else
         {
           if (assign_reads (thr_global. yblock, thr_local)) sleep = 0;
         }
      }
     else
      {
        if (assign_reads (thr_global.xblock,thr_local)) sleep = 0;
      }
     if (sleep)
      {
        pthread_cond_wait (&cs_mutex_cond, &cs_mutex_wnd);
      }
     pthread_mutex_unlock (&cs_mutex_wnd);

     if (!sleep && thr_global.io_thread != thr_local->id)
      {
        // TODO: Process reads and make this an inline function
        for (i = thr_local->start; i < thr_local->end; ++ i)
         {
           mstrrev (thr_local->block->rev->reads[i]->data);    /* reverse the sequence */
           mstrcpl (thr_local->block->rev->reads[i]->data);    /* complement the sequence */
           mstrrev (thr_local->block->rev->reads[i]->qscore);  /* reverse the quality scores */

           //TODO Switch for empirical frequencies
           ass = assembly (thr_local->block->fwd->reads[i], thr_local->block->rev->reads[i], thr_local->match_score, thr_local->mismatch_score, thr_local->sw);
           *(thr_local->block->fwd->reads[i]->qscore - 1) = ass;
           if (!ass)
            {
              if (trim     (thr_local->block->fwd->reads[i], thr_local->sw->qual_thres, 
                        strlen(thr_local->block->fwd->reads[i]->data), &uncalled_forward) < thr_local->sw->min_trim_len ||
                  trim_cpl (thr_local->block->rev->reads[i], thr_local->sw->qual_thres,
                        strlen(thr_local->block->rev->reads[i]->data), &uncalled_reverse) < thr_local->sw->min_trim_len ||
                  uncalled_forward >= thr_local->sw->max_uncalled || uncalled_reverse >= thr_local->sw->max_uncalled)
               {
                 *(thr_local->block->fwd->reads[i]->qscore - 1) = 2;
               }
              else
               {
                 *(thr_local->block->fwd->reads[i]->qscore - 1) = 3;
               }
            }
         }

        pthread_mutex_lock (&cs_mutex_wnd);
          -- thr_local->block->threads;
        pthread_mutex_unlock (&cs_mutex_wnd);
      }
   }
  return (NULL);
}

void init_thr_global (void)
{
  thr_global.xblock = (struct blockinfo_t *) malloc (sizeof(struct blockinfo_t));
  thr_global.yblock = (struct blockinfo_t *) malloc (sizeof(struct blockinfo_t));
  thr_global.xblock->fwd = (struct block_t *) malloc (sizeof(struct block_t));
  thr_global.xblock->rev = (struct block_t *) malloc (sizeof(struct block_t));
  thr_global.yblock->fwd = (struct block_t *) malloc (sizeof(struct block_t));
  thr_global.yblock->rev = (struct block_t *) malloc (sizeof(struct block_t));

  thr_global.xblock->reads     = 0;
  thr_global.xblock->processed = 0;
  thr_global.xblock->threads   = 0;
  thr_global.yblock->reads     = 0;
  thr_global.yblock->processed = 0;
  thr_global.yblock->threads   = 0;

  thr_global.io_thread = -1;
  thr_global.finish = 0;
}

void destroy_thr_global (void)
{
  free (thr_global.xblock->fwd);
  free (thr_global.xblock->rev);
  free (thr_global.yblock->fwd);
  free (thr_global.yblock->rev);
  free (thr_global.xblock);
  free (thr_global.yblock);
}

int 
main (int argc, char * argv[])
{
  int                   i;
  //struct reads_info  ** ri_left;
  //struct reads_info  ** ri_right;
//  int                   cnt_reads_left;
//  int                   cnt_reads_right;
  char                * out[4];
  struct user_args      sw;
  struct emp_freq * ef;
  struct thread_local_t * thr_data;
  pthread_t * tid;

  /* parse command-line arguments */
  if (!decode_switches (argc, argv, &sw))
   {
     /* TODO: Validate reads names */
     usage ();
     return (EXIT_FAILURE);
   }

  ef = (struct emp_freq *)malloc (sizeof(struct emp_freq));
  ef->freqa = ef->freqc = ef->freqg = ef->freqt = ef->total = ef->pa = ef->pc = ef->pg = ef->pt = ef->q = 0.25;

  init_scores(match_score, mismatch_score, ef);
  /* read the two fastq files containing the left-end and right-end reads */
  //ri_left  = read_fastq(sw.fastq_left,  &cnt_reads_left);
  //ri_right = read_fastq(sw.fastq_right, &cnt_reads_right);

  //if (!validate_input (cnt_reads_left, cnt_reads_right))
  // {
  //   return (EXIT_FAILURE);
  // }

  // read_size = strlen (ri_left[0]->data);

  /* TODO: THIS IS WRONG!!!! TO GET EMPIRICAL FREQUENCIES WE NEED TO READ THE WHOLE FILE :-( */
  //ef = get_emp_freq (cnt_reads_right, read_size, fwd_block.reads, rev_block.reads);

  /* reverse the right ends */

  /* allocate memory for the assembled results */
  /*
  ai = (struct asm_info *) malloc (cnt_reads_left * sizeof(struct asm_info));
  for (i = 0; i < cnt_reads_left; ++i)
   {
     ai[i].data          = (char *) malloc ((2 * read_size + 1) * sizeof(char));
     ai[i].quality_score = (char *) malloc ((2 * read_size + 1) * sizeof(char));
   }
  */
  
  init_thr_global ();
  thr_data = (struct thread_local_t *) calloc (sw.threads, sizeof (struct thread_local_t));
  tid      = (pthread_t *) malloc (sw.threads * sizeof (pthread_t));

  //init_fastq_reader_double_buffer (sw.fastq_left, sw.fastq_right, sw.memory, &fwd_block, &rev_block, &dbfwd_block, &dbrev_block);
  init_fastq_reader_double_buffer (sw.fastq_left, 
                                   sw.fastq_right, 
                                   sw.memory, 
                                   thr_global.xblock->fwd, 
                                   thr_global.xblock->rev, 
                                   thr_global.yblock->fwd, 
                                   thr_global.yblock->rev);
  //init_fastq_reader (sw.fastq_left, sw.fastq_right, 100000000, &fwd_block, &rev_block);

  /* construct output file names */
  out[0] = makefilename (sw.outfile, ".assembled.fastq");
  out[1] = makefilename (sw.outfile, ".unassembled.forward.fastq");
  out[2] = makefilename (sw.outfile, ".unassembled.reverse.fastq");
  out[3] = makefilename (sw.outfile, ".discarded.fastq");

  thr_global.fd[0] = fopen (out[0], "w");
  thr_global.fd[1] = fopen (out[1], "w");
  thr_global.fd[2] = fopen (out[2], "w");
  thr_global.fd[3] = fopen (out[3], "w");

  //omp_set_num_threads (sw.threads);
  
  // Read block
  //db_get_next_reads (&fwd_block, &rev_block, &dbfwd_block, &dbrev_block);
  db_get_next_reads (thr_global.yblock->fwd, thr_global.yblock->rev, 
                     thr_global.xblock->fwd, thr_global.xblock->rev);


  /* pthreads entry point */
  for (i = 0; i < sw.threads; ++ i)
   {
     thr_data[i].block  = thr_global.xblock;
     thr_data[i].id     = i;
     pthread_create (&tid[i], NULL, entry_point, (void *)&thr_data[i]); 
   }

  free (ef);
  free (out[0]);
  free (out[1]);
  free (out[2]);
  free (out[3]);

  destroy_reader ();

  /* TODO: Fix those file closings */
  fclose (thr_global.fd[0]);
  fclose (thr_global.fd[1]);
  fclose (thr_global.fd[2]);
  fclose (thr_global.fd[3]);
   
  destroy_thr_global ();
  
  return (EXIT_SUCCESS);
}
