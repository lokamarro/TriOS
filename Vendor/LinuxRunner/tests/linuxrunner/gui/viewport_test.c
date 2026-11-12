#include "app/LinuxRunner/RFBProtocol.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void){uint16_t x,y;
assert(lr_rfb_viewport_point(150,200,0,87.5,300,225,1024,768,&x,&y)&&x==512&&y==384);
assert(lr_rfb_viewport_point(300,250,-200,-50,600,450,1024,768,&x,&y)&&x==853&&y==512);
assert(lr_rfb_viewport_point(450,300,0,-37.5,900,675,1024,768,&x,&y)&&x==512&&y==384);
assert(lr_rfb_viewport_point(-100,-100,0,0,300,225,1024,768,&x,&y)&&x==0&&y==0);
assert(lr_rfb_viewport_point(500,500,0,0,300,225,1024,768,&x,&y)&&x==1023&&y==767);
assert(!lr_rfb_viewport_point(NAN,0,0,0,300,225,1024,768,&x,&y));
assert(!lr_rfb_viewport_point(0,0,0,0,0,225,1024,768,&x,&y));
assert(!lr_rfb_viewport_point(0,0,0,0,300,225,0,768,&x,&y));
printf("VIEWPORT_TRANSFORM_PASS fit portrait landscape zoom pan clamp invalid\n");return 0;}
