/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "develop/imageop.h"
#include "common/opencl.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include "common/darktable.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include <memory.h>
#include <stdlib.h>
#include <string.h>

// we assume people have -msee support.
#include <xmmintrin.h>

DT_MODULE(2)

typedef struct dt_iop_demosaic_params_t
{
  uint32_t flags;
  float median_thrs;
}
dt_iop_demosaic_params_t;

typedef struct dt_iop_demosaic_gui_data_t
{
  GtkDarktableSlider *scale1;
  GtkToggleButton *greeneq;
}
dt_iop_demosaic_gui_data_t;

typedef struct dt_iop_demosaic_global_data_t
{
  // demosaic pattern
  int kernel_green_eq;
  int kernel_pre_median;
  int kernel_ppg_green;
  int kernel_ppg_green_median;
  int kernel_ppg_redblue;
  int kernel_zoom_half_size;
  int kernel_downsample;
}
dt_iop_demosaic_global_data_t;

typedef struct dt_iop_demosaic_data_t
{
  // demosaic pattern
  uint32_t filters;
  uint32_t flags;
  float median_thrs;
}
dt_iop_demosaic_data_t;

const char *
name()
{
  return _("demosaic");
}

int 
groups ()
{
  return IOP_GROUP_BASIC;
}

static int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

#define SWAP(a, b) {const float tmp = (b); (b) = (a); (a) = tmp;}

static void
pre_median_b(float *out, const float *const in, const dt_iop_roi_t *const roi_out, const dt_iop_roi_t *const roi_in, const int filters, const int num_passes, const float threshold, const int f4)
{
  const float thrs  = threshold;
  const float thrsc = 2*threshold;
  // colors:
  for (int pass=0; pass < num_passes; pass++)
  {
    for (int c=0; c < 3; c+=2)
    {
      int rows = 3;
      if(FC(rows,3,filters) != c && FC(rows,4,filters) != c) rows++;
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(rows,c,out) schedule(static)
#endif
      for (int row=rows;row<roi_out->height-3;row+=2)
      {
        float med[9];
        int col = 3;
        if(FC(row,col,filters) != c) col++;
        float *pixo = out + (f4?4:1)*(roi_out->width * row + col);
        const float *pixi = in + roi_in->width * row + col;
        for(;col<roi_out->width-3;col+=2)
        {
          int cnt = 0;
          for (int k=0, i = -2*roi_in->width; i <= 2*roi_in->width; i += 2*roi_in->width)
          {
            for (int j = i-2; j <= i+2; j+=2)
            {
              if(fabsf(pixi[j] - pixi[0]) < thrsc)
              {
                med[k++] = pixi[j];
                cnt ++;
              }
              else med[k++] = 1e7f+j;
            }
          }
          for (int i=0;i<8;i++) for(int ii=i+1;ii<9;ii++) if(med[i] > med[ii]) SWAP(med[i], med[ii]);
          pixo[f4?c:0] = med[(cnt-1)/2];
          pixo += f4?8:2;
          pixi += 2;
        }
      }
    }
  }

  // now green:
  const int lim[5] = {0, 1, 2, 1, 0};
  for (int pass=0; pass < num_passes; pass++)
  {
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out) schedule(static)
#endif
    for (int row=3;row<roi_out->height-3;row++)
    {
      float med[9];
      int col = 3;
      if(FC(row,col,filters) != 1 && FC(row,col,filters) != 3) col++;
      float *pixo = out + (f4?4:1)*(roi_out->width * row + col);
      const float *pixi = in + roi_in->width * row + col;
      for(;col<roi_out->width-3;col+=2)
      {
        int cnt = 0;
        for (int k=0, i = 0; i < 5; i ++)
        {
          for (int j = -lim[i]; j <= lim[i]; j+=2)
          {
            if(fabsf(pixi[roi_in->width*(i-2) + j] - pixi[0]) < thrs)
            {
              med[k++] = pixi[roi_in->width*(i-2) + j];
              cnt++;
            }
            else med[k++] = 1e7f+j;
          }
        }
        for (int i=0;i<8;i++) for(int ii=i+1;ii<9;ii++) if(med[i] > med[ii]) SWAP(med[i], med[ii]);
        pixo[f4?1:0] = med[(cnt-1)/2];
        pixo += f4?8:2;
        pixi += 2;
      }
    }
  }
}
#undef SWAP
static void
pre_median(float *out, const float *const in, const dt_iop_roi_t *const roi_out, const dt_iop_roi_t *const roi_in, const int filters, const int num_passes, const float threshold)
{
  pre_median_b(out, in, roi_out, roi_in, filters, num_passes, threshold, 1);
}

static void
green_equilibration(float *out, const float *const in, const int width, const int height, const uint32_t filters)
{
  int oj = 2, oi = 2;
  const float thr = 0.01f;
  const float maximum = 1.0f;
  if(FC(oj, oi, filters) != 1) oj++;
  if(FC(oj, oi, filters) != 1) oi++;
  if(FC(oj, oi, filters) != 1) oj--;

  memcpy(out,in,height*width*sizeof(float));

#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(out,oi,oj)
#endif
  for(int j=oj;j<height-2;j+=2)
  {
    for(int i=oi;i<width-2;i+=2)
    {
      const float o1_1 = in[(j-1)*width+i-1];
      const float o1_2 = in[(j-1)*width+i+1];
      const float o1_3 = in[(j+1)*width+i-1];
      const float o1_4 = in[(j+1)*width+i+1];
      const float o2_1 = in[(j-2)*width+i];
      const float o2_2 = in[(j+2)*width+i];
      const float o2_3 = in[j*width+i-2];
      const float o2_4 = in[j*width+i+2];

      const float m1 = (o1_1+o1_2+o1_3+o1_4)/4.0f;
      const float m2 = (o2_1+o2_2+o2_3+o2_4)/4.0f;
      if (m2 > 0.0f)
      {
        const float c1 = (fabsf(o1_1-o1_2)+fabsf(o1_1-o1_3)+fabsf(o1_1-o1_4)+fabsf(o1_2-o1_3)+fabsf(o1_3-o1_4)+fabsf(o1_2-o1_4))/6.0f;
        const float c2 = (fabsf(o2_1-o2_2)+fabsf(o2_1-o2_3)+fabsf(o2_1-o2_4)+fabsf(o2_2-o2_3)+fabsf(o2_3-o2_4)+fabsf(o2_2-o2_4))/6.0f;
        if((in[j*width+i]<maximum*0.95)&&(c1<maximum*thr)&&(c2<maximum*thr))
        {
          out[j*width+i] = out[j*width+i]*m1/m2;
        }
      }
    }
  }
}


/** 1:1 demosaic from in to out, in is full buf, out is translated/cropped (scale == 1.0!) */
static void
demosaic_ppg(float *out, const float *const in, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in, const int filters, const float thrs)
{
  // snap to start of mosaic block:
  roi_out->x = 0;//MAX(0, roi_out->x & ~1);
  roi_out->y = 0;//MAX(0, roi_out->y & ~1);
  // offsets only where the buffer ends:
  const int offx = 3; //MAX(0, 3 - roi_out->x);
  const int offy = 3; //MAX(0, 3 - roi_out->y);
  const int offX = 3; //MAX(0, 3 - (roi_in->width  - (roi_out->x + roi_out->width)));
  const int offY = 3; //MAX(0, 3 - (roi_in->height - (roi_out->y + roi_out->height)));

  // border interpolate
  float sum[8];
  for (int j=0; j < roi_out->height; j++) for (int i=0; i < roi_out->width; i++)
  {
    if (i == offx && j >= offy && j < roi_out->height-offY)
      i = roi_out->width-offX;
    if(i == roi_out->width) break;
    memset (sum, 0, sizeof(float)*8);
    for (int y=j-1; y != j+2; y++) for (int x=i-1; x != i+2; x++)
    {
      const int yy = y + roi_out->y, xx = x + roi_out->x;
      if (yy >= 0 && xx >= 0 && yy < roi_in->height && xx < roi_in->width)
      {
        int f = FC(y,x,filters);
        sum[f] += in[yy*roi_in->width+xx];
        sum[f+4]++;
      }
    }
    int f = FC(j,i,filters);
    for(int c=0;c<3;c++)
    {
      if (c != f && sum[c+4] > 0.0f)
        out[4*(j*roi_out->width+i)+c] = sum[c] / sum[c+4];
      else
        out[4*(j*roi_out->width+i)+c] = in[(j+roi_out->y)*roi_in->width+i+roi_out->x];
    }
  }
  const int median = thrs > 0.0f;
  // if(median) fbdd_green(out, in, roi_out, roi_in, filters);
  if(median) pre_median(out, in, roi_out, roi_in, filters, 1, thrs);
  // for all pixels: interpolate green into float array, or copy color.
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_in, roi_out, out) schedule(static)
#endif
  for (int j=offy; j < roi_out->height-offY; j++)
  {
    float *buf = out + 4*roi_out->width*j + 4*offx;
    const float *buf_in = in + roi_in->width*(j + roi_out->y) + offx + roi_out->x;
    for (int i=offx; i < roi_out->width-offX; i++)
    {
      const int c = FC(j,i,filters);
      // prefetch what we need soon (load to cpu caches)
      _mm_prefetch((char *)buf_in + 256, _MM_HINT_NTA); // TODO: try HINT_T0-3
      _mm_prefetch((char *)buf_in +   roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in + 2*roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in + 3*roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in -   roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in - 2*roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in - 3*roi_in->width + 256, _MM_HINT_NTA);
      __m128 col = _mm_load_ps(buf);
      float *color = (float*)&col;
      const float pc = buf_in[0];
      // if(__builtin_expect(c == 0 || c == 2, 1))
      if(c == 0 || c == 2)
      {
        if(!median) color[c] = pc; 
        // get stuff (hopefully from cache)
        const float pym  = buf_in[ - roi_in->width*1];
        const float pym2 = buf_in[ - roi_in->width*2];
        const float pym3 = buf_in[ - roi_in->width*3];
        const float pyM  = buf_in[ + roi_in->width*1];
        const float pyM2 = buf_in[ + roi_in->width*2];
        const float pyM3 = buf_in[ + roi_in->width*3];
        const float pxm  = buf_in[ - 1];
        const float pxm2 = buf_in[ - 2];
        const float pxm3 = buf_in[ - 3];
        const float pxM  = buf_in[ + 1];
        const float pxM2 = buf_in[ + 2];
        const float pxM3 = buf_in[ + 3];

        const float guessx = (pxm + pc + pxM) * 2.0f - pxM2 - pxm2;
        const float diffx  = (fabsf(pxm2 - pc) +
                              fabsf(pxM2 - pc) + 
                              fabsf(pxm  - pxM)) * 3.0f +
                             (fabsf(pxM3 - pxM) + fabsf(pxm3 - pxm)) * 2.0f;
        const float guessy = (pym + pc + pyM) * 2.0f - pyM2 - pym2;
        const float diffy  = (fabsf(pym2 - pc) +
                              fabsf(pyM2 - pc) + 
                              fabsf(pym  - pyM)) * 3.0f +
                             (fabsf(pyM3 - pyM) + fabsf(pym3 - pym)) * 2.0f;
        if(diffx > diffy)
        {
          // use guessy
          const float m = fminf(pym, pyM);
          const float M = fmaxf(pym, pyM);
          color[1] = fmaxf(fminf(guessy*.25f, M), m);
        }
        else
        {
          const float m = fminf(pxm, pxM);
          const float M = fmaxf(pxm, pxM);
          color[1] = fmaxf(fminf(guessx*.25f, M), m);
        }
      }
      else if(!median) color[1] = pc; 

      // write using MOVNTPS (write combine omitting caches)
      // _mm_stream_ps(buf, col);
      memcpy(buf, color, 4*sizeof(float));
      buf += 4;
      buf_in ++;
    }
  }
  // SFENCE (make sure stuff is stored now)
  // _mm_sfence();

  // for all pixels: interpolate colors into float array
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_in, roi_out, out) schedule(static)
#endif
  for (int j=1; j < roi_out->height-1; j++)
  {
    float *buf = out + 4*roi_out->width*j + 4;
    for (int i=1; i < roi_out->width-1; i++)
    {
      // also prefetch direct nbs top/bottom
      _mm_prefetch((char *)buf + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf - roi_out->width*4*sizeof(float) + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf + roi_out->width*4*sizeof(float) + 256, _MM_HINT_NTA);

      const int c = FC(j, i, filters);
      __m128 col = _mm_load_ps(buf);
      float *color = (float *)&col;
      // fill all four pixels with correctly interpolated stuff: r/b for green1/2
      // b for r and r for b
      if(__builtin_expect(c & 1, 1)) // c == 1 || c == 3)
      { // calculate red and blue for green pixels:
        // need 4-nbhood:
        const float* nt = buf - 4*roi_out->width;
        const float* nb = buf + 4*roi_out->width;
        const float* nl = buf - 4;
        const float* nr = buf + 4;
        if(FC(j, i+1, filters) == 0) // red nb in same row
        {
          color[2] = (nt[2] + nb[2] + 2.0f*color[1] - nt[1] - nb[1])*.5f;
          color[0] = (nl[0] + nr[0] + 2.0f*color[1] - nl[1] - nr[1])*.5f;
        }
        else
        { // blue nb
          color[0] = (nt[0] + nb[0] + 2.0f*color[1] - nt[1] - nb[1])*.5f;
          color[2] = (nl[2] + nr[2] + 2.0f*color[1] - nl[1] - nr[1])*.5f;
        }
      }
      else
      {
        // get 4-star-nbhood:
        const float* ntl = buf - 4 - 4*roi_out->width;
        const float* ntr = buf + 4 - 4*roi_out->width;
        const float* nbl = buf - 4 + 4*roi_out->width;
        const float* nbr = buf + 4 + 4*roi_out->width;

        if(c == 0)
        { // red pixel, fill blue:
          const float diff1  = fabsf(ntl[2] - nbr[2]) + fabsf(ntl[1] - color[1]) + fabsf(nbr[1] - color[1]);
          const float guess1 = ntl[2] + nbr[2] + 2.0f*color[1] - ntl[1] - nbr[1];
          const float diff2  = fabsf(ntr[2] - nbl[2]) + fabsf(ntr[1] - color[1]) + fabsf(nbl[1] - color[1]);
          const float guess2 = ntr[2] + nbl[2] + 2.0f*color[1] - ntr[1] - nbl[1];
          if     (diff1 > diff2) color[2] = guess2 * .5f;
          else if(diff1 < diff2) color[2] = guess1 * .5f;
          else color[2] = (guess1 + guess2)*.25f;
        }
        else // c == 2, blue pixel, fill red:
        {
          const float diff1  = fabsf(ntl[0] - nbr[0]) + fabsf(ntl[1] - color[1]) + fabsf(nbr[1] - color[1]);
          const float guess1 = ntl[0] + nbr[0] + 2.0f*color[1] - ntl[1] - nbr[1];
          const float diff2  = fabsf(ntr[0] - nbl[0]) + fabsf(ntr[1] - color[1]) + fabsf(nbl[1] - color[1]);
          const float guess2 = ntr[0] + nbl[0] + 2.0f*color[1] - ntr[1] - nbl[1];
          if     (diff1 > diff2) color[0] = guess2 * .5f;
          else if(diff1 < diff2) color[0] = guess1 * .5f;
          else color[0] = (guess1 + guess2)*.25f;
        }
      }
      // _mm_stream_ps(buf, col);
      memcpy(buf, color, 4*sizeof(float));
      buf += 4;
    }
  }
  // _mm_sfence();
}


// which roi input is needed to process to this output?
// roi_out is unchanged, full buffer in is full buffer out.
void
modify_roi_in (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  // this op is disabled for preview pipe/filters == 0

  *roi_in = *roi_out;
  // need 1:1, demosaic and then sub-sample. or directly sample half-size
  roi_in->x /= roi_out->scale;
  roi_in->y /= roi_out->scale;
  roi_in->width /= roi_out->scale;
  roi_in->height /= roi_out->scale;
  roi_in->scale = 1.0f;
  // clamp to even x/y, to make demosaic pattern still hold..
  roi_in->x = MAX(0, roi_in->x & ~1);
  roi_in->y = MAX(0, roi_in->y & ~1);

  // clamp numeric inaccuracies to full buffer, to avoid scaling/copying in pixelpipe:
  if(self->dev->image->width - roi_in->width < 10 && self->dev->image->height - roi_in->height < 10)
  {
    roi_in->width  = self->dev->image->width;
    roi_in->height = self->dev->image->height;
  }
}

void
process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_roi_t roi, roo;
  roi = *roi_in;
  roo = *roi_out;
  roo.x = roo.y = 0;
  // roi_out->scale = global scale: (iscale == 1.0, always when demosaic is on)

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;

  const float *const pixels = (float *)i;
  if(roi_out->scale > .999f)
  {
    // output 1:1
    // green eq:
    if(data->flags)
    {
      float *in = (float *)dt_alloc_align(16, roi_in->height*roi_in->width*sizeof(float));
      green_equilibration(in, pixels, roi_in->width, roi_in->height, data->filters);
      demosaic_ppg((float *)o, in, &roo, &roi, data->filters, data->median_thrs);
      free(in);
    }
    else demosaic_ppg((float *)o, pixels, &roo, &roi, data->filters, data->median_thrs);
  }
  else if(roi_out->scale > .5f)
  {
    // demosaic and then clip and zoom
    // roo.x = roi_out->x / global_scale;
    // roo.y = roi_out->y / global_scale;
    roo.width  = roi_out->width / roi_out->scale;
    roo.height = roi_out->height / roi_out->scale;
    roo.scale = 1.0f;
     
    float *tmp = (float *)dt_alloc_align(16, roo.width*roo.height*4*sizeof(float));
    if(data->flags)
    {
      float *in = (float *)dt_alloc_align(16, roi_in->height*roi_in->width*sizeof(float));
      green_equilibration(in, pixels, roi_in->width, roi_in->height, data->filters);
      demosaic_ppg(tmp, in, &roo, &roi, data->filters, data->median_thrs);
      free(in);
    }
    else demosaic_ppg(tmp, pixels, &roo, &roi, data->filters, data->median_thrs);
    roi = *roi_out;
    roi.x = roi.y = 0;
    roi.scale = roi_out->scale;
    dt_iop_clip_and_zoom((float *)o, tmp, &roi, &roo, roi.width, roo.width);
    free(tmp);
  }
  else
  {
    // sample half-size raw
    if(piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT && data->median_thrs > 0.0f)
    {
      float *tmp = (float *)dt_alloc_align(16, sizeof(float)*roi_in->width*roi_in->height);
      pre_median_b(tmp, pixels, roi_in, roi_in, data->filters, 1, data->median_thrs, 0);
      dt_iop_clip_and_zoom_demosaic_half_size_f((float *)o, tmp, &roo, &roi, roo.width, roi.width, data->filters);
      free(tmp);
    }
    else
      dt_iop_clip_and_zoom_demosaic_half_size_f((float *)o, pixels, &roo, &roi, roo.width, roi.width, data->filters);
  }
}

#ifdef HAVE_OPENCL
void
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
            const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->data;

  const int devid = piece->pipe->devid;
  size_t sizes[2] = {roi_out->width, roi_out->height};
  cl_mem dev_tmp = NULL;
  
  if(roi_out->scale > .99999f)
  {
    // 1:1 demosaic
    cl_mem dev_green_eq = NULL;
    if(data->flags)
    {
      // green equilibration
      dev_green_eq = dt_opencl_alloc_device(roi_in->width, roi_in->height, devid, sizeof(float));
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_green_eq, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_green_eq, 1, sizeof(cl_mem), &dev_green_eq);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_green_eq, 2, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_green_eq, sizes);
      dev_in = dev_green_eq;
    }
    if(data->median_thrs > 0.0f)
    {
      const int one = 1;
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 2, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 3, sizeof(float), (void*)&data->median_thrs);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 4, sizeof(uint32_t), (void*)&one);
      dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_pre_median, sizes);

      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green_median, 0, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green_median, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green_median, 2, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_green_median, sizes);
    }
    else
    {
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 2, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_green, sizes);
    }

    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 0, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 1, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 2, sizeof(uint32_t), (void*)&data->filters);
    dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_redblue, sizes);

    if(dev_green_eq) clReleaseMemObject(dev_green_eq);
  }
  else if(roi_out->scale > .5f)
  {
    // need to scale to right res
    dev_tmp = dt_opencl_alloc_device(roi_in->width, roi_in->height, devid, 4*sizeof(float));
    cl_mem dev_green_eq = NULL;
    if(data->flags)
    {
      // green equilibration
      dev_green_eq = dt_opencl_alloc_device(roi_in->width, roi_in->height, devid, sizeof(float));
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_green_eq, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_green_eq, 1, sizeof(cl_mem), &dev_green_eq);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_green_eq, 2, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_green_eq, sizes);
      dev_in = dev_green_eq;
    }

    sizes[0] = roi_in->width; sizes[1] = roi_in->height;
    if(data->median_thrs > 0.0f)
    {
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 2, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 3, sizeof(float), (void*)&data->median_thrs);
      dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_pre_median, sizes);

      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green_median, 0, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green_median, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green_median, 2, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_green_median, sizes);
    }
    else
    {
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 2, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_green, sizes);
    }
    if(dev_green_eq) clReleaseMemObject(dev_green_eq);

    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 0, sizeof(cl_mem), &dev_tmp);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 1, sizeof(cl_mem), &dev_tmp);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 2, sizeof(uint32_t), (void*)&data->filters);
    dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_redblue, sizes);

    // scale temp buffer to output buffer
    int zero = 0;
    sizes[0] = roi_out->width; sizes[1] = roi_out->height;
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 0, sizeof(cl_mem), &dev_tmp);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 1, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 2, sizeof(int), (void*)&zero);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 3, sizeof(int), (void*)&zero);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 4, sizeof(int), (void*)&roi_out->width);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 5, sizeof(int), (void*)&roi_out->height);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 6, sizeof(float), (void*)&roi_out->scale);
    dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_downsample, sizes);
  }
  else
  {
    // sample half-size image:
    int zero = 0;
    cl_mem dev_pix = dev_in;
    if(piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT && data->median_thrs > 0.0f)
    {
      sizes[0] = roi_in->width; sizes[1] = roi_in->height;
      dev_tmp = dt_opencl_alloc_device(roi_in->width, roi_in->height, devid, sizeof(float));
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 2, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 3, sizeof(float), (void*)&data->median_thrs);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_pre_median, 4, sizeof(uint32_t), (void*)&zero);
      dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_pre_median, sizes);
      dev_pix = dev_tmp;
      sizes[0] = roi_out->width; sizes[1] = roi_out->height;
    }

    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 0, sizeof(cl_mem), &dev_pix);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 1, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 2, sizeof(int), (void*)&zero);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 3, sizeof(int), (void*)&zero);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 4, sizeof(int), (void*)&roi_out->width);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 5, sizeof(int), (void*)&roi_out->height);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 6, sizeof(float), (void*)&roi_out->scale);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 7, sizeof(uint32_t), (void*)&data->filters);
    dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_zoom_half_size, sizes);
  }

  if(dev_tmp) clReleaseMemObject(dev_tmp);
}
#endif

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_demosaic_params_t));
  module->default_params = malloc(sizeof(dt_iop_demosaic_params_t));
  module->default_enabled = 1;
  module->priority = 240;
  module->hide_enable_button = 1;
  module->params_size = sizeof(dt_iop_demosaic_params_t);
  module->gui_data = NULL;
  dt_iop_demosaic_params_t tmp = (dt_iop_demosaic_params_t){0, 0.0f};
  memcpy(module->params, &tmp, sizeof(dt_iop_demosaic_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_demosaic_params_t));

  const int program = 0; // from programs.conf
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)malloc(sizeof(dt_iop_demosaic_global_data_t));
  module->data = gd;
  gd->kernel_zoom_half_size   = dt_opencl_create_kernel(darktable.opencl, program, "clip_and_zoom_demosaic_half_size");
  gd->kernel_ppg_green        = dt_opencl_create_kernel(darktable.opencl, program, "ppg_demosaic_green");
  gd->kernel_green_eq         = dt_opencl_create_kernel(darktable.opencl, program, "green_equilibration");
  gd->kernel_pre_median       = dt_opencl_create_kernel(darktable.opencl, program, "pre_median");
  gd->kernel_ppg_green_median = dt_opencl_create_kernel(darktable.opencl, program, "ppg_demosaic_green_median");
  gd->kernel_ppg_redblue      = dt_opencl_create_kernel(darktable.opencl, program, "ppg_demosaic_redblue");
  gd->kernel_downsample       = dt_opencl_create_kernel(darktable.opencl, program, "clip_and_zoom");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)module->data;
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_zoom_half_size);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_ppg_green);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_pre_median);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_green_eq);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_ppg_green_median);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_ppg_redblue);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_downsample);
  free(module->data);
  module->data = NULL;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)params;
  dt_iop_demosaic_data_t *d = (dt_iop_demosaic_data_t *)piece->data;
  d->filters = dt_image_flipped_filter(self->dev->image);
  if(!d->filters || pipe->type == DT_DEV_PIXELPIPE_PREVIEW) piece->enabled = 0;
  d->flags = p->flags;
  d->median_thrs = p->median_thrs;
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_demosaic_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update   (struct dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  dtgtk_slider_set_value(g->scale1, p->median_thrs);
  gtk_toggle_button_set_active(g->greeneq, p->flags);
}

static void
median_thrs_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  p->median_thrs = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
greeneq_callback (GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  p->flags = gtk_toggle_button_get_active(button);
  dt_dev_add_history_item(darktable.develop, self);
}

void gui_init     (struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_demosaic_gui_data_t));
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  GtkBox *vbox1 = GTK_BOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  GtkBox *vbox2 = GTK_BOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(vbox2), TRUE, TRUE, 5);

  GtkWidget *widget;
  widget = dtgtk_reset_label_new(_("edge threshold"), self, &p->median_thrs, sizeof(float));
  gtk_box_pack_start(vbox1, widget, TRUE, TRUE, 0);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0, 1.000, 0.001, p->median_thrs, 3));
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("threshold for edge-aware median.\nset to 0.0 to switch off.\nset to 1.0 to ignore edges."), (char *)NULL);
  gtk_box_pack_start(vbox2, GTK_WIDGET(g->scale1), TRUE, TRUE, 0);

  widget = dtgtk_reset_label_new(_("match greens"), self, &p->flags, sizeof(uint32_t));
  gtk_box_pack_start(vbox1, widget, TRUE, TRUE, 0);
  g->greeneq = GTK_TOGGLE_BUTTON(gtk_check_button_new());
  gtk_object_set(GTK_OBJECT(g->greeneq), "tooltip-text", _("switch on green equilibration before demosaicing.\nnecessary for some mid-range cameras such as the EOS 400D."), (char *)NULL);
  gtk_box_pack_start(vbox2, GTK_WIDGET(g->greeneq), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (median_thrs_callback), self);

  g_signal_connect (G_OBJECT (g->greeneq), "toggled",
                    G_CALLBACK (greeneq_callback), self);
}

void gui_cleanup  (struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

