# CaloDigiDQM

`CaloDigiDQM` is an `art::EDAnalyzer` for Mu2e calorimeter Data Quality Monitoring. It reads `CaloDigi` objects, maps offline SiPM IDs to electronics identifiers through `CaloDAQMap`, fills ROOT histograms for detector health and waveform diagnostics, and can optionally stream selected histograms to the otsdaq visualizer through `ots::HistoSender`.

The module is designed for both offline ROOT-file inspection and online DQM use. Global histograms and disk-map objects are booked during construction through `art::TFileService`. At the start of the first event, once `CaloDAQMap` is available, the module prebooks the expected regular board/channel structure and mapped laser-channel structure.

---

## Build and runtime context

This module is intended to run inside the Mu2e `art`/Offline environment with ROOT output through `art::TFileService`.

Main framework and detector dependencies:

| Dependency                                  | Used for                                               |
| ------------------------------------------- | ------------------------------------------------------ |
| `art::EDAnalyzer`                           | Module lifecycle: constructor, `analyze()`, `endJob()` |
| `art::TFileService` / `art::TFileDirectory` | ROOT histogram booking and directory structure         |
| `CaloDigiCollection`                        | Input calorimeter digi data                            |
| `CaloDAQMap` through `ProditionsHandle`     | Mapping offline SiPM IDs to electronics IDs            |
| `THMu2eCaloDisk`                            | Calorimeter disk maps                                  |
| ROOT `TH1`, `TH2`, `TProfile`, `TProfile2D` | Histogram storage and display                          |
| `ots::HistoSender`                          | Optional online histogram streaming to the Visualizer App |

The module does not implement a raw data decoder. It consumes already-produced `CaloDigi` objects and focuses on DQM feature extraction, histogram organization, and optional online streaming.

---

## Main responsibilities

* Read `CaloDigiCollection` from a configurable input module label.
* Convert each offline `SiPMID` to electronics coordinates using `CaloDAQMap`.
* Fill global detector summaries for occupancy, baseline, RMS, amplitude, waveform size, pair completeness, waveform health, issue counts, and run counters.
* Fill per-disk `THMu2eCaloDisk` maps for amplitude, crystal sum, left/right asymmetry, baseline, and RMS.
* Prebook the expected board/channel ROOT structure from `CaloDAQMap` at the start of the first event, while still tracking which channels actually appear in the input data.
* Treat board `160` as the dedicated laser board and monitor it separately from regular disk boards.
* Optionally stream selected summary, board, disk-map, live waveform, first-hit waveform, and laser histograms to otsdaq.
* Optionally overlay a small set of reference histograms when a reference ROOT file is provided. Reference-file overlay support is experimental and intentionally limited. It may be simplified or replaced later if `.rcfg` dashboards with the `superimposed` option become the preferred way to display overlays.

---

## Processing overview

The module follows this event-processing flow:

```text
CaloDigiCollection
      v
Map offline SiPM IDs with CaloDAQMap
      v
Classify as regular calorimeter digi or laser-board digi
      v
Extract waveform features
      v
Fill global, board, channel, disk-map, laser, and diagnostic histograms
      v
Build per-event SiPM-pair quantities
      v
Update DQM summary panels and optional streaming queues
      v
Stream selected histograms if the configured cadence is reached
```

Important implementation choices:

* Regular calorimeter data and laser-board data are handled by separate code paths.
* Pair-based quantities are calculated after all digis in the event have been processed.
* Live waveform streaming uses one representative waveform per SiPM/channel per event.
* Disk maps are running means accumulated over the job.
* Selected streamed disk-map modes are refreshed before disk-map streaming, and all disk-map modes are refreshed at endJob().
* ROOT-file output remains available even if online streaming is disabled or fails.

---

## Detector and encoding conventions

This module uses the following internal geometry assumptions:

| Quantity            |  Value | Meaning                              |
| ------------------- | -----: | ------------------------------------ |
| `kNDisks`           |    `2` | Calorimeter disks: Disk 0 and Disk 1 |
| `kBoardsPerDisk`    |   `80` | Regular electronics boards per disk  |
| `kChannelsPerBoard` |   `20` | Channels per board                   |
| `kChannelsPerDisk`  | `1600` | `80 * 20` regular channels per disk  |
| `kTotalBoards`      |  `160` | Regular boards across both disks     |
| `kTotalChannels`    | `3200` | Regular channels across both disks   |
| `kLaserBoardID`     |  `160` | Dedicated laser board                |
| `kLaserChannels`    |   `20` | Laser board channels                 |
| `kUnmappedRawId`    | `9999` | Sentinel value for unmapped channels |

Board IDs are global across disks:

* Disk 0 uses board IDs `0-79`.
* Disk 1 uses board IDs `80-159`.
* Board `160` is the laser board.

Regular channel indexing:

```text
local channel within disk = (boardID - disk*80) * 20 + chanID
compact global channel    = disk*1600 + local channel
```

Two global encoded channel axes are used:

```text
sparse encoding = boardID*100 + chanID
dense encoding  = boardID*20  + chanID
```

Sparse encoding is easier to read visually because channels from different boards are separated by gaps. Dense encoding is compact and useful for overlays or full-detector profiles.

---

## Address mapping

Each `CaloDigi` is mapped to electronics coordinates through `CaloDAQMap`:

```text
sipmId  = digi.SiPMID()
rawId   = calodaqconds.rawId(mu2e::CaloSiPMId(sipmId)).id()
boardID = rawId / 20
chanID  = rawId % 20
```

The mapped board ID determines the processing path:

| Condition              | Meaning                          | Processing path             |
| ---------------------- | -------------------------------- | --------------------------- |
| `boardID == 160`       | Laser board digi                 | `processLaserDigi()`        |
| `boardID in 0-79`      | Disk 0 regular digi              | `processRegularDigi()`      |
| `boardID in 80-159`    | Disk 1 regular digi              | `processRegularDigi()`      |
| Invalid or unmapped ID | Bad mapping or out-of-range data | Digi is skipped and counted |

Skipped digis are not silently ignored. They are counted in `h_skip_reason` and summarized in `endJob()`.

---

## Prebooked ROOT structure

At the start of the first event, the module retrieves `CaloDAQMap` and calls `prebookExpectedStructureFromCaloDAQMap()`. This creates the expected ROOT folder and histogram structure for mapped regular calorimeter channels and mapped laser channels.

For regular mapped channels, the module prebooks:

```text
Board folder
Board summary histograms
Baseline distribution
RMS distribution
Max ADC distribution
Asymmetry distribution
Live waveform histogram
```

For laser channels, the module prebooks:
```text
Laser board folder
Laser board summary histograms
Laser baseline distribution
Laser RMS distribution
Laser max ADC distribution
Laser live waveform histogram
```

First-hit waveform histograms are not prebooked. They are created only when the channel is actually observed for the first time.

For one tested `CaloDAQMap` configuration, the prebooked structure was:

```text
regularBoards = 136 / 160
regularChannels = 2712 / 3200
laserBoards = 1 / 1
laserChannels = 4 / 20
totalBoardsIncludingLaser = 137
totalChannelsIncludingLaser = 2716
```

These numbers are map-dependent and should be treated as an example, not a hard-coded guarantee.

The module keeps separate concepts of prebooked channels and active channels:

| Concept | Meaning |
| ------- | ------- |
| Prebooked channel | Channel found in `CaloDAQMap`; its ROOT folder/histograms may exist even before data is seen |
| Active channel | Channel that actually appeared in the input data |
| Updated channel | Active channel whose live waveform changed and may be streamed |

An empty board or channel histogram can be normal: it may represent a mapped channel that was expected from `CaloDAQMap` but did not appear in the processed data.

## Pairing convention

Pair-based quantities assume that adjacent even/odd SiPM IDs belong to the same crystal:

```text
even SiPM ID = one side of the crystal
odd SiPM ID  = partner side
crystal ID   = sipmId / 2
```

For each event, the module stores one representative digi per SiPM. If multiple usable digis appear for the same SiPM in the same event, the representative digi is chosen by highest baseline-subtracted amplitude. This keeps crystal-pair quantities stable while still tracking multiplicity through `h_pair_multiplicity`.

Pair-based histograms include left/right amplitude correlation, crystal sum, left/right asymmetry, pair completeness, pair missing fraction, raw-ID delta, and board delta.

Pair quantities are only filled after the full event has been scanned, because both sides of a crystal may appear at different positions in the `CaloDigiCollection`.

---

## Feature extraction

For each mapped digi, the module extracts waveform-level features:

| Feature      | Definition                                                                                      |
| ------------ | ----------------------------------------------------------------------------------------------- |
| `wfSize`     | Number of waveform samples                                                                      |
| `baseline`   | Mean of the first `kBaselineSamples = 5` samples                                                |
| `rms`        | RMS of the first baseline samples                                                               |
| `ampRaw`     | ADC value at `peakpos`                                                                          |
| `amp`        | `ampRaw - baseline`                                                                             |
| `timeResid`  | `peakpos - kExpectedPeakTick`, where `kExpectedPeakTick = 30`                                   |
| `nSat`       | Number of samples with ADC `>= 4090`                                                            |
| `shapeScore` | Roughness score based on second differences, normalized by amplitude squared                    |
| `shapeClass` | Classified as `Good`, `Saturated`, `EdgePeak`, `LongTail`, `Undershoot`, `Noisy`, or `Negative` |

The feature extraction rejects digis with invalid peak position or non-finite baseline/RMS values. Rejected digis are recorded in `h_skip_reason`.

### Feature equations

```text
baseline  = mean(first 5 waveform samples)
rms       = RMS(first 5 waveform samples)
ampRaw    = waveform[peakpos]
amp       = ampRaw - baseline
timeResid = peakpos - 30
```

The pulse-shape score is a normalized waveform roughness estimate based on second differences:

```text
shapeScore = roughness / max(1, amp^2), for amp > 0
```

where roughness is the sum of squared second differences of the baseline-subtracted waveform.

For `amp <= 0`, the implementation sets `shapeScore = 0.0`; negative pulses are handled separately through the `Negative` shape class and `NegAmp` issue.

---

## Quality and issue definitions

The module uses two related concepts: quality metrics and issue types.

### Quality metrics

Quality metrics are stored as scores where:

```text
1.0 = good / passing
0.0 = bad / failing
```

Intermediate values can appear in profile histograms because bins are averaged over many entries.

| Metric           | Meaning                                               |
| ---------------- | ----------------------------------------------------- |
| `Seen`           | Channel produced at least one mapped regular digi after address decoding  |
| `AmpPositive`    | Baseline-subtracted amplitude is positive             |
| `RMSOK`          | Baseline RMS is not above the module threshold        |
| `SaturationOK`   | Waveform has no saturated samples                     |
| `SNRGood`        | `amp / RMS >= 5`, when RMS is positive                |
| `PeakTimeOK`     | Peak is not on the waveform edge                      |
| `PairOK`         | Partner SiPM is present in the same event             |
| `WaveformHealth` | `1 - badness`, where badness combines waveform issues |

### Issue types

Issue types are counted when a problem is detected:

| Issue      | Meaning                                   |
| ---------- | ----------------------------------------- |
| `Sat`      | At least one waveform sample is saturated |
| `EdgePeak` | Peak is too close to the waveform edge    |
| `HighRMS`  | Baseline RMS is above threshold           |
| `LowSNR`   | Signal-to-noise ratio is below threshold  |
| `NegAmp`   | Baseline-subtracted amplitude is negative |
| `BadShape` | Pulse-shape roughness score is high       |
| `PairMiss` | Partner SiPM is missing                   |

### Health score

Waveform badness is a bounded heuristic score, not a probability. Multiple issues can add to the score, and the final value is clamped to `1.0`.

```text
WaveformHealth = 1.0 - badness
```

Current badness contributions:

| Issue              | Badness contribution |
| ------------------ | -------------------: |
| Saturation         |               `0.30` |
| Edge peak          |               `0.15` |
| High RMS           |               `0.25` |
| Low SNR            |               `0.20` |
| Negative amplitude |               `0.20` |
| Bad pulse shape    |               `0.20` |

A missing pair is treated as a mild health penalty through `kPairMissHealth = 0.75`.

Example:

```text
saturated + high RMS = 0.30 + 0.25 = 0.55 badness
WaveformHealth       = 1.0 - 0.55 = 0.45
```

---

## Heuristic constants and thresholds

Several values in this module are intentionally heuristic. They are intended for online monitoring and issue detection, not final physics-quality classification.

### Waveform constants

| Constant               |  Value | Meaning                                                              |
| ---------------------- | -----: | -------------------------------------------------------------------- |
| `kWaveformNBins`       |   `64` | Fixed number of bins stored for live and first-hit waveform displays |
| `kWaveformSizeHistMax` |  `200` | Waveform-size monitoring range and overflow threshold                |
| `kBaselineSamples`     |    `5` | Number of first samples used for baseline and RMS                    |
| `kExpectedPeakTick`    |   `30` | Reference tick for peak-time residual calculation                    |
| `kSaturationAdc`       | `4090` | ADC value counted as saturation                                      |
| `kMinShapeSamples`     |    `5` | Minimum waveform size needed for pulse-shape scoring                 |

### Good/bad thresholds

| Threshold            |      Value | Meaning                                          |
| -------------------- | ---------: | ------------------------------------------------ |
| `kMaxGoodRms`        | `20.0` ADC | RMS above this is treated as high-noise          |
| `kMinGoodSnr`        |      `5.0` | `amp / RMS` below this is treated as low SNR     |
| `kMaxGoodShapeScore` |      `1.0` | Shape score above this is treated as bad/noisy   |
| `kMinDenomForAsym`   |  `5.0` ADC | If `abs(L + R) <= 5`, asymmetry is skipped       |
| `kPairMissHealth`    |     `0.75` | Health score assigned for a missing SiPM partner |

### Shape classification priority

Waveform shape is classified in priority order:

| Priority | Shape class  | Rule                                                             |
| -------: | ------------ | ---------------------------------------------------------------- |
|        1 | `Saturated`  | `nSat > 0`                                                       |
|        2 | `EdgePeak`   | `peakpos <= 1` or `peakpos >= wfSize - 2`                        |
|        3 | `Negative`   | `amp < 0`                                                        |
|        4 | `Noisy`      | `rms > kMaxGoodRms`                                              |
|        5 | `Noisy`      | `shapeScore > kMaxGoodShapeScore`                                |
|        6 | `LongTail`   | sample at `peakpos + 8` is more than `50%` of the peak amplitude |
|        7 | `Undershoot` | sample at `peakpos + 3` is below `-5 * max(1, RMS)`              |
|        8 | `Good`       | None of the above conditions fired                               |

Additional shape constants:

| Constant            |  Value | Meaning                                        |
| ------------------- | -----: | ---------------------------------------------- |
| `kLongTailFraction` | `0.50` | Tail is flagged when `tail / peak > 0.50`      |
| `kUndershootSigma`  |  `5.0` | Undershoot is flagged below `-5 * max(1, RMS)` |

### Trend and range constants

| Constant           |             Value | Meaning                                                                |
| ------------------ | ----------------: | ---------------------------------------------------------------------- |
| `kEventBlockSize`  |      `100` events | Event trend histograms use 100 events per block                        |
| `kEventTrendNBins` |            `4000` | Number of bins in long event-trend profiles                            |
| `kBoardTrendMerge` | `10` event blocks | Board health trend compression factor                                  |
| `kBoardTrendNBins` |             `400` | Number of bins in board-level channel health trends                    |
| `kEvtDigisHistMax` |            `1000` | Upper edge for `h_evt_digis`; larger events increment overflow counter |
| `kAmpHistMin`      |      `-200.0` ADC | Lower amplitude histogram range                                        |
| `kAmpHistMax`      |      `2000.0` ADC | Upper amplitude histogram range                                        |

### Streaming and safety constants

| Constant                |        Value | Meaning                                                  |
| ----------------------- | -----------: | -------------------------------------------------------- |
| `kDiskMapsExtraPeriod`  | `100` events | Disk-map streaming period is `freqDQM + 100`             |
| `kMaxSendErrors_`       |         `10` | Streaming is disabled after 10 consecutive send failures |
| `kMaxSipmIdForMaps_`    |      `10000` | Safety cap for SiPM-indexed arrays                       |
| `kMaxCrystalIdForMaps_` |       `5000` | Safety cap for crystal-indexed pairing arrays            |

---

## DQM summary formulas

`h_dqm_summary` is updated once per event. Most bins are event-local status values, while profile/trend histograms accumulate their means over time.

The main status components are computed conceptually as:

```text
HasEvents     = 1
HasDigis      = 1 if current event has at least one CaloDigi, else 0
PairComplete  = 1 - min(1, nUnpairedSipms / nRepresentativeSipms)
SaturationOK  = 1 - min(1, nSatWaveforms / nHealthSamples)
RMS_OK        = 1 - min(1, nHighRms / nHealthSamples)
LaserSeen     = 1 if positive laser amplitude was accepted, else 0
DetLaserRatio = 1 if detector amplitude and laser amplitude are both available, else 0
HealthOK      = mean event health score, including waveform-health entries and mild pair-miss health penalty entries
Overall       = average of the main status quantities
```
`LaserSeen` and `DetLaserRatio` can be `0` in data-taking modes where laser digis are not expected; in that case, they should not be interpreted as detector failures, and `Overall` should be interpreted with the run mode in mind.

If no representative SiPMs are available, PairComplete is set to 1.0. If no health samples are available, SaturationOK, RMS_OK, and HealthOK are set to 1.0. LaserSeen and DetLaserRatio are set to 0.0 unless positive accepted laser amplitude is available.

Overall includes the laser-related status bins, so runs without expected laser data can have a reduced Overall value even when regular calorimeter data are healthy.

In the current implementation, `nHealthSamples` includes waveform-health entries and mild pair-miss health penalty entries. 

Implementation caveat: the current summary calculation uses a shared health-sample denominator. As a result, pair-miss penalty entries can affect the denominator used by `SaturationOK` and `RMS_OK`. These bins should therefore be interpreted as compact operational indicators rather than independent physics-quality efficiencies.

Important interpretation detail:

* `Overall` is a compact operational indicator, not a physics-quality metric.
* A low `Overall` value should be followed by checking `h_dqm_issue_counts`, `h_issue_board`, `h_health_board`, and `h_skip_reason`.

---

## DQM summary panel

`h_dqm_summary` is a compact status bar intended for shifters and online monitoring.

| Bin | Label           | Meaning                                                                   |
| --: | --------------- | ------------------------------------------------------------------------- |
|   1 | `HasEvents`     | The analyzer has processed events                                         |
|   2 | `HasDigis`      | The current event contains at least one `CaloDigi`                        |
|   3 | `PairComplete`  | Fraction-like score for SiPM partner completeness                         |
|   4 | `SaturationOK`  | Fraction-like score for waveforms without saturation                      |
|   5 | `RMS_OK`        | Fraction-like score for waveforms with acceptable RMS                     |
|   6 | `LaserSeen`     | At least one accepted positive-amplitude laser digi was seen in the event |
|   7 | `DetLaserRatio` | Detector/laser amplitude ratio was available for the event                |
|   8 | `HealthOK`      | Mean event health score: waveform health and mild pair-miss health penalties |
|   9 | `Overall`       | Average of the main status quantities                                     |

The summary values are bounded between `0` and `1`:

```text
1.0 = good / available / passing
0.0 = bad / missing / failing
```

`h_dqm_summary_block` stores the overall DQM status versus event block.

---

## Configuration

The module is configured through FHiCL. The analyzer block can be written as:

```fhicl
physics.analyzers.caloDigiDQM : {
  module_type: "CaloDigiDQM"
  
  # Optional; this is the default.
  sendHists: false
}

physics.e1 : [ caloDigiDQM ]
```

Additional parameters can be provided to override the module defaults.

A complete test configuration is provided at: `otsdaq-mu2e-dqm/test/test_CaloDQM.fcl`.

That file reads an input .art file, runs CaloDigisFromDTCEvents to produce a CaloDigiCollection, and then runs CaloDigiDQM on the produced collection.

Run example from the `otsdaq-mu2e-dqm/test/` directory:

```bash
mu2e -c test_CaloDQM.fcl
```

### Configuration parameters

| Parameter | Type | Default | Meaning |
| --------- | ---- | ------- | ------- |
| `caloDigiModuleLabel` | string | `"CaloDigisFromDTCEvents"` | Input tag label for the `CaloDigiCollection` |
| `sendHists` | bool | `false` | Enables streaming through `ots::HistoSender` |
| `address` | string | `"mu2e-dl-01-data.fnal.gov"` | otsdaq receiver address |
| `port` | int | `6000` | otsdaq receiver port |
| `moduleTag` | string | `"CaloDigiDQM"` | Top-level namespace for streamed histogram paths |
| `freqDQM` | int | `100` | Event cadence for summary/board/global streaming; `0` disables summary streaming |
| `freqWaveforms` | int | `0` | Event cadence for live and first-hit waveform streaming; `0` disables waveform streaming |
| `enableDiskMaps` | bool | `false` | Enables disk-map streaming; disk maps are still written to ROOT regardless |
| `diskCombines` | sequence<string> | `["asym"]` | Disk-map modes to stream |
| `useReferenceFile` | bool | `false` | Enables loading optional reference histograms |
| `referenceFile` | string | `"reference.root"` | Path to reference ROOT file |

Configuration validation:

* `freqDQM` must be `>= 0`.
* `freqWaveforms` must be `>= 0`.
* If `sendHists=true`, then `address` and `moduleTag` must be non-empty, and `port` must be positive.
* Unknown `diskCombines` entries throw a `BADCONFIG` exception.

---

## ROOT output structure

The output ROOT file is organized into top-level folders:

```text
output.root
+-- <analyzer_label>/
    +-- Global_Histograms/
    +-- Disk0/
    +-- Disk1/
    +-- Laser/
```

The ROOT-file top-level folder is determined by the FHiCL analyzer label, for example `caloDigiDQM` or `myAnalysis`. The online streaming namespace is determined separately by `moduleTag`.

### `Global_Histograms/`

Contains full-detector summaries, DQM status histograms, issue counters, topology diagnostics, waveform diagnostics, global profiles, and disk maps.

Important objects include:

```text
h_dqm_summary
h_dqm_issue_counts
h_dqm_run_counters
h_dqm_summary_block
h_health_board
h_health_block
h_issue_board
h_occ_sparse
h_occ_dense
h_base_sparse
h_base_dense
h_rms_sparse
h_rms_dense
h_amp_sparse
h_amp_dense
h_max_sparse
h_max_dense
h_asym
h_board_vs_channel
h_occ_board
h_occ_board_norm
g_nhits_ewt
h_asym_chanid
h_amp_dist
h_waveform_density
h_waveform_size
h_skip_reason
disk0_Amp,       disk1_Amp
disk0_Sum,       disk1_Sum
disk0_Asym,      disk1_Asym
disk0_Baseline,  disk1_Baseline
disk0_RMS,       disk1_RMS
```

### `Disk0/` and `Disk1/`

Each disk folder contains a disk-level board quality matrix and board folders prebooked from `CaloDAQMap`.

Example:

```text
Disk0/
+-- D0_BoardQualityMatrix
+-- Board_027/
    +-- Histograms/
    |   +-- D0_B027_Occupancy
    |   +-- D0_B027_Baseline
    |   +-- D0_B027_RMS
    |   +-- D0_B027_Max
    |   +-- D0_B027_ChannelQualityMatrix
    |   +-- D0_B027_ChannelIssueMap
    |   +-- D0_B027_ChannelHealthTrend
    |   +-- D0_B027_C00_Waveform
    +-- Channels/
        +-- D0_B027_C00_FirstHit
        +-- Baseline/
        |   +-- D0_B027_C00_BaselineDist
        +-- RMS/
        |   +-- D0_B027_C00_RMSDist
        +-- Max/
        |   +-- D0_B027_C00_MaxDist
        +-- Asym/
            +-- D0_B027_C00_AsymDist
```

Board and channel folders are generally prebooked from `CaloDAQMap` at the start of the first event. A folder or histogram may therefore exist even if that channel did not appear in the processed data. First-hit waveform histograms are still created only when a channel is actually observed.

### `Laser/`

The laser board is separated from regular disk boards:

```text
Laser/
+-- Board_160/
    +-- Histograms/
    |   +-- B160_Occupancy
    |   +-- B160_Baseline
    |   +-- B160_RMS
    |   +-- B160_Max
    |   +-- B160_C00_Waveform
    +-- Channels/
        +-- B160_C00_FirstHit
        +-- Baseline/
        |   +-- B160_C00_BaselineDist
        +-- RMS/
        |   +-- B160_C00_RMSDist
        +-- Max/
            +-- B160_C00_MaxDist
```

---

## Saved versus streamed histograms

Not every ROOT histogram is streamed online. This is intentional.

| Histogram category    | Saved to ROOT | Streamed online | Notes |
| --------------------- | ------------- | --------------- | ----- |
| Global summaries      | Yes | Yes, if `sendHists=true` and `freqDQM > 0` | Core DQM status and global diagnostics |
| Board summaries       | Yes | Yes, if `sendHists=true`, `freqDQM > 0`, and boards were updated | Only active/updated boards are sent |
| Channel distributions | Yes | No by default | Prebooked from `CaloDAQMap`; filled only after the channel appears |
| Live waveforms        | Yes | Yes, if `sendHists=true`, `freqWaveforms > 0`, and channels were updated | Prebooked from `CaloDAQMap`; filled only after the channel appears |
| First-hit waveforms   | Yes | Yes, if `sendHists=true` and `freqWaveforms > 0` | Created after the first observed hit and sent once on a waveform streaming cadence |
| Disk maps             | Yes | Yes, if `sendHists=true`, `enableDiskMaps=true`, `freqDQM > 0`, and the disk-map cadence is reached | Controlled by `enableDiskMaps` and `diskCombines` |
| Board health trends   | Yes | No by default | Large payload; saved for offline inspection |
| Laser board summaries | Yes | Yes, if `sendHists=true`, `freqDQM > 0`, and the laser board was updated | Board 160 only |
| Reference overlays    | Loaded optionally into memory | Narrow supported subset | See reference section |

---

## Histogram naming conventions

The module uses consistent object names to make ROOT-file navigation easier.

### Regular boards and channels

```text
D<disk>_B<board>_<quantity>
D<disk>_B<board>_C<channel>_<quantity>
```

Examples:

```text
D0_B027_Occupancy
D0_B027_Baseline
D0_B027_C00_Waveform
D0_B027_C00_FirstHit
D0_B027_C00_BaselineDist
```

### Laser board

Laser histograms use board `160` without disk numbering:

```text
B160_<quantity>
B160_C<channel>_<quantity>
```

Examples:

```text
B160_Occupancy
B160_Baseline
B160_C00_Waveform
B160_C00_FirstHit
```

### Disk maps

Disk maps use:

```text
disk<disk>_<mode>
```

Examples:

```text
disk0_Amp
disk1_Asym
disk0_Baseline
```

---

## Histogram guide

### Shifter-level summary histograms

These are the first histograms to check during online running.

| Histogram             | Purpose                          | What to look for                                                       |
| --------------------- | -------------------------------- | ---------------------------------------------------------------------- |
| `h_dqm_summary`       | Compact current DQM status panel | Values near `1` are good; low bins identify failing categories         |
| `h_dqm_summary_block` | Overall status vs event block    | Sudden drops indicate run-time changes                                 |
| `h_dqm_issue_counts`  | Counts by issue type             | Dominant issue category: saturation, high RMS, pair misses, etc.       |
| `h_dqm_run_counters`  | Mixed cumulative & current-event | Events processed, digis, skipped digis, send errors, overflow counters |
| `h_health_board`      | Mean health score by board       | Boards with low health should be inspected                             |
| `h_health_block`      | Mean health score over time      | Time-dependent degradation                                             |
| `h_issue_board`       | Issue type vs board              | Shows which boards are producing which issues                          |

### Global occupancy and channel profiles

| Histogram                       | Purpose                                                                       |
| ------------------------------- | ----------------------------------------------------------------------------- |
| `h_occ_sparse`                  | Full-detector occupancy using readable sparse encoding `boardID*100 + chanID` |
| `h_occ_dense`                   | Full-detector occupancy using compact dense encoding `boardID*20 + chanID`    |
| `h_board_vs_channel`            | Board/channel occupancy heatmap, including laser board 160                    |
| `h_base_sparse`, `h_base_dense` | Mean baseline by encoded channel                                              |
| `h_rms_sparse`, `h_rms_dense`   | Mean baseline RMS by encoded channel                                          |
| `h_amp_sparse`, `h_amp_dense`   | Mean baseline-subtracted amplitude by encoded channel                         |
| `h_max_sparse`, `h_max_dense`   | Mean raw peak ADC by encoded channel                                          |

Use sparse histograms when diagnosing board-level patterns visually. Use dense histograms when a compact full-detector axis is more useful.

### Additional monitoring objects

| Histogram / object | Purpose |
| ------------------ | ------- |
| `D0_BoardQualityMatrix`, `D1_BoardQualityMatrix` | Disk-level board quality matrices. The x-axis is local board ID within the disk, and the y-axis is the quality metric. Values are averaged scores where `1` is good and `0` is bad. |
| `h_occ_board` | Raw occupancy count by global board ID, including regular boards and laser board 160. |
| `h_occ_board_norm` | Normalized board occupancy. This is useful for shifter displays because it shows the fraction of total hits per board rather than raw counts. |
| `g_nhits_ewt` | Average number of `CaloDigi` objects versus art event number. This provides a compact event-rate or occupancy trend over the run. |
| `h_asym_chanid` | Left/right asymmetry versus sparse encoded channel ID, `boardID*100 + chanID`. This helps identify channels or boards with localized asymmetry patterns. |
| `h_amp_dist` | Global distribution of baseline-subtracted amplitudes for accepted digis. |
| `h_skip_reason` | Counts of skipped or rejected digis and skipped derived calculations by reason, such as unmapped raw ID, invalid peak position, or too-small asymmetry denominator. |

### Event and time-trend histograms

| Histogram           | Purpose                                                                |
| ------------------- | ---------------------------------------------------------------------- |
| `h_evt_digis`       | Number of CaloDigis per event                                          |
| `h_digis_block`     | Mean digis per event block                                             |
| `h_base_block`      | Mean baseline vs event block                                           |
| `h_rms_block`       | Mean RMS vs event block                                                |
| `h_amp_block`       | Mean detector amplitude vs event block                                 |
| `h_laser_block`     | Mean laser amplitude vs event block                                    |
| `h_amp_laser_block` | Mean detector amplitude divided by mean laser amplitude vs event block |

Event blocks use `kEventBlockSize = 100` events per block.

### Waveform quality histograms

| Histogram                    | Purpose                                           |
| ---------------------------- | ------------------------------------------------- |
| `h_waveform_size`            | Distribution of waveform lengths                  |
| `h_waveform_density`         | Tick vs ADC density for all valid digis           |
| `h_peak_amp`                 | Peak tick vs baseline-subtracted amplitude        |
| `h_time_resid_dist`          | Peak-time residual distribution                   |
| `h_time_resid_board`         | Mean peak-time residual by board                  |
| `h_time_resid_board_channel` | Mean peak-time residual by board/channel          |
| `h_sat_samples`              | Number of saturated samples per waveform          |
| `h_sat_ok_board`             | Fraction of waveforms without saturation by board |
| `h_amp_rms`                  | Amplitude vs baseline RMS                         |
| `h_snr_board`                | Mean amplitude/RMS by board                       |
| `h_pulse_shape_score`        | Distribution of pulse-shape roughness score       |
| `h_pulse_shape_score_board`  | Mean pulse-shape roughness by board               |
| `h_shape_class_board`        | Waveform shape class vs board                     |
| `h_shape_class_channel`      | Waveform shape class vs dense encoded channel     |
| `h_badness_board_channel`    | Mean channel badness score by board/channel       |

### Pair and topology histograms

| Histogram                | Purpose                                                |
| ------------------------ | ------------------------------------------------------ |
| `h_pair_ok_board`        | Fraction of SiPMs with partner present by board        |
| `h_unpaired_evt`         | Number of unpaired SiPMs per event                     |
| `h_pair_miss_frac_block` | Pair-missing fraction vs event block                   |
| `h_pair_raw_delta`       | Difference between odd-side and even-side raw IDs      |
| `h_pair_board_delta`     | Difference between odd-side and even-side board IDs    |
| `h_pair_multiplicity`    | Maximum usable digi multiplicity per side of a crystal |
| `h_lr_corr`              | Left amplitude vs right amplitude                      |
| `h_sum_asym`             | Crystal sum amplitude vs left/right asymmetry          |
| `h_asym`                 | Left/right asymmetry distribution                      |
| `h_asym_board`           | Mean left/right asymmetry by board                     |

These histograms are useful for finding mapping issues, missing partners, unexpected board transitions, left/right imbalance, or repeated digis in the same event.

### Disk maps

Disk maps are `THMu2eCaloDisk` objects saved for both disks and all modes:

| Object pattern                     | Meaning                                   |
| ---------------------------------- | ----------------------------------------- |
| `disk0_Amp`, `disk1_Amp`           | Mean SiPM amplitude, `peak - baseline`    |
| `disk0_Sum`, `disk1_Sum`           | Mean crystal pair sum, `L + R`            |
| `disk0_Asym`, `disk1_Asym`         | Mean crystal asymmetry, `(L - R)/(L + R)` |
| `disk0_Baseline`, `disk1_Baseline` | Mean SiPM baseline                        |
| `disk0_RMS`, `disk1_RMS`           | Mean SiPM baseline RMS                    |

Important behavior:

* All disk-map modes are written to the ROOT file.
* `diskCombines` controls only which modes are streamed online.
* Selected streamed disk-map modes are refreshed before disk-map streaming.
* All disk-map modes are refreshed at endJob() before writing the final ROOT output.
* Asymmetry values are displayed with range `[-1, 1]`.

### Board-level histograms

Each mapped regular board can be prebooked with:

| Histogram                    | Purpose                              |
| ---------------------------- | ------------------------------------ |
| `D*_B*_Occupancy`            | Per-channel occupancy on the board   |
| `D*_B*_Baseline`             | Mean baseline by channel             |
| `D*_B*_RMS`                  | Mean RMS by channel                  |
| `D*_B*_Max`                  | Mean raw peak ADC by channel         |
| `D*_B*_ChannelQualityMatrix` | Channel vs quality metric matrix     |
| `D*_B*_ChannelIssueMap`      | Channel vs issue type counts         |
| `D*_B*_ChannelHealthTrend`   | Channel health vs coarse event block |

`ChannelHealthTrend` is saved to ROOT but is not streamed by default because it can dominate the payload size for many active boards.

### Channel-level histograms

Each mapped regular channel can have:

| Histogram               | Purpose                                                      |
| ----------------------- | ------------------------------------------------------------ |
| `D*_B*_C*_Waveform`     | Latest cached live waveform, baseline-subtracted             |
| `D*_B*_C*_FirstHit`     | First observed waveform for the channel, baseline-subtracted |
| `D*_B*_C*_BaselineDist` | Baseline distribution                                        |
| `D*_B*_C*_RMSDist`      | RMS distribution                                             |
| `D*_B*_C*_MaxDist`      | Raw peak ADC distribution                                    |
| `D*_B*_C*_AsymDist`     | Pair asymmetry distribution for that channel                 |

`D*_B*_C*_FirstHit` is created only after the channel appears in data.

Live waveform histograms store the most recent representative waveform and are flushed to the ROOT file at `endJob()`.

### Laser histograms

Laser board `160` is monitored separately:

| Histogram                   | Purpose                                                       |
| --------------------------- | ------------------------------------------------------------- |
| `B160_Occupancy`            | Laser channel occupancy                                       |
| `B160_Baseline`             | Mean laser baseline by channel                                |
| `B160_RMS`                  | Mean laser RMS by channel                                     |
| `B160_Max`                  | Mean laser raw peak ADC by channel                            |
| `B160_C*_Waveform`          | Latest cached laser waveform, baseline-subtracted             |
| `B160_C*_FirstHit`          | First observed laser waveform, baseline-subtracted            |
| `B160_C*_BaselineDist`      | Laser baseline distribution                                   |
| `B160_C*_RMSDist`           | Laser RMS distribution                                        |
| `B160_C*_MaxDist`           | Laser raw peak ADC distribution                               |
| `h_laser_block`             | Mean laser amplitude vs event block                           |
| `h_amp_laser_block`         | Detector mean amplitude / laser mean amplitude vs event block |
| `h_amp_laser_board_channel` | Detector channel amplitude normalized by mean laser amplitude |

Laser normalization is filled only when the event has positive accepted laser amplitude.

---

## Live and first-hit waveform behavior

The module stores two kinds of waveform histograms:

| Waveform type      | Meaning                                      | Update behavior                                        |
| ------------------ | -------------------------------------------- | ------------------------------------------------------ |
| Live waveform      | Latest representative waveform for a channel | Updated when a new representative waveform is selected |
| First-hit waveform | First observed waveform for a channel        | Filled once and then kept unchanged                    |

Waveforms are baseline-subtracted before being stored:

```text
stored bin value = waveform[tick] - baseline
```

If the input waveform is shorter than `kWaveformNBins`, remaining bins are padded with zero. If it is longer, only the first `kWaveformNBins` samples are stored in live/first-hit waveform displays.

The module also tracks waveform-size changes and prints the top variable-size channels at `endJob()`.

---

## Laser normalization

Laser data are processed separately through board `160`. For each event, the module computes:

```text
meanLaserAmp = mean positive laser amplitude in the event
```

When both regular detector amplitude and laser amplitude are available, the module fills:

```text
mean detector amplitude / mean laser amplitude
```

This appears in:

| Histogram                   | Meaning                                                                |
| --------------------------- | ---------------------------------------------------------------------- |
| `h_laser_block`             | Mean laser amplitude vs event block                                    |
| `h_amp_laser_block`         | Mean detector amplitude divided by mean laser amplitude vs event block |
| `h_amp_laser_board_channel` | Detector channel amplitude normalized by mean event laser amplitude    |

These histograms help distinguish detector response changes from laser-amplitude changes.

---

## Streaming behavior

Streaming is enabled only when:

```text
sendHists = true
```

The module creates one `ots::HistoSender` using the configured `address` and `port`.

### Streaming categories

| Category                | Controlled by                        | Notes                                           |
| ----------------------- | ------------------------------------ | ----------------------------------------------- |
| Global summaries        | `freqDQM`                            | Includes global histograms and DQM status panel |
| Board summaries         | `freqDQM`                            | Only updated boards are streamed                |
| Laser board summary     | `freqDQM`                            | Streamed only if laser board was updated        |
| Disk maps               | `enableDiskMaps` and `freqDQM + 100` | Streamed less frequently than summaries         |
| Live waveforms          | `freqWaveforms`                      | Only updated channels are streamed              |
| First-hit waveforms     | `freqWaveforms`                      | Sent once per active channel on a waveform streaming cadence      |
| Global waveform density | `freqWaveforms`                      | Sent only when updated                          |

If no relevant histograms are updated, the module skips the send call.

### Streaming paths

The streamed paths are built under `moduleTag` and use `:replace` semantics.

Examples for `moduleTag = "CaloDigiDQM"`:

```text
CaloDigiDQM/Global:replace
CaloDigiDQM/DQM_Summary:replace
CaloDigiDQM/Shifter:replace
CaloDigiDQM/DiskMaps/Asym:replace
CaloDigiDQM/Disk0/Board027:replace
CaloDigiDQM/Waveforms/Disk0/Board027/Channel00:replace
CaloDigiDQM/OneHitWaveforms/Disk0/Board027/Channel00:replace
CaloDigiDQM/Laser/Board160:replace
CaloDigiDQM/Laser/Waveforms/Board160/Channel00:replace
CaloDigiDQM/Laser/OneHitWaveforms/Board160/Channel00:replace
CaloDigiDQM/Waveforms/GlobalWaveformDensity:replace
```

### Send-error handling

Streaming failures are caught and logged. The module keeps the queued histograms after transient failures so they can be retried.

If `HistoSender::sendHistograms` fails `10` consecutive times:

* summary queues are cleared,
* waveform queues are cleared,
* streaming is disabled,
* the `HistoSender` is reset,
* the ROOT file continues to be filled normally.

Total send errors are tracked in `h_dqm_run_counters`.

---


## Reference histogram support

Reference histogram support is experimental and intentionally limited.

If `.rcfg` dashboards with the `superimposed` option provide a cleaner and more efficient way to display reference overlays in the otsdaq visualizer, this feature may be revised or removed in the future.

If `useReferenceFile=true`, the module attempts to load selected objects from `referenceFile`. Missing files or missing objects do not stop the job; the module logs a warning and continues.

Reference histograms are loaded from the top level of the reference ROOT file by exact object name. They are cloned into memory and may be included in selected streaming groups, but they are not part of the normal output ROOT histogram structure.

Currently supported reference objects:

```text
ref_h_occ_dense
ref_h_base_dense
ref_h_rms_dense
ref_h_max_dense
ref_h_asym
ref_D0_B027_Occupancy
ref_D0_B027_Baseline
ref_D0_B027_RMS
ref_D0_B027_Max
ref_D0_B027_C00_Waveform
```

Reference histograms are cloned into memory with `SetDirectory(nullptr)` so the input file can close safely.

Current limitation: reference streaming is intentionally narrow and mostly demonstrates overlays for global dense profiles and one example board/channel: Disk 0, Board 27, Channel 0.

---

## Skip reasons and diagnostics

Rejected digis and skipped derived calculations are counted in `h_skip_reason` and printed in the `endJob()` summary.

| Skip reason              | Meaning                                                    |
| ------------------------ | ---------------------------------------------------------- |
| `BadSipmId`              | Negative offline SiPM ID                                   |
| `UnmappedRawId`          | `CaloDAQMap` returned the unmapped sentinel raw ID         |
| `OutOfRangeSipmId`       | SiPM ID is outside the internal vector cap                 |
| `RawIdNegative`          | Decoded raw ID is negative                                 |
| `DiskOutOfRange`         | Regular channel does not map to a valid disk/board/channel |
| `PeakPosOutOfRange`      | Peak position is outside waveform bounds                   |
| `NonFiniteBaselineOrRms` | Baseline or RMS is not finite                              |
| `TinyDenomAsym`          | `L + R` is too small for stable asymmetry calculation      |

Overflow-style counters are stored in `h_dqm_run_counters`:

| Counter                | Meaning                                       |
| ---------------------- | --------------------------------------------- |
| `EvtDigisOverflow`     | Events with `nDigis >= kEvtDigisHistMax`      |
| `WaveformSizeOverflow` | Waveforms with size `>= kWaveformSizeHistMax` |
| `AmpOverflow`          | Amplitudes outside the histogram range        |

### Run counter bin meanings

`h_dqm_run_counters` stores a mix of cumulative run counters and current-event values:

| Bin label              | Meaning                                                       |
| ---------------------- | ------------------------------------------------------------- |
| `Events`               | Number of processed events                                    |
| `DigisThisEvent`       | Number of `CaloDigi` objects in the current event             |
| `MappedDisk0Digis`     | Total regular digis mapped/classified to Disk 0               |
| `MappedDisk1Digis`     | Total regular digis mapped/classified to Disk 1               |
| `MappedLaserDigis`     | Total digis mapped/classified to laser board 160              |
| `SkippedTotal`         | Total skip/diagnostic counts, including rejected digis and skipped derived calculations  |
| `UnpairedThisEvent`    | Number of unpaired representative SiPMs in the current event  |
| `TotalSendErrors`      | Total histogram streaming send failures                       |
| `EvtDigisOverflow`     | Number of events exceeding the `h_evt_digis` display range    |
| `WaveformSizeOverflow` | Number of waveforms exceeding the waveform-size display range |
| `AmpOverflow`          | Number of amplitudes outside the amplitude histogram range    |

These counters are useful for fast sanity checks and for distinguishing detector/data issues from online-streaming issues.

---

## `endJob()` behavior

At the end of the job, the module:

1. Flushes all cached regular live waveforms to their ROOT histograms.
2. Flushes all cached laser live waveforms to their ROOT histograms.
3. Refreshes all disk maps.
4. Prints a summary of processed events, mapped disk/laser digis, invalid regular mappings, send errors, out-of-range SiPM IDs, and skip counts.
5. Prints a waveform-size summary for regular channels.
6. Prints a waveform-size summary for laser channels.
7. Reports the top variable-size waveform channels, sorted by number of waveform-size transitions.

This makes the output ROOT file complete even when live waveform streaming was disabled or did not occur on the final event.

---

## Performance and memory design

The module avoids unnecessary memory and network overhead through several design choices:

* The expected board/channel structure is prebooked from `CaloDAQMap` at the start of the first event.
* First-hit waveform histograms remain lazily booked because they require an actual waveform sample.
* Per-event SiPM state uses stamp-based validity instead of clearing large vectors every event.
* Live waveforms are cached in fixed-size arrays of `64` bins.
* Streaming queues use flags to avoid duplicate growth between send attempts.
* Only updated boards and updated waveform channels are streamed.
* Disk maps are streamed less frequently than normal summaries.
* Large board health-trend histograms are written to ROOT but not streamed by default.
* First-hit waveforms are streamed only once per active channel.

### Stamp-based event state

The module avoids clearing large per-SiPM vectors every event. Instead, it uses a monotonically increasing `pairStamp_`:

```text
entry is valid for current event if storedStamp == pairStamp_
```

This pattern is used for:

* representative SiPM features,
* paired-crystal state,
* per-event SiPM multiplicity.

Rollover protection resets stamps if `pairStamp_` reaches `std::numeric_limits<int>::max()`.

---

## Recommended online dashboards

### Shifter dashboard

Use this for fast operational monitoring. In the online visualizer, this dashboard can combine objects from the `Shifter`, `DQM_Summary`, and `Global` streaming groups.

Shifter-focused objects:

* `h_occ_board_norm` - normalized occupancy by board ID
* `g_nhits_ewt` - average number of `CaloDigi` objects versus art event number
* `h_amp_sparse` - mean amplitude versus sparse encoded channel ID
* `h_asym_chanid` - asymmetry versus sparse encoded channel ID

Core DQM status objects:

* `h_dqm_summary` - compact pass/fail-style DQM status panel
* `h_dqm_issue_counts` - issue counts by issue type
* `h_dqm_run_counters` - processed events, digis, skips, send errors, and overflow counters
* `h_health_board` - mean health score by board
* `h_issue_board` - issue type versus board
* `h_pair_ok_board` - pair completeness by board

Useful global occupancy objects:

* `h_occ_sparse` - full-detector occupancy using sparse encoded channel ID
* `h_board_vs_channel` - board/channel occupancy heatmap

This dashboard answers:

* Are events and digis present?
* Are major DQM checks passing?
* Which issue type dominates?
* Which boards look unhealthy?
* Are channels or boards missing?
* Is the occupancy pattern reasonable?
* Are left/right asymmetry problems localized?

### Calorimeter expert dashboard

Use this for detailed detector diagnosis:

```text
h_base_dense
h_rms_dense
h_amp_dense
h_max_dense
h_waveform_density
h_amp_rms
h_snr_board
h_time_resid_board_channel
h_shape_class_board
h_badness_board_channel
h_sum_asym
h_lr_corr
```

This dashboard answers:

* Are baselines stable?
* Is noise increasing?
* Are waveforms saturating?
* Are peaks at the expected time?
* Are left/right channels balanced?
* Are certain boards producing abnormal pulse shapes?

### Mapping and topology dashboard

Use this when checking DAQ mapping, SiPM pairing, or board assignment:

```text
h_pair_raw_delta
h_pair_board_delta
h_pair_miss_frac_block
h_pair_ok_board
h_pair_multiplicity
h_board_vs_channel
h_skip_reason
```

This dashboard answers:

* Are even/odd SiPM partners appearing together?
* Do partners map to expected neighboring electronics IDs?
* Are pair misses localized to certain boards?
* Are multiple digis per SiPM common?
* Are many digis being skipped due to mapping problems?

### Laser monitoring dashboard

Use this when checking laser data and detector/laser normalization:

```text
<analyzer_label>/Laser/Board_160/Histograms/B160_Occupancy
<analyzer_label>/Laser/Board_160/Histograms/B160_Baseline
<analyzer_label>/Laser/Board_160/Histograms/B160_RMS
<analyzer_label>/Laser/Board_160/Histograms/B160_Max
<analyzer_label>/Global_Histograms/h_laser_block
<analyzer_label>/Global_Histograms/h_amp_laser_block
<analyzer_label>/Global_Histograms/h_amp_laser_board_channel
```

This dashboard answers:

* Is laser board 160 present?
* Are laser amplitudes stable over time?
* Are detector amplitudes stable relative to the laser amplitude?
* Are normalized detector channels showing localized changes?

### Waveform dashboard

Use this for waveform-shape and channel-level debugging:

```text
h_waveform_density
h_waveform_size
h_peak_amp
h_pulse_shape_score
h_shape_class_channel
Online streamed waveform paths:
Waveforms/Disk*/Board*/Channel*
OneHitWaveforms/Disk*/Board*/Channel*

ROOT-file waveform objects:
Disk*/Board_*/Histograms/D*_B*_C*_Waveform
Disk*/Board_*/Channels/D*_B*_C*_FirstHit
```

This dashboard answers:

* Are waveform lengths stable?
* Is the peak timing reasonable?
* Are live waveforms updating?
* Do first-hit waveforms look physically reasonable?
* Are channels showing saturation, undershoot, long tails, or noisy shapes?

---

## Interpretation playbook

Use this section when a dashboard shows a problem and you need to decide where to look next.

### If `h_dqm_summary` is low

Check in this order:

1. `h_dqm_issue_counts` - identify the dominant issue type.
2. `h_issue_board` - check whether the issue is localized to a board.
3. `h_health_board` - identify unhealthy boards.
4. `h_skip_reason` - check whether digis are being rejected before histogram filling.
5. Board-level `ChannelQualityMatrix` - identify affected metrics and channels.

### If occupancy looks wrong

Check:

1. `h_occ_sparse`
2. `h_board_vs_channel`
3. `h_skip_reason`
4. `h_pair_raw_delta`
5. `h_pair_board_delta`

Likely causes include missing input digis, mapping mismatch, unexpected board IDs, or channels being skipped before feature extraction.

### If noise or baseline looks wrong

Check:

1. `h_base_dense`
2. `h_rms_dense`
3. `h_base_block`
4. `h_rms_block`
5. board-level `D*_B*_Baseline` and `D*_B*_RMS`
6. channel-level baseline/RMS distributions

A global shift suggests run-condition or calibration effects. A localized feature suggests a board/channel issue.

### If waveform shape looks wrong

Check:

1. `h_waveform_density`
2. `h_shape_class_board`
3. `h_shape_class_channel`
4. `h_pulse_shape_score`
5. `h_peak_amp`
6. live and first-hit waveform histograms for affected channels

Shape problems can appear as saturation, edge peaks, long tails, undershoot, high roughness score, or negative amplitude.

### If pair completeness looks wrong

Check:

1. `h_pair_ok_board`
2. `h_pair_miss_frac_block`
3. `h_pair_raw_delta`
4. `h_pair_board_delta`
5. `h_pair_multiplicity`

Pair problems can indicate missing partner digis, mapping inconsistencies, unexpected multiplicity, or a mismatch between the even/odd SiPM pairing assumption and the input data.

### If detector/laser ratio looks wrong

Check:

1. `<analyzer_label>/Global_Histograms/h_laser_block`
2. `<analyzer_label>/Global_Histograms/h_amp_laser_block`
3. `<analyzer_label>/Global_Histograms/h_amp_laser_board_channel`
4. `<analyzer_label>/Laser/Board_160/Histograms/B160_Max`
5. regular detector amplitude profiles such as `h_amp_dense`

If laser amplitude changes and detector/laser ratio remains stable, the detector response may be stable. If the ratio changes locally, inspect the corresponding board/channel.

---

## Practical validation checklist

After running the module on a test file, check:

1. `h_dqm_run_counters` has nonzero `Events` and expected digi counts.
2. `h_skip_reason` does not show a large unexpected mapping or waveform rejection rate.
3. `h_occ_sparse` and `h_board_vs_channel` show expected active boards and channels.
4. Disk 0 and Disk 1 maps are filled after `endJob()`.
5. Board and channel folders match the expected structure from `CaloDAQMap`.
6. Empty prebooked channel histograms are understood as mapped channels that did not appear in the processed data.
7. First-hit waveform histograms are created only for channels that actually appeared.
8. First-hit waveforms are filled once and remain unchanged.
9. `h_pair_ok_board` and pair topology histograms behave as expected for paired SiPM data.
10. If laser data are present, `Laser/Board_160` and `h_laser_block` are filled.
11. If streaming is enabled, `h_dqm_run_counters` does not show increasing send errors.

---

## Common troubleshooting

### No streamed histograms appear

Check:

* `sendHists` is `true`.
* `address` and `port` point to the correct receiver.
* `moduleTag` is not empty.
* `freqDQM` or `freqWaveforms` is greater than `0`.
* The job has processed enough events to hit the configured cadence.
* `h_dqm_run_counters` does not show increasing send errors.

### ROOT file exists, but some board/channel folders are missing

The expected board/channel structure is prebooked from `CaloDAQMap` after the first event begins. If a board or channel folder is missing, check:

* at least one event was processed,
* the channel exists in `CaloDAQMap`,
* the mapped `rawId` is valid,
* the channel maps to a regular board `0-159` or laser board `160`.

First-hit waveform histograms are an exception: they are created only when the channel actually appears in the data.

### Disk maps exist in ROOT even when `enableDiskMaps=false`

This is expected. `enableDiskMaps` controls streaming only. Disk maps are always saved to ROOT.

### Disk maps are not streamed

Check:

* `sendHists=true`.
* `enableDiskMaps=true`.
* `freqDQM > 0`.
* Enough events have passed for the disk-map period: `freqDQM + 100`.
* `diskCombines` contains the desired modes.

### Waveforms are not streamed

Check:

* `freqWaveforms > 0`.
* Active channels have been updated since the previous waveform send.
* The event count has reached a multiple of `freqWaveforms`.
* Send errors have not disabled streaming.

### Many `UnmappedRawId` skips

This usually points to a `CaloDAQMap` or input-data mismatch. Check that the conditions/proditions setup matches the input data.

### Many `PeakPosOutOfRange` skips

This indicates invalid or inconsistent `peakpos` values relative to waveform size. Check the upstream digi production or waveform content.

### Many `TinyDenomAsym` skips

The pair exists, but `L + R` is too small for stable asymmetry calculation. This can happen for low-amplitude events or noise-like waveforms.

### Board health is low

Inspect, in order:

1. `h_issue_board`
2. `h_badness_board_channel`
3. the board's `ChannelQualityMatrix`
4. the board's `ChannelIssueMap`
5. live and first-hit waveforms for affected channels

---

## Common extension points

This module is organized around a few recurring extension patterns.

| Task                             | Main places to modify                                                               |
| -------------------------------- | ----------------------------------------------------------------------------------- |
| Add a global summary histogram   | data member, `bookGlobalHistograms()`, fill site, `streamIfScheduled()` if streamed |
| Add a board-level diagnostic     | `BoardHists`, `ensureBoardBooked()`, fill site, optional board streaming group      |
| Add a channel-level distribution | storage vector, `allocateBuffers()`, `ensure...Booked()` helper, optional prebooking call, fill site |
| Add a disk-map quantity          | `MapMode`, parser, suffix, titles, accumulation, README tables                      |
| Add a new issue type             | `IssueType`, `issueLabel()`, issue maps, issue-count logic, README tables           |
| Add a new quality metric         | `QualityMetric`, `qualityMetricLabel()`, quality matrix filling, README tables      |
| Add reference overlays           | `loadReferenceFile()`, streaming group, documentation                               |

---

## Developer notes

### Adding a new global histogram

1. Add the histogram pointer as a data member.
2. Book it in `bookGlobalHistograms()`.
3. Fill it in the relevant processing function.
4. Add it to the appropriate streaming group in `streamIfScheduled()` if it should appear online.
5. Add it to this README.

### Adding a new board-level histogram

1. Add the pointer to `BoardHists` if it belongs to each board.
2. Book it in `ensureBoardBooked()`.
3. Fill it from `processRegularDigi()`, `processPairs()`, or another helper.
4. Add it to the board streaming group only if the payload cost is acceptable.

### Adding a new channel-level histogram

Use the existing prebooking/fallback pattern:

* Add a storage vector.
* Allocate it in `allocateBuffers()`.
* Add an `ensure...Booked()` helper.
* Call the helper from `prebookExpectedStructureFromCaloDAQMap()` if the histogram should exist for all mapped channels.
* Keep the helper safe for runtime fallback.
* Fill only after validating `cidx` and after the channel appears in data.

### Adding a new disk-map mode

1. Add a value to `MapMode`.
2. Update `parseMode()`.
3. Update `modeSuffix()`.
4. Update `setDiskMapTitles()`.
5. Update `kNMapModes` and `kAllModes`.
6. Accumulate values with `accDisk()`.
7. Update README tables and configuration examples.

### Adding more reference overlays

1. Load the object in `loadReferenceFile()`.
2. Clone it and call `SetDirectory(nullptr)`.
3. Add it to the appropriate streaming group in `streamIfScheduled()`.
4. Keep reference names stable and document them here.

---

## Current limitations

* The detector geometry is hard-coded for 2 disks, 80 regular boards per disk, 20 channels per board, and laser board 160.
* Pairing assumes adjacent even/odd offline SiPM IDs belong to the same crystal.
* Reference overlay support is limited to a small set of hard-coded histogram names.
* `ChannelHealthTrend` is saved but not streamed by default to avoid excessive payload size.
* Live waveform histograms are prebooked from `CaloDAQMap`, but their bin contents are filled only after the corresponding channel appears in data. Cached live waveform values are flushed to the ROOT file at `endJob()`.
* Disk-map running means are accumulated over the job and refreshed before streaming and again at `endJob()`.
* Heuristic thresholds are intended for DQM monitoring and may need adjustment for different run conditions or data-taking modes.

---

## Minimal interpretation checklist

When opening a new output file, check these first:

1. `<analyzer_label>/Global_Histograms/h_dqm_summary` - are the main status checks passing?
2. `<analyzer_label>/Global_Histograms/h_dqm_run_counters` - were events and digis processed, and are send errors/skips low?
3. `<analyzer_label>/Global_Histograms/h_occ_sparse` - are expected boards/channels active?
4. `<analyzer_label>/Global_Histograms/h_issue_board` - are issues localized to specific boards?
5. `<analyzer_label>/Global_Histograms/h_health_board` - which boards need attention?
6. `<analyzer_label>/Global_Histograms/h_skip_reason` - are digis being rejected for mapping or waveform-quality reasons?
7. Disk maps - are spatial patterns visible on the calorimeter disks?
8. Board-level matrices - which channels and metrics explain the problem?
9. Live/first-hit waveforms - do affected channels look physically reasonable?

---

## Summary

`CaloDigiDQM` provides a complete monitoring path from `CaloDigi` waveform samples to detector-level DQM summaries. It combines ROOT-file persistence, online histogram streaming, geometry-aware disk maps, per-board diagnostics, per-channel waveform views, laser monitoring, pair/topology checks, and robust skip/error accounting.

The main operational idea is simple:

```text
Global summaries show whether the detector looks healthy.
Board and channel histograms show where the problem is.
Waveform and pair diagnostics help explain why the problem is happening.
Disk maps show whether the problem has a spatial detector pattern.
Laser normalization helps separate detector response changes from laser-amplitude changes.
```
