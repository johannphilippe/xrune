#include <iostream>
#include "player.hpp"
#include "xrune.h"

using namespace std;

constexpr const char * version = "0.0";

bool is_parameter(const char* arg)
{
    return arg[0] == '-';
}

void print_xrune()
{
    cout << "\\|/" << endl << " X " << endl << "/|\\" << endl; 
}

int parse_command_line(int argc, char* argv[], xrune::options &options)
{
    // Parse command line arguments
    for(size_t i = 0; i < argc; ++i) 
    {
        if(is_parameter(argv[i]))
        {
            std::string param(argv[i]);
            if(param == "-h" || param == "--help") 
            {
                options.exit_after_parse = true;
                std::cout << "xrune [options]" << std::endl;
                std::cout << "Options:" << std::endl;
                std::cout << "  -h, --help          Show this help message" << std::endl;
                std::cout << "  -v, --version       Show version information" << std::endl;
                std::cout << "  -rt, --runtime      Audio Runtime : RtAudio (default), Miniaudio, or Csound" << std::endl;
                std::cout << "  -d, --driver        Audio Driver (Alsa, Jack, CoreAudio etc...)" << std::endl;
                std::cout << "  -p, --play          Play soundfile" << std::endl;
                std::cout << "  -l, --loop          Loop playback (--play)" << std::endl;
                std::cout << "  -log, --log-scale   Log scale for oscilloscope (--play)" << std::endl;
                return 0;
            } else if (param == "-v" || param == "--version")
            {
                options.exit_after_parse = true;
                std::cout << "xrune version" << version << std::endl;
                return 0;
            } else if (param == "-p" || param == "--play") 
            {
                std::cout << "Audio playback mode" << std::endl;
                // First make sure an audio file is provided 
                if( (i + 1) >= argc ) 
                {
                    std::cerr << "Error: No audio file provided for playback. Missing argument" << std::endl;
                    options.exit_after_parse = true;
                    return -1;
                }

                std::cout << "Starting  audio playback... " << std::endl;

                options.play = true;
                options.audio_file = std::string( argv[i + 1] ) ; 
            } else if(param == "-l" || param == "--loop")
            {
                options.loop = true;
            } else if(param == "-log" || param == "--log-scale")
            {
                options.log_scale = true;
            } else if(param == "-ksmps" || param == "--ksmps")
            {
                options.ksmps = std::stoul( std::string( argv[i + 1] ) );
            } else if(param == "-sr" || param == "--samplerate")
            {
                options.samplerate = std::stoul( std::string( argv[i + 1] ) );
            } else if(param == "-rt" || param == "--runtime") 
            {
                if( (i + 1) >= argc ) 
                {
                    std::cerr << "Error: No runtime provided" << std::endl;
                    options.exit_after_parse = true;
                    return -1;
                }
                std::string rtarg = argv[i+1];
                options.runtime = xrune::parse_runtime( rtarg );
            } else if(param == "-d" || param == "--driver")
            {
                if( (i + 1) >= argc ) 
                {
                    std::cerr << "Error: No driver provided" << std::endl;
                    options.exit_after_parse = true;
                    return -1;
                }
                std::string darg = argv[i+1];
                options.driver = xrune::parse_driver( darg );
            }


        }
    }
}

int main(int argc, char* argv[])
{
    cout << "ᚷrune audio engine" << endl;

    xrune::options options; 
    int exit_code = parse_command_line(argc, argv, options);

    if(options.exit_after_parse)
    {
        return exit_code;
    }

    if(options.play) 
    {
        xrune::player p(options);
        //xrune::player p(options.audio_file, options.loop, options.log_scale, options.ksmps, options.samplerate);
        p.run();
    }

    // Default options 
    bool loop = false;
    bool play = false; 

    return exit_code;
}