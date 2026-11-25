# E7 local accessibility acceptance record

This record documents the local E7 accessibility pass. It is a bounded product
verification record, not a WCAG conformance statement, VPAT, certification, or
claim that every assistive-technology configuration has been tested.

## Accepted macOS environment

- Date: 2026-08-13
- Product commit: `f0dc41176b38e9272f5b575180547704f2ddb587`
- Product: tests-off Release `Talas Workbench`
- OS: macOS 26.6.1 (25G76), Apple silicon (`arm64`)
- VoiceOver: enabled for the pass through System Settings, then restored to its
  original disabled state
- Project: `Realtime Oscillator Demonstration`
- Network/remotes: not used

## Manual macOS pass

| Area | Action and expected evidence | Result |
| --- | --- | --- |
| VoiceOver root | Read the Workbench window and primary toolbar. Play, Pause, Stop, Undo, Redo, Delete Node, Save, More, and all five workspace actions must expose truthful names, disabled state where applicable, help, and shortcuts. | PASS |
| Prototype library | Read search, filters, result count, grouped prototype outline, maturity/origin, execution support, resource limits, and reusable-Sound controls. Unsupported/subset limits must remain explicit text rather than colour-only evidence. | PASS |
| Graph tree | Read the `ROOT Sound graph` outline. Audio Output, VCA, Oscillator, and Constant nodes plus every input/output port were exposed in stable node/port order with direction, type, requirement, rate/layout, connection state, and keyboard action. | PASS |
| Runtime/status surfaces | Read Inspector, Event Values, Diagnostics, persistent project/transport state, audio-device status, Timeline position, and output meter descriptions. Current values and warning text remained queryable. | PASS |
| Assistive action | Invoke `Samples workspace` through its accessibility action. The tree changed to the sample-map, waveform/loop, region inspector, asset, and status controls; invoking `Patch workspace` restored the graph/library tree. | PASS |
| Keyboard workspace navigation | Press `Shift+Command+5` and verify the Samples accessibility tree, then press `Shift+Command+1` and verify return to Patch. | PASS |
| Scale and enhanced contrast | Inspect the generated 150% Patch, Performance, Samples, Analysis, default, and Context Help captures. Toolbar actions remained contained, Context Help preserved usable graph height, Virtual Controls remained scrollable, Performance items did not overlap, and enhanced neutral borders/focus geometry remained visible. | PASS |
| Project authority | Run the native smoke assertions around accessibility preference changes and Context Help. Scaling, contrast, motion, focus/help, and workspace presentation did not change canonical project bytes, dirty state, or structural history. | PASS |
| Large graph/native peer | Create the native macOS peer in the 1,000-node/2,000-connection fixture. The graph handler exposed every node/port child through the ignored intermediary in stable order, while paint/hit work remained viewport-bounded. | PASS |

The inspected local capture names are:

- `workbench-accessibility-150-default.png`
- `workbench-accessibility-150-patch.png`
- `workbench-accessibility-150-analysis.png`
- `workbench-accessibility-150-performance.png`
- `workbench-accessibility-150-samples.png`
- `workbench-context-help-150.png`

They are generated test evidence under `build/workbench-debug/workbench-smoke/`
and are intentionally not product source assets.

## Automated evidence paired with the manual pass

- focused E7/native smoke and independent review: PASS, no P0-P3;
- ASan/UBSan viewport-index target: `1/1` PASS;
- complete Debug suite: `281/281` PASS;
- tests-off Release Workbench smoke: `1/1` PASS;
- product-valid Release large-graph probe: 1,000 nodes / 2,000 connections,
  paint p50 `6.036291 ms`, p95 `6.136709 ms`, max `6.470833 ms`, rebuild
  p50 `25.089 ms`, p95/max `25.804542 ms`;
- generated, governance, release-configuration, diff, and protected-path
  gates: PASS.

## Deferred final-platform evidence

The following are deliberately deferred to project closeout because no Windows
workstation is part of this local acceptance environment:

- Windows Narrator reading order, names, states, actions, and live-region
  behaviour across Patch, Automation, Analysis, Performance, and Samples;
- keyboard-only operation under the Windows shortcut/modifier model;
- 100%, 115%, 130%, and 150% Workbench scale composed with Windows 100%, 125%,
  150%, and 200% display scaling;
- enhanced-contrast/high-contrast theme inspection and visible focus;
- final packaged-binary Narrator/high-DPI evidence.

These deferred checks do not authorize a WCAG, VPAT, or cross-platform
accessibility certification claim.
