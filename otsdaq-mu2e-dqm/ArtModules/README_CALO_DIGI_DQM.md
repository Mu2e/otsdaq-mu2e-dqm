# CaloDigiDQM

`CaloDigiDQM` is an `art::EDAnalyzer` for Mu2e calorimeter Data Quality Monitoring. It reads `CaloDigi` objects, maps offline SiPM IDs to electronics identifiers through `CaloDAQMap`, fills ROOT histograms for detector health and waveform diagnostics, and can optionally stream selected histograms to the otsdaq visualizer through `ots::HistoSender`.

The module is designed for both offline ROOT-file inspection and online DQM use. ROOT output contains all histograms booked and filled for the processed data, including lazily created board/channel histograms for channels that appear in the input.

---

## Quick start

Minimal offline run:

```bash
mu2e -c running.fcl -s input.art -T output.root
```

Minimal analyzer fragment:

```fhicl
physics.analyzers.caloDigiDQM : {
  module_type: "CaloDigiDQM"
  caloDigiModuleLabel: "caloDigis"

  sendHists: false
  address: "127.0.0.1"
  port: 8000
  moduleTag: "CaloDigiDQM"

  freqDQM: 100
  freqWaveforms: 500
  enableDiskMaps: false
  diskCombines: ["asym"]

  useReferenceFile: false
  referenceFile: ""
}

physics.e1 : [ caloDigiDQM ]
```

First histograms to inspect in a new output file:

```text
Global_Histograms/h_dqm_summary
Global_Histograms/h_dqm_run_counters
Global_Histograms/h_occ_sparse
Global_Histograms/h_issue_board
Global_Histograms/h_health_board
Global_Histograms/h_skip_reason
```

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
| `ots::HistoSender`                          | Optional online histogram streaming to otsdaq          |

The module does not implement a raw data decoder. It consumes already-produced `CaloDigi` objects and focuses on DQM feature extraction, histogram organization, and optional online streaming.

---

## Main responsibilities

* Read `CaloDigiCollection` from a configurable input module label.
* Convert each offline `SiPMID` to electronics coordinates using `CaloDAQMap`.
* Fill global detector summaries for occupancy, baseline, RMS, amplitude, waveform size, pair completeness, waveform health, issue counts, and run counters.
* Fill per-disk `THMu2eCaloDisk` maps for amplitude, crystal sum, left/right asymmetry, baseline, and RMS.
* Lazily create board-level and channel-level histograms only for channels that appear in the data.
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
* Disk maps are running means accumulated over the job and refreshed before streaming and at `endJob()`.
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
rawId   = CaloDAQMap(raw SiPM id lookup)
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
| `Seen`           | Channel produced at least one usable digi             |
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

Several values in this module are intentionally heuristic. They are intended for online monitoring and anomaly detection, not final physics-quality classification.

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
|   8 | `HealthOK`      | mean event health score: waveform health, mild pair-miss health penalties |
|   9 | `Overall`       | Average of the main status quantities                                     |

The summary values are bounded between `0` and `1`:

```text
1.0 = good / available / passing
0.0 = bad / missing / failing
```

`h_dqm_summary_block` stores the overall DQM status versus event block.

---

## Configuration

The module is configured through FHiCL.

Example:

```fhicl
physics.analyzers.caloDigiDQM : {
  module_type: "CaloDigiDQM"

  # Input CaloDigi collection
  caloDigiModuleLabel: "caloDigis"

  # Streaming configuration
  sendHists: false
  address: "127.0.0.1"
  port: 8000
  moduleTag: "CaloDigiDQM"

  # Streaming cadence
  # 0 disables the corresponding stream category.
  freqDQM: 100
  freqWaveforms: 500

  # Disk maps are always written to the ROOT file.
  # This flag controls online streaming only.
  enableDiskMaps: false

  # Disk-map modes selected for streaming only.
  # Allowed: "amp", "sum", "asym", "baseline", "rms".
  # If empty, "asym" is streamed by default.
  diskCombines: ["asym"]

  # Optional reference overlay file.
  useReferenceFile: false
  referenceFile: ""
}

physics.e1 : [ caloDigiDQM ]
```

Run example:

```bash
mu2e -c running.fcl -s input.art -T output.root
```

### Configuration parameters

| Parameter             | Type             | Meaning                                                                                  |
| --------------------- | ---------------- | ---------------------------------------------------------------------------------------- |
| `caloDigiModuleLabel` | string           | Input tag label for the `CaloDigiCollection`                                             |
| `sendHists`           | bool             | Enables streaming through `ots::HistoSender`                                             |
| `address`             | string           | otsdaq receiver address; required when `sendHists=true`                                  |
| `port`                | int              | otsdaq receiver port; must be positive when `sendHists=true`                             |
| `moduleTag`           | string           | Top-level namespace for streamed histogram paths                                         |
| `freqDQM`             | int              | Event cadence for summary/board/global streaming; `0` disables summary streaming         |
| `freqWaveforms`       | int              | Event cadence for live and first-hit waveform streaming; `0` disables waveform streaming |
| `enableDiskMaps`      | bool             | Enables disk-map streaming; disk maps are still written to ROOT regardless               |
| `diskCombines`        | sequence<string> | Disk-map modes to stream                                                                 |
| `useReferenceFile`    | bool             | Enables loading optional reference histograms                                            |
| `referenceFile`       | string           | Path to reference ROOT file                                                              |

Configuration validation:

* `freqDQM` must be `>= 0`.
* `freqWaveforms` must be `>= 0`.
* If `sendHists=true`, then `address`, `port`, and `moduleTag` must be valid.
* Unknown `diskCombines` entries throw a `BADCONFIG` exception.

---

## ROOT output structure

The output ROOT file is organized into top-level folders:

```text
output.root
+-- Global_Histograms/
+-- Disk0/
+-- Disk1/
+-- Laser/
```

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

Each disk folder contains a disk-level board quality matrix and lazily booked board folders.

Example:

```text
Disk0/
+-- D0_BoardQualityMatrix
+-- Board_27/
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

Board and channel folders are created only when data appears for that board/channel.

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

| Histogram category    | Saved to ROOT     | Streamed online                         | Notes                                             |
| --------------------- | ----------------- | --------------------------------------- | ------------------------------------------------- |
| Global summaries      | Yes               | Yes, if `freqDQM > 0`                   | Core DQM status and global diagnostics            |
| Board summaries       | Yes               | Yes, for updated boards                 | Only active/updated boards are sent               |
| Channel distributions | Yes               | No by default                           | Can be numerous; intended for ROOT inspection     |
| Live waveforms        | Yes               | Yes, if updated and `freqWaveforms > 0` | Latest representative waveform                    |
| First-hit waveforms   | Yes               | Yes, once per active channel            | Static first observed waveform                    |
| Disk maps             | Yes               | Optional                                | Controlled by `enableDiskMaps` and `diskCombines` |
| Board health trends   | Yes               | No by default                           | Large payload; saved for offline inspection       |
| Laser board summaries | Yes               | Yes, if laser board updated             | Board 160 only                                    |
| Reference overlays    | Loaded optionally | Narrow supported subset                 | See reference section                             |

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
* Disk maps are refreshed before streaming and again in `endJob()`.
* Asymmetry values are displayed with range `[-1, 1]`.

### Board-level histograms

Each active regular board gets:

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

Each active regular channel can get:

| Histogram               | Purpose                                                      |
| ----------------------- | ------------------------------------------------------------ |
| `D*_B*_C*_Waveform`     | Latest cached live waveform, baseline-subtracted             |
| `D*_B*_C*_FirstHit`     | First observed waveform for the channel, baseline-subtracted |
| `D*_B*_C*_BaselineDist` | Baseline distribution                                        |
| `D*_B*_C*_RMSDist`      | RMS distribution                                             |
| `D*_B*_C*_MaxDist`      | Raw peak ADC distribution                                    |
| `D*_B*_C*_AsymDist`     | Pair asymmetry distribution for that channel                 |

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
| `B160_C*_FirstHit`          | First observed laser waveform - baseline for that channel     |
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
| First-hit waveforms     | `freqWaveforms`                      | Sent once per active channel after booking      |
| Global waveform density | `freqWaveforms`                      | Sent only when updated                          |

If no relevant histograms are updated, the module skips the send call.

### Streaming paths

The streamed paths are built under `moduleTag` and use `:replace` semantics.

Examples for `moduleTag = "CaloDigiDQM"`:

```text
CaloDigiDQM/Global:replace
CaloDigiDQM/DQM_Summary:replace
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
| `SkippedTotal`         | Total skipped/rejected digis across all skip reasons          |
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

* Board and channel histograms are lazily booked only when data appears.
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

## Performance testing with `RandomCaloDigiProducer`

A synthetic stress test can be run with `RandomCaloDigiProducer`, an `art::EDProducer` that creates randomized `CaloDigiCollection` objects using the real `CaloDAQMap`. This is useful for testing DQM throughput, histogram booking behavior, streaming payload size, skip counters, issue counters, and memory growth without requiring a real input data file.

The producer generates paired SiPM digis for valid crystals and can inject controlled waveform/pathology types:

| Injected flavor | Purpose                                                       |
| --------------- | ------------------------------------------------------------- |
| `Good`          | Normal paired digis with Gaussian-like pulses                 |
| `PairMiss`      | Produces only one SiPM side of a crystal                      |
| `Saturated`     | Produces high-amplitude waveforms near ADC saturation         |
| `EdgePeak`      | Forces the peak close to the waveform edge                    |
| `HighRMS`       | Uses larger baseline noise                                    |
| `LowSNR`        | Uses low signal amplitude with larger noise                   |
| `NegativeAmp`   | Produces a downward pulse to test negative amplitude handling |
| `BadShape`      | Adds ringing, roughness, and tail-like structure              |

### Example stress-test configuration

```fhicl
source: {
  module_type: EmptyEvent
  maxEvents: 500000
  firstRun: 1
  firstEvent: 1
}

physics.producers.myProducer: {
  module_type: RandomCaloDigiProducer

  nDigis: 50
  waveformSize: 50
  maxADC: 4095
  seed: 1234

  fracPairMiss:    0.10
  fracSaturated:   0.08
  fracEdgePeak:    0.08
  fracHighRMS:     0.08
  fracLowSNR:      0.08
  fracNegativeAmp: 0.05
  fracBadShape:    0.08
}

physics.analyzers.myAnalysis: {
  module_type: CaloDigiDQM
  caloDigiModuleLabel: "myProducer"

  address: "127.0.0.1"
  port: 8000
  moduleTag: "SimpleDQM"

  freqDQM: 100
  freqWaveforms: 100
  sendHists: true

  enableDiskMaps: false
  diskCombines: ["amp"]

  useReferenceFile: false
  referenceFile: "./reference.root"
}

physics.p1: [ myProducer ]
physics.e1: [ myAnalysis ]
physics.trigger_paths: [ p1 ]
physics.end_paths: [ e1 ]
```

This configuration runs `500,000` empty events, generates `50` synthetic digis per event, and sends DQM summaries and waveform updates every `100` events.

### Simple receiver for measuring streamed payload

A lightweight TCP receiver can be used to measure the raw amount of histogram data sent by `HistoSender`:

```bash
python3 receiver.py --host 0.0.0.0 --port 8000 --idle-gap 0.2 --recv-size 65536
```

The receiver does not decode histograms. It only counts bytes, chunks, bursts, elapsed time, and average socket throughput. This makes it useful for payload-size and streaming-rate tests.

### Example result

Test setup:

| Quantity                  |                       Value |
| ------------------------- | --------------------------: |
| Events                    |                   `500,000` |
| Synthetic digis per event |                        `50` |
| Total produced digis      |                `25,000,000` |
| Waveform size             |                `50` samples |
| `freqDQM`                 |                       `100` |
| `freqWaveforms`           |                       `100` |
| Disk-map streaming        |                    Disabled |
| Streaming target          | Localhost, `127.0.0.1:8000` |
| ROOT output               | `random_calo_dqm_test.root` |

End-job DQM summary:

```text
CaloDigiDQM summary:
  events=500000
  d0=12480926
  d1=12482273
  laser=36801
  miss=0
  consecutiveSendErr=0
  totalSendErr=0
  outOfRangeSipmId=0
  skipCounts={BadSipmId:0, UnmappedRawId:0, OutOfRangeSipmId:0,
              RawIdNegative:0, DiskOutOfRange:0, PeakPosOutOfRange:0,
              NonFiniteBaselineOrRms:0, TinyDenomAsym:1}

Waveform-size summary:
  channels_seen=2712
  variable=0
  nbins=64

Laser waveform-size summary:
  channels_seen=4
  variable=0
  nbins=64
```
In this end-job summary, `miss` is the invalid regular mapping count, not the SiPM pair-miss count. Pair-missing behavior is monitored through `h_unpaired_evt`, `h_pair_ok_board`, `h_pair_miss_frac_block`, and the `PairMiss` issue category.

The mapped digi totals are internally consistent:

```text
Disk 0 digis + Disk 1 digis + Laser digis
= 12,480,926 + 12,482,273 + 36,801
= 25,000,000 digis
```

### Timing results

`TimeTracker` output:

| Component                | Average time / event | Median time / event | Max time / event |
| ------------------------ | -------------------: | ------------------: | ---------------: |
| Full event               |      `0.000582084 s` |     `0.000362284 s` |    `0.0712259 s` |
| `RandomCaloDigiProducer` |      `0.000143156 s` |       `0.0001391 s` |    `0.0054007 s` |
| `CaloDigiDQM`            |      `0.000392708 s` |     `0.000179538 s` |    `0.0709097 s` |

Derived approximate rates:

| Metric                                      |             Value |
| ------------------------------------------- | ----------------: |
| Full job throughput from real time          | `~1,614 events/s` |
| Full job throughput from average event time | `~1,718 events/s` |
| DQM-only throughput from average DQM time   | `~2,546 events/s` |
| DQM average processing cost                 |   `~393 us/event` |
| DQM average processing cost per digi        |   `~7.85 us/digi` |
| Producer average processing cost            |   `~143 us/event` |
| Producer average processing cost per digi   |   `~2.86 us/digi` |

Total job summary:

| Quantity  |        Value |
| --------- | -----------: |
| CPU time  |   `305.09 s` |
| Real time |   `309.78 s` |
| VmPeak    | `1161.91 MB` |
| VmHWM     |  `558.58 MB` |

### Streaming payload result

Receiver output:

```text
[receiver] burst=1,
  bytes=39425354492 (36.72 GB),
  chunks=604634,
  time=301.870s,
  rate=124.55 MB/s,
  total=36.72 GB

[receiver] Connection closed.
  Total bytes=39425354492 (36.72 GB),
  chunks=604634,
  bursts=1,
  time=309.85s,
  avg_rate=121.35 MB/s
```

Derived payload estimates:

| Metric                      |                                Value |
| --------------------------- | -----------------------------------: |
| Total streamed payload      | `39,425,354,492 bytes` (`36.72 GiB`) |
| Average payload per event   |                     `~78.9 kB/event` |
| Average payload per digi    |                      `~1.58 kB/digi` |
| Average receiver throughput |                          `~121 MB/s` |
| Burst throughput            |                          `~125 MB/s` |

Because the receiver uses an idle-gap-based burst definition, the full run appeared as one continuous burst. With this configuration, the job produced send activity often enough that the receiver did not observe a long idle gap between sends.

### What this test demonstrates

This test validates several important behaviors:

* The module processed `500,000` events and `25 million` synthetic digis without module failures.
* `CaloDAQMap` decoding worked for all generated digis used by the producer.
* No send errors occurred during localhost streaming.
* No out-of-range SiPM IDs were observed.
* Waveform sizes were stable for all active regular and laser channels.
* Disk 0, Disk 1, and laser counts summed exactly to the expected number of generated digis.
* The DQM module sustained roughly millisecond-subscale event processing with streaming enabled.
* Memory stayed bounded during a long synthetic run.

### Caveats

This is a stress/validation test, not a direct measurement of production online performance.

Important caveats:

* The input is synthetic and generated from `RandomCaloDigiProducer`, not real detector data.
* Streaming was sent to localhost, so network latency and remote receiver behavior were not tested.
* The receiver counted raw socket bytes only; it did not parse or validate histogram objects.
* `freqDQM=100` and `freqWaveforms=100` are aggressive settings and can produce a large payload.
* The max DQM event time is dominated by occasional expensive streaming or large send operations, not typical per-event histogram filling.
* The injected pathology fractions are useful for testing DQM logic but do not represent expected detector rates.

### Additional performance scans

The stress-test result above shows one tested configuration. The following additional scans are recommended for a more complete performance study:

| Scan                                | Purpose                                             |
| ----------------------------------- | --------------------------------------------------- |
| `nDigis = 50, 100, 500, 1000`       | Measure scaling with event occupancy                |
| `waveformSize = 50, 100, 200`       | Measure scaling with waveform length                |
| `sendHists = false` vs `true`       | Separate histogram-filling cost from streaming cost |
| `freqDQM = 100, 500, 1000`          | Measure summary streaming payload sensitivity       |
| `freqWaveforms = 100, 500, 1000, 0` | Measure live waveform streaming cost                |
| `enableDiskMaps = false/true`       | Measure disk-map streaming overhead                 |
| local receiver vs remote receiver   | Measure network and receiver effects                |

A useful minimal benchmark matrix is:

```text
1. sendHists=false, nDigis=50
2. sendHists=true,  freqDQM=1000, freqWaveforms=0
3. sendHists=true,  freqDQM=1000, freqWaveforms=1000
4. sendHists=true,  freqDQM=100,  freqWaveforms=100
5. sendHists=true,  freqDQM=100,  freqWaveforms=100, enableDiskMaps=true
```

This separates pure DQM processing, summary streaming, waveform streaming, aggressive streaming, and disk-map streaming.

---

## Recommended online dashboards

### Shifter dashboard

Use this for fast operational monitoring:

```text
h_dqm_summary
h_dqm_issue_counts
h_dqm_run_counters
h_health_board
h_issue_board
h_pair_ok_board
h_occ_sparse
h_board_vs_channel
```

This dashboard answers:

* Are events and digis present?
* Are major DQM checks passing?
* Which issue type dominates?
* Which boards look unhealthy?
* Are channels or boards missing?

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
Laser/Board_160/B160_Occupancy
Laser/Board_160/B160_Baseline
Laser/Board_160/B160_RMS
Laser/Board_160/B160_Max
h_laser_block
h_amp_laser_block
h_amp_laser_board_channel
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

1. `h_laser_block`
2. `h_amp_laser_block`
3. `h_amp_laser_board_channel`
4. `Laser/Board_160/B160_Max`
5. regular detector amplitude profiles such as `h_amp_dense`

If laser amplitude changes and detector/laser ratio remains stable, the detector response may be stable. If the ratio changes locally, inspect the corresponding board/channel.

---

## Practical validation checklist

After running the module on a test file, check:

1. `h_dqm_run_counters` has nonzero `Events` and expected digi counts.
2. `h_skip_reason` does not show a large unexpected mapping or waveform rejection rate.
3. `h_occ_sparse` and `h_board_vs_channel` show expected active boards and channels.
4. Disk 0 and Disk 1 maps are filled after `endJob()`.
5. Board folders are created only for active boards.
6. Channel folders are created only for active channels.
7. Live waveform histograms contain baseline-subtracted values.
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

This is expected if those boards/channels did not appear in the data. Board and channel histograms are lazily booked.

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
| Add a channel-level distribution | storage vector, `allocateBuffers()`, lazy booking helper, fill site                 |
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

Use the existing lazy-booking pattern:

* Add a storage vector.
* Allocate it in `allocateBuffers()`.
* Add an `ensure...Booked()` helper.
* Book only when the channel is seen.
* Fill only after validating `cidx`.

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
* Live waveform histograms show the latest representative waveform, not every waveform.
* Disk-map running means are accumulated over the job and refreshed before streaming and again at `endJob()`.
* Heuristic thresholds are intended for DQM monitoring and may need adjustment for different run conditions or data-taking modes.

---

## Minimal interpretation checklist

When opening a new output file, check these first:

1. `Global_Histograms/h_dqm_summary` - are the main status checks passing?
2. `Global_Histograms/h_dqm_run_counters` - were events and digis processed, and are send errors/skips low?
3. `Global_Histograms/h_occ_sparse` - are expected boards/channels active?
4. `Global_Histograms/h_issue_board` - are issues localized to specific boards?
5. `Global_Histograms/h_health_board` - which boards need attention?
6. `Global_Histograms/h_skip_reason` - are digis being rejected for mapping or waveform-quality reasons?
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
