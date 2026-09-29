#include "Engine.h"
#include <sndfile.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

int main(int argc,char** argv)
{
    std::string input,output,config;
    for(int i=1;i<argc;++i){std::string a=argv[i]; if(i+1>=argc){std::cerr<<"missing value for "<<a<<"\n";return 2;} if(a=="--input")input=argv[++i];else if(a=="--output")output=argv[++i];else if(a=="--config")config=argv[++i];else{std::cerr<<"unknown option: "<<a<<"\n";return 2;}}
    if(input.empty()||output.empty()||config.empty()){std::cerr<<"usage: skyapo-render --input input.wav --output output.wav --config config.txt\n";return 2;}
    SNDFILE* in=nullptr; SNDFILE* out=nullptr;
    try {
        SF_INFO info{}; in=sf_open(input.c_str(),SFM_READ,&info); if(!in)throw std::runtime_error(sf_strerror(nullptr));
        Engine engine(info.samplerate,info.channels,4096); engine.loadConfig(config);
        SF_INFO oi=info; oi.format=SF_FORMAT_WAV|SF_FORMAT_FLOAT; out=sf_open(output.c_str(),SFM_WRITE,&oi); if(!out)throw std::runtime_error(sf_strerror(nullptr));
        std::vector<float> block(static_cast<size_t>(4096)*info.channels);
        sf_count_t total=0;
        for(;;){auto n=sf_readf_float(in,block.data(),4096);if(n<=0)break;engine.process(block.data(),static_cast<unsigned>(n));if(sf_writef_float(out,block.data(),n)!=n)throw std::runtime_error(sf_strerror(out));total+=n;}
        sf_close(in);in=nullptr;sf_close(out);out=nullptr;
        std::cout<<"Processed "<<total<<" frames at "<<info.samplerate<<" Hz, "<<info.channels<<" channels, "<<engine.filterCount()<<" filters\n";
    } catch(const std::exception& e){if(in)sf_close(in);if(out)sf_close(out);std::cerr<<"skyapo-render: "<<e.what()<<"\n";return 1;}
}
