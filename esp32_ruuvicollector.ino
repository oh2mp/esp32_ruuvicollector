/*
 * WiFi configurable ESP32 Ruuvi Collector
 * 
 * See https://github.com/oh2mp/esp32_ruuvicollector
 *
 */

#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <time.h>

#include "strutils.h"

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define APREQUEST 32            // grounding GPIO32 starts portal
#define APTIMEOUT 180000        // Portal timeout. Reboot after ms if no activity.
#define LED 2                   // Onboard led
#define MAX_TAGS 8

char tagdata[MAX_TAGS][25];     // space for raw tag data unparsed
char tagname[MAX_TAGS][24];     // tag names
char tagmac[MAX_TAGS][18];      // tag macs
uint8_t tagver[MAX_TAGS];       // protocol version tag uses
int tagrssi[MAX_TAGS];          // RSSI for each tag

char measurement[32];           // measurement name for InfluxDB
char userpass[67];              // user:pass for InfluxDB
char base64pass[89];            // user:pass base64 encoded
int  interval = 0;              // POST interval in milliseconds
                                //  0 = ASAP, that is practically approx 10s.
unsigned long last_duty = 0;

url_info urlp;
char url[128];
char scheme[6];
char host[64];
char path[64];
uint16_t port;

WiFiMulti WiFiMulti;
WiFiClientSecure httpsclient;
WiFiClient httpclient;

WebServer server(80);
IPAddress apIP(192,168,4,1);    // portal ip address
unsigned long portal_timer = 0;
char heardtags[MAX_TAGS][18];

File file;
BLEScan* blescan;

const int API_TIMEOUT = 4000; // timeout in milliseconds for http/https client

/* These formats are supposed to be compatible with RuuviCollector by Henrik "Scrin" Heikkilä
   https://github.com/Scrin/RuuviCollector
*/
const char fmt3[] = "temperature=%.02f,humidity=%.01f,pressure=%.02f,"\
                    "accelerationX=%.03f,accelerationY=%.03f,accelerationZ=%.03f,"\
                    "batteryVoltage=%.3f,rssi=%di";

const char fmt5[] = "temperature=%.02f,humidity=%.02f,pressure=%.02f,"\
                    "accelerationX=%.03f,accelerationY=%.03f,accelerationZ=%.03f,"\
                    "batteryVoltage=%.03f,txPower=%di,rssi=%di,"\
                    "movementCounter=%di,measurementSequenceNumber=%di\n";

/* -------------------------------------------------------------------------------
 * Parse ruuvi format v3 (RAWv1) into a char array.
 * https://github.com/ruuvi/ruuvi-sensor-protocols/blob/master/dataformat_03.md
*/

void parse_ruuvi_v3(char* rdata, const char* mfdata, const char* mac, int rssi) {

    double temperature = mfdata[4] & 0b01111111;
    temperature = temperature + ((double)mfdata[5] / 100);
    if (mfdata[4] & 0b10000000 == 0) {temperature = temperature * -1;}
    double pressure = ((unsigned short)mfdata[6]<<8) + (unsigned short)mfdata[7] + 50000;
    short accelX  = ((unsigned short)mfdata[8]<<8)  +  (unsigned short)mfdata[9];
    short accelY  = ((unsigned short)mfdata[10]<<8) +  (unsigned short)mfdata[11];
    short accelZ  = ((unsigned short)mfdata[12]<<8) +  (unsigned short)mfdata[13];
    short voltage = ((unsigned short)mfdata[14]<<8) +  (unsigned short)mfdata[15];

    sprintf(rdata,fmt3, temperature,(double)mfdata[3]/2, pressure,
            (double)accelX/1000,(double)accelY/1000,(double)accelZ/1000,double(voltage)/1000,rssi);
}
/* -------------------------------------------------------------------------------
 * Parse ruuvi format v5 (RAWv2) into a char array.
 * https://github.com/ruuvi/ruuvi-sensor-protocols/blob/master/dataformat_05.md
*/

void parse_ruuvi_v5(char* rdata, const char* mfdata, const char* mac, int rssi) {
    memset(rdata,0,sizeof(rdata));

    short temperature = ((short)mfdata[3]<<8) | (unsigned short)mfdata[4];
    unsigned short humidity = ((unsigned short)mfdata[5]<<8) | (unsigned short)mfdata[6];
    float pressure    = ((unsigned short)mfdata[7]<<8)  + (unsigned short)mfdata[8] + 50000;
    
    short accelX  = ((short)mfdata[9]<<8)  | (short)mfdata[10];
    short accelY  = ((short)mfdata[11]<<8) | (short)mfdata[12];
    short accelZ  = ((short)mfdata[13]<<8) | (short)mfdata[14];
    
    unsigned short foo = ((unsigned short)mfdata[15] << 8) + (unsigned short)mfdata[16];
    float voltage = ((double)foo / 32  + 1600)/1000;
    short power = (((mfdata[16] & 0x1f)*2)-40);
    unsigned short seqnumber = ((unsigned short)mfdata[18] << 8) + (unsigned short)mfdata[19];

    sprintf(rdata,fmt5,(float)temperature*0.005,(float)humidity*0.0025,pressure,
            (float)accelX/1000,(float)accelY/1000,(float)accelZ/1000,
            voltage,power,rssi,(unsigned short)(mfdata[17]),seqnumber);
}

/* ------------------------------------------------------------------------------- */

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advDev) {
        uint8_t taginx = getTagIndex(advDev.getAddress().toString().c_str());
        
        // we are interested about known and saved BLE devices only
        if (taginx == 0xFF) return;
        if (tagname[taginx][0] == 0) return;
        
        // we are interested only about Ruuvi tags (Manufacturer ID 0x0499)
        if (advDev.getManufacturerData()[0] == 0x99 && advDev.getManufacturerData()[1] == 4) {
            memset(tagdata[taginx],0,sizeof(tagdata[taginx]));

            // we accept only data format V3 or V5
            if (advDev.getManufacturerData()[2] == 3 || advDev.getManufacturerData()[2] == 5) {
                for (uint8_t i = 0; i < sizeof(advDev.getManufacturerData()); i++) {
                     tagdata[taginx][i] = advDev.getManufacturerData()[i];
                }
            
                tagrssi[taginx] = advDev.getRSSI();
                tagver[taginx] = advDev.getManufacturerData()[2];

                Serial.printf("BLE callback: %d %s ",int(taginx),advDev.getAddress().toString().c_str());
                for (uint8_t i = 0; i < sizeof(tagdata[taginx]); i++) {
                     Serial.printf("%02x",tagdata[taginx][i]);
                }
                Serial.print("\n");
            }
        }
    }
};
/* ------------------------------------------------------------------------------- */

class ScannedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advDev) {
        uint8_t taginx = getTagIndex(advDev.getAddress().toString().c_str());

        // skip known tags, we are trying to find new
        if (taginx != 0xFF) return;
        
        // we are interested only about Ruuvi tags (Manufacturer ID 0x0499)
        if (advDev.getManufacturerData()[0] == 0x99 && advDev.getManufacturerData()[1] == 4) {
            for (uint8_t i = 0; i < MAX_TAGS; i++) {
                 if (strlen(heardtags[i]) == 0) {
                     strcpy(heardtags[i],advDev.getAddress().toString().c_str());
                     Serial.printf("Heard new Ruuvi tag: %s\n",heardtags[i]);
                     break;
                 }
            }
        }
    }
};
/* ------------------------------------------------------------------------------- */
/* Get known tag index from MAC address. Format: 12:34:56:78:9a:bc */
uint8_t getTagIndex(const char *mac) {
    for (uint8_t i = 0; i < MAX_TAGS; i++) {
        if (strcmp(tagmac[i],mac) == 0) {
            return i;
        }
    }
    return 0xFF; // no tag with this mac found
}
/* ------------------------------------------------------------------------------- */
void loadSavedTags() {
   char sname[25];
   char smac[18];

   if (SPIFFS.exists("/known_tags.txt")) {
        uint8_t foo = 0;
        file = SPIFFS.open("/known_tags.txt", "r");
        while (file.available()) {
            memset(sname, '\0', sizeof(sname));
            memset(smac, '\0', sizeof(smac));
            
            file.readBytesUntil('\t', smac, 18);
            file.readBytesUntil('\n', sname, 25);
            trimr(smac);
            trimr(sname);
            strcpy(tagmac[foo],smac);
            strcpy(tagname[foo],sname);
            foo++;
            if (foo >= MAX_TAGS) break;
        }
        file.close();
    }
}

/* ------------------------------------------------------------------------------- */
void postToInflux() {
    digitalWrite(LED, HIGH);

    const char postdatafmt[] = "POST %s HTTP/1.1\nHost: %s\n"\
                               "Content-Length: %d\nAuthorization: Basic %s\n"\
                               "Connection: close\n\n%s\n";

    char tmpmac[16];
    int postlen = 0;
    unsigned long connecttime;
    
    for (uint8_t i = 0; i < MAX_TAGS; i++) {
         if (tagname[i][0] != 0) {
             postlen += strlen(tagdata[i]) + 128;
         }
    }
        
    char postmsg[512];
    char parsed[256];
    char postdata[MAX_TAGS*300];
    memset(postdata,0,sizeof(postdata));
        
    for (uint8_t i = 0; i < MAX_TAGS; i++) {
         if (tagname[i][0] != 0 && tagdata[i][0] != 0) {
             memset(tmpmac,0,sizeof(tmpmac));
             memset(postmsg,0,sizeof(postmsg));

             if (tagver[i] == 3) {
                 parse_ruuvi_v3(parsed, tagdata[i], tagmac[i], tagrssi[i]);
             }
             if (tagver[i] == 5) {
                 parse_ruuvi_v5(parsed, tagdata[i], tagmac[i], tagrssi[i]);
             }
             tagdata[i][0] = 0; // Set data to 0 if we have not got data for next iteration

             // Convert mac to upper case and strip colons for the database
             uint8_t k = 0;
             for (uint8_t j = 0; j < strlen(tagmac[i]); j++) {
                  if (tagmac[i][j] > 0x29 && tagmac[i][j] < 0x3A) {tmpmac[k++] = tagmac[i][j];}
                  if (tagmac[i][j] > 0x40 && tagmac[i][j] < 0x47) {tmpmac[k++] = tagmac[i][j];}
                  if (tagmac[i][j] > 0x60 && tagmac[i][j] < 0x67) {tmpmac[k++] = tagmac[i][j] & '_';}
             }
              
             sprintf(postmsg,"%s,dataFormat=%d,mac=%s,name=%s %s",measurement,tagver[i],tmpmac,tagname[i],parsed);
             strcat(postdata, postmsg);
         }
    }
    // If we don't have any data to send, just return
    if (strlen(postdata) == 0) {
        digitalWrite(LED, LOW);
        return;
    }

    WiFi.mode(WIFI_STA);
    if (WiFiMulti.run() == WL_CONNECTED) {
        Serial.printf("\nConnecting as %s from WiFi %s to %s\npost-data:\n",
                      WiFi.localIP().toString().c_str(),WiFi.SSID().c_str(), url);
          
        if (strcmp(scheme, "https") == 0) {
            if (httpsclient.connect(host, port)) {
                connecttime = millis();
                httpsclient.setTimeout(API_TIMEOUT);
                httpsclient.printf(postdatafmt, path, host, strlen(postdata), base64pass, postdata);
                Serial.printf(postdatafmt, path, host, strlen(postdata)-1, base64pass, postdata);

                // we reuse variable postmsg here
                while (httpsclient.connected() && millis() - connecttime < API_TIMEOUT) {
                    memset(postmsg,0,sizeof(postmsg));
                    httpsclient.read((uint8_t*)postmsg,511);
                    Serial.print(postmsg);
                }
                httpsclient.stop();
            } else {
                Serial.printf("%s connect failed.\n",scheme);
            }
        }
        if (strcmp(scheme, "http") == 0) {
            if (httpclient.connect(host, port)) {
                connecttime = millis();
                httpclient.setTimeout(API_TIMEOUT);
                httpclient.printf(postdatafmt, path, host, strlen(postmsg), base64pass, postmsg);

                // we reuse variable postmsg here
                while (httpclient.connected() && millis() - connecttime < API_TIMEOUT) {
                    memset(postmsg,0,sizeof(postmsg));
                    httpclient.read((uint8_t*)postmsg,511);
                    Serial.print(postmsg);
                }
                httpclient.stop();
            } else {
                Serial.printf("%s connect failed.\n",scheme);
            }
        }
   }
   digitalWrite(LED, LOW);
}

/* ------------------------------------------------------------------------------- */
void setup() {
    
    pinMode(APREQUEST, INPUT_PULLUP);
    pinMode(LED, OUTPUT);
    digitalWrite(LED, LOW);

    Serial.begin(115200);
    Serial.println("\n\nESP32 Ruuvi Collector by OH2MP 2020");
    
    SPIFFS.begin();

    loadSavedTags();
    
    int len;
    if (SPIFFS.exists("/influxdb.txt")) {
        file = SPIFFS.open("/influxdb.txt", "r");
        file.readBytesUntil('\n', url, 128);
        trimr(url);
        file.readBytesUntil('\n', userpass, 64);
        trimr(userpass);
        file.readBytesUntil('\n', measurement, 32);
        trimr(measurement);
        char intervalstr[8];
        file.readBytesUntil('\n', intervalstr, 8);
        file.close();
        
        interval = atoi(intervalstr);
        Serial.printf("Post interval: %d minutes\n",interval);
        
        b64_encode(userpass,base64pass, strlen(userpass));
    }

    if (SPIFFS.exists("/known_wifis.txt")) {
        char ssid[33];
        char pass[65];
        // WiFi.mode(WIFI_STA);  
        file = SPIFFS.open("/known_wifis.txt", "r");
        while (file.available()) {
            memset(ssid,'\0',sizeof(ssid));
            memset(pass,'\0',sizeof(pass));
            file.readBytesUntil('\t', ssid, 32);
            file.readBytesUntil('\n', pass, 64);
            trimr(ssid);
            trimr(pass);
            WiFiMulti.addAP(ssid, pass);
            Serial.printf("wifi loaded: %s / %s\n",ssid,pass);
        }
        file.close();
    } else {
        startPortal(); // no settings were found, so start the portal without button
    }
    // handle the InfluxDB url
    if (url[0] == 'h') {
        split_url(&urlp, url);

        Serial.printf("scheme %s\nhost %s\nport %d\npath %s\n\n",urlp.scheme, urlp.hostn, urlp.port, urlp.path);

        strcpy(scheme, urlp.scheme);
        strcpy(host, urlp.hostn);
        port = urlp.port;
        strcpy(path, urlp.path);
        sprintf(url,"%s://%s:%d%s\0",scheme,host,port,path);
    }

    BLEDevice::init("");
    blescan = BLEDevice::getScan();
    blescan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    blescan->setActiveScan(true);
    blescan->setInterval(100);
    blescan->setWindow(99);
}

/* ------------------------------------------------------------------------------- */
void loop() {
    struct tm* tm;
    time_t lt;
    time(&lt);
    tm = localtime(&lt);

    if (tm->tm_hour*60+tm->tm_min == interval || tm->tm_hour + tm->tm_min + tm->tm_sec == 0) {

        if ((tm->tm_sec == 0 || interval == 0) && portal_timer == 0) {
            timeval epoch = {0, 0};          // A little "misuse" of RTC, but we don't need it
            const timeval *tv = &epoch;
            settimeofday(tv, NULL);

            /* Current official Ruuvi firmware (v 2.5.9) https://lab.ruuvi.com/dfu/ broadcasts 
             * in every 6425ms in RAWv2 mode, so 9 seconds should be enough to hear all tags 
             * unless you have really many.
             */
            BLEScanResults foundDevices = blescan->start(9, false);
            blescan->clearResults();
            delay(250);
            postToInflux();
            delay(250);
        }
    }  
    if (portal_timer > 0) { // are we in portal mode?
        server.handleClient();
        if (int(millis()%1000) < 500) {
            digitalWrite(LED, HIGH);
        } else {
            digitalWrite(LED, LOW);
        }
    }
  
    if (digitalRead(APREQUEST) == LOW) {
        startPortal();
    }
    if (millis() - portal_timer > APTIMEOUT && portal_timer > 0) {
        Serial.println("Portal timeout. Booting.");
        delay(1000);
        ESP.restart();
    }
}

/* ------------------------------------------------------------------------------- */
/* Portal code begins here
 *  
 *   Yeah, I know that String objects are pure evil 😈, but this is meant to be
 *   rebooted immediately after saving all parameters, so it is quite likely that 
 *   the heap will not fragmentate yet. 
 */
/* ------------------------------------------------------------------------------- */

void startPortal() {
    Serial.print("Starting portal...");
    portal_timer = millis();

    for (uint8_t i = 0; i < MAX_TAGS; i++) {
        memset(heardtags[i],0,sizeof(heardtags[i]));
    }
    Serial.print("\nListening 10 seconds for new tags...\n");
    digitalWrite(LED, HIGH);

    // First listen 10 seconds to find new tags.
    blescan->setAdvertisedDeviceCallbacks(new ScannedDeviceCallbacks());
    blescan->setActiveScan(true);
    blescan->setInterval(100);
    blescan->setWindow(99);
    BLEScanResults foundDevices = blescan->start(10, false);
    blescan->clearResults();
    blescan->stop();
    blescan = NULL;
    BLEDevice::deinit(true);
    digitalWrite(LED, LOW);

    WiFi.disconnect();
    delay(100);

    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32 Ruuvi Collector");
    delay(2000);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

    server.on("/", httpRoot);
    server.on("/style.css", httpStyle);
    server.on("/influx.html", httpInflux);
    server.on("/saveinfl", httpSaveInflux);
    server.on("/wifis.html", httpWifi);
    server.on("/savewifi", httpSaveWifi);
    server.on("/sensors.html", httpSensors);
    server.on("/savesens",httpSaveSensors);
    server.on("/boot", httpBoot);
    
    server.onNotFound([]() {
        server.sendHeader("Refresh", "1;url=/"); 
        server.send(404, "text/plain", "QSD QSY");
    });
    server.begin();
    Serial.println("Portal running.");
}
/* ------------------------------------------------------------------------------- */

void httpRoot() {
    portal_timer = millis();
    String html;
    
    file = SPIFFS.open("/index.html", "r");
    html = file.readString();
    file.close();    
    
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpInflux() {
    portal_timer = millis();
    String html;
    
    file = SPIFFS.open("/influx.html", "r");
    html = file.readString();
    file.close();
    
    html.replace("###URL###", String(url));
    html.replace("###USERPASS###", String(userpass));
    html.replace("###IDBM###", String(measurement));
    html.replace("###INTERVAL###", String(interval));
       
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSaveInflux() {
    portal_timer = millis();
    String html;
        
    file = SPIFFS.open("/influxdb.txt", "w");
    file.printf("%s\n",server.arg("url").c_str());
    file.printf("%s\n",server.arg("userpass").c_str());
    file.printf("%s\n",server.arg("idbm").c_str());
    file.printf("%s\n",server.arg("interval").c_str());
    file.close();

    // reread
    file = SPIFFS.open("/influxdb.txt", "r");
    file.readBytesUntil('\n', url, 128);
    trimr(url);
    file.readBytesUntil('\n', userpass, 64);
    trimr(userpass);
    file.readBytesUntil('\n', measurement, 32);
    trimr(measurement);
    char intervalstr[8];
    file.readBytesUntil('\n', intervalstr, 8);
    interval = atoi(intervalstr);
    file.close();

    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "2;url=/");
    server.send(200, "text/html; charset=UTF-8", html);    
}
/* ------------------------------------------------------------------------------- */

void httpWifi() {
    String html;
    char tablerows[1024];
    char rowbuf[256];
    char ssid[33];
    char pass[33];
    int counter = 0;
    
    portal_timer = millis();
    memset(tablerows, '\0', sizeof(tablerows));
    
    file = SPIFFS.open("/wifis.html", "r");
    html = file.readString();
    file.close();
    
    if (SPIFFS.exists("/known_wifis.txt")) {
        file = SPIFFS.open("/known_wifis.txt", "r");
        while (file.available()) {
            memset(rowbuf, '\0', sizeof(rowbuf)); 
            memset(ssid, '\0', sizeof(ssid));
            memset(pass, '\0', sizeof(pass));
            file.readBytesUntil('\t', ssid, 33);
            file.readBytesUntil('\n', pass, 33);
            sprintf(rowbuf,"<tr><td>SSID</td><td><input type=\"text\" name=\"ssid%d\" maxlength=\"32\" value=\"%s\"></td></tr>",counter,ssid);
            strcat(tablerows,rowbuf);
            sprintf(rowbuf,"<tr><td>PASS</td><td><input type=\"text\" name=\"pass%d\" maxlength=\"32\" value=\"%s\"></td></tr>",counter,pass);
            strcat(tablerows,rowbuf);
            counter++;
        }
        file.close();
    }
    html.replace("###TABLEROWS###", tablerows);
    html.replace("###COUNTER###", String(counter));
    
    if (counter > 3) {
        html.replace("table-row", "none");
    }
    
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSaveWifi() {
    portal_timer = millis();
    String html;
        
    file = SPIFFS.open("/known_wifis.txt", "w");
    
    for (int i = 0; i < server.arg("counter").toInt(); i++) {
         if (server.arg("ssid"+String(i)).length() > 0) {
             file.print(server.arg("ssid"+String(i)));
             file.print("\t");
             file.print(server.arg("pass"+String(i)));
             file.print("\n");
         }
    }
    // Add new
    if (server.arg("ssid").length() > 0) {
        file.print(server.arg("ssid"));
        file.print("\t");
        file.print(server.arg("pass"));
        file.print("\n");
    }    
    file.close();

    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "2;url=/");
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSensors() {
    String html;
    char tablerows[1024];
    char rowbuf[256];
    int counter = 0;
    
    portal_timer = millis();
    memset(tablerows, '\0', sizeof(tablerows));
    
    file = SPIFFS.open("/sensors.html", "r");
    html = file.readString();
    file.close();

    loadSavedTags();
    
    for(int i = 0 ; i < MAX_TAGS; i++) {
        if (strlen(tagmac[i]) == 0) continue;
        
        sprintf(rowbuf,"<tr><td>%s<br /><input type=\"text\" name=\"sname%d\" maxlength=\"24\" value=\"%s\">",
                       tagmac[i],counter,tagname[i]);
        strcat(tablerows,rowbuf);                       
        sprintf(rowbuf,"<input type=\"hidden\" name=\"saddr%d\" value=\"%s\"></td></tr>",counter,tagmac[i]);
        strcat(tablerows,rowbuf);
        counter++;
    }
    if (strlen(heardtags[0]) != 0 && counter < MAX_TAGS) {
        for(int i = 0; i < MAX_TAGS; i++) {
            if (strlen(heardtags[i]) == 0) continue;
            if (getTagIndex(heardtags[i]) != 0xFF) continue;
            
            sprintf(rowbuf,"<tr><td>%s<br /><input type=\"text\" name=\"sname%d\" maxlength=\"24\">",
                    heardtags[i],counter);
            strcat(tablerows,rowbuf);
            sprintf(rowbuf,"<input type=\"hidden\" name=\"saddr%d\" value=\"%s\"></td></tr>",
                           counter,heardtags[i]);
            strcat(tablerows,rowbuf);
            counter++;
            if (counter > MAX_TAGS) break;
        }
    }

    html.replace("###TABLEROWS###", tablerows);
    html.replace("###COUNTER###", String(counter));
            
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSaveSensors() {
    portal_timer = millis();
    String html;
        
    file = SPIFFS.open("/known_tags.txt", "w");
    
    for (int i = 0; i < server.arg("counter").toInt(); i++) {
         if (server.arg("sname"+String(i)).length() > 0) {
             file.print(server.arg("saddr"+String(i)));
             file.print("\t");
             file.print(server.arg("sname"+String(i)));
             file.print("\n");
         }
    }
    file.close();
    loadSavedTags(); // reread

    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "2;url=/");
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpStyle() {
    portal_timer = millis();
    String css;

    file = SPIFFS.open("/style.css", "r");
    css = file.readString();
    file.close();       
    server.send(200, "text/css", css);
}
/* ------------------------------------------------------------------------------- */

void httpBoot() {
    portal_timer = millis();
    String html;
    
    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();
    
    server.sendHeader("Refresh", "2;url=about:blank");
    server.send(200, "text/html; charset=UTF-8", html);
    delay(1000);
    
    timeval epoch = {0, 0}; // clear clock
    const timeval *tv = &epoch;
    settimeofday(tv, NULL);

    ESP.restart();
}
/* ------------------------------------------------------------------------------- */
