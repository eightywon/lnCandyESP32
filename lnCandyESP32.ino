#include <WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <GxEPD2_BW.h>
#include "qrcode.h"
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Servo.h>
#include <EEPROM.h>
#include "dorian.h"

//OTA updates
#include <WebServer.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <Update.h>
#include "ota.h"

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
WiFiServer webhookServer(39780); //webhook listener (for monitoring payments)
WebServer ota(80);
Servo servo;

//BUSY->25, RST->26, DC->27, CS->15, CLK->13, DIN->14
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(GxEPD2_154_D67(15,27,26,25));

const char *ssid="yourwifissid", *pass="yourwifipass", *lnBitsAPIKey="yourlnbitswalletapikey", *webhookEndpoint="http://yourwebhookendpoint:39780";
const char *LNhost="lnbits.com", *invoiceEndpoint="https://lnbits.com/api/v1/payments", *thisHost="lncandy";

const uint32_t invoiceExpOffset=(24*60*60); //LNbits invoices expire after 24 hours - after this period, we request a new invoice and generate new QR code
const uint16_t webhookTimeout=2000, candyCost=25, maxInvRetries=10, invoiceRetryWait=30, servoRunFor=1050; //(PB M&Ms=1050)
const int16_t servoRotation=-180; //pos=clockwise, neg=counter-clockwise (counter for great northern gumball machine)

const bool debug=true; //set to true for serial debugging output

String paymentHash, paymentRequest, tempString;
uint32_t invoiceExpiry, unitsSold, servoStartedAt;

int16_t tbx,tby; uint16_t tbw,tbh,x,y;

void setup() {
  Serial.begin(115200);
  SPI.end(); // release standard SPI pins, e.g. SCK(18), MISO(19), MOSI(23), SS(5)
  SPI.begin(13, 12, 14, 15); // map and init SPI pins SCK(13), MISO(12), MOSI(14), SS(15)
  
  display.init(115200);
  display.setRotation(1);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setFullWindow();
  showDorian(true);
  
  WiFi.begin(ssid,pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  display.fillScreen(GxEPD_WHITE);
  tempString=WiFi.localIP().toString(); displayText(tempString,0,0,true);
  Serial.println(""); Serial.print("WiFi connected, "); Serial.print("IP address: "); Serial.println(WiFi.localIP());
  delay(1000);
  timeClient.begin();
  webhookServer.begin();
  
  EEPROM.begin(32);
  EEPROM.get(0, unitsSold);
  if (unitsSold == -1) {
    //no value stored in EEPROM, set to 0
    unitsSold++;
    EEPROM.put(0, unitsSold); EEPROM.commit();
  }

  //create the task on core 0 that monitors for servo stop timing
  //putting this on a different core allows finer tuned control of total servo rotation
  xTaskCreatePinnedToCore(stopServo,"stopServo",10000,NULL,0,NULL,0); 
  
  if (!MDNS.begin(thisHost)) {
    Serial.println("Error setting up MDNS responder!");
    while (1) {
      delay(1000);
    }
  }
  Serial.println("mDNS responder started");
  //return index page which is stored in Index
  ota.on("/", HTTP_GET, []() {
    ota.sendHeader("Connection", "close");
    ota.send(200, "text/html", serverIndex);
  });
  //handling uploading firmware file
  ota.on("/update", HTTP_POST, []() {
    ota.sendHeader("Connection", "close");
    ota.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = ota.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      //flashing firmware to ESP
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });
  ota.begin();
}

void loop() {
  handleWebhook();
  checkSerialIn();
  timeClient.update();
  if ((timeClient.getEpochTime()>=invoiceExpiry)) {
    Serial.println("invoice expired, creating a new one");
    paymentHash=createInvoice();
    display.hibernate();
  }
  ota.handleClient(); //testing OTA
}

void handleWebhook() {
  WiFiClient client=webhookServer.available();
  if (client) {
    uint32_t start=millis();
    Serial.println("!!POSSIBLE PAYMENT INCOMING!!");
    String currentLine="";
    bool inJsonBody=false;
    JSONVar respBody;
    
    //here we're parsing the post body until we've assembled what passes as a valid JSON object
    //meaning that we've received the whole body
    while (client.connected() && ((start+webhookTimeout)>millis())) {
      if (client.available()) {
        char c=client.read();
        if (debug) Serial.write(c);
        if (c=='{' && !inJsonBody) {
          inJsonBody=true;
          currentLine+=c;
        } else if (inJsonBody && c!='\n' && c!='\r') {
          currentLine+=c; 
        }
      }
      respBody=JSON.parse(currentLine);
      if (JSON.typeof(respBody)!="undefined") break; //we've received a valid json object, exit the loop
    }
    
    if (debug) {
      Serial.print("possibile payment object: "); Serial.println(currentLine);
    }
    
    if (JSON.typeof(respBody)!="undefined") {
      //does the payment hash we received in the webhook post body match the payment hash
      //we're expecting (i.e. the payment hash of the current invoice)
      if (respBody["payment_hash"]==paymentHash) {
        //valid payment received, respond http 200, dispense candy, create and display new invoice
        client.println("HTTP/1.1 200 OK");
        client.println("Content-type:text/html");
        client.println("Connection: close");
        client.println(); client.println();
        unitsSold++;
        dispenseCandy();
        Serial.println("payment_hash confirmed, dispense candy and create new invoice");
        showDorian(false);
        Serial.print("units sold: "); Serial.println(unitsSold);
        EEPROM.put(0,unitsSold); EEPROM.commit();
        paymentHash=createInvoice();
      } else {
        Serial.println("invalid payment hash from webhook, ignoring");
        client.println("HTTP/1.1 400");
        client.println("Content-type:text/html");
        client.println("Connection: close");
        client.println(); client.println();
      }
    } else {
        //never got proper payment header
        Serial.println("invalid payment hash from webhook, ignoring");
        client.println("HTTP/1.1 400");
        client.println("Content-type:text/html");
        client.println("Connection: close");
        client.println(); client.println();
    }
    client.stop();
  }
}

String createInvoice() {
  HTTPClient rest;

  //create post body object
  JSONVar postData;
  postData["out"]=false;
  postData["amount"]=candyCost;
  timeClient.update(); String memo="For Candy at "; memo.concat(timeClient.getEpochTime());
  postData["memo"]=memo;
  postData["webhook"]=webhookEndpoint;

  //make invoice creation API call to LNbits
  int httpResponseCode=-1;
  int retries=0;
  while (httpResponseCode!=201 && retries<maxInvRetries) {
    retries++;
    rest.begin(invoiceEndpoint);
    rest.addHeader("Content-Type", "application/json");
    rest.addHeader("Host", LNhost);
    rest.addHeader("X-Api-Key", lnBitsAPIKey);
    httpResponseCode=rest.POST(JSON.stringify(postData));
    if (httpResponseCode==201) {
      JSONVar respObject=JSON.parse(rest.getString());
      paymentHash=respObject["payment_hash"];
      paymentRequest=respObject["payment_request"];
      timeClient.update();
      invoiceExpiry=timeClient.getEpochTime() + invoiceExpOffset;
      Serial.print("invoice created, expires at "); Serial.print(invoiceExpiry); Serial.println(":"); Serial.println(respObject);
      rest.end();
      createInvoiceQR();
      return paymentHash;
    } else {
      Serial.print("unable to generate invoice from lnbits, http status: ");
      Serial.println(httpResponseCode);
      display.firstPage();
      display.fillScreen(GxEPD_WHITE);
      displayText("INV. CREATE ERR",0,32,false);
      tempString=""; tempString=LNhost; tempString+=": "; tempString+=httpResponseCode; displayText(tempString,0,8,false);
      tempString=""; tempString=retries; tempString+="/"; tempString+=maxInvRetries; displayText(tempString,0,-16,true);
      rest.end();
    }
  }
  Serial.println("couldn't create invoice, waiting 60 seconds");
  tempString="Waiting "; tempString+=invoiceRetryWait; tempString+="s"; displayText(tempString,0,-112,true);
  uint32_t until=(millis()+(invoiceRetryWait*1000));
  while (millis()<until) {
    uint16_t now=millis();
    uint16_t elapsed=now-(until-(invoiceRetryWait*1000));
    float prog=float(elapsed)/float(invoiceRetryWait*1000);
    updateProgress(int(prog*100));
  }
  display.setFullWindow();
  //clear ghosting
  for (int i=0;i<=2;i++) {
    display.firstPage();
    do {
      display.fillScreen(GxEPD_BLACK);
    } while (display.nextPage());
    
    display.firstPage();
    do {
      display.fillScreen(GxEPD_WHITE);
    } while (display.nextPage());
  }
  return "";
}

bool createInvoiceQR () {
  Serial.println("creating QR code from payment_request");
  display.fillScreen(GxEPD_WHITE);
  displayText("SCAN TOP PAY",0,92,true);
  // Create the QR code
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(11)];

  //need a test here to see if paymentRequest exceeds v11 QR data alphanumeric encoding limit of 468B
  //if we exceed, display error or ?? maybe code to dynamically adjust QR version and reposition on screen?
  paymentRequest.toUpperCase(); //BOLT11 spec supports payment request in all caps - this allows us to stay in the
                                //QR code alphanumeric (0-9, A-Z) space as opposed to binary (mixed case), allowing
                                //for support of longer payment requests in same-sized (v11) QR codes
  qrcode_initText(&qrcode, qrcodeData, 11, 0, paymentRequest.c_str()); //QR code v11, 61x61, 468 bytes of info

  byte box_x=8;  //start drawing 8 from left
  byte box_y=16; //start drawing 16 from top
  byte box_s=3;  //make each square 3 units, 61*3=183px
  byte init_x=box_x;

  for (uint8_t y=0; y<qrcode.size; y++) {
    for (uint8_t x=0; x<qrcode.size; x++) {
      if (qrcode_getModule(&qrcode,x,y)) {
        display.fillRect(box_x, box_y, box_s, box_s, GxEPD_BLACK);
      }
      box_x=box_x+box_s;
    }
    box_y=box_y+box_s;
    box_x=init_x;
  }
  display.display(false);
  return true;
}

void dispenseCandy() {
  if (!servo.attach(32)) Serial.println("servo failed to attach");
  servoStartedAt=millis();
  if (debug) Serial.printf("should detach at %d\n",servoStartedAt+servoRunFor);
  servo.write(servoRotation);
}

void stopServo( void * pvParameters ) {
  while (true) {
    if (servoStartedAt!=0 && millis()>=(servoStartedAt+servoRunFor)) {
      uint16_t now=millis();
      servo.detach();
      servoStartedAt=0;
      if (debug) Serial.printf("detaching servo at %d\n\n",now);
    }
    delay(1);
  }
  Serial.print("killing task");
  vTaskDelete(NULL);
}

void showDorian(bool boot) {
  display.fillScreen(GxEPD_WHITE);
  //###1.01 REFACTOR THIS
  if (!boot) {
    displayText("ENJOY YOUR CANDY",0,-188,true);
  }
  display.drawInvertedBitmap(0, 0, dorian, 200, 180, GxEPD_BLACK);
  display.display(false);
}

void checkSerialIn() {
  //for testing dispense without actually making a ln payment
  //send "pay" in serial console to trigger
  char c;
  String input;
  
  while (Serial.available()>0) {
    c=Serial.read();
    if (c!='\n') {
      input+=c;
    } else {
      break;
    }
  }
  
  if (input=="pay") {
    dispenseCandy();
    servo.write(servoRotation);
  }
}

void updateProgress(int percent) {
  display.setPartialWindow(0,0,display.width(),display.height());
  percent*=2;
  if (percent<=display.width()-12) {
    display.setCursor(percent-2,188); display.print("|");
    display.setCursor(percent-1,188); display.print("|");
    display.setCursor(percent,188); display.print("|");
  }
  display.display(true);
}

void displayText(String temp, uint16_t xOffset, uint16_t yOffset, bool paint) {
  display.getTextBounds(temp,xOffset,yOffset,&tbx,&tby,&tbw,&tbh);
  x=((display.width()-tbw)/2)-tbx;
  y=((display.height()-tbh)/2)-tby;
  display.setCursor(x,y);
  display.print(temp);
  if (paint) display.display(false);
}
