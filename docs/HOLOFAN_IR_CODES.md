# Holofan IR remote codes (reference only)

These are the codes of the stock infrared remote that comes with the fan.
This firmware does not use IR: playback commands arrive over the WCB mesh.
The codes are kept for anyone who wants to drive a stock fan by IR.

Protocol: NEC, 32 bits, address 0x0, 38 kHz carrier, captured from the
fan's own remote.

    Button            NEC code
    ----------------  --------
    Play              0xFF7A85
    Pause             0xFF30CF
    Next video        0xFFC23D
    Previous video    0xFF22DD
    Brightness high   0xFF10EF
    Brightness low    0xFF5AA5
    Volume on         0xFFA25D
    Volume off        0xFFE21D
    Rotate left       0xFFE01F
    Rotate right      0xFF906F
    Repeat current    0xFF6897
    Repeat next       0xFFB04F

A transmitter that worked: GPIO -> 1 kOhm -> 2N2222 base; collector -> IR LED
-> 47 Ohm -> 5 V (about 70 mA LED current). It was captured with a SparkFun
WiFi IR Blaster (ESP8266) running a small capture sketch.
