import hashlib
import json
import math
import pathlib
import shutil
import struct
import sys
import tempfile
import unittest
import wave

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from analysis_filter_bank_probe import analyze
from analysis_filter_bank_probe import measure_bank
from analysis_filter_bank_probe import project_tone
from analysis_filter_bank_probe import read_pcm


def tone(sample_rate, frames, frequency, amplitude=1.0, phase=0.0,
         first_frame=0):
    return [
        amplitude * math.cos(
            2.0 * math.pi * frequency * (first_frame + frame) / sample_rate
            + phase)
        for frame in range(frames)
    ]


def decode_phasor(value):
    return complex(value["real"], value["imag"])


def pack_signed_24(value):
    return int(value).to_bytes(3, "little", signed=True)


def write_pcm16_wave(path, sample_rate, channels):
    if not channels:
        raise ValueError("test PCM needs at least one channel")
    frame_count = len(channels[0])
    if frame_count == 0 or any(len(channel) != frame_count for channel in channels):
        raise ValueError("test PCM channels must have matching nonempty frames")
    interleaved = [channel[frame] for frame in range(frame_count)
                   for channel in channels]
    payload = struct.pack("<" + "h" * len(interleaved), *interleaved)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(len(channels))
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(payload)


def build_minimal_packet(packet, external_captures=None):
    sample_rate = 8
    frames = 8
    tone_counts = [round(sample * 32768) for sample in tone(
        sample_rate, frames, 1.0, amplitude=0.5)]
    directories = ["stimuli", "candidate-offline", "candidate-realtime"]
    for name in directories:
        (packet / name).mkdir(parents=True)

    stimulus = packet / "stimuli" / "tone.wav"
    write_pcm16_wave(stimulus, sample_rate, [tone_counts])
    for candidate in ("candidate-offline", "candidate-realtime"):
        member = packet / candidate / "tone-h0.wav"
        direct = packet / candidate / "tone-bank.wav"
        write_pcm16_wave(member, sample_rate, [tone_counts, [0] * frames])
        shutil.copyfile(member, direct)

    if external_captures is not None:
        external_captures.mkdir(parents=True)
        shutil.copyfile(packet / "candidate-offline" / "tone-h0.wav",
                        external_captures / "tone-h0.wav")
        shutil.copyfile(packet / "candidate-offline" / "tone-bank.wav",
                        external_captures / "tone-bank.wav")

    files = [stimulus]
    files.extend(
        path
        for candidate in ("candidate-offline", "candidate-realtime")
        for path in (packet / candidate / "tone-h0.wav",
                     packet / candidate / "tone-bank.wav"))
    manifest = {
        "schemaVersion": 1,
        "purpose": "synthetic packet analyzer test; not a reference observation",
        "requirementId": "AnalysisFilter.unitAmplitudeBank",
        "requirementRevision": 1,
        "sampleRate": sample_rate,
        "frames": frames,
        "fundamentalHz": 1,
        "harmonics": [0],
        "directBankIncluded": True,
        "channelOrder": ["cosine/left", "sine/right"],
        "cases": [{
            "id": "tone",
            "frequenciesHz": [1.0],
            "firstMeasuredFrame": 0,
        }],
        "referenceObserved": False,
        "candidateRenderStatus": "complete",
        "fileSha256": {
            path.relative_to(packet).as_posix(): hashlib.sha256(
                path.read_bytes()).hexdigest()
            for path in files
        },
    }
    (packet / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


class ToneProjectionTests(unittest.TestCase):
    def test_coherent_cosine_and_sine_keep_phase_sign(self):
        sample_rate = 16
        frames = 16
        frequency = 2
        amplitude = 0.75
        first_frame = 8

        cosine = tone(
            sample_rate, frames, frequency, amplitude,
            first_frame=0)
        sine = tone(
            sample_rate, frames, frequency, amplitude,
            phase=math.pi / 2.0, first_frame=0)
        negative_sine = tone(
            sample_rate, frames, frequency, amplitude,
            phase=-math.pi / 2.0, first_frame=0)

        self.assertAlmostEqual(
            project_tone(cosine, sample_rate, frequency).real,
            amplitude, places=12)
        self.assertAlmostEqual(
            project_tone(cosine, sample_rate, frequency).imag,
            0.0, places=12)
        self.assertAlmostEqual(
            project_tone(sine, sample_rate, frequency).real,
            0.0, places=12)
        self.assertAlmostEqual(
            project_tone(sine, sample_rate, frequency).imag,
            amplitude, places=12)
        self.assertAlmostEqual(
            project_tone(negative_sine, sample_rate, frequency).imag,
            -amplitude, places=12)

        offset_window = tone(
            sample_rate, frames, frequency, amplitude,
            phase=0.37)
        offset_phasor = project_tone(
            offset_window, sample_rate, frequency, first_frame)
        self.assertAlmostEqual(offset_phasor.real,
                               amplitude * math.cos(0.37), places=12)
        self.assertAlmostEqual(offset_phasor.imag,
                               amplitude * math.sin(0.37), places=12)

    def test_dc_and_nyquist_use_endpoint_scaling(self):
        sample_rate = 16
        frames = 16
        amplitude = 0.375
        dc = [amplitude] * frames
        nyquist = [amplitude if frame % 2 == 0 else -amplitude
                   for frame in range(frames)]

        dc_phasor = project_tone(dc, sample_rate, 0.0)
        nyquist_phasor = project_tone(
            nyquist, sample_rate, sample_rate / 2.0)
        self.assertAlmostEqual(dc_phasor.real, amplitude, places=12)
        self.assertAlmostEqual(dc_phasor.imag, 0.0, places=12)
        self.assertAlmostEqual(nyquist_phasor.real, amplitude, places=12)
        self.assertAlmostEqual(nyquist_phasor.imag, 0.0, places=12)

        dc_rows = measure_bank(
            dc, [[dc, [-sample for sample in dc]]],
            sample_rate, [0.0], 0)
        nyquist_rows = measure_bank(
            nyquist,
            [[nyquist, [-sample for sample in nyquist]]],
            sample_rate, [sample_rate / 2.0], 0)
        for row in (dc_rows[0], nyquist_rows[0]):
            self.assertIsNone(row["analyticPositive"])
            self.assertIsNone(row["analyticNegative"])

    def test_noncoherent_empty_and_zero_excitation_inputs_are_rejected(self):
        sample_rate = 8
        samples = tone(sample_rate, 8, 1.0)

        with self.assertRaises(ValueError):
            project_tone([], sample_rate, 1.0)
        with self.assertRaises(ValueError):
            project_tone(samples, sample_rate, 1.5)
        with self.assertRaises(ValueError):
            measure_bank([], [[[], []]], sample_rate, [1.0], 0)
        with self.assertRaises(ValueError):
            measure_bank(samples, [], sample_rate, [1.0], 0)
        with self.assertRaises(ValueError):
            measure_bank([0.0] * 8, [[[0.0] * 8, [0.0] * 8]],
                         sample_rate, [1.0], 0)


class BankMeasurementTests(unittest.TestCase):
    def test_cancelling_member_phasors_remain_distinct_from_magnitude_sum(self):
        sample_rate = 8
        frequency = 1.0
        frames = 8
        source = tone(sample_rate, frames, frequency, amplitude=0.5)
        left_a = tone(sample_rate, frames, frequency, amplitude=0.25)
        left_b = [-sample for sample in left_a]
        right_a = tone(sample_rate, frames, frequency, amplitude=0.2)
        right_b = [-sample for sample in right_a]

        row = measure_bank(
            source,
            [[left_a, right_a], [left_b, right_b]],
            sample_rate, [frequency], 0)[0]

        self.assertAlmostEqual(
            decode_phasor(row["members"][0]["left"]).real,
            0.5, places=12)
        self.assertAlmostEqual(
            decode_phasor(row["members"][1]["left"]).real,
            -0.5, places=12)
        self.assertAlmostEqual(
            row["summedLeft"]["magnitude"], 0.0, places=12)
        self.assertAlmostEqual(row["sumOfLeftMagnitudes"], 1.0, places=12)
        self.assertAlmostEqual(
            row["summedRight"]["magnitude"], 0.0, places=12)
        self.assertAlmostEqual(row["sumOfRightMagnitudes"], 0.8, places=12)

    def test_quadrature_sign_selects_the_positive_or_negative_analytic_branch(self):
        sample_rate = 8
        frequency = 1.0
        frames = 8
        phase = 0.23
        source = tone(sample_rate, frames, frequency, phase=phase)
        left = tone(sample_rate, frames, frequency, phase=phase)
        positive_quadrature = tone(
            sample_rate, frames, frequency, phase=phase - math.pi / 2.0)
        negative_quadrature = tone(
            sample_rate, frames, frequency, phase=phase + math.pi / 2.0)

        positive_row = measure_bank(
            source, [[left, positive_quadrature]],
            sample_rate, [frequency], 0)[0]
        negative_row = measure_bank(
            source, [[left, negative_quadrature]],
            sample_rate, [frequency], 0)[0]

        self.assertAlmostEqual(
            decode_phasor(positive_row["members"][0]["right"]).imag,
            -1.0, places=12)
        self.assertAlmostEqual(
            positive_row["analyticPositive"]["magnitude"],
            1.0, places=12)
        self.assertAlmostEqual(
            positive_row["analyticNegative"]["magnitude"],
            0.0, places=12)
        self.assertAlmostEqual(
            negative_row["analyticPositive"]["magnitude"],
            0.0, places=12)
        self.assertAlmostEqual(
            negative_row["analyticNegative"]["magnitude"],
            1.0, places=12)

    def test_missing_or_short_stereo_member_channels_are_rejected(self):
        sample_rate = 8
        frequency = 1.0
        source = tone(sample_rate, 8, frequency)
        left = tone(sample_rate, 8, frequency)
        right = tone(sample_rate, 8, frequency, phase=-math.pi / 2.0)

        with self.assertRaises(ValueError):
            measure_bank(source, [[left]], sample_rate, [frequency], 0)
        with self.assertRaises(ValueError):
            measure_bank(source, [[left, right[:-1]]],
                         sample_rate, [frequency], 0)


class PcmReadingTests(unittest.TestCase):
    def write_pcm_wave(self, path, width, interleaved_samples):
        if width == 2:
            payload = struct.pack(
                "<" + "h" * len(interleaved_samples),
                *interleaved_samples)
        elif width == 3:
            payload = b"".join(
                pack_signed_24(sample)
                for sample in interleaved_samples)
        else:
            raise AssertionError("test helper supports PCM16 and PCM24")

        with wave.open(str(path), "wb") as output:
            output.setnchannels(2)
            output.setsampwidth(width)
            output.setframerate(48000)
            output.writeframes(payload)

    def test_signed_pcm16_and_pcm24_decode_full_scale_and_channel_order(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            pcm16_path = root / "signed16.wav"
            pcm16_values = [
                -32768, 32767,
                -1, 1,
                0, 0,
                1, -1,
                32767, -32768,
            ]
            self.write_pcm_wave(pcm16_path, 2, pcm16_values)
            rate16, channels16 = read_pcm(pcm16_path)
            self.assertEqual(rate16, 48000)
            self.assertEqual(len(channels16), 2)
            self.assertEqual(len(channels16[0]), 5)
            expected16 = 32767 / 32768
            self.assertEqual(channels16[0], [-1.0, -1 / 32768, 0.0,
                                             1 / 32768, expected16])
            self.assertEqual(channels16[1], [expected16, 1 / 32768, 0.0,
                                             -1 / 32768, -1.0])

            pcm24_path = root / "signed24.wav"
            pcm24_values = [
                -8388608, 8388607,
                -1, 1,
                0, 0,
                1, -1,
                8388607, -8388608,
            ]
            self.write_pcm_wave(pcm24_path, 3, pcm24_values)
            rate24, channels24 = read_pcm(pcm24_path)
            self.assertEqual(rate24, 48000)
            self.assertEqual(len(channels24), 2)
            self.assertEqual(len(channels24[0]), 5)
            expected24 = 8388607 / 8388608
            self.assertEqual(channels24[0], [-1.0, -1 / 8388608, 0.0,
                                             1 / 8388608, expected24])
            self.assertEqual(channels24[1], [expected24, 1 / 8388608, 0.0,
                                             -1 / 8388608, -1.0])

    def test_truncated_pcm_frame_data_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "truncated.wav"
            self.write_pcm_wave(path, 2, [-32768, 32767, 0, 0])
            path.write_bytes(path.read_bytes()[:-1])

            with self.assertRaises(ValueError):
                read_pcm(path)


class PacketAnalysisTests(unittest.TestCase):
    def test_analyze_reports_synthetic_direct_bank_and_unreviewed_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            packet = root / "packet"
            external = root / "external-captures"
            build_minimal_packet(packet, external)

            report = analyze(packet, external)

        self.assertFalse(report["referenceObserved"])
        self.assertIn("external PCM is unreviewed", report["status"])
        case = report["cases"][0]
        self.assertEqual(case["id"], "tone")
        self.assertEqual(case["measurements"]["candidateOffline"][0]["frequencyHz"],
                         1.0)
        self.assertIn("unreviewedExternalCapture", case["measurements"])
        self.assertEqual(
            case["measurements"]["unreviewedExternalCapture"],
            case["measurements"]["candidateOffline"])

        for label in ("candidateOffline", "candidateRealtime",
                      "unreviewedExternalCapture"):
            direct_bank = case["directBank"][label]
            self.assertIsNotNone(direct_bank)
            self.assertEqual(
                direct_bank["maximumSampleDifferenceFromSummedMembers"],
                0.0)
            self.assertEqual(
                direct_bank["measurements"], case["measurements"][label])
            self.assertAlmostEqual(
                direct_bank["measurements"][0]["summedLeft"]["real"],
                1.0, places=12)
            self.assertAlmostEqual(
                direct_bank["measurements"][0]["summedRight"]["magnitude"],
                0.0, places=12)

    def test_analyze_rejects_a_changed_fingerprinted_candidate_wave(self):
        with tempfile.TemporaryDirectory() as directory:
            packet = pathlib.Path(directory) / "packet"
            build_minimal_packet(packet)
            candidate = packet / "candidate-offline" / "tone-h0.wav"
            altered = bytearray(candidate.read_bytes())
            altered[-1] ^= 1
            candidate.write_bytes(altered)

            with self.assertRaisesRegex(
                    ValueError,
                    "packet fingerprint differs: candidate-offline/tone-h0.wav"):
                analyze(packet)


if __name__ == "__main__":
    unittest.main()
