#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
static float bf(uint32_t u){float f;memcpy(&f,&u,4);return f;}
static uint32_t fb(float f){uint32_t u;memcpy(&u,&f,4);return u;}
unsigned R[5][10000][12]; int H[5][10000];
static int kid(char c){return c=='M'?0:c=='T'?1:c=='P'?2:3;}
static float PI_=3.14159265359f;
static float normA(float a){ while(a>PI_) a-=2*PI_; while(a<=-PI_) a+=2*PI_; return a;}
static float Cos_(float a){return (float)cos((double)a);} static float Sin_(float a){return (float)sin((double)a);}
static float At2(float y,float x){return (float)atan2((double)y,(double)x);}
typedef struct {float x,y,z,vx,vy,vz,ang,m00,m01,m10,m11; int brkStatus;} S;
float GX,GY; int VERB=1; int dBRK=0,dACC=0,dTURN=0,dMINV=0,dFUDGE=0,dLF=0;
int NOBRAKE=-1; float OGX0=0,OGY0=0; int GOX=0,GOY=0; int FORCE_LOCO=-1, FORCE_IDLE=-1, FORCE_BRK=-1, FORCE_BRKV=0; float FGX,FGY;
static void step(S*s,int k){
  if(H[0][k]){ GX=bf(R[0][k][2]); GY=bf(R[0][k][3]); if(GX==OGX0&&GY==OGY0){ GX=bf(fb(GX)+GOX); GY=bf(fb(GY)+GOY);} }
  if(k==FORCE_LOCO){ GX=FGX; GY=FGY; }
  float maxSpeed=bf(0x40200001), braking=100.0f*((1.0f/60.0f)*(1.0f/60.0f)), maxAcc=60.0f*((1.0f/60.0f)*(1.0f/60.0f));
  float SPF=1.0f/60.0f; float SQ=SPF*SPF; braking=bf(fb(100.0f*SQ)+dBRK); maxAcc=bf(fb(60.0f*SQ)+dACC);
  float mass=50.0f;
  int loco=H[0][k]; if(k==FORCE_IDLE) loco=0; if(k==FORCE_LOCO){ loco=1; GX=FGX; GY=FGY; }
  float ax=0,ay=0,az=0;
  float dx=GX-s->x, dy=GY-s->y; float dist=sqrtf(dx*dx+dy*dy);
  if(!loco){
    /* maintainCurrentPositionHover: brake to a stop along the current heading */
    float SPF2=1.0f/60.0f, SQ2=SPF2*SPF2; float brk2=bf(fb(100.0f*SQ2)+dBRK), macc2=bf(fb(60.0f*SQ2)+dACC); float mass2=50.0f;
    float d0x=Cos_(s->ang), d0y=Sin_(s->ang); float q1=s->vx*d0x, q2=s->vy*d0y; float qd=q1+q2; float qs=sqrtf(q1*q1+q2*q2); float act=qd>=0?qs:-qs;
    float minSpeed=1.0E-10f; float sdl=minSpeed-act;
    if(k!=NOBRAKE && fabsf(sdl)>minSpeed){ float acc=sdl>0?macc2:-brk2; float af=mass2*acc; float mf=mass2*sdl; if(fabsf(af)>fabsf(mf)) af=mf; float fx=af*d0x, fy=af*d0y; float mi=1.0f/mass2; ax+=fx*mi; ay+=fy*mi; }
    s->brkStatus=(k==FORCE_BRK)?FORCE_BRKV:H[4][k]; goto physics; }
  float onPath=dist;
  int wasBraking=s->brkStatus;
  /* moveTowardsPositionOther */
  float desired=bf(0x40200001); if(desired>maxSpeed) desired=maxSpeed; float goalSpeed=desired;
  float dirx=Cos_(s->ang), diry=Sin_(s->ang);
  float vxx=s->vx*dirx, vyy=s->vy*diry; float dot=vxx+vyy; float sp=sqrtf(vxx*vxx+vyy*vyy); float actual=dot>=0?sp:-sp;
  float fdx=dirx, fdy=diry;
  /* rotate */
  float desA=At2(GY-s->y, GX-s->x); float amount=normA(desA-s->ang); float mt=bf(0x3D567A0E);
  float RPD=3.14159265359f/180.0f; float maxTurn=bf(fb(180.0f*(SPF*RPD))+dTURN);
  (void)mt;
  if(amount>maxTurn) amount=maxTurn; else if(amount<-maxTurn) amount=-maxTurn;
  float na=normA(s->ang+amount); float c=Cos_(na), sn=Sin_(na);
  s->m00=c; s->m01=-sn; s->m10=sn; s->m11=c; s->ang=normA(na);
  float delta=actual; float slow=0; if(delta>0){ slow=((delta*delta)/fabsf(braking))*0.5f; slow=slow*bf(fb(1.05f)+dFUDGE); }
  if(onPath<slow) goalSpeed=0;
  float sd=goalSpeed-actual;
  if(sd!=0){ float acc=sd>0?maxAcc:-braking; float af=mass*acc; float mf=mass*sd; if(fabsf(af)>fabsf(mf)) af=mf; float fx=af*fdx, fy=af*fdy; float mi=1.0f/mass; ax+=fx*mi; ay+=fy*mi; }
  if(VERB) printf("  k=%d onPath=%08X slow=%08X actual=%08X ang=%08X desA=%08X\n",k,fb(onPath),fb(slow),fb(actual),fb(s->ang),fb(desA));
  /* z: ignore (constant hover); status */
  int brkNow=(k==FORCE_BRK)?FORCE_BRKV:(int)H[4][k]; s->brkStatus=brkNow;
  if(wasBraking){
    float MINV=bf(fb(10.0f/(float)60)+dMINV); float vel=fabsf(0); /* forward speed with new dir */
    float d2x=Cos_(s->ang), d2y=Sin_(s->ang); float a1=s->vx*d2x, a2=s->vy*d2y; float dt=a1+a2; float sp2=sqrtf(a1*a1+a2*a2); vel=fabsf(dt>=0?sp2:-sp2);
    if(dist>0.001f){ if(vel<MINV) vel=MINV; if(vel>dist) vel=dist; float id=1.0f/dist; float ndx=dx*id, ndy=dy*id; s->x+=ndx*vel; s->y+=ndy*vel; }
  }
  physics: ;
  /* physics */
  float pdx=Cos_(s->ang), pdy=Sin_(s->ang);
  if(s->vx!=0||s->vy!=0){ float ld=s->vx*(-pdy)+s->vy*pdx; float lvx=ld*-pdy, lvy=ld*pdx; float lf=mass*bf(fb(0.15f)+dLF); float fx=-(lf*lvx), fy=-(lf*lvy);
    /* applyForce while motive: lateral projection */
    float ld2=fx*(-pdy)+fy*pdx; float mx=ld2*-pdy, my=ld2*pdx; float mi=1.0f/mass; ax+=mx*mi; ay+=my*mi; }
  s->vx+=ax; s->vy+=ay;
  if(VERB) printf("  k=%d accel=%08X %08X vel=%08X %08X\n",k,fb(ax),fb(ay),fb(s->vx),fb(s->vy));
  if(!s->brkStatus){ s->x+=s->vx; s->y+=s->vy; }
  /* setTransformMatrix -> cached angle */
  s->ang=At2(s->m10,s->m00);
}
static uint32_t rol(uint32_t c){return (c<<1)|(c>>31);} static uint32_t ror(uint32_t c){return (c>>1)|(c<<31);}
static uint32_t bsw(uint32_t x){return __builtin_bswap32(x);}
uint32_t Cpre,Btarget; unsigned MW[12]; uint32_t Cpre2,Btarget2; unsigned MW2[12];
static int crcOK2(S*s){ unsigned m[12]; memcpy(m,MW2,sizeof m); m[0]=fb(s->m00); m[1]=fb(s->m01); m[3]=fb(s->x); m[4]=fb(s->m10); m[5]=fb(s->m11); m[7]=fb(s->y);
  uint32_t c=Cpre2; for(int i=0;i<12;i++) c=rol(c)+bsw(m[i]); return c==Btarget2; }
static int crcOK(S*s){ unsigned m[12]; memcpy(m,MW,sizeof m); m[0]=fb(s->m00); m[1]=fb(s->m01); m[3]=fb(s->x); m[4]=fb(s->m10); m[5]=fb(s->m11); m[7]=fb(s->y);
  uint32_t c=Cpre; for(int i=0;i<12;i++) c=rol(c)+bsw(m[i]); return c==Btarget; }
int main(int argc,char**argv){
  FILE*f=fopen("mtpl321.txt","r"); int fr; char k; unsigned v[12];
  char line[400];
  while(fgets(line,sizeof line,f)){ int n=sscanf(line,"%d %c %x %x %x %x %x %x %x %x %x %x %x %x",&fr,&k,&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],&v[8],&v[9],&v[10],&v[11]); if(n<4) continue; memcpy(R[kid(k)][fr],v,sizeof v); H[kid(k)][fr]=1; if(k=='P') H[4][fr]=(bf(v[11])!=0);}
  int k0=atoi(argv[1]), k1=atoi(argv[2]);
  S s; unsigned*M=R[0][k0]; unsigned*T=R[1][k0]; unsigned*P=R[2][k0-1];
  s.x=bf(M[0]); s.y=bf(M[1]); s.ang=bf(T[4]); s.vx=bf(P[5]); s.vy=bf(P[6]); s.brkStatus=H[4][k0-1];
  GX=bf(M[2]); GY=bf(M[3]);
  { char wn[64],mn[64]; sprintf(wn,"sc/w%d.bin",k1); sprintf(mn,"sc/w%d.meta",k1); FILE*wf=fopen(wn,"rb"); static uint32_t W[151711]; fread(W,4,151711,wf); FILE*mf=fopen(mn,"r"); int n; unsigned te; fscanf(mf,"%d %u",&n,&te);
    uint32_t c=0; for(int i=0;i<5;i++) c=rol(c)+W[i]; Cpre=c; uint32_t B=te; for(int i=n-1;i>=17;i--) B=ror(B-W[i]); Btarget=B; for(int i=0;i<12;i++) MW[i]=bsw(W[5+i]); }
  { FILE*wf=fopen("sc/w3700.bin","rb"); static uint32_t W[151711]; fread(W,4,151711,wf); FILE*mf=fopen("sc/w3700.meta","r"); int n; unsigned te; fscanf(mf,"%d %u",&n,&te);
    uint32_t c=0; for(int i=0;i<5;i++) c=rol(c)+W[i]; Cpre2=c; uint32_t B=te; for(int i=n-1;i>=17;i--) B=ror(B-W[i]); Btarget2=B; for(int i=0;i<12;i++) MW2[i]=bsw(W[5+i]); }
  if(argc>3 && argv[3][0]=='d'){ VERB=0; S st[400]; S cur=s; for(int kk=k0;kk<3700;kk++){ st[kk-k0]=cur; step(&cur,kk);} long tot=0;
    for(int kk=k0;kk<3600;kk++){ for(int a=-64;a<=64;a++) for(int b=-64;b<=64;b++){ for(int what=0;what<2;what++){ S t=st[kk-k0];
        if(what==0){ t.x=bf(fb(t.x)+a); t.y=bf(fb(t.y)+b);} else { if(t.vx==0&&t.vy==0) continue; t.vx=bf(fb(t.vx)+a); t.vy=bf(fb(t.vy)+b);} 
        int ok1=0; for(int j=kk;j<3700;j++){ if(j==3600) ok1=crcOK(&t); if(j==3600 && !ok1) break; step(&t,j);} 
        if(ok1 && crcOK2(&t)){ printf("DOUBLE HIT k=%d %s %d %d\n",kk,what?"vel":"pos",a,b); tot++; } } } }
    printf("total %ld\n",tot); return 0; }
  if(argc>3 && argv[3][0]=='g'){ VERB=0; S s0=s; OGX0=1540.5f; OGY0=690.5f; GOX=atoi(argv[4]); GOY=atoi(argv[5]); s=s0; for(int kk=k0;kk<k1;kk++) step(&s,kk); printf("goal offset %d %d -> crc %d  pos %08X %08X\n",GOX,GOY,crcOK(&s),fb(s.x),fb(s.y)); GOX=GOY=0; s=s0; for(int kk=k0;kk<k1;kk++) step(&s,kk); printf("baseline crc %d pos %08X %08X\n",crcOK(&s),fb(s.x),fb(s.y)); return 0; }
  if(argc>3 && argv[3][0]=='t'){ VERB=0; S s0=s;
    /* 1: extra approach step at 3592 toward the approach point */
    FGX=1540.5f; FGY=690.5f; FORCE_LOCO=3592; s=s0; for(int kk=k0;kk<k1;kk++) step(&s,kk); printf("extra loco 3592 toward approach: %d\n",crcOK(&s)); FORCE_LOCO=-1;
    /* 2: extra step at 3592 toward dock point */
    FGX=bf(0x44C050FB); FGY=bf(0x442D84D6); FORCE_LOCO=3592; s=s0; for(int kk=k0;kk<k1;kk++) step(&s,kk); printf("dock goal already at 3592: %d\n",crcOK(&s)); FORCE_LOCO=-1;
    /* 3: idle at 3591 */
    FORCE_IDLE=3591; s=s0; for(int kk=k0;kk<k1;kk++) step(&s,kk); printf("idle at 3591: %d\n",crcOK(&s)); FORCE_IDLE=-1;
    /* 4: idle at 3593 as well */
    FORCE_IDLE=3593; s=s0; for(int kk=k0;kk<k1;kk++) step(&s,kk); printf("idle at 3593: %d\n",crcOK(&s)); FORCE_IDLE=-1;
    { float gs[][2]={{1540.5f,690.5f},{1570.5f,660.5f},{bf(0x44C050FB),bf(0x442D84D6)}}; for(int g=0;g<3;g++){ OGX0=gs[g][0]; OGY0=gs[g][1]; int h=0;
      for(int a=-300;a<=300;a++) for(int b=-300;b<=300;b++){ GOX=a; GOY=b; s=s0; for(int kk=k0;kk<k1;kk++) step(&s,kk); if(crcOK(&s)){ printf("HIT goal%d %d %d\n",g,a,b); h++;} }
      GOX=GOY=0; printf("goal %d hits %d\n",g,h);} OGX0=OGY0=0; }
    NOBRAKE=3592; s=s0; for(int kk=k0;kk<k1;kk++) step(&s,kk); printf("no hover brake at 3592: %d\n",crcOK(&s)); NOBRAKE=-1;
    for(int fk=3565;fk<3600;fk++) for(int v=0;v<2;v++){ FORCE_BRK=fk; FORCE_BRKV=v; if(v==H[4][fk]) continue; s=s0; for(int kk=k0;kk<k1;kk++) step(&s,kk); if(crcOK(&s)) printf("HIT brk at %d = %d\n",fk,v);} FORCE_BRK=-1;
    return 0; }
  if(argc>3 && argv[3][0]=='k'){ VERB=0; S st[4000]; S cur=s; for(int kk=k0;kk<k1;kk++){ st[kk-k0]=cur; step(&cur,kk);} 
    for(int kk=k0;kk<k1;kk++){ int h=0; for(int a=-40;a<=40;a++) for(int b=-300;b<=300;b++){ S t=st[kk-k0]; t.x=bf(fb(t.x)+a); t.y=bf(fb(t.y)+b); for(int j=kk;j<k1;j++) step(&t,j); if(crcOK(&t)) h++; }
      int hv=0; for(int a=-60;a<=60;a++) for(int b=-60;b<=60;b++){ S t=st[kk-k0]; if(t.vx==0&&t.vy==0) continue; t.vx=bf(fb(t.vx)+a); t.vy=bf(fb(t.vy)+b); for(int j=kk;j<k1;j++) step(&t,j); if(crcOK(&t)) {hv++; if(hv<3) printf("   k=%d vel %d %d\n",kk,a,b);} }
      int ha=0; for(int a=-300;a<=300;a++){ S t=st[kk-k0]; t.ang=bf(fb(t.ang)+a); for(int j=kk;j<k1;j++) step(&t,j); if(crcOK(&t)) {ha++; printf("   k=%d ang %d\n",kk,a);} }
      printf("k=%d posHits=%d velHits=%d angHits=%d brk=%d loco=%d\n",kk,h,hv,ha,H[4][kk],H[0][kk]); }
    return 0; }
  if(argc>3){ VERB=0; S s0=s; float gx0=GX, gy0=GY; long hits=0;
    for(int a=-200;a<=200;a++) for(int b=-200;b<=200;b++){ s=s0; GX=bf(fb(gx0)+a); GY=bf(fb(gy0)+b); for(int kk=k0;kk<k1;kk++) step(&s,kk); if(crcOK(&s)){printf("HIT goal dx=%d dy=%d\n",a,b);hits++;} }
    GX=gx0; GY=gy0;
    for(int a=-100;a<=100;a++) for(int b=-100;b<=100;b++){ s=s0; s.vx=bf(fb(s0.vx)+a); s.vy=bf(fb(s0.vy)+b); for(int kk=k0;kk<k1;kk++) step(&s,kk); if(crcOK(&s)){printf("HIT vel dvx=%d dvy=%d\n",a,b);hits++;} }
    for(int a=-100;a<=100;a++) for(int b=-100;b<=100;b++){ s=s0; s.x=bf(fb(s0.x)+a); s.y=bf(fb(s0.y)+b); for(int kk=k0;kk<k1;kk++) step(&s,kk); if(crcOK(&s)){printf("HIT pos dx=%d dy=%d\n",a,b);hits++;} }
    for(int a=-2000;a<=2000;a++){ s=s0; s.ang=bf(fb(s0.ang)+a); for(int kk=k0;kk<k1;kk++) step(&s,kk); if(crcOK(&s)){printf("HIT ang %d\n",a);hits++;} }
    int *P[]={&dBRK,&dACC,&dTURN,&dMINV,&dFUDGE,&dLF}; const char*N[]={"brake","accel","turn","minvel","fudge","latfric"};
    for(int p=0;p<6;p++) for(int d=-64;d<=64;d++){ if(!d) continue; *P[p]=d; s=s0; for(int kk=k0;kk<k1;kk++) step(&s,kk); if(crcOK(&s)){printf("HIT %s %+d\n",N[p],d);hits++;} *P[p]=0; }
    printf("hits %ld\n",hits); return 0; }
  for(int kk=k0;kk<k1;kk++){
    unsigned*Mk=R[0][kk];
    printf("k=%d trace pos=%08X %08X ang(T)=%08X onPath=%08X slow=%08X actual=%08X | sim pos=%08X %08X ang=%08X\n",kk,Mk[0],Mk[1],R[1][kk][4],Mk[4],Mk[5],Mk[7],fb(s.x),fb(s.y),fb(s.ang));
    unsigned*Pk=R[2][kk]; printf("  trace accel=%08X %08X vel=%08X %08X brk=%d\n",Pk[1],Pk[2],Pk[5],Pk[6],H[4][kk]);
    step(&s,kk);
  }
  printf("end sim pos=%08X %08X m00=%08X m10=%08X\n",fb(s.x),fb(s.y),fb(s.m00),fb(s.m10));
}
