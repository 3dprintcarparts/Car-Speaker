# Car-Speaker
Arduino Sketch for setting up a WLAN accessable Speaker to play sounds. Android App will be published in App Store

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
> // =======================
> // Pins
> // =======================
> static const int PIN_STATUS_LED = 2;
> 
> static const int PIN_SD_CS   = 5;
> static const int PIN_SD_MOSI = 23;
> static const int PIN_SD_MISO = 19;
> static const int PIN_SD_SCK  = 18;
> 
> // AMP Control
> static const int PIN_AMP_SD   = 27;  // SD/EN
> static const int PIN_AMP_GAIN = 26;  // GAIN (optional!)
> static const bool AMP_SD_ACTIVE_HIGH = true;
> 
> // I2S
> static const int PIN_I2S_BCLK  = 14;
> static const int PIN_I2S_LRCLK = 25;
> static const int PIN_I2S_DOUT  = 32;

