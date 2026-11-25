#!/usr/bin/env python3
"""Prepare reproducible AnalysisFilter captures and measure distinct bank sums.

This is a diagnostic packet, not a reference observation. Only explicitly
supplied, independently reviewed captures can establish reference behaviour.
"""
from __future__ import annotations

import argparse
import cmath
import copy
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import wave

ROOT = Path(__file__).resolve().parents[1]
SAMPLE_RATE = 48000
FRAMES = 16384
FUNDAMENTAL = 5625
HARMONICS = tuple(range(5))
TONE_BINS = (7, 15, 30, 37, 45, 60, 75, 83, 90, 105, 117, 120)
FREQUENCIES = tuple(SAMPLE_RATE * value / 256 for value in TONE_BINS)


def read_pcm(path: Path) -> tuple[int, list[list[float]]]:
    """Read standard integer PCM without sign-dependent normalization."""
    with wave.open(str(path), "rb") as stream:
        rate, width, count, frames = (stream.getframerate(), stream.getsampwidth(),
                                      stream.getnchannels(), stream.getnframes())
        if (stream.getcomptype() != "NONE" or width not in (1, 2, 3, 4)
                or rate <= 0 or count <= 0 or frames <= 0):
            raise ValueError(f"unsupported or empty integer PCM: {path}")
        payload = stream.readframes(frames)
    if len(payload) != frames * count * width:
        raise ValueError(f"truncated PCM: {path}")
    channels = [[] for _ in range(count)]
    scale = float(1 << (8 * width - 1))
    for offset in range(0, len(payload), width):
        value = int.from_bytes(payload[offset:offset + width], "little", signed=width != 1)
        channels[(offset // width) % count].append((value - 128 if width == 1 else value) / scale)
    return rate, channels


def project_tone(samples, sample_rate, frequency, first_frame=0) -> complex:
    """Return a coherent cosine phasor with the original frame-zero phase."""
    if (not math.isfinite(sample_rate) or sample_rate <= 0
            or not math.isfinite(frequency) or not 0 <= frequency <= sample_rate / 2
            or type(first_frame) is not int or not 0 <= first_frame < len(samples)):
        raise ValueError("invalid tone projection window, rate or frequency")
    frames = len(samples) - first_frame
    cycles = frequency * frames / sample_rate
    if abs(cycles - round(cycles)) > 1e-8:
        raise ValueError("tone projection requires a coherent whole-cycle window")
    if any(not math.isfinite(value) for value in samples[first_frame:]):
        raise ValueError("tone projection contains nonfinite samples")
    real = math.fsum(samples[frame] * math.cos(2 * math.pi * frequency * frame / sample_rate)
                     for frame in range(first_frame, len(samples)))
    imag = -math.fsum(samples[frame] * math.sin(2 * math.pi * frequency * frame / sample_rate)
                      for frame in range(first_frame, len(samples)))
    scale = (1.0 if frequency in (0, sample_rate / 2) else 2.0) / frames
    return complex(real * scale, imag * scale)


def phasor(value: complex) -> dict:
    return {"real": value.real, "imag": value.imag, "magnitude": abs(value),
            "phaseDegrees": math.degrees(cmath.phase(value))}


def measure_bank(input_samples, member_channels, sample_rate, frequencies, first_frame) -> list[dict]:
    if not member_channels or any(len(channels) != 2 for channels in member_channels):
        raise ValueError("a bank needs nonempty stereo members")
    if any(len(channel) != len(input_samples)
           for channels in member_channels for channel in channels):
        raise ValueError("bank member frame counts must match the input")
    rows = []
    for frequency in frequencies:
        source = project_tone(input_samples, sample_rate, frequency, first_frame)
        if abs(source) < 1e-10:
            raise ValueError(f"input has no measurable excitation at {frequency} Hz")
        gains = [(project_tone(channels[0], sample_rate, frequency, first_frame) / source,
                  project_tone(channels[1], sample_rate, frequency, first_frame) / source)
                 for channels in member_channels]
        left = sum(gain[0] for gain in gains)
        right = sum(gain[1] for gain in gains)
        endpoint = frequency in (0, sample_rate / 2)
        rows.append({
            "frequencyHz": frequency,
            "members": [{"left": phasor(a), "right": phasor(b)} for a, b in gains],
            "summedLeft": phasor(left), "summedRight": phasor(right),
            "sumOfLeftMagnitudes": math.fsum(abs(a) for a, _ in gains),
            "sumOfRightMagnitudes": math.fsum(abs(b) for _, b in gains),
            "analyticPositive": None if endpoint else phasor((left + 1j * right) / 2),
            "analyticNegative": None if endpoint else phasor((left - 1j * right) / 2),
        })
    return rows


def fingerprint(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def write_stimulus(path: Path, samples) -> None:
    pcm = bytearray()
    for sample in samples:
        value = round(sample * 32768)
        if not -32768 <= value <= 32767:
            raise ValueError("stimulus would clip")
        pcm.extend(value.to_bytes(2, "little", signed=True))
    with wave.open(str(path), "wb") as output:
        output.setparams((1, 2, SAMPLE_RATE, len(samples), "NONE", "not compressed"))
        output.writeframes(pcm)


def project_for_harmonic(template, harmonic):
    project = copy.deepcopy(template)
    project["metadata"]["title"] = f"AnalysisFilter bank capture — harmonic {harmonic}"
    project["timeline"]["eventValues"] = []
    project["presentation"]["structureLayouts"] = [{
        "scope": {"kind": "project"}, "nodes": [
            {"nodeId": "input", "x": 100, "y": 180},
            {"nodeId": "analysis", "x": 400, "y": 180},
            {"nodeId": "out", "x": 700, "y": 180}]}]
    project["graph"] = {"nodes": [
        {"id": "input", "typeId": "talas.audioInput", "parameters": [{"id": "channel1", "boolean": True}]},
        {"id": "analysis", "typeId": "talas.analysisFilter", "parameters": [
            {"id": "fundamental", "expression": f"{FUNDAMENTAL} hz"},
            {"id": "harmonic", "expression": str(harmonic)}]},
        {"id": "out", "typeId": "talas.audioOutput", "parameters": []}], "connections": [
            {"fromNode": "input", "fromPort": "audioOut", "toNode": "analysis", "toPort": "audioIn"},
            {"fromNode": "analysis", "fromPort": "leftOut", "toNode": "out", "toPort": "audioIn"},
            {"fromNode": "analysis", "fromPort": "rightOut", "toNode": "out", "toPort": "rightIn"}]}
    if harmonic == "bank":
        graph = project["graph"]
        member_template = graph["nodes"].pop(1)
        graph["connections"] = []
        for index in HARMONICS:
            member = copy.deepcopy(member_template)
            member["id"] = f"analysis{index}"
            member["parameters"][1]["expression"] = str(index)
            graph["nodes"].append(member)
            graph["connections"].append({"fromNode": "input", "fromPort": "audioOut",
                                         "toNode": member["id"], "toPort": "audioIn"})
            if index:
                for lane in ("left", "right"):
                    mix = f"{lane}{index}"
                    graph["nodes"].append({"id": mix, "typeId": "talas.mixer", "parameters": [
                        {"id": "gain1", "expression": "1"}, {"id": "gain2", "expression": "1"}]})
                    graph["connections"].extend([
                        {"fromNode": "analysis0" if index == 1 else f"{lane}{index-1}",
                         "fromPort": f"{lane}Out" if index == 1 else "audioOut", "toNode": mix, "toPort": "in1"},
                        {"fromNode": member["id"], "fromPort": f"{lane}Out", "toNode": mix, "toPort": "in2"}])
        for lane, port in (("left", "audioIn"), ("right", "rightIn")):
            graph["connections"].append({"fromNode": f"{lane}4", "fromPort": "audioOut", "toNode": "out", "toPort": port})
        project["presentation"]["structureLayouts"][0]["nodes"] = [
            {"nodeId": node["id"], "x": 100 + (index % 5) * 240, "y": 100 + (index // 5) * 180}
            for index, node in enumerate(graph["nodes"])]
    return project


def prepare(directory: Path, runner: Path) -> dict:
    if not runner.is_file():
        raise ValueError(f"build talas_analysis_filter_reference_tests first: {runner}")
    directory.mkdir(parents=True, exist_ok=False)
    for child in ("stimuli", "projects", "candidate-offline", "candidate-realtime"):
        (directory / child).mkdir()
    template = json.loads((ROOT / "examples/c159_analysis_filter_scheduled_controls.talas.json").read_text())
    for harmonic in HARMONICS:
        write_json(directory / "projects" / f"h{harmonic}.talas.json", project_for_harmonic(template, harmonic))
    write_json(directory / "projects" / "bank.talas.json", project_for_harmonic(template, "bank"))
    cases = [
        {"id": "fundamental", "frequenciesHz": [FUNDAMENTAL], "firstMeasuredFrame": FRAMES // 2},
        {"id": "multitone", "frequenciesHz": FREQUENCIES, "firstMeasuredFrame": FRAMES // 2},
        {"id": "dc", "frequenciesHz": [0], "firstMeasuredFrame": FRAMES // 2},
        {"id": "nyquist", "frequenciesHz": [SAMPLE_RATE / 2], "firstMeasuredFrame": FRAMES // 2},
        {"id": "impulse", "frequenciesHz": [0, *FREQUENCIES, SAMPLE_RATE / 2], "firstMeasuredFrame": 0},
    ]
    for case in cases:
        case_id = case["id"]
        if case_id == "fundamental":
            samples = [0.125 * math.cos(2 * math.pi * FUNDAMENTAL * frame / SAMPLE_RATE)
                       for frame in range(FRAMES)]
        elif case_id == "multitone":
            samples = [sum(math.cos(2 * math.pi * frequency * frame / SAMPLE_RATE)
                           for frequency in FREQUENCIES) / 64 for frame in range(FRAMES)]
        elif case_id == "dc":
            samples = [0.125] * FRAMES
        elif case_id == "nyquist":
            samples = [0.125 if frame % 2 == 0 else -0.125 for frame in range(FRAMES)]
        else:
            samples = [0.125] + [0.0] * (FRAMES - 1)
        write_stimulus(directory / "stimuli" / f"{case_id}.wav", samples)
    source_paths = ["src/runtime/Runtime.cpp", "src/runtime/RealtimeRenderSession.cpp",
                    "src/runtime/CheckedBiquadMath.h", "src/runtime/CheckedAnalysisFilterMath.h",
                    "tests/runtime/AnalysisFilterReferenceTests.cpp", "tools/analysis_filter_bank_probe.py"]
    manifest = {
        "schemaVersion": 1, "purpose": "unreviewed AnalysisFilter bank diagnostic",
        "requirementId": "AnalysisFilter.unitAmplitudeBank", "requirementRevision": 1,
        "sampleRate": SAMPLE_RATE, "frames": FRAMES, "fundamentalHz": FUNDAMENTAL,
        "harmonics": HARMONICS, "directBankIncluded": True,
        "channelOrder": ["cosine/left", "sine/right"], "cases": cases,
        "referenceObserved": False, "candidateRenderStatus": "pending",
        "candidateRunnerSha256": fingerprint(runner),
        "sourceSha256": {path: fingerprint(ROOT / path) for path in source_paths},
        "quantization": "PCM16 exports; standard signed PCM analysis. Native WAV decoding and float realtime input conversions remain part of candidate behavior.",
        "actions": ["Reset filter histories before each stimulus/member.",
                    "Render each full stimulus at unity level and the exact sample rate; disable normalization, limiting, resampling and fades.",
                    "Retain stereo channel order and frame-zero alignment; record any unavoidable device latency without silently shifting samples."],
    }
    manifest_path = directory / "manifest.json"
    write_json(manifest_path, manifest)
    commands = []
    with (directory / "candidate-render.log").open("w", encoding="utf-8") as log:
        for case in cases:
            for harmonic in (*HARMONICS, "bank"):
                stem = f"{case['id']}-" + ("bank" if harmonic == "bank" else f"h{harmonic}") + ".wav"
                command = [str(runner), "--render-bank-member", str(directory / "stimuli" / f"{case['id']}.wav"),
                           str(FUNDAMENTAL), "bank0-4" if harmonic == "bank" else str(harmonic),
                           str(directory / "candidate-offline" / stem),
                           str(directory / "candidate-realtime" / stem)]
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                commands.append(command)
                log.write(json.dumps(command) + "\n" + result.stdout + result.stderr)
                log.flush()
                if result.returncode != 0:
                    manifest["candidateRenderStatus"] = "failed"
                    write_json(manifest_path, manifest)
                    raise ValueError(f"candidate render failed for {stem}; see candidate-render.log")
    manifest["candidateRenderStatus"] = "complete"
    manifest["commands"] = commands
    files = [path for child in ("stimuli", "projects", "candidate-offline", "candidate-realtime")
             for path in (directory / child).iterdir()]
    manifest["fileSha256"] = {str(path.relative_to(directory)): fingerprint(path) for path in sorted(files)}
    write_json(manifest_path, manifest)
    return manifest


def analyze(directory: Path, captures: Path | None = None) -> dict:
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest["schemaVersion"] != 1 or manifest["candidateRenderStatus"] != "complete":
        raise ValueError("packet is unsupported or its candidate render did not complete")
    for relative, expected in manifest["fileSha256"].items():
        path = (directory / relative).resolve()
        if not path.is_relative_to(directory.resolve()) or fingerprint(path) != expected:
            raise ValueError(f"packet fingerprint differs: {relative}")
    report = {"schemaVersion": 1, "requirementId": manifest["requirementId"],
              "referenceObserved": False,
              "status": "diagnostic only; external PCM is unreviewed and is not reference evidence",
              "sampleRate": manifest["sampleRate"], "fundamentalHz": manifest["fundamentalHz"],
              "harmonics": manifest["harmonics"], "cases": []}
    locations = {"candidateOffline": directory / "candidate-offline",
                 "candidateRealtime": directory / "candidate-realtime"}
    if captures is not None:
        locations["unreviewedExternalCapture"] = captures
    for case in manifest["cases"]:
        rate, source = read_pcm(directory / "stimuli" / f"{case['id']}.wav")
        if rate != manifest["sampleRate"] or len(source) != 1 or len(source[0]) != manifest["frames"]:
            raise ValueError("stimulus dimensions differ from manifest")
        entry = {"id": case["id"], "firstMeasuredFrame": case["firstMeasuredFrame"], "measurements": {}}
        for label, location in locations.items():
            members = []
            for harmonic in manifest["harmonics"]:
                path = location / f"{case['id']}-h{harmonic}.wav"
                member_rate, channels = read_pcm(path)
                if member_rate != rate:
                    raise ValueError(f"capture sample rate differs: {path}")
                members.append(channels)
            entry["measurements"][label] = measure_bank(
                source[0], members, rate, case["frequenciesHz"], case["firstMeasuredFrame"])
            direct_path = location / f"{case['id']}-bank.wav"
            if direct_path.exists() or (label != "unreviewedExternalCapture" and manifest.get("directBankIncluded")):
                direct_rate, direct_channels = read_pcm(direct_path)
                if direct_rate != rate:
                    raise ValueError(f"direct bank sample rate differs: {direct_path}")
                rows = measure_bank(source[0], [direct_channels], rate,
                                    case["frequenciesHz"], case["firstMeasuredFrame"])
                difference = max(abs(direct_channels[lane][frame]
                                     - sum(member[lane][frame] for member in members))
                                 for lane in range(2) for frame in range(len(source[0])))
                entry.setdefault("directBank", {})[label] = {
                    "measurements": rows, "maximumSampleDifferenceFromSummedMembers": difference}
            else:
                entry.setdefault("directBank", {})[label] = None
        report["cases"].append(entry)
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_parser = commands.add_parser("prepare", help="create a new packet and both candidate renders")
    prepare_parser.add_argument("directory", type=Path)
    prepare_parser.add_argument("--runner", type=Path,
                                default=ROOT / "build/workbench-debug/talas_analysis_filter_reference_tests")
    analyze_parser = commands.add_parser("analyze", help="measure candidate and optional external PCM")
    analyze_parser.add_argument("directory", type=Path)
    analyze_parser.add_argument("--captures", type=Path)
    analyze_parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        directory = args.directory.resolve()
        if args.command == "prepare":
            prepare(directory, args.runner.resolve())
            print("Prepared 5 stimuli, 5 members plus direct bank and 60 candidate WAVs; no reference recording supplied.")
        else:
            report = analyze(directory, args.captures)
            output = args.output or directory / "measurement-report.json"
            write_json(output, report)
            print(f"Measured distinct complex and magnitude sums: {output}; reference remains unreviewed.")
        return 0
    except (OSError, ValueError, KeyError, TypeError, wave.Error, subprocess.TimeoutExpired) as error:
        print(f"AnalysisFilter probe: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
