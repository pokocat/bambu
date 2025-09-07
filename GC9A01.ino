/*********************************************************************
 * GC9A01 240×240 圆屏 ─ 打印机状态仪表盘（文字扩展版）
 * ESP32-C3  •  MQTT (Bambu)  •  DHT11  •  Arduino_GFX (文字扩展开启)
 *********************************************************************/
#include "config.h"                 // SSID / MQTT / 引脚常量
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <math.h>

/* ---------- 颜色 ---------- */
#define WHITE     0xFFFF
#define BLACK     0x0000
#define BG_GREY   0xC618          // 环背景灰
#define RING_COL  0x07E0          // 进度环绿色  (改红只需设成 TEXT_RED)
#define TEXT_RED  0xF800          // 统一红字

/* ---------- 显示屏 ---------- */
Arduino_DataBus *bus = new Arduino_SWSPI(
    TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, -1);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RST, 0, true);

/* ---------- DHT11 ---------- */
DHT dht(DHTPIN, DHTTYPE);

/* ---------- MQTT ---------- */
WiFiClient wifi;
PubSubClient mqtt(wifi);

/* ---------- 数据 ---------- */
struct Data {
  int    progress = 0;
  String pState   = "Idle";
  String stage    = "-";
  String layerCur = "0", layerTot="0";
  String eta      = "--:--";
  String material = "PLA";
  String weight   = "--g";
  float  temp = 0, humid = 0;
} cur;

/* ---------- 局部刷新缓存 ---------- */
int    lastProg = -1;
String lastCenter = "";
String lastEta="", lastLayer="", lastTemp="", lastHum="", lastMat="", lastWgt="";

bool          dirty    = true;   // 首次强刷
unsigned long lastDraw = 0;
const  uint16_t REFRESH_MS = 150;

/* ---------- 简易缩写 ---------- */
String shorten(const String &s, uint8_t max=10){
  return s.length()<=max ? s : s.substring(0,max-1)+"…";
}

/* ---------- 环 ---------- */
void updateRing(int p){
  if(p == lastProg) return;
  const int cx=120,cy=120,rI=95,rO=110;
  gfx->fillArc(cx,cy,rI,rO,-210,-210+3*lastProg,BG_GREY);
  gfx->fillArc(cx,cy,rI,rO,-210,-210+3*p,RING_COL /*,true*/); // 圆头需宏
  lastProg = p;
}

/* ---------- 中心三行 ---------- */
void updateCenter(){
  String now = String(cur.progress) + "|" + cur.pState + "|" + cur.stage;
  if(now == lastCenter) return;

  gfx->fillRect(0,60,240,94,WHITE);
  gfx->setTextColor(TEXT_RED,WHITE);
  gfx->setTextDatum(MC_DATUM);

  gfx->setTextSize(4); gfx->drawNumber(cur.progress,120,78);
  gfx->setTextSize(2); gfx->drawString("%",170,78);

  gfx->setTextSize(1); gfx->drawString(shorten(cur.pState),120,105);
  gfx->setTextSize(2); gfx->drawString(shorten(cur.stage), 120,134);

  lastCenter = now;
}

/* ---------- 单区块 ---------- */
void updateBlock(String &cache,const String &val,int ang){
  if(val == cache) return;
  cache = val;

  const float r=0.72f;
  float rad=radians(ang);
  int x=120+r*120*cos(rad);
  int y=120+r*120*sin(rad);

  gfx->setTextSize(1);
  gfx->setTextColor(TEXT_RED,WHITE);
  int w = val.length()*6;
  gfx->fillRect(x-w/2-2,y-4,w+4,8,WHITE);  // 擦旧
  gfx->setTextDatum(MC_DATUM);
  gfx->drawString(val,x,y);
}

/* ---------- 统一刷新 ---------- */
void refreshUI(){
  updateRing(cur.progress);
  updateCenter();
  updateBlock(lastEta,   "ETA:"+cur.eta,                         150);
  updateBlock(lastLayer, "L:"+cur.layerCur+"/"+cur.layerTot,      90);
  updateBlock(lastTemp,  "T:"+String(cur.temp,1)+"C",             30);
  updateBlock(lastHum,   "H:"+String(cur.humid,1)+"%",           -30);
  updateBlock(lastWgt,   "W:"+cur.weight,                        -90);
  updateBlock(lastMat,   "M:"+shorten(cur.material),            -150);
}

/* ---------- MQTT 回调 ---------- */
void mqttCB(char* topic, byte* payload, unsigned len){
  String v; v.reserve(len); for(uint32_t i=0;i<len;i++) v+=(char)payload[i];
  String t(topic);  bool changed=false;

  if      (t.endsWith("print_progress/state"))    {cur.progress=v.toInt(); changed=true;}
  else if (t.endsWith("print_status/state"))      {cur.pState=v;           changed=true;}
  else if (t.endsWith("current_stage/state"))     {cur.stage=v;            changed=true;}
  else if (t.endsWith("current_layer/state"))     {cur.layerCur=v;         changed=true;}
  else if (t.endsWith("total_layer_count/state")) {cur.layerTot=v;         changed=true;}
  else if (t.endsWith("remaining_time/state"))    {int m=v.toInt(); char b[8]; sprintf(b,"%02d:%02d",m/60,m%60); cur.eta=b; changed=true;}
  else if (t.endsWith("active_tray/state")||t.endsWith("external_spool/state"))
                                                 {cur.material=v;          changed=true;}
  else if (t.endsWith("weight/state"))           {cur.weight=v;            changed=true;}

  if(changed) dirty = true;
}

/* ---------- Wi-Fi / MQTT ---------- */
void connectWiFi(){
  gfx->fillScreen(WHITE);
  gfx->setTextColor(TEXT_RED,WHITE);
  gfx->setTextDatum(MC_DATUM);
  gfx->setTextSize(2); gfx->drawString("WiFi...",120,120);
  WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  while(WiFi.status()!=WL_CONNECTED) delay(500);
}
void connectMQTT(){
  gfx->fillScreen(WHITE);
  gfx->drawString("MQTT...",120,120);
  while(!mqtt.connected()){
    mqtt.connect("ESP32C3",MQTT_USER,MQTT_PASS);
    if(!mqtt.connected()) delay(2000);
  }
  mqtt.subscribe("bambu/#");
}

/* ---------- SETUP ---------- */
void setup(){
  pinMode(TFT_BL,OUTPUT); digitalWrite(TFT_BL,HIGH);
  gfx->begin(); gfx->fillScreen(WHITE);
  dht.begin();

  connectWiFi();
  mqtt.setServer(MQTT_SERVER,1883);
  mqtt.setCallback(mqttCB);
  connectMQTT();

  refreshUI();
  dirty=false; lastDraw=millis();
}

/* ---------- LOOP ---------- */
void loop(){
  mqtt.loop();

  /* DHT11 采样 */
  static unsigned long tDHT=0;
  if(millis()-tDHT>5000){
    float h=dht.readHumidity(), t=dht.readTemperature();
    if(!isnan(h)&&!isnan(t) &&
       (fabs(h-cur.humid)>0.1 || fabs(t-cur.temp)>0.1)){
      cur.humid=h; cur.temp=t; dirty=true;
    }
    tDHT=millis();
  }

  if(dirty && millis()-lastDraw>REFRESH_MS){
    refreshUI();
    dirty=false; lastDraw=millis();
  }
}
