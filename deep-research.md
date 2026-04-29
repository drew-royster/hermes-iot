# Full-Duplex AEC on Echo Pyramid

## Bottom line

The production Echo Pyramid hardware does appear to implement a real hardware-AEC path: the ES8311 differential DAC output is routed through a dedicated analog AEC network into the ES7210 MIC3 differential input, while MIC1 is the voice microphone. The crucial catch is that the board only brings the ES7210 **SDOUT1/TDMOUT** path back to the AtomS3R I2S input pin, and in ordinary non-TDM ES7210 mode, ADC12 and ADC34 are split across different serial outputs. That means MIC3 will not reliably show up on the stock single wired data line unless ES7210 is switched into a TDM output mode. In other words, your “ref slot stays idle in normal stereo capture” observation is not a mystery; it is exactly what the schematic, the ES7210 serial-output muxing rules, and the published M5 code imply. citeturn17view0turn18view0turn14view0turn41view0turn47view0

The practical implementation I would recommend is this: keep the ES8311 on its normal 16-bit slave I2S playback configuration, switch the **ES7210 output path** to TDM-I2S on SDOUT1/TDMOUT, and deinterleave the first two TDM slots as MIC1 voice and MIC3 reference. On stock hardware, that is the path with the highest confidence. I would **not** bet the project on ES8311 digital loopback on this board, because the published pin map exposes ES7210 ASDOUT to the ESP and does **not** expose ES8311 ASDOUT on the shared I2S input path. citeturn11view0turn14view0turn41view0turn40view0

## What the production hardware actually routes

The strongest evidence comes from the production schematic published by entity["company","M5Stack","embedded systems company"]. The ES7210 sheet labels MIC3P/MIC3N alongside the AEC nets, while the ES8311 DAC outputs are labeled DAC_P and DAC_N. Those DAC nets then feed a dedicated passive “AEC” network whose outputs are AEC_P and AEC_N, and those AEC nets return to the ES7210 MIC3 differential input. The same sheet also shows that the Si5351 CLK1 output fans out to both the ES8311 MCLK path and the ES7210 MCLK path through R27 and R28. citeturn18view0turn17view0turn13view0

The Echo Pyramid documentation independently matches the schematic. Its pin map lists the ES8311 on SCLK/LRCK/DSDIN and the ES7210 on SCLK/LRCK/ASDOUT, and it separately states that the Si5351 CLK1 feeds both `I2S_MCLK_DAC` and `I2S_MCLK_ADC`. That tells you two important things. First, the stock ESP receives ADC data from the ES7210 serial output, not from the ES8311. Second, there is only one published receive-data path from the audio front end back into the AtomS3R, and it is the ES7210 path. citeturn14view0

This layout is also consistent with entity["company","Espressif","semiconductor company"]’s documented hardware-AEC design on the ESP32-S3-Korvo-2. Espressif states that the default and recommended AEC reference source is the codec DAC output, and that the echo reference is collected by the ES7210 MIC3 differential input and sent back to the ESP for the AEC algorithm. Echo Pyramid is not a Korvo-2 clone, but the signal topology is the same in the place that matters: DAC analog output into ES7210 MIC3. citeturn33view0turn32view0

So the answer to the first core question is straightforward: **yes, production Echo Pyramid hardware does route ES8311 analog DAC output to ES7210 MIC3 through an analog AEC network.** The expected reference path is not “through another digital loopback path” on stock hardware; it is the analog DAC-to-MIC3 return. **Confidence: high.** citeturn17view0turn18view0turn33view0

## What the published M5 code configures today

The published M5Echo-Pyramid initialization sequence is: I2C bus up, Si5351 begin, I2S init, ES7210 begin, ES8311 begin, STM32 helper begin, then AW87559 begin. That means the board software is explicitly designed around the external shared MCLK coming from the Si5351 before codec bring-up, and around the speaker amp being enabled last. citeturn10view0turn13view0

The Si5351 code only enables **CLK1** and documents that CLK1 is routed to both `I2S_MCLK_DAC` and `I2S_MCLK_ADC`. Its supported MCLK values are 4.096 MHz for 16 kHz, 11.2896 MHz for 44.1 kHz, and 12.288 MHz for 48 kHz. That is exactly the clocking behavior you described in the hardware context. citeturn13view0

The ES8311 code is conservative and ordinary: it programs slave mode and 16-bit I2S, writing `0x0C` to the SDPIN and SDPOUT format registers. Nothing in the published M5 ES8311 path suggests TDM or a special digital reference mode is being used on Echo Pyramid. citeturn11view0

The ES7210 code, however, is where the AEC confusion starts. M5’s `ES7210::begin()` does all of the expected reset, clock, analog-bias, gain, power, and state-machine writes. But the critical serial-output configuration is `REG12 = 0x00`, which the public ES7210 register definition describes as `SDOUT_MODE = 00`, meaning **ADC12 to SDOUT1** and **ADC34 to SDOUT2**. On Echo Pyramid, the published pin map exposes the ES7210 ASDOUT path to the AtomS3R and does not expose SDOUT2 as a separate receive wire. So the stock M5 configuration powers MIC3, but then leaves MIC3 on the wrong serialized output path for this board. citeturn47view0turn14view0turn41view0

There is another important detail: the M5 source comments are not fully trustworthy. In the public datasheet, `REG45 = 0x1A` means MIC3 selected at 30 dB gain, and `0x1C` means MIC3 selected at 34.5 dB. Yet the M5 code comments `0x1C` as roughly 24 dB. Likewise, the public datasheet’s description of `REG4B` and `REG4C` conflicts with M5’s “0x0F = Fully open” comment. That mismatch explains why you were right to be suspicious of “safe” writes that did not change the real behavior. citeturn43view0turn44view0turn47view0

## Why the reference stays invisible in stock stereo capture

The simplest explanation is also the best-supported one: in normal, non-TDM ES7210 mode, the ADC is split into two serialized output groups. The public ES7210 register map says `SDOUT_MODE = 00` routes ADC12 to SDOUT1 and ADC34 to SDOUT2. MIC1 belongs to the ADC12 side; MIC3 belongs to the ADC34 side. Echo Pyramid only publishes the SDOUT1/TDMOUT receive path to the ESP. So when you read “stereo” data from the stock board path, you are **not** reading “MIC1 + MIC3”; you are reading whatever the wired SDOUT1 stream carries under the active mode. MIC3’s AEC signal can be physically present on the analog pins and still never reach the CPU in that configuration. citeturn41view0turn14view0turn18view0

That is also why your observation that “4-slot ES7210 TDM RX works for mic/WakeNet” fits the documentation. In ES7210 TDM-I2S mode, all channels are serialized onto the TDMOUT stream. The ES7210 datasheet’s TDM-I2S figure shows the slot order as **Channel 1, Channel 3, Channel 2, Channel 4**. Since Channel 1 is MIC1 and Channel 3 is MIC3, the first two TDM slots are the ones you care about for voice plus AEC reference. citeturn40view0turn36view0

That gives a very specific expected slot map on Echo Pyramid when ES7210 is put into TDM-I2S mode on the single wired output:

```text
slot 0 = MIC1  -> voice microphone
slot 1 = MIC3  -> DAC analog AEC reference
slot 2 = MIC2  -> unused on this board
slot 3 = MIC4  -> unused on this board
```

That slot order is supported directly by the ES7210 TDM-I2S timing diagram, and it matches the practical mapping used in the ES7210-plus-ES8311 hardware-reference implementation in the ESPHome intercom project. citeturn40view0turn29view1turn29view3

So the correct answer to the second core question is: **the reference stays “near idle” in normal stereo mode because stock Echo Pyramid software leaves ES7210 in a split-output mode where MIC3 is not on the single wired output pin.** **Confidence: high.** citeturn41view0turn14view0turn47view0

## The recommended bus topology and register delta

For stock Echo Pyramid hardware, the cleanest implementation is **not** “make ES8311 do AEC.” The cleanest implementation is: keep ES8311 in its normal playback role, and make ES7210 produce a MIC1-plus-MIC3 TDM stream on the single wired ADC data output. The minimum functional delta from the published M5 bring-up is therefore on the **ES7210 output mode**, not the ES8311 playback mode. citeturn11view0turn47view0turn41view0

The lowest-risk register delta, assuming you already have M5’s baseline initialization working for clocking and basic MIC1 capture, is:

```c
// After baseline ES7210 init, before starting your TDM receive path.
es7210_write(0x12, 0x02);  // SDOUT_MODE = 10 -> TDM I2S / Left-Justified
es7210_write(0x13, 0x00);  // automute off
es7210_write(0x43, 0x18);  // MIC1 selected, 24 dB (or 0x1A for 30 dB if needed)
es7210_write(0x45, 0x1A);  // MIC3 selected, 30 dB for AEC reference
```

The public register definitions support those meanings directly: `0x12 = 0x02` selects TDM-I2S output on the ES7210 serial data path, `0x43` and `0x45` bit 4 select MIC1 and MIC3 respectively, and nibble values 8 and 10 mean 24 dB and 30 dB gain. Turning automute off avoids the codec self-silencing the ref path when it thinks the signal is quiet. citeturn41view0turn43view0

I would **not** recommend making `0x4B` and `0x4C` part of the “must-change” debug delta unless you are rebuilding ES7210 bring-up from reset and validating on a logic analyzer. The public datasheet describes those registers as power/reset controls where `1` means various blocks are powered down or held reset, yet the M5 code writes `0x0F` and comments it as fully open. Since your own testing found that post-init writes to `0x4B/0x4C = 0x0F` killed capture on your path, the safest recommendation is to leave those power-path registers at whatever state already yields valid MIC1 audio, and focus on the serial-output mux and slot capture first. On this specific question, my confidence is only medium because the public datasheet and M5’s working code disagree. citeturn44view0turn47view0

For the ESP-IDF receive side, the conceptual recommendation is just as important as the register writes: **stop using M5’s `read()` helper**, because it assumes a 2-slot stereo frame and copies `buffer[i*2+0]` to mic and `buffer[i*2+1]` to ref. That helper cannot deinterleave a 4-slot TDM stream correctly. Once ES7210 is in TDM-I2S mode, you should read four 16-bit samples per audio frame and treat the first two as mic and ref. citeturn10view0turn40view0

A minimal deinterleave looks like this:

```c
// 4-slot TDM-I2S receive buffer, 16-bit samples
for (int i = 0; i < frames; ++i) {
    int16_t s0 = rx[i * 4 + 0];   // MIC1
    int16_t s1 = rx[i * 4 + 1];   // MIC3 / AEC reference
    mic[i] = s0;
    ref[i] = s1;
}
```

That mapping follows the ES7210 TDM-I2S timing diagram and matches field usage in the ES7210-plus-ES8311 TDM reference implementation. citeturn40view0turn29view1

## Initialization order and software ownership

The hardware bring-up order that best matches the published board support package is: start I2C, initialize the Si5351, program the shared MCLK, wait briefly for the clock tree to stabilize, initialize the I2S peripheral, initialize ES7210, initialize ES8311, then bring up the STM32 helper and the AW87559 speaker path. M5’s actual code follows that ordering, and the Si5351 code explicitly powers only CLK1, which feeds both codecs. citeturn10view0turn13view0

For clock values, the board support package supports exactly the rates you already listed: 16 kHz uses 4.096 MHz MCLK, 44.1 kHz uses 11.2896 MHz MCLK, and 48 kHz uses 12.288 MHz MCLK. For the narrow goal of proving the AEC path, 16 kHz end-to-end is the easiest debug mode because the AEC engine, wake-word stack, and STT front ends commonly want 16 kHz anyway. For the broader “humanlike full duplex” experience, a 48 kHz I2S bus with internal decimation to 16 kHz for AEC and voice processing is a good target, because the shared-codec stack runs more naturally at 48 kHz and third-party working implementations report better DAC quality that way. I would rate 16 kHz as **high-confidence for bring-up** and 48 kHz plus decimation as **medium-confidence for final polish** on this board. citeturn13view0turn10view0turn30view0

The other software trap is ownership of the I2S peripheral. The official ES8311 component documentation says the codec driver only handles the I2C control path and that the user is responsible for initializing I2S and starting audio streaming. The official esp_codec_dev documentation describes the data path as an `audio_codec_data_if_t`, and a recent esp-adf issue shows that opening separate ADC and DAC codec devices with separate I2S data-interface objects can lead to bad sample-format assumptions and broken slot configuration. My inference from those sources is this: **yes, esp_codec_dev can absolutely upset a hand-crafted TDM setup if it is allowed to own or reopen the I2S data interface after your manual initialization.** The safe pattern is to have one full-duplex I2S owner, or to use esp_codec_dev only for control-plane codec setup while you keep data-plane ownership yourself. citeturn20search17turn20search1turn20search3

## Direct answers to the open questions

**Does Echo Pyramid production hardware actually route ES8311 DAC analog output to ES7210 MIC3, or is AEC expected through another path?**  
Yes. The production schematic shows DAC_P/DAC_N feeding an AEC network whose outputs land on the ES7210 MIC3 differential input, and that mirrors Espressif’s documented “DAC output to ES7210 MIC3” AEC topology on Korvo-2. **Confidence: high.** citeturn17view0turn18view0turn33view0

**What exact ES7210 register sequence should enable MIC1 voice plus MIC3 DAC/reference capture on this board?**  
Starting from M5’s working baseline, the critical delta is `REG12 = 0x02` to move ES7210 onto TDM-I2S output on the single wired serial pin, with MIC1 and MIC3 explicitly selected and automute off. A practical sequence is `0x12=0x02`, `0x13=0x00`, `0x43=0x18` or `0x1A`, and `0x45=0x1A`. Leave the power-path registers alone if MIC1 already works, unless you are rebuilding from reset and validating carefully. **Confidence: high on `0x12/0x43/0x45/0x13`, medium on `0x4B/0x4C`.** citeturn41view0turn43view0turn44view0turn47view0

**In ES7210 TDM mode, what slot mapping should we expect for MIC1 and MIC3 on ESP-IDF I2S TDM?**  
For ES7210 TDM-I2S, the datasheet shows channel order `1, 3, 2, 4`. On Echo Pyramid that means slot 0 is MIC1 and slot 1 is MIC3, which is exactly the voice-plus-reference pair you want. **Confidence: high.** citeturn40view0turn36view0

**Should ES8311 remain standard stereo while ES7210 uses TDM RX, or should both TX and RX be TDM?**  
On stock Echo Pyramid, the ES8311 playback side should remain in its normal 16-bit slave I2S role. The change that matters is on the ES7210 output path and the ESP receive side, because that is how MIC3 becomes visible on the single wired input pin. I would not try to invent an “ES8311 TDM mode” solution for this board. **Confidence: high on the architectural answer, medium on any mixed-mode driver details inside one ESP-IDF controller because that depends on the exact I2S ownership model in your stack.** citeturn11view0turn14view0turn41view0

**Does esp_codec_dev reconfigure I2S after manual init in a way that breaks TDM assumptions?**  
Potentially yes. The official model separates control and data paths, and a recent esp-adf issue shows that opening codec devices can push bad standard-mode slot assumptions back into the data path. My inference is that if esp_codec_dev owns the I2S data-interface object, or if ADC and DAC reopen it independently, it can break a manual TDM receive setup. **Confidence: medium, because this conclusion is an inference from the official architecture plus a concrete bug report rather than a single explicit sentence in the docs.** citeturn20search17turn20search1turn20search3

**Is there an ES8311 digital loopback/reference mode we should use instead?**  
Probably not on stock Echo Pyramid. A community implementation for other boards uses ES8311 stereo digital feedback by programming register `0x44`, but Echo Pyramid’s published pin map exposes the ES7210 ADC output as the receive data source, not ES8311 ASDOUT. So even if ES8311 digital loopback exists, the stock board does not appear to route that return data back to the AtomS3R. Use MIC3 hardware reference first. **Confidence: medium on the “ES8311 supports it elsewhere” claim, high on “not the primary stock Echo Pyramid path.”** citeturn30view0turn14view0

**What exact initialization order and clock values should be used for Si5351, ES8311, ES7210, I2S, and AW87559?**  
Use the board-support ordering: I2C up, Si5351 begin, set CLK1 shared MCLK, wait briefly, initialize the shared I2S controller, initialize ES7210, initialize ES8311, initialize STM32 helper, and only then enable the AW87559 speaker path. Use 4.096 MHz for 16 kHz operation and 12.288 MHz for 48 kHz operation. **Confidence: high.** citeturn10view0turn13view0

**What concrete register writes and ESP-IDF I2S config should I try first?**  
First, keep the M5 board clocks exactly as shipped. Second, change ES7210 to `REG12=0x02`, keep automute off, select MIC1 and MIC3, and stop using the M5 stereo `read()` helper. Third, allocate your receive buffers for **4 slots per frame** and deinterleave slot 0 as MIC1 and slot 1 as MIC3. Fourth, do not let another framework reopen the bus in plain stereo mode after that. If you want the most conservative proof-of-life path, start at 16 kHz everywhere. If you want the best final UX, move to a 48 kHz shared bus and decimate mic and ref to 16 kHz for ESP-SR AEC and STT. **Confidence: high on the 16 kHz proof path, medium on the 48 kHz polished path.** citeturn41view0turn40view0turn10view0turn13view0turn30view0

The single most important correction, therefore, is not a fancy AEC algorithm tweak. It is this: **Echo Pyramid’s hardware AEC reference appears to be real, but stock non-TDM ES7210 output mode leaves MIC3 on the wrong serial output for the only receive pin that the AtomS3R can actually see.** Once you fix that muxing problem, the rest of the full-duplex barge-in stack becomes much more conventional. citeturn14view0turn17view0turn18view0turn41view0turn47view0