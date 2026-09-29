#include "Engine.h"
#include "BiQuadFilterFactory.h"
#include "PreampFilterFactory.h"
#include "IIRFilterFactory.h"
#include "DelayFilterFactory.h"
#include "IFilterFactory.h"
#include "helpers/StringHelper.h"
#include <fstream>
#include <sstream>
#include <stdexcept>

void Engine::FilterDeleter::operator()(IFilter* filter) const
{
    if (!filter) return;
    filter->~IFilter();
    MemoryHelper::free(filter);
}

Engine::Engine(unsigned sampleRate, unsigned channels, unsigned maxFrames, std::vector<std::wstring> names)
    : rate(sampleRate), channelCount(channels), maxFrames(maxFrames), planar(channels, std::vector<float>(maxFrames)), scratch(channels, std::vector<float>(maxFrames)), channelPtrs(channels), scratchPtrs(channels)
{
    if(!names.empty()&&names.size()!=channels)throw std::runtime_error("channel names/count mismatch");
    channelNames=std::move(names);
    for (unsigned c=0;c<channels;++c) { channelPtrs[c]=planar[c].data(); scratchPtrs[c]=scratch[c].data(); }
}

void Engine::loadConfig(const std::string& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config: " + path);
    PreampFilterFactory preamp;
    BiQuadFilterFactory biquad;
    IIRFilterFactory iir;
    DelayFilterFactory delay;
    std::vector<std::unique_ptr<IFilter, FilterDeleter>> candidate;
    std::string raw;
    unsigned lineNo=0;
    while (std::getline(in,raw)) {
        ++lineNo;
        auto comment=raw.find('#'); if(comment!=std::string::npos)raw.resize(comment);
        auto line=StringHelper::trim(StringHelper::toWString(raw,65001)); if(line.empty())continue;
        auto colon=line.find(L':');
        if(colon==line.npos) throw std::runtime_error(path+":"+std::to_string(lineNo)+": expected command:");
        std::wstring command=StringHelper::trim(line.substr(0,colon));
        std::wstring params=StringHelper::trim(line.substr(colon+1));
        std::vector<IFilter*> made;
        const auto widePath=StringHelper::toWString(path,65001);
        if(command==L"Preamp") made=preamp.createFilter(widePath,command,params);
        else if(command==L"Filter") {
            made=biquad.createFilter(widePath,command,params);
            if(made.empty()) made=iir.createFilter(widePath,command,params);
        }
        else if(command==L"Delay") made=delay.createFilter(widePath,command,params);
        else throw std::runtime_error(path+":"+std::to_string(lineNo)+": unsupported command '"+StringHelper::toString(command,65001)+"'");
        if(made.empty()) throw std::runtime_error(path+":"+std::to_string(lineNo)+": invalid "+StringHelper::toString(command,65001)+" parameters");
        static const std::wstring positions[]={L"L",L"R",L"C",L"LFE",L"RL",L"RR",L"SL",L"SR"};
        std::vector<std::wstring> channels;
        if(channelCount==1) channels={L"C"};
        else if(channelCount==2) channels={L"L",L"R"};
        else for(unsigned c=0;c<channelCount;++c) channels.push_back(c<8?positions[c]:std::to_wstring(c+1));
        if(!channelNames.empty())channels=channelNames;
        for(auto* f:made){ candidate.emplace_back(f); candidate.back()->initialize(static_cast<float>(rate),maxFrames,channels); }
    }
    filters.swap(candidate);
}

void Engine::process(float* samples, unsigned frames)
{
    if(frames>maxFrames) throw std::runtime_error("frame block exceeds configured maximum");
    for(unsigned c=0;c<channelCount;++c) { channelPtrs[c]=planar[c].data(); scratchPtrs[c]=scratch[c].data(); }
    for(unsigned c=0;c<channelCount;++c) for(unsigned f=0;f<frames;++f) planar[c][f]=samples[f*channelCount+c];
    for(auto& filter:filters) {
        float** output=filter->getInPlace()?channelPtrs.data():scratchPtrs.data();
        filter->process(output,channelPtrs.data(),frames);
        if(!filter->getInPlace()) std::swap(channelPtrs,scratchPtrs);
    }
    for(unsigned c=0;c<channelCount;++c) for(unsigned f=0;f<frames;++f) samples[f*channelCount+c]=channelPtrs[c][f];
}
