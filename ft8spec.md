# FT8 Specification and Frequency Convention

## 1. Overview

FT8 is an amateur-radio weak-signal digital mode developed within the WSJT family of modes. It is designed for reliable communication under weak-signal conditions.

The most important frequency concept is:

> **The FT8 frequency published for a band (for example, 14.074 MHz on 20 m) is normally the transceiver's USB dial frequency, not the RF center frequency of every individual FT8 transmission.**

WSJT-X generates FT8 audio tones in the computer's audio passband. When the transceiver is operated in USB, those audio frequencies are translated above the radio's dial/carrier reference frequency.

For an ideal USB transmitter:

\[
f_{RF} = f_{dial} + f_{audio}
\]

Thus, if the radio is tuned to 14.074000 MHz and WSJT-X transmits at 1500 Hz audio:

\[
14.074000\ \text{MHz} + 0.001500\ \text{MHz}
= 14.075500\ \text{MHz}
\]

So the RF signal at the antenna is approximately **14.075500 MHz**.

---

## 2. FT8 Signal Characteristics

FT8 uses 8-FSK (eight-frequency shift keying).

Typical fundamental parameters include:

| Parameter | Value |
|---|---:|
| Modulation | 8-FSK |
| Tone spacing | 6.25 Hz |
| Number of tones | 8 |
| Symbol duration | 160 ms |
| Number of transmitted symbols | 79 |
| Nominal transmission duration | ~12.64 s |
| Typical sequence period | 15 s |
| Approximate occupied bandwidth of one FT8 signal | ~50 Hz |

The individual FT8 signal is therefore very narrow compared with the approximately 3 kHz audio passband normally used by WSJT-X.

The exact interpretation of "bandwidth" depends on the measurement convention, but the commonly cited nominal FT8 occupied signal bandwidth is about **50 Hz**.

---

## 3. Standard FT8 Dial Frequencies

The commonly used FT8 frequencies below are **USB dial/reference frequencies**:

| Amateur band | WSJT-X / transceiver dial frequency | Approximate RF range if using a 0–3 kHz USB audio passband |
|---|---:|---:|
| 160 m | 1.840 MHz | ~1.840–1.843 MHz |
| 80 m | 3.573 MHz | ~3.573–3.576 MHz |
| 40 m | 7.074 MHz | ~7.074–7.077 MHz |
| 30 m | 10.136 MHz | ~10.136–10.139 MHz |
| 20 m | 14.074 MHz | ~14.074–14.077 MHz |
| 17 m | 18.100 MHz | ~18.100–18.103 MHz |
| 15 m | 21.074 MHz | ~21.074–21.077 MHz |
| 12 m | 24.915 MHz | ~24.915–24.918 MHz |
| 10 m | 28.074 MHz | ~28.074–28.077 MHz |
| 6 m | 50.313 MHz | ~50.313–50.316 MHz |

These are commonly used operating frequencies, not a statement that the entire listed range is an FT8-only allocation.

Band plans and permitted operating frequencies can differ by country and ITU region. Operators must comply with the applicable regulations and band plan.

---

## 4. Three Different Meanings of "Frequency" and "Width"

It is important to distinguish three different concepts.

### 4.1 Published FT8/Dial Frequency

Example:

```text
20 m FT8 = 14.074000 MHz
```

This is normally the **transceiver's USB dial frequency** used by WSJT-X.

It is a frequency reference for the audio passband.

It is not necessarily the RF center frequency of a particular FT8 transmission.

### 4.2 Individual FT8 Signal Bandwidth

A single FT8 signal is approximately:

```text
~50 Hz wide
```

The exact occupied bandwidth depends on the measurement definition and signal characteristics.

### 4.3 WSJT-X Audio Passband

The transceiver normally processes an audio passband of roughly:

```text
0 to 3000 Hz
```

depending on the radio, audio interface, filtering, and configuration.

Many separate FT8 signals can therefore coexist within one approximately 3 kHz USB passband.

For example:

```text
Audio frequency

0 Hz                                             3000 Hz
 |--------------------------------------------------|
       |        |          |       |          |
      FT8      FT8        FT8     FT8        FT8
      ~50      ~50        ~50     ~50        ~50 Hz
```

The individual FT8 signals occupy only a small fraction of the total audio passband.

---

## 5. USB Frequency Translation

The basic frequency relationship for USB is:

\[
f_{RF} = f_{dial} + f_{audio}
\]

where:

- \(f_{RF}\) = actual RF frequency of the transmitted FT8 signal
- \(f_{dial}\) = transceiver's dial/carrier reference frequency
- \(f_{audio}\) = WSJT-X audio frequency

### Example: 20 m

Radio:

```text
Mode: USB
Dial: 14.074000 MHz
```

WSJT-X transmission audio:

```text
Audio frequency: 1500 Hz
```

Then:

\[
f_{RF}
=
14.074000\ \text{MHz}
+
0.001500\ \text{MHz}
\]

\[
\boxed{f_{RF}=14.075500\ \text{MHz}}
\]

Therefore:

- WSJT-X/transceiver dial = **14.074000 MHz**
- Audio frequency = **1500 Hz**
- Approximate RF center frequency = **14.075500 MHz**

---

## 6. Why "1500 Hz Below" Is Used

The phrase:

> "Tune the transceiver 1500 Hz below the FT8 frequency"

can be useful as a conceptual description, but it can also be misleading.

More precisely:

> **The transceiver is tuned to the USB dial/reference frequency, and WSJT-X places the FT8 signal at an audio frequency above that reference.**

If the desired RF signal is at:

```text
14.075500 MHz
```

and the desired audio frequency is:

```text
1500 Hz
```

then the USB dial frequency is:

```text
14.075500 MHz - 0.001500 MHz
= 14.074000 MHz
```

So the dial is 1500 Hz below that particular RF signal.

However, **1500 Hz is not a requirement that every FT8 transmission must use 1500 Hz audio**.

WSJT-X can use different audio frequencies within its passband.

---

## 7. WSJT-X Receive Operation

Suppose a station transmits an FT8 signal at:

```text
14.075500 MHz
```

Your transceiver is tuned to:

```text
14.074000 MHz USB
```

The frequency difference is:

\[
14.075500 - 14.074000
= 0.001500\ \text{MHz}
\]

or:

```text
1500 Hz
```

The receiver therefore presents approximately 1500 Hz of audio to WSJT-X.

WSJT-X displays the station around:

```text
1500 Hz
```

on its waterfall.

The receive relationship is therefore:

```text
RF received:
14.075500 MHz
       |
       v
USB receiver dial:
14.074000 MHz
       |
       | difference
       v
Audio:
1500 Hz
       |
       v
WSJT-X waterfall:
1500 Hz
```

---

## 8. WSJT-X Transmit Operation

The reverse process occurs during transmission.

Example:

```text
WSJT-X audio = 1500 Hz
Radio dial   = 14.074000 MHz
Radio mode   = USB
```

The USB transmitter translates the audio to RF:

```text
14.074000 MHz + 1500 Hz
= 14.075500 MHz
```

Conceptually:

```text
WSJT-X
   |
   | 1500 Hz FT8 audio
   v
Computer audio interface
   |
   v
USB transceiver
   |
   | USB frequency translation
   v
14.075500 MHz RF
   |
   v
Antenna
```

There is no need for WSJT-X itself to create a separate 14.075500 MHz RF carrier.

The transceiver generates the RF.

---

## 9. Why USB Is Normally Used

With USB:

\[
f_{RF} = f_{dial} + f_{audio}
\]

Therefore, increasing the WSJT-X audio frequency moves the transmitted RF frequency upward.

For example:

| Dial | WSJT-X audio | Approximate RF |
|---:|---:|---:|
| 14.074000 MHz | 1000 Hz | 14.075000 MHz |
| 14.074000 MHz | 1500 Hz | 14.075500 MHz |
| 14.074000 MHz | 2000 Hz | 14.076000 MHz |
| 14.074000 MHz | 2500 Hz | 14.076500 MHz |

This makes the frequency relationship easy to understand and is the basis of the normal FT8 USB convention.

For LSB, the frequency relationship would be reversed:

\[
f_{RF} = f_{dial} - f_{audio}
\]

FT8 HF operation is conventionally configured for USB.

---

## 10. Multiple FT8 Signals on One Dial Frequency

A major advantage of the WSJT-X frequency convention is that many stations can use the same dial frequency simultaneously.

For example:

```text
Radio dial = 14.074000 MHz

WSJT-X audio frequencies:

  800 Hz  -> RF ≈ 14.074800 MHz
 1200 Hz  -> RF ≈ 14.075200 MHz
 1500 Hz  -> RF ≈ 14.075500 MHz
 2000 Hz  -> RF ≈ 14.076000 MHz
 2400 Hz  -> RF ≈ 14.076400 MHz
```

Each individual FT8 signal remains only about 50 Hz wide, so the signals can be separated in frequency.

Conceptually:

```text
RF frequency

14.074000                                      14.077000 MHz
     |-----------------------------------------------|
          |     |       |       |        |
         FT8   FT8     FT8     FT8      FT8
        ~50Hz ~50Hz   ~50Hz   ~50Hz    ~50Hz
```

The exact spacing between stations is not fixed. WSJT-X selects or displays audio frequencies according to the received signals and operating circumstances.

---

## 11. Detailed 20 m Example

Assume:

```text
Band:       20 m
Mode:       FT8
Radio mode: USB
Dial:       14.074000 MHz
```

A received station appears at:

```text
1500 Hz
```

The corresponding RF frequency is approximately:

\[
14.074000 + 0.001500
=
14.075500\ \text{MHz}
\]

Therefore:

| Quantity | Value |
|---|---:|
| Band | 20 m |
| WSJT-X dial frequency | 14.074000 MHz |
| Radio mode | USB |
| WSJT-X audio frequency | 1500 Hz |
| Approx. RF frequency | 14.075500 MHz |
| Approx. individual FT8 bandwidth | 50 Hz |

If the signal were instead at 2000 Hz audio:

\[
14.074000 + 0.002000
=
14.076000\ \text{MHz}
\]

The radio dial remains at 14.074000 MHz. Only the audio frequency changes.

---

## 12. "At the Antenna" Frequency

When discussing the RF frequency at the antenna, use the actual translated RF frequency rather than the published FT8 dial frequency.

For example:

```text
WSJT-X dial:
14.074000 MHz

Audio:
1500 Hz

RF at antenna:
≈14.075500 MHz
```

The transmitted FT8 spectrum is centered approximately around that RF frequency, with an individual-signal bandwidth of roughly 50 Hz.

Thus, if the signal is centered at 14.075500 MHz, an approximate representation of its spectrum is:

```text
14.075475 MHz       14.075500 MHz       14.075525 MHz
       |----------------|----------------|
                  ~50 Hz total
                       ^
                 signal center
```

The exact spectral shape and measured occupied bandwidth depend on the signal, filtering, transmitter, and measurement definition.

---

## 13. Important Distinction: FT8 Signal vs. FT8 Operating Window

The term "FT8 bandwidth" is sometimes used ambiguously.

### Individual signal

Approximately:

```text
50 Hz
```

### Typical WSJT-X audio window

Approximately:

```text
0–3000 Hz
```

### Resulting RF window with a USB dial frequency

Approximately:

```text
dial frequency
        to
dial frequency + 3000 Hz
```

For example, with:

```text
Dial = 14.074000 MHz
```

a nominal 0–3000 Hz USB audio window corresponds approximately to:

```text
14.074000–14.077000 MHz
```

This does **not** mean that FT8 itself occupies the whole 3 kHz.

It means that the transceiver/WSJT-X audio passband provides a roughly 3 kHz region in which individual narrow FT8 signals can be placed.

---

## 14. Band Plan vs. Signal Bandwidth

Another important distinction is between:

1. Amateur-radio band allocation
2. Amateur-radio band plan
3. Common FT8 dial frequency
4. WSJT-X audio passband
5. Individual FT8 signal bandwidth

These are different things.

For example:

```text
Amateur allocation
       |
       +---- Band plan / recommended mode segments
                    |
                    +---- Common FT8 dial frequency
                                |
                                +---- ~3 kHz USB passband
                                         |
                                         +---- individual FT8 signals
                                               ~50 Hz each
```

The common FT8 frequency is not itself a legal allocation boundary.

Operators must follow the regulations applicable in their jurisdiction and the relevant amateur-radio band plan.

---

## 15. Practical WSJT-X Setup

A typical HF FT8 setup is conceptually:

```text
Transceiver:
    Mode = USB
    Dial frequency = published FT8 dial frequency

WSJT-X:
    Mode = FT8
    Radio = connected via CAT
    Audio input = receiver audio
    Audio output = transmitter audio
```

For 20 m, for example:

```text
Transceiver:
    USB
    14.074000 MHz

WSJT-X:
    FT8
    RX/TX audio frequency selected within the audio passband
```

WSJT-X normally controls the transceiver's dial frequency through CAT when CAT is configured.

The audio interface supplies the FT8 waveform to the transceiver.

The transceiver then converts the audio waveform into RF.

---

## 16. Do Not Interpret 1500 Hz as a Mandatory FT8 Center Frequency

A common misconception is:

> "FT8 is transmitted at 1500 Hz."

That is not correct.

1500 Hz is a convenient audio-frequency example/reference.

A station could transmit at, for example:

```text
1000 Hz
1500 Hz
2000 Hz
2500 Hz
```

and, with a fixed 14.074000 MHz USB dial frequency, the corresponding RF frequencies would be approximately:

```text
14.075000 MHz
14.075500 MHz
14.076000 MHz
14.076500 MHz
```

The radio dial does not have to change for each of those signals.

---

## 17. Frequency Formula Summary

### USB

\[
\boxed{f_{RF}=f_{dial}+f_{audio}}
\]

### Reverse calculation

If the desired RF frequency is known:

\[
\boxed{f_{dial}=f_{RF}-f_{audio}}
\]

### Example

Desired RF:

```text
14.075500 MHz
```

Audio:

```text
1500 Hz
```

Therefore:

```text
14.075500 MHz - 0.001500 MHz
= 14.074000 MHz
```

So:

```text
Dial = 14.074000 MHz USB
Audio = 1500 Hz
RF = 14.075500 MHz
```

---

## 18. Quick Reference Table

| Concept | Typical value/example |
|---|---|
| FT8 modulation | 8-FSK |
| Tone spacing | 6.25 Hz |
| Number of tones | 8 |
| Symbol duration | 160 ms |
| Symbols | 79 |
| Transmission duration | ~12.64 s |
| Sequence period | 15 s |
| Individual FT8 bandwidth | ~50 Hz |
| Typical audio passband | ~0–3000 Hz |
| Common HF mode | USB |
| USB frequency equation | RF = dial + audio |
| Example dial | 14.074000 MHz |
| Example audio | 1500 Hz |
| Example RF | 14.075500 MHz |

---

## 19. Conceptual Diagram

```text
                       TRANSMIT

       WSJT-X
          |
          | FT8 waveform
          | e.g. 1500 Hz audio
          v
   Computer audio interface
          |
          v
    USB transceiver
          |
          | Dial = 14.074000 MHz
          |
          | USB translates audio upward
          |
          v
     RF ≈ 14.075500 MHz
          |
          v
       Antenna
```

And on receive:

```text
                       RECEIVE

       Antenna
          |
          | RF ≈ 14.075500 MHz
          v
    USB transceiver
          |
          | Dial = 14.074000 MHz
          |
          | frequency difference
          v
       1500 Hz audio
          |
          v
       WSJT-X
          |
          v
   FT8 decoded station
```

---

## 20. Bottom Line

For normal USB FT8 operation, think of the published FT8 frequency as the **radio's dial/reference frequency**.

For a particular FT8 signal:

\[
\boxed{\text{RF frequency}
=
\text{WSJT-X dial frequency}
+
\text{WSJT-X audio frequency}}
\]

For example:

```text
20 m FT8

Dial/reference:  14.074000 MHz
USB audio:             1500 Hz
                         |
                         v
RF at antenna:     14.075500 MHz
```

The **individual FT8 signal is only about 50 Hz wide**, while the **WSJT-X USB audio passband is roughly 3 kHz**, allowing many individual FT8 signals to coexist around the same dial frequency.

Therefore, saying:

> "Tune 1500 Hz below the FT8 frequency"

is best understood as a frequency-translation example. It is not a requirement that every FT8 transmission be centered at 1500 Hz audio.

---

## References

- WSJT-X documentation: https://wsjt.sourceforge.io/wsjtx.html
- WSJT-X user documentation: https://wsjt.sourceforge.io/wsjtx-doc/
- ARRL FT8 material: https://www.arrl.org/ft8
- ARRL band plans: https://www.arrl.org/band-plan
