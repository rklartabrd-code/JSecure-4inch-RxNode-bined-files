/*
 * 4-inch CYD RxNode: single-screen JPEG receiver
 *
 * REQUIRED INTEGRATION:
 * 1. Include your existing jNowMessage_t and jNOW/JS constants below.
 * 2. Paste your existing ESP-NOW initialization (peer/channel/security) into
 *    setupRadio(), and set myPeer to the sender MAC.
 * 3. Set SD_CS to the SD chip-select used by your working board setup.
 * 4. Keep your working TFT_eSPI User_Setup (ST7796 driver).
 *
 * Uses Arduino-ESP32 3.x callback signature, TFT_eSPI, TJpg_Decoder.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <JSecure.h>
#include "rklar_logo_64.h"

#define TFT_BLACK TFT_PURPLE
#define BOOT_BUTTON 0
#define USING_BINS  1


#ifndef SD_CS
#define SD_CS 5 // REPLACE with your working 4-inch CYD SD chip select
#endif
#define BAT_ADC 34
#define SAR_PAYLOAD 218u
#define SAR_HEADER 32u
#define MAX_JPEG 200000u
#define QUEUE_DEPTH 6

TFT_eSPI tft;
struct JpegMessage { uint8_t *data; size_t length; };

static esp_now_peer_info_t  peerInfo;
static volatile uint32_t    packets = 0, completed = 0, dropped = 0;
static uint8_t       myMacAddress[6] = { 0, 0, 0, 0, 0, 0 };
static uint8_t       myPeer[6] = {0}; 
static uint8_t       myPmk[32] = {0};
static QueueHandle_t jpegQueue = nullptr;
static uint8_t       *rxBuff   = nullptr;
static uint32_t      piclen    = 0;
static int           seg       = 0;
static int           pixState   = 0;
static uint32_t      saved     = 0;
static uint32_t      binned    = 0;
static bool          sdReady   = false;
static int           myChannel = 0xff   ;


static int      bin       = 0;

bool using_sd =false;
static void serviceBootButton()
{
  static bool lastRaw = HIGH;
  static bool stable = HIGH;
  static uint32_t changedMs = 0;

  const bool raw = digitalRead(BOOT_BUTTON);
 
  if (raw != lastRaw) {
    lastRaw = raw;
    changedMs = millis();
  }

  if ((millis() - changedMs) >= 30 && stable != raw) {
    stable = raw;

    if (stable == LOW) {
       Serial.println ("button");

      using_sd = (using_sd  == true) ?false  :true ;
   
    }
  }
}

void readMacAddress()
{
  uint8_t *baseMac;
  //esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, baseMac);
  
    Serial.println(WiFi.macAddress());
    WiFi.macAddress(myMacAddress);
    baseMac = myMacAddress;
    Serial.printf("%02x:%02x:%02x:%02x:%02x:%02x\n",
                 baseMac[0], baseMac[1], baseMac[2],
                  baseMac[3], baseMac[4], baseMac[5]);
}



static bool matchMac(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, 6) == 0;
}

static float batteryVoltage() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; ++i) sum += analogReadMilliVolts(BAT_ADC);
  return (sum / 16.0f) * 0.002f;
}

static bool jpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h,
                      uint16_t *bitmap) {
  if (y >= tft.height()) return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}
#define LINE_WID 100
// Left status strip: x=0..81; JPEG begins at x=82.


uint32_t test_count = 0;
static void drawStatus(float volts) {
  //tft.fillRect(0, 0, 82, tft.height(), TFT_BLACK);


  // ***********   logo
  bool oldSwap = tft.getSwapBytes();
  tft.setSwapBytes(true);
  tft.pushImage(16, 12, RKLAR_LOGO_W, RKLAR_LOGO_H,
                boohoorklar_logo_64, RKLAR_LOGO_TRANSPARENT);
  tft.setSwapBytes(oldSwap);

  //   ***********  Node Type on top
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextFont(4);
  tft.setCursor(192, 4);
  tft.print("*JSecure RxNode*");


  // ************** side panel
  // card stats
  tft.setTextFont(2);
  tft.setCursor(4, 100);
  tft.print("Sd Card Status" );
  tft.drawFastHLine(2, 118, LINE_WID, TFT_DARKGREY);

  tft.setTextFont(1);
  tft.setCursor(4, 124);
  tft.print(sdReady ? "SD OK" : "SD ERR");
  tft.setCursor(4, 134);
  tft.print(using_sd ? "SD On \n" : "SD OFF");


  // battery and boot stats
  tft.setTextFont(2);
  tft.setCursor(4, 165);
  tft.print("General Health" );
  tft.drawFastHLine(2, 185, LINE_WID, TFT_DARKGREY);

  tft.setTextFont(1);
  tft.setCursor(4, 190);
  tft.printf("Volts:%.2f ", volts);

  tft.setCursor(4, 200);
  tft.printf("bins  :%lu ", bin+1);
  
  tft.setCursor(4, 210);
  tft.printf("Boots:%lu"  , booted);

 
  // rx stats
  tft.setTextFont(2);
  tft.setCursor(4, 250);
  tft.print("Rx Stats" );
  tft.drawFastHLine(2, 270, LINE_WID, TFT_DARKGREY);

  tft.setTextFont(1);
  tft.setCursor(4, 280);
  tft.printf("Packets   : %lu ", (unsigned long)packets);
  tft.setCursor(4, 290);
  tft.printf("Pics Rxed : %lu ", (unsigned long)completed);
  tft.setCursor(4, 300);
  tft.printf("Dropped   : %lu ", (unsigned long)dropped);
  tft.setCursor(4, 310);
  tft.printf("Pics saved: %lu ", (unsigned long)binned);
}


  uint32_t test_counter     =  0;
  uint32_t test_end_counter = 500;

  #define TLV_TYPE_JPEG 0xbd 
  #define binNdx_MAX   5000


typedef struct tlv_header_t {
  uint32_t  type = TLV_TYPE_JPEG;
  uint32_t  len =  0;
} TLV_HEADER ;


File file;

static void RxTask(void *arg) {
  JpegMessage msg;
  char filename[32];

  //int      bins      = 0;
  bool     fyleOpen  = false;
  int binNdx = 0;



  for (;;) 
  {
 
      if (xQueueReceive(jpegQueue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE)
      {
        // got a pic. if sd card off or not enabled, just display pic and update status np matter what
          // we are saving the pic,
          // if the bin index is 0, we need to open a new bin file
          // a new bin file needs a new File name,
          // IT IS REMOVED first IF IT EXISTS         
          // AND THE FILE OPENED FOR WRITING.
          // save the pic at the current index  
          // increment the bin index and finish up the bin if needed

        TJpgDec.drawJpg(128, 34, msg.data, msg.length);

        // saving current pic
        if (sdReady && using_sd) 
        {
          bool wrote = true;

          if (binNdx == 0)
          {
              snprintf(filename, sizeof(filename), "/j_%05lu.jbn",
                      (unsigned long)bin  );

              // if file already exists, delete it!
              if (SD.exists(filename ) )
              {
                SD.remove(filename);
              }   
                file = SD.open(filename, FILE_WRITE);
                            if (file)   
                            {
                              fyleOpen = true;
                            }
          }   
            
          TLV_HEADER  header = { TLV_TYPE_JPEG , msg.length} ;
          
          // write header and jpeg 
            if (file) 
            {
              wrote = file.write ((uint8_t *)&header, sizeof(header) ) == sizeof(header) ;
              if (wrote)
                wrote = (file.write(msg.data, msg.length) == msg.length);
            }

            if ( wrote) {
                ++binned;
          
                binNdx ++;
            }
            

              // more bin to fill?
              // check for bin at MAX
            if (binNdx >= binNdx_MAX) 
            {
              binNdx = 0;
              // flush
              // close   
              file.flush();
              file.close();
              ++bin;                              
            }  

        } // saving  
            free(msg.data);

      } // qrx
              
    drawStatus(batteryVoltage());
    
  } // end for

}
 


// Existing SAR state machine, with per-picture malloc and transfer of ownership.
// Only basic packet-length and allocation-bound guards have been added.
void OnDataRecv(const esp_now_recv_info_t *rcv_info,
                const uint8_t *inBytes, int len) {
  ++packets;
  if (!matchMac(myPeer, rcv_info->src_addr)) return;
  if (len < (int)SAR_HEADER) return;
  const jNowMessage_t *jNowMess = (const jNowMessage_t *)inBytes;
  if (jNowMess->jMagic != jNOW_JMAGIC ||
      jNowMess->type != jNOW_TYPE_JSECURE ||
      jNowMess->subType != JS_SUBTYPE_SAR) return;
  const int newpixState = jNowMess->sarState;

  switch (pixState) {
    case 0:
      if (newpixState == JS_SAR_STATE_START) {
        seg = 0;
        piclen = jNowMess->len32;
        if (piclen == 0 || piclen > MAX_JPEG ||
            len < (int)(SAR_HEADER + SAR_PAYLOAD)) {
          ++dropped;
          break;
        }
        const size_t allocLen = ((size_t(piclen) + 217) / 218) * 218;
        rxBuff = (uint8_t *)malloc(allocLen);
        if (!rxBuff) { ++dropped; break; }
        memcpy(rxBuff, &inBytes[32], 218);
        pixState = 1;
        break;
      }
      break;
    case 1:

      if (newpixState == JS_SAR_STATE_MID ||
          newpixState == JS_SAR_STATE_END) {
 #if 1
        const size_t allocLen = ((size_t(piclen) + 217) / 218) * 218;
        const size_t nextOffset = size_t(seg + 1) * 218;
        if (len < (int)(SAR_HEADER + SAR_PAYLOAD) ||
            nextOffset + 218 > allocLen) {
          free(rxBuff);
          rxBuff = nullptr;
          pixState = 0;
          ++dropped;
          break;
        }
#endif
        ++seg;
        memcpy(rxBuff + (seg * 218), &inBytes[32], 218);
        if (newpixState == JS_SAR_STATE_END) {
          ++completed;
          JpegMessage msg = {rxBuff, piclen};
          if (xQueueSend(jpegQueue, &msg, 0) != pdTRUE) {
            free(rxBuff);
            ++dropped;
          }
          rxBuff = nullptr;
          pixState = 0;
        }
      }
      break;
    default:
      break;
  }
}

static void setupRadio() {
  WiFi.mode(WIFI_STA);
  // PASTE YOUR EXISTING ESP-NOW INITIALIZATION HERE:
  //   - channel setup
  //   - sender MAC copied into myPeer
  //   - encryption/PMK/peer configuration if applicable
  //   - esp_now_init()

  // Register the callback LAST, after your existing initialization:
WiFi.STA.begin();
// get channel from peer table
  readMacAddress();

  int nfFlag = 0;
  for (int i=0;i< sizeof(peer_table); i++)
  {
    if (matchMac(peer_table[i].peer_mac, myMacAddress)) 
    {
      Serial.printf("got chan i:%d %d\n", peer_table[i].channel,i);
      myChannel = peer_table[i].channel;
      memcpy(myPeer, peer_table[i].cam_mac,6);
      memcpy(peerInfo.peer_addr, myPeer , 6);
      // check and setup encryption
      if (peer_table[i].cryptFlags)
      {
        memcpy (peerInfo.lmk, peer_table[i].lmk, 16);
        memcpy(myPmk, peer_table[i].pmk, 16);
        peerInfo.encrypt = true;
      }

      nfFlag = 1;
      break;
    }
 }
    if (nfFlag == 0)
    {
      Serial.printf("something wrong with mac and peer, sorry!!");
      while (1) ;
     }


  esp_wifi_set_channel(myChannel, WIFI_SECOND_CHAN_NONE);
   
  esp_wifi_set_max_tx_power(84);
  
  if (esp_now_init() !=ESP_OK)
  {
      Serial.println("err now init");
      while (1) ;
  }

  esp_now_set_pmk( myPmk );

   // Add peer        
   if (esp_now_add_peer(&peerInfo) != ESP_OK){
     Serial.println("Failed to add peer");
     return;
 }



  esp_now_register_recv_cb(OnDataRecv);
}

void setup() {
  Serial.begin(115200);


  pinMode(BOOT_BUTTON, INPUT_PULLUP);

  booted = getBoots();

  tft.init();
  tft.setRotation(1); // Landscape: expected 480 x 320 for ST7796
  tft.fillScreen(TFT_BLACK);
  //tft.setTextColor(TFT_WHITE, TFT_BLACK);
 //tft.drawString("RxNode starting", 84, 8, 2);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC, ADC_11db);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(jpgOutput);



  sdReady = SD.begin(SD_CS); // Use your existing SD.begin(...) if different
  jpegQueue = xQueueCreate(QUEUE_DEPTH, sizeof(JpegMessage));
  if (!jpegQueue) { Serial.println("Queue allocation failed"); abort(); }
  xTaskCreatePinnedToCore(RxTask, "RxTask", 8192, nullptr, 1, nullptr, 1);
  drawStatus(batteryVoltage());


  setupRadio();
}

void loop() {  serviceBootButton(); delay(100); }
