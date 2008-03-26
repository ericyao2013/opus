/* (C) 2007-2008 Jean-Marc Valin, CSIRO
*/
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:
   
   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
   
   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
   
   - Neither the name of the Xiph.org Foundation nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.
   
   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mathops.h"
#include "cwrs.h"
#include "vq.h"
#include "arch.h"
#include "os_support.h"

/** Takes the pitch vector and the decoded residual vector, computes the gain
    that will give ||p+g*y||=1 and mixes the residual with the pitch. */
static void mix_pitch_and_residual(int * restrict iy, celt_norm_t * restrict X, int N, int K, const celt_norm_t * restrict P)
{
   int i;
   celt_word32_t Ryp, Ryy, Rpp;
   celt_word32_t g;
   VARDECL(celt_norm_t, y);
#ifdef FIXED_POINT
   int yshift;
#endif
   SAVE_STACK;
#ifdef FIXED_POINT
   yshift = 14-EC_ILOG(K);
#endif
   ALLOC(y, N, celt_norm_t);

   /*for (i=0;i<N;i++)
   printf ("%d ", iy[i]);*/
   Rpp = 0;
   for (i=0;i<N;i++)
      Rpp = MAC16_16(Rpp,P[i],P[i]);

   for (i=0;i<N;i++)
      y[i] = SHL16(iy[i],yshift);
   
   Ryp = 0;
   Ryy = 0;
   /* If this doesn't generate a dual MAC (on supported archs), fire the compiler guy */
   for (i=0;i<N;i++)
   {
      Ryp = MAC16_16(Ryp, y[i], P[i]);
      Ryy = MAC16_16(Ryy, y[i], y[i]);
   }
   
   /* g = (sqrt(Ryp^2 + Ryy - Rpp*Ryy)-Ryp)/Ryy */
   g = MULT16_32_Q15(
            celt_sqrt(MULT16_16(ROUND16(Ryp,14),ROUND16(Ryp,14)) + Ryy -
                      MULT16_16(ROUND16(Ryy,14),ROUND16(Rpp,14)))
            - ROUND16(Ryp,14),
       celt_rcp(SHR32(Ryy,9)));

   for (i=0;i<N;i++)
      X[i] = P[i] + ROUND16(MULT16_16(y[i], g),11);
   RESTORE_STACK;
}


void alg_quant(celt_norm_t *X, celt_mask_t *W, int N, int K, const celt_norm_t *P, ec_enc *enc)
{
   VARDECL(celt_norm_t, y);
   VARDECL(int, iy);
   VARDECL(int, signx);
   int i, j, is;
   celt_word16_t s;
   int pulsesLeft;
   celt_word32_t sum;
   celt_word32_t xy, yy, yp;
   celt_word16_t Rpp;
#ifdef FIXED_POINT
   int yshift;
#endif
   SAVE_STACK;

#ifdef FIXED_POINT
   yshift = 14-EC_ILOG(K);
#endif

   ALLOC(y, N, celt_norm_t);
   ALLOC(iy, N, int);
   ALLOC(signx, N, int);

   for (j=0;j<N;j++)
   {
      if (X[j]>0)
         signx[j]=1;
      else
         signx[j]=-1;
   }
   
   sum = 0;
   for (j=0;j<N;j++)
   {
      sum = MAC16_16(sum, P[j],P[j]);
   }
   Rpp = ROUND16(sum, NORM_SHIFT);

   celt_assert2(Rpp<=NORM_SCALING, "Rpp should never have a norm greater than unity");

   for (i=0;i<N;i++)
      y[i] = 0;
   for (i=0;i<N;i++)
      iy[i] = 0;
   xy = yy = yp = 0;

   pulsesLeft = K;
   while (pulsesLeft > 0)
   {
      int pulsesAtOnce=1;
      int sign;
      celt_word32_t Rxy, Ryy, Ryp;
      celt_word32_t g;
      celt_word32_t best_num;
      celt_word16_t best_den;
      int best_id;
      
      /* Decide on how many pulses to find at once */
      pulsesAtOnce = pulsesLeft/N;
      if (pulsesAtOnce<1)
         pulsesAtOnce = 1;

      /* This should ensure that anything we can process will have a better score */
      best_num = -SHR32(VERY_LARGE32,4);
      best_den = 0;
      best_id = 0;
      /* Choose between fast and accurate strategy depending on where we are in the search */
      if (pulsesLeft>1)
      {
         for (j=0;j<N;j++)
         {
            celt_word32_t num;
            celt_word16_t den;
            /* Select sign based on X[j] alone */
            sign = signx[j];
            s = SHL16(sign*pulsesAtOnce, yshift);
            /* Temporary sums of the new pulse(s) */
            Rxy = xy + MULT16_16(s,X[j]);
            Ryy = yy + 2*MULT16_16(s,y[j]) + MULT16_16(s,s);
            
            /* Approximate score: we maximise Rxy/sqrt(Ryy) */
            num = MULT16_16(ROUND16(Rxy,14),ABS16(ROUND16(Rxy,14)));
            den = ROUND16(Ryy,14);
            /* The idea is to check for num/den >= best_num/best_den, but that way
               we can do it without any division */
            if (MULT16_32_Q15(best_den, num) > MULT16_32_Q15(den, best_num))
            {
               best_den = den;
               best_num = num;
               best_id = j;
            }
         }
      } else {
         for (j=0;j<N;j++)
         {
            celt_word32_t num;
            /* Select sign based on X[j] alone */
            sign = signx[j];
            s = SHL16(sign*pulsesAtOnce, yshift);
            /* Temporary sums of the new pulse(s) */
            Rxy = xy + MULT16_16(s,X[j]);
            Ryy = yy + 2*MULT16_16(s,y[j]) + MULT16_16(s,s);
            Ryp = yp + MULT16_16(s, P[j]);

            /* Compute the gain such that ||p + g*y|| = 1 */
            g = MULT16_32_Q15(
                     celt_sqrt(MULT16_16(ROUND16(Ryp,14),ROUND16(Ryp,14)) + Ryy -
                               MULT16_16(ROUND16(Ryy,14),Rpp))
                     - ROUND16(Ryp,14),
                celt_rcp(SHR32(Ryy,12)));
            /* Knowing that gain, what's the error: (x-g*y)^2 
               (result is negated and we discard x^2 because it's constant) */
            /* score = 2.f*g*Rxy - 1.f*g*g*Ryy*NORM_SCALING_1;*/
            num = 2*MULT16_32_Q14(ROUND16(Rxy,14),g)
                  - MULT16_32_Q14(EXTRACT16(MULT16_32_Q14(ROUND16(Ryy,14),g)),g);
            if (num >= best_num)
            {
               best_num = num;
               best_id = j;
            } 
         }
      }
      
      j = best_id;
      is = signx[j]*pulsesAtOnce;
      s = SHL16(is, yshift);

      /* Updating the sums of the new pulse(s) */
      xy = xy + MULT16_16(s,X[j]);
      yy = yy + 2*MULT16_16(s,y[j]) + MULT16_16(s,s);
      yp = yp + MULT16_16(s, P[j]);

      /* Only now that we've made the final choice, update y/iy */
      y[j] += s;
      iy[j] += is;
      pulsesLeft -= pulsesAtOnce;
   }
   
   encode_pulses(iy, N, K, enc);
   
   /* Recompute the gain in one pass to reduce the encoder-decoder mismatch
   due to the recursive computation used in quantisation. */
   mix_pitch_and_residual(iy, X, N, K, P);
   RESTORE_STACK;
}


/** Decode pulse vector and combine the result with the pitch vector to produce
    the final normalised signal in the current band. */
void alg_unquant(celt_norm_t *X, int N, int K, celt_norm_t *P, ec_dec *dec)
{
   VARDECL(int, iy);
   SAVE_STACK;
   ALLOC(iy, N, int);
   decode_pulses(iy, N, K, dec);
   mix_pitch_and_residual(iy, X, N, K, P);
   RESTORE_STACK;
}

#ifdef FIXED_POINT
static const celt_word16_t pg[11] = {32767, 24576, 21299, 19661, 19661, 19661, 18022, 18022, 16384, 16384, 16384};
#else
static const celt_word16_t pg[11] = {1.f, .75f, .65f, 0.6f, 0.6f, .6f, .55f, .55f, .5f, .5f, .5f};
#endif

#define MAX_INTRA 32
#define LOG_MAX_INTRA 5
      
void intra_prediction(celt_norm_t *x, celt_mask_t *W, int N, int K, celt_norm_t *Y, celt_norm_t * restrict P, int B, int N0, ec_enc *enc)
{
   int i,j;
   int best=0;
   celt_word32_t best_num=-SHR32(VERY_LARGE32,4);
   celt_word16_t best_den=0;
   celt_word16_t s = 1;
   int sign;
   celt_word32_t E;
   celt_word16_t pred_gain;
   int max_pos = N0-N/B;
   if (max_pos > MAX_INTRA)
      max_pos = MAX_INTRA;

   for (i=0;i<max_pos*B;i+=B)
   {
      celt_word32_t xy=0, yy=0;
      celt_word32_t num;
      celt_word16_t den;
      /* If this doesn't generate a double-MAC on supported architectures, 
         complain to your compilor vendor */
      for (j=0;j<N;j++)
      {
         xy = MAC16_16(xy, x[j], Y[i+N-j-1]);
         yy = MAC16_16(yy, Y[i+N-j-1], Y[i+N-j-1]);
      }
      /* Using xy^2/yy as the score but without having to do the division */
      num = MULT16_16(ROUND16(xy,14),ROUND16(xy,14));
      den = ROUND16(yy,14);
      /* If you're really desperate for speed, just use xy as the score */
      if (MULT16_32_Q15(best_den, num) >  MULT16_32_Q15(den, best_num))
      {
         best_num = num;
         best_den = den;
         best = i;
         /* Store xy as the sign. We'll normalise it to +/- 1 later. */
         s = ROUND16(xy,14);
      }
   }
   if (s<0)
   {
      s = -1;
      sign = 1;
   } else {
      s = 1;
      sign = 0;
   }
   /*printf ("%d %d ", sign, best);*/
   ec_enc_bits(enc,sign,1);
   if (max_pos == MAX_INTRA)
      ec_enc_bits(enc,best/B,LOG_MAX_INTRA);
   else
      ec_enc_uint(enc,best/B,max_pos);

   /*printf ("%d %f\n", best, best_score);*/
   
   if (K>10)
      pred_gain = pg[10];
   else
      pred_gain = pg[K];
   E = EPSILON;
   for (j=0;j<N;j++)
   {
      P[j] = s*Y[best+N-j-1];
      E = MAC16_16(E, P[j],P[j]);
   }
   /*pred_gain = pred_gain/sqrt(E);*/
   pred_gain = MULT16_16_Q15(pred_gain,celt_rcp(SHL32(celt_sqrt(E),9)));
   for (j=0;j<N;j++)
      P[j] = PSHR32(MULT16_16(pred_gain, P[j]),8);
   if (K>0)
   {
      for (j=0;j<N;j++)
         x[j] -= P[j];
   } else {
      for (j=0;j<N;j++)
         x[j] = P[j];
   }
   /*printf ("quant ");*/
   /*for (j=0;j<N;j++) printf ("%f ", P[j]);*/

}

void intra_unquant(celt_norm_t *x, int N, int K, celt_norm_t *Y, celt_norm_t * restrict P, int B, int N0, ec_dec *dec)
{
   int j;
   int sign;
   celt_word16_t s;
   int best;
   celt_word32_t E;
   celt_word16_t pred_gain;
   int max_pos = N0-N/B;
   if (max_pos > MAX_INTRA)
      max_pos = MAX_INTRA;
   
   sign = ec_dec_bits(dec, 1);
   if (sign == 0)
      s = 1;
   else
      s = -1;
   
   if (max_pos == MAX_INTRA)
      best = B*ec_dec_bits(dec, LOG_MAX_INTRA);
   else
      best = B*ec_dec_uint(dec, max_pos);
   /*printf ("%d %d ", sign, best);*/

   if (K>10)
      pred_gain = pg[10];
   else
      pred_gain = pg[K];
   E = EPSILON;
   for (j=0;j<N;j++)
   {
      P[j] = s*Y[best+N-j-1];
      E = MAC16_16(E, P[j],P[j]);
   }
   /*pred_gain = pred_gain/sqrt(E);*/
   pred_gain = MULT16_16_Q15(pred_gain,celt_rcp(SHL32(celt_sqrt(E),9)));
   for (j=0;j<N;j++)
      P[j] = PSHR32(MULT16_16(pred_gain, P[j]),8);
   if (K==0)
   {
      for (j=0;j<N;j++)
         x[j] = P[j];
   }
}

void intra_fold(celt_norm_t *x, int N, celt_norm_t *Y, celt_norm_t * restrict P, int B, int N0, int Nmax)
{
   int i, j;
   celt_word32_t E;
   celt_word16_t g;
   
   E = EPSILON;
   if (N0 >= (Nmax>>1))
   {
      for (i=0;i<B;i++)
      {
         for (j=0;j<N/B;j++)
         {
            P[j*B+i] = Y[(Nmax-N0-j-1)*B+i];
            E += P[j*B+i]*P[j*B+i];
         }
      }
   } else {
      for (j=0;j<N;j++)
      {
         P[j] = Y[j];
         E = MAC16_16(E, P[j],P[j]);
      }
   }
   g = celt_rcp(SHL32(celt_sqrt(E),9));
   for (j=0;j<N;j++)
      P[j] = PSHR32(MULT16_16(g, P[j]),8);
   for (j=0;j<N;j++)
      x[j] = P[j];
}

