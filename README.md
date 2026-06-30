# ESP32 IP Phone

New ESP-IDF project for a SIP phone based on the hardware wiring used by
`fufilius/esp32-PTT`.

## Current scope

- keep the PTT pinout as default project configuration;
- connect to Wi-Fi with power saving disabled and RTP-friendly STA settings;
- register a SIP account over UDP with Digest authentication;
- handle incoming calls: ring, answer, RTP audio, BYE/CANCEL;
- place an outgoing default call by long-pressing the main button;
- use G.711A/PCMA at 8 kHz for RTP audio in both directions;
- play local incoming ring and outgoing 425 Hz ringback tones;
- use the main button for answer, hang up, outgoing call, and outgoing cancel.

## PTT-compatible pinout

| Function | GPIO |
| --- | ---: |
| Main button | 25 |
| I2S microphone BCLK/SCK | 18 |
| I2S microphone WS | 19 |
| I2S microphone DIN/SD | 23 |
| I2S speaker BCLK | 5 |
| I2S speaker LRC/LRCLK | 17 |
| I2S speaker DIN/DOUT from ESP32 | 22 |
| I2C SDA | 21 |
| I2C SCL | 4 |
| Green status LED | 2 |
| Red status LED | 16 |
| microSD SCK | 14 |
| microSD MOSI | 13 |
| microSD MISO | 27 |
| microSD CS | 32 |

All defaults are in `sdkconfig.defaults` and can be changed through
`idf.py menuconfig` under `ESP32 IP Phone`.

## Wi-Fi setup

Set `ESP32 IP Phone -> Wi-Fi -> Wi-Fi SSID` and `Wi-Fi password` with:

```powershell
idf.py menuconfig
```

Then rebuild and flash:

```powershell
idf.py build flash monitor
```

For SIP use, Wi-Fi power saving is disabled and the station is configured for
20 MHz operation. The firmware logs RSSI, channel, and BSSID after connection so
radio conditions can be checked while testing calls.

## SIP setup

Set the first SIP account under `ESP32 IP Phone -> SIP account`:

- `SIP server host or IP`
- `SIP server port`
- `SIP username/extension`
- `SIP password`

The current SIP layer supports UDP `REGISTER` and `INVITE` Digest
authentication. Calls currently use G.711A/PCMA payload type 8 at 8000 Hz.

The default outgoing extension is configured as `1002` in `sdkconfig.defaults`
and can be changed in menuconfig.

## Button behavior

| State | Click | Hold |
| --- | --- | --- |
| Registered | ignored | call default extension |
| Calling | cancel outgoing call | cancel outgoing call |
| Ringing | answer | reject |
| In call | hang up | hang up |

## Next steps

1. Verify the board bring-up with `idf.py build flash monitor`.
2. Add persistent runtime settings or provisioning.
3. Improve jitter buffering and Wi-Fi-loss recovery during active calls.
4. Tune microphone gain for the final microphone module and enclosure.
