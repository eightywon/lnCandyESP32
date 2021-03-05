# lnCandyESP32  
An esp32-controlled candy machine that accepts bitcoin (lightning network) payments. Very short demo - https://www.youtube.com/watch?v=qnh52xSBXpo. Pardon the music, I blame the kids >.>  

Tf is bitcoin? - https://www.youtube.com/watch?v=bBC-nXj3Ng4  

...then what's ln/lightning network? - https://www.youtube.com/watch?v=rrr_zPmEiME  

# Hardware  
* Servo (any small 360* rotation servo should work) - https://www.amazon.com/gp/product/B07FVLQ94C/ref=ppx_yo_dt_b_asin_title_o05_s00?ie=UTF8&psc=1  
* Candy machine - https://www.amazon.com/gp/product/B0055OWLFI/ref=ppx_yo_dt_b_asin_title_o09_s02?ie=UTF8&psc=1  
* esp32 (I used Adafruit's feather, but other boards will work with small changes to pint settings etc) - https://www.amazon.com/Adafruit-HUZZAH32-ESP32-Feather-Board/dp/B01NCRYHDL  
* ePaper display board - https://www.amazon.com/gp/product/B0728BJTZC/  

Note on the ePaper display - I tried switching to one of those cool 3-color models (b/w/r or b/w/y... yellow lightning logo, anyone?!?!) but the refresh rate on those is still horribly slow so I'd stick with b/w for this project.  It caused timing problems and panic reboots on the esp32, plus waiting 20 seconds for a new invoice isn't a great user experience.  

Hardware/install details - https://www.youtube.com/watch?v=ECt_lBkswTw  

# Software  
Update line 21 in the sketch:  
> const char *ssid="yourwifissid", *pass="yourwifipass", *lnBitsAPIKey="yourlnbitswalletapikey", *webhookEndpoint="http://yourwebhookendpoint:39780";  

...to contain your wifi SSID and password (version 2 will have a captive portal for wifi setup and will allow OTA updates, but not supported yet), your LNbits api key (available after creating a custodial wallet at https://lnbits.com), and your webhook endpoint.  

The webhook endpoint is your esp32. If your internet connection has a static IP address, use your IP, e.g. http://55.44.33.22:39780. If you don't have a static IP (it's changed periodically by your ISP), register for a free dynamic DNS service. I use https://duckdns.org. This gives you a DNS name that will always point to your IP address, e.g. https://SOMEHOSTNAME.duckdns.org where SOMEHOSTNAME is the name you picked when creating your duckdns account.  

Then, in your router configuration settings, port forward TCP port 39780 to your esp32's private IP address within your network. More info here - https://www.howtogeek.com/66214/how-to-forward-ports-on-your-router/  

Using port 39780 isn't required, that's just what I used and it's hard-coded in the sketch. You can change this throughout the code if you're familiar/comfortable and want to use a different port (0-65353, just stay non-standard).  

Setting up the webhook allows your esp32 to listen for successful payments. When an invoice is paid, LNbits' API will send an http post to whatever webhook endpoint was specified when invoice was created (the webhook endpoint we define in our code, in this case).  

This webhook is handled in the sketch and is what triggers the servo to disepnse candy and a new invoice to be created for the next sale. In this way, LNBits can "push" to us the fact that a payment has been made rather than us polling their servers continuously checking for the completed payment ("pulling") .  


