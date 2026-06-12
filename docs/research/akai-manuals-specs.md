# Akai S-Series Samplers — TIMESTRETCH and Audio Path, from the Original Manuals

Research date: 2026-06-12.
Method: original operator's manuals were downloaded as PDFs and read directly (text extracted with `pdftotext`; the image-only S1100 scan was read page-by-page). Verbatim quotes carry the manual's own page numbers. Anything not from a manual is explicitly marked **(secondary)**.

Local copies of all manuals used: `docs/research/_manuals/`

| Manual | Edition | Source URL | Local file |
|---|---|---|---|
| S900 Operator's Manual | original (OS 1.x era) | https://www.synthxl.com/wp-content/uploads/2018/01/Akai-s900-Operator-manual.pdf | `akai-s900-manual.pdf` |
| S950 Operator's Manual | original (1988) | https://manuals.fdiskc.com/flat/Akai%20S-950%20Owners%20Manual.pdf (also archive.org `synthmanual-akai-S950ownersmanual`) | `akai-s950-manual.pdf` |
| S1000 Series Operator's Manual | **Software Version 2.0, dated 89/11** (covers S1000/S1000HD/S1000PB) | https://www.firstpr.com.au/rwi/smem/akai-manuals/S1000-V2.0-Manual.pdf | `akai-s1000-v2-manual.pdf` |
| S1100 Operator's Manual | **Software Version 1.0, dated 90/08** | https://www.synthxl.com/wp-content/uploads/2018/04/Akai-s-1100-Operators-manual.pdf | `akai-s1100-manual.pdf` |
| S2800/S3000/S3200 Operator's Manual | Version 1.0, 10/92 | https://archive.org/details/akai-s-2800-sampler-operators-manual | `akai-s2800-manual.txt` (full text) |
| S2000 Operator's Manual | Version 1.30 | https://www.platinumaudiolab.com/free_stuff/manuals/Akai/akai_s2000_manual.PDF | `akai-s2000-manual.pdf` |

Caveat: the S950 PDF in circulation is a re-typed/OCR transcription of the original, so it contains character-level typos (e.g. `5950` for S950, `Cup to 999%)` for `(up to 999%)`, `Monl/Po12` for `Mon1/Pol2`). Quotes below preserve the file's text; obvious OCR garbling is annotated.

---

## 1. Akai S900 (1986) — no timestretch

### Timestretch
The operator's manual contains **zero occurrences of the word "stretch"** (verified by full-text search of the whole manual). The S900 has no timestretch function in its stock OS. Sample edit facilities listed in the spec page are: "Scanning (ONE SHOT, LOOPING, ALTERNATING) / A.D.S.R … / Velocity cross fade / Velocity switch / Positional cross fade / Attack pitch offset / LFO / Filter / Sample merge" (manual p.25).

**(secondary)** Did later S900 OS versions add it? No. Forum reports of S900 OS 4.0 (mpc-forums thread "Akai S900 timestretch?", https://www.mpc-forums.com/viewtopic.php?f=42&t=183074): "Going to page 14 on the S900 in OS4 brings you to TIME SKEW, unlike TIME STRETCH on the S950" and "os4.0 does not give the s900 time-stretch". TIME SKEW is not a pitch-preserving stretch. Treat as forum-grade evidence; the stock manual is unambiguous that timestretch is absent.

### Audio path (manual "Specifications", p.25 — verbatim)
```
System      Digital sampling
            Sampling frequency: 7.5 kHz — 40 kHz (MIN -MAX)
            Sampling time: 63.3 sec. — 11.75 sec. (MAX -MIN)
            Voice: 8 Voice
            Range: 6 Octave
Storage     Built-in FLOPPY DISK DRIVE ... 3.5 inch (2DD)
            Internal memory: 750K byte
Multi sampling  32
External jack   MIDI (IN, OUT, THRU), REC trigger x1, Mic input/P.B trigger x1,
                Line input/P.B trigger x1, Line output x8, Stereo output x2 (L, R),
                Mix output x1, Voice output x1 (13 PIN/DIN)
```
- Bit depth is **not stated on the spec page**; **(secondary)** the S900 is universally documented as 12-bit (Wikipedia, Akai brochures).
- Playback architecture **(secondary, service-manual lore)**: per-voice variable-clock playback through analog anti-imaging filters — the same architecture as the S950; the operator's manual does not describe the reconstruction filters.

---

## 2. Akai S950 (1988) — timestretch from launch, "up to 999%"

### Timestretch — UI and parameters
Timestretch shipped in the launch OS; the manual's introduction sells it as a headline feature:

> "the first sampler in its price range to feature TIMESTRETCH which enables you [to lengthen or shorten a sample by as much as 999% without any change in pitch]" (Introduction)

> "Timestretch is a facility that allows you to either lengthen or shorten a sample without changing that samples pitch over a factor of 999% (i.e a one second sample can be stretched to a maximum length of nearly 10 seconds)." (manual p.29)

It lives on **EDIT SAMPLE Page 14**. The display, verbatim from manual p.30 (OCR cleaned in brackets):

```
>14 STRETCH #New name NEW SAMPLE #200%
 D-time 1000 #Auto D_ [Mon1/Pol2] 2 #Do_
```

Parameters as documented (manual pp.29–30):
- **New name** — "it is necessary to create a new sample … enter a new name in the usual way and hit the ENT button." Destination must fit in free memory: "if you do not have enough memory, the display will tell you."
- **Stretch percentage** — "select the percentage by which you want to stretch or compress a sample." Display default shown: `200%`. Maximum factor 999% (per intro and spec sheet). No numeric minimum is stated in the manual; compression ("shorten") is explicitly supported.
- **D-TIME** — "The next field allows you to set what is known as a D-TIME. … With extreme timestretch values, the end result can sometimes sound slightly metallic. The D-TIME function smooths things out allowing you to suppress this effect. Longer D-TIME values will give the sample a slight tremolo effect whilst shorter D-TIME values give rise to the metallic effect." Display default shown: `1000`. **Units and value range are not documented in the manual.**
- **AUTO-D** — "there is an AUTO-D function which, like autoloop, gets the S950 to select a suitable D-TIME value."
- **Mon1/Pol2** — "allows you to select whether the sample you are stretching or compressing is a monophonic or polyphonic sample. The 'Mon1' setting is more suitable for lengthening a single tone whilst the 'Pol2' is better suited to lengthening polyphonic samples such as backing vocals, drum loops, etc.."
- **DO** — "move the cursor to the 'DO' field and press ENT and the S950 will pause for a short while whilst it calculates the new sample (the EDIT SAMPLE light will remain lit)." No abort procedure and no remaining-time display are documented.
- Loop carry-over caveat: "Note 2: The looping for the stretched/compressed sample will remain the same as the original samples. As a result, if the original sample was looped, you will have to edit the new timestretched sample and change its loop length to something more suitable."
- Mono/stereo: the S950 is a **mono sampler** (Mic/Line input; "Mono mix, left and right, 8 individual assignable outputs"), so timestretch is inherently mono. The Mon1/Pol2 switch refers to musical content (monophonic vs polyphonic material), not channel count.

Spec-sheet confirmation (p.74): the Sample editing facilities list includes "`Timestretch Cup to 999%)`" — i.e. "Timestretch (up to 999%)".

### Audio path (manual "S950 - SPECIFICATIONS", p.74 — verbatim, OCR noted)
```
System:           Digital sampling (12-bit sampling/16-bit processing)
Sampling rate:    7.5kHz - 48.0kHz (continuously variable)
Bandwidth:        3kHz - 19.2kHz (continuously variable)
Sampling time:    9.89 sets - 03.3 sees        [OCR; original: 9.89 secs - 63.3 secs]
Polyphony:        8 Voice
Data storage:     3.5 inch disk drive (DD and HD disks)
Internal memory:  750k Byte
Max number of samples: 99      Max number of programs: 99
Outputs:          Mono mix, left and right, 8 individual assignable outputs
Inputs:           Mic, Line
Options:          Memory expansion board (EXM006) X 2
                  Atari/Supra-CD/RDAT hard disk and digital audio interface
```
- The "9.89 secs – 63.3 secs" reading is confirmed by arithmetic: 474,720 samples ÷ 48,000 Hz = 9.89 s and ÷ 7,500 Hz = 63.3 s. (Also matches the S900's 63.3 s figure at 7.5 kHz.)
- 12-bit conversion, **16-bit internal processing** — stated directly in the spec line.
- Reconstruction filters are not described in the operator's manual. **(secondary)** S900/S950 use per-voice variable-clock playback through analog low-pass filtering, which is the commonly cited source of their "rounded" character.
- Memory: 750 KB standard; up to two EXM006 boards (the timestretch chapter notes "With the EXM006 memory expansion boards fitted it could be used in audio/visual applications to change the overall time of a voice-over…").

### OS history
Timestretch is documented in the original 1988 operator's manual — **present from launch**. The S950 OS is in ROM; no incremental OS feature history is documented in the manual.

---

## 3. Akai S1000 (1988) — TIME-STRETCH page, 25%–2000%, CYCLIC/INTELL

### Timestretch — UI and parameters (V2.0 manual, pp.45–47, verbatim)
Access: "Pressing the TIME button from the ED.2 page enters the TIME-STRETCH page." (p.45)

> "This enables you to lengthen or shorten a sample or a selected part of a sample from 25% of its original length to 2000% (twenty times). Since this operation can take a lot of memory, it's as well to delete unwanted samples from memory…" (p.45)

> "Two modes are available for stretching: CYCLIC, in which a fixed interpolation rate is maintained throughout the whole of the sample (suitable for individual instrument samples), and INTELL, in which the S1000 'intelligently' varies the interpolation rate according to the sample content (suitable for speech and music)." (p.45)

Parameters:
- **sample** (top line) — source sample selection.
- **"stretch zone" and "to"** — "select the part of the sample that you want stretched". You can stretch a sub-range, not just the whole sample.
- **ZONE** soft key — "you can listen to this part of the sample by pressing the ZONE button. This will replay this part of the sample stretched at the set cycle length (but only if you are using CYCLIC mode)." (p.46)
- **NAME** — "Since you cannot stretch a sample to itself, you must find another name for the new sample." (p.46)
- **stretch mode** — CYCLIC or INTELL: "Be warned, though - although the intelligent mode will produce better results, the time taken for this operation is much longer than when the CYCLIC mode (up to several minutes)." (p.46)
- **Cycle length** — "If you decide to use the CYCLIC mode, you can set the cycle length **(in samples)** in the 'Cycle length' field. The soft key autC can be used to help you find the right sample length. As with autolooping, the S1000 applies software logic to the sample to calculate what it believes is the right answer … it is not always infallible." (p.46)
- **time factor** — "the time factor by which the original sample is to be stretched **(from 25% to 2000%)**. As this is altered, the length and time of the new stretched sample (and the percentage of memory it will occupy) are displayed. Make sure that these figures do not exceed the amount of memory available." (p.47)
- **quality / width** — "The 'quality' (the time that the S1000 spends determining cycle lengths) and the 'width' of stretch crossfading in the final stretched sample may also be altered from 01 to 99. **This only applies to the INTELL mode.**" (p.47) (The S2800-family manual later clarifies semantics: quality = number of analysis decisions; width = crossfade between original and inserted data, with the advice "when low QUALITY values are set, this [crossfade] should be set high and vice versa".)
- **GO / abort** — "When you are ready, press GO. You can abort the operation by holding down F8, for a few seconds, otherwise the process may take a few minutes (depending on the length of the sample being stretched). The display will display a message showing that the time-stretch is in progress, and will also show you how much processing time remains before finishing." (p.47)
- Audition: PLAY soft key plays the stretched result; ENT/PLAY plays the original.
- Mono/stereo: the TIME page operates on **one sample at a time**. Stereo material on the S1000 is stored as two mono samples with "-L"/"-R" suffixes (manual pp. on stereo recording: the S1000 "adds '-L' and '-R' to the two sample names"); the manual does not document any linked stereo stretch, so each side must be processed separately (CYCLIC with identical settings being the deterministic option). *(The per-side processing is an inference from the documented UI; the manual is silent on stereo coherence.)*

### Audio path (V2.0 manual, "Technical Specifications", p.81 — verbatim)
```
Sampling rates       44.1kHz, 22.05kHz (20Hz-20kHz, 20Hz-10kHz audio bandwidth)
Data format          16-bit linear encoding
Memory               2Mbyte standard, expandable to 8Mbyte
Sampling time        23.76 seconds - mono @ 44.1kHz
(unexpanded memory)  47.52 seconds - mono @ 22.05kHz
                     11.88 seconds - stereo @ 44.1kHz
                     23.76 seconds - stereo @ 22.05kHz
Maximum number of samples   200
Maximum number of programs  100
Pitch shifting       ±2 octaves (1 cent steps) interpolation and decimation 24-bit
                     algorithm, using custom VLSI circuit
Filter               Digital moving low-pass filter (-18dB/octave)
Envelope generators  2 x digital ADSR
STEREO OUT           2 x 1/4-inch phone (unbalanced)   -3dBv, 600Ω
ASSIGNABLE OUTS      8 x 1/4-inch phone (unbalanced)   -3dBv, 600Ω
REC GAIN             HI –58dBm, MID -38dBm, LO -18dBm
OPTIONS              EXM005 2Mbyte memory expansion board (up to 8Mbytes may be fitted)
                     IB-102 Atari hard disk interface / IB-103 SCSI / IB-104 AES/EBU
```
(The spec table's "32 octaves" in the OCR is "±2 octaves" — confirmed against the identical line in the S1100 spec table.) 16 voices polyphony. ADC/DAC component types are not stated in the operator's manual; **(secondary)** Wikipedia and period reviews describe 16-bit conversion with **24-bit internal processing**. Reconstruction filtering is digital interpolation in the custom VLSI (fixed-rate playback) rather than the S900/S950's variable-clock analog approach — the spec's "interpolation and decimation 24-bit algorithm, using custom VLSI circuit" is the manual's only statement on this. Memory ceiling later rose to 32 MB with EXM008 boards (1990) **(secondary: Wikipedia/GB Audio; the V2.0 manual still says 8 MB)**.

### OS history
- The manual in hand is "Software Version 2.0, 89/11" and fully documents TIME-STRETCH.
- **Timestretch was not in the launch OS.** Sources conflict on exactly when it arrived:
  - GB Audio S1000 data sheet (hire-company spec sheet): "Time Stretch function was available on **version 1.3** and higher" (http://www.gbaudio.co.uk/data/s1000.htm).
  - Wikipedia (Akai S1000): "**Version 2.0** of the S1000's operating system introduced primitive timestretching" (https://en.wikipedia.org/wiki/Akai_S1000).
  - Forum consensus (dogsonacid etc.): "OS 1.0 doesn't do timestretch — you need 2.0 onward."
  - Safe statement: absent at launch (OS 1.0), present by OS 2.0 (Nov 1989 manual); GB Audio claims availability from 1.3.
- OS is held on EPROM with disk-loadable upgrades; final version **4.40** (sold today as EPROM upgrades, e.g. studio.supplies, circuitbenders) **(secondary)**.

---

## 4. Akai S1100 (1990) — same timestretch engine, upgraded converters and DSP

### Timestretch — UI and parameters (V1.0 manual, pp.53–55)
The TIME section (pp.53–55) is **word-for-word identical to the S1000 V2.0 manual** apart from the model name:
- "Pressing the TIME button from the ED.2 page enters the TIME-STRETCH page. This enables you to lengthen or shorten a sample or a selected part of a sample **from 25% of its original length to 2000% (twenty times)**." (p.53)
- Modes: "CYCLIC, in which a fixed interpolation rate is maintained throughout the whole of the sample (suitable for individual instrument samples), and INTELL, in which the S1100 'intelligently' varies the interpolation rate according to the sample content (suitable for speech and music)." (p.53)
- "stretch zone" / "to" sub-range selection; ZONE audition (CYCLIC only); NAME for the mandatory new sample. (p.54)
- "If you decide to use the CYCLIC mode, you can set the cycle length (in samples) in the 'Cycle length' field. The soft key autC can be used to help you find the right sample length." (p.54)
- "the time factor by which the original sample is to be stretched (from 25% to 2000%) … the length and time of the new stretched sample (and the percentage of memory it will occupy) are displayed." (p.55)
- "The 'quality' (the time that the S1100 spends determining cycle lengths) and the 'width' of stretch crossfading in the final stretched sample may also be altered from 01 to 99. This only applies to the INTELL mode." (p.55)
- "press GO. You can abort the operation by holding down F8 … The display will display a message showing that the time-stretch is in progress, and will also show you how much processing time remains before finishing." (p.55)
- The manual's p.54 diagrams explicitly contrast "Stretched sample (using CYCLIC mode). The distances between the original sample points are equal" with "Stretched sample (using INTELL mode). The distances between the original sample points are different (as determined by the S1100)".
- Adjacent re-sampling page (p.55) carries a chart: "Lowest resampling frequency allowed by S1100 (8kHz) … Highest resampling frequency allowed by S1100 (65.54kHz)".

Features-page framing (p.7): "The S1100 is capable of editing samples digitally — 'stretching' or 'squeezing' samples without changing the pitch in order to fit them into a specific time slot."

### Audio path (V1.0 manual, "Technical Specifications", p.96 — verbatim)
```
Sampling rates       44.1kHz, 22.05kHz (20Hz-20kHz, 20Hz-10kHz audio bandwidth)
Data format          16-bit linear encoding
Memory               2Mbyte standard, expandable to 32Mbyte
Sampling time        23.76 s mono @44.1kHz / 47.52 s mono @22.05kHz /
(unexpanded)         11.88 s stereo @44.1kHz / 23.76 s stereo @22.05kHz
Max samples / programs   200 / 100
Pitch shifting       ±2 octaves (1 cent steps) interpolation and decimation 24-bit
                     algorithm, using custom VLSI circuit
Filter               Digital moving low-pass filter (-18dB/octave)
REC IN               2 x XLR (balanced), 2 x 1/4-inch phone (balanced)
STEREO OUT           2 x 1/4-inch phone (unbalanced)   -5dBm, 600Ω
AES/EBU OUT          1 x XLR (AES/EBU digital audio output)   RS-422 level
ASSIGNABLE OUTS      8 x 1/4-inch phone (unbalanced)   -5dBm, 600Ω
SMPTE IN/OUT         2 x 1/4" phone (balanced)
REC GAIN             HI -58dBm, MID -38dBm, LO -18dBm
OPTIONS              EXM005 2Mbyte / EXM008 8Mbyte memory boards, IB-104 AES/EBU
```
Features page (p.7): "With a sampling rate of 44.1kHz, and 16-bit resolution … 16-note polyphony … Digital effects are also integrated into the S1100 … (reverb, echo and time modulation) … an XLR connector is provided for digital transmission of stereo audio data in the AES/EBU format." Optional IB-104 gives "direct digital sampling at rates of up to 48kHz" (p.8). SCSI (IB-103) is integral; hard disks to 510 MB.

**(secondary)** Converter upgrade vs S1000: Music Technology, May 1991 S1100 review (https://www.muzines.co.uk/articles/akai-s1100/2162) — the S1100 has **20-bit DACs** for "improved s/n ratio and dynamic range"; gear-forum sources additionally describe sigma-delta ADCs. The operator's manual itself does not state converter bit depths beyond "16-bit linear encoding."

### OS history
The V1.0 manual (90/08) already contains the full timestretch chapter — **present from launch**, inherited from S1000 OS 2.x. S1100 OS continued in step with the S1000 line (final ~4.30/4.40 family) **(secondary)**. The S1100EX is a 16-voice expander, unchanged engine **(secondary: SoS Sep 1992)**.

---

## 5. Later families — brief

### S01 (1993) — no timestretch
**(secondary: soundprogramming.net/samplers/akai/akai-s01, vintagesynth)** 16-bit, max 32 kHz sample rate, 8 voices, 1 MB (→2 MB), mono in/out. Editing limited to truncate/transpose/loop/reverse — **no timestretch**.

### S2800 / S3000 / S3200 (1992) — same UX, new 28-bit engine, documented defaults
Operator's Manual Version 1.0, 10/92 (one manual covers all three). Timestretch chapter (pp.55–60) keeps the S1000 model — "from 25% of its original length to 2000% (twenty times)", CYCLIC vs INTELL — but adds an explicit explanation: "Timestretch works by instructing the digital signal processor to analyse the signal and insert or delete blocks of sample data at appropriate places and crossfades are used to make the insertions and deletions as seamless as possible." It also candidly warns: "especially with stretch factors exceeding 10% or so, you may get an echo or 'flam' effect on some transients."

The printed example screen documents defaults:
```
sample: ...            stretch zone: 0  to: 128
Cycle length: 1000     total: 220512  7%
time factor: 100%      norm. time= 5.00sec
stretch mode: CYCLIC   qual: 10   width: 10
new sample: STRING C4  **existing samp**
softkeys: TIME (autC) (ZONE) [GO] (PLAY)
```
Parameter glossary (verbatim fragments): "time factor: … The range is 25% to 2000% (although we are the first to admit that such extremes are only going find favour with the truly mad!)"; "qual: … sets the number of decisions it will make as it works its way through the sample … only has a function when INTELL is selected"; "width: This sets a crossfade between the original and the inserted data. It is recommended that when low qual: values are set, this should be set high and vice versa."

Audio path (Introduction, p.1, verbatim): "Polyphony: 32 voices / **A-D conversion: 16-bit stereo with 64-times oversampling** / **Internal processing: 28-bit accumulation** / **D-A conversion: 18-bit with 8-times oversampling (L/R outs), 18-bit with 8-times oversampling (ind outs)** / Sampling rates: 44.1kHz/22.050kHz / Phase locked stereo sampling and playback / Internal memory: 2 Megabytes - expandable to 16 Megabytes (254 programs/255 samples)". Spec appendix: "Filter: Digital moving low-pass filter (**-12 dB/octave with resonant**)" — note the change from the S1000/S1100's non-resonant -18 dB/oct. The S3200 adds AES/EBU + SMPTE; **CD3000** is the S3000 engine with CD-ROM sample loading **(secondary)**.

### S2000 (1995, manual V1.30) / S3000XL family
Same timestretch parameters re-skinned for the smaller UI: "The STRETCH parameter sets the percentage by which the sample will be stretched or shrunk. The range is 25% to 2000%"; TYPE = CYCLIC/INTELL(igent); CYC LENGTH in samples with AUTO (F1); QUALITY ("the number of decisions the S2000's processor will make") and XFD crossfade for INTELL; EXEC/halt soft keys; A/B audition of original vs processed. (S2000 manual pp.152–154.)

---

## 6. Comparison table — timestretch

| | **S900** (1986) | **S950** (1988) | **S1000** (1988) | **S1100** (1990) | S2800/S3000/S3200 (1992) |
|---|---|---|---|---|---|
| Timestretch present | **No** (no mention in manual; OS4 "TIME SKEW" ≠ timestretch, *secondary*) | **Yes, from launch** | **Not at launch**; via OS update (v1.3 per GB Audio / v2.0 per Wikipedia) | Yes, from launch (OS 1.0) | Yes, from launch |
| Page / access | — | EDIT SAMPLE **Page 14 "STRETCH"** | **TIME** soft key from ED.2 → "TIME-STRETCH page" | TIME from ED.2 (identical) | ED.2 → TIME |
| Stretch range | — | up to **999%** ("over a factor of 999%"; display default 200%) | **25% – 2000%** | **25% – 2000%** | **25% – 2000%** (default 100%) |
| Modes | — | **Mon1 / Pol2** (mono vs poly source material) | **CYCLIC / INTELL** | CYCLIC / INTELL | CYCLIC / INTELL |
| Cycle parameter | — | **D-TIME** (smoothing; display default 1000; units/range undocumented) + **AUTO-D** | **Cycle length, in samples** (CYCLIC only) + **autC** auto-detect | same | **Cycle length: 1000** default, in samples + autC/AUTO |
| Crossfade / quality | — | none documented (D-TIME is the only smoothing control) | **quality** and **width** each **01–99**, INTELL only | quality/width 01–99, INTELL only | **qual / width**, defaults **10/10**, INTELL only |
| Partial-sample stretch | — | no (whole sample) | yes — "stretch zone"/"to" + ZONE audition (CYCLIC) | yes (identical) | yes ("stretch zone: 0 to: 128") |
| Execution / abort | — | cursor to **DO**, ENT; no abort documented | **GO**; abort = hold **F8**; remaining-time countdown shown | GO / hold F8 / countdown | GO (LGO); halt soft key |
| Processing time | — | "the S950 will pause for a short while" | CYCLIC fast; INTELL "up to several minutes" | same | INTELL "up to several minutes" |
| Destination | new named sample (memory permitting) — all models require a new sample; loops carry over unchanged (S950 note) | | | | |
| Mono/stereo | — | mono machine | per-sample; stereo = separate -L/-R samples | same | phase-locked stereo sampling, but stretch still per named sample |

## 7. Comparison table — hardware character

| | **S900** | **S950** | **S1000** | **S1100** | S2800/S3000/S3200 |
|---|---|---|---|---|---|
| Conversion | 12-bit *(secondary; not on spec page)* | **12-bit sampling / 16-bit processing** (spec p.74) | **16-bit linear encoding**; "24-bit algorithm, custom VLSI" for interpolation/decimation | 16-bit linear; same 24-bit VLSI; **20-bit DACs** *(secondary: Music Technology 5/91)* | **16-bit ADC 64× oversampling; 28-bit accumulation; 18-bit DAC 8× oversampling** |
| Sample rates | **7.5–40 kHz continuously variable** | **7.5–48 kHz continuously variable** (bandwidth 3–19.2 kHz) | **44.1 / 22.05 kHz** fixed | 44.1 / 22.05 kHz (digital in to 48 kHz via IB-104) | 44.1 / 22.05 kHz |
| Playback architecture | variable-clock + analog filters *(secondary)* | variable-clock + analog filters *(secondary)* | fixed-rate digital interpolation, custom VLSI | same + DSP FX | same family, 28-bit accumulation |
| Voice filter | analog | analog (per spec: filter per keygroup) | **digital moving LPF, -18 dB/oct, non-resonant** | digital moving LPF, -18 dB/oct | **digital moving LPF, -12 dB/oct with resonance** |
| Polyphony | 8 | 8 | 16 | 16 | 32 |
| Memory | 750 KB | 750 KB (+2× EXM006) | 2 MB → 8 MB (later 32 MB w/ EXM008, *secondary*) | 2 MB → **32 MB** | 2 MB → 16 MB |
| Max sample time (std mem) | 11.75 s @40 kHz – 63.3 s @7.5 kHz | 9.89 s @48 kHz – 63.3 s @7.5 kHz | 23.76 s mono @44.1 | 23.76 s mono @44.1 | 22.28 s mono @44.1 |
| Output level/impedance | — | — | **-3 dBv, 600 Ω** (all analog outs) | **-5 dBm, 600 Ω**; AES/EBU XLR (RS-422 level) | line outs; S3200 adds AES/EBU |
| Input gain ranges | — | Mic/Line | REC GAIN HI -58 / MID -38 / LO -18 dBm | same | balanced jack ins |
| Samples/programs | 32 multisamples | 99 / 99 | 200 / 100 | 200 / 100 | 255 / 254 |

## 8. Surprising / notable findings

1. **S950 tops out at 48 kHz, not 40 kHz.** The spec page reads "Sampling rate: 7.5kHz - 48.0kHz (continuously variable)" — an upgrade over the S900's 7.5–40 kHz that is often misquoted.
2. **The S950's timestretch is a completely different beast from the S1000's**: percent-up-to-999% with a D-TIME smoothing constant and a Mon1/Pol2 material switch, versus the S1000/S1100's 25–2000% with cycle-length/CYCLIC vs INTELL and quality/width 01–99. The D-TIME parameter's units and range are simply not documented anywhere in the S950 manual (display default `1000`).
3. **S1000 timestretch was an OS retrofit, and the sources disagree on the version**: GB Audio's data sheet says v1.3+, Wikipedia says v2.0. The earliest manual that documents it is the V2.0 manual dated 89/11. Either way it was not in the launch OS — whereas the cheaper S950 had it from day one, months earlier.
4. **The S1100 manual's timestretch chapter is a verbatim copy of the S1000 V2.0 text** (only the model number changed) — the S1100's improvements were converters (20-bit DACs per Music Technology), onboard DSP effects, AES/EBU, SMPTE cue lists and 32 MB addressing, not the stretch algorithm.
5. **The S900 never got timestretch** — zero mentions in the manual, and even final OS 4.0 only offers "TIME SKEW" (forum-grade evidence).
6. The S2800/S3000/S3200 manual is unusually frank about artifacts: flam/echo on transients "with stretch factors exceeding 10% or so", and calls users of the 25%/2000% extremes "the truly mad". It is also the first manual to document concrete defaults (cycle 1000, qual 10, width 10) and to describe the insert/delete-blocks-with-crossfades algorithm explicitly.
7. Output level specs differ between S1000 (-3 dBv) and S1100 (-5 dBm) into the same 600 Ω — a small but real gain-staging difference between the two.
8. S1100 resampling is documented as 8 kHz–65.54 kHz (manual p.55 chart) — i.e. the engine can resample *above* 44.1 kHz even though sampling rates are fixed at 22.05/44.1.
