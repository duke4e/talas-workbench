#include "talas/plugin/PluginRuntime.h"

// ADR-0029: the projects a raw template plays when its bundle embeds none.
// Both pass `talas_cli validate`; the plugin tests stage them too.
namespace talas::plugin
{
namespace
{
// The effect: input channels 1 and 2 to the outputs, unchanged.
constexpr std::string_view passThroughProject = R"json({
 "format": "talas.project",
 "version": 156,
 "expressionLanguage": "variablesV2",
 "expressionDefinitions": [],
 "variableLibrary": {
  "version": 2,
  "variables": []
 },
 "soundVariableLibrary": {
  "version": 1,
  "variables": []
 },
 "generatorLibrary": {
  "version": 2,
  "programs": []
 },
 "liveSetLibrary": {
  "version": 1,
  "liveSets": [],
  "tools": []
 },
 "metadata": {
  "title": "Pass Through",
  "description": "The raw Talas Effect template: its stereo input, unchanged."
 },
 "presentation": {
  "version": 2,
  "structureLayouts": [
   {
    "scope": {
     "kind": "project"
    },
    "nodes": [
     {
      "nodeId": "input",
      "x": 0,
      "y": 0
     },
     {
      "nodeId": "output",
      "x": 220,
      "y": 0
     }
    ]
   }
  ],
  "performanceControls": [],
  "performanceMeters": []
 },
 "snapshotLibrary": {
  "version": 1,
  "banks": []
 },
 "sampleMapLibrary": {
  "version": 1,
  "maps": []
 },
 "virtualControls": [],
 "assets": [],
 "partialTrackEdits": [],
 "timeline": {
  "loop": {
   "enabled": false,
   "startSeconds": 0,
   "endSeconds": 4
  },
  "musicalTime": {
   "ticksPerQuarter": 960,
   "tempo": [
    {
     "tick": 0,
     "microsecondsPerQuarter": 500000
    }
   ],
   "meter": [
    {
     "tick": 0,
     "numerator": 4,
     "denominator": 4
    }
   ]
  },
  "score": {
   "tracks": [],
   "clips": [],
   "automation": [],
   "markers": []
  },
  "eventValues": []
 },
 "deviceSettings": {
  "configured": false,
  "audioInputDevice": "",
  "audioOutputDevice": "",
  "sampleRate": 0,
  "bufferSize": 0,
  "inputChannels": [],
  "outputChannels": [],
  "midiInputDevices": [],
  "transportSync": {
   "sourceMode": "internal",
   "sourceIdentifier": ""
  },
  "oscIngress": {
   "enabled": false,
   "port": 9000
  },
  "oscEgress": {
   "enabled": false,
   "port": 9001
  }
 },
 "soundDefinitions": [],
 "graph": {
  "nodes": [
   {
    "id": "input",
    "typeId": "talas.audioInput",
    "parameters": [
     {
      "id": "routing",
      "choice": "stereoPair"
     },
     {
      "id": "channel1",
      "boolean": true
     },
     {
      "id": "channel2",
      "boolean": true
     }
    ]
   },
   {
    "id": "output",
    "typeId": "talas.audioOutput",
    "parameters": []
   }
  ],
  "connections": [
   {
    "fromNode": "input",
    "fromPort": "audioOut",
    "toNode": "output",
    "toPort": "audioIn"
   },
   {
    "fromNode": "input",
    "fromPort": "rightOut",
    "toNode": "output",
    "toPort": "rightIn"
   }
  ]
 }
})json";

// The instrument: eight saw voices, an envelope each, from live MIDI; !Level
// and !Release are its parameters.
constexpr std::string_view sawVoiceProject = R"json({
 "format": "talas.project",
 "version": 156,
 "expressionLanguage": "variablesV2",
 "expressionDefinitions": [],
 "variableLibrary": {
  "version": 2,
  "variables": []
 },
 "soundVariableLibrary": {
  "version": 1,
  "variables": []
 },
 "generatorLibrary": {
  "version": 2,
  "programs": []
 },
 "liveSetLibrary": {
  "version": 1,
  "liveSets": [],
  "tools": []
 },
 "metadata": {
  "title": "Saw Voice",
  "description": "The raw Talas Instrument template: eight saw voices with an envelope, played from MIDI."
 },
 "presentation": {
  "version": 2,
  "structureLayouts": [
   {
    "scope": {
     "kind": "project"
    },
    "nodes": [
     {
      "nodeId": "keysVoice",
      "x": 0,
      "y": 0
     },
     {
      "nodeId": "keys",
      "x": 220,
      "y": 0
     },
     {
      "nodeId": "output",
      "x": 440,
      "y": 0
     }
    ]
   },
   {
    "scope": {
     "kind": "soundDefinition",
     "definitionId": "voice"
    },
    "nodes": [
     {
      "nodeId": "saw",
      "x": 0,
      "y": 0
     },
     {
      "nodeId": "env",
      "x": 220,
      "y": 0
     },
     {
      "nodeId": "amp",
      "x": 440,
      "y": 0
     }
    ]
   }
  ],
  "performanceControls": [
   {
    "controlId": "Level",
    "target": {
     "kind": "eventValue",
     "eventName": "Level",
     "domain": "scalar"
    },
    "kind": "fader",
    "displayName": "Level",
    "x": 0,
    "y": 0,
    "visible": true,
    "modulationSlots": []
   },
   {
    "controlId": "Release",
    "target": {
     "kind": "eventValue",
     "eventName": "Release",
     "domain": "scalar"
    },
    "kind": "fader",
    "displayName": "Release (s)",
    "x": 174,
    "y": 0,
    "visible": true,
    "modulationSlots": []
   }
  ],
  "performanceMeters": []
 },
 "snapshotLibrary": {
  "version": 1,
  "banks": []
 },
 "sampleMapLibrary": {
  "version": 1,
  "maps": []
 },
 "virtualControls": [
  {
   "eventName": "Level",
   "domain": "scalar",
   "kind": "fader",
   "value": 0.5,
   "minimum": 0,
   "maximum": 1,
   "widget": {
    "style": "default",
    "showNumber": true,
    "label": "Level",
    "onColour": "",
    "offColour": "",
    "grid": 0,
    "taper": "linear",
    "tab": ""
   }
  },
  {
   "eventName": "Release",
   "domain": "scalar",
   "kind": "fader",
   "value": 0.3,
   "minimum": 0.01,
   "maximum": 4,
   "widget": {
    "style": "default",
    "showNumber": true,
    "label": "Release (s)",
    "onColour": "",
    "offColour": "",
    "grid": 0,
    "taper": "log",
    "tab": ""
   }
  }
 ],
 "assets": [],
 "partialTrackEdits": [],
 "timeline": {
  "loop": {
   "enabled": false,
   "startSeconds": 0,
   "endSeconds": 4
  },
  "musicalTime": {
   "ticksPerQuarter": 960,
   "tempo": [
    {
     "tick": 0,
     "microsecondsPerQuarter": 500000
    }
   ],
   "meter": [
    {
     "tick": 0,
     "numerator": 4,
     "denominator": 4
    }
   ]
  },
  "score": {
   "tracks": [],
   "clips": [],
   "automation": [],
   "markers": []
  },
  "eventValues": []
 },
 "deviceSettings": {
  "configured": false,
  "audioInputDevice": "",
  "audioOutputDevice": "",
  "sampleRate": 0,
  "bufferSize": 0,
  "inputChannels": [],
  "outputChannels": [],
  "midiInputDevices": [],
  "transportSync": {
   "sourceMode": "internal",
   "sourceIdentifier": ""
  },
  "oscIngress": {
   "enabled": false,
   "port": 9000
  },
  "oscEgress": {
   "enabled": false,
   "port": 9001
  }
 },
 "soundDefinitions": [
  {
   "id": "voice",
   "name": "Saw Voice",
   "description": "One voice of the raw Talas Instrument template.",
   "interfaceVersion": 1,
   "inputs": [],
   "outputs": [
    {
     "id": "audioOut",
     "type": "audio",
     "nodeId": "amp",
     "nodePortId": "audioOut"
    }
   ],
   "parameters": [],
   "graph": {
    "nodes": [
     {
      "id": "saw",
      "typeId": "talas.oscillator",
      "parameters": [
       {
        "id": "frequency",
        "expression": "!KeyPitch"
       },
       {
        "id": "amplitude",
        "expression": "0.25"
       },
       {
        "id": "wavetable",
        "resource": "Saw21"
       }
      ]
     },
     {
      "id": "env",
      "typeId": "talas.adsr",
      "parameters": [
       {
        "id": "attackTime",
        "expression": "5 ms"
       },
       {
        "id": "decayTime",
        "expression": "200 ms"
       },
       {
        "id": "sustainLevel",
        "expression": "0.7"
       },
       {
        "id": "releaseTime",
        "expression": "!Release * 1 s"
       },
       {
        "id": "gate",
        "expression": "!KeyDown"
       },
       {
        "id": "scale",
        "expression": "!KeyVelocity"
       }
      ]
     },
     {
      "id": "amp",
      "typeId": "talas.vca",
      "parameters": [
       {
        "id": "gain",
        "expression": "1"
       }
      ]
     }
    ],
    "connections": [
     {
      "fromNode": "saw",
      "fromPort": "audioOut",
      "toNode": "amp",
      "toPort": "audioIn"
     },
     {
      "fromNode": "env",
      "fromPort": "value",
      "toNode": "amp",
      "toPort": "gain"
     }
    ]
   }
  }
 ],
 "graph": {
  "nodes": [
   {
    "id": "keysVoice",
    "soundDefinitionId": "voice",
    "parameters": []
   },
   {
    "id": "keys",
    "typeId": "talas.midiVoice",
    "parameters": [
     {
      "id": "left",
      "expression": "!Level"
     },
     {
      "id": "right",
      "expression": "!Level"
     },
     {
      "id": "channel",
      "expression": "0"
     },
     {
      "id": "source",
      "text": "Live MIDI"
     },
     {
      "id": "midiFile",
      "resource": ""
     },
     {
      "id": "polyphony",
      "expression": "8"
     },
     {
      "id": "lowPitch",
      "expression": "0 nn"
     },
     {
      "id": "highPitch",
      "expression": "127 nn"
     },
     {
      "id": "script",
      "text": ""
     },
     {
      "id": "shared",
      "boolean": false
     }
    ]
   },
   {
    "id": "output",
    "typeId": "talas.audioOutput",
    "parameters": []
   }
  ],
  "connections": [
   {
    "fromNode": "keysVoice",
    "fromPort": "audioOut",
    "toNode": "keys",
    "toPort": "audioIn"
   },
   {
    "fromNode": "keys",
    "fromPort": "leftOut",
    "toNode": "output",
    "toPort": "audioIn"
   },
   {
    "fromNode": "keys",
    "fromPort": "rightOut",
    "toNode": "output",
    "toPort": "rightIn"
   }
  ]
 }
})json";
}

std::string_view defaultPluginProjectJson(PluginKind kind) noexcept
{
    return kind == PluginKind::Instrument ? sawVoiceProject : passThroughProject;
}
}
