import json
import pathlib
import unittest

import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from report_capability_matrix import build_matrix
from report_capability_matrix import render_json
from report_capability_matrix import render_markdown


def descriptor(
    type_id: str,
    maturity: str,
    offline: str,
    realtime: str,
    limitations: list[str],
) -> dict:
    return {
        "typeId": type_id,
        "displayName": type_id,
        "category": "Testing",
        "supportLevel": maturity,
        "supportSummary": "Fixture summary.",
        "capabilities": {
            "offline": offline,
            "offlineBinding": (
                "none" if offline == "unsupported" else "legacyAdapter"
            ),
            "preparedRealtime": realtime,
            "preparedRealtimeBinding": (
                "none" if realtime == "unsupported" else "legacyAdapter"
            ),
            "nestedSounds": "expanded",
            "state": "sessionStateful",
            "supportsReset": False,
            "typedEvents": False,
            "transportAware": False,
            "latency": {
                "kind": "none",
                "fixedFrames": 0,
                "parameterId": "",
                "preparedRule": "none",
            },
            "resourceAccess": "none",
            "fanInLatencyPolicy": "notApplicable",
            "outputBusLayouts": [],
            "limitations": limitations,
        },
        "inputs": [],
        "outputs": [],
        "parameters": [],
    }


class CapabilityMatrixTests(unittest.TestCase):
    def test_builds_sorted_orthogonal_matrix(self):
        catalog = {
            "nodes": [
                descriptor(
                    "talas.zeta",
                    "functionalSubset",
                    "subset",
                    "unsupported",
                    ["offline-functional-subset", "prepared-realtime-unsupported"],
                ),
                descriptor(
                    "talas.alpha",
                    "fullRuntime",
                    "supported",
                    "subset",
                    ["prepared-realtime-subset"],
                ),
            ]
        }

        matrix = build_matrix(catalog)

        self.assertEqual(matrix["schemaVersion"], 2)
        self.assertEqual(
            [node["typeId"] for node in matrix["nodes"]],
            ["talas.alpha", "talas.zeta"],
        )
        self.assertEqual(
            matrix["counts"]["offline"],
            {"unsupported": 0, "subset": 1, "supported": 1},
        )
        self.assertEqual(
            matrix["counts"]["preparedRealtime"],
            {"unsupported": 1, "subset": 1, "supported": 0},
        )
        self.assertEqual(
            matrix["nodes"][0]["limitations"],
            ["prepared-realtime-subset"],
        )
        self.assertEqual(matrix["nodes"][0]["resourceAccess"], "none")
        self.assertEqual(
            matrix["nodes"][0]["fanInLatencyPolicy"], "notApplicable"
        )
        self.assertEqual(matrix["nodes"][0]["outputBusLayouts"], [])

        payload = json.loads(render_json(matrix))
        self.assertEqual(payload, matrix)
        markdown = render_markdown(matrix)
        self.assertIn("# Capability Matrix v2", markdown)
        self.assertIn(
            "| talas.alpha | fullRuntime | supported | legacyAdapter | subset | legacyAdapter |",
            markdown,
        )

    def test_rejects_duplicate_or_unknown_capabilities(self):
        duplicate = descriptor(
            "talas.bad",
            "placeholder",
            "subset",
            "unsupported",
            ["same", "same"],
        )
        with self.assertRaisesRegex(ValueError, "duplicate limitation"):
            build_matrix({"nodes": [duplicate]})

        unknown = descriptor(
            "talas.bad",
            "placeholder",
            "sometimes",
            "unsupported",
            [],
        )
        with self.assertRaisesRegex(ValueError, "offline"):
            build_matrix({"nodes": [unknown]})

        inconsistent = descriptor(
            "talas.bad",
            "fullRuntime",
            "supported",
            "unsupported",
            [],
        )
        inconsistent["capabilities"]["offlineBinding"] = "none"
        with self.assertRaisesRegex(ValueError, "support and binding"):
            build_matrix({"nodes": [inconsistent]})

        unknown_resource = descriptor(
            "talas.bad", "placeholder", "subset", "unsupported", []
        )
        unknown_resource["capabilities"]["resourceAccess"] = "sometimes"
        with self.assertRaisesRegex(ValueError, "resourceAccess"):
            build_matrix({"nodes": [unknown_resource]})

    def test_preserves_prepared_latency_and_m3_contract_axes(self):
        item = descriptor(
            "talas.contract",
            "functionalSubset",
            "subset",
            "unsupported",
            [],
        )
        item["capabilities"]["latency"] = {
            "kind": "preparationTime",
            "fixedFrames": 0,
            "parameterId": "length",
            "preparedRule": "parameterFramesMinusOne",
        }
        item["capabilities"]["resourceAccess"] = "streamRead"
        item["capabilities"]["fanInLatencyPolicy"] = "align"
        item["capabilities"]["outputBusLayouts"] = ["discrete4"]
        item["inputs"] = [
            {
                "id": "frames",
                "type": "complexSpectrum",
                "required": True,
                "rate": "frame",
                "audioLayout": "mono",
                "edge": "continuous",
                "timelineBearing": True,
                "spectralFormat": "complexSpectrumV1",
            }
        ]
        item["outputs"] = [
            {
                "id": "audio",
                "type": "audio",
                "required": False,
                "rate": "sample",
                "audioLayout": "mono",
                "edge": "continuous",
                "timelineBearing": True,
                "spectralFormat": "notApplicable",
            }
        ]

        node = build_matrix({"nodes": [item]})["nodes"][0]

        self.assertEqual(node["latency"], item["capabilities"]["latency"])
        self.assertEqual(node["resourceAccess"], "streamRead")
        self.assertEqual(node["fanInLatencyPolicy"], "align")
        self.assertEqual(node["outputBusLayouts"], ["discrete4"])
        self.assertEqual(node["inputs"], item["inputs"])
        self.assertEqual(node["outputs"], item["outputs"])

        invalid_port = dict(item)
        invalid_port["inputs"] = [dict(item["inputs"][0])]
        invalid_port["inputs"][0]["rate"] = "sometimes"
        with self.assertRaisesRegex(ValueError, "port rate"):
            build_matrix({"nodes": [invalid_port]})

    def test_accepts_sample_rate_derived_prepared_latency_rule(self):
        item = descriptor(
            "talas.liveSpectralAnalysis",
            "functionalSubset",
            "supported",
            "supported",
            [],
        )
        item["capabilities"]["latency"] = {
            "kind": "preparationTime",
            "fixedFrames": 0,
            "parameterId": "lowestAnalyzedFreq",
            "preparedRule": (
                "sampleRateOverParameterNextPowerOfTwoMinusOne"
            ),
        }

        node = build_matrix({"nodes": [item]})["nodes"][0]

        self.assertEqual(
            node["latency"], item["capabilities"]["latency"]
        )

        item["capabilities"]["latency"]["preparedRule"] = (
            "sampleRateOverParameterRoundedMinusOne"
        )
        with self.assertRaisesRegex(ValueError, "preparedRule"):
            build_matrix({"nodes": [item]})

    def test_accepts_cdp_port_reported_latency_rule(self):
        item = descriptor(
            "talas.cdpFastConvolution",
            "functionalSubset",
            "supported",
            "supported",
            [],
        )
        item["capabilities"]["latency"] = {
            "kind": "preparationTime",
            "fixedFrames": 0,
            "parameterId": "impulse",
            "preparedRule": "cdpPortReported",
        }

        node = build_matrix({"nodes": [item]})["nodes"][0]

        self.assertEqual(
            node["latency"], item["capabilities"]["latency"]
        )

    def test_repository_artifacts_are_current(self):
        catalog_path = ROOT / "docs/coverage/builtin-node-catalog-v2.json"
        matrix_json_path = ROOT / "docs/coverage/capability-matrix-v2.json"
        matrix_markdown_path = ROOT / "docs/coverage/capability-matrix-v2.md"

        catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
        matrix = build_matrix(catalog)
        self.assertEqual(
            render_json(matrix),
            matrix_json_path.read_text(encoding="utf-8"),
        )
        self.assertEqual(
            render_markdown(matrix),
            matrix_markdown_path.read_text(encoding="utf-8"),
        )
        self.assertEqual(len(matrix["nodes"]), 364)
        self.assertEqual(
            matrix["counts"]["offline"],
            {"unsupported": 0, "subset": 123, "supported": 241},
        )
        self.assertEqual(
            matrix["counts"]["preparedRealtime"],
            {"unsupported": 0, "subset": 129, "supported": 235},
        )
        averaging = next(
            node for node in matrix["nodes"]
            if node["typeId"] == "talas.averagingLowPassFilter"
        )
        self.assertEqual(averaging["preparedRealtime"], "supported")
        self.assertEqual(averaging["state"], "sessionStateful")
        self.assertTrue(averaging["supportsReset"])
        self.assertEqual(averaging["fanInLatencyPolicy"], "align")
        self.assertEqual(averaging["outputBusLayouts"], ["mono"])
        self.assertEqual(averaging["resourceAccess"], "variableOrUnknown")
        self.assertIn(
            "sample-progressive-partition-stable-prehistory-scaling-and-oldest-sample-fraction",
            averaging["limitations"],
        )
        delay = next(
            node for node in matrix["nodes"]
            if node["typeId"] == "talas.delayWithFeedback"
        )
        self.assertEqual(delay["preparedRealtime"], "supported")
        self.assertEqual(delay["state"], "sessionStateful")
        self.assertTrue(delay["supportsReset"])
        self.assertEqual(delay["latency"]["kind"], "none")
        self.assertEqual(delay["fanInLatencyPolicy"], "align")
        self.assertEqual(delay["outputBusLayouts"], ["mono"])
        self.assertEqual(delay["resourceAccess"], "variableOrUnknown")
        self.assertIn(
            "sample-progressive-partition-stable-zero-delay-history-and-transport-reset",
            delay["limitations"],
        )
        pan = next(
            node for node in matrix["nodes"]
            if node["typeId"] == "talas.pan"
        )
        self.assertEqual(pan["offline"], "supported")
        self.assertEqual(pan["preparedRealtime"], "supported")
        self.assertEqual(pan["preparedRealtimeBinding"], "legacyAdapter")
        self.assertEqual(
            pan["outputBusLayouts"],
            ["stereo", "discrete4", "discrete8"],
        )
        self.assertIn(
            "allocation-free-whole-layout-whole-block-failure-atomic-retry",
            pan["limitations"],
        )
        crossfade = next(
            node for node in matrix["nodes"]
            if node["typeId"] == "talas.crossfade"
        )
        self.assertEqual(crossfade["offline"], "supported")
        self.assertEqual(crossfade["preparedRealtime"], "supported")
        self.assertEqual(
            crossfade["preparedRealtimeBinding"], "legacyAdapter"
        )
        self.assertEqual(crossfade["fanInLatencyPolicy"], "align")
        self.assertEqual(crossfade["outputBusLayouts"], ["stereo"])
        self.assertIn(
            "allocation-free-dual-output-whole-block-failure-atomic-retry",
            crossfade["limitations"],
        )
        stereo_mix2 = next(
            node for node in matrix["nodes"]
            if node["typeId"] == "talas.stereoMix2"
        )
        self.assertEqual(stereo_mix2["offline"], "supported")
        self.assertEqual(stereo_mix2["preparedRealtime"], "supported")
        self.assertEqual(
            stereo_mix2["preparedRealtimeBinding"], "legacyAdapter"
        )
        self.assertEqual(stereo_mix2["fanInLatencyPolicy"], "align")
        self.assertEqual(stereo_mix2["outputBusLayouts"], ["stereo"])
        self.assertIn(
            "allocation-free-dual-output-whole-block-failure-atomic-retry",
            stereo_mix2["limitations"],
        )
        stereo_mix4 = next(
            node for node in matrix["nodes"]
            if node["typeId"] == "talas.stereoMix4"
        )
        self.assertEqual(stereo_mix4["offline"], "supported")
        self.assertEqual(stereo_mix4["preparedRealtime"], "supported")
        self.assertEqual(
            stereo_mix4["preparedRealtimeBinding"], "legacyAdapter"
        )
        self.assertEqual(stereo_mix4["fanInLatencyPolicy"], "align")
        self.assertEqual(stereo_mix4["outputBusLayouts"], ["stereo"])
        self.assertIn(
            "allocation-free-dual-output-whole-block-failure-atomic-retry",
            stereo_mix4["limitations"],
        )
        global_controller = next(
            node for node in matrix["nodes"]
            if node["typeId"] == "talas.soundToGlobalController"
        )
        self.assertEqual(global_controller["preparedRealtime"], "subset")
        self.assertEqual(global_controller["state"], "sessionStateful")
        self.assertTrue(global_controller["supportsReset"])
        self.assertEqual(global_controller["resourceAccess"], "none")
        self.assertIn(
            "capture-only-no-global-controller-mutation",
            global_controller["limitations"],
        )
        self.assertIn(
            "bounded-event-value-or-connected-finite-scalar-values",
            global_controller["limitations"],
        )
        self.assertNotIn(
            "static-or-connected-finite-scalar-values",
            global_controller["limitations"],
        )
        channeller = next(
            node for node in matrix["nodes"]
            if node["typeId"] == "talas.channeller"
        )
        self.assertEqual(channeller["preparedRealtime"], "supported")
        self.assertEqual(channeller["state"], "stateless")
        self.assertEqual(channeller["resourceAccess"], "none")
        self.assertIn(
            "offline-and-prepared-shared-checked-finite-bit-exact-binary64-live-channel-selection",
            channeller["limitations"],
        )
        self.assertNotIn(
            "prepared-realtime-static-or-per-frame-finite-channel-selectors",
            channeller["limitations"],
        )
        self.assertEqual(
            [port["id"] for port in channeller["inputs"]],
            ["audioIn", "rightIn", "leftChannel", "rightChannel"],
        )
        self.assertEqual(
            [port["rate"] for port in channeller["inputs"]],
            ["sample", "sample", "sample", "sample"],
        )
        spectrum_modifier = next(
            node for node in matrix["nodes"]
            if node["typeId"] == "talas.spectrumModifier"
        )
        self.assertIn(
            "prepared-realtime-static-or-block-constant-selection-and-transform-controls",
            spectrum_modifier["limitations"],
        )
        self.assertIn(
            "prepared-realtime-deterministic-live-bounded-seed",
            spectrum_modifier["limitations"],
        )
        self.assertIn(
            "prepared-realtime-deterministic-public-frame-probability-stream",
            spectrum_modifier["limitations"],
        )
        self.assertIn(
            "prepared-realtime-static-one-based-source-ordinal-probability-profile",
            spectrum_modifier["limitations"],
        )
        self.assertIn(
            "prepared-realtime-variables-v2-remainder-track-profiles",
            spectrum_modifier["limitations"],
        )
        self.assertIn(
            "prepared-realtime-bounded-4096-track-live-hysteresis-state",
            spectrum_modifier["limitations"],
        )
        self.assertNotIn(
            "prepared-realtime-deterministic-live-bounded-seed-no-live-hysteresis-state",
            spectrum_modifier["limitations"],
        )
        self.assertEqual(spectrum_modifier["state"], "sessionStateful")
        self.assertTrue(spectrum_modifier["supportsReset"])
        self.assertIn(
            "independent-zero-to-below-one-frequency-and-amplitude-frame-smoothing-by-source-track-ordinal-with-connected-block-constant-controls",
            spectrum_modifier["limitations"],
        )
        self.assertEqual(
            [port["id"] for port in spectrum_modifier["inputs"]],
            [
                "spectrumIn", "select", "loTrack", "hiTrack", "loFreq",
                "hiFreq", "freqHysteresis", "freqScale", "freqOffset",
                "loAmp", "hiAmp", "ampHysteresis", "ampScale",
                "ampOffset", "probability", "seed", "hearAll",
                "freqSmoothing", "ampSmoothing",
            ],
        )
        self.assertEqual(
            next(
                node for node in matrix["nodes"]
                if node["typeId"]
                    == "talas.partialSpectrumLegacyHoldV1"
            )["limitations"],
            [],
        )
        self.assertEqual(
            next(
                node for node in matrix["nodes"]
                if node["typeId"] == "talas.diskPlayer"
            )["resourceAccess"],
            "streamRead",
        )
        self.assertEqual(
            next(
                node for node in matrix["nodes"]
                if node["typeId"] == "talas.diskCache"
            )["resourceAccess"],
            "streamRead",
        )
        self.assertEqual(
            next(
                node for node in matrix["nodes"]
                if node["typeId"] == "talas.timeFrequencyScale"
            )["resourceAccess"],
            # MemoryWriter sources make the analyzed recording mutable.
            "variableOrUnknown",
        )
        diffuse = next(
            node for node in matrix["nodes"]
            if node["typeId"] == "talas.diffuseSpectralStretch"
        )
        self.assertEqual(diffuse["resourceAccess"], "immutableRead")
        self.assertEqual(
            diffuse["limitations"],
            [
                "offline-and-prepared-shared-checked-finite-clean-room-monophonic-diffuse-spectral-stretch",
                "static-direct-event-value-or-per-frame-connected-reset-gate-and-level-with-static-stretch-smear-diffusion-seed-and-immutable-sample",
                "sample-rate-eight-thousand-through-384000-source-one-through-8388608-finite-samples-stretch-two-through-thirty-two-unit-smear-and-diffusion-exact-uint32-seed-and-level-zero-through-four",
                "compiler-owned-exact-descriptor-plan-settings-table-resource-snapshot-parameter-binding-mono-output-buffer-producer-scope-4095-frame-latency-private-state-and-local-transaction-authority",
                "allocation-free-prepared-callback-failure-atomic-spectrum-phase-magnitude-overlap-cursor-reset-gate-tail-retry-and-transport-reset",
                "disk-streaming-live-input-dynamic-spectral-configuration-multichannel-alternate-fft-profiles-transient-formant-pitch-preservation-and-exact-named-algorithm-or-reference-semantics-unsupported",
            ],
        )
        migrated = {
            node["typeId"]: node
            for node in matrix["nodes"]
            if node["state"] == "sessionStateful"
        }
        self.assertEqual(
            set(migrated),
            {
                "talas.analysisFilter",
                "talas.ar",
                "talas.averagingLowPassFilter",
                "talas.valToSound",
                "talas.cdpAttackBoost",
                "talas.cdpBlurAverage",
                "talas.cdpBlurBlur",
                "talas.cdpBlurChorus",
                "talas.cdpBlurNoise",
                "talas.cdpBlurScatter",
                "talas.cdpBlurSpread",
                "talas.cdpBlurSuppress",
                "talas.cdpBounce",
                "talas.cdpBrassage",
                "talas.cdpRotatingPan",
                "talas.cdpSpreadPan",
                "talas.cdpTablePan",
                "talas.cdpStepSpread",
                "talas.cdpCrossPan",
                "talas.cdpAntiphony",
                "talas.cdpMultichannelStadium",
                "talas.cdpFlutter",
                "talas.cdpFrameRotate",
                "talas.cdpFrameRemap",
                "talas.cdpChannelEnvelope",
                "talas.cdpToStereo",
                "talas.cdpStereoSpace",
                "talas.cdpChannelZero",
                "talas.cdpChannelMixdown",
                "talas.cdpMonoToStereo",
                "talas.cdpChannelPhase",
                "talas.cdpMonoToMany",
                "talas.cdpChannelPair",
                "talas.cdpStereoPlacement",
                "talas.cdpPrefix",
                "talas.cdpSilenceMasks",
                "talas.cdpSilenceOverwrite",
                "talas.cdpEnvelopeTimestretch",
                "talas.cdpCascade",
                "talas.cdpClipWaveset",
                "talas.cdpCombineCross",
                "talas.cdpCombineDiff",
                "talas.cdpCombineMax",
                "talas.cdpCombineMean",
                "talas.cdpCombineSum",
                "talas.cdpCrossModulate",
                "talas.cdpDcShift",
                "talas.cdpDeglitch",
                "talas.cdpDistmoreDouble",
                "talas.cdpDistortDivide",
                "talas.cdpDistortEnvelope",
                "talas.cdpDistortFractal",
                "talas.cdpDistortHarmonic",
                "talas.cdpDistortMultiply",
                "talas.cdpDistortOmit",
                "talas.cdpDistortOverload",
                "talas.cdpDistortReform",
                "talas.cdpDistortReverse",
                "talas.cdpDistortShuffle",
                "talas.cdpEcho",
                "talas.cdpEnvelopeImpose",
                "talas.cdpEnvelopeTarget",
                "talas.cdpEnvelopeWarp",
                "talas.cdpFastConvolution",
                "talas.cdpFixedFilter",
                "talas.cdpFocusAccumulate",
                "talas.cdpFocusExaggerate",
                "talas.cdpFocusFocus",
                "talas.cdpFocusFold",
                "talas.cdpFocusFreeze",
                "talas.cdpFocusStep",
                "talas.cdpFormantsPut",
                "talas.cdpFormantsVocode",
                "talas.cdpHiliteArpeg",
                "talas.cdpHiliteBand",
                "talas.cdpHiliteBlurTrace",
                "talas.cdpHiliteFilter",
                "talas.cdpHiliteGraphicEq",
                "talas.cdpHilitePluck",
                "talas.cdpHiliteTrace",
                "talas.cdpHiliteVowels",
                "talas.cdpLohiFilter",
                "talas.cdpLoudnessGain",
                "talas.cdpLowerResolution",
                "talas.cdpMixBalance",
                "talas.cdpMixCrossfade",
                "talas.cdpMixFaders",
                "talas.cdpMixInbetween",
                "talas.cdpMixInterleave",
                "talas.cdpMixMerge",
                "talas.cdpMorphBridge",
                "talas.cdpMorphMorph",
                "talas.cdpNoiseCut",
                "talas.cdpPan",
                "talas.cdpPhaseInvert",
                "talas.cdpPhaseVocoder",
                "talas.cdpPhasing",
                "talas.cdpPhasor",
                "talas.cdpPitchAltharms",
                "talas.cdpPitchChord",
                "talas.cdpPitchChordf",
                "talas.cdpPitchOctmove",
                "talas.cdpPitchPick",
                "talas.cdpPitchTransp",
                "talas.cdpPitchTune",
                "talas.cdpPitchedDelay",
                "talas.cdpQuantise",
                "talas.cdpQuirkWaveset",
                "talas.cdpRejoin",
                "talas.cdpRepitchTranspose",
                "talas.cdpRepitchTransposef",
                "talas.cdpReverb",
                "talas.cdpRingModulate",
                "talas.cdpRoomReverb",
                "talas.cdpSilenceGate",
                "talas.cdpSpecBare",
                "talas.cdpSpecClean",
                "talas.cdpSpectralGain",
                "talas.cdpSpectralGate",
                "talas.cdpSpin",
                "talas.cdpStadiumEcho",
                "talas.cdpStrangeGlis",
                "talas.cdpStrangeInvert",
                "talas.cdpStrangeShift",
                "talas.cdpStrangeWaver",
                "talas.cdpStretchSpectrum",
                "talas.cdpSubtract",
                "talas.cdpSweepingFilter",
                "talas.cdpTapDelay",
                "talas.cdpTremolo",
                "talas.cdpTremoloFine",
                "talas.cdpUserBank",
                "talas.cdpVariableFilter",
                "talas.cdpVaryingBank",
                "talas.cdpVaryingBank2",
                "talas.cdpVaryingDelay",
                "talas.delayWithFeedback",
                "talas.euSeq",
                "talas.frameSynchronizer",
                "talas.modalFilter",
                "talas.noise",
                "talas.oscillator",
                "talas.antialiasedOscillator",
                "talas.oscillatorTimeIndex",
                "talas.splitSurroundOscillator",
                "talas.splitSurroundAntialiasedOscillator",
                "talas.oscillatorBank",
                "talas.formantBank",
                "talas.formantBankOscillator",
                "talas.gaOscillators",
                "talas.sumOfSines",
                "talas.syntheticSpectrumFromArray",
                "talas.genericSource",
                "talas.sample",
                "talas.sampleAndHold",
                "talas.samplesFromDiskSingleStep",
                "talas.pastedSoundExpressionV1",
                "talas.phaseShiftBy90",
                "talas.singleSideBandRm",
                "talas.sosOscillators",
                "talas.multisample",
                "talas.keyMappedMultisample",
                "talas.lfo",
                "talas.ramp",
                "talas.timeIndex",
                "talas.timeIndexFromFileName",
                "talas.timeIndexForSamples",
                "talas.timeIndexForFileNames",
                "talas.multifileDiskPlayer",
                "talas.filter",
                "talas.adsr",
                "talas.functionGenerator",
                "talas.graphicalEnvelope",
                "talas.multislopeFunctionGenerator",
                "talas.multisegmentEnvelope",
                "talas.pulseTrain",
                "talas.pulseGenerator",
                "talas.analogSequencer",
                "talas.audioInput",
                "talas.biDiFollower",
                "talas.binauralPan",
                "talas.hysteresis",
                "talas.lossyIntegrator",
                "talas.clock",
                "talas.clockDivider",
                "talas.chopper",
                "talas.cloudBank",
                "talas.cloudBankElement",
                "talas.warpedTimeIndex",
                "talas.dualMorphOscillator",
                "talas.dynamicRangeController",
                "talas.dynamicBreakpointOscillator",
                "talas.diffuseSpectralStretch",
                "talas.diskCache",
                "talas.diskRecorder",
                "talas.dualParallelTwoPoleFilter",
                "talas.waveTerrainOscillator",
                "talas.scannedMassOscillator",
                "talas.feedbackLoopInput",
                "talas.feedbackLoopOutput",
                "talas.fft",
                "talas.firFilter",
                "talas.filterBank",
                "talas.filterBankElement",
                "talas.frequencyScale",
                "talas.frequencyTracker",
                "talas.forwardFftV1",
                "talas.gate",
                "talas.graphicEq",
                "talas.grainCloud",
                "talas.harmonicResonator",
                "talas.highShelvingFilter",
                "talas.sampleCloud",
                "talas.inverseFftV1",
                "talas.liveSpectralAnalysis",
                "talas.lowPassGate",
                "talas.lowShelvingFilter",
                "talas.memoryWriter",
                "talas.midiMapper",
                "talas.midiFileEcho",
                "talas.midiFileEchoTap",
                "talas.midiOutputController",
                "talas.midiOutputControllerTap",
                "talas.midiOutputEvent",
                "talas.midiOutputEventTap",
                "talas.midiOutputEventInBytes",
                "talas.midiOutputTimingClock",
                "talas.midiOutputStartStopMasterClock",
                "talas.morphFilter",
                "talas.pluckedWaveguide",
                "talas.presenceFilter",
                "talas.probabilityChaosGenerator",
                "talas.quadOscillator",
                "talas.quadFunctionGenerator",
                "talas.reResonator",
                "talas.reverbSection",
                "talas.amplitudeFollower",
                "talas.peakDetector",
                "talas.runningMax",
                "talas.runningMin",
                "talas.rmsSquared",
                "talas.monotonizer",
                "talas.partialSpectrumLegacyHoldV1",
                "talas.partialSpectrumTrackSelectorV1",
                "talas.partialTrackPlayerV1",
                "talas.sampleMapPlayer",
                "talas.scaleVocoder",
                "talas.simplePitchShifter",
                "talas.slew",
                "talas.soundToGlobalController",
                "talas.triggeredSoundToGlobalController",
                "talas.soundCollectionVariable",
                "talas.spectrumModifier",
                "talas.spectrumFundamental",
                "talas.spectrumInRam",
                "talas.spectrumOnDisk",
                "talas.spectrumTrackSelector",
                "talas.spectrumVoicedUnvoiced",
                "talas.stereoDispersiveResonator",
                "talas.threshold",
                "talas.triggeredSampleAndHold",
                "talas.triggeredTableRead",
                "talas.timeControl",
                "talas.setDuration",
                "talas.timeOffset",
                "talas.timeStopper",
                "talas.timeFrequencyScale",
                "talas.waitUntil",
                "talas.twoFormantElement",
                "talas.twoFormantVoiceElement",
                "talas.tunableVocoder",
                "talas.variable",
                "talas.vcf",
                "talas.vocoder",
                "talas.vocoderChannelBank",
            },
        )
        for type_id, node in migrated.items():
            self.assertEqual(
                node["supportsReset"],
                type_id not in {
                    "talas.audioInput",
                    "talas.sampleMapPlayer",
                },
            )
        self.assertEqual(
            migrated["talas.forwardFftV1"]["latency"],
            {
                "kind": "preparationTime",
                "fixedFrames": 0,
                "parameterId": "frameSize",
                "preparedRule": "parameterFramesMinusOne",
            },
        )
        cdp_wavecycle_latency = {
            "talas.cdpClipWaveset",
            "talas.cdpDistmoreDouble",
            "talas.cdpDistortDivide",
            "talas.cdpDistortEnvelope",
            "talas.cdpDistortFractal",
            "talas.cdpDistortHarmonic",
            "talas.cdpDistortMultiply",
            "talas.cdpDistortReform",
            "talas.cdpDistortReverse",
            "talas.cdpDistortShuffle",
            "talas.cdpQuirkWaveset",
        }
        for type_id in cdp_wavecycle_latency:
            self.assertEqual(
                migrated[type_id]["latency"],
                {
                    "kind": "fixed",
                    "fixedFrames": 2047,
                    "parameterId": "",
                    "preparedRule": "none",
                },
            )
        for type_id, node in migrated.items():
            if type_id in cdp_wavecycle_latency:
                continue
            if type_id in {
                "talas.cdpFastConvolution",
                "talas.cdpSilenceGate",
                "talas.cdpDeglitch",
                "talas.cdpNoiseCut",
                "talas.cdpAttackBoost",
                "talas.cdpEnvelopeWarp",
                "talas.cdpEnvelopeImpose",
                "talas.cdpEnvelopeTarget",
                "talas.cdpCascade",
                "talas.cdpPhasor",
                "talas.cdpSpin",
                "talas.cdpBrassage",
                "talas.cdpLowerResolution",
                "talas.cdpPhaseVocoder",
                "talas.cdpSpectralGain",
                "talas.cdpSpectralGate",
                "talas.cdpBlurAverage",
                "talas.cdpBlurBlur",
                "talas.cdpBlurChorus",
                "talas.cdpBlurNoise",
                "talas.cdpBlurScatter",
                "talas.cdpBlurSpread",
                "talas.cdpBlurSuppress",
                "talas.cdpFocusAccumulate",
                "talas.cdpFocusExaggerate",
                "talas.cdpFocusFocus",
                "talas.cdpFocusFold",
                "talas.cdpFocusStep",
                "talas.cdpFormantsPut",
                "talas.cdpFormantsVocode",
                "talas.cdpHiliteArpeg",
                "talas.cdpHiliteBand",
                "talas.cdpHiliteBlurTrace",
                "talas.cdpHiliteFilter",
                "talas.cdpHiliteGraphicEq",
                "talas.cdpHilitePluck",
                "talas.cdpHiliteTrace",
                "talas.cdpFocusStep",
                "talas.cdpFormantsPut",
                "talas.cdpFormantsVocode",
                "talas.cdpHiliteArpeg",
                "talas.cdpHiliteBand",
                "talas.cdpHiliteBlurTrace",
                "talas.cdpHiliteFilter",
                "talas.cdpHiliteGraphicEq",
                "talas.cdpHilitePluck",
                "talas.cdpRepitchTranspose",
                "talas.cdpStrangeShift",
                "talas.cdpStrangeWaver",
                "talas.cdpPitchTransp",
                "talas.cdpBlurChorus",
                "talas.cdpBlurNoise",
                "talas.cdpStretchSpectrum",
                "talas.cdpBlurScatter",
                "talas.cdpHiliteArpeg",
                "talas.cdpHiliteBand",
                "talas.cdpHiliteGraphicEq",
                "talas.cdpBlurSpread",
                "talas.cdpStrangeGlis",
                "talas.cdpCombineCross",
                "talas.cdpCombineDiff",
                "talas.cdpCombineMax",
                "talas.cdpCombineMean",
                "talas.cdpCombineSum",
                "talas.cdpFormantsVocode",
                "talas.cdpMorphBridge",
                "talas.cdpMorphMorph",
                "talas.cdpRepitchTransposef",
                "talas.cdpFormantsPut",
                "talas.cdpHiliteVowels",
                "talas.cdpBlurBlur",
                "talas.cdpHiliteBlurTrace",
                "talas.cdpFocusFocus",
                "talas.cdpPitchChord",
                "talas.cdpPitchChordf",
                "talas.cdpPitchOctmove",
                "talas.cdpPitchPick",
                "talas.cdpPitchTune",
                "talas.cdpFocusFreeze",
                "talas.cdpPitchAltharms",
                "talas.cdpPitchOctmove",
                "talas.cdpSpecBare",
                "talas.cdpSpecClean",
                "talas.cdpStrangeInvert",
                "talas.cdpEnvelopeTimestretch",
                "talas.forwardFftV1",
                "talas.fft",
                "talas.diffuseSpectralStretch",
                "talas.dynamicRangeController",
                "talas.frequencyScale",
                "talas.frequencyTracker",
                "talas.frameSynchronizer",
                "talas.liveSpectralAnalysis",
                "talas.monotonizer",
                "talas.simplePitchShifter",
                "talas.timeFrequencyScale",
            }:
                continue
            self.assertEqual(
                node["latency"],
                {
                    "kind": "none",
                    "fixedFrames": 0,
                    "parameterId": "",
                    "preparedRule": "none",
                },
            )
        for type_id, parameter_id in (
            ("talas.cdpFastConvolution", "impulse"),
            ("talas.cdpSilenceGate", "gate"),
            ("talas.cdpDeglitch", "glitch"),
            ("talas.cdpNoiseCut", "splice"),
            ("talas.cdpAttackBoost", "onset"),
            ("talas.cdpEnvelopeWarp", "window"),
            ("talas.cdpEnvelopeImpose", "window"),
            ("talas.cdpEnvelopeTarget", "window"),
            ("talas.cdpEnvelopeTimestretch", "window"),
            ("talas.cdpCascade", "clipSize"),
            ("talas.cdpPhasor", "frequency"),
            ("talas.cdpSpin", "rate"),
            ("talas.cdpBrassage", "grainsize"),
        ):
            self.assertEqual(
                migrated[type_id]["latency"],
                {
                    "kind": "preparationTime",
                    "fixedFrames": 0,
                    "parameterId": parameter_id,
                    "preparedRule": "cdpPortReported",
                },
            )
        self.assertEqual(
            migrated["talas.cdpLowerResolution"]["latency"],
            {
                "kind": "preparationTime",
                "fixedFrames": 0,
                "parameterId": "sampleRateDivision",
                "preparedRule": "parameterFramesMinusOne",
            },
        )
        for type_id in ("talas.cdpPhaseVocoder", "talas.cdpSpectralGain", "talas.cdpSpectralGate", "talas.cdpBlurAverage", "talas.cdpBlurSuppress", "talas.cdpFocusAccumulate", "talas.cdpFocusExaggerate", "talas.cdpFocusFold", "talas.cdpHiliteTrace", "talas.cdpFocusStep", "talas.cdpHiliteFilter", "talas.cdpHilitePluck", "talas.cdpRepitchTranspose", "talas.cdpStrangeShift", "talas.cdpStrangeWaver", "talas.cdpPitchTransp", "talas.cdpBlurChorus", "talas.cdpBlurNoise", "talas.cdpStretchSpectrum", "talas.cdpBlurScatter", "talas.cdpHiliteArpeg", "talas.cdpHiliteBand", "talas.cdpHiliteGraphicEq", "talas.cdpBlurSpread", "talas.cdpStrangeGlis", "talas.cdpCombineCross", "talas.cdpCombineDiff", "talas.cdpCombineMax", "talas.cdpCombineMean", "talas.cdpCombineSum", "talas.cdpFormantsVocode", "talas.cdpMorphBridge", "talas.cdpMorphMorph", "talas.cdpRepitchTransposef", "talas.cdpFormantsPut", "talas.cdpHiliteVowels", "talas.cdpBlurBlur", "talas.cdpHiliteBlurTrace", "talas.cdpFocusFocus", "talas.cdpPitchChord", "talas.cdpPitchChordf", "talas.cdpPitchPick", "talas.cdpPitchTune", "talas.cdpPitchAltharms", "talas.cdpPitchOctmove", "talas.cdpSpecBare", "talas.cdpSpecClean", "talas.cdpStrangeInvert", "talas.cdpFocusFreeze"):
            self.assertEqual(
                migrated[type_id]["latency"],
                {
                    "kind": "preparationTime",
                    "fixedFrames": 0,
                    "parameterId": "points",
                    "preparedRule": "cdpPhaseVocoderProfile",
                },
            )
        self.assertEqual(
            migrated["talas.dynamicRangeController"]["latency"],
            {
                "kind": "variableOrUnknown",
                "fixedFrames": 0,
                "parameterId": "",
                "preparedRule": "none",
            },
        )
        self.assertEqual(
            migrated["talas.frequencyTracker"]["latency"],
            {
                "kind": "variableOrUnknown",
                "fixedFrames": 0,
                "parameterId": "",
                "preparedRule": "none",
            },
        )
        self.assertEqual(
            migrated["talas.timeControl"]["latency"],
            {
                "kind": "none",
                "fixedFrames": 0,
                "parameterId": "",
                "preparedRule": "none",
            },
        )
        self.assertEqual(
            migrated["talas.timeStopper"]["latency"],
            {
                "kind": "none",
                "fixedFrames": 0,
                "parameterId": "",
                "preparedRule": "none",
            },
        )
        self.assertEqual(
            migrated["talas.timeOffset"]["latency"],
            {
                "kind": "none",
                "fixedFrames": 0,
                "parameterId": "",
                "preparedRule": "none",
            },
        )
        self.assertEqual(
            migrated["talas.waitUntil"]["latency"],
            {
                "kind": "none",
                "fixedFrames": 0,
                "parameterId": "",
                "preparedRule": "none",
            },
        )
        self.assertEqual(
            migrated["talas.fft"]["latency"],
            {
                "kind": "preparationTime",
                "fixedFrames": 0,
                "parameterId": "length",
                "preparedRule": "parameterFramesMinusOne",
            },
        )
        self.assertEqual(
            migrated["talas.diffuseSpectralStretch"]["latency"],
            {
                "kind": "fixed",
                "fixedFrames": 4095,
                "parameterId": "",
                "preparedRule": "none",
            },
        )
        self.assertEqual(
            migrated["talas.liveSpectralAnalysis"]["latency"],
            {
                "kind": "preparationTime",
                "fixedFrames": 0,
                "parameterId": "lowestAnalyzedFreq",
                "preparedRule": (
                    "sampleRateOverParameterNextPowerOfTwoMinusOne"
                ),
            },
        )
        self.assertEqual(
            migrated["talas.frequencyScale"]["latency"],
            {
                "kind": "preparationTime",
                "fixedFrames": 0,
                "parameterId": "frequencyScale",
                "preparedRule": "frequencyScaleProfile",
            },
        )
        self.assertEqual(
            migrated["talas.monotonizer"]["latency"],
            {
                "kind": "preparationTime",
                "fixedFrames": 0,
                "parameterId": "frequency",
                "preparedRule": "monotonizerProfile",
            },
        )
        self.assertEqual(
            migrated["talas.simplePitchShifter"]["latency"],
            {
                "kind": "preparationTime",
                "fixedFrames": 0,
                "parameterId": "interval",
                "preparedRule": "simplePitchShifterInterval",
            },
        )
        self.assertEqual(
            migrated["talas.timeFrequencyScale"]["latency"],
            {
                "kind": "preparationTime",
                "fixedFrames": 0,
                "parameterId": "frequencyScale",
                "preparedRule": "timeFrequencyScaleProfile",
            },
        )
        self.assertEqual(migrated["talas.fft"]["maturity"], "functionalSubset")
        self.assertEqual(migrated["talas.fft"]["offline"], "supported")
        self.assertEqual(migrated["talas.fft"]["preparedRealtime"], "supported")
        self.assertTrue(migrated["talas.fft"]["transportAware"])
        self.assertEqual(migrated["talas.fft"]["limitations"], [])
        self.assertTrue(migrated["talas.adsr"]["typedEvents"])
        self.assertTrue(migrated["talas.midiMapper"]["typedEvents"])
        self.assertFalse(migrated["talas.midiMapper"]["transportAware"])
        self.assertTrue(migrated["talas.soundCollectionVariable"]["typedEvents"])
        self.assertTrue(migrated["talas.variable"]["typedEvents"])
        for type_id, node in migrated.items():
            if type_id not in {
                "talas.adsr",
                "talas.midiMapper",
                "talas.soundCollectionVariable",
                "talas.variable",
            }:
                self.assertFalse(node["typedEvents"])

        native_stft = {
            type_id: migrated[type_id]
            for type_id in {"talas.forwardFftV1", "talas.inverseFftV1"}
        }
        for node in native_stft.values():
            self.assertEqual(node["maturity"], "utility")
            self.assertEqual(node["offline"], "supported")
            self.assertEqual(node["preparedRealtime"], "supported")
            self.assertEqual(node["resourceAccess"], "none")
            self.assertTrue(node["transportAware"])
            self.assertEqual(node["limitations"], [])

        native_utilities = {
            node["typeId"]: node
            for node in matrix["nodes"]
            if node["typeId"]
            in {
                "talas.clock",
                "talas.clockDivider",
                "talas.quantizer",
                "talas.gate",
                "talas.logic",
                "talas.slew",
            }
        }
        self.assertEqual(len(native_utilities), 6)
        for type_id, node in native_utilities.items():
            self.assertEqual(node["preparedRealtime"], "supported")
            self.assertFalse(node["typedEvents"])

        self.assertEqual(
            native_utilities["talas.clockDivider"]["limitations"],
            [
                "offline-and-prepared-shared-checked-finite-positive-uint32-clock-divider",
                "static-or-direct-event-value-positive-integer-division-and-steps-with-static-scalar-or-connected-reset",
                "rising-edge-count-modulo-reduction-held-step-divided-gate-and-reset-precedence",
                "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-input-producer-source-buffer-dual-output-scope-state-reset-zero-latency-and-local-transaction-authority",
                "allocation-free-prepared-callback-failure-atomic-modulus-schedule-state-output-retry-reset-discontinuity-and-zero-frame-no-op",
                "typed-event-clock-reset-dynamic-reset-fallback-fractional-counter-multichannel-and-exact-reference-semantics-unsupported",
            ],
        )
        self.assertEqual(
            native_utilities["talas.clock"]["limitations"],
            [
                "offline-and-prepared-shared-checked-finite-long-double-phase-continuous-clock",
                "static-or-direct-event-value-positive-hertz-frequency-unit-duty-cycle-and-static-enabled",
                "exact-absolute-frame-and-epoch-rebase-phase-less-than-duty-zero-or-one-gate-and-disabled-zero-output",
                "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-output-buffer-scope-state-reset-transport-zero-latency-and-local-transaction-authority",
                "allocation-free-prepared-callback-failure-atomic-phase-schedule-output-retry-reset-discontinuity-and-zero-frame-no-op",
                "external-sync-reset-input-phase-offset-alternate-pulse-shape-multichannel-and-exact-reference-semantics-unsupported",
            ],
        )
        self.assertEqual(
            native_utilities["talas.quantizer"]["limitations"],
            [
                "offline-and-prepared-shared-checked-finite-clean-room-step-quantizer",
                "static-or-direct-event-value-positive-step-size-and-finite-offset",
                "exact-offset-plus-round-value-times-step-size-with-finite-input-and-result",
                "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority",
                "allocation-free-prepared-callback-failure-atomic-schedule-output-retry-and-zero-frame-no-op",
                "alternate-tie-breaking-units-clipping-hysteresis-stochastic-multichannel-and-exact-reference-semantics-unsupported",
            ],
        )
        self.assertEqual(
            native_utilities["talas.gate"]["limitations"],
            [
                "offline-and-prepared-shared-checked-finite-binary64-schmitt-gate",
                "static-or-direct-event-value-finite-ordered-low-and-high-thresholds",
                "exact-closed-start-high-inclusive-open-low-inclusive-close-positive-zero-or-one-output",
                "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-state-reset-zero-latency-and-local-transaction-authority",
                "allocation-free-prepared-callback-failure-atomic-state-schedule-output-retry-reset-and-zero-frame-no-op",
                "debounce-hold-time-edge-events-alternate-polarity-smoothing-multichannel-and-exact-reference-semantics-unsupported",
            ],
        )
        self.assertEqual(
            native_utilities["talas.slew"]["limitations"],
            [
                "offline-and-prepared-shared-checked-finite-binary64-directional-exponential-slew",
                "static-or-direct-event-value-finite-nonnegative-rise-and-fall-seconds",
                "exact-first-sample-initialization-target-greater-or-equal-rise-selection-and-zero-time-direct-tracking",
                "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-state-reset-zero-latency-and-local-transaction-authority",
                "allocation-free-prepared-callback-failure-atomic-state-schedule-output-retry-reset-and-zero-frame-no-op",
                "alternate-curve-rate-initialization-overshoot-multichannel-and-exact-reference-semantics-unsupported",
            ],
        )
        self.assertEqual(
            native_utilities["talas.logic"]["limitations"],
            [
                "offline-and-prepared-shared-checked-finite-clean-room-static-choice-logic",
                "static-and-or-xor-not-with-direct-event-value-or-per-frame-connected-left-and-right",
                "strict-positive-truth-with-exact-zero-or-one-output-and-not-right-operand-ignored",
                "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority",
                "allocation-free-prepared-callback-failure-atomic-schedule-output-retry-and-zero-frame-no-op",
                "dynamic-operation-coercion-alternate-threshold-stateful-edge-logic-and-exact-reference-semantics-unsupported",
            ],
        )

        annotation = next(
            node for node in matrix["nodes"]
            if node["typeId"] == "talas.annotation"
        )
        self.assertEqual(annotation["preparedRealtime"], "supported")
        self.assertEqual(annotation["preparedRealtimeBinding"], "legacyAdapter")
        self.assertEqual(annotation["state"], "stateless")
        self.assertEqual(annotation["latency"]["kind"], "none")
        self.assertEqual(annotation["fanInLatencyPolicy"], "notApplicable")
        self.assertEqual(annotation["outputBusLayouts"], ["mono"])
        self.assertEqual(
            annotation["limitations"],
            [
                "offline-and-prepared-bit-preserving-mono-audio-pass-through",
                "stateless-resource-free-zero-latency-and-no-analysis-capture",
                "compiler-owned-exact-descriptor-plan-settings-resource-free-input-output-producer-scope-latency-and-local-transaction-authority",
                "allocation-free-prepared-callback-failure-atomic-output-retry-and-zero-frame-no-op",
                "annotation-text-rich-format-layout-interaction-multichannel-and-exact-reference-semantics-unsupported",
            ],
        )

        threshold = next(
            node for node in matrix["nodes"]
            if node["typeId"] == "talas.threshold"
        )
        self.assertEqual(threshold["preparedRealtime"], "supported")
        self.assertEqual(threshold["preparedRealtimeBinding"], "legacyAdapter")
        self.assertEqual(threshold["state"], "sessionStateful")
        self.assertTrue(threshold["supportsReset"])
        self.assertEqual(threshold["latency"]["kind"], "none")
        self.assertEqual(threshold["fanInLatencyPolicy"], "notApplicable")
        self.assertEqual(threshold["resourceAccess"], "none")
        self.assertEqual(threshold["outputBusLayouts"], [])
        self.assertEqual(
            threshold["limitations"],
            [
                "offline-and-prepared-matched-checked-finite-absolute-amplitude-schmitt-latch",
                "static-finite-threshold-and-non-negative-hysteresis-or-per-frame-connected-controls-with-connected-precedence",
                "exact-upper-inclusive-lower-strict-state-persistence-and-transport-reset",
                "compiler-owned-exact-descriptor-plan-settings-resource-free-input-output-producer-scope-latency-and-local-transaction-authority",
                "allocation-free-prepared-callback-failure-atomic-state-output-retry-and-reset",
                "dynamic-language-direct-scheduled-fallback-edge-event-smoothing-input-analysis-multichannel-and-exact-reference-semantics-unsupported",
            ],
        )

        stateless = {
            node["typeId"]: node
            for node in matrix["nodes"]
            if node["state"] == "stateless"
        }
        self.assertEqual(
            set(stateless),
            {
                "talas.add",
                "talas.absoluteValue",
                "talas.annotation",
                "talas.diskRecorderTap",
                "talas.prefixer",
                "talas.replicator",
                "talas.stereoizer",
                "talas.surroundifer",
                "talas.arcTan",
                "talas.linearToLog",
                "talas.logToLinear",
                "talas.equality",
                "talas.audioOutput",
                "talas.channelCrosser",
                "talas.channelJoin",
                "talas.complexProduct",
                "talas.clippingDivide",
                "talas.mapEventValues",
                "talas.sampleWithTimeIndex",
                "talas.splitSurroundSampleWithTimeIndex",
                "talas.saturator",
                "talas.multiSampleWithTimeIndex",
                "talas.sharedFileName",
                "talas.sharedFileNames",
                "talas.minimum",
                "talas.maximum",
                "talas.channeller",
                "talas.constant",
                "talas.crossfade",
                "talas.difference",
                "talas.horizontalAmbisonicDecoder",
                "talas.horizontalAmbisonicEncoder",
                "talas.inputOutputCharacteristic",
                "talas.interpolate",
                "talas.iteratedWaveshaper",
                "talas.logic",
                "talas.level",
                "talas.matrix4",
                "talas.matrix8",
                "talas.mixer",
                "talas.mixerV2",
                "talas.multiplyingWaveshaper",
                "talas.overlappingMixer",
                "talas.output4",
                "talas.output6",
                "talas.output8",
                "talas.pan",
                "talas.product",
                "talas.quantizer",
                "talas.scaleAndOffset",
                "talas.setRange",
                "talas.sqrtMagnitude",
                "talas.stereoInOutput4",
                "talas.stereoInOutput8",
                "talas.stereoMix2",
                "talas.stereoMix4",
                "talas.spectrumFromSingleCycle",
                "talas.harmonicSpectrum",
                "talas.uniqueMemoryWriterRecordingNames",
                "talas.cancelUniqueMemoryWriterRecordingNames",
                "talas.syntheticSpectrumFromSounds",
                "talas.vca",
                "talas.waveshaper",
                "talas.spectralShape",
                "talas.spectrumFrequencyScale",
                "talas.spectrumLinearToLog",
                "talas.spectrumLogToLinear",
            },
        )
        self.assertTrue(migrated["talas.clock"]["transportAware"])
        self.assertFalse(stateless["talas.quantizer"]["transportAware"])
        self.assertFalse(stateless["talas.logic"]["transportAware"])
        for node in stateless.values():
            self.assertFalse(node["supportsReset"])
        unclassified = [
            node
            for node in matrix["nodes"]
            if node["typeId"] not in migrated
            and node["typeId"] not in stateless
        ]
        self.assertEqual({node["state"] for node in unclassified}, {"unknown"})
        self.assertEqual(
            {node["latency"]["kind"] for node in unclassified},
            {"variableOrUnknown"},
        )


if __name__ == "__main__":
    unittest.main()
