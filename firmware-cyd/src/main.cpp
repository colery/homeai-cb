/*
 * Claude Buddy — CYD (ESP32-2432S028)
 *
 * Two board variants — select via compile flag:
 *   #undef  NEW_BOARD  → original CYD: 240×320 portrait,  setRotation(0)
 *   #define NEW_BOARD  → new CYD/BYD:  240×240 portrait,  setRotation(3)
 *                        (panel physically rotated 90°; rotation 3 corrects it,
 *                         logical height shrinks to 240 so layout is compressed)
 *
 * Dark sci-fi HUD aesthetic:
 *   Deep navy background · state-reactive neon accents · pulsing glow rings
 *   around the cat · corner brackets · glowing separator lines
 *
 * Flicker strategy:
 *   - Full-screen redraws only on state/tab changes
 *   - Data updates use updateBuddyText() (targeted small fills, no screen clear)
 *   - Cat + glow live in a TFT_eSprite → pushed atomically, zero blank-frame flash
 *   - startWrite/endWrite wraps every render path (single SPI transaction)
 *   - DMA enabled, SPI at 40 MHz
 */

#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_mac.h>
#include <time.h>

// ── Hardware ───────────────────────────────────────────────────────────────
#define LED_RED    4
#define LED_GREEN 16
#define LED_BLUE  17
#define BL_PIN    21
#define TOUCH_CS  33
#define TOUCH_IRQ 36
#define TOUCH_CLK 25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39

// ── NUS UUIDs ─────────────────────────────────────────────────────────────
#define NUS_SVC "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// ── Palette ────────────────────────────────────────────────────────────────
#define BG      0x0821
#define BG2     0x1042
#define BG3     0x0010
#define WHITE   0xFFFF
#define DIM1    0x2104
#define DIM2    0x4208
#define DIM3    0x630C
#define C_USB 0x07FF
#define C_BLE 0xFDA0

static const uint16_t ACC[] = { 0x4208, 0x07FF, 0x07E0, 0xFDA0 };
static const uint16_t ACC2[]= { 0x2104, 0x0398, 0x0260, 0x7940 };

// ── Board variant ──────────────────────────────────────────────────────────
// NEW_BOARD is defined via -DNEW_BOARD build flag in the [env:cyd_new]
// platformio environment. Do not hardcode it here.

#ifdef NEW_BOARD
  // ILI9342 driver (env:cyd_new), rotation 3 = portrait right-side-up.
  // TFT_WIDTH=320, TFT_HEIGHT=240 in platformio.ini; rotation 3 swaps to 240×320.
  // Full 320-row portrait layout identical to original board.
  #define SCR_W     240
  #define SCR_H     320
  #define SCR_ROT   3
  #define TS_ROT    0
  #define SPR_X     0
  #define SPR_Y     19
  #define SPR_W     240
  #define SPR_H     148
  #define GL1       18
  #define GL2       167
  #define GL3       193
  #define GL4       259
  #define GL5       291
  #define STAT_Y    168
  #define STAT_H    25
  #define FEED_Y    194
  #define FEED_N    5
  #define FEED_H    65
  #define FEED_DS   13
  #define SEC_Y     260
  #define SEC_H     31
  #define TAB_Y     292
  #define TAB_H     28
  #define LOG_N     50
  #define LOG_W     37
  #define LOG_VISIBLE 29
  #define SPR_CX    120
  #define SPR_CY    70
  #define SPR_A0    22
  #define SPR_AS    28
  #define SPR_LY    112
  #define SPR_TY    134
#else
  // 240×320 portrait (setRotation 0) — original CYD
  #define SCR_W     240
  #define SCR_H     320
  #define SCR_ROT   0
  #define TS_ROT    0
  #define SPR_X     0
  #define SPR_Y     19
  #define SPR_W     240
  #define SPR_H     148
  #define GL1       18
  #define GL2       167
  #define GL3       193
  #define GL4       259
  #define GL5       291
  #define STAT_Y    168
  #define STAT_H    25
  #define FEED_Y    194
  #define FEED_N    5
  #define FEED_H    65
  #define SEC_Y     260
  #define SEC_H     31
  #define TAB_Y     292
  #define TAB_H     28
  #define LOG_N     50
  #define LOG_W     37
  #define LOG_VISIBLE 29
  #define FEED_DS   13    // feed entry vertical spacing (px)
  #define SPR_CX    120
  #define SPR_CY    70
  #define SPR_A0    22
  #define SPR_AS    28
  #define SPR_LY    112
  #define SPR_TY    134
#endif

// ── Touch calibration ─────────────────────────────────────────────────────
#define TCH_X_MIN   230
#define TCH_X_MAX  3900
#define TCH_Y_MIN   230
#define TCH_Y_MAX  3900
#define TCH_SWAP_XY false
#define TCH_FLIP_X  false
#define TCH_FLIP_Y  false

static void mapTouch(int16_t rx, int16_t ry, int* sx, int* sy) {
  int x = TCH_FLIP_X ? (TCH_X_MAX-(rx-TCH_X_MIN)) : rx;
  int y = TCH_FLIP_Y ? (TCH_Y_MAX-(ry-TCH_Y_MIN)) : ry;
  if (TCH_SWAP_XY) { int t=x; x=y; y=t; }
  *sx = constrain(map(x,TCH_X_MIN,TCH_X_MAX,0,SCR_W-1),0,SCR_W-1);
  *sy = constrain(map(y,TCH_Y_MIN,TCH_Y_MAX,0,SCR_H-1),0,SCR_H-1);
}

// ── BLE ────────────────────────────────────────────────────────────────────
#define BLE_BUF 4096
static uint8_t           ring[BLE_BUF];
static volatile uint16_t rH=0,rT=0;
static BLECharacteristic* pTx=nullptr;
static volatile bool bleConn=false,bleSec=false,showKey=false;
static volatile uint32_t connAt=0,bleKey=0;
static char devName[20]="Claude-????";

static void rPush(const char* d,size_t n){for(size_t i=0;i<n;i++){uint16_t nx=(rH+1)%BLE_BUF;if(nx!=rT){ring[rH]=(uint8_t)d[i];rH=nx;}}}
static int  rAvail(){return(rH+BLE_BUF-rT)%BLE_BUF;}
static char rRead(){char c=(char)ring[rT];rT=(rT+1)%BLE_BUF;return c;}

// ── USB serial link (Claude Code, via the bridge hub) ─────────────────────
// Same newline-delimited JSON as BLE. Separate line buffer: the BLE ring has a
// second producer (the BLE task), so serial bytes must not go through it.
static char     sbuf[4096]; static int slen=0;
static uint32_t usbAt=0;                 // last valid JSON line seen on USB
static inline bool usbLive(){return usbAt&&(millis()-usbAt)<15000;}
static inline bool online(){return usbLive()||(bleConn&&bleSec);}
static void bleTx(const char* s){
  if(!bleConn||!bleSec||!pTx)return;
  char b[512];size_t n=snprintf(b,sizeof(b),"%s\n",s);
  for(size_t o=0;o<n;o+=20){size_t k=n-o<20?n-o:20;pTx->setValue((uint8_t*)(b+o),k);pTx->notify();}
}

class SecCB:public BLESecurityCallbacks{
  uint32_t onPassKeyRequest()override{return 0;}
  void onPassKeyNotify(uint32_t k)override{bleKey=k;showKey=true;}
  bool onConfirmPIN(uint32_t)override{return true;}
  bool onSecurityRequest()override{return true;}
  void onAuthenticationComplete(esp_ble_auth_cmpl_t c)override{if(c.success){bleSec=true;showKey=false;}}
};
class SrvCB:public BLEServerCallbacks{
  void onConnect(BLEServer*)override{bleConn=true;bleSec=false;showKey=false;connAt=millis();}
  void onDisconnect(BLEServer*)override{bleConn=false;bleSec=false;showKey=false;BLEDevice::startAdvertising();}
};
class RxCB:public BLECharacteristicCallbacks{
  void onWrite(BLECharacteristic* ch)override{std::string v=ch->getValue();rPush(v.c_str(),v.size());}
};

// ── State ──────────────────────────────────────────────────────────────────
struct Buddy{
  bool conn=false; uint8_t running=0,waiting=0;
  uint32_t tokToday=0,tokTotal=0,lastBeat=0;
  uint8_t runS[2]={},waitS[2]={}; uint32_t tokS[2]={}; uint8_t pSrc=0;   // per source: [0]=USB, [1]=BLE
  char msg[64]=""; char entries[5][52]={}; uint8_t nEntries=0;
  char pId[40]="",pTool[20]="",pHint[44]=""; bool pInfo=false;
  uint16_t approvals=0,denials=0;
  uint32_t epoch=0,epochAt=0; int32_t tzOff=0;
} g;
enum BS{S_SLEEP,S_IDLE,S_BUSY,S_ATTN};
BS gState=S_SLEEP;

// ── Log ────────────────────────────────────────────────────────────────────
static char log_buf[LOG_N][LOG_W+1];
static int logI=0,logScroll=0;
static void logLine(const char* s){strncpy(log_buf[logI],s,LOG_W);log_buf[logI][LOG_W]=0;logI=(logI+1)%LOG_N;}

// ── ASCII art ─────────────────────────────────────────────────────────────
static const char* ART[4][2][3]={
  {{"  /\\_/\\  ","  (-.-)z ","  > zzz< "},{"  /\\_/\\  ","  (-.-)Z ","  >z z z<"}},
  {{"  /\\_/\\  ","  (o . o)","   > ^ < "},{"  /\\_/\\  ","  (- . -)","   > - < "}},
  {{"  /\\_/\\  ","  (>.<)  ","  >[###]<"},{"  /\\_/\\  ","  (<.>)  ","  >[###]<"}},
  {{"  /\\_/\\  ","  (O . O)","  >! ! !<"},{"  /\\_/\\  ","  (O . O)","  > ! ! <"}},
};
static const char* S_LBL[]={"SLEEPING","IDLE","WORKING","APPROVE?"};

static const char* linkText(){
  if(usbLive())return (bleConn&&bleSec)?"USB+BLE linked":(bleConn?"USB linked, BLE pairing":"USB linked, BLE idle");
  if(bleConn)  return bleSec?"BLE  secured":"BLE  pairing";
  return "LINK offline";
}
static uint16_t linkCol(uint16_t col){
  if(online())return col;
  return bleConn?0x07FF:0xE000;
}
static void linkTx(const char* s){bleTx(s); if(usbLive())Serial.println(s);}

// ── Globals ───────────────────────────────────────────────────────────────
static char lbuf[4096]; static int llen=0;
static uint32_t lastLed=0; static uint8_t frame=0;
static bool ledOn=false; static uint32_t turnEnd=0;
static char dismissedId[40]="";

enum Tab{TAB_BUDDY=0,TAB_STATS=1,TAB_LOG=2};
static Tab curTab=TAB_BUDDY;
static bool dirty=true,textDirty=false;
struct SwipeTrk{int16_t sx,sy,ex,ey;uint32_t st;bool active,drag;BS startState;}swk;

TFT_eSPI    tft;
TFT_eSprite catSpr(&tft);
SPIClass    tspi(HSPI);
XPT2046_Touchscreen ts(TOUCH_CS,TOUCH_IRQ);

// ── Helpers ────────────────────────────────────────────────────────────────
static void fmtTok(char* o,size_t n,uint32_t t){
  if(t>=1000000)snprintf(o,n,"%.1fM",t/1e6f);
  else if(t>=1000)snprintf(o,n,"%.1fK",t/1e3f);
  else snprintf(o,n,"%lu",t);
}
static void getTime(char* o){
  if(!g.epoch){strcpy(o,"--:--");return;}
  uint32_t s=g.epoch+(millis()-g.epochAt)/1000+g.tzOff;
  snprintf(o,6,"%02lu:%02lu",(s%86400)/3600,(s%3600)/60);
}
static uint16_t dimC(uint16_t c,int div){
  if(div<1)div=1;
  uint16_t r=(c>>11)&0x1F, g=(c>>5)&0x3F, b=c&0x1F;
  return(uint16_t)(((r/div)<<11)|((g/div)<<5)|(b/div));
}
static void glowLine(int y,uint16_t col){
  tft.drawFastHLine(0,y,SCR_W,col);
  uint16_t d=dimC(col,4);
  tft.drawFastHLine(0,y-1,SCR_W,d);
  tft.drawFastHLine(0,y+1,SCR_W,d);
}
static void cornerBrkt(int x,int y,int w,int h,int sz,uint16_t col){
  tft.drawFastHLine(x,y,sz,col);   tft.drawFastVLine(x,y,sz,col);
  tft.drawFastHLine(x+w-sz,y,sz,col); tft.drawFastVLine(x+w-1,y,sz,col);
  tft.drawFastHLine(x,y+h-1,sz,col); tft.drawFastVLine(x,y+h-sz,sz,col);
  tft.drawFastHLine(x+w-sz,y+h-1,sz,col); tft.drawFastVLine(x+w-1,y+h-sz,sz,col);
}
static void spritGlow(TFT_eSprite* spr,int cx,int cy,uint16_t col,int pf){
  uint8_t r5=(col>>11)&0x1F, g6=(col>>5)&0x3F, b5=col&0x1F;
  for(int i=0;i<7;i++){
    int div=1<<(6-i);
    uint16_t rc=(uint16_t)(((r5/div)<<11)|((g6/div)<<5)|(b5/div));
    int rad=70-i*9+(pf?2:0);
    if(rad>0) spr->drawCircle(cx,cy,rad,rc);
  }
}

// ── State machine ──────────────────────────────────────────────────────────
// g.running/waiting/tokToday are the sums over sources. A source that is no longer
// linked stops counting toward running/waiting; the day's token total is kept.
static void sumSources(){
  bool on[2]={usbLive(),(bool)bleConn};
  g.running=g.waiting=0; g.tokToday=0;
  for(int i=0;i<2;i++){
    if(on[i]){g.running+=g.runS[i];g.waiting+=g.waitS[i];}
    g.tokToday+=g.tokS[i];
  }
}
static void recompute(){
  sumSources();
  if(!g.conn||(millis()-g.lastBeat)>15000) gState=S_SLEEP;
  else if(g.waiting>0) gState=S_ATTN;
  else if(g.running>0) gState=S_BUSY;
  else                  gState=S_IDLE;
}

// ── Cat sprite ────────────────────────────────────────────────────────────
static void pushCatSprite(){
  uint16_t col=ACC[gState];
  uint8_t  f=frame&1;
  catSpr.fillSprite(BG);

  cornerBrkt(0,0,SPR_W,SPR_H,14,dimC(col,2));
  cornerBrkt(2,2,SPR_W-4,SPR_H-4,10,dimC(col,4));

  spritGlow(&catSpr,SPR_CX,SPR_CY,col,f);

  catSpr.setTextSize(3); catSpr.setTextColor(col,BG);
  for(int i=0;i<3;i++){
    const char* ln=ART[gState][f][i];
    int tw=strlen(ln)*18;
    catSpr.setCursor((SPR_W-tw)/2, SPR_A0+i*SPR_AS);
    catSpr.print(ln);
  }

  catSpr.setTextSize(2); catSpr.setTextColor(col,BG);
  const char* lbl=S_LBL[gState];
  int lw=strlen(lbl)*12;
  catSpr.setCursor((SPR_W-lw)/2, SPR_LY);
  catSpr.print(lbl);

  char tally[16]; snprintf(tally,16,"ok:%u no:%u",g.approvals,g.denials);
  catSpr.setTextSize(1); catSpr.setTextColor(ACC2[gState],BG);
  catSpr.setCursor(SPR_W-strlen(tally)*6-4, SPR_TY);
  catSpr.print(tally);

  catSpr.pushSprite(SPR_X,SPR_Y);
}


// ── Top bar (y=0-17) ──────────────────────────────────────────────────────
static void drawTopBar(){
  tft.fillRect(0,0,SCR_W,18,BG2);
  tft.setTextSize(1);
  tft.setTextColor(ACC[gState],BG2); tft.setCursor(4,5); tft.print(devName);
  char tim[6]; getTime(tim);
  int tw=strlen(tim)*6; tft.setTextColor(DIM3,BG2); tft.setCursor((SCR_W-tw)/2,5); tft.print(tim);
  uint16_t dc=online()?ACC[gState]:(bleConn?0x07FF:DIM1);
  tft.setTextColor(online()?DIM3:DIM2,BG2); {bool both=usbLive()&&bleConn&&bleSec; const char* l=both?"USB+BLE":(usbLive()?"USB":"BLE"); tft.setCursor(SCR_W-16-4-(int)strlen(l)*6,5); tft.print(l);}
  tft.fillCircle(SCR_W-10,9,6,dc);
  tft.drawCircle(SCR_W-10,9,6,dimC(dc,2));
}

// ── Stat row ──────────────────────────────────────────────────────────────
static void drawStatRow(){
  tft.fillRect(0,STAT_Y,SCR_W,STAT_H,BG);
  uint16_t col=ACC[gState];
  int W3=SCR_W/3;

  tft.setTextSize(1);
  tft.setTextColor(g.running>0?col:DIM2,BG);
  tft.setCursor(4,STAT_Y+4); tft.print("r:");
  tft.setTextSize(2); tft.setCursor(16,STAT_Y+1);
  char rv[4]; snprintf(rv,4,"%u",g.running); tft.print(rv);
  tft.setTextSize(1); tft.setTextColor(DIM2,BG); tft.setCursor(30,STAT_Y+4); tft.print(" w:");
  tft.setTextColor(g.waiting>0?0xFDA0:DIM2,BG); tft.setTextSize(2); tft.setCursor(48,STAT_Y+1);
  char wv[4]; snprintf(wv,4,"%u",g.waiting); tft.print(wv);

  tft.drawFastVLine(W3,  STAT_Y,STAT_H,DIM1);
  tft.drawFastVLine(2*W3,STAT_Y,STAT_H,DIM1);

  // tokens today, split by source: USB (Claude Code) over BLE (Claude Desktop)
  {
    char n[10],t[16]; tft.setTextSize(1);
    fmtTok(n,10,g.tokS[0]); snprintf(t,16,"USB %s",n);
    tft.setTextColor(g.tokS[0]?C_USB:DIM2,BG); tft.setCursor(W3+(W3-(int)strlen(t)*6)/2,STAT_Y+4); tft.print(t);
    fmtTok(n,10,g.tokS[1]); snprintf(t,16,"BLE %s",n);
    tft.setTextColor(g.tokS[1]?C_BLE:DIM2,BG); tft.setCursor(W3+(W3-(int)strlen(t)*6)/2,STAT_Y+14); tft.print(t);
  }

  tft.setTextSize(1); tft.setTextColor(col,BG);
  int bw=strlen(S_LBL[gState])*6;
  tft.setCursor(2*W3+(W3-bw)/2,STAT_Y+4); tft.print(S_LBL[gState]);
  uint16_t dotcol=(frame&1)?col:dimC(col,3);
  tft.fillCircle(2*W3+W3/2,STAT_Y+STAT_H-7,3,dotcol);
}

// ── Activity feed ─────────────────────────────────────────────────────────
static void drawFeed(){
  tft.fillRect(0,FEED_Y,SCR_W,FEED_H,BG);
  static const uint16_t bc[]={WHITE,DIM3,DIM2,DIM1,BG};
  tft.setTextSize(1);
  for(int i=0;i<FEED_N;i++){
    if(i>=(int)g.nEntries) break;
    tft.setTextColor(bc[i],BG);
    tft.setCursor(4,FEED_Y+2+i*FEED_DS);
    tft.print("\x10 ");
    char e[36]; strncpy(e,g.entries[i],35); e[35]=0;
    tft.print(e);
  }
  if(!g.nEntries){
    tft.setTextColor(DIM1,BG); tft.setCursor(4,FEED_Y+FEED_H/2-4); tft.print("no activity yet");
  }
  if(g.msg[0]){
    tft.setTextColor(ACC2[gState],BG); tft.setCursor(4,FEED_Y+FEED_H-10);
    char tmp[36]; strncpy(tmp,g.msg,35); tmp[35]=0; tft.print(tmp);
  }
}

// ── Secondary stats ───────────────────────────────────────────────────────
static void drawSecondary(){
  tft.fillRect(0,SEC_Y,SCR_W,SEC_H,BG);
  uint16_t col=ACC[gState];
  int midY=SEC_Y+SEC_H/2;
  tft.setTextSize(1);

  uint32_t ba=g.lastBeat>0?(millis()-g.lastBeat)/1000:999;
  uint16_t bc2=ba>8?0xE000:(ba>4?0xFDA0:col);
  tft.fillCircle(8,midY,(frame&1)?4:3,bc2);
  char bts[14]; snprintf(bts,14,"%lus ago",ba);
  tft.setTextColor(bc2,BG); tft.setCursor(16,midY-7); tft.print(bts);

  uint32_t us=millis()/1000;
  char up[16]; snprintf(up,16,"%02lu:%02lu:%02lu",us/3600,(us%3600)/60,us%60);
  tft.setTextColor(DIM2,BG); tft.setCursor(16,midY+2); tft.print(up);

  uint32_t tot=g.approvals+g.denials;
  uint8_t rate=tot?(uint8_t)((uint32_t)g.approvals*100/tot):0;
  char rs[8]; snprintf(rs,8,"%u%%",rate);
  int bx=SCR_W/2+10, barW=SCR_W/2-20;
  tft.setTextColor(DIM3,BG); tft.setCursor(bx,SEC_Y+2); tft.print("approval");
  tft.drawRect(bx,midY-3,barW,8,DIM1);
  if(rate>0){ int fw=barW*rate/100; tft.fillRect(bx+1,midY-2,fw-1,6,col); }
  tft.setTextColor(col,BG);
  int rw=strlen(rs)*6; tft.setCursor(bx+(barW-rw)/2,midY+6); tft.print(rs);
}

// ── Tab bar ───────────────────────────────────────────────────────────────
static void drawTabBar(){
  tft.fillRect(0,TAB_Y,SCR_W,TAB_H,BG);
  static const char* lbl[3]={"BUDDY","STATS","LOG"};
  uint16_t col=ACC[gState];
  int W3=SCR_W/3;
  for(int i=0;i<3;i++){
    int x=i*W3;
    bool active=(curTab==(Tab)i);
    uint16_t fg=active?col:DIM2;
    tft.setTextSize(1); tft.setTextColor(fg,BG);
    int tw=strlen(lbl[i])*6;
    tft.setCursor(x+(W3-tw)/2,TAB_Y+TAB_H/2-4); tft.print(lbl[i]);
    if(active){
      tft.drawFastHLine(x+4,TAB_Y+TAB_H-4,W3-8,col);
      tft.drawFastHLine(x+4,TAB_Y+TAB_H-3,W3-8,dimC(col,3));
    }
  }
  tft.drawFastVLine(W3,  TAB_Y+2,TAB_H-4,DIM1);
  tft.drawFastVLine(2*W3,TAB_Y+2,TAB_H-4,DIM1);
}

// ── Clock-only partial update ─────────────────────────────────────────────
static void updateTopBarClock(){
  char tim[6]; getTime(tim);
  int tw=strlen(tim)*6;
  tft.setTextSize(1); tft.setTextColor(DIM3,BG2);
  tft.setCursor((SCR_W-tw)/2,5); tft.print(tim);
}

// ── Targeted text update for BUDDY tab ────────────────────────────────────
static void updateBuddyText(){
  drawStatRow(); drawFeed(); drawSecondary();
  tft.setTextSize(1); tft.setTextColor(ACC[gState],BG2);
  tft.fillRect(4,2,strlen(devName)*6,14,BG2);
  tft.setCursor(4,5); tft.print(devName);
}

// ── Per-source usage (USB = Claude Code via hub, BLE = Claude Desktop) ────
static void srcCounts(int x,int y,uint8_t u,uint8_t b,uint16_t bg){
  char t[10]; tft.setTextSize(1);
  snprintf(t,10,"USB %u",u); tft.setTextColor(u?C_USB:DIM2,bg); tft.setCursor(x,y);    tft.print(t);
  snprintf(t,10,"BLE %u",b); tft.setTextColor(b?C_BLE:DIM2,bg); tft.setCursor(x+54,y); tft.print(t);
}
static void drawSrcSplit(int x,int y,int w){
  uint32_t u=g.tokS[0],b=g.tokS[1],t=u+b;
  char ub[10],bb[10],ln[24]; fmtTok(ub,10,u); fmtTok(bb,10,b);
  tft.setTextSize(1); tft.fillRect(x,y,w,9,BG);
  snprintf(ln,24,"USB %s",ub); tft.setTextColor(u?C_USB:DIM2,BG); tft.setCursor(x,y); tft.print(ln);
  snprintf(ln,24,"BLE %s",bb); tft.setTextColor(b?C_BLE:DIM2,BG); tft.setCursor(x+w-(int)strlen(ln)*6,y); tft.print(ln);
  tft.fillRect(x,y+11,w,5,BG3);
  if(t>0){int uw=(int)((uint64_t)u*w/t); if(uw>0)tft.fillRect(x,y+11,uw,5,C_USB); if(w-uw>0)tft.fillRect(x+uw,y+11,w-uw,5,C_BLE);}
}

// ── Full BUDDY tab draw ───────────────────────────────────────────────────
static void drawBuddy(){
  tft.fillScreen(BG);
  drawTopBar();
  glowLine(GL1,ACC[gState]);
  tft.fillRect(SPR_X,SPR_Y,SPR_W,SPR_H,BG);
  pushCatSprite();
  glowLine(GL2,ACC[gState]);
  drawStatRow();
  glowLine(GL3,ACC[gState]);
  drawFeed();
  glowLine(GL4,ACC[gState]);
  drawSecondary();
  glowLine(GL5,ACC[gState]);
  drawTabBar();
}

// ── STATS tab ─────────────────────────────────────────────────────────────
static void drawStats(){
  tft.fillScreen(BG);
  uint16_t col=ACC[gState];
  drawTopBar();
  glowLine(GL1,col);

#ifdef NEW_BOARD
  // Landscape full-width stats layout
  int y=24, cx=8;
  auto section=[&](const char* title){
    tft.setTextSize(1); tft.setTextColor(col,BG);
    tft.setCursor(cx,y); tft.print(title);
    tft.drawFastHLine(cx+strlen(title)*6+4,y+3,SCR_W-cx-strlen(title)*6-8,dimC(col,4));
    y+=10;
  };
  section("SESSIONS");
  int hw=(SCR_W-4)/2;
  uint16_t rbg=g.running>0?0x0220:DIM1; uint16_t rfg=g.running>0?col:DIM2;
  tft.fillRect(2,y,hw,22,rbg); cornerBrkt(2,y,hw,22,5,rfg);
  tft.setTextSize(1); tft.setTextColor(rfg,rbg); tft.setCursor(6,y+4); tft.print("RUNNING");
  char rn[4]; snprintf(rn,4,"%u",g.running);
  tft.setTextSize(2); tft.setTextColor(g.running>0?col:DIM2,rbg);
  tft.setCursor(hw-strlen(rn)*12-4,y+4); tft.print(rn);
  srcCounts(6,y+13,usbLive()?g.runS[0]:0,bleConn?g.runS[1]:0,rbg);
  uint16_t wbg=g.waiting>0?0x1800:DIM1; uint16_t wfg=g.waiting>0?0xFDA0:DIM2;
  tft.fillRect(hw+4,y,hw,22,wbg); cornerBrkt(hw+4,y,hw,22,5,wfg);
  tft.setTextSize(1); tft.setTextColor(wfg,wbg); tft.setCursor(hw+8,y+4); tft.print("WAITING");
  char wn[4]; snprintf(wn,4,"%u",g.waiting);
  tft.setTextSize(2); tft.setTextColor(g.waiting>0?0xFDA0:DIM2,wbg);
  tft.setCursor(SCR_W-strlen(wn)*12-6,y+4); tft.print(wn);
  srcCounts(hw+8,y+13,usbLive()?g.waitS[0]:0,bleConn?g.waitS[1]:0,wbg);
  y+=28; tft.drawFastHLine(0,y,SCR_W,dimC(col,4)); y+=4;

  section("TOKENS TODAY");
  char tokbuf[12]; fmtTok(tokbuf,12,g.tokToday);
  tft.setTextSize(3); tft.setTextColor(col,BG);
  int tw2=strlen(tokbuf)*18; tft.setCursor((SCR_W-tw2)/2,y); tft.print(tokbuf);
  y+=26;
  uint32_t mx=1000; while(mx<g.tokToday&&mx<1000000)mx*=10;
  uint32_t bfill=mx>0?(uint32_t)((uint64_t)g.tokToday*(SCR_W-16)/mx):0;
  if(bfill>(uint32_t)(SCR_W-16))bfill=SCR_W-16;
  tft.fillRect(8,y,SCR_W-16,8,BG3); tft.drawRect(8,y,SCR_W-16,8,DIM1);
  if(bfill>0){ uint16_t bc=bfill<80?0x07E0:(bfill<160?0xFFE0:0xE000); tft.fillRect(9,y+1,bfill,6,bc); }
  y+=12;
  char mxs[10]; fmtTok(mxs,10,mx);
  tft.setTextSize(1); tft.setTextColor(DIM2,BG); tft.setCursor(8,y); tft.print("0");
  tft.setCursor(SCR_W-8-strlen(mxs)*6,y); tft.print(mxs);
  if(g.tokTotal>0){char lt[20],lb[10]; fmtTok(lb,10,g.tokTotal); snprintf(lt,20,"Life %s",lb);
    tft.setTextColor(DIM2,BG); tft.setCursor((SCR_W-(int)strlen(lt)*6)/2,y);tft.print(lt);}
  drawSrcSplit(8,y+11,SCR_W-16);
  y+=30; tft.drawFastHLine(0,y,SCR_W,dimC(col,4)); y+=4;

  section("DECISIONS");
  uint32_t tot=g.approvals+g.denials;
  auto dbar=[&](const char* lbl2,uint32_t v,uint16_t fc,int dy){
    tft.setTextColor(fc,BG); tft.setCursor(cx,y+dy); tft.print(lbl2);
    uint32_t bw2=tot>0?(uint32_t)((uint64_t)v*156/tot):0;
    tft.fillRect(50,y+dy-2,156,10,BG3); tft.drawRect(50,y+dy-2,156,10,DIM1);
    if(bw2>0)tft.fillRect(51,y+dy-1,bw2,8,fc);
    char cv[6]; snprintf(cv,6,"%u",v); tft.setCursor(210,y+dy); tft.print(cv);
  };
  dbar("ALLOW",g.approvals,col,0); dbar("DENY ",g.denials,0xE000,14);
  uint8_t rate=tot?(uint8_t)((uint32_t)g.approvals*100/tot):0;
  char rs[20]; snprintf(rs,20,"Allow rate  %u%%",rate);
  tft.setTextColor(DIM3,BG); tft.setCursor(cx,y+30); tft.print(rs);
  y+=42;
  tft.drawFastHLine(0,y,SCR_W,dimC(col,4)); y+=4;

  section("SYSTEM");
  uint32_t us=millis()/1000;
  char ups[24]; snprintf(ups,24,"Uptime  %02lu:%02lu:%02lu",us/3600,(us%3600)/60,us%60);
  tft.setTextColor(DIM3,BG); tft.setCursor(cx,y); tft.print(ups); y+=10;
  if(g.lastBeat>0){
    uint32_t ba=(millis()-g.lastBeat)/1000;
    char bts[20]; snprintf(bts,20,"Beat  %lus ago",ba);
    tft.setTextColor(ba>8?0xE000:DIM3,BG); tft.setCursor(cx,y); tft.print(bts); y+=10;
  }
  tft.setTextColor(linkCol(col),BG); tft.setCursor(cx,y); tft.print(linkText());

#else
  int y=24, cx=8;
  auto section=[&](const char* title){
    tft.setTextSize(1); tft.setTextColor(col,BG);
    tft.setCursor(cx,y); tft.print(title);
    tft.drawFastHLine(cx+strlen(title)*6+4,y+3,SCR_W-cx-strlen(title)*6-8,dimC(col,4));
    y+=14;
  };

  section("SESSIONS");
  uint16_t rbg=g.running>0?0x0220:DIM1; uint16_t rfg=g.running>0?col:DIM2;
  tft.fillRect(cx,y,110,28,rbg); cornerBrkt(cx,y,110,28,6,rfg);
  tft.setTextSize(1); tft.setTextColor(rfg,rbg); tft.setCursor(cx+4,y+4); tft.print("RUNNING");
  char rn[4]; snprintf(rn,4,"%u",g.running);
  tft.setTextSize(2); tft.setTextColor(g.running>0?col:DIM2,rbg);
  tft.setCursor(cx+100-strlen(rn)*12,y+7); tft.print(rn);
  srcCounts(cx+4,y+17,usbLive()?g.runS[0]:0,bleConn?g.runS[1]:0,rbg);
  uint16_t wbg=g.waiting>0?0x1800:DIM1; uint16_t wfg=g.waiting>0?0xFDA0:DIM2;
  tft.fillRect(124,y,108,28,wbg); cornerBrkt(124,y,108,28,6,wfg);
  tft.setTextSize(1); tft.setTextColor(wfg,wbg); tft.setCursor(128,y+4); tft.print("WAITING");
  char wn[4]; snprintf(wn,4,"%u",g.waiting);
  tft.setTextSize(2); tft.setTextColor(g.waiting>0?0xFDA0:DIM2,wbg);
  tft.setCursor(220-strlen(wn)*12,y+7); tft.print(wn);
  srcCounts(128,y+17,usbLive()?g.waitS[0]:0,bleConn?g.waitS[1]:0,wbg);
  y+=36;

  glowLine(y,col); y+=6;

  section("TOKENS TODAY");
  char tokbuf[12]; fmtTok(tokbuf,12,g.tokToday);
  tft.setTextSize(3); tft.setTextColor(col,BG);
  int tw2=strlen(tokbuf)*18; tft.setCursor((SCR_W-tw2)/2,y); tft.print(tokbuf);
  y+=30;
  uint32_t mx=1000; while(mx<g.tokToday&&mx<1000000)mx*=10;
  uint32_t bfill=mx>0?(uint32_t)((uint64_t)g.tokToday*224/mx):0;
  if(bfill>224)bfill=224;
  tft.fillRect(8,y,224,10,BG3); tft.drawRect(8,y,224,10,DIM1);
  if(bfill>0){ uint16_t bc=bfill<75?0x07E0:(bfill<150?0xFFE0:0xE000); tft.fillRect(9,y+1,bfill,8,bc); }
  y+=14;
  char mxs[10]; fmtTok(mxs,10,mx);
  tft.setTextSize(1); tft.setTextColor(DIM2,BG); tft.setCursor(8,y); tft.print("0");
  tft.setCursor(234-strlen(mxs)*6,y); tft.print(mxs);
  if(g.tokTotal>0){char lt[20],lb[10]; fmtTok(lb,10,g.tokTotal); snprintf(lt,20,"Life %s",lb);
    tft.setTextColor(DIM2,BG); tft.setCursor((SCR_W-(int)strlen(lt)*6)/2,y);tft.print(lt);}
  drawSrcSplit(8,y+11,224);
  y+=30;

  glowLine(y,col); y+=6;

  section("DECISIONS");
  uint32_t tot=g.approvals+g.denials;
  auto dbar=[&](const char* lbl2,uint32_t v,uint16_t fc,int dy){
    tft.setTextColor(fc,BG); tft.setCursor(cx,y+dy); tft.print(lbl2);
    uint32_t bw2=tot>0?(uint32_t)((uint64_t)v*156/tot):0;
    tft.fillRect(50,y+dy-2,156,12,BG3); tft.drawRect(50,y+dy-2,156,12,DIM1);
    if(bw2>0)tft.fillRect(51,y+dy-1,bw2,10,fc);
    char cv[6]; snprintf(cv,6,"%u",v); tft.setCursor(210,y+dy); tft.print(cv);
  };
  dbar("ALLOW",g.approvals,col,0); dbar("DENY ",g.denials,0xE000,14);
  uint8_t rate=tot?(uint8_t)((uint32_t)g.approvals*100/tot):0;
  char rs[20]; snprintf(rs,20,"Allow rate  %u%%",rate);
  tft.setTextColor(DIM3,BG); tft.setCursor(cx,y+30); tft.print(rs);
  y+=44;

  glowLine(y,col); y+=6;

  section("SYSTEM");
  uint32_t us=millis()/1000;
  char ups[24]; snprintf(ups,24,"Uptime  %02lu:%02lu:%02lu",us/3600,(us%3600)/60,us%60);
  tft.setTextColor(DIM3,BG); tft.setCursor(cx,y); tft.print(ups); y+=12;
  if(g.lastBeat>0){
    uint32_t ba=(millis()-g.lastBeat)/1000;
    char bts[20]; snprintf(bts,20,"Beat  %lus ago",ba);
    tft.setTextColor(ba>8?0xE000:DIM3,BG); tft.setCursor(cx,y); tft.print(bts); y+=12;
  }
  tft.setTextColor(linkCol(col),BG); tft.setCursor(cx,y); tft.print(linkText());
#endif

  glowLine(GL5,col);
  drawTabBar();
}

// ── LOG tab ────────────────────────────────────────────────────────────────
static void drawLog(){
  tft.fillScreen(BG3);
  drawTopBar();
  glowLine(GL1,0x07E0);
  tft.fillRect(0,SPR_Y,SCR_W,GL5-SPR_Y,0x0000);
  tft.setTextSize(1);
  for(int i=0;i<LOG_VISIBLE;i++){
    int idx=(logI+logScroll+i)%LOG_N;
    if(!log_buf[idx][0])continue;
    tft.setTextColor(i%2==0?0x07E0:0x03A0,0x0000);
    tft.setCursor(4,SPR_Y+2+i*8); tft.print(log_buf[idx]);
  }
  if(frame&1){ int cy=SPR_Y+2+LOG_VISIBLE*8; if(cy<GL5-2)tft.fillRect(4,cy,8,7,0x07E0); }
  int barH=max(8,(GL5-SPR_Y)*LOG_VISIBLE/LOG_N);
  int barY=SPR_Y+(GL5-SPR_Y-barH)*logScroll/max(1,LOG_N-LOG_VISIBLE);
  tft.fillRect(SCR_W-4,SPR_Y,4,GL5-SPR_Y,DIM1);
  tft.fillRect(SCR_W-4,barY,4,barH,DIM2);
  glowLine(GL5,0x07E0);
  drawTabBar();
}

// Claude Code permission prompts can only be answered in the terminal, so instead of
// Allow/Deny zones show where to go; a tap dismisses it until the prompt changes.
static void drawPromptInfo(int top,int bot,uint16_t col){
  int h=bot-top;
  tft.fillRect(0,top,SCR_W,h,0x1000);
  cornerBrkt(4,top+4,SCR_W-8,h-8,12,dimC(col,2));
  tft.setTextSize(2); tft.setTextColor(col,0x1000);
  int w1=strlen("Answer in")*12; tft.setCursor((SCR_W-w1)/2,top+h/2-26); tft.print("Answer in");
  int w2=strlen("terminal")*12;  tft.setCursor((SCR_W-w2)/2,top+h/2-6);  tft.print("terminal");
  tft.setTextSize(1); tft.setTextColor(DIM3,0x1000);
  int w3=strlen("tap to dismiss")*6; tft.setCursor((SCR_W-w3)/2,top+h/2+18); tft.print("tap to dismiss");
  tft.fillCircle(SCR_W/2,top+h-18,(frame&1)?7:5,dimC(col,2));
  tft.fillCircle(SCR_W/2,top+h-18,(frame&1)?4:3,col);
}

// ── ATTENTION overlay ──────────────────────────────────────────────────────
static void drawAttention(){
  tft.fillScreen(BG);
  uint16_t col=0xFDA0;

#ifdef NEW_BOARD
  // Compact header for landscape
  uint16_t hbg=(frame&1)?0x2000:0x1800;
  tft.fillRect(0,0,SCR_W,28,hbg);
  cornerBrkt(0,0,SCR_W,28,10,col);
  tft.setTextSize(2); tft.setTextColor(col,hbg);
  int htw=strlen("! APPROVE !")*12; tft.setCursor((SCR_W-htw)/2,6); tft.print("! APPROVE !");

  glowLine(29,col);

  tft.setTextSize(1); tft.setTextColor(DIM3,BG); tft.setCursor(8,35); tft.print("TOOL:");
  tft.setTextSize(3); tft.setTextColor(col,BG);
  int tw=strlen(g.pTool)*18; tft.setCursor((SCR_W-tw)/2,44); tft.print(g.pTool);

  tft.fillRect(8,74,SCR_W-16,30,BG3);
  cornerBrkt(8,74,SCR_W-16,30,7,dimC(col,3));
  tft.setTextSize(1); tft.setTextColor(0x07E0,BG3);
  tft.setCursor(14,80); tft.print("$ "); tft.print(g.pHint);
  tft.setTextColor(DIM1,BG3); tft.setCursor(14,91); tft.print(g.pId);

  glowLine(107,col);

  // Decision zones: left=Don't Allow, right=Allow
  int zoneTop=109, zoneBot=GL5-1, zoneH=zoneBot-zoneTop;
  int mid=SCR_W/2-1;
  if(g.pInfo){ drawPromptInfo(zoneTop,zoneBot,col); } else {

  tft.fillRect(0,zoneTop,mid,zoneH,0x1800);
  cornerBrkt(2,zoneTop+2,mid-4,zoneH-4,10,dimC(0xE000,2));
  tft.setTextSize(2); tft.setTextColor(0xE000,0x1800);
  tft.setCursor(8,zoneTop+zoneH/2-24); tft.print("Don't");
  tft.setCursor(8,zoneTop+zoneH/2-4); tft.print("Allow");
  tft.setTextSize(1); tft.setTextColor(DIM2,0x1800);
  tft.setCursor(8,zoneTop+zoneH/2+18); tft.print("tap left");
  tft.fillCircle(mid/2,zoneTop+zoneH-18,(frame&1)?7:5,dimC(0xE000,2));
  tft.fillCircle(mid/2,zoneTop+zoneH-18,(frame&1)?4:3,0xE000);

  tft.fillRect(mid+2,zoneTop,SCR_W-mid-2,zoneH,0x0220);
  cornerBrkt(mid+4,zoneTop+2,SCR_W-mid-6,zoneH-4,10,dimC(col,2));
  tft.setTextSize(2); tft.setTextColor(col,0x0220);
  tft.setCursor(mid+10,zoneTop+zoneH/2-14); tft.print("Allow");
  tft.setCursor(mid+10,zoneTop+zoneH/2+6); tft.print("Once");
  tft.setTextSize(1); tft.setTextColor(DIM2,0x0220);
  tft.setCursor(mid+10,zoneTop+zoneH/2+26); tft.print("tap right");
  tft.fillCircle(mid+2+(SCR_W-mid-2)/2,zoneTop+zoneH-18,(frame&1)?7:5,dimC(col,2));
  tft.fillCircle(mid+2+(SCR_W-mid-2)/2,zoneTop+zoneH-18,(frame&1)?4:3,col);
  }

#else
  uint16_t hbg=(frame&1)?0x2000:0x1800;
  tft.fillRect(0,0,SCR_W,36,hbg);
  cornerBrkt(0,0,SCR_W,36,10,col);
  tft.setTextSize(2); tft.setTextColor(col,hbg);
  int htw=strlen("! APPROVE !")*12; tft.setCursor((SCR_W-htw)/2,10); tft.print("! APPROVE !");

  glowLine(37,col);

  tft.setTextSize(1); tft.setTextColor(DIM3,BG); tft.setCursor(8,46); tft.print("TOOL:");
  tft.setTextSize(3); tft.setTextColor(col,BG);
  int tw=strlen(g.pTool)*18; tft.setCursor((SCR_W-tw)/2,54); tft.print(g.pTool);

  tft.fillRect(8,84,SCR_W-16,34,BG3);
  cornerBrkt(8,84,SCR_W-16,34,8,dimC(col,3));
  tft.setTextSize(1); tft.setTextColor(0x07E0,BG3);
  tft.setCursor(14,92); tft.print("$ "); tft.print(g.pHint);
  tft.setTextColor(DIM1,BG3); tft.setCursor(14,104); tft.print(g.pId);

  glowLine(122,col);

  if(g.pInfo){ drawPromptInfo(124,286,col); } else {
  tft.fillRect(0,124,117,162,0x1800);
  cornerBrkt(2,126,113,158,12,dimC(0xE000,2));
  tft.setTextSize(2); tft.setTextColor(0xE000,0x1800);
  tft.setCursor(10,175); tft.print("Don't");
  tft.setCursor(10,195); tft.print("Allow");
  tft.setTextSize(1); tft.setTextColor(DIM2,0x1800);
  tft.setCursor(10,220); tft.print("tap left");
  tft.fillCircle(58,250,(frame&1)?8:6,dimC(0xE000,2));
  tft.fillCircle(58,250,(frame&1)?5:4,0xE000);

  tft.fillRect(123,124,117,162,0x0220);
  cornerBrkt(125,126,113,158,12,dimC(col,2));
  tft.setTextSize(2); tft.setTextColor(col,0x0220);
  tft.setCursor(131,185); tft.print("Allow");
  tft.setCursor(131,205); tft.print("Once");
  tft.setTextSize(1); tft.setTextColor(DIM2,0x0220);
  tft.setCursor(131,220); tft.print("tap right");
  tft.fillCircle(181,250,(frame&1)?8:6,dimC(col,2));
  tft.fillCircle(181,250,(frame&1)?5:4,col);
  }
#endif

  glowLine(GL5,col);
  drawTabBar();
}

// ── Advertising screen ────────────────────────────────────────────────────
static void drawAdvertising(){
  tft.fillScreen(BG);
  uint16_t col=0x07FF;
  glowLine(GL1,col);
  tft.fillRect(0,SPR_Y,SCR_W,GL5-SPR_Y,BG);

  int nameY=SCR_H/4;
  uint16_t nc=(frame&1)?col:dimC(col,3);
  int nw=strlen(devName)*12; tft.setTextSize(2); tft.setTextColor(nc,BG);
  tft.setCursor((SCR_W-nw)/2,nameY); tft.print(devName);
  cornerBrkt((SCR_W-nw)/2-8,nameY-4,nw+16,24,8,dimC(col,3));

  tft.setTextSize(1); tft.setTextColor(DIM3,BG);
  tft.setCursor(20,nameY+30); tft.print("Waiting: USB bridge or Desktop...");

  const char* steps[]={"1. Claude Desktop","2. Help > Troubleshooting",
                        "3. Enable Developer Mode","4. Developer > Hardware Buddy"};
  int stY=nameY+48;
  for(int i=0;i<4;i++){
    tft.setTextColor(i==0?DIM3:DIM2,BG);
    tft.setCursor(20,stY+i*16); tft.print(steps[i]);
  }

  int dotY=stY+4*16+14;
  if(dotY<GL5-10){
    for(int i=0;i<5;i++){
      uint16_t dc=(i==(int)(frame%5))?col:DIM1;
      tft.fillCircle(SCR_W/2-40+i*20,dotY,4,dc);
    }
  }

  glowLine(GL5,col);
  drawTabBar();

  tft.fillRect(0,0,SCR_W,18,BG2);
  int tw=strlen(devName)*6; tft.setTextSize(1); tft.setTextColor(col,BG2);
  tft.setCursor((SCR_W-tw)/2,5); tft.print(devName);
  tft.fillCircle(SCR_W-10,9,6,DIM1); tft.drawCircle(SCR_W-10,9,6,DIM2);
}

static void drawPasskey(){
  tft.fillScreen(BG);
  uint16_t col=0xFDA0;
  tft.fillRect(0,0,SCR_W,18,BG2); tft.setTextSize(1); tft.setTextColor(col,BG2);
  tft.setCursor(4,5); tft.print("BLE Pairing");
  glowLine(GL1,col);
  tft.setTextSize(1); tft.setTextColor(DIM3,BG);
  tft.setCursor(20,SCR_H/3); tft.print("Enter this code in Claude Desktop:");
  char pk[8]; snprintf(pk,sizeof(pk),"%06lu",bleKey);
  tft.setTextSize(4); tft.setTextColor(col,BG);
  int pw=strlen(pk)*24; tft.setCursor((SCR_W-pw)/2,SCR_H/2-20); tft.print(pk);
  cornerBrkt((SCR_W-pw)/2-10,SCR_H/2-25,pw+20,44,10,dimC(col,3));
  tft.setTextSize(1); tft.setTextColor(DIM2,BG);
  tft.setCursor(20,SCR_H*2/3); tft.print("Waiting for confirmation...");
  glowLine(GL5,col);
  drawTabBar();
}

static void drawConnecting(){
  tft.fillScreen(BG);
  uint16_t col=0x07FF;
  glowLine(GL1,col);
  tft.setTextSize(2); tft.setTextColor(col,BG);
  tft.setCursor(20,SCR_H/2-12); tft.print("Pairing...");
  tft.setTextSize(1); tft.setTextColor(DIM2,BG);
  tft.setCursor(20,SCR_H/2+8); tft.print("Establishing secure BLE connection");
  glowLine(GL5,col);
  drawTabBar();
}

// ── Protocol ───────────────────────────────────────────────────────────────
static void sendPerm(const char* id,const char* dec){
  char b[128]; snprintf(b,128,"{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}",id,dec);
  linkTx(b);
}

static void parseLine(const char* line,bool usb=false){
  if(!usb)logLine(line);
  JsonDocument doc;
  if(deserializeJson(doc,line)!=DeserializationError::Ok)return;
  if(usb)logLine(line);
  g.conn=true; g.lastBeat=millis();
  if(usb)usbAt=millis();

  JsonArray ta=doc["time"].as<JsonArray>();
  if(!ta.isNull()&&ta.size()>=2){g.epoch=ta[0].as<uint32_t>();g.epochAt=millis();g.tzOff=ta[1].as<int32_t>();}

  const char* evt=doc["evt"];
  if(evt&&strcmp(evt,"turn")==0){
    const char* role=doc["role"];
    if(role&&strcmp(role,"user")==0){strncpy(g.msg,"thinking...",63);turnEnd=millis()+30000;g.runS[1]=1;}
    else if(role&&strcmp(role,"assistant")==0){strncpy(g.msg,"done",63);turnEnd=0;g.runS[1]=0;}
    BS prev=gState; recompute();
    if(gState!=prev)dirty=true; else textDirty=true;
    return;
  }

  const uint8_t src=usb?0:1;
  if(!doc["running"].isNull())      g.runS[src] =doc["running"].as<uint8_t>();
  if(!doc["waiting"].isNull())      g.waitS[src]=doc["waiting"].as<uint8_t>();
  if(!doc["tokens_today"].isNull()) g.tokS[src] =doc["tokens_today"].as<uint32_t>();
  if(!doc["tokens"].isNull())       g.tokTotal=doc["tokens"].as<uint32_t>();
  const char* m=doc["msg"]; if(m){strncpy(g.msg,m,63);g.msg[63]=0;}

  JsonArray ea=doc["entries"].as<JsonArray>();
  if(!ea.isNull()){g.nEntries=0;for(JsonVariant v:ea){if(g.nEntries>=5)break;strncpy(g.entries[g.nEntries],v.as<const char*>(),51);g.entries[g.nEntries++][51]=0;}}

  JsonObject p=doc["prompt"].as<JsonObject>();
  if(!p.isNull()){
    const char* pid=p["id"]|"";
    if(dismissedId[0]&&!strcmp(pid,dismissedId)){g.waitS[src]=0;}
    else{strncpy(g.pId,pid,39);strncpy(g.pTool,p["tool"]|"",19);strncpy(g.pHint,p["hint"]|"",43);g.pInfo=p["info"]|false;g.waitS[src]=1;g.pSrc=src;}
  }else if(src==g.pSrc&&!doc["waiting"].isNull()&&doc["waiting"].as<uint8_t>()==0){
    g.pId[0]=g.pTool[0]=g.pHint[0]=0;g.pInfo=false;dismissedId[0]=0;
  }

  const char* cmd=doc["cmd"];
  if(cmd){
    if     (!strcmp(cmd,"status")){char r[128];snprintf(r,128,"{\"ack\":\"status\",\"ok\":true,\"data\":{\"name\":\"%s\",\"sec\":%s}}",devName,bleSec?"true":"false");linkTx(r);}
    else if(!strcmp(cmd,"name"))  linkTx("{\"ack\":\"name\",\"ok\":true}");
    else if(!strcmp(cmd,"owner")) linkTx("{\"ack\":\"owner\",\"ok\":true}");
    else if(!strcmp(cmd,"unpair"))linkTx("{\"ack\":\"unpair\",\"ok\":true}");
  }
  BS prev=gState; recompute();
  if(gState!=prev||!online())dirty=true; else textDirty=true;
}

// ── BLE init ───────────────────────────────────────────────────────────────
static void initBLE(){
  BLEDevice::init(devName);
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  BLEDevice::setSecurityCallbacks(new SecCB());
  BLESecurity* pSec=new BLESecurity();
  pSec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  pSec->setCapability(ESP_IO_CAP_OUT);
  pSec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK|ESP_BLE_ID_KEY_MASK);
  pSec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK|ESP_BLE_ID_KEY_MASK);
  pSec->setKeySize(16);
  BLEServer* srv=BLEDevice::createServer(); srv->setCallbacks(new SrvCB());
  BLEService* svc=srv->createService(BLEUUID(NUS_SVC));
  pTx=svc->createCharacteristic(BLEUUID(NUS_TX),BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY);
  pTx->addDescriptor(new BLE2902());
  BLECharacteristic* rx=svc->createCharacteristic(BLEUUID(NUS_RX),BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCB());
  svc->start();
  BLEAdvertising* adv=BLEDevice::getAdvertising();
  adv->addServiceUUID(BLEUUID(NUS_SVC)); adv->setScanResponse(true);
  adv->setMinPreferred(0x06); adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
}

// ── Arduino ────────────────────────────────────────────────────────────────
void setup(){
  Serial.setRxBufferSize(2048);
  Serial.begin(115200);
  pinMode(LED_RED,OUTPUT);   digitalWrite(LED_RED,HIGH);
  pinMode(LED_GREEN,OUTPUT); digitalWrite(LED_GREEN,HIGH);
  pinMode(LED_BLUE,OUTPUT);  digitalWrite(LED_BLUE,HIGH);
  pinMode(BL_PIN,OUTPUT);    digitalWrite(BL_PIN,HIGH);

  tft.init(); tft.setRotation(SCR_ROT); tft.initDMA(true); tft.fillScreen(BG);
  tft.setTextSize(1); tft.setTextColor(0x07FF,BG);
  int tx=(SCR_W-strlen("Claude Buddy")*6)/2;
  tft.setCursor(tx,SCR_H/2-8); tft.print("Claude Buddy");
  tft.setTextColor(DIM2,BG);
  int sx=(SCR_W-strlen("starting...")*6)/2;
  tft.setCursor(sx,SCR_H/2+4); tft.print("starting...");

  catSpr.createSprite(SPR_W,SPR_H);
  catSpr.setTextWrap(false);

  tspi.begin(TOUCH_CLK,TOUCH_MISO,TOUCH_MOSI,TOUCH_CS);
  ts.begin(tspi); ts.setRotation(TS_ROT);

  uint8_t mac[6]; esp_read_mac(mac,ESP_MAC_BT);
  snprintf(devName,sizeof(devName),"Claude-%02X%02X",mac[4],mac[5]);

  initBLE();
  delay(600);
  tft.startWrite(); drawAdvertising(); tft.endWrite();
  Serial.printf("\n{\"hello\":\"claude-buddy\",\"name\":\"%s\"}\n",devName);
}

void loop(){
  uint32_t now=millis();

  while(rAvail()>0){
    char c=rRead();
    if(c=='\n'||c=='\r'){if(llen>0){lbuf[llen]=0;parseLine(lbuf);llen=0;}}
    else if(llen<(int)sizeof(lbuf)-1){lbuf[llen++]=c;}
  }

  while(Serial.available()>0){
    char c=(char)Serial.read();
    if(c=='\n'||c=='\r'){if(slen>0){sbuf[slen]=0;parseLine(sbuf,true);slen=0;}}
    else if(slen<(int)sizeof(sbuf)-1){sbuf[slen++]=c;}
    else slen=0;
  }

  if(bleConn&&!bleSec&&!showKey&&(now-connAt)>3000){bleSec=true;dirty=true;}
  if(turnEnd>0&&now>turnEnd){turnEnd=0;g.runS[1]=0;strncpy(g.msg,"idle",63);recompute();dirty=true;}
  // signed: a heartbeat parsed earlier in this pass has lastBeat > now, which as unsigned wraps to ~4e9
  if(g.conn&&(int32_t)(now-g.lastBeat)>15000){g.conn=false;recompute();dirty=true;}

  // LEDs
  if(gState==S_ATTN){
    if(now-lastLed>400){lastLed=now;ledOn=!ledOn;digitalWrite(LED_RED,ledOn?LOW:HIGH);digitalWrite(LED_GREEN,HIGH);}
  }else if(online()){
    if(now-lastLed>2000){lastLed=now;ledOn=!ledOn;digitalWrite(LED_RED,HIGH);digitalWrite(LED_GREEN,ledOn?LOW:HIGH);}
  }else{digitalWrite(LED_RED,HIGH);digitalWrite(LED_GREEN,HIGH);}

  // Touch
  if(ts.tirqTouched()&&ts.touched()){
    TS_Point p=ts.getPoint();
    if(!swk.active){swk.sx=p.x;swk.sy=p.y;swk.st=millis();swk.active=true;swk.drag=false;swk.startState=gState;}
    swk.ex=p.x;swk.ey=p.y;
    if(!swk.drag&&(abs(swk.ex-swk.sx)>500||abs(swk.ey-swk.sy)>500))swk.drag=true;
  }else if(swk.active){
    int dx=swk.ex-swk.sx,dy=swk.ey-swk.sy;
    uint32_t dt=millis()-swk.st;
    if(swk.drag&&abs(dx)>800&&abs(dx)>abs(dy)*2&&dt<600){
      curTab=(Tab)((curTab+(dx<0?1:2))%3);logScroll=0;dirty=true;
    }else if(swk.drag&&curTab==TAB_LOG&&abs(dy)>300){
      logScroll=constrain(logScroll+(-dy/120),0,max(0,LOG_N-LOG_VISIBLE));dirty=true;
    }else if(!swk.drag&&dt<400){
      int sx,sy; mapTouch(swk.ex,swk.ey,&sx,&sy);
      if(swk.startState==S_ATTN&&g.pInfo&&sy<TAB_Y){
        strncpy(dismissedId,g.pId,39);dismissedId[39]=0;
        g.waitS[g.pSrc]=0;g.pId[0]=0;g.pInfo=false;recompute();dirty=true;
      }else if(swk.startState==S_ATTN&&g.pId[0]&&sy<TAB_Y){
        bool ok=(sx>=SCR_W/2);
        char pl[48]; snprintf(pl,48,"PERM %s: %s",g.pTool,ok?"allow":"deny"); logLine(pl);
        sendPerm(g.pId,ok?"once":"deny");
        if(ok)g.approvals++;else g.denials++;
        g.waitS[g.pSrc]=0;g.pId[0]=0;recompute();dirty=true;
      }else if(sy>=TAB_Y){
        int W3=SCR_W/3;
        Tab t=(sx<W3)?TAB_BUDDY:(sx<2*W3)?TAB_STATS:TAB_LOG;
        if(t!=curTab){curTab=t;logScroll=0;dirty=true;}
      }
    }
    swk.active=false;
  }

  // Stats/Log tabs have no incremental path; repaint at 1 Hz so beat age, uptime
  // and new log lines stay live instead of freezing until the next state change.
  static uint32_t lastTabRef=0;
  if(!dirty&&curTab!=TAB_BUDDY&&online()&&gState!=S_ATTN&&now-lastTabRef>1000){lastTabRef=now;dirty=true;}

  // Render
  static uint32_t lastAnim=0;
  if(dirty){
    tft.startWrite();
    if(!online()){
      if(bleConn&&showKey)       drawPasskey();
      else if(bleConn&&!bleSec)  drawConnecting();
      else                       drawAdvertising();
    }
    else if(gState==S_ATTN) drawAttention();
    else if(curTab==TAB_BUDDY) drawBuddy();
    else if(curTab==TAB_STATS) drawStats();
    else                       drawLog();
    tft.endWrite();
    dirty=false;textDirty=false;
  }else if(textDirty){
    tft.startWrite();
    if(online()&&curTab==TAB_BUDDY&&gState!=S_ATTN){
      updateBuddyText();
      pushCatSprite();
    }
    tft.endWrite();
    textDirty=false;
  }else if(now-lastAnim>700){
    frame++;
    tft.startWrite();
    if(online()&&gState!=S_ATTN&&curTab==TAB_BUDDY) pushCatSprite();
    updateTopBarClock();
    if(curTab==TAB_LOG&&online()&&gState!=S_ATTN){
      int cy=SPR_Y+2+LOG_VISIBLE*8;
      if(cy<GL5-2){tft.fillRect(4,cy,8,7,(frame&1)?0x07E0:0x0000);}
    }
    tft.endWrite();
    lastAnim=now;
  }
}
