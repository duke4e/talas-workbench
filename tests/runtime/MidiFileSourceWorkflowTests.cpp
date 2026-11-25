#include "talas/audio/ProjectAssetLoader.h"
#include "talas/audio/ProjectAssetChangeTracker.h"
#include "talas/audio/WavReader.h"
#include "talas/cli/Cli.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectFile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <sstream>
#include <stdexcept>

namespace
{
void require(bool value, std::string_view message)
{
    if (!value) throw std::runtime_error(std::string(message));
}
const auto& registry() { static const auto r=talas::core::makeBuiltinNodeRegistry(); return r; }
talas::project::ProjectParameterExpression text(std::string id,std::string value)
{
    talas::project::ProjectParameterExpression p; p.parameterId=std::move(id);p.text=std::move(value);return p;
}
struct Directory
{
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("talas-midi-file-source-workflow-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Directory() { std::filesystem::create_directories(path); }
    ~Directory() { std::error_code e;std::filesystem::remove_all(path,e); }
};
talas::project::ProjectDocument instrument(std::string source,bool nested=false)
{
    talas::project::ProjectDocument d;
    talas::project::ProjectParameterExpression resource;resource.parameterId="midiFile";resource.resource="notes";
    talas::project::ProjectParameterExpression shared;shared.parameterId="shared";shared.boolean=false;
    talas::project::ProjectGraph body;
    body.nodes={{"osc","talas.oscillator",{{"frequency","7 hz"},{"amplitude","(!KeyDown > 0) * !KeyVelocity"}}},
        {"voice","talas.midiVoice",{{"left","1"},{"right","0.5"},{"channel","1"},
            {"polyphony","1"},{"lowPitch","0 nn"},{"highPitch","127 nn"},
            text("source",std::move(source)),resource,text("script",""),shared}}};
    body.connections={{"osc","audioOut","voice","audioIn"}};
    std::string owner="voice";
    if (nested)
    {
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId="instrument";definition.displayName="Instrument";definition.graph=std::move(body);
        definition.outputs={{"leftOut",talas::core::PortType::Audio,"voice","leftOut"},
            {"rightOut",talas::core::PortType::Audio,"voice","rightOut"}};
        d.soundDefinitions.push_back(std::move(definition));
        talas::project::ProjectNode instance;instance.instanceId="cell";instance.soundDefinitionId="instrument";
        d.graph.nodes.push_back(std::move(instance));owner="cell";
    }
    else d.graph=std::move(body);
    d.graph.nodes.push_back({"out","talas.audioOutput",{}});
    d.graph.connections.push_back({owner,"leftOut","out","audioIn"});
    d.graph.connections.push_back({owner,"rightOut","out","rightIn"});
    d.assets={{"notes",talas::project::ProjectAssetKind::Binary,"notes.mid"}};
    return d;
}
talas::project::ProjectDocument mapperInstrument(std::string source,bool nested=false)
{
    auto document=instrument(std::move(source),nested);
    auto& node=nested?document.soundDefinitions[0].graph.nodes[1]:document.graph.nodes[1];
    node.typeId="talas.midiMapper";
    node.parameters.push_back(text("map",""));
    talas::project::ProjectParameterExpression model;
    model.parameterId="voiceModel";model.choice="polyphonicInput";
    node.parameters.push_back(std::move(model));
    return document;
}
void writeMidi(const std::filesystem::path& path)
{
    const std::array<std::uint8_t,34> bytes={'M','T','h','d',0,0,0,6,0,0,0,1,1,0xf4,
        'M','T','r','k',0,0,0,12, 0,0x90,60,127, 20,0x80,60,0, 0,0xff,0x2f,0};
    std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());require(out.good(),"write MIDI fixture");
}
void testInactiveSourceReferences()
{
    Directory dir;
    for (const bool mapper : {false,true})
    for (const bool nested : {false,true})
    for (const auto* source : {"","Live MIDI","  lIvE MiDi\t","Script"})
    {
        auto d=mapper?mapperInstrument(source,nested):instrument(source,nested);
        const auto refs=talas::audio::reachableProjectAssetReferences(d,registry());
        require(refs.ok() && refs.references.empty(),"inactive MIDI file must not be a reachable resource");
        const auto loaded=talas::audio::loadReferencedProjectResourceSnapshots(d,registry(),dir.path/"sound.talas.json");
        require(loaded.ok() && loaded.snapshots.empty(),"missing inactive MIDI file must not block staging");
        require(talas::audio::captureProjectAssetWatchSnapshot(d,registry(),dir.path/"sound.talas.json").entries.empty(),
            "inactive file must not start a missing-asset watcher");
        require((nested?d.soundDefinitions[0].graph.nodes[1]:d.graph.nodes[1]).parameters[7].resource=="notes",
            "reference selection must preserve authored inactive field");
    }
    auto d=instrument("");
    std::erase_if(d.graph.nodes[1].parameters,[](const auto& p){return p.parameterId=="source";});
    const auto omitted=talas::audio::reachableProjectAssetReferences(d,registry());
    require(omitted.ok() && omitted.references.empty(),
        "omitted source uses historical live default");
}
void testExposedSourceSelection()
{
    for (const bool mapper : {false,true})
    {
        auto d=mapper?mapperInstrument("Live MIDI",true):instrument("Live MIDI",true);
        talas::project::ProjectSoundExposedParameter source;
        source.parameterId="inputSource";source.displayName="Input Source";
        source.kind=talas::core::ParameterKind::Text;
        source.defaultValue=text("inputSource","MIDI File");
        source.bindings={{talas::project::ProjectSoundParameterTarget::NodeParameter,"voice","source"}};
        d.soundDefinitions[0].exposedParameters={source};
        auto second=d.graph.nodes[0];second.instanceId="other";
        second.parameters={text("inputSource","Live MIDI")};
        d.graph.nodes.push_back(second);
        d.graph.connections.back()={"other","rightOut","out","rightIn"};
        auto refs=talas::audio::reachableProjectAssetReferences(d,registry());
        require(refs.ok() && refs.references.size()==1 && refs.references[0].structuralSourcePath.find("cell")!=std::string::npos,
            "expanded source default selects only its owning instance");
        d.graph.nodes[0].parameters={text("inputSource","Live MIDI")};
        refs=talas::audio::reachableProjectAssetReferences(d,registry());
        require(refs.ok() && refs.references.empty(),
            "instance override disables a file selected by the exposed default");
        d.graph.nodes.back().parameters={text("inputSource","MIDI File")};
        refs=talas::audio::reachableProjectAssetReferences(d,registry());
        require(refs.ok() && refs.references.size()==1 && refs.references[0].structuralSourcePath.find("other")!=std::string::npos,
            "source override belongs to its exact second instance");
    }
}
void testActiveAndInvalidReferences()
{
    Directory dir;
    for (const bool mapper : {false,true})
    for (const bool nested : {false,true})
    {
        auto d=mapper?mapperInstrument("MIDI File",nested):instrument("MIDI File",nested);
        const auto refs=talas::audio::reachableProjectAssetReferences(d,registry());
        require(refs.ok() && refs.references.size()==1 && refs.references[0].assetId=="notes",
            "selected MIDI file must be reachable");
        const auto loaded=talas::audio::loadReferencedProjectResourceSnapshots(d,registry(),dir.path/"sound.talas.json");
        require(!loaded.ok() && loaded.failure && loaded.failure->parameterId=="midiFile",
            "missing active file identifies its parameter");
        require(!nested || loaded.failure->structuralSourcePath.find("cell")!=std::string::npos,
            "nested missing file retains structural source path");
    }
    auto defaultMapper=mapperInstrument("MIDI File");
    defaultMapper.graph.nodes[1].parameters.pop_back();
    auto refs=talas::audio::reachableProjectAssetReferences(defaultMapper,registry());
    require(refs.ok() && refs.references.size()==1 && refs.references[0].assetId=="notes",
        "new Mapper without an explicit voiceModel must select its MIDI file");
    auto legacyMapper=mapperInstrument("");
    legacyMapper.graph.nodes[1].parameters.back().choice="legacyPassthrough";
    refs=talas::audio::reachableProjectAssetReferences(legacyMapper,registry());
    require(refs.ok() && refs.references.empty(),
        "legacy Mapper's inactive MIDI file must not be loaded");
    for (const bool mapper : {false,true})
    {
        auto invalid=mapper?mapperInstrument("MIDIFile"):instrument("MIDIFile");
        const auto refs=talas::audio::reachableProjectAssetReferences(invalid,registry());
        require(!refs.ok() && refs.references.empty() && refs.failure->parameterId=="source",
            "unknown source must fail atomically before resource lookup");
    }
}
void testSourceEditorAndPersistence()
{
    Directory dir;
    for (const bool mapper : {false,true})
    for (const bool nested : {false,true})
    {
        talas::project::ProjectEditor editor(
            mapper?mapperInstrument("Live MIDI",nested):instrument("Live MIDI",nested),registry());
        const auto set=[&](std::string value) {
            return nested?editor.setParameterInSoundDefinition("instrument","voice",text("source",std::move(value)))
                :editor.setParameter("voice",text("source",std::move(value)));
        };
        const auto count=[&] {
            const auto refs=talas::audio::reachableProjectAssetReferences(editor.document(),registry());
            require(refs.ok(),"editor source references must resolve");
            return refs.references.size();
        };
        require(count()==0,"editor live source starts without active file");
        require(set("MIDI File").ok() && count()==1,"source edit activates resource");
        require(editor.undo().ok() && count()==0,"undo restores live source");
        require(editor.redo().ok() && count()==1,"redo restores file source");
        const auto path=dir.path/(nested?"nested.talas.json":"root.talas.json");
        require(talas::project::saveProjectFile(path,editor.document()).ok(),"save source selection");
        const auto reopened=talas::project::loadProjectFile(path);
        require(reopened.ok() && reopened.document->graph==editor.document().graph
            && reopened.document->soundDefinitions==editor.document().soundDefinitions
            && reopened.document->assets==editor.document().assets,"save/reopen preserves exact source and file fields");
        require(talas::audio::reachableProjectAssetReferences(*reopened.document,registry()).references.size()==1,
            "reopened selected file remains reachable");
    }
}
void testLoadedProjectCliExport()
{
    Directory dir;
    for (const bool mapper : {false,true})
    for (const bool nested : {false,true})
    {
        writeMidi(dir.path/"notes.mid");
        auto d=mapper?mapperInstrument("MIDI File",nested):instrument("MIDI File",nested);
        const auto path=dir.path/"sound.talas.json";const auto wav=dir.path/"sound.wav";
        require(talas::project::saveProjectFile(path,d).ok(),"save file instrument");
        const auto loaded=talas::audio::loadReferencedProjectResourceSnapshots(d,registry(),path);
        require(loaded.ok() && loaded.snapshots.size()==1 && loaded.snapshots[0]->binaryBytes.size()==34,
            "prepare immutable MIDI bytes from project-relative asset");
        const auto projectArg=path.string();const auto wavArg=wav.string();
        const std::array<std::string_view,17> args={"render","--project",projectArg,"--frames","32",
            "--sample-rate","1000","--max-block-frames","7","--output",wavArg,
            "--event","KeyDown=0","--event","KeyVelocity=0","--event","Pitch:nn=60"};
        std::ostringstream output,error;
        const auto result=talas::cli::runCli(args,output,error);
        require(result==0,"CLI MIDI instrument export: "+error.str());
        const auto audio=talas::audio::readPcm16Wav(wav);
        require(audio.ok() && audio.sampleRate==1000 && audio.channels.size()==2 && audio.channels[0].size()==32,
            "exported MIDI instrument WAV shape");
        for (std::size_t f=0; f<32; ++f)
        for (std::size_t ch=0;ch<2;++ch)
        {
            const auto expected=f>=5 && f<20 ? (ch==0?1:0.5)*std::sin(2*std::numbers::pi*7*f/1000):0;
            require(std::abs(audio.channels[ch][f]-expected)<1.0/32767,"CLI PCM16 matches independent note/oscillator oracle");
        }
        std::filesystem::remove(dir.path/"notes.mid");
        auto& body=nested?d.soundDefinitions[0].graph:d.graph;
        body.nodes[1].parameters[6].text="Live MIDI";
        require(talas::project::saveProjectFile(path,d).ok(),"save inactive file for live export");
        if (mapper)
        {
            const auto reopened=talas::project::loadProjectFile(path);
            require(reopened.ok(),"reopen live Mapper project");
            const auto& savedBody=nested?reopened.document->soundDefinitions[0].graph
                :reopened.document->graph;
            const auto model=std::ranges::find_if(savedBody.nodes[1].parameters,
                [](const auto& parameter){return parameter.parameterId=="voiceModel";});
            require(model!=savedBody.nodes[1].parameters.end() && model->choice
                    && *model->choice=="polyphonicInput",
                "reopened live Mapper retains polyphonic model");
        }
        std::ostringstream liveOutput,liveError;
        const auto liveResult=talas::cli::runCli(args,liveOutput,liveError);
        require(liveResult==0,
            std::string(mapper?"Mapper":"MIDI Voice")
                +(nested?" nested":" root")
                +" CLI live source ignores missing inactive MIDI file: "+liveError.str());
        const auto silent=talas::audio::readPcm16Wav(wav);
        require(silent.ok() && silent.channels.size()==2 && silent.channels[0].size()==32,
            "inactive file export retains WAV shape");
        for (const auto& channel:silent.channels)
            require(std::ranges::all_of(channel,[](double value){return value==0;}),
                "live export must not replay the inactive file");
    }
}
void testScriptEditorPersistenceAndCliExport()
{
    Directory dir;
    for (const bool mapper : {false,true})
    for (const bool nested : {false, true})
    {
        auto document = mapper?mapperInstrument("Live MIDI", nested)
            :instrument("Live MIDI", nested);
        auto& body = nested ? document.soundDefinitions[0].graph : document.graph;
        body.nodes[0].parameters[0].expression = "!Pitch";
        talas::project::ProjectEditor editor(std::move(document), registry());
        const auto set = [&](talas::project::ProjectParameterExpression parameter) {
            return nested ? editor.setParameterInSoundDefinition("instrument", "voice", std::move(parameter))
                : editor.setParameter("voice", std::move(parameter));
        };
        const auto script = "self keyDownAt: 0 ms duration: 20 ms frequency: 445 hz velocity: 0.7.";
        require(set(text("source", "Script")).ok() && set(text("script", script)).ok(),
            "author literal MIDI Voice Script");
        require(editor.undo().ok() && editor.redo().ok(), "script text is one Undo/Redo step");
        const auto path = dir.path / (nested ? "nested-script.talas.json" : "script.talas.json");
        const auto wav = dir.path / (nested ? "nested-script.wav" : "script.wav");
        require(talas::project::saveProjectFile(path, editor.document()).ok(), "save script instrument");
        const auto reopened = talas::project::loadProjectFile(path);
        require(reopened.ok(), "reopen script instrument");
        const auto& reopenedBody = nested ? reopened.document->soundDefinitions[0].graph
            : reopened.document->graph;
        require(reopenedBody.nodes[1].parameters[6].text == "Script"
            && reopenedBody.nodes[1].parameters[8].text == script,
            "script source and exact text survive save/reopen");
        const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
            *reopened.document, registry(), path);
        require(loaded.ok() && loaded.snapshots.empty(),
            "script source does not load its inactive missing MIDI file");
        const auto projectArg = path.string(), wavArg = wav.string();
        const std::array<std::string_view, 17> args = {"render", "--project", projectArg,
            "--frames", "32", "--sample-rate", "1000", "--max-block-frames", "7",
            "--output", wavArg, "--event", "KeyDown=0", "--event", "KeyVelocity=0",
            "--event", "Pitch:nn=60"};
        std::ostringstream output, error;
        require(talas::cli::runCli(args, output, error) == 0,
            "CLI MIDI Script export: " + error.str());
        const auto audio = talas::audio::readPcm16Wav(wav);
        require(audio.ok() && audio.channels.size() == 2 && audio.channels[0].size() == 32,
            "script CLI WAV shape");
        double phase = 0.0;
        for (std::size_t frame = 0; frame < 32; ++frame)
        {
            const auto expected = frame >= 5 && frame < 20
                ? 0.7 * std::sin(2 * std::numbers::pi * phase) : 0.0;
            require(std::abs(audio.channels[0][frame] - expected) < 1.0 / 32767
                && std::abs(audio.channels[1][frame] - expected * 0.5) < 1.0 / 32767,
                "script CLI WAV retains continuous frequency and velocity");
            const auto frequency = frame >= 5 ? 445.0
                : 440.0 * std::pow(2.0, (60.0 - 69.0) / 12.0);
            phase = std::fmod(phase + frequency / 1000.0, 1.0);
        }
    }
}
}
int main()
{
    bool failed=false;
    for (const auto& [name,test] : std::array{
        std::pair{"inactive-source-references",&testInactiveSourceReferences},
        std::pair{"active-invalid-references",&testActiveAndInvalidReferences},
        std::pair{"source-editor-persistence",&testSourceEditorAndPersistence},
        std::pair{"exposed-source-selection",&testExposedSourceSelection},
        std::pair{"loaded-project-cli-export",&testLoadedProjectCliExport},
        std::pair{"script-editor-cli-export",&testScriptEditorPersistenceAndCliExport}})
    {
        try {test();std::cout<<"PASS "<<name<<'\n';}
        catch(const std::exception& e) {failed=true;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
    }
    return failed?1:0;
}
