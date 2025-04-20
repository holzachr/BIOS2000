# BIOS 2000
## An open-source cocktail machine and shot dispenser

- Peristaltic pumps are used to deliver liquids from bottles, 6 in this case, through 3x5 mm (inner and outer diameter) silicon hoses to an output.
- A simple web interface can be used to compose mixed drinks or just dispense shots.
- An RFID reader offers an "instant shot mode" after the tag has been read and a button pressed.

## Hardware

Main controller of this project is an ESP8266 "NodeMCU" module, programmed through the Arduino IDE.

The type G528 pumps are available in different sizes, 6 and 12 V, and different speeds. Because even the largest pump is quite slow in terms of typical drink volume, you want to get the biggest 150 ml/s 12 V variant.

Each two pumps are driven by a L298N H-bridge, and each H-Bridge is controlled by two GPIO lines.

To save GPIO lines from the ESP module, PCF8574 I2C-to-GPIO expanders are used, but this is generally optional.

The MFRC522 RFID reader modules generally only support SPI modes, but can be modified in order to use their I2C bus. This requires some fine soldering skills - check out the "Hardware" directory.

A LED strip offers fancy lighting.

Because my machine case is an old PC case, I repurposed the HDD activity LED to flicker randomly when the machine is working.

## ESP/Arduino libraries

- https://github.com/me-no-dev/ESPAsyncWebServer for the web server
- https://github.com/me-no-dev/ESPAsyncTCP for WebSocket
- https://github.com/bblanchon/ArduinoJson for data exchange between Arduino and web code
- https://github.com/RobTillaart/PCF8574/ for the IO expanders
- https://github.com/arozcan/MFRC522-I2C-Library/ for the RFID reader
- https://github.com/FastLED/FastLED for an optional LED strip

## Pump calibration

The pump variation is quite large, and you want to make sure that the drinks don't spill over.
That's what the calibration.html website on the ESP is for:
You dispense a drink, say 300 ml, from each pump. You measure or weight how much you actually got, and use the calibration.html website to calculate individual per-pump ml/s values, which are compiled into the code (nasty).
Make sure you adapt those constants to your individual pumps.
