#include "ed_lf_features.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void){static float x[208][3]={{0}};ed_features_t f;int i;for(i=0;i<208;i++)x[i][0]=0.1f*sinf(2.f*3.14159265f*10.f*(float)i/208.f);assert(ed_lf_features_compute((const float (*)[3])x,208,208,&f)==0);assert(f.values[0]>0.06f&&f.values[0]<0.08f);assert(f.band_energy_g2[1]>f.band_energy_g2[0]);assert(f.values[8]==f.band_energy_g2[1]);assert(f.values[6]>1.f);memset(x,0,sizeof x);assert(ed_lf_features_compute((const float (*)[3])x,208,208,&f)==0);assert(f.values[0]==0.f);assert(ed_lf_features_compute((const float (*)[3])x,32,208,&f)<0);puts("low-frequency features: all tests passed");return 0;}
