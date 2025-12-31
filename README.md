# Car-Speaker
Arduino Sketch for setting up a WLAN accessable Speaker to play sounds. Android App will be published in App Store. Initially the idea behind this ESP Code and the additional Android app, is a speaker to add into a car (over 12V Battery) so you can access a Speaker over WLAN to play Meme Sounds out loud of the Car :D

Needed electronic parts:
1) ESP32
2) i2s AMP
3) SD Card SPI Adapter
4) 5v stepdown konverter

Needed additionals:
1) SD Card
2) Speaker
3) Maybe an developer board for the esp32

the layout for the pins will follow in the readme. meanwhile its just available directly in the ESP Code or as Text:
> // =======================<br/>
> // Pins<br/>
> // =======================<br/>
> static const int PIN_STATUS_LED = 2;<br/>
> <br/>
> static const int PIN_SD_CS   = 5;<br/>
> static const int PIN_SD_MOSI = 23;<br/>
> static const int PIN_SD_MISO = 19;<br/>
> static const int PIN_SD_SCK  = 18;<br/>
> <br/>
> // AMP Control<br/>
> static const int PIN_AMP_SD   = 27;  // SD/EN<br/>
> static const int PIN_AMP_GAIN = 26;  // GAIN (optional!)<br/>
> static const bool AMP_SD_ACTIVE_HIGH = true;<br/>
> <br/>
> // I2S<br/>
> static const int PIN_I2S_BCLK  = 14;<br/>
> static const int PIN_I2S_LRCLK = 25;<br/>
> static const int PIN_I2S_DOUT  = 32;<br/>
<br/>
