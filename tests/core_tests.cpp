#include "Engine.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <unistd.h>

int main()
{
    const std::string path="/tmp/skyapo-test-"+std::to_string(getpid())+".txt";
    {std::ofstream f(path);f<<"Preamp: -6 dB\n";}
    Engine e(48000,2,128);e.loadConfig(path);float data[8]={1,1,.5f,.5f,-1,-1,.25f,.25f};e.process(data,4);
    const float gain=std::pow(10.0f,-6.0f/20.0f);
    const float expected[8]={gain,gain,.5f*gain,.5f*gain,-gain,-gain,.25f*gain,.25f*gain};
    for(int i=0;i<8;++i)if(std::abs(data[i]-expected[i])>1e-5f){std::cerr<<"preamp amplitude mismatch at "<<i<<": "<<data[i]<<'\n';return 1;}
    {std::ofstream f(path);f<<"Filter: ON PK Fc 1000 Hz Gain 6 dB Q 1.0\n";}
    Engine eq(48000,2,128);eq.loadConfig(path);float impulse[32]={1.0f};eq.process(impulse,16);
    bool changed=false;for(int i=0;i<32;++i)changed|=std::abs(impulse[i])>1e-5f&&i>0;
    if(!changed){std::cerr<<"upstream BiQuad did not produce a filter tail\n";return 1;}
    {std::ofstream f(path);f<<"NotACommand: 1\n";}
    bool rejected=false;try{Engine bad(48000,2,128);bad.loadConfig(path);}catch(const std::exception&){rejected=true;}
    if(!rejected){std::cerr<<"unsupported config command was not rejected\n";return 1;}
    {std::ofstream f(path);f<<"Filter: ON IIR Order 1 Coefficients 1 0 1 0\n";}
    Engine iir(48000,2,128);iir.loadConfig(path);float unity[6]={.1f,-.2f,.3f,-.4f,.5f,-.6f};iir.process(unity,3);
    const float original[6]={.1f,-.2f,.3f,-.4f,.5f,-.6f};for(int i=0;i<6;++i)if(std::abs(unity[i]-original[i])>1e-5f){std::cerr<<"IIR unity mismatch\n";return 1;}
    {std::ofstream f(path);f<<"Delay: 1 Samples\n";}
    Engine delay(48000,2,128);delay.loadConfig(path);float samples[6]={1,10,2,20,3,30};delay.process(samples,1);delay.process(samples+2,2);
    const float delayed[6]={0,0,1,10,2,20};for(int i=0;i<6;++i)if(std::abs(samples[i]-delayed[i])>1e-5f){std::cerr<<"Delay mismatch at "<<i<<" got "<<samples[i]<<'\n';return 1;}
    unlink(path.c_str());std::cout<<"Upstream Preamp/BiQuad/IIR/Delay and config rejection tests passed\n";return 0;
}
