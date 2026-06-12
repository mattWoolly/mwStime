# Deep Research Report: Akai S-Series Timestretch

*Produced by a fan-out research workflow (101 agents): 5 search angles, source fetching, 3-vote adversarial claim verification, synthesis. 2026-06-12.*

## Question

Comprehensive technical and cultural research on the Akai S-series samplers' timestretch capabilities, to inform a faithful software emulation (VST plugin). Three tracks: (1) TECHNICAL: exactly how the timestretch algorithm worked on the S950, S1000, and S1100 (and whether/how the S900 fits in) — algorithm family (cyclic/granular slice-splice), the "cycle time" (CT) parameter and its units/ranges, intelligent vs cyclic modes, crossfading behavior, why the characteristic metallic/fluttery artifacts occur, OS versions that introduced or changed timestretch, plus per-model hardware signal path details that color the sound: bit depth (12-bit S900/S950 vs 16-bit S1000/S1100), DAC architecture, sample rates (fixed vs variable), filters, headroom. (2) PRIOR ART: existing emulations and reverse-engineering efforts — the open-source Akaizer project, Inphonik RX950, TAL-Sampler Akai modes, Bitwig/Live "stretch" modes claiming Akai character, forum reverse-engineering threads (Gearspace, KVR, Vintage Synth Explorer), patents by Akai/IVL Technologies if any. (3) CULTURAL: how jungle, breakbeat, and IDM artists of the 90s/00s used S-series timestretch — concrete documented examples (e.g. Goldie's Timeless intro, Remarc, Dillinja, Photek, Source Direct, Aphex Twin, Squarepusher, Luke Vibert), typical workflows (stretching Amen/Think breaks, vocal stretches), which models were studio staples and why, and which other Akai models (S2000, S3000, S3200, S5000/S6000, MPC60) had notable timestretch variants worth emulating. Output should be a cited, fact-checked report with enough algorithmic detail to implement the DSP.

## Summary

The Akai S-series timestretch is a time-domain cyclic (slice/splice, granular overlap-add) algorithm with two documented modes — CYCLIC (fixed, user-set cycle length) and INTELLIGENT (content analysis varies the interpolation/cycle rate) — introduced to the S1000 via a free OS update (v1.3 per the gbaudio datasheet, confirmed pre-v2.0 by 1989 Sound On Sound coverage) and carried into the S950, S1100, S2000 and S3000 families. The characteristic metallic/fluttery artifacts come from repeating fixed-length crossfaded slices, and a 4-parameter granular model (grain length, crossfade amount, grain trigger rate, playhead speed) is reported sufficient to approximate it; per-model coloration matters greatly — the 12-bit S900/S950 use per-voice variable-clock DACs with no interpolation and a clock-tracking 6th-order Butterworth switched-capacitor filter (variable 7.5–48 kHz rate), while the S1000/S1100 are 16-bit 44.1 kHz stereo machines that interpolate to fixed-rate DACs. Prior art includes Akaizer (CLASSIC/REVISED algorithms, 25–2000% stretch, ±36 semitones, adjustable cycle length; Windows-only today), TAL-Sampler (vendor-documented CYCLIC "Akai style" and INTELL correlation modes with a DENSITY grain-size knob, plus S1000 DAC emulation), and Inphonik RX950 (S950 AD/DA path coloration only — no timestretch, no pitch-tracking aliasing). Culturally, the effect is foundational to jungle: Dead Dred's "Dred Bass" (1994) timestretched vocals, Amen-break stretching on the S1100 (documented settings: Cyclic mode, Cycle Length 1000, Time Factor 300%), Goldie's Timeless produced by Akai devotee Rob Playford (S950→S1000→S1100→S3200), with the scene's gear shifting from S950+Atari ST (1992–95) toward E-mu E6400s in the techstep era.

## Verified findings

### Finding 1 (confidence: medium, verification vote: 3-0 (mode existence), 3-0 (hypothesis attribution), 3-0 (TAL characterization))

Algorithm family and modes: Akai S-series timestretch (S950/S1000/S1100/S3000 family) offers two modes — a basic CYCLIC mode with a fixed, user-settable cycle length/interpolation rate, and an INTELLIGENT (INTELL) mode where interpolation rate is varied by sample-content analysis. The family is widely characterized as a time-domain cyclic slice-splice (SOLA/granular overlap-add) algorithm; the metallic/fluttery artifacts arise from repeating crossfaded fixed-length slices. The exact firmware internals (especially INTELL's analysis) have never been confirmed by ROM-level reverse engineering — the SOLA characterization is a well-corroborated hypothesis.

**Evidence:** S1000 manual documents exactly two modes: CYCLIC (fixed interpolation rate) and INTELL (rate varied by sample content). music-dsp thread (Stephen Blinkhorn) hypothesizes 'simple SOLA algorithms with the intelligent mode doing some analysis to change the window length' — explicitly a question, not a confirmed result. TAL manual independently mirrors the taxonomy: 'CYCLIC: Akai style cyclic stretcher. The DENSITY knob controls the grain size. INTELL: Time stretch with correlation algorithm.'

**Sources:**

- https://music-dsp.music.columbia.narkive.com/4XeZqepe/hardware-sampler-timestretch
- https://tal-software.com/downloads/docs/TAL-Sampler-UserManual.pdf
- https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1342678-difference-timestretch-akai-samplers-mpcs.html
- https://the-akaizer-project.blogspot.com/

### Finding 2 (confidence: medium, verification vote: 2-1 (4-param recipe), 2-0 (Akaizer ranges))

DSP implementation recipe: a granular slice-repeat engine with four tunable parameters — grain (cycle) length, crossfade amount, grain trigger rate (how often a new grain start is picked from the playhead), and playhead speed through the source — is sufficient to approximate the Akai cyclic timestretch sound. Practical parameter ranges from Akaizer (which models the same algorithm): time stretch 25%–2000%, pitch shift ±36 semitones, fully adjustable cycle length. Note: a specific '40 ms window / 20 ms crossfade' ballpark claim was REFUTED in verification, so window sizes should be tuned by ear/measurement, not assumed.

**Evidence:** JUCE forum developer (asimilon, 2024) lists exactly those four parameters and reports results 'quite like the old Akai method'; parameters map nearly one-to-one onto Akaizer's Cycle Length / Time Factor / crossfade controls. Akaizer page states 'Time Stretch (25% to 2000%)', '+/-36 semitones' (extended from ±24 in v2.5), and 'Time Factor, Cycle Length and Transpose parameters are all fully adjustable'. Caveats: 'quite like' is one developer's subjective ear; the 4-param model omits the INTELL mode and hardware coloration.

**Sources:**

- https://forum.juce.com/t/resources-and-techniques-for-tsm-cycle-based-akai-style-time-stretch/59766
- https://the-akaizer-project.blogspot.com/

### Finding 3 (confidence: medium, verification vote: 3-0)

OS history: timestretch was NOT in the S1000's original 1988 OS — it arrived via a free operating-system update, available from OS v1.3 onward. Contemporary 1989 Sound On Sound coverage confirms timestretch shipped as a free update (disk or chip) before v2.0, refuting Wikipedia's uncited statement that v2.0 introduced it. The S950 imported timestretch from the S1000 line (its TIME STRETCH page is in the owner's manual); the S900 lacked true timestretch (only TIME SKEW in OS4). The S1100 (1990) retained and refined it (parameters include Stretch Mode, Cycle Length, Time Factor, Quality, Width).

**Evidence:** gbaudio S1000 datasheet: 'Time Stretch function was available on version 1.3 and higher.' SOS July 1989: Time Stretch 'is available as a free update to any S1000 owner who requests it, and the software comes on disk or on a chip.' The exact '1.3' number rests solely on the gbaudio datasheet (hence medium confidence); the pre-v2.0 free-update fact is primary-press confirmed. S900 TIME SKEW vs true timestretch distinction comes from verifier corroboration, not a standalone verified claim.

**Sources:**

- http://www.gbaudio.co.uk/data/s1000.htm
- https://www.muzines.co.uk/articles/making-the-most-of-your-akai-s1000-sampler/5604
- https://manuals.fdiskc.com/flat/Akai%20S-950%20Owners%20Manual.pdf

### Finding 4 (confidence: high, verification vote: 3-0, 3-0, 3-0 (three claims merged))

S900/S950 hardware signal path (12-bit generation): 12-bit resolution with continuously variable sample rate (S950: 7.5–48 kHz, rate set at 2.5x the chosen bandwidth of 3–19.2 kHz). Each of the 8 voices has its own DAC and independent playback clock (service-manual confirmed: per-voice BA9221 12-bit DAC + MF6CN-50 clock-tunable switched-capacitor filter, uPD8253C timers) — playback does NOT interpolate; pitch transposition is done by changing the DAC clock, with the 6th-order Butterworth switched-capacitor reconstruction filter cutoff tracking the clock. This is why high frequencies are retained better than on fixed-rate interpolating samplers, and an authentic S950 emulation must model clock-tracked filtering per voice, not a static bitcrush.

**Evidence:** Verified at service-manual level: S950 Voice PCB has eight identical per-voice DAC+filter circuits with independent clocks. Corroborated by Paul Kellett (ex-Akai R&D, on KVR): 'There's a separate D/A for each voice, each with a variable sample rate up to 50 kHz on the S950... much better than the S1000 which interpolates then outputs everything through D/A's fixed at 44100Hz.' ALM Busy Circuits (who recreated the filter in hardware as MUM M8) confirms the 6th-order Butterworth switched-capacitor, clock-controlled design with characteristic clock-bleed whine at low cutoffs. Note: a separate claim attributing the S900 sound to stacked preamp/filter saturation layers was REFUTED — do not model that.

**Sources:**

- https://www.inphonik.com/products/rx950-classic-ad-da-converter/
- https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1341382-why-does-my-akai-s900-sound-so-good.html
- https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/871357-akai-s950-bandwidth-vs-samplerate.html
- https://busycircuits.com/pages/alm018

### Finding 5 (confidence: high, verification vote: 3-0)

S1000/S1100 hardware signal path (16-bit generation): the S1000 samples 16-bit at 44.1 kHz in stereo (with a 22.05 kHz option), distinguishing it from the 12-bit mono S900/S950; per ex-Akai R&D corroboration it interpolates playback through DACs fixed at 44.1 kHz — the architectural opposite of the S950's variable-clock approach. The S1100 is an enhanced S1000 retaining timestretch.

**Evidence:** gbaudio: 'one of the most popular 16-bit 44.1kHz stereo samplers of it's time'; Wikipedia: 'a 16-bit, 44.1 kHz professional stereo digital sampler... quickly displaced the S900 as the studio standard'. Fixed-rate-interpolation detail from Paul Kellett quote in the S900 verification. Music Technology May 1991 reviews the S1100's timestretch.

**Sources:**

- http://www.gbaudio.co.uk/data/s1000.htm
- https://en.wikipedia.org/wiki/Akai_S1000
- https://vintagesynth.com/akai/s1000
- https://www.muzines.co.uk/articles/akai-s1100/2162

### Finding 6 (confidence: high, verification vote: 3-0, 3-0, 3-0)

Prior art — Akaizer: a software recreation of the Akai 'cyclic' timestretch, naming the S950/S1000/S2000/S3000 series (notably not the S1100 or S900). It ships two algorithms: CLASSIC ('simulates the Akai cyclic time stretch as faithfully as possible') and REVISED (deliberately improved/deviating). It is Windows-only today (v2.5, March 2026, paid £10); the Mac port was abandoned at 32-bit (v2.3, dead since macOS Catalina; usable on modern Macs only via Wine). IMPORTANT: claims that Akaizer is open-source or freeware, and that it replicates the algorithm 'near-exactly', were REFUTED — treat it as closed-source paid prior art whose fidelity is the developer's own characterization.

**Evidence:** Primary site (fetched 2026-06-12): 'time stretch (and/or pitch shift) any WAVE or AIFF sound file in the style of the cyclic time stretch which featured on old Akai sound samplers, like the S950 / S1000 / S2000 / S3000 series... classic metallic-sounding effect'; 'Two algorithms are available: CLASSIC... REVISED improves on the classic algorithm.' Grep for 'S1100' and 'S900' on the page: 0 matches. Site instructs 'Linux and macOS: Use Wine'; last native Mac build was 32-bit v2.3, dead on Catalina+.

**Sources:**

- https://the-akaizer-project.blogspot.com/
- https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/176692-akai-s950-cyclic-mode-timestretch-mac-software-emulation.html

### Finding 7 (confidence: high, verification vote: 3-0, 3-0)

Prior art — Inphonik RX950: emulates only the S950's 12-bit AD/DA conversion signal path (12-bit quantization, audio-bandwidth knob tied to target sample rate, original 6th-order Butterworth low-pass). It is a static signal-path colorator effect: it does NOT reproduce pitch-shift/transposition-tracking aliasing of the real hardware, has no sample-playback engine, and does not emulate timestretch. Useful as a reference for the S950 conversion coloration layer of a plugin, not for the stretch algorithm.

**Evidence:** Inphonik page: 'faithfully mimic the whole AD/DA conversion process of the Akai S950', 'Legendary 12-bit resolution', 'Original steep 6th-order low-pass Butterworth filter', bandwidth knob controls 'the target sample rate'. KVR thread: as an effect it lacks note-dependent pitch-up/down aliasing ('A bit crusher doesn't follow the notes'). It does still produce signal-dependent downsampling aliasing.

**Sources:**

- https://www.inphonik.com/products/rx950-classic-ad-da-converter/
- https://www.kvraudio.com/forum/viewtopic.php?t=502077&start=15

### Finding 8 (confidence: high, verification vote: 3-0, 3-0)

Prior art — TAL-Sampler: implements two vintage timestretch modes per its official manual — CYCLIC ('Akai style cyclic stretcher', DENSITY knob = grain size) and INTELL (correlation algorithm, density = correlation windows) — mirroring Akai's own mode names, plus genuine vintage DAC emulation (changelog: 'S1000 DAC added' v1.6.7, cycle stretch mode v1.9.6; vendor states samples are truly down-sampled then DAC-processed, covering S950 variable-rate and S1000 44.1/22.05 kHz behavior). Forum consensus says it 'sounds a lot like that akai sound', but it is a perceptual approximation, not a reverse-engineered port.

**Evidence:** TAL manual (Stretch Section, p.11), verbatim: 'CYCLIC: Akai style cyclic stretcher. The DENSITY knob controls the grain size. INTELL: Time stretch with correlation algorithm. Density changes the correlation windows.' Vendor: 'We really down-sample the sample to the desired sampling frequency, then process the data depending on the chosen DAC and up-sample it to the desired pitch.'

**Sources:**

- https://tal-software.com/downloads/docs/TAL-Sampler-UserManual.pdf
- https://tal-software.com/products/tal-sampler
- https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1342678-difference-timestretch-akai-samplers-mpcs.html
- https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/176692-akai-s950-cyclic-mode-timestretch-mac-software-emulation.html

### Finding 9 (confidence: high, verification vote: 3-0)

Cultural — timestretched jungle vocals: the Akai timestretch's metallic timbre was used on vocals in Dead Dred's 'Dred Bass' (Moving Shadow, Sept 1994), generally acknowledged as the first jungle record to feature the timestretched-vocal effect, made by pushing the Akai S-series algorithm beyond its limits. (The Reverb source article miscredits the artist as 'Dred Dred' and implies an S950 specifically; per-machine attribution is generic 'Akai S series'.)

**Evidence:** Reverb: S950 'featured a time-stretch function... used to manipulate breaks and create the kind of odd, metallic-sounding timbre to vocals on tracks like Dred Dred's 1994 classic Dred Bass.' Discogs confirms Dead Dred — 'Dred Bass', Moving Shadow SHADOW 50, 12 Sept 1994; NITELIFE Audio calls it 'usually acknowledged as the first Jungle record to feature the time-stretched vocal effect'.

**Sources:**

- https://reverb.com/news/the-samplers-behind-90s-jungle-and-drum-and-bass
- https://www.discogs.com (Dead Dred – Dred Bass, SHADOW 50)

### Finding 10 (confidence: high, verification vote: 3-0, 3-0)

Cultural — Amen-break stretching on the S1100: timestretching the Amen break on an Akai S1100 is a documented, demonstrable jungle technique (Pete Cannon performs the full workflow on real hardware on video), and the S1100's stretch created the characteristic 'futuristic' jungle/DnB sounds. Concrete classic-jungle settings are documented (SPOD): Stretch Mode Cyclic, Cycle Length 1000, Time Factor 300%, Quality 20, Width 10 — directly usable as preset/validation targets for an emulation.

**Evidence:** Video title: 'Jungle production technique: Time stretching the Amen Break with an Akai S1100 sampler #jungle' by Pete Cannon (documented jungle producer), corroborated by Elektronauts/LoveThatBass coverage. SPOD page gives the concrete parameter set, proving the feature is user-controllable with those exact parameter names. Note: a 2023 retrospective demonstration, not a 1990s primary document.

**Sources:**

- https://www.youtube.com/watch?v=17UgyuBVaH8
- SPOD 'AKAI S1100 - Timestretch Settings'
- https://en.wikipedia.org/wiki/Amen_break

### Finding 11 (confidence: high, verification vote: 3-0, 3-0)

Cultural — Goldie's Timeless and the jungle gear timeline: Timeless was produced/engineered by Rob Playford on Akai samplers ('I went from the S950 to the S1000, then up to the S1100, and finally to a S3200' — SOS June 1998; the S1100 used on 'Timeless': 'It was all done in a sampler'), with the Akai timestretch audible across the album per listener consensus. More broadly, early jungle (1992–95 ragga/jungle/hardstep) was made mostly on Akai samplers — especially S950 + Atari ST running Cubase (Doc Scott's 1993 studio, 4hero) — while mid-to-late-90s techstep/neurofunk producers (Ed Rush & Optical with 3x E6400 Ultras, Dillinja by 1998) shifted heavily to E-mu E6400s with Z-Plane filters, though Dillinja famously kept S950s for breaks.

**Evidence:** SOS 1998 interview confirms the Akai lineage quote verbatim. Caveats: the SOS piece does not itself discuss timestretch (that part is listener attribution); Timeless is strictly a Goldie/Playford co-production; the famous stretch on Goldie/Rufige Kru's 'Terminator' (1992) is debated — some accounts attribute it to an Eventide H3000 pitch-shift rather than Akai timestretch — so per-track attributions need care.

**Sources:**

- https://www.soundonsound.com/people/rob-playford-producing-goldie
- https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1138822-90s-jungle-what-gear.html
- https://www.factmag.com (Doc Scott 1993 studio)

## Refuted claims (do NOT rely on these)

- {"claim": "The vintage Akai samplers S950, S1000, S2000, and S3000 all featured a timestretch mode known as 'cyclic' time stretch, and the open-source tool Akaizer replicates this algorithm near-exactly.", "vote": "0-3", "source": "https://forum.cockos.com/showthread.php?t=89148"}
- {"claim": "The Akai cyclic timestretch character can be roughly approximated with a generic time-domain slice/crossfade ('simple windowed') algorithm using approximately a 40 ms window and 20 ms crossfade, suggesting the Akai algorithm operates with segment sizes in that ballpark.", "vote": "0-3", "source": "https://forum.cockos.com/showthread.php?t=89148"}
- {"claim": "As of February 2008, two PC-only software tools \u2014 TimeMachine and Akaizer \u2014 existed that emulated the Akai samplers' cyclic-mode timestretch, with no Mac equivalent available at that time.", "vote": "1-2", "source": "https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/176692-akai-s950-cyclic-mode-timestretch-mac-software-emulation.html"}
- {"claim": "The S900's characteristic sound is attributed to multiple stacked layers of subtle distortion: 12-bit resolution, a sample rate of 40kHz or below, sample interpolation imperfections, preamp saturation, and filter saturation, which together add harmonic content.", "vote": "0-3", "source": "https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1341382-why-does-my-akai-s900-sound-so-good.html"}
- {"claim": "Akaizer is a freeware application for Windows/macOS/Linux that emulates the 'cyclic' time stretch mode of Akai S950/S1000/S2000/S3000 samplers, and this cyclic stretch is the source of the characteristic 'metallic' sound heard in 1990s hardcore/jungle/speed garage tracks.", "vote": "0-3", "source": "https://www.modwiggler.com/forum/viewtopic.php?t=239824"}

## Caveats

Key uncertainties: (1) No claim survived that documents the actual Akai firmware algorithm at ROM/code level — the SOLA/cyclic-granular characterization is a strongly corroborated inference from manuals, vendor emulations (TAL, Akaizer), and developer hypotheses, not a confirmed disassembly; INTELLIGENT mode internals are entirely unverified. (2) Original Akai manual parameter units/ranges for cycle time (CT) were not directly captured — only Akaizer's analogous ranges and the SPOD S1100 settings (Cycle Length 1000, units unspecified) survived; the refuted '40 ms window / 20 ms crossfade' claim shows ballpark window sizes should not be trusted. (3) The exact S1000 OS version '1.3' rests on a single secondary datasheet (gbaudio), though the pre-v2.0 free-update fact is primary-press confirmed; Wikipedia's v2.0 attribution is wrong. (4) Several refuted claims warn against repeating common lore: Akaizer is NOT open-source/freeware (paid, closed, Windows-only), its fidelity is self-characterized; the 'stacked saturation layers' explanation of the S900 sound was refuted. (5) The music-dsp narkive quote was reconstructed from search-index snippets (page unreachable). (6) Crossfade shape (linear vs equal-power), S1100 Quality/Width semantics, S900 TIME SKEW details, and any Akai/IVL patents were not covered by surviving claims. (7) Cultural per-track machine attributions (Dred Bass on S950 specifically; Terminator possibly Eventide H3000 not Akai) are loose and should be hedged in the final report. Source mix: hardware-path findings reach service-manual/ex-Akai-engineer quality (high); algorithm findings rest on vendor docs and forum corroboration (medium); cultural findings mix primary press (SOS 1998) with retrospective forum/video sources.

## Open questions

- What are the exact CT/cycle-time parameter units, ranges, and defaults in the original Akai S1000/S1100/S950 manuals (and what do the S1100's extra Quality and Width parameters control algorithmically)?
- How does INTELLIGENT mode's content analysis actually work (pitch detection? autocorrelation for cycle-boundary selection?), and what crossfade shape (linear vs equal-power) and overlap policy does the firmware use — answerable only via ROM disassembly or systematic black-box measurement of real hardware output?
- Did Akai (or IVL Technologies) hold patents covering the timestretch algorithm, and how do the later S2000/S3000/S3200 and S5000/S6000 or MPC timestretch implementations differ from the S1000-family algorithm?
- Where exactly on the S900 does TIME SKEW (OS4) sit relative to true timestretch, and is it worth emulating as a distinct mode?

## Refuted claims (do NOT rely on these)

- {"claim": "The vintage Akai samplers S950, S1000, S2000, and S3000 all featured a timestretch mode known as 'cyclic' time stretch, and the open-source tool Akaizer replicates this algorithm near-exactly.", "vote": "0-3", "source": "https://forum.cockos.com/showthread.php?t=89148"}
- {"claim": "The Akai cyclic timestretch character can be roughly approximated with a generic time-domain slice/crossfade ('simple windowed') algorithm using approximately a 40 ms window and 20 ms crossfade, suggesting the Akai algorithm operates with segment sizes in that ballpark.", "vote": "0-3", "source": "https://forum.cockos.com/showthread.php?t=89148"}
- {"claim": "As of February 2008, two PC-only software tools \u2014 TimeMachine and Akaizer \u2014 existed that emulated the Akai samplers' cyclic-mode timestretch, with no Mac equivalent available at that time.", "vote": "1-2", "source": "https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/176692-akai-s950-cyclic-mode-timestretch-mac-software-emulation.html"}
- {"claim": "The S900's characteristic sound is attributed to multiple stacked layers of subtle distortion: 12-bit resolution, a sample rate of 40kHz or below, sample interpolation imperfections, preamp saturation, and filter saturation, which together add harmonic content.", "vote": "0-3", "source": "https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1341382-why-does-my-akai-s900-sound-so-good.html"}
- {"claim": "Akaizer is a freeware application for Windows/macOS/Linux that emulates the 'cyclic' time stretch mode of Akai S950/S1000/S2000/S3000 samplers, and this cyclic stretch is the source of the characteristic 'metallic' sound heard in 1990s hardcore/jungle/speed garage tracks.", "vote": "0-3", "source": "https://www.modwiggler.com/forum/viewtopic.php?t=239824"}

## All sources consulted

- {"url": "https://music-dsp.music.columbia.narkive.com/4XeZqepe/hardware-sampler-timestretch", "quality": "forum", "angle": "Algorithm internals / manual-level detail", "claimCount": 5}
- {"url": "https://forum.juce.com/t/resources-and-techniques-for-tsm-cycle-based-akai-style-time-stretch/59766", "quality": "forum", "angle": "Algorithm internals / manual-level detail", "claimCount": 5}
- {"url": "https://the-akaizer-project.blogspot.com/", "quality": "primary", "angle": "Algorithm internals / manual-level detail", "claimCount": 5}
- {"url": "https://forum.cockos.com/showthread.php?t=89148", "quality": "forum", "angle": "Algorithm internals / manual-level detail", "claimCount": 5}
- {"url": "http://www.gbaudio.co.uk/data/s1000.htm", "quality": "secondary", "angle": "Algorithm internals / manual-level detail", "claimCount": 5}
- {"url": "https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/176692-akai-s950-cyclic-mode-timestretch-mac-software-emulation.html", "quality": "forum", "angle": "Algorithm internals / manual-level detail", "claimCount": 5}
- {"url": "https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1341382-why-does-my-akai-s900-sound-so-good.html", "quality": "forum", "angle": "Hardware signal path coloration", "claimCount": 4}
- {"url": "https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/871357-akai-s950-bandwidth-vs-samplerate.html", "quality": "forum", "angle": "Hardware signal path coloration", "claimCount": 5}
- {"url": "https://www.inphonik.com/products/rx950-classic-ad-da-converter/", "quality": "primary", "angle": "Hardware signal path coloration", "claimCount": 5}
- {"url": "https://www.kvraudio.com/forum/viewtopic.php?t=624699", "quality": "forum", "angle": "Hardware signal path coloration", "claimCount": 5}
- {"url": "https://forum.juce.com/t/a-lightweight-akai-style-time-stretch-algorithm-realtime/60514", "quality": "forum", "angle": "Prior art and reverse engineering", "claimCount": 5}
- {"url": "https://www.kvraudio.com/forum/viewtopic.php?t=502077&start=15", "quality": "forum", "angle": "Prior art and reverse engineering", "claimCount": 5}
- {"url": "https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/5175769", "quality": "primary", "angle": "Patents and algorithm-family literature", "claimCount": 5}
- {"url": "https://reverb.com/news/the-samplers-behind-90s-jungle-and-drum-and-bass", "quality": "secondary", "angle": "Cultural usage and workflows", "claimCount": 5}
- {"url": "https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1138822-90s-jungle-what-gear.html", "quality": "forum", "angle": "Cultural usage and workflows", "claimCount": 5}
- {"url": "https://www.dogsonacid.com/threads/s1000-jungle-can-this-be-done-an-s900.819432/page-2", "quality": "unreliable", "angle": "Cultural usage and workflows", "claimCount": 0}
- {"url": "https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1342678-difference-timestretch-akai-samplers-mpcs.html", "quality": "forum", "angle": "Cultural usage and workflows", "claimCount": 5}
- {"url": "https://www.youtube.com/watch?v=17UgyuBVaH8", "quality": "secondary", "angle": "Cultural usage and workflows", "claimCount": 4}
- {"url": "https://www.modwiggler.com/forum/viewtopic.php?t=239824", "quality": "forum", "angle": "Cultural usage and workflows", "claimCount": 5}
